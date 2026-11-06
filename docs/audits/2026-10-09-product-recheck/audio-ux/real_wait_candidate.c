
#include <assert.h>
#include "candidate_policy.h"
static unsigned dynamic_epoch=1;
static bool transport_closed, menu_open, mode_owner, mode_pending;
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "conversation/awake_session_state.h"
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
static nuwax_turn_guard_t s_turn_guard;
static nuwax_playback_terminal_guard_t s_playback_terminal_guard;
static nuwax_prelisten_state_t s_prelisten;
static nuwax_listen_resume_state_t s_listen_resume; static int s_notification_transaction;
static atomic_bool s_reprovision_quiesce_requested;
static bool s_post_playback_resume_pending;
static uint32_t s_cancel_turn_request_seq, s_speaker_fault_recovery_epoch, s_speaker_fault_recovery_generation;
static void *s_wake_word_events;
static uint32_t now;
static int state, capture_starts, wake_rearms, cleared, status_updates, close_requests;
static uint32_t monotonic_ms(void) { return now; }
static int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }
static int conversation_state_current(void) { return state; }
static bool local_power_transition_active(void) { return false; }
static bool exclusive_audio_mode_blocks_normal_audio(void) { return mode_pending||mode_owner; }
static bool nuwax_app_shell_blocks_auto_followup(void) { return menu_open; }
static bool sleep_transaction_blocks_new_audio(void) { return false; }
static bool meeting_or_monitoring_mode_blocks_sleep(void) { return false; }
static bool ota_blocks_wake_detector(void) { return false; }
static bool nuwax_notification_blocks_conversation(void *p) { (void)p;return false; }
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
#define memory_order_release memory_order_relaxed
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
static void clear_conversation_visual_round(const char *reason) {
    if (!nuwax_conversation_visual_round_is_active(&s_visual_round)) {
        return;
    }
    const nuwax_visual_round_kind_t kind = s_visual_round.kind;
    const uint32_t ws_epoch = s_visual_round.ws_epoch;
    const uint32_t speaker_generation = s_visual_round.speaker_generation;
    if (nuwax_conversation_visual_round_is_interaction(&s_visual_round)) {
        nuwax_state_bridge_cancel_touch();
    }
    nuwax_conversation_visual_round_clear(&s_visual_round);
    ESP_LOGI(TAG,
             "conversation visual round cleared kind=%s epoch=%lu speaker_generation=%lu reason=%s",
             visual_round_kind_name(kind),
             (unsigned long)ws_epoch,
             (unsigned long)speaker_generation,
             reason ? reason : "-");
}

static void clear_active_turn_guard(const char *reason) {
#if CONFIG_NUWAX_SPEAKER_ENABLE
    atomic_store_explicit(&s_pending_speaker_start_generation,
                          0U,
                          memory_order_release);
    /* Close the RX admission gate before inspecting or invalidating speaker
     * state. The WebSocket task may be on another core, so leaving this until
     * the end permits a late binary frame to enter the generation being
     * cleared. */
    const bool had_active_turn = s_turn_guard.active;
    const uint32_t guard_speaker_generation =
        s_turn_guard.speaker_generation;
    if (had_active_turn && guard_speaker_generation != 0U &&
        s_playback_terminal_guard.active &&
        s_playback_terminal_guard.speaker_generation ==
            guard_speaker_generation &&
        s_playback_terminal_guard.connection_epoch ==
            active_ws_epoch_snapshot() &&
        !s_playback_terminal_guard.physical_done &&
        !s_playback_terminal_guard.faulted) {
        const uint32_t terminal_epoch =
            s_playback_terminal_guard.connection_epoch;
        (void)nuwax_playback_terminal_guard_mark_fault(
            &s_playback_terminal_guard,
            terminal_epoch,
            guard_speaker_generation,
            monotonic_ms(),
            NUWAX_PLAYBACK_TERMINAL_WAIT_TIMEOUT_MS);
        ESP_LOGW(TAG,
                 "turn owner cleared before physical playback terminal epoch=%lu generation=%lu reason=%s; bounded local recovery armed",
                 (unsigned long)terminal_epoch,
                 (unsigned long)guard_speaker_generation,
                 reason ? reason : "-");
    }
    (void)update_normal_downlink_admission(false);
    if (had_active_turn && guard_speaker_generation == 0U) {
        (void)nuwax_speaker_playback_abort_metadata_only_session(
            s_turn_guard.turn_id,
            s_turn_guard.playback_id,
            reason ? reason : "turn_clear");
    }
    nuwax_speaker_playback_stats_t speaker_stats = {0};
    nuwax_speaker_playback_get_stats(&speaker_stats);
    const uint32_t pending_start_generation =
        (speaker_stats.start_gate_pending ||
         speaker_stats.start_gate_authorized)
            ? speaker_stats.start_gate_generation
            : 0U;
    bool guard_start_rejected = false;
    if (had_active_turn) {
        guard_start_rejected =
            nuwax_speaker_playback_reject_start_generation(
            guard_speaker_generation,
            reason ? reason : "turn_clear");
    }
    if (pending_start_generation != 0U &&
        pending_start_generation != guard_speaker_generation) {
        (void)nuwax_speaker_playback_reject_start_generation(
            pending_start_generation,
            reason ? reason : "turn_clear_pending");
    }
    if (s_deferred_speaker_start_valid) {
        (void)nuwax_speaker_playback_reject_start_generation(
            s_deferred_speaker_start_event.speaker_generation,
            reason ? reason : "turn_clear_deferred");
        s_deferred_speaker_start_valid = false;
        memset(&s_deferred_speaker_start_event, 0,
               sizeof(s_deferred_speaker_start_event));
        s_deferred_speaker_start_epoch = 0U;
    }
    if (guard_speaker_generation != 0U && !guard_start_rejected) {
        (void)nuwax_speaker_playback_request_cancel_generation(
            guard_speaker_generation,
            reason ? reason : "turn_clear",
            0U);
    }
#endif
    if (s_turn_guard.active) {
        ESP_LOGI(TAG,
                 "conversation turn guard cleared epoch=%lu phase=%s request=%s turn=%s playback=%s reason=%s",
                 (unsigned long)s_turn_guard.epoch,
                 nuwax_turn_phase_name(s_turn_guard.phase),
                 s_turn_guard.request_id[0] ? s_turn_guard.request_id : "-",
                 s_turn_guard.turn_id[0] ? s_turn_guard.turn_id : "-",
                 s_turn_guard.playback_id[0] ? s_turn_guard.playback_id : "-",
                 reason ? reason : "-");
    }
    nuwax_turn_guard_clear(&s_turn_guard);
    s_active_turn_id[0] = '\0';
    clear_conversation_visual_round(reason ? reason : "turn_guard_clear");
}

