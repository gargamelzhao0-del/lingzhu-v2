#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_log.h"
#include "offline_capture_health.h"
typedef enum { NUWAX_MEETING_STATE_DISABLED, NUWAX_MEETING_STATE_STARTING,
    NUWAX_MEETING_STATE_ACTIVE, NUWAX_MEETING_STATE_STOPPING, NUWAX_MEETING_STATE_ERROR } nuwax_meeting_state_t;
typedef enum { NUWAX_MONITORING_STATE_DISABLED, NUWAX_MONITORING_STATE_STARTING,
    NUWAX_MONITORING_STATE_ACTIVE, NUWAX_MONITORING_STATE_STOPPING, NUWAX_MONITORING_STATE_ERROR } nuwax_monitoring_state_t;
typedef nuwax_meeting_state_t nuwax_meeting_ui_state_t;
typedef nuwax_monitoring_state_t nuwax_monitoring_ui_state_t;
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 1
#define NUWAX_OFFLINE_AUDIO_MODE_MONITORING 1
#define TAG "ui_host"
static atomic_uint s_ui_lifecycle_generation;
static int s_meeting_mode, s_monitoring_mode, last_meeting, last_monitoring, old_kind;
static bool lease, capture, meeting_resume, monitoring_resume;
static nuwax_offline_capture_health_t health;
static nuwax_offline_capture_health_t nuwax_offline_audio_capture_health(void) { return health; }
static bool ui_ready_snapshot(void) { return true; }
static nuwax_meeting_state_t nuwax_meeting_mode_state(const int *m) { return *m; }
static nuwax_monitoring_state_t nuwax_monitoring_mode_state(const int *m) { return *m; }
static bool offline_mode_lease_pending(void) { return lease; }
static bool nuwax_offline_audio_capture_running(void) { return capture; }
static int offline_mode_kind(void) { return old_kind; }
static bool nuwax_meeting_mode_offline_resume_pending(const int *m) { (void)m; return meeting_resume; }
static bool nuwax_monitoring_mode_offline_resume_pending(const int *m) { (void)m; return monitoring_resume; }
static void nuwax_app_shell_set_meeting_state(nuwax_meeting_ui_state_t s) { last_meeting=s; }
static void nuwax_app_shell_set_monitoring_state(nuwax_monitoring_ui_state_t s) { last_monitoring=s; }
#include "audio_mode_ui_host.inc"
int main(void) {
    /* Current controllers must win over every stale/offline snapshot. */
    for (int kind=0;kind<2;kind++) for (int bits=0;bits<16;bits++) {
        old_kind=kind; lease=bits&1; capture=bits&2;
        meeting_resume=bits&4; monitoring_resume=bits&8;
        for(int m=0;m<5;m++) for(int n=0;n<5;n++) {
            if(!m&&!n) continue;
            s_meeting_mode=m; s_monitoring_mode=n;
            atomic_fetch_add(&s_ui_lifecycle_generation,1);
            sync_exclusive_audio_mode_ui_state();
            assert(last_meeting==m && last_monitoring==n);
        }
    }
    s_meeting_mode=s_monitoring_mode=0;
    lease=capture=meeting_resume=monitoring_resume=false;
    atomic_fetch_add(&s_ui_lifecycle_generation,1);
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==0 && last_monitoring==0); /* FILE close alone never turns a mode on. */
    old_kind=0; monitoring_resume=true;
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==0 && last_monitoring==NUWAX_MONITORING_STATE_ERROR);
    monitoring_resume=false; meeting_resume=true; old_kind=1;
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==NUWAX_MEETING_STATE_ERROR && last_monitoring==0);
    capture=true; health=NUWAX_OFFLINE_CAPTURE_STARTING;
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==NUWAX_MEETING_STATE_STARTING);
    health=NUWAX_OFFLINE_CAPTURE_RECORDING;
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==NUWAX_MEETING_STATE_ACTIVE);
    health=NUWAX_OFFLINE_CAPTURE_BUFFERING;
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==NUWAX_MEETING_STATE_ACTIVE);
    health=NUWAX_OFFLINE_CAPTURE_UNAVAILABLE;
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==NUWAX_MEETING_STATE_ERROR);
    meeting_resume=false; old_kind=0;
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==NUWAX_MEETING_STATE_STOPPING);
    capture=false;
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==NUWAX_MEETING_STATE_DISABLED);
    puts("production UI reconciliation: online precedence, no-card error, MIC startup, retained buffer, failure, stop and rebuild PASS");
}
