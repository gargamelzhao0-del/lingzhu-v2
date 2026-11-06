"""Execute the production policy and owner functions with injected time/I2C/SD faults."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--gcc', default='gcc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
app = (root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')


def function(signature):
    start = app.index(signature)
    pos = app.index('{', start)
    depth, end = 1, pos+1
    while depth:
        depth += (app[end] == '{') - (app[end] == '}')
        end += 1
    return app[start:end]


stubs = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "board/low_battery_shutdown_policy.h"
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 1
#define NUWAX_POWER_INHIBIT_BATTERY_PROFILE 8U
#define NUWAX_APP_STATE_SLEEPING 0
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static nuwax_low_battery_shutdown_policy_t s_low_battery_policy;
static bool s_low_battery_warning_visible, s_low_battery_shutdown_owned;
static bool s_power_shutdown_requested, s_power_shutdown_mode_stop_requested;
static bool s_sleep_request_local_power;
static struct { bool active, local_request; } s_sleep_transaction;
static bool s_low_battery_sampler_stopping, s_low_battery_sampler_restart_pending;
static nuwax_battery_service_snapshot_t measurement;
static uint64_t now, charger_at, drain_at, stop_at, show_at, warning_at;
static unsigned inhibitors, starts, stops, wake_requests, commits, start_failures;
static bool prepared, drawer_visible, sampler_running, sampler_stopping;
static bool cancel_busy, fail_commit_once, missing_gauge;
static jmp_buf powered_off;

static nuwax_battery_service_snapshot_t critical(uint64_t time) {
    return (nuwax_battery_service_snapshot_t){
        .valid=true,.sample_available=true,.power_valid=true,.battery_present=true,
        .gauge_ready=true,.percent=1,.display_percent=1,.voltage_mv=3300,
        .current_ma=-150,.last_error=ESP_OK,
        .sample_timestamp_ms=time,.power_timestamp_ms=time};
}
static void publish(void) {
    if (!sampler_running || sampler_stopping || missing_gauge) return;
    measurement.sample_timestamp_ms=measurement.power_timestamp_ms=now;
    if (charger_at && now>=charger_at) { measurement.current_ma=150; measurement.charging=true; }
}
static int64_t esp_timer_get_time(void) { return now*1000; }
static void vTaskDelay(unsigned delay) {
    now+=delay;
    assert(now<20000); /* Owner deadlock, not an allowed test timeout. */
    if (now%500U==0U) publish();
}
int nuwax_battery_service_get_snapshot(nuwax_battery_service_snapshot_t *out) {
    *out=measurement; return ESP_OK;
}
static bool ui_ready_snapshot(void) { return true; }
static bool nuwax_lvgl_port_lock(unsigned timeout) { (void)timeout; return now>=show_at; }
static void nuwax_lvgl_port_unlock(void) {}
static int nuwax_low_battery_warning_show(bool visible) {
    if (visible && !drawer_visible) warning_at=now;
    drawer_visible=visible; return ESP_OK;
}
static uint32_t nuwax_power_ctrl_shutdown_inhibit_owners(void) { return inhibitors; }
static bool nuwax_power_ctrl_shutdown_is_inhibited(void) { return inhibitors!=0; }
static bool nuwax_power_ctrl_prepare_shutdown(void) {
    if (inhibitors) return false;
    prepared=true; return true;
}
static void nuwax_power_ctrl_abort_shutdown(void) { prepared=false; }
static int nuwax_offline_audio_prepare_shutdown(void) {
    return now>=drain_at ? ESP_OK : ESP_ERR_NOT_FINISHED;
}
static int nuwax_offline_audio_cancel_shutdown(void) {
    assert(now>=drain_at);
    return cancel_busy ? ESP_ERR_NOT_FINISHED : ESP_OK;
}
int nuwax_battery_service_stop(unsigned timeout) {
    (void)timeout;
    stops++;
    sampler_stopping=true;
    if (now<stop_at) return ESP_ERR_TIMEOUT;
    sampler_running=false;
    return ESP_OK;
}
static int start_battery_service_for_ui(void) {
    assert(!sampler_running);
    starts++;
    if (start_failures) { start_failures--; return ESP_FAIL; }
    sampler_running=true; sampler_stopping=false; publish(); return ESP_OK;
}
static bool nuwax_power_ctrl_commit_shutdown(bool force) {
    assert(!force && prepared && !sampler_running && !inhibitors);
    assert(now>=drain_at && now>=warning_at+5000);
    commits++;
    if (fail_commit_once) { fail_commit_once=false; return false; }
    longjmp(powered_off,1);
    return true;
}
static int conversation_state_current(void) { return NUWAX_APP_STATE_SLEEPING; }
static void request_sleep_wake_word_detector(const char *reason) { (void)reason; wake_requests++; }
'''

bodies = '\n'.join(function(sig) for sig in [
    'static bool local_power_transition_active(void) {',
    'static bool set_low_battery_warning(bool visible)\n{',
    'static bool service_low_battery_sampler_restart(void)\n{',
    'static void service_low_battery_shutdown(void)\n{',
    'static bool low_battery_confirm_before_power_off(bool rearm_wake)\n{',
    'static void service_boot_low_battery_gate(void)\n{',
])

checks = r'''
static void reset(void) {
    now=1000; measurement=critical(now);
    s_low_battery_policy=(nuwax_low_battery_shutdown_policy_t){0};
    s_low_battery_warning_visible=s_low_battery_shutdown_owned=false;
    s_power_shutdown_requested=s_power_shutdown_mode_stop_requested=false;
    s_low_battery_sampler_stopping=s_low_battery_sampler_restart_pending=false;
    prepared=drawer_visible=sampler_stopping=cancel_busy=fail_commit_once=missing_gauge=false;
    sampler_running=true;
    charger_at=drain_at=stop_at=show_at=warning_at=0;
    inhibitors=starts=stops=wake_requests=commits=start_failures=0;
}
static void policy_checks(void) {
    reset();
    assert(NUWAX_LOW_BATTERY_WARNING_MS==5000U);
    assert(nuwax_low_battery_shutdown_required(&measurement,now));
    assert(!nuwax_low_battery_shutdown_required(NULL,now));
    /* All percentages, including the 0% boundary and unverified cell profile. */
    for (unsigned i=0;i<=100;i++) {
        measurement.percent=measurement.display_percent=i;
        assert(nuwax_low_battery_shutdown_required(&measurement,now)==(i<=1));
    }
    measurement=critical(now);
#define REJECT(field,value) do { nuwax_battery_service_snapshot_t bad=measurement; \
    bad.field=value; assert(!nuwax_low_battery_shutdown_required(&bad,now)); } while(0)
    REJECT(valid,false); REJECT(sample_available,false); REJECT(stale,true);
    REJECT(power_valid,false); REJECT(power_stale,true); REJECT(battery_present,false);
    REJECT(gauge_ready,false); REJECT(last_error,ESP_FAIL);
    REJECT(sample_timestamp_ms,0); REJECT(power_timestamp_ms,0);
    REJECT(sample_timestamp_ms,now+1); REJECT(power_timestamp_ms,now+1);
    REJECT(display_percent,2); REJECT(current_ma,21);
#undef REJECT
    assert(!measurement.soc_trusted);
    assert(nuwax_low_battery_shutdown_required(&measurement,now+1500));
    assert(!nuwax_low_battery_shutdown_required(&measurement,now+1501));
    measurement.charging=true;
    assert(nuwax_low_battery_shutdown_required(&measurement,now)); /* unplug: old UI latch */
    measurement.current_ma=0;
    assert(!nuwax_low_battery_shutdown_required(&measurement,now));
    reset();
    assert(nuwax_low_battery_shutdown_update(&s_low_battery_policy,&measurement,now,false)==NUWAX_LOW_BATTERY_WARNING);
    now=2499;
    assert(nuwax_low_battery_shutdown_update(&s_low_battery_policy,&measurement,now,false)==NUWAX_LOW_BATTERY_WARNING);
    measurement=critical(5500); now=6000;
    assert(nuwax_low_battery_shutdown_update(&s_low_battery_policy,&measurement,now,false)==NUWAX_LOW_BATTERY_WARNING);
    measurement=critical(now);
    assert(nuwax_low_battery_shutdown_update(&s_low_battery_policy,&measurement,now,false)==NUWAX_LOW_BATTERY_COMMIT);
    assert(nuwax_low_battery_shutdown_update(&s_low_battery_policy,&measurement,now,true)==NUWAX_LOW_BATTERY_IDLE);
    assert(!s_low_battery_policy.warning);
    puts("PASS: policy SOC 0..100, charging/unplug, invalid/stale I2C, deadline and fresh confirmation");
}
static void owner_checks(void) {
    reset();
    service_low_battery_shutdown();
    assert(prepared && drawer_visible && !s_power_shutdown_requested);
    assert(!local_power_transition_active()); /* Cancellable reminder cannot consume playback resume. */
    now=5999; measurement=critical(now); service_low_battery_shutdown();
    assert(!s_power_shutdown_requested);
    now=6000; measurement=critical(now); service_low_battery_shutdown();
    assert(s_power_shutdown_requested && s_low_battery_shutdown_owned);
    assert(local_power_transition_active());
    /* Storage lock contention must not reopen normal admission prematurely. */
    measurement.current_ma=100; cancel_busy=true;
    assert(!low_battery_confirm_before_power_off(true));
    assert(prepared && s_power_shutdown_requested && drawer_visible && wake_requests==0);
    cancel_busy=false;
    assert(!low_battery_confirm_before_power_off(true));
    assert(!prepared && !s_power_shutdown_requested && !drawer_visible && wake_requests==1);
    assert(!local_power_transition_active());
    /* Unplug gets a new complete reminder. */
    now=6100; measurement=critical(now); service_low_battery_shutdown();
    assert(prepared && !s_power_shutdown_requested);
    now=11099; measurement=critical(now); service_low_battery_shutdown();
    assert(!s_power_shutdown_requested);
    now=11100; measurement=critical(now); service_low_battery_shutdown();
    assert(s_power_shutdown_requested);
    s_low_battery_shutdown_owned=false; measurement.current_ma=100;
    assert(low_battery_confirm_before_power_off(true)); /* explicit power key remains authoritative */
    reset(); show_at=2000;
    for (;now<7000;now+=20) { publish(); service_low_battery_shutdown(); assert(!s_power_shutdown_requested); }
    publish(); service_low_battery_shutdown();
    assert(warning_at==2000 && s_power_shutdown_requested);
    reset(); service_low_battery_shutdown();
    measurement.last_error=ESP_FAIL; service_low_battery_shutdown();
    assert(!prepared && !drawer_visible);
    reset(); inhibitors=1; service_low_battery_shutdown();
    assert(!prepared && !drawer_visible && !s_power_shutdown_requested);
    puts("PASS: runtime cancellation, safe storage retry, manual power key, UI contention and protection");
}
static void boot_checks(void) {
    reset();
    if (!setjmp(powered_off)) { service_boot_low_battery_gate(); assert(!"empty boot escaped"); }
    assert(now==6000 && commits==1 && starts==0);
    reset(); measurement.percent=measurement.display_percent=2;
    service_boot_low_battery_gate(); assert(now==1000 && !prepared);
    reset(); measurement.current_ma=80;
    service_boot_low_battery_gate(); assert(now==1000 && !prepared);
    reset(); charger_at=3000;
    service_boot_low_battery_gate(); assert(now==3000 && !prepared && !drawer_visible && !commits);
    reset(); drain_at=6500;
    if (!setjmp(powered_off)) { service_boot_low_battery_gate(); assert(!"pending SD boot escaped"); }
    assert(now==6500 && commits==1);
    reset(); drain_at=8500; charger_at=6500;
    service_boot_low_battery_gate(); assert(now==8500 && !prepared && !commits);
    reset(); stop_at=9000; charger_at=6500; start_failures=1;
    service_boot_low_battery_gate();
    assert(now==9000 && !prepared && !commits && s_low_battery_sampler_restart_pending);
    service_low_battery_shutdown(); /* Recover even after temporary task-allocation failure. */
    assert(sampler_running && !sampler_stopping && !s_low_battery_sampler_restart_pending && starts==2);
    assert(!s_power_shutdown_requested && !drawer_visible);
    reset(); fail_commit_once=true;
    if (!setjmp(powered_off)) { service_boot_low_battery_gate(); assert(!"failed commit escaped"); }
    assert(commits==2 && starts==1);
    reset(); missing_gauge=true; measurement=(nuwax_battery_service_snapshot_t){0};
    service_boot_low_battery_gate(); assert(now==6000 && !commits && !prepared);
    reset(); inhibitors=1;
    service_boot_low_battery_gate(); assert(now==1000 && !commits);
    puts("PASS: boot 1%, charging bypass/late insertion, SD drain, asynchronous sampler stop/restart, unknown gauge and OTA");
}
int main(void) { policy_checks(); owner_checks(); boot_checks(); return 0; }
'''

# Verify runtime uses the same state machine, then drains all existing owners before final cut.
owner = function('static void service_power_key_events(nuwax_ws_client_t *ws) {')
assert owner.index('service_low_battery_shutdown();') < owner.index('if (!s_power_shutdown_requested)')
assert owner.index('nuwax_offline_audio_prepare_shutdown() != ESP_OK') < owner.index('low_battery_confirm_before_power_off(true)')
assert owner.index('stop_sleep_wake_word_detector') < owner.index('low_battery_confirm_before_power_off(true)')
assert owner.index('low_battery_confirm_before_power_off(true)') < owner.index('nuwax_battery_service_stop(0U)') < owner.index('nuwax_power_ctrl_commit_shutdown(false)')

with tempfile.TemporaryDirectory(prefix='nuwax-low-battery-') as tmp:
    path=Path(tmp)
    (path/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n'+'\n'.join(
        f'#define {name} {i}' for i,name in enumerate([
            'ESP_OK','ESP_FAIL','ESP_ERR_NOT_FINISHED','ESP_ERR_TIMEOUT'])), encoding='utf-8')
    src=path/'test.c'; exe=path/'test.exe'
    src.write_text(stubs+bodies+checks,encoding='utf-8')
    subprocess.run([args.gcc,'-std=c11','-Wall','-Wextra','-Werror',f'-I{path}',f'-I{root/"main"}',
                    str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Low battery production-boundary checks: PASS')