static bool begin_active_turn_guard(const char *request_id,
                                    const char *turn_id,
                                    uint32_t timeout_ms,
                                    const char *reason) {
    /* A live guard owns the current cloud turn.  Replacing it in-place would
     * let late text/TTS events from the previous turn enter the new owner.
     * Callers that intentionally start a new turn must clear the old owner
     * first (awake-session and listen transitions already do so). */
    if (s_turn_guard.active) {
        ESP_LOGW(TAG,
                 "conversation turn guard start rejected while another turn is active epoch=%lu phase=%s request=%s reason=%s",
                 (unsigned long)s_turn_guard.epoch,
                 nuwax_turn_phase_name(s_turn_guard.phase),
                 s_turn_guard.request_id[0] ? s_turn_guard.request_id : "-",
                 reason ? reason : "-");
        return false;
    }
    if (!nuwax_awake_session_state_is_active(&s_awake_session)) {
        ESP_LOGW(TAG,
                 "conversation turn guard start rejected outside awake session reason=%s request=%s turn=%s",
                 reason ? reason : "-",
                 request_id && request_id[0] ? request_id : "-",
                 turn_id && turn_id[0] ? turn_id : "-");
        return false;
    }
    const uint32_t awake_generation =
        nuwax_awake_session_state_generation(&s_awake_session);
    const uint32_t epoch = nuwax_turn_guard_begin(&s_turn_guard,
                                                  awake_generation,
                                                  request_id,
                                                  turn_id,
                                                  monotonic_ms(),
                                                  timeout_ms);
    if (epoch == 0U) {
        return false;
    }
#if CONFIG_NUWAX_SPEAKER_ENABLE
    (void)update_normal_downlink_admission(true);
#endif
    if (turn_id && turn_id[0]) {
        strlcpy(s_active_turn_id, turn_id, sizeof(s_active_turn_id));
    } else {
        s_active_turn_id[0] = '\0';
    }
    ESP_LOGI(TAG,
             "conversation turn guard begun epoch=%lu awake_generation=%lu request=%s turn=%s timeout_ms=%lu reason=%s",
             (unsigned long)epoch,
             (unsigned long)awake_generation,
             request_id && request_id[0] ? request_id : "-",
             turn_id && turn_id[0] ? turn_id : "-",
             (unsigned long)timeout_ms,
             reason ? reason : "-");
    return true;
}

static void begin_awake_session(const char *reason) {
    /* A deferred local idle request belongs to the awake generation that
     * created it. Never let it cross a new wake/touch conversation boundary. */
    if (s_sleep_request_pending && s_sleep_request_requires_idle_expiry) {
        clear_sleep_request();
    }
    clear_head_touch_agent_intent(reason ? reason : "awake_session_begin");
    clear_touch_cancel_recovery(reason ? reason : "awake_session_begin");
    /* An awake conversation owns the microphone lifecycle until it returns to
     * sleep. Prevent the independent wake supervisor from racing that handoff. */
    inhibit_sleep_wake_word_detector(reason ? reason : "awake_session_begin");
    s_awake_resume_after_ws_reconnect = false;
    s_awake_disconnect_ms = 0U;
    clear_ws_reconnect_audio_recovery();
#if CONFIG_NUWAX_SPEAKER_ENABLE
    nuwax_playback_terminal_guard_clear(&s_playback_terminal_guard);
#endif
    clear_active_turn_guard(reason ? reason : "awake_session_begin");
    clear_last_listen_resume();
    s_consecutive_post_end_response_timeouts = 0U;
    nuwax_awake_session_state_begin(
        &s_awake_session,
        (uint32_t)CONFIG_NUWAX_CONVERSATION_AWAKE_IDLE_TIMEOUT_MS);
    ESP_LOGI(TAG,
             "conversation awake session begun paused reason=%s timeout_ms=%u generation=%lu",
             reason ? reason : "-",
             (unsigned)CONFIG_NUWAX_CONVERSATION_AWAKE_IDLE_TIMEOUT_MS,
              (unsigned long)nuwax_awake_session_state_generation(
                  &s_awake_session));
}

static bool begin_conversation_visual_round(
    nuwax_visual_round_kind_t kind,
    const char *request_id,
    const char *reason) {
    if (nuwax_conversation_visual_round_is_active(&s_visual_round)) {
        ESP_LOGW(TAG,
                 "conversation visual round replacing residual owner old_kind=%s new_kind=%s reason=%s",
                 visual_round_kind_name(s_visual_round.kind),
                 visual_round_kind_name(kind),
                 reason ? reason : "-");
        clear_conversation_visual_round("visual_round_replaced");
    }
    const uint32_t ws_epoch = active_ws_epoch_snapshot();
    const uint32_t awake_generation =
        nuwax_awake_session_state_generation(&s_awake_session);
    if (!nuwax_conversation_visual_round_begin(&s_visual_round,
                                                kind,
                                                ws_epoch,
                                                awake_generation,
                                                request_id)) {
        ESP_LOGE(TAG,
                 "conversation visual round start rejected kind=%s epoch=%lu awake_generation=%lu request=%s reason=%s",
                 visual_round_kind_name(kind),
                 (unsigned long)ws_epoch,
                 (unsigned long)awake_generation,
                 request_id && request_id[0] ? request_id : "-",
                 reason ? reason : "-");
        return false;
    }
    ESP_LOGI(TAG,
             "conversation visual round begun kind=%s epoch=%lu awake_generation=%lu request=%s reason=%s",
             visual_round_kind_name(kind),
             (unsigned long)ws_epoch,
             (unsigned long)awake_generation,
             request_id && request_id[0] ? request_id : "-",
             reason ? reason : "-");
    return true;
}

static void start_prelisten_window(const char *mode, uint32_t window_ms, uint32_t ready_delay_ms) {
#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
    clear_tts_skip_emotion_window("prelisten_window_start");
    if (sleep_lifecycle_blocks_new_capture()) {
        ESP_LOGI(TAG,
                 "prelisten window blocked by sleep lifecycle mode=%s state=%s deferred_token=%lu",
                 mode ? mode : "-",
                 nuwax_sleep_transaction_state_name(
                     s_sleep_transaction.state),
                 (unsigned long)s_deferred_sleep_intent.token);
        return;
    }
    if ((!mode || strcmp(mode, "during_playback") != 0) &&
        nuwax_playback_terminal_guard_waiting(
            &s_playback_terminal_guard)) {
        return;
    }
    if (!mode || strcmp(mode, "during_playback") != 0) {
        clear_active_turn_guard("new_listen_window");
    }
#if CONFIG_NUWAX_SPEAKER_ENABLE
    nuwax_speaker_playback_release_idle_output("prelisten_window");
#endif
    stop_sleep_wake_word_detector("prelisten_window");
    const uint32_t now_ms = monotonic_ms();
    nuwax_prelisten_state_start_window(&s_prelisten, mode, now_ms, window_ms, ready_delay_ms);
    set_conversation_status_guarded("VOICE", "LISTENING", "listening", true);
    set_state(NUWAX_APP_STATE_LISTENING);
    ESP_LOGI(TAG,
             "conversation prelisten pending mode=%s window_ms=%lu grace_ms=%lu",
             nuwax_prelisten_state_mode_or(&s_prelisten, "prelisten"),
             (unsigned long)(window_ms == 0 ? 1 : window_ms),
             (unsigned long)ready_delay_ms);
#else
    (void)mode;
    (void)window_ms;
    (void)ready_delay_ms;
#endif
}

static void return_to_listening_after_cancel(const char *reason) {
    if (local_power_transition_active()) {
        /* A local power-key sleep owns the post-cancel lifecycle.  A late
         * speaker callback must not re-arm microphone capture underneath it. */
        ESP_LOGI(TAG,
                 "listening handoff suppressed while local power sleep converges reason=%s",
                 reason ? reason : "-");
        return;
    }
#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
    if (wake_detector_request_is_playback() ||
        nuwax_wake_word_detector_is_running()) {
        stop_sleep_wake_word_detector(
            reason ? reason : "cancel_followup");
    }
    clear_last_listen_resume();
    start_prelisten_window(reason ? reason : "cancel_followup",
                           awake_session_listen_window_ms(monotonic_ms()),
                           0);
#else
    (void)reason;
    set_state(NUWAX_APP_STATE_IDLE);
#endif
}

