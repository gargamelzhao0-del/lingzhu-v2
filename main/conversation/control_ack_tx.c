#include "control_ack_tx.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#define ACK_QUEUE_DEPTH 8U
#define ACK_MESSAGE_BYTES 256U
#define ACK_DEADLINE_US 500000LL

typedef struct {
    nuwax_ws_client_t *ws;
    uint32_t epoch;
    int64_t deadline_us;
    bool ok;
    char request_id[sizeof(((nuwax_session_command_t *)0)->request_id)];
    char command[sizeof(((nuwax_session_command_t *)0)->command)];
    char message[ACK_MESSAGE_BYTES];
} control_ack_job_t;

static const char *TAG = "control_ack_tx";
static QueueHandle_t s_queue;
static StaticQueue_t s_queue_control;
static uint8_t *s_queue_storage;
static TaskHandle_t s_task;

static void control_ack_task(void *arg) {
    (void)arg;
    control_ack_job_t job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        if (esp_timer_get_time() >= job.deadline_us ||
            nuwax_ws_connection_epoch(job.ws) != job.epoch) {
            /* The dispatch ledger retains the result for a cloud retry. */
            continue;
        }
        esp_err_t err = nuwax_session_send_command_ack_until_if_epoch(
            job.ws, job.request_id, job.command, job.ok, job.message,
            job.deadline_us, job.epoch);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "deferred command ACK failed request=%s epoch=%lu err=%s",
                     job.request_id, (unsigned long)job.epoch, esp_err_to_name(err));
        }
    }
}

esp_err_t nuwax_control_ack_tx_init(void) {
    if (s_task) return ESP_OK;
    s_queue_storage = heap_caps_malloc(ACK_QUEUE_DEPTH * sizeof(control_ack_job_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_queue_storage) return ESP_ERR_NO_MEM;
    s_queue = xQueueCreateStatic(ACK_QUEUE_DEPTH, sizeof(control_ack_job_t),
                                s_queue_storage, &s_queue_control);
    if (!s_queue || xTaskCreateWithCaps(control_ack_task, "control_ack", 8192,
                                      NULL, 4, &s_task,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        if (s_queue) vQueueDelete(s_queue);
        heap_caps_free(s_queue_storage);
        s_queue_storage = NULL;
        s_queue = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t nuwax_control_ack_tx_enqueue(nuwax_ws_client_t *ws,
                                     const nuwax_session_command_t *command,
                                     bool ok, const char *message,
                                     uint32_t epoch) {
    if (!ws || !command || epoch == 0U) return ESP_ERR_INVALID_ARG;
    if (!s_task || !s_queue) return ESP_ERR_INVALID_STATE;
    control_ack_job_t job = {
        .ws = ws, .epoch = epoch, .ok = ok,
        .deadline_us = esp_timer_get_time() + ACK_DEADLINE_US,
    };
    const size_t message_len = message ? strlen(message) : 0U;
    if (message_len >= sizeof(job.message)) return ESP_ERR_INVALID_SIZE;
    memcpy(job.request_id, command->request_id, sizeof(job.request_id));
    memcpy(job.command, command->command, sizeof(job.command));
    job.request_id[sizeof(job.request_id) - 1U] = '\0';
    job.command[sizeof(job.command) - 1U] = '\0';
    if (message_len) memcpy(job.message, message, message_len);
    return xQueueSend(s_queue, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
