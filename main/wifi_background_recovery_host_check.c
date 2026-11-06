/* Runs the production mailbox/poll/worker functions with a virtual radio and
 * clock. Long radio waits interleave main polls, just as separate RTOS tasks do. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#define TAG "host"
static void host_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
static atomic_bool s_reprovision_quiesce_requested, s_wifi_connect_in_progress;
static int s_wifi_connect_requests = 1;
static bool connected, provisioning, offline, lock_available = true;
static unsigned explicit_requests, driver_calls, reports, success_reports, checkpoints;
static int64_t now_us, last_checkpoint_us, max_checkpoint_gap_us;
static unsigned radio_ms = 30000, inject;
static esp_err_t radio_result = ESP_ERR_TIMEOUT;
static int64_t esp_timer_get_time(void) { return now_us; }
static bool nuwax_wifi_is_connected(void) { return connected; }
static bool nuwax_wifi_provisioning_active(void) { return provisioning; }
static bool nuwax_wifi_simulated_offline(void) { return offline; }
static unsigned uxQueueMessagesWaiting(int q) { assert(q == 1); return explicit_requests; }
static void app_task_watchdog_checkpoint(void) {
    int64_t gap = now_us - last_checkpoint_us;
    if (gap > max_checkpoint_gap_us) max_checkpoint_gap_us = gap;
    last_checkpoint_us = now_us;
    ++checkpoints;
}
static bool begin_wifi_connect_operation(const char *reason, unsigned wait) {
    assert(reason && wait == 0);
    if (!lock_available || s_reprovision_quiesce_requested) return false;
    assert(!s_wifi_connect_in_progress); s_wifi_connect_in_progress = true; return true;
}
static void end_wifi_connect_operation(void) { assert(s_wifi_connect_in_progress); s_wifi_connect_in_progress = false; }
static void nuwax_app_shell_set_bridge_status(bool value) { assert(!value); }
static void set_wifi_transport_connecting_state(const char *s) { assert(s); }
static void queue_wifi_status_report(const char *phase, const char *ssid, const char *ip, bool ok, const char *detail) {
    assert(phase && ssid && ip && detail); ++reports;
    if (ok) ++success_reports;
}
static void nuwax_wifi_get_status(char *ssid, size_t ns, char *ip, size_t ni) {
    assert(connected); snprintf(ssid, ns, "host-ap"); snprintf(ip, ni, "192.0.2.2");
}
static void nuwax_app_shell_set_wifi_status(const char *s, const char *ip) { assert(s && ip); }
static void set_wifi_result_from_current_link(const char *s, bool ok, const char *detail) { assert(s && detail); (void)ok; }
static esp_err_t nuwax_wifi_recover(void);
#include "wifi_background_recovery_host.inc"
static esp_err_t nuwax_wifi_recover(void) {
    ++driver_calls;
    for (unsigned ms = 0; ms < radio_ms; ms += 50) {
        now_us += 50000;
        assert(!recover_wifi_if_needed()); // Main must return while the worker waits.
        assert(s_wifi_recovery_state == WIFI_RECOVERY_RUNNING);
        if (inject && ms == 500) {
            cancel_background_wifi_recovery();
            if (inject == 1) provisioning = true;
            if (inject == 2) offline = true;
            if (inject <= 2) return ESP_ERR_INVALID_STATE;
        }
    }
    if (radio_result == ESP_OK) connected = true;
    if (inject == 4) {
        assert(s_wifi_recovery_state == WIFI_RECOVERY_RUNNING);
        assert(!recover_wifi_if_needed());
        assert(success_reports == 0); /* GOT_IP is not completion publication. */
    }
    return radio_result;
}
static void reset(void) {
    assert(!s_wifi_connect_in_progress);
    connected = provisioning = offline = false; lock_available = true;
    s_reprovision_quiesce_requested = false;
    s_wifi_recovery_state = WIFI_RECOVERY_IDLE; s_wifi_recovery_epoch = 0;
    s_wifi_recovery_request_epoch = 0; s_wifi_recovery_next_ms = 0;
    s_wifi_recovery_failures = driver_calls = reports = success_reports = checkpoints = 0;
    now_us = last_checkpoint_us = max_checkpoint_gap_us = 0;
    explicit_requests = inject = 0; radio_ms = 30000; radio_result = ESP_ERR_TIMEOUT;
}
int main(void) {
    reset();
    const unsigned delays[] = {3000,6000,12000,24000,30000,30000};
    for (unsigned i = 0; i < sizeof(delays)/sizeof(delays[0]); ++i) {
        assert(!recover_wifi_if_needed());
        assert(s_wifi_recovery_state == WIFI_RECOVERY_PENDING);
        for (unsigned j=0; j<100; ++j) assert(!recover_wifi_if_needed());
        service_background_wifi_recovery();
        assert(driver_calls == i+1 && s_wifi_recovery_state == WIFI_RECOVERY_DONE);
        assert(!recover_wifi_if_needed());
        assert(s_wifi_recovery_next_ms == now_us/1000 + delays[i]);
        for (unsigned ms=0; ms<delays[i]; ms+=50) {
            assert(!recover_wifi_if_needed());
            assert(s_wifi_recovery_state == WIFI_RECOVERY_IDLE);
            now_us += 50000;
        }
    }
    assert(max_checkpoint_gap_us <= 50000 && checkpoints > 3600);
    puts("PASS: absent AP across six 30s attempts, bounded backoff, no duplicate jobs; main watchdog/control polls continue every 50ms");
    reset(); radio_ms=60000; // Primary plus backup must not block main either.
    recover_wifi_if_needed(); service_background_wifi_recovery(); recover_wifi_if_needed();
    assert(max_checkpoint_gap_us<=50000 && driver_calls==1);
    reset(); radio_result=ESP_OK;
    recover_wifi_if_needed(); service_background_wifi_recovery();
    assert(recover_wifi_if_needed() && success_reports==1 && s_wifi_recovery_failures==0);
    for(unsigned j=0;j<100;j++) assert(recover_wifi_if_needed());
    assert(driver_calls==1 && success_reports==1);
    reset(); radio_result=ESP_OK;
    recover_wifi_if_needed(); service_background_wifi_recovery(); connected=false;
    unsigned before_lost=reports; recover_wifi_if_needed();
    assert(reports==before_lost && !success_reports && s_wifi_recovery_next_ms==now_us/1000+500);
    puts("PASS: 60s primary/backup wait remains off main; success consumed exactly once");
    reset(); radio_result=ESP_OK; radio_ms=0; inject=4;
    assert(!recover_wifi_if_needed()); service_background_wifi_recovery();
    assert(s_wifi_recovery_state==WIFI_RECOVERY_DONE && !success_reports);
    assert(recover_wifi_if_needed() && success_reports==1);
    assert(s_wifi_recovery_state==WIFI_RECOVERY_IDLE);
    reset(); assert(!recover_wifi_if_needed()); connected=true;
    assert(recover_wifi_if_needed() && success_reports==1);
    service_background_wifi_recovery(); assert(!driver_calls);
    assert(recover_wifi_if_needed() && success_reports==1);
    puts("PASS: GOT_IP-before-DONE cannot bypass publication; pending job superseded by live link publishes once");
    for(unsigned mode=1; mode<=3; ++mode) {
        reset(); inject=mode; recover_wifi_if_needed(); service_background_wifi_recovery();
        unsigned before=reports; recover_wifi_if_needed();
        assert(reports==before && s_wifi_recovery_failures==0 && !success_reports);
        provisioning=offline=false; now_us+=500000; inject=0;
        assert(!recover_wifi_if_needed() && s_wifi_recovery_state==WIFI_RECOVERY_PENDING);
    }
    reset(); recover_wifi_if_needed(); cancel_background_wifi_recovery();
    service_background_wifi_recovery(); assert(!driver_calls && s_wifi_recovery_state==WIFI_RECOVERY_IDLE);
    explicit_requests=1; recover_wifi_if_needed(); assert(s_wifi_recovery_state==WIFI_RECOVERY_IDLE);
    explicit_requests=0; lock_available=false; recover_wifi_if_needed(); service_background_wifi_recovery();
    recover_wifi_if_needed(); assert(!driver_calls && s_wifi_recovery_next_ms==500);
    puts("PASS: provisioning/offline/stale completion cancellation, queued cancellation, explicit request priority and busy owner retry");
}
