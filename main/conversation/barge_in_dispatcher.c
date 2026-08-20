#include "barge_in_dispatcher.h"

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "session_protocol.h"

#define NUWAX_BARGE_DISPATCH_QUEUE_DEPTH 8U
#define NUWAX_BARGE_DISPATCH_TASK_STACK_BYTES 6144U
#define NUWAX_BARGE_DISPATCH_TASK_PRIORITY 8U
#define NUWAX_BARGE_DISPATCH_COMMIT_DEADLINE_US 1000000LL

typedef struct {
    uint32_t binding_generation;
    uint32_t ws_connection_epoch;
    char event_id[48];
    char parent_turn_id[48];
    char playback_id[48];
    char audio_session_id[48];
    char listen_mode[40];
    uint32_t onset_sequence;
    uint32_t confirmed_sequence;
    uint32_t confidence_q15;
    int64_t enqueued_us;
} barge_dispatch_item_t;

static const char *TAG = "barge_dispatch";
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static nuwax_ws_client_t *s_bound_ws;
static uint32_t s_bound_ws_epoch;
static uint32_t s_binding_generation;
static uint32_t s_status_binding_generation;
static char s_status_event_id[48];
static nuwax_barge_dispatch_status_t s_status;
static esp_err_t s_status_result = ESP_ERR_INVALID_STATE;

static uint32_t next_generation(uint32_t generation) {
    generation++;
    return generation == 0U ? 1U : generation;
}

static bool snapshot_binding(uint32_t expected_generation,
                             uint32_t expected_ws_epoch,
                             nuwax_ws_client_t **out_ws) {
    if (!s_lock || expected_ws_epoch == 0U || !out_ws) {
        return false;
    }
    bool current = false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        current = s_bound_ws &&
                  s_binding_generation == expected_generation &&
                  s_bound_ws_epoch == expected_ws_epoch &&
                  nuwax_ws_connection_epoch(s_bound_ws) == expected_ws_epoch;
        *out_ws = current ? s_bound_ws : NULL;
        xSemaphoreGive(s_lock);
    }
    return current;
}

static void set_transaction_status(uint32_t binding_generation,
                                   const char *event_id,
                                   nuwax_barge_dispatch_status_t status,
                                   esp_err_t result) {
    if (!s_lock || !event_id ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_status_binding_generation == binding_generation &&
        strcmp(s_status_event_id, event_id) == 0 &&
        (s_status == NUWAX_BARGE_DISPATCH_PENDING ||
         s_status == NUWAX_BARGE_DISPATCH_LISTEN_ATTEMPTED)) {
        s_status = status;
        s_status_result = result;
    }
    xSemaphoreGive(s_lock);
}