static void recover_normal_conversation_after_playback_abort(
    const char *reason) {
    if (local_power_transition_active()) {
        ESP_LOGI(TAG,
                 "playback recovery handoff suppressed while local power sleep converges reason=%s",
                 reason ? reason : "-");
        return;
    }
    if (conversation_state_current() == NUWAX_APP_STATE_SLEEPING) {
        return;
    }
    if (nuwax_awake_session_state_is_active(&s_awake_session)) {
        reset_awake_wait_budget_paused(
            reason ? reason : "playback_abort");
    }
#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
    if (nuwax_capture_phase_cancel_recovery_needs_resume(
            s_prelisten.capture_active,
            nuwax_prelisten_state_mode_is(&s_prelisten,
                                            "during_playback"),
            nuwax_prelisten_state_mode_is(&s_prelisten, "barge_in"))) {
        s_post_playback_resume_pending = true;
        set_state(NUWAX_APP_STATE_LISTENING);
        ESP_LOGI(TAG,
                 "speaker fault recovery kept active %s capture and requested post-playback resume reason=%s",
                 nuwax_prelisten_state_mode_or(&s_prelisten,
                                                "during_playback"),
                 reason ? reason : "-");
        return;
    }
#endif
    return_to_listening_after_cancel(reason ? reason : "playback_abort");
}

static bool awake_idle_sleep_admitted(uint32_t now_ms) {
#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
    /* At the 60-second boundary END/listen-stop may already be committed
     * while the matching cloud response is still queued. Keep the awake
     * owner until that bounded response window converges. */
    if (nuwax_listen_resume_state_holds_idle_expiry_boundary(
            &s_listen_resume)) {
        return false;
    }
#endif
#if CONFIG_NUWAX_SPEAKER_ENABLE
    const bool playback_active = nuwax_speaker_playback_has_pending();
#else
    const bool playback_active = false;
#endif
    return nuwax_awake_session_state_can_enter_idle_sleep(
        &s_awake_session,
        now_ms,
        conversation_state_current() == NUWAX_APP_STATE_LISTENING,
        s_turn_guard.active,
        playback_active,
        false);
}

static void maybe_enter_idle_sleep(void) {
    const uint32_t now_ms = monotonic_ms();
    if (nuwax_awake_session_state_is_active(&s_awake_session)) {
        if (awake_idle_sleep_admitted(now_ms)) {
            if (sleep_transaction_blocks_new_audio()) {
                return;
            }
            enter_sleeping_conversation_state("awake_idle_timeout");
            return;
        }
        if (!nuwax_awake_session_state_is_waiting(&s_awake_session)) {
#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
            if (conversation_state_current() == NUWAX_APP_STATE_IDLE &&
                !s_prelisten.pending && !s_prelisten.capture_active &&
                !exclusive_audio_mode_blocks_normal_audio() &&
                !nuwax_app_shell_blocks_auto_followup()) {
                ESP_LOGI(TAG,
                         "conversation awake paused session resuming after idle/menu handoff remaining_ms=%lu",
                         (unsigned long)nuwax_awake_session_state_remaining_ms(
                             &s_awake_session, now_ms));
                start_prelisten_window("awake_session",
                                       awake_session_listen_window_ms(now_ms),
                                       0);
            }
#endif
            return;
        }
#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
        if (!s_prelisten.pending && !s_prelisten.capture_active &&
            !exclusive_audio_mode_blocks_normal_audio() &&
            !nuwax_app_shell_blocks_auto_followup()) {
            const uint32_t remaining_ms =
                nuwax_awake_session_state_remaining_ms(&s_awake_session, now_ms);
            const uint32_t window_ms = awake_session_listen_window_ms(now_ms);
            ESP_LOGI(TAG,
                     "conversation awake session maintaining listen remaining_ms=%lu window_ms=%lu",
                     (unsigned long)remaining_ms,
                     (unsigned long)window_ms);
            start_prelisten_window("awake_session", window_ms, 0);
        }
#endif
        return;
    }
    /* Ordinary IDLE is intentionally not an automatic sleep state.  The
     * only local inactivity deadline is the rolling awake-session budget
     * above, which is started by a wake/touch/speech event and expires only
     * while the normal conversation path is LISTENING. */
    (void)now_ms;
}

static void ensure_sleep_wake_word_detector(void) {
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
    if (atomic_load_explicit(&s_reprovision_quiesce_requested,
                             memory_order_acquire) ||
        conversation_state_current() != NUWAX_APP_STATE_SLEEPING ||
        /* The owner is awake before deferred feedback reaches the UI. Do not
         * let sleep-health re-arm MultiNet during greeting admission. */
        nuwax_awake_session_state_is_active(&s_awake_session) ||
        s_turn_guard.active ||
        nuwax_notification_blocks_conversation(
            &s_notification_transaction) ||
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE
        nuwax_offline_audio_capture_running() ||
#endif
        meeting_or_monitoring_mode_blocks_sleep() ||
        ota_blocks_wake_detector()) {
        return;
    }
    if (s_wake_word_events && uxQueueMessagesWaiting(s_wake_word_events) > 0U) {
        return;
    }
    request_sleep_wake_word_detector("sleep_health");
#endif
}


static wg_ticket_t pending_greeting_recovery;
static bool ota_active;
static bool candidate_service_recovery(nuwax_ws_client_t *ws) {
    (void)ws;
    const wg_action_t action=wg_decide(&pending_greeting_recovery,dynamic_epoch,
        s_awake_session.generation,s_turn_guard.epoch,s_turn_guard.active,
        !transport_closed,s_awake_session.active,
        s_sleep_transaction.active||s_sleep_transaction.state==NUWAX_SLEEP_TRANSACTION_SLEEPING||s_sleep_request_pending||s_deferred_sleep_intent.active,
        s_power_shutdown_requested,ota_active,atomic_load(&s_reprovision_quiesce_requested),false,mode_owner||mode_pending);
    if(action==WG_ROTATE_EPOCH) {
        assert(wg_candidate_close_admits(WG_CLOSE_UNRESOLVED_GREETING,true));
        close_requests++;transport_closed=true;pending_greeting_recovery.valid=false;return true;
    }
    if(action==WG_YIELD_NEW_OWNER)pending_greeting_recovery.valid=false;
    return action==WG_DEFER_OTA||action==WG_YIELD_NEW_OWNER;
}

