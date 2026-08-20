#include "conversation/audio_terminal_recovery.h"

#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

#include "conversation/audio_uplink.h"
#include "conversation/audio_terminal_recovery_policy.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "session_protocol.h"

static const char *TAG = "nuwax_terminal_recovery";

typedef struct {
    bool end_committed;
    uint32_t ws_epoch;
    nuwax_ws_client_t *ws;
    uint32_t attempt_timeout_ms;
    uint32_t failures;
    int64_t armed_us;
    int64_t deadline_us;
    int64_t next_attempt_us;
    char mode[NUWAX_AUDIO_TERMINAL_RECOVERY_MODE_MAX];
    char audio_session_id[NUWAX_AUDIO_TERMINAL_RECOVERY_SESSION_MAX];
    char request_id[NUWAX_AUDIO_TERMINAL_RECOVERY_REQUEST_MAX];
} recovery_state_t;

static recovery_state_t s_recovery;
static EXT_RAM_BSS_ATTR uint8_t s_recovery_packet[
    NUWAX_AUDIO_UPLINK_PACKET_BYTES];
static size_t s_recovery_packet_len;
typedef enum {
    RECOVERY_PHASE_IDLE = 0,
    RECOVERY_PHASE_ARMING,
    RECOVERY_PHASE_PENDING,
    RECOVERY_PHASE_SNAPSHOTTING,
    RECOVERY_PHASE_SERVICING,
} recovery_phase_t;
static _Atomic int s_recovery_phase;
static _Atomic bool s_completion_ready;
static nuwax_audio_terminal_recovery_completion_t s_completion;

static void copy_text(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0U) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, dst_size, "%s", src);
}

/* The caller exclusively owns RECOVERY_PHASE_ARMING or SERVICING. */
static uint32_t recovery_age_ms(int64_t now_us) {
    if (s_recovery.armed_us <= 0 || now_us <= s_recovery.armed_us) {
        return 0U;
    }
    const uint64_t age_ms =
        (uint64_t)(now_us - s_recovery.armed_us) / 1000ULL;
    return age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
}

static void clear_recovery_exclusive(bool success, bool quarantined) {
    if (s_recovery.ws && s_recovery.ws_epoch != 0U) {
        if (!atomic_load_explicit(&s_completion_ready, memory_order_acquire)) {
            const int64_t now_us = esp_timer_get_time();
            s_completion.valid = true;
            s_completion.success = success;
            s_completion.quarantined = quarantined;
            s_completion.ws_epoch = s_recovery.ws_epoch;
            s_completion.failures = s_recovery.failures;
            s_completion.age_ms = recovery_age_ms(now_us);
            copy_text(s_completion.mode,
                      sizeof(s_completion.mode),
                      s_recovery.mode);
            copy_text(s_completion.audio_session_id,
                      sizeof(s_completion.audio_session_id),
                      s_recovery.audio_session_id);
            atomic_store_explicit(&s_completion_ready,
                                  true,
                                  memory_order_release);
        }
    }
    memset(&s_recovery, 0, sizeof(s_recovery));
    s_recovery_packet_len = 0U;
    atomic_store_explicit(&s_recovery_phase,
                          RECOVERY_PHASE_IDLE,
                          memory_order_release);
}

static void schedule_retry(void) {
    const uint32_t delay_ms =
        nuwax_audio_terminal_recovery_backoff_ms(s_recovery.failures);
    if (s_recovery.failures < UINT32_MAX) {
        s_recovery.failures++;
    }
    s_recovery.next_attempt_us =
        esp_timer_get_time() + (int64_t)delay_ms * 1000LL;
}

