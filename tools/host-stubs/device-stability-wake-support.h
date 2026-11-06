/* Deterministic RTOS, transport and hardware doubles; production functions are extracted by the runner. */

#include <assert.h>
#include "conversation/wake_greeting_recovery_policy.h"
#include "ws_close_policy.h"
static unsigned dynamic_epoch=1;
static bool transport_closed, menu_open, mode_owner, mode_pending, ota_active, notification_busy, power_pending;
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "conversation/awake_session_state.h"
#include "conversation/normal_conversation_policy.h"
#include "conversation/prelisten_state.h"
#include "conversation/listen_resume_state.h"
#include "conversation/conversation_visual_round.h"
#include "conversation/turn_guard.h"
#include "conversation/playback_terminal_guard.h"
#define CONFIG_NUWAX_SPEAKER_ENABLE 1
#define CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC 1
#define CONFIG_NUWAX_WAKE_WORD_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 0
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define NUWAX_CONVERSATION_FIRST_PCM_TIMEOUT_MS 15000U
#define NUWAX_CONVERSATION_PLAYBACK_INGRESS_IDLE_TIMEOUT_MS 60000U
#define NUWAX_PLAYBACK_TIMEOUT_CANCEL_SEND_MS 250U
#define NUWAX_PLAYBACK_TERMINAL_WAIT_TIMEOUT_MS 15000U
#define NUWAX_PLAYBACK_FAULT_CANCEL_ACK_TIMEOUT_MS 3000U
enum { NUWAX_APP_STATE_SLEEPING, NUWAX_APP_STATE_IDLE, NUWAX_APP_STATE_LISTENING };
typedef int nuwax_ws_client_t;
typedef int esp_err_t;
static nuwax_awake_session_state_t s_awake_session;
static nuwax_normal_conversation_policy_t s_normal_conversation;
static nuwax_turn_guard_t s_turn_guard;
static nuwax_playback_terminal_guard_t s_playback_terminal_guard;
static nuwax_prelisten_state_t s_prelisten;
static nuwax_listen_resume_state_t s_listen_resume;
static struct { bool active; } s_notification_transaction;
static struct { int state; } s_head_touch_agent_intent;
#define NUWAX_HEAD_TOUCH_AGENT_STATE_IDLE 0
static atomic_bool s_reprovision_quiesce_requested;
static bool s_post_playback_resume_pending;
static uint32_t s_cancel_turn_request_seq, s_speaker_fault_recovery_epoch, s_speaker_fault_recovery_generation;
static void *s_wake_word_events;
static uint32_t now;
static int state, capture_starts, wake_rearms, cleared, status_updates, close_requests;
static uint32_t monotonic_ms(void) { return now; }
static int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }
static int conversation_state_current(void) { return state; }
static bool local_power_transition_active(void) { return power_pending; }
static bool exclusive_audio_mode_blocks_normal_audio(void) { return mode_pending||mode_owner; }
static bool nuwax_ota_device_blocks_audio(void) { return ota_active; }
static bool product_music_blocks_audio(void) { return false; }
static bool conversation_capture_busy_for_sleep(void) { return false; }
static struct { bool active; } s_touch_cancel_recovery;
static bool nuwax_audio_terminal_recovery_pending(void) { return false; }
static bool nuwax_audio_uplink_sender_busy(void) { return false; }
/* The separate dialogue-r207 liveness fixture executes this service itself. */
static bool service_normal_conversation_inactivity(void) { return false; }
static bool nuwax_app_shell_blocks_auto_followup(void) { return menu_open; }
static bool sleep_transaction_blocks_new_audio(void) { return false; }
static bool meeting_or_monitoring_mode_blocks_sleep(void) { return false; }
static bool ota_blocks_wake_detector(void) { return ota_active; }
static bool nuwax_notification_blocks_conversation(void *p) { (void)p;return notification_busy; }
static unsigned uxQueueMessagesWaiting(void *p);
static bool nuwax_speaker_playback_has_pending(void) { return false; }

static void set_state(int next) { state = next; if(next==NUWAX_APP_STATE_LISTENING) capture_starts++; }