static void check_conversation_turn_timeout(nuwax_ws_client_t *ws) {
    const uint32_t now_ms = monotonic_ms();
    if (recover_post_end_cloud_response_timeout(ws, now_ms)) {
        return;
    }
    if (nuwax_playback_terminal_guard_ready(&s_playback_terminal_guard)) {
        (void)finalize_playback_terminal_if_ready(
            "terminal_barrier_poll");
        return;
    }
    if (nuwax_playback_terminal_guard_deadline_expired(
            &s_playback_terminal_guard, now_ms)) {
        const uint32_t terminal_epoch =
            s_playback_terminal_guard.connection_epoch;
        const uint32_t terminal_generation =
            s_playback_terminal_guard.speaker_generation;
        if (!s_playback_terminal_guard.physical_done) {
            ESP_LOGE(TAG,
                     "playback terminal liveness expired without physical stop epoch=%lu generation=%lu; starting bounded local speaker recovery and retaining WebSocket",
                     (unsigned long)terminal_epoch,
                     (unsigned long)terminal_generation);
            (void)begin_playback_terminal_local_recovery(
                "playback_terminal_liveness_timeout");
            return;
        }
        if (!nuwax_playback_terminal_guard_commit_local_recovery(
                &s_playback_terminal_guard,
                terminal_epoch,
                terminal_generation)) {
            ESP_LOGE(TAG,
                     "playback terminal local quarantine binding failed epoch=%lu generation=%lu; retaining WebSocket and retrying barrier",
                     (unsigned long)terminal_epoch,
                     (unsigned long)terminal_generation);
            return;
        }
        ESP_LOGW(TAG,
                 "playback terminal cloud grace expired epoch=%lu generation=%lu turn=%s playback=%s stream_end=%d logical_end=%d cancel_sent=%d cancel_ack=%d; old generation quarantined locally and WebSocket retained",
                 (unsigned long)terminal_epoch,
                 (unsigned long)terminal_generation,
                 s_playback_terminal_guard.turn_id[0]
                     ? s_playback_terminal_guard.turn_id
                     : "-",
                 s_playback_terminal_guard.playback_id[0]
                     ? s_playback_terminal_guard.playback_id
                     : "-",
                 s_playback_terminal_guard.stream_end_seen ? 1 : 0,
                 s_playback_terminal_guard.logical_end_seen ? 1 : 0,
                 s_playback_terminal_guard.cancel_sent ? 1 : 0,
                 s_playback_terminal_guard.cancel_ack_seen ? 1 : 0);
        (void)finalize_playback_terminal_if_ready(
            "playback_terminal_local_quarantine");
        return;
    }
    bool first_pcm_timeout = false;
#if CONFIG_NUWAX_SPEAKER_ENABLE
    if (s_turn_guard.active && s_turn_guard.phase == NUWAX_TURN_PHASE_PLAYING &&
        s_turn_guard.speaker_generation == 0U) {
        /* Uses the original playback_begin timestamp (duplicate metadata does
         * not renew it), with a locked START/readiness/physical-owner check. */
        first_pcm_timeout = nuwax_speaker_playback_expire_metadata_only_session(
            s_turn_guard.turn_id, s_turn_guard.playback_id,
            NUWAX_CONVERSATION_FIRST_PCM_TIMEOUT_MS);
    }
#endif
    if (!first_pcm_timeout &&
        !nuwax_turn_guard_deadline_expired(&s_turn_guard, now_ms)) {
        return;
    }

    const wg_failure_t recovery_failure = first_pcm_timeout ? WG_FAILURE_FIRST_PCM_TIMEOUT :
        s_turn_guard.phase==NUWAX_TURN_PHASE_WAIT_TEXT ? WG_FAILURE_TEXT_TIMEOUT :
        s_turn_guard.phase==NUWAX_TURN_PHASE_WAIT_TTS ? WG_FAILURE_TTS_TIMEOUT : WG_FAILURE_NONE;
    const wg_ticket_t greeting_ticket=wg_capture(&s_visual_round,&s_turn_guard,
        active_ws_epoch_snapshot(),s_awake_session.generation,recovery_failure);
    const uint32_t epoch = s_turn_guard.epoch;
    const uint32_t speaker_generation = s_turn_guard.speaker_generation;
    const nuwax_turn_phase_t phase = s_turn_guard.phase;
    const uint32_t phase_elapsed_ms =
        now_ms - s_turn_guard.phase_started_ms;
    const bool playback_ingress_idle_timeout =
        phase == NUWAX_TURN_PHASE_PLAYING;
    const bool metadata_retry_timeout =
        phase == NUWAX_TURN_PHASE_WAIT_TTS_RETRY;
    const char *timeout_reason =
        playback_ingress_idle_timeout
            ? (first_pcm_timeout ? "tts_first_pcm_timeout" : "tts_no_audio_timeout")
            : (metadata_retry_timeout ? "tts_metadata_retry_timeout"
                                      : "cloud_phase_timeout");
    char request_id[sizeof(s_turn_guard.request_id)] = {0};
    char turn_id[sizeof(s_turn_guard.turn_id)] = {0};
    char playback_id[sizeof(s_turn_guard.playback_id)] = {0};
    strlcpy(request_id, s_turn_guard.request_id, sizeof(request_id));
    strlcpy(turn_id, s_turn_guard.turn_id, sizeof(turn_id));
    strlcpy(playback_id, s_turn_guard.playback_id, sizeof(playback_id));

    ESP_LOGE(TAG,
             "conversation timeout reason=%s epoch=%lu phase=%s phase_elapsed_ms=%lu ingress_idle_limit_ms=%lu request=%s turn=%s playback=%s speaker_generation=%lu; aborting current turn while retaining WebSocket",
             timeout_reason,
             (unsigned long)epoch,
             nuwax_turn_phase_name(phase),
             (unsigned long)phase_elapsed_ms,
             (unsigned long)
                 NUWAX_CONVERSATION_PLAYBACK_INGRESS_IDLE_TIMEOUT_MS,
             request_id[0] ? request_id : "-",
             turn_id[0] ? turn_id : "-",
             playback_id[0] ? playback_id : "-",
             (unsigned long)speaker_generation);
    clear_last_listen_resume();
    wake_greeting_metrics_cancel(timeout_reason);
#if CONFIG_NUWAX_SPEAKER_ENABLE
    const uint32_t connection_epoch =
        ws ? nuwax_ws_connection_epoch(ws) : 0U;
    char cancel_request_id[NUWAX_PLAYBACK_CANCEL_REQUEST_ID_MAX_LEN] = {0};
    bool cancel_sent = false;
    bool cancel_send_failed = false;
    if (playback_ingress_idle_timeout) {
        /* The first-PCM or ingress-idle budget expired. Close admission first
         * so late audio cannot enter the next listen owner. */
        (void)update_normal_downlink_admission(false);
        snprintf(cancel_request_id,
                 sizeof(cancel_request_id),
                 "timeout_cancel_%lu",
                 (unsigned long)++s_cancel_turn_request_seq);
        const int64_t cancel_deadline_us =
            esp_timer_get_time() +
            (int64_t)NUWAX_PLAYBACK_TIMEOUT_CANCEL_SEND_MS * 1000LL;
        const esp_err_t cancel_err =
            connection_epoch != 0U && turn_id[0] != '\0'
                ? nuwax_session_send_cancel_turn_until_if_epoch(
                      ws,
                      cancel_request_id,
                      turn_id,
                      timeout_reason,
                      cancel_deadline_us,
                      connection_epoch)
                : ESP_ERR_INVALID_STATE;
        if (cancel_err != ESP_OK) {
            cancel_send_failed = true;
            ESP_LOGW(TAG,
                     "playback ingress idle timeout cancel_turn failed epoch=%lu generation=%lu err=%s; local recovery continues and WebSocket is retained",
                     (unsigned long)connection_epoch,
                     (unsigned long)speaker_generation,
                     esp_err_to_name(cancel_err));
        } else {
            cancel_sent = true;
            ESP_LOGI(TAG,
                     "playback ingress idle timeout cancel_turn sent request=%s epoch=%lu generation=%lu turn=%s reason=%s",
                     cancel_request_id,
                     (unsigned long)connection_epoch,
                     (unsigned long)speaker_generation,
                     turn_id,
                     timeout_reason);
        }
    }
    if (speaker_generation != 0U) {
        if (!nuwax_playback_terminal_guard_mark_fault(
                &s_playback_terminal_guard,
                connection_epoch,
                speaker_generation,
                now_ms,
                NUWAX_PLAYBACK_TERMINAL_WAIT_TIMEOUT_MS)) {
            ESP_LOGE(TAG,
                     "conversation timeout could not bind playback terminal barrier reason=%s epoch=%lu generation=%lu; reconnecting",
                     timeout_reason,
                     (unsigned long)connection_epoch,
                     (unsigned long)speaker_generation);
            if (ws) {
                (void)nuwax_ws_request_close_if_epoch(ws, connection_epoch);
            }
            return;
        }
        if (cancel_sent) {
            if (!nuwax_playback_terminal_guard_mark_cancel_sent(
                    &s_playback_terminal_guard,
                    connection_epoch,
                    speaker_generation,
                    cancel_request_id,
                    now_ms,
                    NUWAX_PLAYBACK_FAULT_CANCEL_ACK_TIMEOUT_MS)) {
                ESP_LOGW(TAG,
                         "conversation timeout cancel transaction could not bind epoch=%lu generation=%lu request=%s; local terminal quarantine remains armed and WebSocket is retained",
                         (unsigned long)connection_epoch,
                         (unsigned long)speaker_generation,
                         cancel_request_id);
            }
        } else if (!cancel_send_failed) {
            (void)send_playback_fault_cancel(timeout_reason);
        }
        if (s_turn_guard.phase != NUWAX_TURN_PHASE_DRAINING) {
            (void)set_active_turn_phase(
                NUWAX_TURN_PHASE_DRAINING,
                NUWAX_PLAYBACK_TERMINAL_WAIT_TIMEOUT_MS);
        }
        s_speaker_fault_recovery_epoch = connection_epoch;
        s_speaker_fault_recovery_generation = speaker_generation;
        (void)nuwax_speaker_playback_request_cancel_generation(
            speaker_generation,
            timeout_reason,
            0U);
    } else {
        clear_active_turn_guard(timeout_reason);
        nuwax_speaker_playback_request_fade_cancel(timeout_reason, 0U);
        if(greeting_ticket.valid) {
            pending_greeting_recovery=greeting_ticket;
            if(candidate_service_recovery(ws))return;
        }
        recover_normal_conversation_after_playback_abort(
            timeout_reason);
    }
#else
    clear_active_turn_guard(timeout_reason);
    recover_normal_conversation_after_playback_abort(timeout_reason);
#endif
    set_conversation_status_guarded(playback_ingress_idle_timeout
                                        ? "TTS IDLE TIMEOUT"
                                        : (metadata_retry_timeout
                                               ? "TTS RETRY TIMEOUT"
                                               : "CLOUD TIMEOUT"),
                                    "LISTENING",
                                    "listening",
                                    true);
}

