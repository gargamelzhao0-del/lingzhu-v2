
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "conversation/awake_session_state.h"
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
static struct { bool pending, capture_active; } s_prelisten;
static int s_listen_resume, s_notification_transaction;
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
static bool exclusive_audio_mode_blocks_normal_audio(void) { return false; }
static bool nuwax_app_shell_blocks_auto_followup(void) { return false; }
static bool sleep_transaction_blocks_new_audio(void) { return false; }
static bool meeting_or_monitoring_mode_blocks_sleep(void) { return false; }
static bool ota_blocks_wake_detector(void) { return false; }
static bool nuwax_notification_blocks_conversation(void *p) { (void)p;return false; }
static unsigned uxQueueMessagesWaiting(void *p) { (void)p;return 0; }
static bool nuwax_speaker_playback_has_pending(void) { return false; }
static bool nuwax_listen_resume_state_holds_idle_expiry_boundary(void *p) { (void)p;return false; }
static void set_state(int next) { state = next; }
static bool nuwax_prelisten_state_mode_is(void *p,const char *m) { (void)p;(void)m;return false; }
static bool nuwax_capture_phase_cancel_recovery_needs_resume(bool a,bool b,bool c) { return a||b||c; }
static void reset_awake_wait_budget_paused(const char *r) { (void)r;nuwax_awake_session_state_reset_paused(&s_awake_session); }
static uint32_t awake_session_listen_window_ms(uint32_t t) { return nuwax_awake_session_state_remaining_ms(&s_awake_session,t); }
static void start_prelisten_window(const char *r,uint32_t w,uint32_t g) { (void)r;(void)w;(void)g;capture_starts++;s_prelisten.pending=true;state=NUWAX_APP_STATE_LISTENING; }
static void return_to_listening_after_cancel(const char *r) { start_prelisten_window(r,60000,0); }
static void enter_sleeping_conversation_state(const char *r) { (void)r;state=NUWAX_APP_STATE_SLEEPING;nuwax_awake_session_state_clear(&s_awake_session);wake_rearms++; }
static void request_sleep_wake_word_detector(const char *r) { (void)r;wake_rearms++; }
static bool recover_post_end_cloud_response_timeout(void *p,uint32_t t) { (void)p;(void)t;return false; }
static bool finalize_playback_terminal_if_ready(const char *r) { (void)r;return false; }
static bool begin_playback_terminal_local_recovery(const char *r) { (void)r;return false; }
static bool nuwax_speaker_playback_expire_metadata_only_session(const char *a,const char *b,uint32_t t) { (void)a;(void)b;(void)t;return false; }
static void clear_last_listen_resume(void) {}
static void wake_greeting_metrics_cancel(const char *r) { (void)r; }
static uint32_t nuwax_ws_connection_epoch(void *p) { (void)p;return 1; }
static uint32_t update_normal_downlink_admission(bool b) { (void)b;return 1; }
static int nuwax_session_send_cancel_turn_until_if_epoch(void *p,const char *a,const char *b,const char *c,int64_t t,uint32_t e) { (void)p;(void)a;(void)b;(void)c;(void)t;(void)e;return 0; }
static bool nuwax_ws_request_close_if_epoch(void *p,uint32_t e) { (void)p;(void)e;close_requests++;return true; }
static bool send_playback_fault_cancel(const char *r) { (void)r;return true; }
static bool set_active_turn_phase(int p,uint32_t t) { (void)p;(void)t;return true; }
static bool nuwax_speaker_playback_request_cancel_generation(uint32_t g,const char *r,uint32_t t) { (void)g;(void)r;(void)t;return true; }
static void clear_active_turn_guard(const char *r) { (void)r;cleared++;nuwax_turn_guard_clear(&s_turn_guard); }
static void nuwax_speaker_playback_request_fade_cancel(const char *r,uint32_t t) { (void)r;(void)t; }
static void set_conversation_status_guarded(const char *a,const char *b,const char *c,bool d) { (void)a;(void)b;(void)c;(void)d;status_updates++; }
static size_t strlcpy(char *d,const char *s,size_t n) { size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l; }
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
static void reproduce(int initial_state,uint32_t timeout,int phase) {
    memset(&s_prelisten,0,sizeof(s_prelisten));
    nuwax_awake_session_state_clear(&s_awake_session);
    nuwax_turn_guard_clear(&s_turn_guard);
    nuwax_playback_terminal_guard_clear(&s_playback_terminal_guard);
    state=initial_state;now=100;capture_starts=wake_rearms=cleared=status_updates=close_requests=0;
    nuwax_awake_session_state_begin(&s_awake_session,60000);
    assert(nuwax_turn_guard_begin(&s_turn_guard,s_awake_session.generation,"wake_1",NULL,now,timeout));
    s_turn_guard.phase=phase;
    now+=timeout;
    int ws=0;
    check_conversation_turn_timeout(&ws);
    assert(!s_turn_guard.active && cleared==1 && status_updates==1);
    for (unsigned i=0;i<100;++i) {
        now+=60000;
        maybe_enter_idle_sleep();
        ensure_sleep_wake_word_detector();
    }
    if(initial_state==NUWAX_APP_STATE_SLEEPING) {
        assert(state==NUWAX_APP_STATE_SLEEPING);
        assert(!capture_starts && !wake_rearms && !close_requests);
        assert(s_awake_session.active && !s_awake_session.waiting);
        assert(!s_prelisten.pending && !s_prelisten.capture_active);
        printf("REPRODUCED phase=%d timeout=%lu: after 100 idle minutes state=SLEEPING awake=active/paused capture=0 wake_rearm=0 websocket_close=0\n",phase,(unsigned long)timeout);
    } else {
        assert(state==NUWAX_APP_STATE_LISTENING && capture_starts>0);
        puts("CONTROL PASSED: the same timeout starting in an awake state schedules listening");
    }
}
int main(void) {
    reproduce(NUWAX_APP_STATE_SLEEPING,15000,NUWAX_TURN_PHASE_WAIT_TEXT);
    reproduce(NUWAX_APP_STATE_SLEEPING,30000,NUWAX_TURN_PHASE_WAIT_TTS);
    reproduce(NUWAX_APP_STATE_IDLE,15000,NUWAX_TURN_PHASE_WAIT_TEXT);
    return 0;
}
