/* Only hardware/media outcomes are injected. Retry, duration, cancellation and UI code are extracted. */

#include <assert.h>
#include <stdatomic.h>
#include "offline_capture_health.h"
#define CONFIG_NUWAX_MEETING_OFFLINE_MAX_HOURS 8U
#define CONFIG_NUWAX_MONITORING_OFFLINE_MAX_HOURS 8U
static atomic_uint s_offline_wait_started_ms;
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 1
#define CONFIG_NUWAX_OTA_ENABLE 0
#define CONFIG_NUWAX_VOICE_SAMPLE_RATE 16000U
#define CONFIG_NUWAX_VOICE_CHANNELS 1U
#define CONFIG_NUWAX_VOICE_FRAME_DURATION_MS 20U
#define NUWAX_DEVICE_ERROR_RESULT_BYTES 160U
#define NUWAX_OFFLINE_AUDIO_MODE_MEETING 0
#define NUWAX_OFFLINE_AUDIO_MODE_MONITORING 1
#define TAG "audit"
static void ignored_log(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
#define ESP_LOGI ignored_log
#define ESP_LOGW ignored_log
typedef enum { NUWAX_MEETING_STATE_DISABLED, NUWAX_MEETING_STATE_STARTING,
    NUWAX_MEETING_STATE_ACTIVE, NUWAX_MEETING_STATE_STOPPING, NUWAX_MEETING_STATE_ERROR } nuwax_meeting_state_t;
typedef enum { NUWAX_MONITORING_STATE_DISABLED, NUWAX_MONITORING_STATE_STARTING,
    NUWAX_MONITORING_STATE_ACTIVE, NUWAX_MONITORING_STATE_STOPPING, NUWAX_MONITORING_STATE_ERROR } nuwax_monitoring_state_t;
typedef nuwax_meeting_state_t nuwax_meeting_ui_state_t;
typedef nuwax_monitoring_state_t nuwax_monitoring_ui_state_t;
typedef struct {uint32_t next_capture_sequence; bool resuming; char session_id[48];} nuwax_audio_mode_identity_t;
typedef struct {int state; nuwax_audio_mode_identity_t identity;} mode_t;
static mode_t s_meeting_mode, s_monitoring_mode;
static atomic_uint s_ui_lifecycle_generation;
static atomic_bool s_offline_mode_capture_failed,s_offline_mode_cancel_to_sleep,s_reprovision_quiesce_requested;
static char s_offline_mode_resume_session[48]="audit_original_session";
static bool s_offline_mode_capture_started;
static uint32_t s_offline_capture_retry_next_ms,s_offline_capture_retry_backoff_ms,s_offline_capture_retry_attempts;
static bool lease, capture, expired, meeting_resume,monitoring_resume;
static int mode_kind,init_error,last_meeting,last_monitoring;
static uint32_t clock_ms,init_calls,start_calls,warnings;
static nuwax_offline_capture_health_t health;
static bool cancel_during_start;
static nuwax_offline_capture_health_t nuwax_offline_audio_capture_health(void){return health;}
static void nuwax_offline_audio_supervisor_wake(void){}
static esp_err_t nuwax_offline_audio_request_stop(bool complete){(void)complete;capture=false;return ESP_OK;}
static bool offline_mode_lease_pending(void){return lease;}
static void offline_mode_lease_store(bool value){lease=value;}
static int offline_mode_kind(void){return mode_kind;}
static bool nuwax_offline_audio_capture_running(void){return capture;}
static bool nuwax_offline_audio_limit_reached(void){return expired;}
static uint32_t nuwax_offline_audio_next_capture_sequence(void){return 42;}
static void nuwax_audio_mode_identity_clear(nuwax_audio_mode_identity_t *identity){memset(identity,0,sizeof(*identity));}
static uint32_t monotonic_ms(void){return clock_ms;}
static bool elapsed_ms(uint32_t now,uint32_t deadline){return (int32_t)(now-deadline)>=0;}

static esp_err_t nuwax_offline_audio_start_capture_continuation(int mode,const char *session,uint32_t rate,uint8_t ch,uint16_t frame,uint32_t seq){
    assert(mode==mode_kind && session[0] && rate==16000 && ch==1 && frame==20);
    assert(seq==0 || seq==42);init_calls++;if(init_error)return init_error;start_calls++;capture=true;health=NUWAX_OFFLINE_CAPTURE_RECORDING;if(cancel_during_start)lease=false;return ESP_OK;
}
typedef struct {unsigned internal_free,internal_largest,dma_largest;} nuwax_audio_runtime_memory_t;
static nuwax_audio_runtime_memory_t exclusive_audio_memory_snapshot(void){return (nuwax_audio_runtime_memory_t){100000,50000,30000};}
static const char *esp_err_to_name(esp_err_t err){return err?"INJECTED_MISSING_SD":"ESP_OK";}
static bool nuwax_device_warning_report_enqueue_detail(const char *component,const char *code,esp_err_t err,const char *detail,const char *state,uint32_t generation,const char *turn){
    assert(!strcmp(component,"audio") && !strcmp(code,"offline_capture_retry") && err && detail && state && !generation && turn);warnings++;return true;
}
static bool ui_ready_snapshot(void){return true;}
static nuwax_meeting_state_t nuwax_meeting_mode_state(const mode_t *m){return m->state;}
static nuwax_monitoring_state_t nuwax_monitoring_mode_state(const mode_t *m){return m->state;}
static bool nuwax_meeting_mode_offline_resume_pending(const mode_t *m){(void)m;return meeting_resume;}
static bool nuwax_monitoring_mode_offline_resume_pending(const mode_t *m){(void)m;return monitoring_resume;}
static void nuwax_app_shell_set_meeting_state(nuwax_meeting_ui_state_t state){last_meeting=state;}
static void nuwax_app_shell_set_monitoring_state(nuwax_monitoring_ui_state_t state){last_monitoring=state;}