esp_err_t nuwax_audio_terminal_recovery_arm(
    nuwax_ws_client_t *ws,
    uint32_t ws_epoch,
    const uint8_t *packet,
    size_t packet_len,
    bool end_committed,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    uint32_t attempt_timeout_ms) {
    if (!ws || ws_epoch == 0U || !mode || mode[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const char *session_id = audio_session_id ? audio_session_id : "";
    if (!end_committed &&
        (!packet || packet_len == 0U ||
         packet_len > sizeof(s_recovery_packet))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nuwax_ws_connection_epoch(ws) != ws_epoch ||
        !nuwax_ws_is_connected(ws)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (atomic_load_explicit(&s_completion_ready, memory_order_acquire)) {
        return ESP_ERR_NOT_FINISHED;
    }

    int expected_phase = RECOVERY_PHASE_IDLE;
    if (!atomic_compare_exchange_strong_explicit(
            &s_recovery_phase,
            &expected_phase,
            RECOVERY_PHASE_ARMING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        if (expected_phase != RECOVERY_PHASE_PENDING) {
            return ESP_ERR_NOT_FINISHED;
        }
        expected_phase = RECOVERY_PHASE_PENDING;
        if (!atomic_compare_exchange_strong_explicit(
                &s_recovery_phase,
                &expected_phase,
                RECOVERY_PHASE_ARMING,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return ESP_ERR_NOT_FINISHED;
        }
        if (s_recovery.ws != ws || s_recovery.ws_epoch != ws_epoch ||
            strcmp(s_recovery.mode, mode) != 0 ||
            strcmp(s_recovery.audio_session_id, session_id) != 0) {
            atomic_store_explicit(&s_recovery_phase,
                                  RECOVERY_PHASE_PENDING,
                                  memory_order_release);
            ESP_LOGE(TAG,
                     "refusing to replace pending terminal transaction new_epoch=%lu new_session=%s",
                     (unsigned long)ws_epoch,
                     session_id[0] ? session_id : "-");
            return ESP_ERR_INVALID_STATE;
        } else if (end_committed && !s_recovery.end_committed) {
            /* A stop-only failure may arrive after an END retry was armed.
             * Upgrade the transaction monotonically; it must never move back
             * to binary END. */
            s_recovery.end_committed = true;
            s_recovery.failures = 0U;
            s_recovery.next_attempt_us = 0;
        }
        atomic_store_explicit(&s_recovery_phase,
                              RECOVERY_PHASE_PENDING,
                              memory_order_release);
        return ESP_OK;
    }

    memset(&s_recovery, 0, sizeof(s_recovery));
    s_recovery.ws = ws;
    s_recovery.ws_epoch = ws_epoch;
    s_recovery.end_committed = end_committed;
    s_recovery.attempt_timeout_ms =
        attempt_timeout_ms == 0U
            ? NUWAX_AUDIO_TERMINAL_RECOVERY_DEFAULT_ATTEMPT_TIMEOUT_MS
            : attempt_timeout_ms;
    s_recovery.armed_us = esp_timer_get_time();
    s_recovery.deadline_us =
        s_recovery.armed_us +
        (int64_t)NUWAX_AUDIO_TERMINAL_RECOVERY_MAX_AGE_MS * 1000LL;
    copy_text(s_recovery.mode, sizeof(s_recovery.mode), mode);
    copy_text(s_recovery.audio_session_id,
              sizeof(s_recovery.audio_session_id),
              session_id);
    copy_text(s_recovery.request_id,
              sizeof(s_recovery.request_id),
              request_id);
    if (!end_committed) {
        memcpy(s_recovery_packet, packet, packet_len);
        s_recovery_packet_len = packet_len;
    }
    /* The ARMING phase prevents readers while the PSRAM packet is copied. */
    atomic_store_explicit(&s_recovery_phase,
                          RECOVERY_PHASE_PENDING,
                          memory_order_release);
    ESP_LOGW(TAG,
             "terminal recovery armed mode=%s session=%s epoch=%lu stage=%s packet_bytes=%u",
             mode,
             session_id[0] ? session_id : "-",
             (unsigned long)ws_epoch,
             end_committed ? "listen_stop" : "binary_end",
             (unsigned)(end_committed ? 0U : packet_len));
    return ESP_OK;
}

esp_err_t nuwax_audio_terminal_recovery_service(nuwax_ws_client_t *ws) {
    if (atomic_load_explicit(&s_recovery_phase, memory_order_acquire) ==
        RECOVERY_PHASE_IDLE) {
        return ESP_OK;
    }
    int expected_phase = RECOVERY_PHASE_PENDING;
    if (!atomic_compare_exchange_strong_explicit(
            &s_recovery_phase,
            &expected_phase,
            RECOVERY_PHASE_SERVICING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return expected_phase == RECOVERY_PHASE_IDLE
                   ? ESP_OK
                   : ESP_ERR_NOT_FINISHED;
    }
    if (!ws || s_recovery.ws != ws ||
        nuwax_ws_connection_epoch(ws) != s_recovery.ws_epoch ||
        !nuwax_ws_is_connected(ws)) {
        ESP_LOGI(TAG,
                 "terminal recovery abandoned after connection epoch changed old_epoch=%lu current_epoch=%lu",
                 (unsigned long)s_recovery.ws_epoch,
                 (unsigned long)(ws ? nuwax_ws_connection_epoch(ws) : 0U));
        clear_recovery_exclusive(false, false);
        return ESP_OK;
    }
    const int64_t now_us = esp_timer_get_time();
    const uint32_t age_ms = recovery_age_ms(now_us);
    if (nuwax_audio_terminal_recovery_should_quarantine(
            s_recovery.failures, age_ms) ||
        s_recovery.deadline_us <= now_us) {
        const bool close_admitted = nuwax_ws_close_if_epoch(
            ws,
            s_recovery.ws_epoch,
            NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY);
        ESP_LOGE(TAG,
                 "terminal recovery exhausted; transport-integrity close requested mode=%s session=%s epoch=%lu stage=%s failures=%lu age_ms=%lu close_admitted=%d",
                 s_recovery.mode,
                 s_recovery.audio_session_id[0]
                     ? s_recovery.audio_session_id
                     : "-",
                 (unsigned long)s_recovery.ws_epoch,
                 s_recovery.end_committed ? "listen_stop" : "binary_end",
                 (unsigned long)s_recovery.failures,
                 (unsigned long)age_ms,
                 close_admitted ? 1 : 0);
        clear_recovery_exclusive(false, true);
        return ESP_OK;
    }
    if (nuwax_audio_uplink_sender_busy()) {
        /* The original sender still owns the exact terminal packet and may be
         * inside the WebSocket send. Waiting prevents a duplicate END, but the
         * total recovery deadline still bounds this ownership wait. */
        atomic_store_explicit(&s_recovery_phase,
                              RECOVERY_PHASE_PENDING,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }
    if (s_recovery.next_attempt_us > now_us) {
        atomic_store_explicit(&s_recovery_phase,
                              RECOVERY_PHASE_PENDING,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }

    const nuwax_audio_terminal_recovery_action_t action =
        nuwax_audio_terminal_recovery_action(
            true,
            true,
            true,
            true,
            s_recovery.end_committed);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (action == NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_RETRY_END) {
        err = nuwax_ws_send_binary_with_timeout_if_epoch(
            ws,
            s_recovery_packet,
            s_recovery_packet_len,
            s_recovery.attempt_timeout_ms,
            s_recovery.ws_epoch);
        if (err == ESP_OK) {
            s_recovery.end_committed = true;
            s_recovery.failures = 0U;
            s_recovery.next_attempt_us = now_us + 1000LL;
            ESP_LOGI(TAG,
                     "terminal recovery binary END committed epoch=%lu session=%s",
                     (unsigned long)s_recovery.ws_epoch,
                     s_recovery.audio_session_id);
            atomic_store_explicit(&s_recovery_phase,
                                  RECOVERY_PHASE_PENDING,
                                  memory_order_release);
            return ESP_ERR_NOT_FINISHED;
        }
    } else if (action == NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_RETRY_STOP) {
        err = nuwax_session_send_listen_with_audio_session_until_if_epoch(
            ws,
            "stop",
            s_recovery.mode,
            s_recovery.audio_session_id,
            s_recovery.request_id[0] ? s_recovery.request_id : NULL,
            now_us + (int64_t)s_recovery.attempt_timeout_ms * 1000LL,
            s_recovery.ws_epoch);
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "terminal recovery listen stop committed mode=%s session=%s epoch=%lu",
                     s_recovery.mode,
                     s_recovery.audio_session_id,
                     (unsigned long)s_recovery.ws_epoch);
            clear_recovery_exclusive(true, false);
            return ESP_OK;
        }
    }

    if (!nuwax_ws_is_connected(ws) ||
        nuwax_ws_connection_epoch(ws) != s_recovery.ws_epoch) {
        clear_recovery_exclusive(false, false);
        return ESP_OK;
    }
    schedule_retry();
    ESP_LOGW(TAG,
             "terminal recovery attempt failed stage=%s err=%s epoch=%lu next_backoff_ms=%lu",
                 s_recovery.end_committed ? "listen_stop" : "binary_end",
             esp_err_to_name(err),
             (unsigned long)s_recovery.ws_epoch,
             (unsigned long)nuwax_audio_terminal_recovery_backoff_ms(
                 s_recovery.failures > 0U ? s_recovery.failures - 1U : 0U));
    atomic_store_explicit(&s_recovery_phase,
                          RECOVERY_PHASE_PENDING,
                          memory_order_release);
    return ESP_ERR_NOT_FINISHED;
}

bool nuwax_audio_terminal_recovery_pending(void) {
    return atomic_load_explicit(&s_recovery_phase, memory_order_acquire) !=
               RECOVERY_PHASE_IDLE ||
           atomic_load_explicit(&s_completion_ready, memory_order_acquire);
}

uint32_t nuwax_audio_terminal_recovery_next_service_ms(
    uint32_t max_wait_ms) {
    if (max_wait_ms == 0U ||
        atomic_load_explicit(&s_completion_ready, memory_order_acquire)) {
        return 0U;
    }
    int expected_phase = RECOVERY_PHASE_PENDING;
    if (!atomic_compare_exchange_strong_explicit(
            &s_recovery_phase,
            &expected_phase,
            RECOVERY_PHASE_SNAPSHOTTING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        if (atomic_load_explicit(&s_completion_ready,
                                 memory_order_acquire)) {
            return 0U;
        }
        if (expected_phase == RECOVERY_PHASE_IDLE) {
            return max_wait_ms;
        }
        return 1U;
    }
    const int64_t now_us = esp_timer_get_time();
    const int64_t next_attempt_us = s_recovery.next_attempt_us;
    const int64_t deadline_us = s_recovery.deadline_us;
    atomic_store_explicit(&s_recovery_phase,
                          RECOVERY_PHASE_PENDING,
                          memory_order_release);
    return nuwax_audio_terminal_recovery_wait_ms(
        next_attempt_us,
        deadline_us,
        now_us,
        max_wait_ms);
}

bool nuwax_audio_terminal_recovery_complete_if_matches(
    nuwax_ws_client_t *ws,
    uint32_t ws_epoch,
    const char *mode,
    const char *audio_session_id) {
    if (!ws || ws_epoch == 0U || !mode) {
        return false;
    }
    int expected_phase = RECOVERY_PHASE_PENDING;
    for (unsigned attempt = 0U; attempt < 8U; ++attempt) {
        if (atomic_compare_exchange_strong_explicit(
                &s_recovery_phase,
                &expected_phase,
                RECOVERY_PHASE_ARMING,
                memory_order_acq_rel,
                memory_order_acquire)) {
            break;
        }
        if (expected_phase != RECOVERY_PHASE_SNAPSHOTTING &&
            expected_phase != RECOVERY_PHASE_SERVICING) {
            return false;
        }
        taskYIELD();
        expected_phase = RECOVERY_PHASE_PENDING;
        if (attempt == 7U) {
            return false;
        }
    }
    const char *session_id = audio_session_id ? audio_session_id : "";
    if (s_recovery.ws != ws || s_recovery.ws_epoch != ws_epoch ||
        strcmp(s_recovery.mode, mode) != 0 ||
        strcmp(s_recovery.audio_session_id, session_id) != 0) {
        atomic_store_explicit(&s_recovery_phase,
                              RECOVERY_PHASE_PENDING,
                              memory_order_release);
        return false;
    }
    ESP_LOGI(TAG,
             "late terminal sender completed armed transaction mode=%s session=%s epoch=%lu",
             mode,
             session_id[0] ? session_id : "-",
             (unsigned long)ws_epoch);
    clear_recovery_exclusive(true, false);
    return true;
}

bool nuwax_audio_terminal_recovery_take_completion(
    nuwax_audio_terminal_recovery_completion_t *out) {
    if (!out) {
        return false;
    }
    if (!atomic_load_explicit(&s_completion_ready, memory_order_acquire)) {
        return false;
    }
    *out = s_completion;
    memset(&s_completion, 0, sizeof(s_completion));
    atomic_store_explicit(&s_completion_ready,
                          false,
                          memory_order_release);
    return true;
}