static bool nuwax_capture_phase_cancel_recovery_needs_resume(bool a,bool b,bool c) { return a||b||c; }
static void reset_awake_wait_budget_paused(const char *r) { (void)r;nuwax_awake_session_state_reset_paused(&s_awake_session); }
static uint32_t awake_session_listen_window_ms(uint32_t t) { return nuwax_awake_session_state_remaining_ms(&s_awake_session,t); }


static void enter_sleeping_conversation_state(const char *r) { (void)r;state=NUWAX_APP_STATE_SLEEPING;nuwax_awake_session_state_clear(&s_awake_session);wake_rearms++; }
static void request_sleep_wake_word_detector(const char *r) { (void)r;wake_rearms++; }
static bool recover_post_end_cloud_response_timeout(void *p,uint32_t t) { (void)p;(void)t;return false; }
static bool finalize_playback_terminal_if_ready(const char *r) { (void)r;return false; }
static bool begin_playback_terminal_local_recovery(const char *r) { (void)r;return false; }
static bool nuwax_speaker_playback_expire_metadata_only_session(const char *a,const char *b,uint32_t t) { (void)a;(void)b;(void)t;return false; }

static void wake_greeting_metrics_cancel(const char *r) { (void)r; }
static uint32_t nuwax_ws_connection_epoch(void *p) { (void)p;return dynamic_epoch; }
static uint32_t update_normal_downlink_admission(bool b) { (void)b;return 1; }
static int nuwax_session_send_cancel_turn_until_if_epoch(void *p,const char *a,const char *b,const char *c,int64_t t,uint32_t e) { (void)p;(void)a;(void)b;(void)c;(void)t;(void)e;return 0; }
static bool nuwax_ws_request_close_if_epoch(void *p,uint32_t e) { (void)p;(void)e;close_requests++;return true; }
static bool send_playback_fault_cancel(const char *r) { (void)r;return true; }
static bool set_active_turn_phase(int p,uint32_t t) { (void)p;(void)t;return true; }
static bool nuwax_speaker_playback_request_cancel_generation(uint32_t g,const char *r,uint32_t t) { (void)g;(void)r;(void)t;return true; }

static void nuwax_speaker_playback_request_fade_cancel(const char *r,uint32_t t) { (void)r;(void)t; }
static void set_conversation_status_guarded(const char *a,const char *b,const char *c,bool d) { (void)a;(void)b;(void)c;(void)d;status_updates++; }
static size_t strlcpy(char *d,const char *s,size_t n) { size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l; }

#define CONFIG_NUWAX_VOICE_MIC_TOUCH_TRIGGER 1
#define CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE 1
#define CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE 0
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 0
#define CONFIG_NUWAX_CONVERSATION_AWAKE_IDLE_TIMEOUT_MS 60000
#define CONFIG_NUWAX_WAKE_WORD_LISTEN_GRACE_MS 650
#define NUWAX_CONVERSATION_WAKE_TEXT_TIMEOUT_MS 15000
#define NUWAX_WAKE_WORD_GREETING_REQUEST_PREFIX "wake_"
#define NUWAX_WAKE_WORD_GREETING_TEXT "hello"
#define NUWAX_INTERACTIVE_CONTROL_SEND_TIMEOUT_MS 250
#define NUWAX_WAKE_EVENT_MAX_AGE_MS 3000
#define pdTRUE 1