static esp_err_t send_wake_word_greeting(nuwax_ws_client_t *ws, const char *wake_word) {
    char request_id[48];
    snprintf(request_id,
             sizeof(request_id),
             NUWAX_WAKE_WORD_GREETING_REQUEST_PREFIX "%lu",
             (unsigned long)++s_cloud_text_request_seq);
    ESP_LOGI(TAG,
             "Sending wake greeting id=%s wake_word=%s text=%s",
             request_id,
             wake_word && wake_word[0] ? wake_word : "-",
             NUWAX_WAKE_WORD_GREETING_TEXT);
    /* Establish ownership before putting bytes on the wire.  A response can
     * arrive immediately after the send, so creating the guard afterwards is
     * a real race rather than just an ordering preference. */
    if (!begin_active_turn_guard(request_id,
                                 NULL,
                                 NUWAX_CONVERSATION_WAKE_TEXT_TIMEOUT_MS,
                                 "wake_greeting")) {
        ESP_LOGE(TAG,
                 "wake greeting turn guard could not be established; request not sent");
        wake_greeting_metrics_cancel("wake_greeting_guard_failed");
        return ESP_ERR_INVALID_STATE;
    }
    if (!begin_conversation_visual_round(
            NUWAX_VISUAL_ROUND_WAKE_GREETING,
            request_id,
            "wake_greeting")) {
        clear_active_turn_guard("wake_greeting_visual_round_failed");
        wake_greeting_metrics_cancel("wake_greeting_visual_round_failed");
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t expected_epoch = active_ws_epoch_snapshot();
    if (expected_epoch == 0U ||
        nuwax_ws_connection_epoch(ws) != expected_epoch) {
        clear_active_turn_guard("wake_greeting_epoch_lost");
        wake_greeting_metrics_cancel("wake_greeting_epoch_lost");
        return ESP_ERR_INVALID_STATE;
    }
    /* Keep the current face until the owned greeting actually starts playing.
     * Playback metadata alone does not mean audio is available. The existing
     * generation-checked first-PCM handler publishes LISTENING; playback
     * completion remains the only successful-greeting capture handoff. */
    ESP_LOGI(TAG,
             "wake feedback deferred revision=wake-pcm-v3 request=%s wake_to_request_ms=%ld capture_started=0 trigger=first_pcm",
             request_id,
             (long)metrics_delta_ms(s_wake_greeting_metrics.wake_detected_ms,
                                   monotonic_ms()));
    esp_err_t err = nuwax_session_send_text_request_with_timeout_if_epoch(
        ws,
        request_id,
        NUWAX_WAKE_WORD_GREETING_TEXT,
        NUWAX_INTERACTIVE_CONTROL_SEND_TIMEOUT_MS,
        expected_epoch);
    wake_greeting_metrics_mark_sent(request_id, err);
    if (err != ESP_OK) {
        /* No response may be admitted to a request whose send failed.  The
         * caller will return to listening and can retry on the next wake. */
        clear_active_turn_guard("wake_greeting_send_failed");
    }
    return err;
}

static esp_err_t wait_for_mic_start_action(nuwax_ws_client_t *ws) {
#if CONFIG_NUWAX_VOICE_MIC_TOUCH_TRIGGER
    if (!s_shell_actions) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE || \
    CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE
    ESP_LOGI(TAG,
             "Waiting for wake word or enabled touch conversation control");
#else
    ESP_LOGI(TAG,
             "Waiting for local wake word; touch wake and touch interruption are disabled");
#endif
    while (true) {
        app_task_watchdog_checkpoint();
        service_power_key_events(ws);
        /* Cancellation is serviced before connection/state exits so the
         * isolated HTTP worker cannot outlive a product preemption edge. */
        (void)service_music_url_test(false);
        if (s_power_shutdown_requested) {
            return ESP_ERR_INVALID_STATE;
        }
        if (atomic_load_explicit(&s_reprovision_quiesce_requested,
                                 memory_order_acquire)) {
            ESP_LOGI(TAG,
                     "normal listen wait preempted by BLE reprovision");
            return ESP_ERR_INVALID_STATE;
        }
        if (!nuwax_ws_is_connected(ws)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (exclusive_audio_mode_blocks_normal_audio()) {
            ESP_LOGI(TAG, "normal listen wait preempted by exclusive audio mode");
            return ESP_ERR_INVALID_STATE;
        }
        complete_during_playback_wake_stop();
        if (s_prelisten.capture_active) {
            return ESP_OK;
        }
        drain_session_commands(ws);
        /* OTA commands are consumed inside this blocking wake wait. Service
         * the non-reboot recovery marker here as well as in the outer owner
         * loop; otherwise the outer loop cannot run until a wake or another
         * lifecycle event breaks this wait. */
        service_ota_wake_detector_recovery();
        (void)candidate_service_recovery(ws);
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
        /* The control-center test switch must be serviced while the normal
         * conversation owner is waiting for a wake word as well as during an
         * exclusive meeting/monitoring run. */
        drain_shell_actions(ws);
        if (nuwax_wifi_simulated_offline()) {
            return ESP_ERR_INVALID_STATE;
        }
#endif
        /* PLAYBACK_DONE is delivered through drain_session_commands() and can
         * start post-playback capture synchronously. Recheck before entering
         * the generic owner wait; otherwise every successful handoff pays an
         * avoidable wait of up to 100 ms despite the capture already being
         * armed. */
        if (s_prelisten.capture_active) {
            return ESP_OK;
        }
        /* A committed sleep state still owns the wake-word detector, but it
         * does not need to tear down and recreate the normal audio source on
         * every poll. Keep this wait loop alive so it can consume a wake word
         * or a notification command while refusing normal MIC ownership.
         * Pending/quiescing sleep, notification takeover, reprovision, and
         * power shutdown remain hard preemption paths below. */
        const bool sleeping_idle_wait =
            conversation_state_current() == NUWAX_APP_STATE_SLEEPING &&
            !s_sleep_transaction.active &&
            !s_sleep_request_pending &&
            !s_deferred_sleep_intent.active &&
            !nuwax_notification_blocks_conversation(
                &s_notification_transaction);
        if (sleep_lifecycle_blocks_new_capture() &&
            !sleeping_idle_wait) {
            return ESP_ERR_INVALID_STATE;
        }
        if (nuwax_sleep_transaction_requires_local_sleep_reconcile(
                &s_sleep_transaction,
                conversation_state_current() == NUWAX_APP_STATE_SLEEPING)) {
            request_sleeping_conversation_state(
                "idle_wait_sleep_state_reconcile");
            continue;
        }
        service_wake_interrupt_cancel(ws);
        maybe_enter_idle_sleep();
        const bool music_url_test_active = service_music_url_test(true);
        if (!music_url_test_active) {
            ensure_sleep_wake_word_detector();
        }
        int64_t now_ms = now_ms_i64();
        esp_err_t activity_err =
            service_idle_wait_application_keepalive(ws);
        if (activity_err != ESP_OK) {
            return activity_err;
        }
        if (s_prelisten.pending) {
            const uint32_t now32 = (uint32_t)now_ms;
            if (elapsed_ms(now32, s_prelisten.deadline_ms)) {
                clear_auto_followup("expired");
            } else if (!nuwax_app_shell_blocks_auto_followup() &&
                       elapsed_ms(now32, s_prelisten.ready_ms)) {
                (void)activate_pending_prelisten_capture(now32, "started");
                return ESP_OK;
            }
        }
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
        if (s_wake_word_events) {
            wake_word_event_t wake_event = {0};
            if (xQueueReceive(s_wake_word_events, &wake_event, 0) == pdTRUE) {
                if (wake_event.arm.mode ==
                    NUWAX_WAKE_MODE_PLAYBACK_INTERRUPT) {
                    (void)handle_playback_wake_event(ws, &wake_event);
                    continue;
                }
                const uint32_t now32 = monotonic_ms();
                const uint32_t active_epoch = active_ws_epoch_snapshot();
                if (wake_event.arm.ws_epoch == 0U ||
                    wake_event.arm.ws_epoch != active_epoch ||
                    elapsed_ms(now32,
                               wake_event.detected_ms +
                                   NUWAX_WAKE_EVENT_MAX_AGE_MS)) {
                    ESP_LOGW(TAG,
                              "stale wake word event discarded event_epoch=%lu active_epoch=%lu age_ms=%lu",
                              (unsigned long)wake_event.arm.ws_epoch,
                             (unsigned long)active_epoch,
                             (unsigned long)(now32 - wake_event.detected_ms));
                    request_sleep_wake_word_detector("stale_wake_event");
                    continue;
                }
                const nuwax_app_state_t current_state =
                    conversation_state_current();
                if (s_deferred_sleep_intent.active) {
                    ESP_LOGI(TAG,
                             "wake word ignored while sleep-after-playback intent is pending token=%lu",
                             (unsigned long)s_deferred_sleep_intent.token);
                    continue;
                }
                if (s_sleep_transaction.active) {
                    ESP_LOGI(TAG,
                             "wake word deferred while sleep transaction is quiescing token=%lu state=%s",
                             (unsigned long)s_sleep_transaction.token,
                             nuwax_sleep_transaction_state_name(
                                 s_sleep_transaction.state));
                    continue;
                }
                if (s_sleep_transaction.state ==
                    NUWAX_SLEEP_TRANSACTION_SLEEPING) {
                    nuwax_sleep_transaction_reset_for_wake(
                        &s_sleep_transaction);
                }
                ESP_LOGI(TAG,
                         "conversation wake by local wake word word=%s state=%d",
                         wake_event.wake_word[0] ? wake_event.wake_word : "-",
                         (int)current_state);
                begin_awake_session("wake_word");
                wake_greeting_metrics_begin(wake_event.wake_word,
                                            wake_event.detected_ms);
                stop_sleep_wake_word_detector("wake_word_greeting");
                clear_auto_followup("wake_word_greeting");
                clear_last_listen_resume();
                /* First PCM owns the greeting visual; owned playback completion
                 * controls microphone capture. */
                set_conversation_status_guarded(
                    "VOICE", "WAKE_REPLY", NUWAX_WAKE_WORD_GREETING_TEXT, false);
                esp_err_t greet_err = send_wake_word_greeting(ws, wake_event.wake_word);
                if (greet_err != ESP_OK) {
                    ESP_LOGW(TAG, "wake greeting send failed: %s; fallback to listen", esp_err_to_name(greet_err));
                    reset_awake_wait_budget_paused("wake_greeting_failed");
                    start_prelisten_window("wake_word",
                                           conversation_listen_window_ms(),
                                           (uint32_t)CONFIG_NUWAX_WAKE_WORD_LISTEN_GRACE_MS);
                } else {
                    const esp_err_t aec_err =
                        conversation_live_aec_pool_init();
                    if (aec_err != ESP_OK) {
                        ESP_LOGW(TAG,
                                 "wake greeting AEC prewarm deferred: %s",
                                 esp_err_to_name(aec_err));
                        restart_if_post_reprovision_aec_failed(
                            "wake_greeting_prewarm");
                    }
                }
                continue;
            }
        }
#endif
        const esp_err_t keepalive_err =
            service_idle_wait_application_keepalive(ws);
        if (keepalive_err != ESP_OK) {
            return keepalive_err;
        }
        nuwax_app_shell_result_t action = {0};
        if (!take_deferred_shell_listen_action(&action) &&
            xQueueReceive(s_shell_actions, &action, 0) != pdTRUE) {
            conversation_owner_wait_ms(100U);
            continue;
        }
        if (action.action == NUWAX_APP_SHELL_ACTION_START_LISTEN) {
            const nuwax_app_state_t current_state =
                conversation_state_current();
            if (s_deferred_sleep_intent.active) {
                ESP_LOGI(TAG,
                         "touch listen ignored while sleep-after-playback intent is pending token=%lu",
                         (unsigned long)s_deferred_sleep_intent.token);
                continue;
            }
            if (s_sleep_transaction.active) {
                ESP_LOGI(TAG,
                         "touch wake deferred while sleep transaction is quiescing token=%lu state=%s",
                         (unsigned long)s_sleep_transaction.token,
                         nuwax_sleep_transaction_state_name(
                             s_sleep_transaction.state));
                continue;
            }
            if (s_sleep_transaction.state ==
                NUWAX_SLEEP_TRANSACTION_SLEEPING) {
                nuwax_sleep_transaction_reset_for_wake(
                    &s_sleep_transaction);
            }
            begin_awake_session(current_state == NUWAX_APP_STATE_SLEEPING
                                    ? "touch_wake"
                                    : "touch_listen");
            if (s_prelisten.pending) {
                const uint32_t now32 = (uint32_t)now_ms;
                if (elapsed_ms(now32, s_prelisten.deadline_ms)) {
                    clear_auto_followup("expired");
                } else {
                    (void)activate_pending_prelisten_capture(now32, "touch_start");
                    return ESP_OK;
                }
            }
            const char *mode = (current_state == NUWAX_APP_STATE_SLEEPING) ? "touch_wake" : "touch_listen";
            if (current_state == NUWAX_APP_STATE_SLEEPING) {
                ESP_LOGI(TAG, "conversation wake by touch_hold");
            }
            reset_awake_wait_budget_paused("touch_listen_ready");
            start_prelisten_capture_now(mode, conversation_listen_window_ms(), "touch_start");
            return ESP_OK;
        }
        if (action.action == NUWAX_APP_SHELL_ACTION_STOP_LISTEN) {
            continue;
        }
        handle_shell_action(ws, &action);
    }
#else
    vTaskDelay(pdMS_TO_TICKS(CONFIG_NUWAX_VOICE_FRAME_DURATION_MS * CONFIG_NUWAX_VOICE_MIC_FRAMES_PER_BURST));
    return nuwax_ws_is_connected(ws) ? ESP_OK : ESP_ERR_INVALID_STATE;
#endif
}
static void reset(int kind) {
    memset(&s_awake_session,0,sizeof(s_awake_session));nuwax_prelisten_state_clear(&s_prelisten);
    nuwax_turn_guard_clear(&s_turn_guard);nuwax_playback_terminal_guard_clear(&s_playback_terminal_guard);
    nuwax_conversation_visual_round_clear(&s_visual_round);clear_last_listen_resume();
    scenario=kind;now=100;harness_end_ms=600100;state=NUWAX_APP_STATE_SLEEPING;
    send_result=kind==2?ESP_ERR_INVALID_STATE:ESP_OK;
    queued_wake=true;wake_desired=false;tts_injected=false;s_wake_word_events=(void*)1;
    real_loop_iterations=sends=0;capture_starts=wake_rearms=cleared=status_updates=close_requests=0;
    s_sleep_transaction.state=NUWAX_SLEEP_TRANSACTION_SLEEPING;
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
static void pause_awake_waiting(const char *reason) {
    if (!nuwax_awake_session_state_is_waiting(&s_awake_session)) {
        return;
    }
    const uint32_t now_ms = monotonic_ms();
    nuwax_awake_session_state_pause(&s_awake_session, now_ms);
    ESP_LOGI(TAG,
             "conversation awake idle timer paused reason=%s remaining_ms=%lu generation=%lu",
             reason ? reason : "-",
             (unsigned long)nuwax_awake_session_state_remaining_ms(
                 &s_awake_session, now_ms),
             (unsigned long)nuwax_awake_session_state_generation(
                 &s_awake_session));
}

static void clear_ws_reconnect_audio_recovery(void) {
    memset(&s_ws_audio_recovery, 0, sizeof(s_ws_audio_recovery));
}

static bool preserve_awake_session_for_ws_reconnect(const char *reason) {
    if (!nuwax_awake_session_state_is_active(&s_awake_session)) {
        return false;
    }
    const uint32_t now_ms = monotonic_ms();
    if (nuwax_awake_session_state_is_expired(&s_awake_session, now_ms) ||
        nuwax_awake_session_state_remaining_ms(&s_awake_session, now_ms) == 0U) {
        return false;
    }
    pause_awake_waiting(reason ? reason : "ws_disconnect");
    clear_head_touch_agent_intent(reason ? reason : "ws_disconnect");
    clear_active_turn_guard(reason ? reason : "ws_disconnect");
    clear_last_listen_resume();
    clear_auto_followup(reason ? reason : "ws_disconnect");
    s_during_playback_handoff_pending = false;
    s_during_playback_handoff_playback_started = false;
    s_during_playback_handoff_seamless = false;
    portENTER_CRITICAL(&s_wake_stop_handoff_lock);
    s_during_playback_wake_stop_pending = false;
    s_during_playback_wake_stop_playback_started = false;
    portEXIT_CRITICAL(&s_wake_stop_handoff_lock);
    s_post_playback_resume_pending = false;
    clear_ws_reconnect_audio_recovery();
    nuwax_prelisten_state_clear(&s_prelisten);
    s_awake_resume_after_ws_reconnect = true;
    s_awake_disconnect_ms = now_ms;
    ESP_LOGI(TAG,
             "conversation awake session preserved for WebSocket reconnect reason=%s remaining_ms=%lu generation=%lu grace_ms=%u",
             reason ? reason : "ws_disconnect",
             (unsigned long)nuwax_awake_session_state_remaining_ms(
                 &s_awake_session, now_ms),
             (unsigned long)nuwax_awake_session_state_generation(
                 &s_awake_session),
             (unsigned)NUWAX_AWAKE_WS_RESUME_GRACE_MS);
    return true;
}

static bool arm_ws_reconnect_audio_recovery(nuwax_ws_client_t *ws,
                                            const char *reason) {
    if (!ws || !nuwax_ws_is_connected(ws) ||
        !nuwax_awake_session_state_is_active(&s_awake_session)) {
        clear_ws_reconnect_audio_recovery();
        return false;
    }
    const uint32_t ws_epoch = nuwax_ws_connection_epoch(ws);
    const uint32_t active_epoch = active_ws_epoch_snapshot();
    const uint32_t awake_generation =
        nuwax_awake_session_state_generation(&s_awake_session);
    if (ws_epoch == 0U || ws_epoch != active_epoch ||
        awake_generation == 0U) {
        clear_ws_reconnect_audio_recovery();
        ESP_LOGE(TAG,
                 "WebSocket reconnect audio recovery arm rejected reason=%s ws_epoch=%lu active_epoch=%lu awake_generation=%lu",
                 reason ? reason : "-",
                 (unsigned long)ws_epoch,
                 (unsigned long)active_epoch,
                 (unsigned long)awake_generation);
        return false;
    }
    s_ws_audio_recovery.pending = true;
    s_ws_audio_recovery.ws_epoch = ws_epoch;
    s_ws_audio_recovery.awake_generation = awake_generation;
    s_ws_audio_recovery.deadline_ms =
        monotonic_ms() + NUWAX_WS_RECONNECT_AUDIO_RECOVERY_TIMEOUT_MS;
    ESP_LOGI(TAG,
             "WebSocket reconnect audio recovery armed reason=%s epoch=%lu awake_generation=%lu deadline_ms=%lu",
             reason ? reason : "-",
             (unsigned long)ws_epoch,
             (unsigned long)awake_generation,
             (unsigned long)s_ws_audio_recovery.deadline_ms);
    return true;
}

static nuwax_ws_recovery_result_t
recover_awake_conversation_after_ws_reconnect(nuwax_ws_client_t *ws,
                                               const char *reason) {
#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
    const uint32_t now_ms = monotonic_ms();
    const uint32_t ws_epoch = ws ? nuwax_ws_connection_epoch(ws) : 0U;
    const uint32_t active_epoch = active_ws_epoch_snapshot();
    const uint32_t awake_generation =
        nuwax_awake_session_state_generation(&s_awake_session);
    if (!s_ws_audio_recovery.pending || !ws ||
        !nuwax_ws_is_connected(ws) || ws_epoch == 0U ||
        ws_epoch != active_epoch || ws_epoch != s_ws_audio_recovery.ws_epoch ||
        awake_generation != s_ws_audio_recovery.awake_generation ||
        !nuwax_awake_session_state_is_active(&s_awake_session) ||
        nuwax_awake_session_state_remaining_ms(&s_awake_session, now_ms) ==
            0U) {
        ESP_LOGE(TAG,
                 "WebSocket reconnect audio recovery context mismatch reason=%s pending=%d ws_epoch=%lu active_epoch=%lu bound_epoch=%lu awake_generation=%lu bound_generation=%lu awake_active=%d",
                 reason ? reason : "-",
                 s_ws_audio_recovery.pending ? 1 : 0,
                 (unsigned long)ws_epoch,
                 (unsigned long)active_epoch,
                 (unsigned long)s_ws_audio_recovery.ws_epoch,
                 (unsigned long)awake_generation,
                 (unsigned long)s_ws_audio_recovery.awake_generation,
                 nuwax_awake_session_state_is_active(&s_awake_session) ? 1 : 0);
        return NUWAX_WS_RECOVERY_FALLBACK_SLEEP;
    }
    if (exclusive_audio_owner_active()) {
        ESP_LOGI(TAG,
                 "WebSocket reconnect recovery transferred to exclusive audio owner reason=%s meeting_state=%d monitoring_state=%d",
                 reason ? reason : "-",
                 (int)nuwax_meeting_mode_state(&s_meeting_mode),
                 (int)nuwax_monitoring_mode_state(&s_monitoring_mode));
        clear_awake_session("ws_reconnect_handoff_to_exclusive");
        return NUWAX_WS_RECOVERY_TRANSFERRED_TO_OWNER;
    }
    if (exclusive_audio_mode_blocks_normal_audio()) {
        return NUWAX_WS_RECOVERY_WAIT_MODE_QUEUE;
    }
    if (nuwax_app_shell_blocks_auto_followup()) {
        clear_ws_reconnect_audio_recovery();
        set_state(NUWAX_APP_STATE_IDLE);
        ESP_LOGI(TAG,
                 "WebSocket reconnect recovery deferred by menu reason=%s remaining_ms=%lu",
                 reason ? reason : "-",
                 (unsigned long)nuwax_awake_session_state_remaining_ms(
                     &s_awake_session, now_ms));
        return NUWAX_WS_RECOVERY_MENU_BLOCKED;
    }

    /* A disconnect may race a speaker stop or capture cleanup. Never trust a
     * pending/capture flag that was owned by the old connection epoch. */
    nuwax_prelisten_state_clear(&s_prelisten);
    clear_last_listen_resume();
    s_post_playback_resume_pending = false;
    s_during_playback_handoff_pending = false;
    s_during_playback_handoff_playback_started = false;
    s_during_playback_handoff_seamless = false;

    const uint32_t window_ms = awake_session_listen_window_ms(now_ms);
    start_prelisten_window("awake_session", window_ms, 0U);
    if (!s_prelisten.pending && !s_prelisten.capture_active) {
        ESP_LOGE(TAG,
                 "WebSocket reconnect recovery failed to arm awake capture reason=%s",
                 reason ? reason : "-");
        return NUWAX_WS_RECOVERY_FALLBACK_SLEEP;
    }
    if (conversation_state_current() != NUWAX_APP_STATE_LISTENING) {
        ESP_LOGE(TAG,
                 "WebSocket reconnect recovery armed capture without LISTENING state reason=%s state=%s",
                 reason ? reason : "-",
                 nuwax_app_state_name(conversation_state_current()));
        return NUWAX_WS_RECOVERY_FALLBACK_SLEEP;
    }
    clear_ws_reconnect_audio_recovery();
    ESP_LOGI(TAG,
             "WebSocket reconnect recovery converged target=awake_prelisten reason=%s window_ms=%lu state=%s",
             reason ? reason : "-",
             (unsigned long)window_ms,
             nuwax_app_state_name(conversation_state_current()));
    return NUWAX_WS_RECOVERY_CONVERGED;
#else
    (void)ws;
    (void)reason;
    return NUWAX_WS_RECOVERY_FALLBACK_SLEEP;
#endif
}
static void candidate_reset(int scenario) {
    transport_closed=menu_open=mode_owner=mode_pending=ota_active=false;dynamic_epoch=1;
    pending_greeting_recovery=(wg_ticket_t){0};reset(scenario);
}
static void reconnect_on_new_epoch(void) { transport_closed=false;dynamic_epoch=2;now+=1000; }
int main(void) {
    int ws=0;
    assert(!nuwax_ws_close_policy_admits(NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY,true));
    assert(!nuwax_ws_close_policy_admits(NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED,true));
    assert(wg_candidate_close_admits(WG_CLOSE_UNRESOLVED_GREETING,true));
    puts("PASS generic close stays suppressed with healthy heartbeat; new scoped cause is required");
    for(int scenario=0;scenario<2;scenario++) {
        candidate_reset(scenario);
        assert(wait_for_mic_start_action(&ws)==ESP_ERR_INVALID_STATE);
        assert(close_requests==1&&transport_closed&&capture_starts==0);
        const uint32_t elapsed=now-100;
        assert(elapsed==((scenario==0)?15100U:31100U));
        assert(preserve_awake_session_for_ws_reconnect("candidate_epoch_rotation"));
        assert(s_awake_resume_after_ws_reconnect&&s_awake_session.active&&!s_awake_session.waiting);
        reconnect_on_new_epoch();
        assert(arm_ws_reconnect_audio_recovery(&ws,"candidate"));
        assert(recover_awake_conversation_after_ws_reconnect(&ws,"candidate")==NUWAX_WS_RECOVERY_CONVERGED);
        assert(s_prelisten.pending&&state==NUWAX_APP_STATE_LISTENING);
        printf("CANDIDATE scenario=%d real_wait_ms=%lu requests_one_epoch_rotation; original preserve+reconnect functions arm listening on epoch=2\n",scenario,(unsigned long)elapsed);
    }
    candidate_reset(0);assert(wait_for_mic_start_action(&ws)==ESP_ERR_INVALID_STATE);
    assert(preserve_awake_session_for_ws_reconnect("menu"));reconnect_on_new_epoch();menu_open=true;
    assert(arm_ws_reconnect_audio_recovery(&ws,"menu"));
    assert(recover_awake_conversation_after_ws_reconnect(&ws,"menu")==NUWAX_WS_RECOVERY_MENU_BLOCKED);
    assert(state==NUWAX_APP_STATE_IDLE&&!s_prelisten.pending);menu_open=false;maybe_enter_idle_sleep();
    assert(s_prelisten.pending);puts("PASS reconnect preserves open menu; closing menu resumes via original idle code");

    candidate_reset(0);begin_awake_session("policy");assert(begin_active_turn_guard("wake_99",NULL,15000,"policy"));
    assert(begin_conversation_visual_round(NUWAX_VISUAL_ROUND_WAKE_GREETING,"wake_99","policy"));
    wg_ticket_t t=wg_capture(&s_visual_round,&s_turn_guard,1,s_awake_session.generation,WG_FAILURE_TEXT_TIMEOUT);
    assert(t.valid);s_visual_round.first_pcm_seen=true;
    assert(!wg_capture(&s_visual_round,&s_turn_guard,1,s_awake_session.generation,WG_FAILURE_TEXT_TIMEOUT).valid);
    assert(wg_decide(&t,2,t.awake_generation,t.turn_epoch,false,true,true,false,false,false,false,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation+1,t.turn_epoch,false,true,true,false,false,false,false,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch+1,false,true,true,false,false,false,false,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,true,false,false,false,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,false,true,false,false,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,false,false,true,false,false,false)==WG_DEFER_OTA);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,false,false,false,true,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,false,false,false,false,true,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,false,false,false,false,false,true)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,false,true,false,false,false,false,false,false)==WG_YIELD_NEW_OWNER);
    puts("PASS ticket identity/first-PCM/active sleep/shutdown/OTA/reprovision/notification/exclusive/disconnect policy fences");
    assert(!nuwax_ws_close_requested_for_epoch(2,1));
    puts("PASS an old-epoch close does not affect the replacement connection");
    candidate_reset(0);assert(wait_for_mic_start_action(&ws)==ESP_ERR_INVALID_STATE);
    assert(preserve_awake_session_for_ws_reconnect("grace_failure"));
    now=s_awake_disconnect_ms+30001;
    /* Exact outer-owner condition at production lines 34314-34320.
       enter_sleeping stub models its state/wake intent, not hardware readiness. */
    if(s_awake_resume_after_ws_reconnect&&elapsed_ms(now,s_awake_disconnect_ms+NUWAX_AWAKE_WS_RESUME_GRACE_MS))
        enter_sleeping_conversation_state("ws_reconnect_grace_expired");
    assert(!s_awake_session.active&&state==NUWAX_APP_STATE_SLEEPING&&wake_rearms==1);
    puts("PASS reconnect grace expiry condition releases awake owner and requests wake-ready sleep (hardware wake startup not simulated)");
    return 0;
}