static bool mark_listen_attempted(uint32_t binding_generation,
                                  const char *event_id) {
    if (!s_lock || !event_id ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool can_send =
        s_status_binding_generation == binding_generation &&
        strcmp(s_status_event_id, event_id) == 0 &&
        s_status == NUWAX_BARGE_DISPATCH_PENDING;
    if (can_send) {
        s_status = NUWAX_BARGE_DISPATCH_LISTEN_ATTEMPTED;
        s_status_result = ESP_ERR_NOT_FINISHED;
    }
    xSemaphoreGive(s_lock);
    return can_send;
}

static bool transaction_deadline_open(int64_t deadline_us) {
    return esp_timer_get_time() < deadline_us;
}

static bool failed_binding_is_current(
    nuwax_ws_client_t *ws,
    uint32_t binding_generation,
    uint32_t ws_connection_epoch) {
    if (!ws || !s_lock ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    const bool current = s_bound_ws == ws &&
                         s_binding_generation == binding_generation &&
                         s_bound_ws_epoch == ws_connection_epoch;
    xSemaphoreGive(s_lock);
    return current;
}

static void barge_dispatch_task(void *arg) {
    (void)arg;
    barge_dispatch_item_t item = {0};
    while (true) {
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const int64_t dequeued_us = esp_timer_get_time();
        nuwax_ws_client_t *ws = NULL;
        if (!snapshot_binding(item.binding_generation,
                              item.ws_connection_epoch,
                              &ws) ||
            !nuwax_ws_is_connected(ws)) {
            ESP_LOGW(TAG,
                     "dropping stale barge transaction event=%s binding=%lu epoch=%lu",
                     item.event_id,
                     (unsigned long)item.binding_generation,
                     (unsigned long)item.ws_connection_epoch);
            set_transaction_status(item.binding_generation,
                                   item.event_id,
                                   NUWAX_BARGE_DISPATCH_FAILED,
                                   ESP_ERR_INVALID_STATE);
            continue;
        }
        nuwax_barge_dispatch_status_t queued_status =
            NUWAX_BARGE_DISPATCH_UNKNOWN;
        esp_err_t queued_result = ESP_ERR_INVALID_STATE;
        if (!nuwax_barge_dispatcher_get_status(
                item.binding_generation,
                item.event_id,
                &queued_status,
                &queued_result) ||
            queued_status != NUWAX_BARGE_DISPATCH_PENDING) {
            ESP_LOGW(TAG,
                     "dropping cancelled barge transaction event=%s binding=%lu status=%d",
                     item.event_id,
                     (unsigned long)item.binding_generation,
                     (int)queued_status);
            continue;
        }

        const int64_t send_started_us = esp_timer_get_time();
        const int64_t transaction_deadline_us =
            item.enqueued_us + NUWAX_BARGE_DISPATCH_COMMIT_DEADLINE_US;
        const esp_err_t candidate_err =
            transaction_deadline_open(transaction_deadline_us)
                ? nuwax_session_send_barge_in_until_if_epoch(
                      ws,
                      "candidate",
                      item.event_id,
                      item.parent_turn_id,
                      item.playback_id,
                      item.audio_session_id,
                      item.onset_sequence,
                      item.confirmed_sequence,
                      item.confidence_q15,
                      transaction_deadline_us,
                      item.ws_connection_epoch)
                : ESP_ERR_TIMEOUT;
        nuwax_barge_dispatch_status_t after_candidate_status =
            NUWAX_BARGE_DISPATCH_UNKNOWN;
        esp_err_t after_candidate_result = ESP_ERR_INVALID_STATE;
        const bool transaction_still_pending =
            nuwax_barge_dispatcher_get_status(
                item.binding_generation,
                item.event_id,
                &after_candidate_status,
                &after_candidate_result) &&
            after_candidate_status == NUWAX_BARGE_DISPATCH_PENDING;
        nuwax_ws_client_t *confirmed_ws = NULL;
        const bool binding_still_current =
            candidate_err == ESP_OK && transaction_still_pending &&
            snapshot_binding(item.binding_generation,
                             item.ws_connection_epoch,
                             &confirmed_ws) &&
            confirmed_ws == ws && nuwax_ws_is_connected(confirmed_ws);
        const esp_err_t confirmed_err = binding_still_current &&
                                                transaction_deadline_open(
                                                    transaction_deadline_us)
                                            ? nuwax_session_send_barge_in_until_if_epoch(
                                                  confirmed_ws,
                                                  "confirmed",
                                                  item.event_id,
                                                  item.parent_turn_id,
                                                  item.playback_id,
                                                  item.audio_session_id,
                                                  item.onset_sequence,
                                                  item.confirmed_sequence,
                                                  item.confidence_q15,
                                                  transaction_deadline_us,
                                                  item.ws_connection_epoch)
                                            : (candidate_err == ESP_OK
                                                   ? (transaction_deadline_open(
                                                          transaction_deadline_us)
                                                          ? ESP_ERR_INVALID_STATE
                                                          : ESP_ERR_TIMEOUT)
                                                   : candidate_err);
        nuwax_ws_client_t *listen_ws = NULL;
        const bool commit_deadline_ok =
            transaction_deadline_open(transaction_deadline_us);
        const bool confirmed_binding_still_current =
            confirmed_err == ESP_OK && commit_deadline_ok &&
            snapshot_binding(item.binding_generation,
                             item.ws_connection_epoch,
                             &listen_ws) &&
            listen_ws == ws && nuwax_ws_is_connected(listen_ws) &&
            mark_listen_attempted(item.binding_generation, item.event_id);
        const bool listen_attempted = confirmed_binding_still_current;
        const esp_err_t listen_err = listen_attempted
            ? nuwax_session_send_listen_with_audio_session_until_if_epoch(
                  listen_ws,
                  "start",
                  item.listen_mode[0] ? item.listen_mode
                                      : "during_playback",
                  item.audio_session_id,
                  item.event_id,
                  transaction_deadline_us,
                  item.ws_connection_epoch)
            : (confirmed_err == ESP_OK
                   ? (commit_deadline_ok ? ESP_ERR_INVALID_STATE
                                         : ESP_ERR_TIMEOUT)
                                       : confirmed_err);
        const int64_t completed_us = esp_timer_get_time();
        const bool delivered = candidate_err == ESP_OK &&
                               confirmed_err == ESP_OK &&
                               listen_err == ESP_OK;
        const nuwax_barge_dispatch_status_t final_status =
            delivered
                ? NUWAX_BARGE_DISPATCH_DELIVERED
                : (listen_attempted
                       ? NUWAX_BARGE_DISPATCH_COMMIT_UNCERTAIN
                       : NUWAX_BARGE_DISPATCH_FAILED);
        const esp_err_t final_result =
            delivered ? ESP_OK : (candidate_err != ESP_OK
                                      ? candidate_err
                                      : (confirmed_err != ESP_OK
                                             ? confirmed_err
                                             : listen_err));
        set_transaction_status(
            item.binding_generation,
            item.event_id,
            final_status,
            final_result);
        if (!delivered) {
            /* Keep the authenticated socket and fail only this transaction.
             * The capture owner retains the session identifiers and performs
             * best-effort cancel/stop cleanup before the next listen. */
            if (!failed_binding_is_current(
                    ws,
                    item.binding_generation,
                    item.ws_connection_epoch)) {
                ESP_LOGW(TAG,
                         "failed barge transaction belongs to a stale connection event=%s binding=%lu epoch=%lu",
                         item.event_id,
                         (unsigned long)item.binding_generation,
                         (unsigned long)item.ws_connection_epoch);
            }
        }
        if (delivered) {
            ESP_LOGI(TAG,
                     "barge transaction event=%s candidate=%s confirmed=%s listen=%s queue_wait_us=%lld dispatch_us=%lld total_us=%lld",
                     item.event_id,
                     esp_err_to_name(candidate_err),
                     esp_err_to_name(confirmed_err),
                     esp_err_to_name(listen_err),
                     (long long)(dequeued_us - item.enqueued_us),
                     (long long)(completed_us - send_started_us),
                     (long long)(completed_us - item.enqueued_us));
        } else if (listen_attempted) {
            ESP_LOGE(TAG,
                     "barge listen commit uncertain event=%s candidate=%s confirmed=%s listen=%s; caller must stop or close binding=%lu queue_wait_us=%lld dispatch_us=%lld total_us=%lld",
                     item.event_id,
                     esp_err_to_name(candidate_err),
                     esp_err_to_name(confirmed_err),
                     esp_err_to_name(listen_err),
                     (unsigned long)item.binding_generation,
                     (long long)(dequeued_us - item.enqueued_us),
                     (long long)(completed_us - send_started_us),
                     (long long)(completed_us - item.enqueued_us));
        } else {
            ESP_LOGE(TAG,
                     "barge transaction failed before listen attempt event=%s candidate=%s confirmed=%s listen=%s binding_current=%d queue_wait_us=%lld dispatch_us=%lld total_us=%lld",
                     item.event_id,
                     esp_err_to_name(candidate_err),
                     esp_err_to_name(confirmed_err),
                     esp_err_to_name(listen_err),
                     confirmed_binding_still_current ? 1 : 0,
                     (long long)(dequeued_us - item.enqueued_us),
                     (long long)(completed_us - send_started_us),
                     (long long)(completed_us - item.enqueued_us));
        }
    }
}

esp_err_t nuwax_barge_dispatcher_init(void) {
    if (s_queue && s_lock && s_task) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_queue) {
        s_queue = xQueueCreateWithCaps(NUWAX_BARGE_DISPATCH_QUEUE_DEPTH,
                                      sizeof(barge_dispatch_item_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_queue) {
            s_queue = xQueueCreate(NUWAX_BARGE_DISPATCH_QUEUE_DEPTH,
                                   sizeof(barge_dispatch_item_t));
        }
    }
    if (!s_lock || !s_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_task) {
        BaseType_t created = xTaskCreateWithCaps(
            barge_dispatch_task,
            "barge_dispatch",
            NUWAX_BARGE_DISPATCH_TASK_STACK_BYTES,
            NULL,
            NUWAX_BARGE_DISPATCH_TASK_PRIORITY,
            &s_task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created != pdPASS || !s_task) {
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

uint32_t nuwax_barge_dispatcher_bind(nuwax_ws_client_t *ws) {
    if (!ws || nuwax_barge_dispatcher_init() != ESP_OK) {
        return 0U;
    }
    const uint32_t ws_connection_epoch = nuwax_ws_connection_epoch(ws);
    if (ws_connection_epoch == 0U || !nuwax_ws_is_connected(ws) ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return 0U;
    }
    if (!nuwax_ws_is_connected(ws) ||
        nuwax_ws_connection_epoch(ws) != ws_connection_epoch) {
        xSemaphoreGive(s_lock);
        return 0U;
    }
    s_binding_generation = next_generation(s_binding_generation);
    s_bound_ws = ws;
    s_bound_ws_epoch = ws_connection_epoch;
    const uint32_t generation = s_binding_generation;
    xSemaphoreGive(s_lock);
    return generation;
}

uint32_t nuwax_barge_dispatcher_current_binding(nuwax_ws_client_t *ws) {
    if (!ws || !s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return 0U;
    }
    const uint32_t ws_connection_epoch = nuwax_ws_connection_epoch(ws);
    const uint32_t generation =
        s_bound_ws == ws && ws_connection_epoch != 0U &&
                s_bound_ws_epoch == ws_connection_epoch &&
                nuwax_ws_is_connected(ws)
            ? s_binding_generation
            : 0U;
    xSemaphoreGive(s_lock);
    return generation;
}

void nuwax_barge_dispatcher_unbind(nuwax_ws_client_t *ws,
                                   uint32_t binding_generation) {
    if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_bound_ws == ws && s_binding_generation == binding_generation) {
        s_bound_ws = NULL;
        s_bound_ws_epoch = 0U;
        s_binding_generation = next_generation(s_binding_generation);
    }
    xSemaphoreGive(s_lock);
}

esp_err_t nuwax_barge_dispatcher_enqueue_confirmed(
    uint32_t binding_generation,
    const nuwax_barge_confirmed_event_t *event) {
    if (!s_queue || binding_generation == 0U || !event ||
        !event->event_id || event->event_id[0] == '\0' ||
        !event->audio_session_id || event->audio_session_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    barge_dispatch_item_t item = {
        .binding_generation = binding_generation,
        .onset_sequence = event->onset_sequence,
        .confirmed_sequence = event->confirmed_sequence,
        .confidence_q15 = event->confidence_q15,
        .enqueued_us = esp_timer_get_time(),
    };
    strlcpy(item.event_id, event->event_id, sizeof(item.event_id));
    strlcpy(item.parent_turn_id,
            event->parent_turn_id ? event->parent_turn_id : "",
            sizeof(item.parent_turn_id));
    strlcpy(item.playback_id,
            event->playback_id ? event->playback_id : "",
            sizeof(item.playback_id));
    strlcpy(item.audio_session_id,
            event->audio_session_id ? event->audio_session_id : "",
            sizeof(item.audio_session_id));
    strlcpy(item.listen_mode,
            event->listen_mode ? event->listen_mode : "during_playback",
            sizeof(item.listen_mode));
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t current_ws_epoch =
        s_bound_ws ? nuwax_ws_connection_epoch(s_bound_ws) : 0U;
    if (!s_bound_ws || s_binding_generation != binding_generation ||
        s_bound_ws_epoch == 0U || s_bound_ws_epoch != current_ws_epoch ||
        !nuwax_ws_is_connected(s_bound_ws)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool transaction_in_flight =
        s_status_binding_generation == binding_generation &&
        (s_status == NUWAX_BARGE_DISPATCH_PENDING ||
         s_status == NUWAX_BARGE_DISPATCH_LISTEN_ATTEMPTED);
    if (transaction_in_flight) {
        const bool duplicate = strcmp(s_status_event_id, item.event_id) == 0;
        char active_event_id[sizeof(s_status_event_id)] = {0};
        strlcpy(active_event_id, s_status_event_id, sizeof(active_event_id));
        xSemaphoreGive(s_lock);
        if (duplicate) {
            ESP_LOGI(TAG,
                     "barge dispatch duplicate retained event=%s binding=%lu",
                     item.event_id,
                     (unsigned long)binding_generation);
            return ESP_OK;
        }
        ESP_LOGW(TAG,
                 "barge dispatch rejected overlapping transaction active=%s incoming=%s binding=%lu",
                 active_event_id,
                 item.event_id,
                 (unsigned long)binding_generation);
        return ESP_ERR_INVALID_STATE;
    }
    item.ws_connection_epoch = s_bound_ws_epoch;
    s_status_binding_generation = binding_generation;
    strlcpy(s_status_event_id, item.event_id,
            sizeof(s_status_event_id));
    s_status = NUWAX_BARGE_DISPATCH_PENDING;
    s_status_result = ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_lock);
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        ESP_LOGE(TAG,
                 "barge dispatch queue full event=%s depth=%u",
                 item.event_id,
                 (unsigned)uxQueueMessagesWaiting(s_queue));
        set_transaction_status(binding_generation,
                               item.event_id,
                               NUWAX_BARGE_DISPATCH_FAILED,
                               ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool nuwax_barge_dispatcher_get_status(
    uint32_t binding_generation,
    const char *event_id,
    nuwax_barge_dispatch_status_t *status,
    esp_err_t *result) {
    if (!s_lock || binding_generation == 0U || !event_id ||
        event_id[0] == '\0' || !status || !result ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool matched =
        s_status_binding_generation == binding_generation &&
        strcmp(s_status_event_id, event_id) == 0;
    if (matched) {
        *status = s_status;
        *result = s_status_result;
    }
    xSemaphoreGive(s_lock);
    return matched;
}

bool nuwax_barge_dispatcher_cancel(uint32_t binding_generation,
                                   const char *event_id) {
    if (!s_lock || binding_generation == 0U || !event_id ||
        event_id[0] == '\0' ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool cancelled =
        s_status_binding_generation == binding_generation &&
        strcmp(s_status_event_id, event_id) == 0 &&
        s_status == NUWAX_BARGE_DISPATCH_PENDING;
    if (cancelled) {
        s_status = NUWAX_BARGE_DISPATCH_FAILED;
        s_status_result = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_lock);
    return cancelled;
}