typedef int nuwax_app_state_t;
enum { NUWAX_WAKE_MODE_SLEEP, NUWAX_WAKE_MODE_PLAYBACK_INTERRUPT };
enum { NUWAX_SLEEP_TRANSACTION_IDLE, NUWAX_SLEEP_TRANSACTION_SLEEPING };
enum { NUWAX_APP_SHELL_ACTION_NONE, NUWAX_APP_SHELL_ACTION_START_LISTEN, NUWAX_APP_SHELL_ACTION_STOP_LISTEN };
typedef struct { int action, index; } nuwax_app_shell_result_t;
typedef struct { struct { int mode; uint32_t ws_epoch; } arm; uint32_t detected_ms; char wake_word[32]; } wake_word_event_t;
typedef struct { bool start_gate_pending,start_gate_authorized; uint32_t start_gate_generation,submitted_generation; } nuwax_speaker_playback_stats_t;
static nuwax_conversation_visual_round_t s_visual_round;
static atomic_uint s_pending_speaker_start_generation;
static bool s_deferred_speaker_start_valid;
static struct { uint32_t speaker_generation; } s_deferred_speaker_start_event;
static uint32_t s_deferred_speaker_start_epoch;
static bool s_sleep_request_pending,s_sleep_request_requires_idle_expiry,s_awake_resume_after_ws_reconnect;
static uint32_t s_awake_disconnect_ms,s_consecutive_post_end_response_timeouts;
static char s_active_turn_id[48];
static struct { bool active; int state; } s_sleep_transaction;
static struct { bool active; } s_deferred_sleep_intent;
static bool s_power_shutdown_requested;
static struct { uint32_t wake_detected_ms; } s_wake_greeting_metrics;
static uint32_t s_cloud_text_request_seq;
static void *s_shell_actions=(void*)2;
static bool queued_wake,wake_desired,tts_injected;
static unsigned uxQueueMessagesWaiting(void *p) { return p==s_wake_word_events && queued_wake ? 1U : 0U; }
static unsigned real_loop_iterations,sends;
static int send_result,scenario;
static uint32_t harness_end_ms;
static void check_conversation_turn_timeout(nuwax_ws_client_t *ws);
static uint32_t active_ws_epoch_snapshot(void) { return dynamic_epoch; }
static void clear_head_touch_agent_intent(const char *r) { (void)r; }
static void clear_touch_cancel_recovery(const char *r) { (void)r; }
static void clear_ws_reconnect_audio_recovery(void);
static void clear_sleep_request(void) { s_sleep_request_pending=false; }
static void nuwax_state_bridge_cancel_touch(void) {}
static void inhibit_sleep_wake_word_detector(const char *r) { (void)r;wake_desired=false; }
static void stop_sleep_wake_word_detector(const char *r) { inhibit_sleep_wake_word_detector(r); }
static bool wake_detector_request_is_playback(void) { return false; }
static bool nuwax_wake_word_detector_is_running(void) { return false; }
static void nuwax_speaker_playback_get_stats(nuwax_speaker_playback_stats_t *s) { memset(s,0,sizeof(*s)); }
static bool nuwax_speaker_playback_abort_metadata_only_session(const char *a,const char *b,const char *r) { (void)a;(void)b;(void)r;return false; }
static bool nuwax_speaker_playback_reject_start_generation(uint32_t g,const char *r) { (void)g;(void)r;return false; }
static void clear_tts_skip_emotion_window(const char *r) { (void)r; }
static void nuwax_speaker_playback_release_idle_output(const char *r) { (void)r; }
static bool sleep_lifecycle_blocks_new_capture(void) { return s_sleep_request_pending||s_sleep_transaction.active||s_deferred_sleep_intent.active; }
static void clear_last_listen_resume(void) { nuwax_listen_resume_state_clear(&s_listen_resume); }
static uint32_t conversation_listen_window_ms(void) { return 60000; }
static void app_task_watchdog_checkpoint(void) { real_loop_iterations++; }
static void service_power_key_events(void *ws) { (void)ws; }
static bool service_music_url_test(bool b) { (void)b;return false; }
static bool nuwax_ws_is_connected(void *ws) { (void)ws;return !transport_closed && now<harness_end_ms; }
static void complete_during_playback_wake_stop(void) {}
static void service_ota_wake_detector_recovery(void) {}
static void service_exclusive_audio_sleep_reconcile(void) {}
static void drain_shell_actions(void *ws) { (void)ws; }
static int service_application_keepalive(void *ws,const char *why) { (void)ws;(void)why;return ESP_OK; }
static bool nuwax_sleep_transaction_requires_local_sleep_reconcile(void *t,bool sleeping) { (void)t;(void)sleeping;return false; }
static void request_sleeping_conversation_state(const char *r) { enter_sleeping_conversation_state(r); }
static void service_wake_interrupt_cancel(void *ws) { (void)ws; }
static int64_t now_ms_i64(void) { return now; }
/* Transport heartbeat stays healthy; no cloud business output is fabricated. */
static int service_idle_wait_application_keepalive(void *ws) { (void)ws;return ESP_OK; }
static bool elapsed_ms(uint32_t n,uint32_t d) { return (int32_t)(n-d)>=0; }
static void clear_auto_followup(const char *r) { (void)r;nuwax_prelisten_state_clear(&s_prelisten); }
static uint32_t activate_pending_prelisten_capture(uint32_t n,const char *r) { (void)r;return nuwax_prelisten_state_activate_pending(&s_prelisten,n); }
static bool handle_playback_wake_event(void *ws,void *e) { (void)ws;(void)e;return false; }
static void nuwax_sleep_transaction_reset_for_wake(void *t) { (void)t;s_sleep_transaction.state=NUWAX_SLEEP_TRANSACTION_IDLE; }
static void wake_greeting_metrics_begin(const char *w,uint32_t t) { (void)w;s_wake_greeting_metrics.wake_detected_ms=t; }
static void wake_greeting_metrics_mark_sent(const char *r,int e) { (void)r;(void)e; }
static int nuwax_session_send_text_request_with_timeout_if_epoch(void *ws,const char *r,const char *t,uint32_t ms,uint32_t e) { (void)ws;(void)r;(void)t;assert(ms==250&&e==dynamic_epoch);sends++;return send_result; }
static int conversation_live_aec_pool_init(void) { return ESP_OK; }
static void restart_if_post_reprovision_aec_failed(const char *r) { (void)r; }
static bool take_deferred_shell_listen_action(void *p) { (void)p;return false; }
static int xQueueReceive(void *q,void *p,int timeout) { (void)timeout;if(q==s_wake_word_events&&queued_wake){queued_wake=false;wake_word_event_t *e=p;e->arm.ws_epoch=1;e->arm.mode=NUWAX_WAKE_MODE_SLEEP;e->detected_ms=now;strcpy(e->wake_word,"word");return pdTRUE;}return 0; }
static void conversation_owner_wait_ms(uint32_t ms) { now+=ms; }
static void start_prelisten_capture_now(const char *m,uint32_t w,const char *r) { (void)r;nuwax_prelisten_state_start_capture(&s_prelisten,m,now,w,false);state=NUWAX_APP_STATE_LISTENING;capture_starts++; }
static void handle_shell_action(void *ws,void *a) { (void)ws;(void)a; }
static void drain_session_commands(nuwax_ws_client_t *ws) {
    if(scenario==1&&s_turn_guard.active&&!tts_injected&&now>=1100) {
        /* Same transition as TEXT_RESPONSE(ok,tts_pending) at production line 30150. */
        assert(nuwax_turn_guard_transition(&s_turn_guard,NUWAX_TURN_PHASE_WAIT_TEXT,NUWAX_TURN_PHASE_WAIT_TTS,now,30000));
        tts_injected=true;
    }
    check_conversation_turn_timeout(ws);
}

