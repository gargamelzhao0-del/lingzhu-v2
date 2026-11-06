"""Fault-inject production C bodies; no firmware hardware access is performed."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    pos = source.index("{", start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser()
parser.add_argument("--gcc", default="gcc")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
read = lambda p: (root / p).read_text(encoding="utf-8-sig")
power = read("main/board/nuwax_power_ctrl.c")
driver = read("main/board/bq27220_driver.c")
battery = read("main/board/battery_service.c")
spool = read("main/offline_audio_spool.c")
app = read("main/nuwax_ai_partner_main.c")

common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdarg.h>
#include "board/nuwax_power_ctrl.h"
#include "board/board_drivers.h"
#include "board/battery_service.h"
#include "board/battery_policy.h"
#include "nuwax_ota_new/nuwax_ota_power_policy.h"
typedef int *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
static int xSemaphoreTake(int *sem, unsigned timeout) {
    (void)timeout;
    if (!*sem) return 0;
    *sem = 0;
    return 1;
}
static void xSemaphoreGive(int *sem) { assert(!*sem); *sem=1; }
static uint64_t fake_ms=1000;
static int64_t esp_timer_get_time(void) { return fake_ms*1000; }
static uint64_t now_ms(void) { return fake_ms; }
static void test_log(const char *fmt, ...) { (void)fmt; }
static const char *TAG="test";
static const char *esp_err_to_name(int err) { (void)err; return "injected"; }
#define ESP_LOGI(tag,...) ((void)(tag), test_log(__VA_ARGS__))
#define ESP_LOGW(tag,...) ((void)(tag), test_log(__VA_ARGS__))
#define ESP_LOGE(tag,...) ((void)(tag), test_log(__VA_ARGS__))
'''

power_globals = power[power.index("#define NUWAX_SHUTDOWN_GATE_CLAIMED"):
                      power.index("#if CONFIG_NUWAX_POWER_KEY_ENABLE", power.index("#define NUWAX_SHUTDOWN_GATE_CLAIMED"))]
power_bodies = "\n".join(function(power, sig) for sig in [
    "static bool claim_shutdown_gate(", "static void restore_shutdown_gate(",
    "static bool valid_inhibit_owner(",
    "bool nuwax_power_ctrl_acquire_shutdown_inhibit(",
    "bool nuwax_power_ctrl_release_shutdown_inhibit(",
    "uint32_t nuwax_power_ctrl_shutdown_inhibit_owners(",
    "bool nuwax_power_ctrl_shutdown_is_inhibited("])
power_checks = r'''
int main(void) {
    assert(nuwax_power_ctrl_acquire_shutdown_inhibit(NUWAX_POWER_INHIBIT_OTA_BOOT));
    assert(nuwax_power_ctrl_acquire_shutdown_inhibit(NUWAX_POWER_INHIBIT_BATTERY_PROFILE));
    assert(!nuwax_power_ctrl_prepare_shutdown());
    assert(!nuwax_power_ctrl_acquire_shutdown_inhibit(NUWAX_POWER_INHIBIT_OTA_UPDATE));
    assert(nuwax_power_ctrl_release_shutdown_inhibit(NUWAX_POWER_INHIBIT_BATTERY_PROFILE));
    assert(nuwax_power_ctrl_release_shutdown_inhibit(NUWAX_POWER_INHIBIT_OTA_BOOT));
    assert(nuwax_power_ctrl_prepare_shutdown());
    assert(nuwax_power_ctrl_prepare_shutdown());
    assert(nuwax_power_ctrl_shutdown_preparing());
    for (unsigned owner=1; owner<=8; owner<<=1)
        assert(!nuwax_power_ctrl_acquire_shutdown_inhibit(owner));
    uint_fast32_t previous=0;
    assert(claim_shutdown_gate(false, &previous));
    restore_shutdown_gate(previous); /* Hardware commit failed: admission stays shut. */
    assert(nuwax_power_ctrl_shutdown_preparing());
    nuwax_power_ctrl_abort_shutdown();
    assert(!nuwax_power_ctrl_shutdown_preparing());
    assert(nuwax_power_ctrl_acquire_shutdown_inhibit(NUWAX_POWER_INHIBIT_OTA_UPDATE));
    assert(!nuwax_power_ctrl_acquire_shutdown_inhibit(NUWAX_POWER_INHIBIT_BATTERY_PROFILE));
    assert(!nuwax_power_ctrl_prepare_shutdown());
    assert(!claim_shutdown_gate(false, &previous));
    assert(claim_shutdown_gate(true, &previous));
    nuwax_power_ctrl_abort_shutdown(); /* Cannot undo a terminal claim. */
    assert(nuwax_power_ctrl_shutdown_preparing());
    puts("PASS: production power gate admission, maintenance, rollback and emergency paths");
}
'''

registers = "\n".join(re.findall(r"^#define BQ27220_(?:REG|STATUS|OPERATION)_.*$", driver, re.M))
driver_stubs = r'''
static int init_sem=1;
static SemaphoreHandle_t s_init_lock=&init_sem;
static struct { bool initialized, profile_verified; void *dev; uint8_t low_threshold; } s_bq;
static int ensure_init_lock(void) { return ESP_OK; }
static uint16_t words[256];
static int fail_reg=-1;
static esp_err_t read_word(uint8_t reg, uint16_t *out) {
    if (reg==fail_reg) return ESP_ERR_TIMEOUT;
    *out=words[reg]; return ESP_OK;
}
static int16_t temperature_c_from_raw(uint16_t raw) { (void)raw; return 25; }
'''
ctx = re.search(r"typedef struct \{.*?\} battery_service_context_t;", battery, re.S).group()
service_stubs = r'''
static battery_service_context_t s_service;
static int service_sem=1;
static int stop_sem=1, stopped_sem;
static SemaphoreHandle_t s_stop_lock=&stop_sem;
static unsigned notifications, deletions;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)99; }
static void xTaskNotifyGive(TaskHandle_t task) { assert(task==(void *)42); notifications++; }
static void vTaskDeleteWithCaps(TaskHandle_t task) { assert(task==(void *)42); deletions++; }
#define BATTERY_CHARGING_EXIT_SAMPLES 2U
#define BATTERY_DISPLAY_MAX_STEP_PERCENT 2U
#define BATTERY_SNAPSHOT_MAX_PERIODS 3U
'''
battery_bodies = "\n".join(function(battery, sig) for sig in [
    "static nuwax_battery_power_state_t classify_power(",
    "static nuwax_battery_service_snapshot_t commit_success(",
    "static nuwax_battery_service_snapshot_t commit_error(",
    "esp_err_t nuwax_battery_service_get_snapshot(",
    "esp_err_t nuwax_battery_service_stop("])
battery_bodies += "\n#define CONFIG_NUWAX_OTA_MIN_BATTERY_PERCENT 5\n" + function(
    read("main/nuwax_ota_new/nuwax_ota_device.c"),
    "static const char *battery_preflight_code(")
battery_checks = r'''
int main(void) {
    s_bq.initialized=true; s_bq.dev=&words; s_bq.low_threshold=10;
    s_service.lock=&service_sem;
    s_service.config=NUWAX_BATTERY_SERVICE_DEFAULT_CONFIG();
    words[BQ27220_REG_VOLTAGE]=3650;
    words[BQ27220_REG_BATTERY_STATUS]=BQ27220_STATUS_BATTPRES;
    words[BQ27220_REG_OPERATION_STATUS]=BQ27220_OPERATION_INITCOMP;
    words[BQ27220_REG_STATE_OF_CHARGE]=60;
    words[BQ27220_REG_DESIGN_CAPACITY]=1200;
    words[BQ27220_REG_FULL_CHARGE_CAPACITY]=1100;
    words[BQ27220_REG_REMAINING_CAPACITY]=600;
    bq27220_battery_status_t raw;
    /* Real-board regression: CHGINH/OTC do not stop the independent charger.
     * Preserve every flag while deriving display state from measured current. */
    for (unsigned flags=0; flags<16; ++flags) {
        unsigned advisory=((flags & 1)?BQ27220_STATUS_CHGINH:0) |
            ((flags & 2)?BQ27220_STATUS_DSG:0) |
            ((flags & 4)?(1U<<11):0) | ((flags & 8)?(1U<<10):0);
        words[BQ27220_REG_BATTERY_STATUS]=BQ27220_STATUS_BATTPRES | advisory;
        words[BQ27220_REG_CURRENT]=390;
        words[BQ27220_REG_STATE_OF_CHARGE]=100;
        assert(bq27220_get_status_partial(&raw)==ESP_OK);
        assert(raw.is_charging && !raw.is_full && raw.soc==100);
        assert(raw.battery_status_flags==words[BQ27220_REG_BATTERY_STATUS]);
        nuwax_battery_service_snapshot_t charged=commit_success(&raw);
        assert(charged.charging && !charged.full);
        assert(charged.power_state==NUWAX_BATTERY_POWER_CHARGING);
        words[BQ27220_REG_CURRENT]=0;
        assert(bq27220_get_status_partial(&raw)==ESP_OK && !raw.is_charging);
        assert(commit_success(&raw).charging); /* One transient pause. */
        assert(!commit_success(&raw).charging); /* Two samples release. */
        words[BQ27220_REG_CURRENT]=(uint16_t)-230;
        assert(bq27220_get_status_partial(&raw)==ESP_OK && !raw.is_charging);
        words[BQ27220_REG_CURRENT]=20;
        assert(bq27220_get_status_partial(&raw)==ESP_OK && !raw.is_charging);
        words[BQ27220_REG_CURRENT]=21;
        assert(bq27220_get_status_partial(&raw)==ESP_OK && raw.is_charging);
        assert(commit_success(&raw).charging);
        words[BQ27220_REG_BATTERY_STATUS] |= BQ27220_STATUS_FC;
        assert(bq27220_get_status_partial(&raw)==ESP_OK && !raw.is_charging && raw.is_full);
        assert(!commit_success(&raw).charging); /* FC exits immediately. */
    }
    words[BQ27220_REG_BATTERY_STATUS]=BQ27220_STATUS_BATTPRES | BQ27220_STATUS_CHGINH;
    words[BQ27220_REG_CURRENT]=390;
    words[BQ27220_REG_VOLTAGE]=3300;
    assert(bq27220_get_status_partial(&raw)==ESP_OK);
    assert(commit_success(&raw).charging);
    assert(strcmp(battery_preflight_code(),"low_power")==0); /* Never bypass voltage. */
    fail_reg=BQ27220_REG_CURRENT;
    assert(bq27220_get_status_partial(&raw)==ESP_ERR_TIMEOUT && !raw.is_charging);
    assert(!commit_success(&raw).charging);
    fail_reg=-1;
    words[BQ27220_REG_VOLTAGE]=3650;
    words[BQ27220_REG_CURRENT]=0;
    words[BQ27220_REG_STATE_OF_CHARGE]=60;
    words[BQ27220_REG_BATTERY_STATUS]=BQ27220_STATUS_BATTPRES;
    assert(bq27220_get_status_partial(&raw)==ESP_OK);
    nuwax_battery_service_snapshot_t snap=commit_success(&raw);
    assert(snap.valid && snap.power_valid && !snap.soc_trusted);
    assert(battery_preflight_code()==NULL); /* Unverified profile is allowed. */
    assert(nuwax_ota_boot_power_evaluate(snap.power_valid,false,false)==NUWAX_OTA_POWER_ALLOWED);
    s_bq.profile_verified=true;
    assert(bq27220_get_status_partial(&raw)==ESP_OK);
    snap=commit_success(&raw);
    assert(snap.soc_trusted);
    for (unsigned soc=0; soc<=100; ++soc) {
        words[BQ27220_REG_STATE_OF_CHARGE]=soc;
        assert(bq27220_get_status_partial(&raw)==ESP_OK);
        snap=commit_success(&raw);
        const char *code=battery_preflight_code();
        if (soc<5) assert(code && strcmp(code,"low_power")==0);
        else assert(code==NULL); /* Includes the UI's 5..10% low warning. */
        if (soc==5) assert(snap.low && !snap.hard_low_voltage);
    }
    words[BQ27220_REG_VOLTAGE]=3300;
    assert(bq27220_get_status_partial(&raw)==ESP_OK);
    snap=commit_success(&raw);
    assert(strcmp(battery_preflight_code(),"low_power")==0);
    words[BQ27220_REG_FULL_CHARGE_CAPACITY]=1;
    words[BQ27220_REG_VOLTAGE]=3300;
    assert(bq27220_get_status_partial(&raw)==ESP_OK);
    snap=commit_success(&raw);
    assert(!snap.valid && snap.power_valid && snap.hard_low_voltage && snap.low);
    fail_reg=BQ27220_REG_CURRENT; /* Later read fails; power group survives. */
    fake_ms+=8000;
    assert(bq27220_get_status_partial(&raw)==ESP_ERR_TIMEOUT);
    assert(raw.power_sample_valid && !raw.telemetry_valid);
    snap=commit_success(&raw); snap=commit_error(ESP_ERR_TIMEOUT);
    assert(snap.power_valid && snap.hard_low_voltage && !snap.power_stale);
    assert(nuwax_ota_boot_power_evaluate(snap.power_valid,false,snap.hard_low_voltage)==NUWAX_OTA_POWER_LOW);
    words[BQ27220_REG_VOLTAGE]=0; fail_reg=-1;
    assert(bq27220_get_status_partial(&raw)==ESP_OK);
    snap=commit_success(&raw);
    assert(!snap.power_valid); /* Never promote a zero read to physical proof. */
    words[BQ27220_REG_VOLTAGE]=3700;
    assert(bq27220_get_status_partial(&raw)==ESP_OK);
    snap=commit_success(&raw);
    assert(snap.power_valid && !snap.hard_low_voltage && !snap.valid);
    fake_ms+=24001;
    assert(nuwax_battery_service_get_snapshot(&snap)==ESP_OK && snap.power_stale);
    assert(strcmp(battery_preflight_code(),"power_unknown")==0);
    fail_reg=BQ27220_REG_BATTERY_STATUS;
    assert(bq27220_get_status_partial(&raw)==ESP_ERR_TIMEOUT && !raw.power_sample_valid);
    s_service.stopped=&stopped_sem; s_service.task=(void *)42; s_service.running=true;
    assert(nuwax_battery_service_stop(0)==ESP_ERR_TIMEOUT);
    assert(s_service.stop_requested && s_service.running && deletions==0);
    stop_sem=0; /* Another caller already owns this stop transaction. */
    assert(nuwax_battery_service_stop(0)==ESP_ERR_TIMEOUT && notifications==1);
    stop_sem=1; service_sem=0;
    assert(nuwax_battery_service_stop(0)==ESP_ERR_TIMEOUT && stop_sem==1);
    service_sem=1; stopped_sem=1;
    assert(nuwax_battery_service_stop(0)==ESP_OK && deletions==1 && !s_service.running);
    assert(nuwax_battery_service_stop(0)==ESP_OK && deletions==1);
    puts("PASS: production driver/service partial I2C faults, invalid FCC, zero voltage, staleness and OTA separation");
}
'''

spool_stubs = r'''
static int state_sem=1, admission_sem=1, writer_sem=1;
static struct { int *lock,*admission_lock,*writer_gate; void *file,*events; } s =
    {&state_sem,&admission_sem,&writer_sem,0,0};
static atomic_bool s_shutdown_requested, s_formal_replay_cancel_requested;
static bool storage_busy, replay_running, session_active, capture_running;
#define REPLAY_ACK_BIT 1
#define REPLAY_CHUNK_ACK_BIT 2
static bool enabled(void) { return true; }
static bool runtime_initialized(void) { return true; }
static bool nuwax_offline_audio_storage_busy(void) { return storage_busy; }
static bool nuwax_offline_audio_replay_running(void) { return replay_running; }
static bool nuwax_sd_session_active(void) { return session_active; }
static bool nuwax_offline_audio_capture_running(void) { return capture_running; }
static int xEventGroupSetBits(void *e,int bits) { (void)e; return bits; }
'''
spool_checks = r'''
int main(void) {
    replay_running=true;
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_ERR_NOT_FINISHED);
    assert(atomic_load(&s_shutdown_requested) && atomic_load(&s_formal_replay_cancel_requested));
    replay_running=false;
    atomic_store(&s_formal_replay_cancel_requested,false); /* Worker clears its transient latch. */
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_OK);
    assert(atomic_load(&s_shutdown_requested)); /* Persistent gate cannot reopen. */
    storage_busy=true;
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_ERR_NOT_FINISHED);
    storage_busy=false; writer_sem=0; /* Between dequeue and busy publication. */
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_ERR_NOT_FINISHED);
    assert(admission_sem==1 && state_sem==1);
    writer_sem=1; s.file=&writer_sem;
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_ERR_NOT_FINISHED);
    s.file=0;
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_OK);
    session_active=true;
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_ERR_NOT_FINISHED);
    session_active=false; capture_running=true;
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_ERR_NOT_FINISHED);
    capture_running=false;
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_OK);
    assert(admission_sem==1 && writer_sem==1 && state_sem==1);
    state_sem=0;
    assert(nuwax_offline_audio_cancel_shutdown()==ESP_ERR_NOT_FINISHED);
    assert(atomic_load(&s_shutdown_requested));
    state_sem=1;
    assert(nuwax_offline_audio_cancel_shutdown()==ESP_OK);
    assert(!atomic_load(&s_shutdown_requested));
    assert(atomic_load(&s_formal_replay_cancel_requested));
    puts("PASS: production storage shutdown barrier, worker cancellation, open files and dequeue handoff");
}
'''

# Wiring assertions supplement executable bodies; they do not replace them.
start = function(spool, "static esp_err_t replay_formal_start_locked(")
launches = list(re.finditer(r'\bxTaskCreate(?:PinnedToCore)?WithCaps\(', start))
assert launches, "Formal replay must launch a task through the checked owner"
assert all(start.index("s_shutdown_requested") < launch.start() for launch in launches)
wrapper = function(spool, "esp_err_t nuwax_offline_audio_replay_formal_start(")
assert wrapper.index("xSemaphoreTake(s.lock") < wrapper.index("replay_formal_start_locked")
owner = function(app, "static void service_power_key_events(nuwax_ws_client_t *ws) {")
assert owner.index("nuwax_power_ctrl_prepare_shutdown") < owner.index("s_power_shutdown_requested = true")
assert owner.index("nuwax_offline_audio_prepare_shutdown() != ESP_OK") < owner.index("nuwax_power_ctrl_commit_shutdown")
init = function(driver, "esp_err_t bq27220_init(")
assert init.index("design_capacity != 1200U") < init.index("nuwax_i2c_bus_add_device")

programs = {
    "gate": common + power_globals + power_bodies + power_checks,
    "battery": common + registers + driver_stubs + function(driver, "esp_err_t bq27220_get_status_partial(") + ctx + service_stubs + battery_bodies + battery_checks,
    "storage": common + spool_stubs + function(spool, "esp_err_t nuwax_offline_audio_prepare_shutdown(") + function(spool, "esp_err_t nuwax_offline_audio_cancel_shutdown(") + spool_checks,
}
with tempfile.TemporaryDirectory(prefix="nuwax-board-power-") as tmp:
    path = Path(tmp)
    (path / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n" + "\n".join(
        f"#define {name} {value}" for value,name in enumerate([
            "ESP_OK","ESP_FAIL","ESP_ERR_NO_MEM","ESP_ERR_INVALID_ARG","ESP_ERR_INVALID_STATE",
            "ESP_ERR_NOT_SUPPORTED","ESP_ERR_NOT_FINISHED","ESP_ERR_TIMEOUT","ESP_ERR_INVALID_RESPONSE"])), encoding="utf-8")
    for name, code in programs.items():
        src=path/f"{name}.c"
        exe=path/f"{name}.exe"
        src.write_text(code, encoding="utf-8")
        subprocess.run([args.gcc,"-std=c11","-Wall","-Wextra","-Werror",
                        "-Wno-unused-function","-Wno-unused-variable",f"-I{path}",f"-I{root/'main'}",
                        str(src),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
print("Board power production-boundary checks: PASS")
