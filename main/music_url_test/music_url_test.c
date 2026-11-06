#include "music_url_test.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "conversation/speaker_playback.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "nuwax_music_test";

#define NUWAX_MUSIC_URL_TEST_SAMPLE_RATE 16000U
#define NUWAX_MUSIC_URL_TEST_CHANNELS 1U
#define NUWAX_MUSIC_URL_TEST_READ_BYTES \
    (CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME * sizeof(int16_t))
#define NUWAX_MUSIC_URL_TEST_WRITE_TIMEOUT_MS 2000U
#define NUWAX_MUSIC_URL_TEST_COMPLETION_GRACE_MS 15000U
#define NUWAX_MUSIC_URL_TEST_TASK_STACK_BYTES 8192U
#define NUWAX_MUSIC_URL_TEST_TASK_PRIORITY 4U

typedef enum {
    MUSIC_TEST_IDLE = 0,
    MUSIC_TEST_RUNNING,
    MUSIC_TEST_TERMINAL,
} music_test_phase_t;

static atomic_int s_phase = ATOMIC_VAR_INIT(MUSIC_TEST_IDLE);
static atomic_bool s_stop_requested = ATOMIC_VAR_INIT(false);
static atomic_uint_fast32_t s_generation = ATOMIC_VAR_INIT(0U);
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_attempted;
static bool s_cloud_ready_seen;
static uint32_t s_cloud_ready_since_ms;
static esp_err_t s_terminal_result = ESP_ERR_INVALID_STATE;

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t monotonic_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static esp_err_t fetch_and_play(void) {
    esp_http_client_config_t config = {
        .url = CONFIG_NUWAX_MUSIC_URL_TEST_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_NUWAX_MUSIC_URL_TEST_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_http_client_open(client, 0);
    bool opened = result == ESP_OK;
    uint8_t *buffer = NULL;
    uint32_t generation = 0U;
    uint32_t sequence = 0U;
    uint32_t received = 0U;
    size_t carry_bytes = 0U;

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed url=%s err=%s errno=%d",
                 CONFIG_NUWAX_MUSIC_URL_TEST_URL,
                 esp_err_to_name(result), errno);
        goto cleanup;
    }

    const int64_t content_length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (content_length <= 0 || content_length > UINT32_MAX ||
        content_length > CONFIG_NUWAX_MUSIC_URL_TEST_MAX_BYTES ||
        (content_length & 1LL) != 0LL || status != 200) {
        ESP_LOGE(TAG,
                 "HTTP source rejected status=%d bytes=%lld max=%u",
                 status,
                 (long long)content_length,
                 (unsigned)CONFIG_NUWAX_MUSIC_URL_TEST_MAX_BYTES);
        result = status == 200 ? ESP_ERR_INVALID_SIZE
                               : ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    buffer = heap_caps_malloc(NUWAX_MUSIC_URL_TEST_READ_BYTES,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "source accepted bytes=%lu duration_ms=%lu format=pcm_s16le_16000_mono",
             (unsigned long)content_length,
             (unsigned long)((uint64_t)content_length * 1000ULL /
                             (NUWAX_MUSIC_URL_TEST_SAMPLE_RATE * 2ULL)));

    while (received < (uint32_t)content_length) {
        if (atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
            result = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        const uint32_t remaining = (uint32_t)content_length - received;
        const size_t read_capacity =
            NUWAX_MUSIC_URL_TEST_READ_BYTES - carry_bytes;
        const int requested = remaining < read_capacity
                                  ? (int)remaining
                                  : (int)read_capacity;
        const int count = esp_http_client_read(client,
                                               (char *)buffer + carry_bytes,
                                               requested);
        if (count == -ESP_ERR_HTTP_EAGAIN) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (count <= 0) {
            result = count == 0 ? ESP_ERR_INVALID_SIZE
                                : ESP_ERR_INVALID_RESPONSE;
            ESP_LOGE(TAG,
                     "HTTP PCM read failed received=%lu expected=%lu count=%d err=%s",
                     (unsigned long)received,
                     (unsigned long)content_length,
                     count,
                     esp_err_to_name(result));
            goto cleanup;
        }
        if (atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
            result = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }

        const size_t buffered = carry_bytes + (size_t)count;
        const size_t pcm_bytes = buffered & ~(size_t)1U;
        const bool final_pcm =
            received + (uint32_t)count == (uint32_t)content_length &&
            buffered == pcm_bytes;
        if (pcm_bytes > 0U) {
            const size_t samples = pcm_bytes / sizeof(int16_t);
            if (sequence == 0U) {
                result = nuwax_speaker_playback_url_test_begin(
                    (uint32_t)content_length,
                    final_pcm,
                    (const int16_t *)buffer,
                    samples,
                    &generation);
                if (result == ESP_OK) {
                    atomic_store_explicit(&s_generation,
                                          generation,
                                          memory_order_release);
                }
            } else {
                result = nuwax_speaker_playback_url_test_write(
                    generation,
                    sequence,
                    final_pcm,
                    (const int16_t *)buffer,
                    samples,
                    NUWAX_MUSIC_URL_TEST_WRITE_TIMEOUT_MS);
            }
            if (result != ESP_OK) {
                ESP_LOGW(TAG,
                         "PCM ingress stopped seq=%lu generation=%lu err=%s",
                         (unsigned long)sequence,
                         (unsigned long)generation,
                         esp_err_to_name(result));
                goto cleanup;
            }
            sequence++;
        }
        carry_bytes = buffered - pcm_bytes;
        if (carry_bytes != 0U) {
            buffer[0] = buffer[pcm_bytes];
        }
        received += (uint32_t)count;
    }

    if (received != (uint32_t)content_length || carry_bytes != 0U ||
        generation == 0U) {
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    /* END on the final PCM block has only entered the shared queue. Keep the
     * test admission active
     * until physical drain releases the speaker owner. */
    {
        const uint32_t completion_deadline =
            monotonic_ms() + NUWAX_MUSIC_URL_TEST_COMPLETION_GRACE_MS;
        while (nuwax_speaker_playback_url_test_active(generation)) {
            if (atomic_load_explicit(&s_stop_requested,
                                     memory_order_acquire)) {
                result = ESP_ERR_INVALID_STATE;
                goto cleanup;
            }
            if (deadline_reached(monotonic_ms(), completion_deadline)) {
                result = ESP_ERR_TIMEOUT;
                goto cleanup;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    result = ESP_OK;

cleanup:
    if (generation != 0U &&
        nuwax_speaker_playback_url_test_active(generation)) {
        (void)nuwax_speaker_playback_url_test_stop(
            generation,
            result == ESP_OK ? "url_test_cleanup"
                             : "url_test_failed_or_preempted");
    }
    free(buffer);
    if (opened) {
        (void)esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return result;
}

static void music_url_test_task(void *arg) {
    (void)arg;
    const esp_err_t result = fetch_and_play();
    portENTER_CRITICAL(&s_state_lock);
    s_terminal_result = result;
    portEXIT_CRITICAL(&s_state_lock);
    atomic_store_explicit(&s_generation, 0U, memory_order_release);
    atomic_store_explicit(&s_phase, MUSIC_TEST_TERMINAL,
                          memory_order_release);
    ESP_LOGI(TAG, "one-shot test terminal result=%s",
             esp_err_to_name(result));
    vTaskDeleteWithCaps(NULL);
}

esp_err_t nuwax_music_url_test_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }
    if (CONFIG_NUWAX_MUSIC_URL_TEST_URL[0] == '\0') {
        ESP_LOGE(TAG, "enabled without a URL");
        return ESP_ERR_INVALID_ARG;
    }
    s_initialized = true;
    s_attempted = false;
    s_cloud_ready_seen = false;
    s_cloud_ready_since_ms = 0U;
    s_terminal_result = ESP_ERR_INVALID_STATE;
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    atomic_store_explicit(&s_generation, 0U, memory_order_release);
    atomic_store_explicit(&s_phase, MUSIC_TEST_IDLE, memory_order_release);
    ESP_LOGW(TAG,
             "development URL playback test enabled url=%s delay_ms=%u max_bytes=%u",
             CONFIG_NUWAX_MUSIC_URL_TEST_URL,
             (unsigned)CONFIG_NUWAX_MUSIC_URL_TEST_STABLE_DELAY_MS,
             (unsigned)CONFIG_NUWAX_MUSIC_URL_TEST_MAX_BYTES);
    return ESP_OK;
}

void nuwax_music_url_test_observe_cloud_ready(bool ready, uint32_t now_ms) {
    if (!s_initialized || s_attempted) {
        return;
    }
    if (!ready) {
        s_cloud_ready_seen = false;
        s_cloud_ready_since_ms = 0U;
        return;
    }
    if (!s_cloud_ready_seen) {
        s_cloud_ready_seen = true;
        s_cloud_ready_since_ms = now_ms;
        ESP_LOGI(TAG, "cloud stable-delay timer armed delay_ms=%u",
                 (unsigned)CONFIG_NUWAX_MUSIC_URL_TEST_STABLE_DELAY_MS);
    }
}

bool nuwax_music_url_test_delay_elapsed(uint32_t now_ms) {
    return s_initialized && !s_attempted && s_cloud_ready_seen &&
           deadline_reached(
               now_ms,
               s_cloud_ready_since_ms +
                   (uint32_t)CONFIG_NUWAX_MUSIC_URL_TEST_STABLE_DELAY_MS);
}

esp_err_t nuwax_music_url_test_start(void) {
    if (!s_initialized || s_attempted ||
        atomic_load_explicit(&s_phase, memory_order_acquire) !=
            MUSIC_TEST_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_attempted = true;
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    atomic_store_explicit(&s_phase, MUSIC_TEST_RUNNING, memory_order_release);
    TaskHandle_t task = NULL;
    const BaseType_t created = xTaskCreateWithCaps(
        music_url_test_task,
        "music_url_test",
        NUWAX_MUSIC_URL_TEST_TASK_STACK_BYTES,
        NULL,
        NUWAX_MUSIC_URL_TEST_TASK_PRIORITY,
        &task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS || !task) {
        atomic_store_explicit(&s_phase, MUSIC_TEST_TERMINAL,
                              memory_order_release);
        portENTER_CRITICAL(&s_state_lock);
        s_terminal_result = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void nuwax_music_url_test_request_stop(const char *reason) {
    if (!nuwax_music_url_test_active()) {
        return;
    }
    atomic_store_explicit(&s_stop_requested, true, memory_order_release);
    const uint32_t generation = (uint32_t)atomic_load_explicit(
        &s_generation, memory_order_acquire);
    if (generation != 0U) {
        (void)nuwax_speaker_playback_url_test_stop(
            generation, reason ? reason : "url_test_preempted");
    }
    ESP_LOGI(TAG, "stop requested reason=%s generation=%lu",
             reason ? reason : "-", (unsigned long)generation);
}

bool nuwax_music_url_test_active(void) {
    return atomic_load_explicit(&s_phase, memory_order_acquire) ==
           MUSIC_TEST_RUNNING;
}

bool nuwax_music_url_test_take_terminal(esp_err_t *result_out) {
    int expected = MUSIC_TEST_TERMINAL;
    if (!atomic_compare_exchange_strong_explicit(
            &s_phase,
            &expected,
            MUSIC_TEST_IDLE,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return false;
    }
    portENTER_CRITICAL(&s_state_lock);
    const esp_err_t result = s_terminal_result;
    portEXIT_CRITICAL(&s_state_lock);
    if (result_out) {
        *result_out = result;
    }
    return true;
}