static bool nuwax_ws_close_if_epoch(void *ws, uint32_t epoch, nuwax_ws_close_cause_t cause) {
    (void)ws;
    if (epoch != dynamic_epoch || !nuwax_ws_close_policy_admits(cause, true)) return false;
    close_requests++; transport_closed=true; return true;
}

#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define NUWAX_AWAKE_WS_RESUME_GRACE_MS 30000
#define NUWAX_WS_RECONNECT_AUDIO_RECOVERY_TIMEOUT_MS 5000
typedef enum {NUWAX_WS_RECOVERY_CONVERGED,NUWAX_WS_RECOVERY_WAIT_MODE_QUEUE,
 NUWAX_WS_RECOVERY_TRANSFERRED_TO_OWNER,NUWAX_WS_RECOVERY_MENU_BLOCKED,
 NUWAX_WS_RECOVERY_FALLBACK_SLEEP} nuwax_ws_recovery_result_t;
static struct {bool pending;uint32_t ws_epoch,awake_generation,deadline_ms;} s_ws_audio_recovery;
static bool s_during_playback_handoff_pending,s_during_playback_handoff_playback_started,s_during_playback_handoff_seamless;
static bool s_during_playback_wake_stop_pending,s_during_playback_wake_stop_playback_started;
static int s_wake_stop_handoff_lock;
static bool exclusive_audio_owner_active(void){return mode_owner;}
static void clear_awake_session(const char *r){(void)r;nuwax_awake_session_state_clear(&s_awake_session);clear_ws_reconnect_audio_recovery();}
