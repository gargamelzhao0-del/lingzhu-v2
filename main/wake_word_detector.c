#include "wake_word_detector.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mic_input.h"
#include "conversation/wake_word_input_policy.h"
#include "conversation/wake_word_detection_policy.h"
#include "conversation/wake_word_interrupt_filter.h"
#include "conversation/wake_word_probe_policy.h"
#include "p3_software_ref.h"

#if CONFIG_NUWAX_WAKE_WORD_ENABLE
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#endif

static const char *TAG = "nuwax_wake";

#define NUWAX_WAKE_CAPTURE_STOP_TIMEOUT_MS 5000U

#if CONFIG_FREERTOS_UNICORE
#define NUWAX_WAKE_AUDIO_TASK_CORE 0
#else
#define NUWAX_WAKE_AUDIO_TASK_CORE 1
#endif
#define NUWAX_WAKE_CAPTURE_START_TIMEOUT_MS 5000U
#define NUWAX_WAKE_TASK_SUSPEND_SETTLE_TIMEOUT_MS 100U
#define NUWAX_WAKE_CAPTURE_QUEUE_RECOVERY_WAIT_MS 10U
#define NUWAX_WAKE_CAPTURE_QUEUE_FAULT_STREAK 10U
#define NUWAX_WAKE_INFERENCE_COOPERATE_INTERVAL_MS 1000U
#define NUWAX_WAKE_MULTINET_MAX_ARM_CYCLES 32U /* advisory maintenance threshold */
#define NUWAX_WAKE_MULTINET_MAX_LIFETIME_MS (10U * 60U * 1000U)

#ifndef CONFIG_NUWAX_WAKE_WORD_ENABLE
#define CONFIG_NUWAX_WAKE_WORD_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_WAKE_WORD_TASK_STACK_BYTES
#define CONFIG_NUWAX_WAKE_WORD_TASK_STACK_BYTES 8192
#endif

#ifndef CONFIG_NUWAX_WAKE_WORD_LOG_INTERVAL_FRAMES
#define CONFIG_NUWAX_WAKE_WORD_LOG_INTERVAL_FRAMES 0
#endif

#ifndef CONFIG_NUWAX_WAKE_WORD_TASK_PRIORITY
#define CONFIG_NUWAX_WAKE_WORD_TASK_PRIORITY 8
#endif

#ifndef CONFIG_NUWAX_WAKE_CAPTURE_TASK_STACK_BYTES
#define CONFIG_NUWAX_WAKE_CAPTURE_TASK_STACK_BYTES 6144
#endif

#ifndef CONFIG_NUWAX_WAKE_CAPTURE_TASK_PRIORITY
#define CONFIG_NUWAX_WAKE_CAPTURE_TASK_PRIORITY 11
#endif

#ifndef CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH
#define CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH 12
#endif

#ifndef CONFIG_NUWAX_WAKE_WORD_MODEL_NAME
#define CONFIG_NUWAX_WAKE_WORD_MODEL_NAME ""
#endif

#ifndef CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
#define CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET 0
#endif

#ifndef CONFIG_NUWAX_WAKE_WORD_BACKEND_WAKENET
#define CONFIG_NUWAX_WAKE_WORD_BACKEND_WAKENET (!CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET)
#endif

#ifndef CONFIG_NUWAX_CUSTOM_WAKE_WORD_COMMAND
#define CONFIG_NUWAX_CUSTOM_WAKE_WORD_COMMAND ""
#endif

#ifndef CONFIG_NUWAX_CUSTOM_WAKE_WORD_DISPLAY
#define CONFIG_NUWAX_CUSTOM_WAKE_WORD_DISPLAY ""
#endif

#ifndef CONFIG_NUWAX_CUSTOM_WAKE_WORD_LANGUAGE
#define CONFIG_NUWAX_CUSTOM_WAKE_WORD_LANGUAGE "cn"
#endif

#ifndef CONFIG_NUWAX_CUSTOM_WAKE_WORD_DURATION_MS
#define CONFIG_NUWAX_CUSTOM_WAKE_WORD_DURATION_MS 3000
#endif

#ifndef CONFIG_NUWAX_CUSTOM_WAKE_WORD_THRESHOLD_PERCENT
#define CONFIG_NUWAX_CUSTOM_WAKE_WORD_THRESHOLD_PERCENT 7
#endif

#ifndef CONFIG_NUWAX_WAKE_WORD_INTERRUPT_ENABLE
#define CONFIG_NUWAX_WAKE_WORD_INTERRUPT_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_WAKE_WORD_INTERRUPT_SHADOW_ONLY
#define CONFIG_NUWAX_WAKE_WORD_INTERRUPT_SHADOW_ONLY 0
#endif

#ifndef CONFIG_NUWAX_WAKE_WORD_INTERRUPT_THRESHOLD_PERCENT
#define CONFIG_NUWAX_WAKE_WORD_INTERRUPT_THRESHOLD_PERCENT 10
#endif

#define NUWAX_WAKE_WORD_NVS_NAMESPACE "wake_word"
#define NUWAX_WAKE_WORD_NVS_LEGACY_KEY "command"
#define NUWAX_WAKE_WORD_NVS_CONFIG_KEY "config_v2"
#define NUWAX_WAKE_WORD_CONFIG_MAGIC 0x57414B45UL
#define NUWAX_WAKE_WORD_CONFIG_LEGACY_VERSION 2U
#define NUWAX_WAKE_WORD_CONFIG_VERSION 3U
#define NUWAX_WAKE_WORD_LEGACY_MAX_VARIANTS 4U
#define NUWAX_WAKE_WORD_MAX_FEED_GAP_MS 50U
#define NUWAX_WAKE_WORD_HEALTH_LOG_MS 5000U
#define NUWAX_WAKE_INTERRUPT_PRIMARY_REF_DELAY_SAMPLES 360U
#define NUWAX_WAKE_INTERRUPT_SECONDARY_REF_DELAY_SAMPLES 840U
#define NUWAX_WAKE_INTERRUPT_NEARFIELD_MAX_AGE_MS 700U
#define NUWAX_WAKE_CAPTURE_CHUNK_SAMPLES 240U
#define NUWAX_WAKE_CAPTURE_READ_TIMEOUT_MS 35U

#if CONFIG_NUWAX_WAKE_WORD_ENABLE
typedef enum {
    NUWAX_WAKE_TASK_STOPPED = 0,
    NUWAX_WAKE_TASK_RUNNING,
    NUWAX_WAKE_TASK_STOP_REQUESTED,
    NUWAX_WAKE_TASK_EXITING,
    NUWAX_WAKE_TASK_REAPING,
    NUWAX_WAKE_TASK_EXITED,
} nuwax_wake_task_state_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    volatile bool initialized;
    bool deinitializing;
    nuwax_wake_task_state_t task_state;
    atomic_bool stop_requested;
    atomic_bool ready;
    nuwax_wake_word_callback_t callback;
    void *user_ctx;
    srmodel_list_t *models;
    const esp_wn_iface_t *iface;
    model_iface_data_t *model_data;
    const esp_mn_iface_t *mn_iface;
    model_iface_data_t *mn_data;
    bool mn_commands_allocated;
    /* Hard rebuilds are reserved for a missing/broken backend or an explicit
     * command update. Periodic maintenance must not block sleep wake-up. */
    bool mn_rebuild_pending;
    bool mn_maintenance_due;
    uint32_t mn_generation;
    uint32_t mn_created_ms;
    uint32_t mn_arm_cycles;
    uint32_t mn_rebuilds;
    uint32_t mn_rebuild_failures;
    StaticTask_t *task_tcb;
    StackType_t *task_stack;
    int feed_chunksize;
    int sample_rate;
    char active_model[64];
    nuwax_wake_word_config_t config;
    bool command_configured;
    char last_wake_word[64];
    char backend[16];
    uint32_t detections;
    uint32_t weak_rejections;
    uint32_t partial_reads;
    uint32_t read_timeouts;
    uint32_t input_gaps;
    uint32_t max_feed_gap_ms;
    nuwax_wake_arm_context_t arm_context;
} nuwax_wake_detector_t;

static nuwax_wake_detector_t s_detector;

typedef struct {
    int16_t mic1[NUWAX_WAKE_CAPTURE_CHUNK_SAMPLES];
    int16_t mic2[NUWAX_WAKE_CAPTURE_CHUNK_SAMPLES];
    uint32_t generation;
    uint16_t sample_count;
} nuwax_wake_capture_chunk_t;

typedef struct {
    QueueHandle_t free_chunks;
    QueueHandle_t ready_chunks;
    bool free_chunks_with_caps;
    bool ready_chunks_with_caps;
    TaskHandle_t task;
    TaskHandle_t owner_task;
    atomic_bool stop_requested;
    atomic_bool ready;
    atomic_bool exited;
    atomic_uint_fast32_t partial_reads;
    atomic_uint_fast32_t read_timeouts;
    atomic_uint_fast32_t queue_drops;
    atomic_uint_fast32_t queue_high_water;
    atomic_uint_fast32_t queue_empty_waits;
    atomic_uint_fast32_t queue_invariant_faults;
    esp_err_t init_result;
    uint32_t producer_generation;
    uint32_t consumer_generation;
    uint8_t consumer_chunk;
    uint16_t consumer_offset;
    bool consumer_holds_chunk;
    nuwax_wake_capture_chunk_t chunks[CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH];
} nuwax_wake_capture_context_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    char variants[NUWAX_WAKE_WORD_MAX_VARIANTS]
                 [NUWAX_WAKE_WORD_MAX_COMMAND_LEN + 1U];
} nuwax_wake_word_persisted_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    char variants[NUWAX_WAKE_WORD_LEGACY_MAX_VARIANTS]
                 [NUWAX_WAKE_WORD_MAX_COMMAND_LEN + 1U];
} nuwax_wake_word_persisted_legacy_v2_t;

typedef struct {
    bool present;
    nuwax_wake_word_config_t config;
} nuwax_wake_word_persisted_snapshot_t;

typedef enum {
    NUWAX_WAKE_PERSIST_NONE = 0,
    NUWAX_WAKE_PERSIST_WRITE,
    NUWAX_WAKE_PERSIST_ERASE,
} nuwax_wake_persist_action_t;

static void wake_config_to_persisted(
    const nuwax_wake_word_config_t *config,
    nuwax_wake_word_persisted_v2_t *persisted)
{
    memset(persisted, 0, sizeof(*persisted));
    persisted->magic = NUWAX_WAKE_WORD_CONFIG_MAGIC;
    persisted->version = NUWAX_WAKE_WORD_CONFIG_VERSION;
    persisted->count = config->count;
    for (uint8_t i = 0U; i < config->count; ++i) {
        strlcpy(persisted->variants[i],
                config->variants[i],
                sizeof(persisted->variants[i]));
    }
}

static bool persisted_to_wake_config(
    const nuwax_wake_word_persisted_v2_t *persisted,
    nuwax_wake_word_config_t *config)
{
    if (!persisted || !config ||
        persisted->magic != NUWAX_WAKE_WORD_CONFIG_MAGIC ||
        persisted->version != NUWAX_WAKE_WORD_CONFIG_VERSION ||
        persisted->count == 0U ||
        persisted->count > NUWAX_WAKE_WORD_MAX_VARIANTS) {
        return false;
    }
    nuwax_wake_word_config_t raw = {0};
    raw.count = (uint8_t)persisted->count;
    for (uint8_t i = 0U; i < raw.count; ++i) {
        if (!memchr(persisted->variants[i], '\0',
                    sizeof(persisted->variants[i]))) {
            return false;
        }
        strlcpy(raw.variants[i],
                persisted->variants[i],
                sizeof(raw.variants[i]));
    }
    nuwax_wake_word_config_t normalized = {0};
    if (!nuwax_wake_word_config_normalize(&raw, &normalized) ||
        normalized.count != raw.count) {
        return false;
    }
    for (uint8_t i = 0U; i < raw.count; ++i) {
        if (strcmp(raw.variants[i], normalized.variants[i]) != 0) {
            return false;
        }
    }
    *config = normalized;
    return true;
}

static bool legacy_persisted_to_wake_config(
    const nuwax_wake_word_persisted_legacy_v2_t *persisted,
    nuwax_wake_word_config_t *config)
{
    if (!persisted || !config ||
        persisted->magic != NUWAX_WAKE_WORD_CONFIG_MAGIC ||
        persisted->version != NUWAX_WAKE_WORD_CONFIG_LEGACY_VERSION ||
        persisted->count == 0U ||
        persisted->count > NUWAX_WAKE_WORD_LEGACY_MAX_VARIANTS) {
        return false;
    }
    nuwax_wake_word_config_t raw = {0};
    raw.count = (uint8_t)persisted->count;
    for (uint8_t i = 0U; i < raw.count; ++i) {
        if (!memchr(persisted->variants[i], '\0',
                    sizeof(persisted->variants[i]))) {
            return false;
        }
        strlcpy(raw.variants[i],
                persisted->variants[i],
                sizeof(raw.variants[i]));
    }
    nuwax_wake_word_config_t normalized = {0};
    if (!nuwax_wake_word_config_normalize(&raw, &normalized) ||
        normalized.count != raw.count) {
        return false;
    }
    for (uint8_t i = 0U; i < raw.count; ++i) {
        if (strcmp(raw.variants[i], normalized.variants[i]) != 0) {
            return false;
        }
    }
    *config = normalized;
    return true;
}

static esp_err_t load_persisted_config(nuwax_wake_word_config_t *output,
                                       bool *out_used_legacy)
{
    if (!output) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(output, 0, sizeof(*output));
    if (out_used_legacy) {
        *out_used_legacy = false;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NUWAX_WAKE_WORD_NVS_NAMESPACE,
                             NVS_READONLY,
                             &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    nuwax_wake_word_persisted_v2_t persisted = {0};
    size_t persisted_size = sizeof(persisted);
    const esp_err_t v2_err = nvs_get_blob(nvs,
                                          NUWAX_WAKE_WORD_NVS_CONFIG_KEY,
                                          &persisted,
                                          &persisted_size);
    nuwax_wake_word_config_t v2_config = {0};
    const bool v2_valid = v2_err == ESP_OK &&
                          persisted_size == sizeof(persisted) &&
                          persisted_to_wake_config(&persisted, &v2_config);
    nuwax_wake_word_persisted_legacy_v2_t legacy_persisted = {0};
    if (persisted_size == sizeof(legacy_persisted)) {
        memcpy(&legacy_persisted, &persisted, sizeof(legacy_persisted));
    }
    nuwax_wake_word_config_t legacy_v2_config = {0};
    const bool legacy_v2_valid = v2_err == ESP_OK &&
        persisted_size == sizeof(legacy_persisted) &&
        legacy_persisted_to_wake_config(&legacy_persisted,
                                        &legacy_v2_config);
    const bool stored_array_valid = v2_valid || legacy_v2_valid;
    const nuwax_wake_word_config_t *stored_array =
        v2_valid ? &v2_config : &legacy_v2_config;
    if (!stored_array_valid && v2_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG,
                 "stored wake config v2 invalid or unreadable err=%s size=%u; trying legacy key",
                 esp_err_to_name(v2_err),
                 (unsigned)persisted_size);
    }

    char legacy[NUWAX_WAKE_WORD_MAX_COMMAND_LEN + 1U] = {0};
    size_t length = sizeof(legacy);
    const esp_err_t legacy_err = nvs_get_str(nvs,
                                             NUWAX_WAKE_WORD_NVS_LEGACY_KEY,
                                             legacy,
                                             &length);
    nvs_close(nvs);

    nuwax_wake_word_config_t legacy_primary_input = {0};
    nuwax_wake_word_config_t legacy_primary = {0};
    legacy_primary_input.count = 1U;
    strlcpy(legacy_primary_input.variants[0],
            legacy,
            sizeof(legacy_primary_input.variants[0]));
    const bool legacy_primary_valid = legacy_err == ESP_OK &&
        nuwax_wake_word_config_normalize(&legacy_primary_input,
                                         &legacy_primary);
    nuwax_wake_word_config_t legacy_config = {0};
    const bool legacy_valid = legacy_primary_valid &&
        nuwax_wake_word_config_from_legacy(legacy_primary.variants[0],
                                           &legacy_config);
    if (stored_array_valid) {
        /* A downgraded legacy firmware can update only command. If that key
         * differs from the v2 primary, prefer and migrate the newer legacy
         * value instead of resurrecting a stale array after re-upgrade. */
        if (legacy_valid &&
            strcmp(legacy_primary.variants[0],
                   stored_array->variants[0]) != 0) {
            *output = legacy_config;
            if (out_used_legacy) {
                *out_used_legacy = true;
            }
        } else {
            *output = *stored_array;
            if (out_used_legacy && legacy_v2_valid) {
                *out_used_legacy = true;
            }
        }
        return ESP_OK;
    }
    if (legacy_valid) {
        *output = legacy_config;
        if (out_used_legacy) {
            *out_used_legacy = true;
        }
        return ESP_OK;
    }
    if (legacy_err != ESP_OK && legacy_err != ESP_ERR_NVS_NOT_FOUND) {
        return legacy_err;
    }
    return v2_err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND
                                            : ESP_ERR_INVALID_ARG;
}

static esp_err_t persist_config(const nuwax_wake_word_config_t *config)
{
    if (!config || config->count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NUWAX_WAKE_WORD_NVS_NAMESPACE,
                             NVS_READWRITE,
                             &nvs);
    if (err != ESP_OK) {
        return err;
    }
    nuwax_wake_word_persisted_v2_t persisted = {0};
    wake_config_to_persisted(config, &persisted);
    err = nvs_set_blob(nvs,
                       NUWAX_WAKE_WORD_NVS_CONFIG_KEY,
                       &persisted,
                       sizeof(persisted));
    if (err == ESP_OK) {
        err = nvs_set_str(nvs,
                          NUWAX_WAKE_WORD_NVS_LEGACY_KEY,
                          config->variants[0]);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t erase_persisted_config(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NUWAX_WAKE_WORD_NVS_NAMESPACE,
                             NVS_READWRITE,
                             &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    esp_err_t erase_err = nvs_erase_key(nvs,
                                        NUWAX_WAKE_WORD_NVS_CONFIG_KEY);
    if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
        err = erase_err;
    }
    erase_err = nvs_erase_key(nvs, NUWAX_WAKE_WORD_NVS_LEGACY_KEY);
    if (err == ESP_OK && erase_err != ESP_OK &&
        erase_err != ESP_ERR_NVS_NOT_FOUND) {
        err = erase_err;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t capture_persisted_snapshot(
    nuwax_wake_word_persisted_snapshot_t *snapshot)
{
    if (!snapshot) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    const esp_err_t err = load_persisted_config(&snapshot->config, NULL);
    if (err == ESP_OK) {
        snapshot->present = true;
        return ESP_OK;
    }
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_ARG) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t restore_persisted_snapshot(
    const nuwax_wake_word_persisted_snapshot_t *snapshot)
{
    return snapshot && snapshot->present
               ? persist_config(&snapshot->config)
               : erase_persisted_config();
}

static void detector_lock(void) {
    if (s_detector.lock) {
        xSemaphoreTake(s_detector.lock, portMAX_DELAY);
    }
}

static void detector_unlock(void) {
    if (s_detector.lock) {
        xSemaphoreGive(s_detector.lock);
    }
}

static uint32_t detector_monotonic_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *detector_task_state_name(nuwax_wake_task_state_t state) {
    switch (state) {
        case NUWAX_WAKE_TASK_STOPPED:
            return "stopped";
        case NUWAX_WAKE_TASK_RUNNING:
            return "running";
        case NUWAX_WAKE_TASK_STOP_REQUESTED:
            return "stop_requested";
        case NUWAX_WAKE_TASK_EXITING:
            return "exiting";
        case NUWAX_WAKE_TASK_REAPING:
            return "reaping";
        case NUWAX_WAKE_TASK_EXITED:
            return "exited";
        default:
            return "unknown";
    }
}

/* Only call this when no detector task exists. */
static void detector_release_resources(void) {
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    if (s_detector.mn_iface && s_detector.mn_data) {
        /* MultiNet owns the global command tree and releases it from
         * destroy(). Calling esp_mn_commands_free() first double-frees that
         * tree and emits a false MN_COMMAND lifecycle error. */
        s_detector.mn_iface->destroy(s_detector.mn_data);
        s_detector.mn_data = NULL;
        s_detector.mn_commands_allocated = false;
    } else if (s_detector.mn_commands_allocated) {
        (void)esp_mn_commands_free();
        s_detector.mn_commands_allocated = false;
    }
#else
    if (s_detector.mn_iface && s_detector.mn_data) {
        s_detector.mn_iface->destroy(s_detector.mn_data);
        s_detector.mn_data = NULL;
    }
#endif
    if (s_detector.iface && s_detector.model_data) {
        s_detector.iface->destroy(s_detector.model_data);
        s_detector.model_data = NULL;
    }
    if (s_detector.models) {
        esp_srmodel_deinit(s_detector.models);
        s_detector.models = NULL;
    }
    if (s_detector.task_stack) {
        heap_caps_free(s_detector.task_stack);
        s_detector.task_stack = NULL;
    }
    if (s_detector.task_tcb) {
        heap_caps_free(s_detector.task_tcb);
        s_detector.task_tcb = NULL;
    }
    if (s_detector.lock) {
        vSemaphoreDelete(s_detector.lock);
        s_detector.lock = NULL;
    }
    memset(&s_detector, 0, sizeof(s_detector));
}

/*
 * The worker publishes EXITING only after releasing the microphone, finishing
 * all logging, and freeing its temporary buffers. It then suspends itself.
 * The owner waits for that suspension before deleting the static task; it must
 * not suspend the worker asynchronously because the worker could otherwise be
 * stopped while holding a logging or libc lock. Deleting a suspended task is
 * synchronous in ESP-IDF FreeRTOS, so the TCB/stack can only be reused after
 * STOPPED is published below.
 */
static bool detector_reap_exited_task(void) {
    TaskHandle_t task = NULL;

    detector_lock();
    if (s_detector.task_state == NUWAX_WAKE_TASK_EXITING && s_detector.task) {
        task = s_detector.task;
        s_detector.task_state = NUWAX_WAKE_TASK_REAPING;
    }
    detector_unlock();

    if (!task) {
        return false;
    }
    if (task == xTaskGetCurrentTaskHandle()) {
        detector_lock();
        if (s_detector.task == task &&
            s_detector.task_state == NUWAX_WAKE_TASK_REAPING) {
            s_detector.task_state = NUWAX_WAKE_TASK_EXITING;
        }
        detector_unlock();
        return false;
    }

    const uint32_t suspend_started_ms = detector_monotonic_ms();
    eTaskState task_state = eTaskGetState(task);
    while (task_state != eSuspended &&
           (uint32_t)(detector_monotonic_ms() - suspend_started_ms) <
               NUWAX_WAKE_TASK_SUSPEND_SETTLE_TIMEOUT_MS) {
        vTaskDelay(1);
        task_state = eTaskGetState(task);
    }
    if (task_state != eSuspended) {
        ESP_LOGE(TAG,
                 "wake detector exited task did not self-suspend within %u ms state=%d; controlled restart preserves task storage ownership",
                 (unsigned)NUWAX_WAKE_TASK_SUSPEND_SETTLE_TIMEOUT_MS,
                 (int)task_state);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        vTaskDelay(portMAX_DELAY);
        return false;
    }

    detector_lock();
    if (s_detector.task != task ||
        s_detector.task_state != NUWAX_WAKE_TASK_REAPING) {
        detector_unlock();
        ESP_LOGE(TAG, "wake detector task ownership changed during reap");
        return false;
    }
    s_detector.task_state = NUWAX_WAKE_TASK_EXITED;
    detector_unlock();

    vTaskDelete(task);

    detector_lock();
    if (s_detector.task == task &&
        s_detector.task_state == NUWAX_WAKE_TASK_EXITED) {
        s_detector.task = NULL;
        atomic_store_explicit(&s_detector.stop_requested,
                              false,
                              memory_order_release);
        s_detector.task_state = NUWAX_WAKE_TASK_STOPPED;
    }
    detector_unlock();
    ESP_LOGI(TAG, "wake detector task reaped by owner");
    return true;
}

#if CONFIG_NUWAX_WAKE_WORD_BACKEND_WAKENET
static void select_wakenet_model(char *out_model, size_t out_size) {
    if (!out_model || out_size == 0 || !s_detector.models) {
        return;
    }
    out_model[0] = '\0';
    if (CONFIG_NUWAX_WAKE_WORD_MODEL_NAME[0]) {
        for (int i = 0; i < s_detector.models->num; i++) {
            const char *name = s_detector.models->model_name[i];
            if (name && strcmp(name, CONFIG_NUWAX_WAKE_WORD_MODEL_NAME) == 0) {
                strlcpy(out_model, name, out_size);
                return;
            }
        }
        ESP_LOGE(TAG,
                 "Configured WakeNet model not found: %s",
                 CONFIG_NUWAX_WAKE_WORD_MODEL_NAME);
        return;
    }
    char *filtered = esp_srmodel_filter(s_detector.models, ESP_WN_PREFIX, NULL);
    if (filtered) {
        strlcpy(out_model, filtered, out_size);
        return;
    }
    for (int i = 0; i < s_detector.models->num; i++) {
        const char *name = s_detector.models->model_name[i];
        if (name && strstr(name, ESP_WN_PREFIX)) {
            strlcpy(out_model, name, out_size);
            return;
        }
    }
}
#endif

#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
static void select_multinet_model(char *out_model, size_t out_size) {
    if (!out_model || out_size == 0 || !s_detector.models) {
        return;
    }
    out_model[0] = '\0';
    char *filtered = esp_srmodel_filter(s_detector.models,
                                        ESP_MN_PREFIX,
                                        CONFIG_NUWAX_CUSTOM_WAKE_WORD_LANGUAGE);
    if (filtered) {
        strlcpy(out_model, filtered, out_size);
        return;
    }
    filtered = esp_srmodel_filter(s_detector.models, ESP_MN_PREFIX, NULL);
    if (filtered) {
        strlcpy(out_model, filtered, out_size);
        return;
    }
    for (int i = 0; i < s_detector.models->num; i++) {
        const char *name = s_detector.models->model_name[i];
        if (name && strstr(name, ESP_MN_PREFIX)) {
            strlcpy(out_model, name, out_size);
            return;
        }
    }
}
#endif

static esp_err_t configure_multinet_commands(
    const nuwax_wake_word_config_t *config) {
    if (!config || config->count == 0U ||
        config->count > NUWAX_WAKE_WORD_MAX_VARIANTS) {
        ESP_LOGE(TAG, "custom wake word config is empty or invalid");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    bool commands_reused = s_detector.mn_commands_allocated;
    if (!s_detector.mn_commands_allocated) {
        err = esp_mn_commands_alloc(s_detector.mn_iface, s_detector.mn_data);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_mn_commands_alloc failed err=%s", esp_err_to_name(err));
            return err;
        }
        s_detector.mn_commands_allocated = true;
    }
    if (commands_reused) {
        err = esp_mn_commands_clear();
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "esp_mn_commands_clear failed err=%s",
                     esp_err_to_name(err));
            return err;
        }
    }
    for (uint8_t i = 0U; i < config->count; ++i) {
        const char *variant = config->variants[i];
        err = esp_mn_commands_add(1, variant);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "esp_mn_commands_add failed variant=%s index=%u err=%s",
                     variant,
                     (unsigned)i,
                     esp_err_to_name(err));
            return err;
        }
    }

    esp_mn_error_t *mn_error = esp_mn_commands_update();
    if (mn_error) {
        ESP_LOGE(TAG,
                 "custom wake word variants cannot be parsed primary=%s variants=%u",
                 config->variants[0],
                 (unsigned)config->count);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_detector.mn_iface->print_active_speech_commands) {
        s_detector.mn_iface->print_active_speech_commands(s_detector.mn_data);
    }
    return ESP_OK;
}

#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
/* The ESP-SR RNNT decoder owns opaque heap state that clean() does not
 * reconstruct. Refresh it only while the detector task is fully stopped so a
 * corrupt or stale beam-search generation cannot survive indefinitely. */
static void destroy_multinet_backend_locked(void) {
    if (s_detector.mn_iface && s_detector.mn_data) {
        /* model_destroy() owns esp_mn_commands_free(). Keep the application
         * flag in sync without releasing the process-global tree twice. */
        s_detector.mn_iface->destroy(s_detector.mn_data);
        s_detector.mn_data = NULL;
        s_detector.mn_commands_allocated = false;
    } else if (s_detector.mn_commands_allocated) {
        (void)esp_mn_commands_free();
        s_detector.mn_commands_allocated = false;
    }
    s_detector.command_configured = false;
    s_detector.feed_chunksize = 0;
    s_detector.sample_rate = 0;
}

static esp_err_t rebuild_multinet_backend_locked(
    const nuwax_wake_word_config_t *config,
    const char *reason) {
    if (!config || config->count == 0U || !s_detector.mn_iface ||
        s_detector.task_state != NUWAX_WAKE_TASK_STOPPED || s_detector.task) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t previous_generation = s_detector.mn_generation;
    const uint32_t started_ms = detector_monotonic_ms();
    ESP_LOGI(TAG,
             "MultiNet backend rebuild begin reason=%s generation=%lu arm_cycles=%lu internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             reason ? reason : "scheduled",
             (unsigned long)previous_generation,
             (unsigned long)s_detector.mn_arm_cycles,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                                        MALLOC_CAP_8BIT));

    destroy_multinet_backend_locked();
    s_detector.mn_data = s_detector.mn_iface->create(
        s_detector.active_model, CONFIG_NUWAX_CUSTOM_WAKE_WORD_DURATION_MS);
    if (!s_detector.mn_data) {
        s_detector.mn_rebuild_pending = true;
        s_detector.mn_rebuild_failures++;
        ESP_LOGE(TAG,
                 "MultiNet backend rebuild create failed reason=%s failures=%lu",
                 reason ? reason : "scheduled",
                 (unsigned long)s_detector.mn_rebuild_failures);
        return ESP_ERR_NO_MEM;
    }

    float threshold =
        (float)CONFIG_NUWAX_CUSTOM_WAKE_WORD_THRESHOLD_PERCENT / 100.0f;
    if (threshold < 0.01f) {
        threshold = 0.01f;
    } else if (threshold > 0.99f) {
        threshold = 0.99f;
    }
    s_detector.mn_iface->set_det_threshold(s_detector.mn_data, threshold);

    const esp_err_t command_err = configure_multinet_commands(config);
    if (command_err != ESP_OK) {
        destroy_multinet_backend_locked();
        s_detector.mn_rebuild_pending = true;
        s_detector.mn_rebuild_failures++;
        ESP_LOGE(TAG,
                 "MultiNet backend rebuild command restore failed reason=%s err=%s failures=%lu",
                 reason ? reason : "scheduled",
                 esp_err_to_name(command_err),
                 (unsigned long)s_detector.mn_rebuild_failures);
        return command_err;
    }

    const int feed_chunksize =
        s_detector.mn_iface->get_samp_chunksize(s_detector.mn_data);
    const int sample_rate =
        s_detector.mn_iface->get_samp_rate(s_detector.mn_data);
    if (feed_chunksize <= 0 ||
        feed_chunksize > CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME ||
        sample_rate <= 0) {
        destroy_multinet_backend_locked();
        s_detector.mn_rebuild_pending = true;
        s_detector.mn_rebuild_failures++;
        ESP_LOGE(TAG,
                 "MultiNet backend rebuild returned invalid audio contract reason=%s chunksize=%d sample_rate=%d failures=%lu",
                 reason ? reason : "scheduled",
                 feed_chunksize,
                 sample_rate,
                 (unsigned long)s_detector.mn_rebuild_failures);
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_detector.feed_chunksize = feed_chunksize;
    s_detector.sample_rate = sample_rate;
    s_detector.config = *config;
    s_detector.command_configured = true;
    s_detector.mn_generation = previous_generation + 1U;
    if (s_detector.mn_generation == 0U) {
        s_detector.mn_generation = 1U;
    }
    s_detector.mn_created_ms = detector_monotonic_ms();
    s_detector.mn_arm_cycles = 0U;
    s_detector.mn_rebuild_pending = false;
    s_detector.mn_maintenance_due = false;
    s_detector.mn_rebuilds++;
    ESP_LOGI(TAG,
             "MultiNet backend rebuild complete reason=%s generation=%lu elapsed_ms=%lu rebuilds=%lu chunksize=%d sample_rate=%d",
             reason ? reason : "scheduled",
             (unsigned long)s_detector.mn_generation,
             (unsigned long)(s_detector.mn_created_ms - started_ms),
             (unsigned long)s_detector.mn_rebuilds,
             s_detector.feed_chunksize,
             s_detector.sample_rate);
    return ESP_OK;
}
#endif

static void wake_capture_return_chunk(nuwax_wake_capture_context_t *ctx,
                                      uint8_t index) {
    if (!ctx || !ctx->free_chunks ||
        xQueueSend(ctx->free_chunks, &index, 0) != pdTRUE) {
        ESP_LOGE(TAG,
                 "playback wake capture chunk ownership lost index=%u",
                 (unsigned)index);
    }
}

static void wake_capture_task(void *arg) {
    nuwax_wake_capture_context_t *ctx = arg;
    nuwax_mic_input_t mic = {0};
    bool mic_ready = false;
    uint32_t queue_empty_streak = 0U;
    TickType_t queue_recovery_ticks =
        pdMS_TO_TICKS(NUWAX_WAKE_CAPTURE_QUEUE_RECOVERY_WAIT_MS);
    if (queue_recovery_ticks == 0U) {
        queue_recovery_ticks = 1U;
    }
    ctx->init_result = nuwax_mic_input_init_with_owner(
        &mic, NUWAX_MIC_OWNER_WAKE_DETECTOR);
    if (ctx->init_result != ESP_OK) {
        ESP_LOGE(TAG,
                 "playback wake capture mic init failed: %s",
                 esp_err_to_name(ctx->init_result));
        goto done;
    }
    mic_ready = true;
    atomic_store_explicit(&ctx->ready, true, memory_order_release);
    if (ctx->owner_task) {
        xTaskNotifyGive(ctx->owner_task);
    }
    ESP_LOGI(TAG,
             "playback wake capture ready chunk_samples=%u queue_depth=%u stack=%u priority=%u core=%u",
             (unsigned)NUWAX_WAKE_CAPTURE_CHUNK_SAMPLES,
             (unsigned)CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH,
             (unsigned)CONFIG_NUWAX_WAKE_CAPTURE_TASK_STACK_BYTES,
             (unsigned)CONFIG_NUWAX_WAKE_CAPTURE_TASK_PRIORITY,
             (unsigned)NUWAX_WAKE_AUDIO_TASK_CORE);

    while (!atomic_load_explicit(&ctx->stop_requested,
                                 memory_order_acquire) &&
           !atomic_load_explicit(&s_detector.stop_requested,
                                 memory_order_acquire)) {
        uint8_t index = 0U;
        if (xQueueReceive(ctx->free_chunks, &index, 0) != pdTRUE) {
            if (xQueueReceive(ctx->ready_chunks, &index, 0) != pdTRUE) {
                if (xQueueReceive(ctx->free_chunks,
                                  &index,
                                  queue_recovery_ticks) != pdTRUE) {
                    queue_empty_streak++;
                    const uint_fast32_t empty_waits =
                        atomic_fetch_add_explicit(&ctx->queue_empty_waits,
                                                  1U,
                                                  memory_order_relaxed) +
                        1U;
                    if (queue_empty_streak == 1U) {
                        ESP_LOGW(TAG,
                                 "playback wake capture has no owned chunk; bounded recovery started free=%lu ready=%lu",
                                 (unsigned long)uxQueueMessagesWaiting(
                                     ctx->free_chunks),
                                 (unsigned long)uxQueueMessagesWaiting(
                                     ctx->ready_chunks));
                    }
                    if (queue_empty_streak >=
                        NUWAX_WAKE_CAPTURE_QUEUE_FAULT_STREAK) {
                        atomic_fetch_add_explicit(
                            &ctx->queue_invariant_faults,
                            1U,
                            memory_order_relaxed);
                        ctx->init_result = ESP_ERR_INVALID_STATE;
                        ESP_LOGE(TAG,
                                 "playback wake capture queue ownership invariant failed streak=%lu waits=%lu depth=%u; exiting child for supervisor recovery",
                                 (unsigned long)queue_empty_streak,
                                 (unsigned long)empty_waits,
                                 (unsigned)CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH);
                        goto done;
                    }
                    continue;
                }
            } else {
                atomic_fetch_add_explicit(&ctx->queue_drops,
                                          1U,
                                          memory_order_relaxed);
                ctx->producer_generation =
                    nuwax_wake_input_next_generation(ctx->producer_generation);
            }
        }
        queue_empty_streak = 0U;
        if (index >= CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH) {
            ESP_LOGE(TAG,
                     "playback wake capture invalid free chunk index=%u",
                     (unsigned)index);
            ctx->producer_generation =
                nuwax_wake_input_next_generation(ctx->producer_generation);
            continue;
        }
        if (atomic_load_explicit(&ctx->stop_requested,
                                 memory_order_acquire) ||
            atomic_load_explicit(&s_detector.stop_requested,
                                 memory_order_acquire)) {
            wake_capture_return_chunk(ctx, index);
            break;
        }

        nuwax_wake_capture_chunk_t *chunk = &ctx->chunks[index];
        size_t samples_read = 0U;
        const esp_err_t read_result =
            nuwax_mic_input_read_vocat_mic12_pcm16(
                &mic,
                chunk->mic1,
                chunk->mic2,
                NUWAX_WAKE_CAPTURE_CHUNK_SAMPLES,
                NUWAX_WAKE_CAPTURE_READ_TIMEOUT_MS,
                &samples_read);
        if (read_result == ESP_ERR_NOT_SUPPORTED) {
            ctx->init_result = read_result;
            wake_capture_return_chunk(ctx, index);
            ESP_LOGE(TAG,
                     "playback wake capture requires vocat dual-microphone input");
            goto done;
        }
        if (read_result == ESP_ERR_INVALID_RESPONSE) {
            ctx->producer_generation =
                nuwax_wake_input_next_generation(ctx->producer_generation);
            wake_capture_return_chunk(ctx, index);
            continue;
        }
        if (read_result != ESP_OK && read_result != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG,
                     "playback wake capture read failed: %s",
                     esp_err_to_name(read_result));
            ctx->producer_generation =
                nuwax_wake_input_next_generation(ctx->producer_generation);
            wake_capture_return_chunk(ctx, index);
            /* At the product's 100 Hz tick rate, pdMS_TO_TICKS(5) is zero and
             * turns a hardware error into a scheduler-speed retry loop. */
            vTaskDelay(1);
            continue;
        }
        if (read_result == ESP_ERR_TIMEOUT) {
            atomic_fetch_add_explicit(&ctx->read_timeouts,
                                      1U,
                                      memory_order_relaxed);
        }
        if (samples_read == 0U) {
            ctx->producer_generation =
                nuwax_wake_input_next_generation(ctx->producer_generation);
            wake_capture_return_chunk(ctx, index);
            continue;
        }
        if (samples_read < NUWAX_WAKE_CAPTURE_CHUNK_SAMPLES) {
            atomic_fetch_add_explicit(&ctx->partial_reads,
                                      1U,
                                      memory_order_relaxed);
        }
        if (samples_read > UINT16_MAX) {
            samples_read = UINT16_MAX;
        }
        chunk->sample_count = (uint16_t)samples_read;
        chunk->generation = ctx->producer_generation;
        if (xQueueSend(ctx->ready_chunks, &index, 0) != pdTRUE) {
            ctx->producer_generation =
                nuwax_wake_input_next_generation(ctx->producer_generation);
            wake_capture_return_chunk(ctx, index);
            continue;
        }
        const uint_fast32_t depth =
            (uint_fast32_t)uxQueueMessagesWaiting(ctx->ready_chunks);
        uint_fast32_t previous = atomic_load_explicit(
            &ctx->queue_high_water, memory_order_relaxed);
        while (depth > previous &&
               !atomic_compare_exchange_weak_explicit(
                   &ctx->queue_high_water,
                   &previous,
                   depth,
                   memory_order_relaxed,
                   memory_order_relaxed)) {
        }
    }

done:
    atomic_store_explicit(&ctx->ready, false, memory_order_release);
    if (mic_ready) {
        (void)nuwax_mic_input_deinit(&mic);
    }
    atomic_store_explicit(&ctx->exited, true, memory_order_release);
    if (ctx->owner_task) {
        xTaskNotifyGive(ctx->owner_task);
    }
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void wake_capture_delete_queue(QueueHandle_t *queue,
                                      bool *created_with_caps) {
    if (!queue || !*queue) {
        return;
    }
    if (created_with_caps && *created_with_caps) {
        vQueueDeleteWithCaps(*queue);
    } else {
        vQueueDelete(*queue);
    }
    *queue = NULL;
    if (created_with_caps) {
        *created_with_caps = false;
    }
}

static void wake_capture_stop(nuwax_wake_capture_context_t **owner) {
    if (!owner || !*owner) {
        return;
    }
    nuwax_wake_capture_context_t *ctx = *owner;
    UBaseType_t task_stack_min_free = 0U;
    atomic_store_explicit(&ctx->stop_requested, true, memory_order_release);
    const uint32_t wait_started_ms = detector_monotonic_ms();
    uint32_t last_wait_log_ms = detector_monotonic_ms();
    while (ctx->task &&
           !atomic_load_explicit(&ctx->exited, memory_order_acquire)) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        const uint32_t now_ms = detector_monotonic_ms();
        if ((uint32_t)(now_ms - last_wait_log_ms) >= 1000U) {
            ESP_LOGW(TAG,
                     "waiting for playback wake capture to release microphone elapsed_ms=%lu",
                     (unsigned long)(now_ms - wait_started_ms));
            last_wait_log_ms = now_ms;
        }
        if ((uint32_t)(now_ms - wait_started_ms) >=
            NUWAX_WAKE_CAPTURE_STOP_TIMEOUT_MS) {
            ESP_LOGE(TAG,
                     "playback wake capture did not release microphone timeout_ms=%u; controlled restart preserves I2S ownership",
                     (unsigned)NUWAX_WAKE_CAPTURE_STOP_TIMEOUT_MS);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            vTaskDelay(portMAX_DELAY);
        }
    }
    if (ctx->task) {
        eTaskState task_state = eTaskGetState(ctx->task);
        const uint32_t suspend_started_ms = detector_monotonic_ms();
        while (task_state != eSuspended &&
               (uint32_t)(detector_monotonic_ms() - suspend_started_ms) <
                   NUWAX_WAKE_TASK_SUSPEND_SETTLE_TIMEOUT_MS) {
            vTaskSuspend(ctx->task);
            vTaskDelay(1);
            task_state = eTaskGetState(ctx->task);
        }
        if (task_state != eSuspended) {
            ESP_LOGE(TAG,
                     "playback wake capture exited task did not suspend state=%d timeout_ms=%u; controlled restart preserves task storage ownership",
                     (int)task_state,
                     (unsigned)NUWAX_WAKE_TASK_SUSPEND_SETTLE_TIMEOUT_MS);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            vTaskDelay(portMAX_DELAY);
        }
        task_stack_min_free = uxTaskGetStackHighWaterMark(ctx->task);
        vTaskDeleteWithCaps(ctx->task);
        ctx->task = NULL;
    }
    if (ctx->consumer_holds_chunk) {
        ctx->consumer_holds_chunk = false;
    }
    ESP_LOGI(TAG,
             "playback wake capture stopped partial_reads=%lu timeouts=%lu queue_drops=%lu queue_high_water=%lu empty_waits=%lu invariant_faults=%lu stack_min_free=%u stack_bytes=%u",
             (unsigned long)atomic_load_explicit(&ctx->partial_reads,
                                                 memory_order_relaxed),
             (unsigned long)atomic_load_explicit(&ctx->read_timeouts,
                                                 memory_order_relaxed),
             (unsigned long)atomic_load_explicit(&ctx->queue_drops,
                                                 memory_order_relaxed),
             (unsigned long)atomic_load_explicit(&ctx->queue_high_water,
                                                 memory_order_relaxed),
             (unsigned long)atomic_load_explicit(&ctx->queue_empty_waits,
                                                 memory_order_relaxed),
             (unsigned long)atomic_load_explicit(
                 &ctx->queue_invariant_faults, memory_order_relaxed),
             (unsigned)task_stack_min_free,
             (unsigned)CONFIG_NUWAX_WAKE_CAPTURE_TASK_STACK_BYTES);
    wake_capture_delete_queue(&ctx->ready_chunks,
                              &ctx->ready_chunks_with_caps);
    wake_capture_delete_queue(&ctx->free_chunks,
                              &ctx->free_chunks_with_caps);
    heap_caps_free(ctx);
    *owner = NULL;
}

static esp_err_t wake_capture_start(nuwax_wake_capture_context_t **owner) {
    if (!owner) {
        return ESP_ERR_INVALID_ARG;
    }
    *owner = NULL;
    nuwax_wake_capture_context_t *ctx = heap_caps_calloc(
        1U,
        sizeof(*ctx),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    atomic_init(&ctx->stop_requested, false);
    atomic_init(&ctx->ready, false);
    atomic_init(&ctx->exited, false);
    atomic_init(&ctx->partial_reads, 0U);
    atomic_init(&ctx->read_timeouts, 0U);
    atomic_init(&ctx->queue_drops, 0U);
    atomic_init(&ctx->queue_high_water, 0U);
    atomic_init(&ctx->queue_empty_waits, 0U);
    atomic_init(&ctx->queue_invariant_faults, 0U);
    ctx->init_result = ESP_ERR_NOT_FINISHED;
    ctx->producer_generation = 1U;
    ctx->owner_task = xTaskGetCurrentTaskHandle();
    ctx->free_chunks = xQueueCreateWithCaps(
        CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH,
        sizeof(uint8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->free_chunks_with_caps = ctx->free_chunks != NULL;
    if (!ctx->free_chunks) {
        ctx->free_chunks = xQueueCreate(CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH,
                                        sizeof(uint8_t));
    }
    ctx->ready_chunks = xQueueCreateWithCaps(
        CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH,
        sizeof(uint8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->ready_chunks_with_caps = ctx->ready_chunks != NULL;
    if (!ctx->ready_chunks) {
        ctx->ready_chunks = xQueueCreate(CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH,
                                         sizeof(uint8_t));
    }
    if (!ctx->free_chunks || !ctx->ready_chunks) {
        wake_capture_stop(&ctx);
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t index = 0U;
         index < CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH;
         ++index) {
        if (xQueueSend(ctx->free_chunks, &index, 0) != pdTRUE) {
            wake_capture_stop(&ctx);
            return ESP_ERR_NO_MEM;
        }
    }
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        wake_capture_task,
        "wake_capture",
        CONFIG_NUWAX_WAKE_CAPTURE_TASK_STACK_BYTES,
        ctx,
        CONFIG_NUWAX_WAKE_CAPTURE_TASK_PRIORITY,
        &ctx->task,
        NUWAX_WAKE_AUDIO_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS || !ctx->task) {
        ctx->task = NULL;
        wake_capture_stop(&ctx);
        return ESP_ERR_NO_MEM;
    }
    *owner = ctx;
    const uint32_t start_wait_ms = detector_monotonic_ms();
    while (!atomic_load_explicit(&ctx->ready, memory_order_acquire) &&
           !atomic_load_explicit(&ctx->exited, memory_order_acquire) &&
           !atomic_load_explicit(&s_detector.stop_requested,
                                 memory_order_acquire) &&
           (uint32_t)(detector_monotonic_ms() - start_wait_ms) <
               NUWAX_WAKE_CAPTURE_START_TIMEOUT_MS) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
    if (atomic_load_explicit(&ctx->ready, memory_order_acquire)) {
        return ESP_OK;
    }
    const bool start_timed_out =
        !atomic_load_explicit(&ctx->exited, memory_order_acquire) &&
        !atomic_load_explicit(&s_detector.stop_requested,
                              memory_order_acquire) &&
        (uint32_t)(detector_monotonic_ms() - start_wait_ms) >=
            NUWAX_WAKE_CAPTURE_START_TIMEOUT_MS;
    if (start_timed_out) {
        ESP_LOGE(TAG,
                 "playback wake capture start timed out after %u ms; requesting bounded child shutdown",
                 (unsigned)NUWAX_WAKE_CAPTURE_START_TIMEOUT_MS);
    }
    const esp_err_t result = start_timed_out
                                 ? ESP_ERR_TIMEOUT
                                 : ctx->init_result == ESP_ERR_NOT_FINISHED
                                       ? ESP_ERR_INVALID_STATE
                                       : ctx->init_result;
    wake_capture_stop(owner);
    return result;
}

static esp_err_t wake_capture_read(nuwax_wake_capture_context_t *ctx,
                                   int16_t *mic1,
                                   int16_t *mic2,
                                   size_t sample_count,
                                   uint32_t timeout_ms,
                                   size_t *samples_read) {
    if (!ctx || !mic1 || !mic2 || !samples_read) {
        return ESP_ERR_INVALID_ARG;
    }
    *samples_read = 0U;
    const uint32_t started_ms = detector_monotonic_ms();
    while (*samples_read < sample_count) {
        if (!ctx->consumer_holds_chunk) {
            if (atomic_load_explicit(&ctx->exited, memory_order_acquire) &&
                uxQueueMessagesWaiting(ctx->ready_chunks) == 0U) {
                return ctx->init_result == ESP_OK
                           ? ESP_ERR_INVALID_STATE
                           : ctx->init_result;
            }
            uint32_t remaining_ms = timeout_ms;
            if (timeout_ms != portMAX_DELAY) {
                const uint32_t elapsed =
                    detector_monotonic_ms() - started_ms;
                if (elapsed >= timeout_ms) {
                    return ESP_ERR_TIMEOUT;
                }
                remaining_ms = timeout_ms - elapsed;
            }
            TickType_t wait_ticks = remaining_ms == portMAX_DELAY
                                        ? portMAX_DELAY
                                        : pdMS_TO_TICKS(remaining_ms);
            if (wait_ticks == 0U && remaining_ms != 0U) {
                wait_ticks = 1U;
            }
            if (xQueueReceive(ctx->ready_chunks,
                              &ctx->consumer_chunk,
                              wait_ticks) != pdTRUE) {
                return ESP_ERR_TIMEOUT;
            }
            if (ctx->consumer_chunk >= CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH) {
                ESP_LOGE(TAG,
                         "playback wake capture invalid ready chunk index=%u",
                         (unsigned)ctx->consumer_chunk);
                return ESP_ERR_INVALID_RESPONSE;
            }
            ctx->consumer_offset = 0U;
            ctx->consumer_holds_chunk = true;
            const uint32_t generation =
                ctx->chunks[ctx->consumer_chunk].generation;
            if (ctx->consumer_generation == 0U) {
                ctx->consumer_generation = generation;
            } else if (nuwax_wake_input_generation_changed(
                           ctx->consumer_generation, generation)) {
                ctx->consumer_generation = generation;
                return ESP_ERR_INVALID_RESPONSE;
            }
        }

        nuwax_wake_capture_chunk_t *chunk =
            &ctx->chunks[ctx->consumer_chunk];
        const size_t available =
            chunk->sample_count > ctx->consumer_offset
                ? (size_t)chunk->sample_count - ctx->consumer_offset
                : 0U;
        if (available == 0U) {
            const uint8_t released = ctx->consumer_chunk;
            ctx->consumer_holds_chunk = false;
            ctx->consumer_offset = 0U;
            wake_capture_return_chunk(ctx, released);
            continue;
        }
        const size_t needed = sample_count - *samples_read;
        const size_t copied = available < needed ? available : needed;
        memcpy(mic1 + *samples_read,
               chunk->mic1 + ctx->consumer_offset,
               copied * sizeof(mic1[0]));
        memcpy(mic2 + *samples_read,
               chunk->mic2 + ctx->consumer_offset,
               copied * sizeof(mic2[0]));
        *samples_read += copied;
        ctx->consumer_offset += (uint16_t)copied;
        if (ctx->consumer_offset >= chunk->sample_count) {
            const uint8_t released = ctx->consumer_chunk;
            ctx->consumer_holds_chunk = false;
            ctx->consumer_offset = 0U;
            wake_capture_return_chunk(ctx, released);
        }
    }
    return ESP_OK;
}

static void wake_word_task(void *arg) {
    (void)arg;
    nuwax_mic_input_t mic = {0};
    nuwax_wake_capture_context_t *playback_capture = NULL;
    bool mic_ready = false;
    int16_t *samples = NULL;
    int16_t *mic2_samples = NULL;
    int16_t *reference_primary = NULL;
    int16_t *reference_secondary = NULL;
    nuwax_p3_software_ref_reader_t reference_reader = {0};
    nuwax_wake_interrupt_filter_result_t filter_result = {0};
    nuwax_wake_interrupt_filter_result_t last_safe_filter_result = {0};
    nuwax_wake_probe_policy_t probe_policy = {0};
    uint32_t last_nearfield_safe_ms = 0U;
    nuwax_wake_arm_context_t arm_context = {0};
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    uint32_t mn_generation = 0U;
    uint32_t mn_created_ms = 0U;
#endif
    detector_lock();
    arm_context = s_detector.arm_context;
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    mn_generation = s_detector.mn_generation;
    mn_created_ms = s_detector.mn_created_ms;
#endif
    detector_unlock();
    const bool playback_interrupt_mode =
        arm_context.mode == NUWAX_WAKE_MODE_PLAYBACK_INTERRUPT;
    size_t sample_count = (size_t)s_detector.feed_chunksize;
    uint32_t frames = 0U;
    size_t samples_filled = 0U;
    uint32_t last_feed_ms = 0U;
    uint32_t last_health_log_ms = 0U;
    uint32_t partial_reads = 0U;
    uint32_t read_timeouts = 0U;
    uint32_t input_gaps = 0U;
    uint32_t delayed_feeds = 0U;
    uint32_t max_feed_gap_ms = 0U;
    bool capture_gap_since_feed = false;
    uint32_t noise_floor_avg_abs = 0U;
    uint32_t voice_signal_avg_abs_max = 0U;
    uint32_t voice_span_started_ms = 0U;
    uint32_t voice_last_seen_ms = 0U;
    uint32_t window_clipped_samples = 0U;
    uint32_t window_total_samples = 0U;
    uint32_t weak_rejections = 0U;
    uint64_t inference_window_us = 0U;
    uint32_t inference_window_calls = 0U;
    uint32_t inference_window_max_us = 0U;
    uint32_t cooperative_blocks = 0U;
    uint32_t cooperative_blocks_window = 0U;
    uint32_t max_cooperate_gap_ms = 0U;
    uint32_t cooperate_queue_high = 0U;
    uint32_t last_cooperate_ms = 0U;
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    bool lifetime_refresh_deferred = false;
#endif
    if (sample_count == 0) {
        sample_count = (size_t)(CONFIG_NUWAX_VOICE_SAMPLE_RATE * CONFIG_NUWAX_VOICE_FRAME_DURATION_MS / 1000);
    }
    if (sample_count == 0 || sample_count > CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME) {
        sample_count = CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME;
    }

    samples = heap_caps_malloc(sample_count * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!samples) {
        ESP_LOGE(TAG, "wake detector sample allocation failed samples=%u", (unsigned)sample_count);
        goto done;
    }
    if (playback_interrupt_mode) {
        mic2_samples = heap_caps_malloc(sample_count * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        reference_primary = heap_caps_malloc(sample_count * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        reference_secondary = heap_caps_malloc(sample_count * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mic2_samples || !reference_primary || !reference_secondary) {
            ESP_LOGE(TAG,
                     "playback wake filter buffers unavailable; detector will remain unarmed mic2=%p ref1=%p ref2=%p",
                     mic2_samples,
                     reference_primary,
                     reference_secondary);
            goto done;
        }
    }

    esp_err_t err = playback_interrupt_mode
                        ? wake_capture_start(&playback_capture)
                        : nuwax_mic_input_init_with_owner(
                              &mic, NUWAX_MIC_OWNER_WAKE_DETECTOR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wake detector mic init failed: %s", esp_err_to_name(err));
        goto done;
    }
    mic_ready = !playback_interrupt_mode;
    detector_lock();
    const bool stop_before_ready = atomic_load_explicit(&s_detector.stop_requested,
                                                        memory_order_acquire);
    if (!stop_before_ready && s_detector.task_state == NUWAX_WAKE_TASK_RUNNING) {
        atomic_store_explicit(&s_detector.ready, true, memory_order_release);
    }
    detector_unlock();
    if (stop_before_ready) {
        ESP_LOGI(TAG, "wake detector stop requested before ready; releasing microphone");
        goto done;
    }
    ESP_LOGI(TAG, "wake detector microphone ready");

    ESP_LOGI(TAG,
             "wake detector running model=%s mode=%d token=%lu ws_epoch=%lu speaker_generation=%lu sample_rate=%d feed_chunksize=%d read_samples=%u",
             s_detector.active_model,
             (int)arm_context.mode,
             (unsigned long)arm_context.mode_token,
             (unsigned long)arm_context.ws_epoch,
             (unsigned long)arm_context.speaker_generation,
             s_detector.sample_rate,
             s_detector.feed_chunksize,
             (unsigned)sample_count);

    const uint32_t detector_sample_rate =
        s_detector.sample_rate > 0 ? (uint32_t)s_detector.sample_rate : 16000U;
    uint32_t read_timeout_ms =
        (uint32_t)(((uint64_t)sample_count * 1000U +
                    detector_sample_rate - 1U) /
                   detector_sample_rate) +
        20U;
    if (read_timeout_ms < 20U) {
        read_timeout_ms = 20U;
    } else if (read_timeout_ms > 100U) {
        read_timeout_ms = 100U;
    }
    ESP_LOGI(TAG,
             "wake detector bounded read timeout=%lums configured_mic_timeout=%dms",
             (unsigned long)read_timeout_ms,
             CONFIG_NUWAX_MIC_READ_TIMEOUT_MS);

    last_health_log_ms = detector_monotonic_ms();
    last_cooperate_ms = last_health_log_ms;
    while (!atomic_load_explicit(&s_detector.stop_requested,
                                 memory_order_acquire)) {
        size_t samples_read = 0;
        const size_t samples_needed =
            nuwax_wake_input_samples_needed(samples_filled, sample_count);
        if (playback_interrupt_mode && playback_capture) {
            err = wake_capture_read(
                playback_capture,
                samples + samples_filled,
                mic2_samples + samples_filled,
                samples_needed,
                read_timeout_ms,
                &samples_read);
        } else {
            err = nuwax_mic_input_read_pcm16(&mic,
                                             samples + samples_filled,
                                             samples_needed,
                                             read_timeout_ms,
                                             &samples_read);
        }
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            if (err == ESP_ERR_INVALID_RESPONSE) {
                /* mic_input consumed and rejected a discontinuous DMA frame.
                 * Retry immediately so RX catches up; delaying here used to
                 * let another DMA buffer overflow before every retry. */
                samples_filled = 0U;
                capture_gap_since_feed = true;
                continue;
            }
            if (playback_interrupt_mode && playback_capture) {
                const bool stop_requested =
                    atomic_load_explicit(&s_detector.stop_requested,
                                         memory_order_acquire) ||
                    atomic_load_explicit(&playback_capture->stop_requested,
                                         memory_order_acquire);
                const bool capture_exited =
                    atomic_load_explicit(&playback_capture->exited,
                                         memory_order_acquire);
                const bool expected_exit = err == ESP_ERR_INVALID_STATE &&
                    nuwax_wake_input_capture_exit_expected(
                        stop_requested,
                        capture_exited,
                        playback_capture->init_result == ESP_OK);
                if (expected_exit) {
                    ESP_LOGI(TAG,
                             "playback wake capture stopped after owner request");
                } else {
                    ESP_LOGE(TAG,
                             "playback wake capture stopped unexpectedly: %s",
                             esp_err_to_name(err));
                }
                break;
            }
            ESP_LOGW(TAG, "wake detector mic read failed: %s", esp_err_to_name(err));
            samples_filled = 0U;
            capture_gap_since_feed = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (err == ESP_ERR_TIMEOUT) {
            read_timeouts++;
            if (samples_read == 0U) {
                samples_filled = 0U;
                capture_gap_since_feed = true;
            }
        }
        if (samples_read < samples_needed) {
            partial_reads++;
        }
        samples_filled += nuwax_wake_input_accept_count(
            samples_filled, samples_read, sample_count);
        if (samples_filled < sample_count) {
            continue;
        }

        const uint32_t feed_ms = detector_monotonic_ms();
        const uint32_t feed_gap_ms = last_feed_ms == 0U
                                         ? 0U
                                         : (uint32_t)(feed_ms - last_feed_ms);
        if (feed_gap_ms > max_feed_gap_ms) {
            max_feed_gap_ms = feed_gap_ms;
        }
        const bool delayed_feed = nuwax_wake_input_gap_is_discontinuous(
            last_feed_ms, feed_ms, NUWAX_WAKE_WORD_MAX_FEED_GAP_MS);
        if (capture_gap_since_feed) {
            input_gaps++;
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
            if (s_detector.mn_iface && s_detector.mn_data) {
                s_detector.mn_iface->clean(s_detector.mn_data);
            }
#endif
            voice_span_started_ms = 0U;
            voice_last_seen_ms = 0U;
            voice_signal_avg_abs_max = 0U;
            window_clipped_samples = 0U;
            window_total_samples = 0U;
            last_nearfield_safe_ms = 0U;
            memset(&last_safe_filter_result, 0,
                   sizeof(last_safe_filter_result));
            (void)nuwax_wake_probe_policy_observe(
                &probe_policy, false, feed_ms);
            nuwax_p3_software_ref_reader_invalidate(&reference_reader);
            ESP_LOGW(TAG,
                     "wake detector capture discontinuity gap_ms=%lu; recognition state reset",
                     (unsigned long)feed_gap_ms);
        } else if (delayed_feed) {
            /* A late task wake-up does not imply lost PCM: the I2S DMA can
             * still provide a continuous full frame. Keep the recognizer
             * history and report the scheduling delay separately. */
            delayed_feeds++;
        }
        last_feed_ms = feed_ms;
        samples_filled = 0U;
        capture_gap_since_feed = false;

        frames++;
        uint64_t frame_sum_abs = 0U;
        int64_t frame_sum = 0;
        uint32_t frame_peak = 0U;
        uint32_t frame_clipped_samples = 0U;
        for (size_t i = 0U; i < sample_count; ++i) {
            const int32_t value = samples[i];
            const uint32_t abs_value =
                (uint32_t)(value < 0 ? -value : value);
            frame_sum_abs += abs_value;
            frame_sum += value;
            if (abs_value > frame_peak) {
                frame_peak = abs_value;
            }
            if (abs_value >= 32000U) {
                frame_clipped_samples++;
            }
        }
        const uint32_t frame_avg_abs =
            (uint32_t)(frame_sum_abs / sample_count);
        const int32_t frame_dc_mean =
            (int32_t)(frame_sum / (int64_t)sample_count);
        window_clipped_samples += frame_clipped_samples;
        window_total_samples += (uint32_t)sample_count;

        if (playback_interrupt_mode && mic2_samples && reference_primary &&
            reference_secondary) {
            size_t secondary_copied = 0U;
            const size_t primary_copied =
                nuwax_p3_software_ref_read_delayed_pair_continuous(
                    &reference_reader,
                    reference_primary,
                    NUWAX_WAKE_INTERRUPT_PRIMARY_REF_DELAY_SAMPLES,
                    reference_secondary,
                    NUWAX_WAKE_INTERRUPT_SECONDARY_REF_DELAY_SAMPLES,
                    sample_count,
                    &secondary_copied);
            const bool reference_available =
                primary_copied == sample_count &&
                secondary_copied == sample_count &&
                reference_reader.initialized;
            nuwax_wake_interrupt_filter_analyze(
                samples,
                mic2_samples,
                reference_primary,
                reference_secondary,
                sample_count,
                reference_available,
                &filter_result);
            if (filter_result.nearfield_safe) {
                last_nearfield_safe_ms = feed_ms;
                last_safe_filter_result = filter_result;
            }
        } else if (playback_interrupt_mode) {
            memset(&filter_result, 0, sizeof(filter_result));
            last_nearfield_safe_ms = 0U;
        }

        const bool probe_requested_now =
            playback_interrupt_mode &&
            nuwax_wake_probe_policy_observe(
                &probe_policy,
                filter_result.nearfield_safe && filter_result.reference_active,
                feed_ms);
        if (probe_requested_now) {
            nuwax_wake_word_callback_t callback = NULL;
            void *callback_ctx = NULL;
            char probe_word[64] = {0};
            detector_lock();
            callback = s_detector.callback;
            callback_ctx = s_detector.user_ctx;
            strlcpy(probe_word,
                    s_detector.config.variants[0],
                    sizeof(probe_word));
            detector_unlock();
            if (callback) {
                nuwax_wake_word_result_t result = {
                    .arm = arm_context,
                    .detected_ms = feed_ms,
                    .detected_us = esp_timer_get_time(),
                    .reference_generation = reference_reader.generation,
                    .reference_correlation_q15 =
                        filter_result.reference_correlation_q15,
                    .mic1_avg_abs = filter_result.mic1_avg_abs,
                    .mic1_peak = filter_result.mic1_peak,
                    .mic2_avg_abs = filter_result.mic2_avg_abs,
                    .mic2_peak = filter_result.mic2_peak,
                    .diff_avg_abs = filter_result.diff_avg_abs,
                    .diff_peak = filter_result.diff_peak,
                    .reference_available = filter_result.reference_available,
                    .reference_active = filter_result.reference_active,
                    .echo_likely = filter_result.echo_likely,
                    .nearfield_safe = filter_result.nearfield_safe,
                    .probe_requested = true,
                    .action_eligible = false,
                };
                strlcpy(result.wake_word,
                        probe_word,
                        sizeof(result.wake_word));
                callback(&result, callback_ctx);
                ESP_LOGI(TAG,
                         "playback wake probe requested token=%lu generation=%lu safe_frames=%u ref_corr_q15=%lu mic1=%lu/%lu mic2=%lu/%lu diff=%lu/%lu",
                         (unsigned long)arm_context.mode_token,
                         (unsigned long)arm_context.speaker_generation,
                         (unsigned)probe_policy.consecutive_safe_frames,
                         (unsigned long)filter_result.reference_correlation_q15,
                         (unsigned long)filter_result.mic1_avg_abs,
                         (unsigned long)filter_result.mic1_peak,
                         (unsigned long)filter_result.mic2_avg_abs,
                         (unsigned long)filter_result.mic2_peak,
                         (unsigned long)filter_result.diff_avg_abs,
                         (unsigned long)filter_result.diff_peak);
            }
        }

        if (noise_floor_avg_abs == 0U) {
            noise_floor_avg_abs = frame_avg_abs > 0U ? frame_avg_abs : 1U;
        } else if (frame_avg_abs <= noise_floor_avg_abs * 2U + 80U) {
            noise_floor_avg_abs =
                (noise_floor_avg_abs * 31U + frame_avg_abs) / 32U;
            if (noise_floor_avg_abs == 0U) {
                noise_floor_avg_abs = 1U;
            }
        }
        const bool voice_like = nuwax_wake_voice_frame_is_voice_like(
            frame_avg_abs, frame_peak, noise_floor_avg_abs);
        if (voice_like) {
            if (voice_span_started_ms == 0U ||
                nuwax_wake_voice_tail_expired(feed_ms,
                                              voice_last_seen_ms)) {
                voice_span_started_ms = feed_ms;
                voice_signal_avg_abs_max = 0U;
            }
            voice_last_seen_ms = feed_ms;
            if (frame_avg_abs > voice_signal_avg_abs_max) {
                voice_signal_avg_abs_max = frame_avg_abs;
            }
        } else if (nuwax_wake_voice_tail_expired(feed_ms,
                                                 voice_last_seen_ms)) {
            voice_span_started_ms = 0U;
            voice_last_seen_ms = 0U;
            voice_signal_avg_abs_max = 0U;
        }
#if CONFIG_NUWAX_WAKE_WORD_LOG_INTERVAL_FRAMES > 0
        if ((frames % CONFIG_NUWAX_WAKE_WORD_LOG_INTERVAL_FRAMES) == 0) {
            ESP_LOGI(TAG,
                     "wake mic level frames=%lu avg_abs=%lu peak=%lu",
                     (unsigned long)frames,
                     (unsigned long)frame_avg_abs,
                     (unsigned long)frame_peak);
        }
#endif

        if ((uint32_t)(feed_ms - last_health_log_ms) >=
            NUWAX_WAKE_WORD_HEALTH_LOG_MS) {
            detector_lock();
            s_detector.partial_reads = partial_reads;
            s_detector.read_timeouts = read_timeouts;
            s_detector.input_gaps = input_gaps;
            s_detector.max_feed_gap_ms = max_feed_gap_ms;
            s_detector.weak_rejections = weak_rejections;
            detector_unlock();
            const uint32_t inference_avg_us = inference_window_calls > 0U
                                                  ? (uint32_t)(
                                                        inference_window_us /
                                                        inference_window_calls)
                                                  : 0U;
            ESP_LOGI(TAG,
                     "wake input health frames=%lu partial_reads=%lu timeouts=%lu input_gaps=%lu delayed_feeds=%lu max_feed_gap_ms=%lu avg_abs=%lu peak=%lu noise_floor=%lu dc_mean=%ld clipped=%lu/%lu voice_span_ms=%lu weak_rejections=%lu inference_avg_us=%lu inference_max_us=%lu cooperative_blocks=%lu total=%lu max_cooperate_gap_ms=%lu cooperate_queue_high=%lu partial_reads_dropped=0",
                     (unsigned long)frames,
                     (unsigned long)partial_reads,
                     (unsigned long)read_timeouts,
                     (unsigned long)input_gaps,
                     (unsigned long)delayed_feeds,
                     (unsigned long)max_feed_gap_ms,
                     (unsigned long)frame_avg_abs,
                     (unsigned long)frame_peak,
                     (unsigned long)noise_floor_avg_abs,
                     (long)frame_dc_mean,
                     (unsigned long)window_clipped_samples,
                     (unsigned long)window_total_samples,
                     (unsigned long)(voice_span_started_ms == 0U
                                         ? 0U
                                         : feed_ms - voice_span_started_ms +
                                               (uint32_t)((sample_count * 1000U) /
                                                          detector_sample_rate)),
                     (unsigned long)weak_rejections,
                     (unsigned long)inference_avg_us,
                     (unsigned long)inference_window_max_us,
                     (unsigned long)cooperative_blocks_window,
                     (unsigned long)cooperative_blocks,
                     (unsigned long)max_cooperate_gap_ms,
                     (unsigned long)cooperate_queue_high);
            if (playback_capture) {
                ESP_LOGI(TAG,
                         "playback wake capture health ready_depth=%lu free_depth=%lu queue_high_water=%lu queue_drops=%lu empty_waits=%lu invariant_faults=%lu partial_reads=%lu timeouts=%lu",
                         (unsigned long)uxQueueMessagesWaiting(
                             playback_capture->ready_chunks),
                         (unsigned long)uxQueueMessagesWaiting(
                             playback_capture->free_chunks),
                         (unsigned long)atomic_load_explicit(
                             &playback_capture->queue_high_water,
                             memory_order_relaxed),
                         (unsigned long)atomic_load_explicit(
                             &playback_capture->queue_drops,
                             memory_order_relaxed),
                         (unsigned long)atomic_load_explicit(
                             &playback_capture->queue_empty_waits,
                             memory_order_relaxed),
                         (unsigned long)atomic_load_explicit(
                             &playback_capture->queue_invariant_faults,
                             memory_order_relaxed),
                         (unsigned long)atomic_load_explicit(
                             &playback_capture->partial_reads,
                             memory_order_relaxed),
                         (unsigned long)atomic_load_explicit(
                             &playback_capture->read_timeouts,
                             memory_order_relaxed));
            }
            inference_window_us = 0U;
            inference_window_calls = 0U;
            inference_window_max_us = 0U;
            cooperative_blocks_window = 0U;
            max_cooperate_gap_ms = 0U;
            cooperate_queue_high = 0U;
            last_health_log_ms = feed_ms;
        }

        const int64_t inference_started_us = esp_timer_get_time();
        bool detected_now = false;
        bool action_eligible = false;
        uint32_t detected_probability_q1000 = 0U;
        nuwax_wake_interrupt_filter_result_t event_filter = filter_result;
        const char *word = NULL;
        char detected_word[64] = {0};
        int detected = 0;
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
        if (s_detector.mn_iface && s_detector.mn_data) {
            esp_mn_state_t mn_state = s_detector.mn_iface->detect(s_detector.mn_data, samples);
            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *mn_result = s_detector.mn_iface->get_results(s_detector.mn_data);
                if (mn_result && mn_result->num > 0) {
                    const uint32_t probability_q1000 =
                        mn_result->prob[0] <= 0.0f
                            ? 0U
                            : (uint32_t)(mn_result->prob[0] * 1000.0f + 0.5f);
                    const uint32_t voice_span_ms =
                        voice_span_started_ms == 0U
                            ? 0U
                            : feed_ms - voice_span_started_ms +
                                  (uint32_t)((sample_count * 1000U) /
                                             detector_sample_rate);
                    detected_probability_q1000 = probability_q1000;
                    if (playback_interrupt_mode) {
                        const bool recent_nearfield =
                            last_nearfield_safe_ms != 0U &&
                            (uint32_t)(feed_ms - last_nearfield_safe_ms) <=
                                NUWAX_WAKE_INTERRUPT_NEARFIELD_MAX_AGE_MS;
                        if (recent_nearfield) {
                            event_filter = last_safe_filter_result;
                        }
                        action_eligible =
                            recent_nearfield && event_filter.reference_available &&
                            event_filter.nearfield_safe &&
                            probability_q1000 >=
                                (uint32_t)CONFIG_NUWAX_WAKE_WORD_INTERRUPT_THRESHOLD_PERCENT *
                                    10U;
                        detected_now = CONFIG_NUWAX_WAKE_WORD_INTERRUPT_SHADOW_ONLY ||
                                       action_eligible;
                    } else {
                        action_eligible = nuwax_wake_detection_accept(
                            probability_q1000,
                            noise_floor_avg_abs,
                            voice_signal_avg_abs_max,
                            voice_span_ms,
                            window_clipped_samples,
                            window_total_samples);
                        detected_now = action_eligible;
                    }
                    detected = mn_result->command_id[0];
                    /* All variants share command_id=1. Preserve the actual
                     * phrase when MultiNet exposes it, with the primary
                     * configured phrase as a stable fallback. */
                    detector_lock();
                    strlcpy(detected_word,
                            mn_result->string[0]
                                ? mn_result->string
                                : s_detector.config.variants[0],
                            sizeof(detected_word));
                    detector_unlock();
                    word = detected_word;
                    if (playback_interrupt_mode) {
                        ESP_LOGI(TAG,
                                 "playback wake candidate command_id=%d prob=%.3f eligible=%d shadow=%d ref_available=%d ref_active=%d ref_corr_q15=%lu echo=%d nearfield=%d mic1=%lu/%lu mic2=%lu/%lu diff=%lu/%lu token=%lu generation=%lu",
                                 detected,
                                 (double)mn_result->prob[0],
                                 action_eligible ? 1 : 0,
                                 CONFIG_NUWAX_WAKE_WORD_INTERRUPT_SHADOW_ONLY ? 1 : 0,
                                 event_filter.reference_available ? 1 : 0,
                                 event_filter.reference_active ? 1 : 0,
                                 (unsigned long)event_filter.reference_correlation_q15,
                                 event_filter.echo_likely ? 1 : 0,
                                 event_filter.nearfield_safe ? 1 : 0,
                                 (unsigned long)event_filter.mic1_avg_abs,
                                 (unsigned long)event_filter.mic1_peak,
                                 (unsigned long)event_filter.mic2_avg_abs,
                                 (unsigned long)event_filter.mic2_peak,
                                 (unsigned long)event_filter.diff_avg_abs,
                                 (unsigned long)event_filter.diff_peak,
                                 (unsigned long)arm_context.mode_token,
                                 (unsigned long)arm_context.speaker_generation);
                        if (!detected_now) {
                            weak_rejections++;
                        }
                    } else if (detected_now) {
                        const char *acceptance_path =
                            probability_q1000 >=
                                    NUWAX_WAKE_STRONG_THRESHOLD_Q1000
                                ? "strong"
                                : (voice_span_ms <
                                           NUWAX_WAKE_WEAK_MIN_VOICE_SPAN_MS
                                       ? "fast"
                                       : "normal");
                        ESP_LOGI(TAG,
                                 "custom wake word accepted command_id=%d prob=%.3f path=%s voice_span_ms=%lu signal_avg_max=%lu noise_floor=%lu text=%s raw=%s",
                                 detected,
                                 (double)mn_result->prob[0],
                                 acceptance_path,
                                 (unsigned long)voice_span_ms,
                                 (unsigned long)voice_signal_avg_abs_max,
                                 (unsigned long)noise_floor_avg_abs,
                                 mn_result->string,
                                 mn_result->raw_string);
                    } else {
                        weak_rejections++;
                        ESP_LOGW(TAG,
                                 "custom wake word weak detection rejected command_id=%d prob=%.3f voice_span_ms=%lu signal_avg_max=%lu noise_floor=%lu clipped=%lu/%lu",
                                 detected,
                                 (double)mn_result->prob[0],
                                 (unsigned long)voice_span_ms,
                                 (unsigned long)voice_signal_avg_abs_max,
                                 (unsigned long)noise_floor_avg_abs,
                                 (unsigned long)window_clipped_samples,
                                 (unsigned long)window_total_samples);
                    }
                }
                s_detector.mn_iface->clean(s_detector.mn_data);
                voice_span_started_ms = 0U;
                voice_last_seen_ms = 0U;
                voice_signal_avg_abs_max = 0U;
                window_clipped_samples = 0U;
                window_total_samples = 0U;
            } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
                s_detector.mn_iface->clean(s_detector.mn_data);
                voice_span_started_ms = 0U;
                voice_last_seen_ms = 0U;
                voice_signal_avg_abs_max = 0U;
                window_clipped_samples = 0U;
                window_total_samples = 0U;
            }
        }
#else
        if (s_detector.iface && s_detector.model_data) {
            detected = s_detector.iface->detect(s_detector.model_data, samples);
            if (detected > 0) {
                detected_now = true;
                word = s_detector.iface->get_word_name(s_detector.model_data, detected);
            }
        }
#endif
        const uint32_t inference_elapsed_us =
            (uint32_t)(esp_timer_get_time() - inference_started_us);
        inference_window_us += inference_elapsed_us;
        inference_window_calls++;
        if (inference_elapsed_us > inference_window_max_us) {
            inference_window_max_us = inference_elapsed_us;
        }
        if (detected_now) {
            if (!word || !word[0]) {
                word = s_detector.active_model;
            }
            nuwax_wake_word_callback_t callback = NULL;
            void *callback_ctx = NULL;
            uint32_t detections = 0U;
            detector_lock();
            s_detector.detections++;
            detections = s_detector.detections;
            strlcpy(s_detector.last_wake_word, word, sizeof(s_detector.last_wake_word));
            callback = s_detector.callback;
            callback_ctx = s_detector.user_ctx;
            detector_unlock();
            ESP_LOGI(TAG,
                     "wake word detected backend=%s mode=%d token=%lu index=%d word=%s detections=%lu eligible=%d",
                     s_detector.backend,
                     (int)arm_context.mode,
                     (unsigned long)arm_context.mode_token,
                     detected,
                     word,
                     (unsigned long)detections,
                     action_eligible ? 1 : 0);
            if (callback) {
                nuwax_wake_word_result_t result = {
                    .arm = arm_context,
                    .probability_q1000 = detected_probability_q1000,
                    .detected_ms = feed_ms,
                    .detected_us = esp_timer_get_time(),
                    .reference_generation = reference_reader.generation,
                    .reference_correlation_q15 =
                        event_filter.reference_correlation_q15,
                    .mic1_avg_abs = event_filter.mic1_avg_abs,
                    .mic1_peak = event_filter.mic1_peak,
                    .mic2_avg_abs = event_filter.mic2_avg_abs,
                    .mic2_peak = event_filter.mic2_peak,
                    .diff_avg_abs = event_filter.diff_avg_abs,
                    .diff_peak = event_filter.diff_peak,
                    .reference_available = event_filter.reference_available,
                    .reference_active = event_filter.reference_active,
                    .echo_likely = event_filter.echo_likely,
                    .nearfield_safe = event_filter.nearfield_safe,
                    .probe_requested = false,
                    .action_eligible = action_eligible,
                };
                strlcpy(result.wake_word, word, sizeof(result.wake_word));
                callback(&result, callback_ctx);
            }
            break;
        }

        const uint32_t inference_finished_ms = detector_monotonic_ms();
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
        if (!lifetime_refresh_deferred && mn_created_ms != 0U &&
            (uint32_t)(inference_finished_ms - mn_created_ms) >=
                NUWAX_WAKE_MULTINET_MAX_LIFETIME_MS) {
            bool refresh_requested = false;
            detector_lock();
            if (s_detector.mn_generation == mn_generation &&
                !s_detector.mn_rebuild_pending &&
                !s_detector.mn_maintenance_due) {
                s_detector.mn_maintenance_due = true;
                refresh_requested = true;
            }
            detector_unlock();
            lifetime_refresh_deferred = true;
            if (refresh_requested) {
                ESP_LOGI(TAG,
                         "MultiNet backend maintenance due generation=%lu age_ms=%lu frames=%lu; healthy backend retained and refresh deferred",
                         (unsigned long)mn_generation,
                         (unsigned long)(inference_finished_ms - mn_created_ms),
                         (unsigned long)frames);
            }
        }
#endif
        if (nuwax_wake_input_cooperate_due(
                playback_interrupt_mode,
                last_cooperate_ms,
                inference_finished_ms,
                NUWAX_WAKE_INFERENCE_COOPERATE_INTERVAL_MS)) {
            const uint32_t cooperate_gap_ms =
                (uint32_t)(inference_finished_ms - last_cooperate_ms);
            if (cooperate_gap_ms > max_cooperate_gap_ms) {
                max_cooperate_gap_ms = cooperate_gap_ms;
            }
            if (playback_capture) {
                const uint32_t ready_depth = (uint32_t)uxQueueMessagesWaiting(
                    playback_capture->ready_chunks);
                if (ready_depth > cooperate_queue_high) {
                    cooperate_queue_high = ready_depth;
                }
            }
            /* taskYIELD() cannot schedule IDLE1 below this priority. A single
             * blocked tick once per second gives the watchdog idle task a
             * deterministic window; the 180 ms capture queue absorbs the
             * bounded pause without changing PCM order or recognizer input. */
            vTaskDelay(1);
            last_cooperate_ms = detector_monotonic_ms();
            cooperative_blocks++;
            cooperative_blocks_window++;
        }
    }

done:
    atomic_store_explicit(&s_detector.ready, false, memory_order_release);
    wake_capture_stop(&playback_capture);
    if (mic_ready) {
        (void)nuwax_mic_input_deinit(&mic);
    }
    detector_lock();
    s_detector.partial_reads = partial_reads;
    s_detector.read_timeouts = read_timeouts;
    s_detector.input_gaps = input_gaps;
    s_detector.max_feed_gap_ms = max_feed_gap_ms;
    s_detector.weak_rejections = weak_rejections;
    detector_unlock();
    ESP_LOGI(TAG,
             "wake input final frames=%lu partial_reads=%lu timeouts=%lu input_gaps=%lu delayed_feeds=%lu max_feed_gap_ms=%lu cooperative_blocks=%lu stack_min_free=%u stack_bytes=%u partial_reads_dropped=0",
             (unsigned long)frames,
             (unsigned long)partial_reads,
             (unsigned long)read_timeouts,
             (unsigned long)input_gaps,
             (unsigned long)delayed_feeds,
             (unsigned long)max_feed_gap_ms,
             (unsigned long)cooperative_blocks,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)CONFIG_NUWAX_WAKE_WORD_TASK_STACK_BYTES);
    heap_caps_free(reference_secondary);
    heap_caps_free(reference_primary);
    heap_caps_free(mic2_samples);
    free(samples);

    /* EXITING is the reaper handshake: no task-owned resource or lock may be
     * touched after publishing it. The owner may delete this task as soon as
     * it observes the following self-suspended state. */
    detector_lock();
    s_detector.task_state = NUWAX_WAKE_TASK_EXITING;
    detector_unlock();
    for (;;) {
        vTaskSuspend(NULL);
    }
}
#endif

esp_err_t nuwax_wake_word_detector_init(nuwax_wake_word_callback_t callback, void *user_ctx) {
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
    if (s_detector.initialized) {
        detector_lock();
        if (s_detector.deinitializing) {
            detector_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        s_detector.callback = callback;
        s_detector.user_ctx = user_ctx;
        detector_unlock();
        return ESP_OK;
    }
    memset(&s_detector, 0, sizeof(s_detector));
    atomic_init(&s_detector.stop_requested, false);
    atomic_init(&s_detector.ready, false);
    s_detector.task_state = NUWAX_WAKE_TASK_STOPPED;
    s_detector.lock = xSemaphoreCreateMutex();
    if (!s_detector.lock) {
        return ESP_ERR_NO_MEM;
    }
    s_detector.callback = callback;
    s_detector.user_ctx = user_ctx;
    esp_err_t result = ESP_OK;

    s_detector.models = esp_srmodel_init(CONFIG_NUWAX_WAKE_WORD_MODEL_PARTITION);
    if (!s_detector.models || s_detector.models->num <= 0) {
        ESP_LOGE(TAG,
                 "ESP-SR model init failed partition=%s models=%p num=%d",
                 CONFIG_NUWAX_WAKE_WORD_MODEL_PARTITION,
                 s_detector.models,
                 s_detector.models ? s_detector.models->num : -999);
        result = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    for (int i = 0; i < s_detector.models->num; i++) {
        ESP_LOGI(TAG, "ESP-SR model[%d]=%s", i, s_detector.models->model_name[i]);
    }

#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    strlcpy(s_detector.backend, "multinet", sizeof(s_detector.backend));
    select_multinet_model(s_detector.active_model, sizeof(s_detector.active_model));
    if (!s_detector.active_model[0]) {
        ESP_LOGE(TAG,
                 "No MultiNet model found in partition=%s language=%s",
                 CONFIG_NUWAX_WAKE_WORD_MODEL_PARTITION,
                 CONFIG_NUWAX_CUSTOM_WAKE_WORD_LANGUAGE);
        result = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    s_detector.mn_iface = esp_mn_handle_from_name(s_detector.active_model);
    if (!s_detector.mn_iface) {
        ESP_LOGE(TAG, "MultiNet handle not found model=%s", s_detector.active_model);
        result = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    s_detector.mn_data = s_detector.mn_iface->create(s_detector.active_model,
                                                     CONFIG_NUWAX_CUSTOM_WAKE_WORD_DURATION_MS);
    if (!s_detector.mn_data) {
        ESP_LOGE(TAG, "MultiNet create failed model=%s", s_detector.active_model);
        result = ESP_FAIL;
        goto fail;
    }
    float threshold = (float)CONFIG_NUWAX_CUSTOM_WAKE_WORD_THRESHOLD_PERCENT / 100.0f;
    if (threshold < 0.01f) {
        threshold = 0.01f;
    } else if (threshold > 0.99f) {
        threshold = 0.99f;
    }
    s_detector.mn_iface->set_det_threshold(s_detector.mn_data, threshold);
    nuwax_wake_word_config_t configured = {0};
    nuwax_wake_word_config_t factory_config = {0};
    bool used_legacy_persistence = false;
    const esp_err_t persisted_err =
        load_persisted_config(&configured, &used_legacy_persistence);
    const bool factory_config_valid = nuwax_wake_word_config_from_legacy(
        CONFIG_NUWAX_CUSTOM_WAKE_WORD_COMMAND,
        &factory_config);
    if (persisted_err != ESP_OK && factory_config_valid) {
        configured = factory_config;
    } else if (persisted_err != ESP_OK) {
        ESP_LOGE(TAG, "custom wake word command is invalid");
        result = ESP_ERR_INVALID_ARG;
        goto fail;
    }
    esp_err_t command_err = configure_multinet_commands(&configured);
    if (command_err != ESP_OK) {
        result = command_err;
        goto fail;
    }
    s_detector.feed_chunksize = s_detector.mn_iface->get_samp_chunksize(s_detector.mn_data);
    s_detector.sample_rate = s_detector.mn_iface->get_samp_rate(s_detector.mn_data);
    if (s_detector.feed_chunksize <= 0 ||
        s_detector.feed_chunksize > CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME ||
        s_detector.sample_rate <= 0) {
        ESP_LOGE(TAG,
                 "MultiNet returned invalid audio contract chunksize=%d sample_rate=%d",
                 s_detector.feed_chunksize,
                 s_detector.sample_rate);
        result = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    s_detector.config = configured;
    s_detector.command_configured = true;
    s_detector.mn_generation = 1U;
    s_detector.mn_created_ms = detector_monotonic_ms();
    s_detector.mn_arm_cycles = 0U;
    s_detector.mn_rebuild_pending = false;
    s_detector.mn_maintenance_due = false;
    s_detector.initialized = true;
    ESP_LOGI(TAG,
             "MultiNet custom wake initialized model=%s command=%s variants=%u display=%s threshold=%.2f sample_rate=%d feed_chunksize=%d partition=%s",
             s_detector.active_model,
             s_detector.config.variants[0],
             (unsigned)s_detector.config.count,
             CONFIG_NUWAX_CUSTOM_WAKE_WORD_DISPLAY,
             (double)threshold,
             s_detector.sample_rate,
             s_detector.feed_chunksize,
             CONFIG_NUWAX_WAKE_WORD_MODEL_PARTITION);
    ESP_LOGI(TAG,
             "wake high-recall policy strong=%.3f weak=%.3f fast_span=%u..%ums fast_snr_q8=%u normal_span=%u..%ums normal_snr_q8=%u voice_peak=%u tail_hold_ms=%u",
             (double)NUWAX_WAKE_STRONG_THRESHOLD_Q1000 / 1000.0,
             (double)NUWAX_WAKE_WEAK_THRESHOLD_Q1000 / 1000.0,
             (unsigned)NUWAX_WAKE_FAST_MIN_VOICE_SPAN_MS,
             (unsigned)(NUWAX_WAKE_WEAK_MIN_VOICE_SPAN_MS - 1U),
             (unsigned)NUWAX_WAKE_FAST_MIN_SNR_RATIO_Q8,
             (unsigned)NUWAX_WAKE_WEAK_MIN_VOICE_SPAN_MS,
             (unsigned)NUWAX_WAKE_WEAK_MAX_VOICE_SPAN_MS,
             (unsigned)NUWAX_WAKE_WEAK_MIN_SNR_RATIO_Q8,
             (unsigned)NUWAX_WAKE_VOICE_MIN_PEAK,
             (unsigned)NUWAX_WAKE_VOICE_TAIL_HOLD_MS);
    if (used_legacy_persistence) {
        const esp_err_t migrate_err = persist_config(&configured);
        if (migrate_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "wake word NVS migration deferred err=%s",
                     esp_err_to_name(migrate_err));
        }
    }
    return ESP_OK;
#else
    strlcpy(s_detector.backend, "wakenet", sizeof(s_detector.backend));
    select_wakenet_model(s_detector.active_model, sizeof(s_detector.active_model));
    if (!s_detector.active_model[0]) {
        ESP_LOGE(TAG,
                 "No WakeNet model found in partition=%s requested=%s",
                 CONFIG_NUWAX_WAKE_WORD_MODEL_PARTITION,
                 CONFIG_NUWAX_WAKE_WORD_MODEL_NAME[0] ? CONFIG_NUWAX_WAKE_WORD_MODEL_NAME : "-");
        result = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    s_detector.iface = esp_wn_handle_from_name(s_detector.active_model);
    if (!s_detector.iface) {
        ESP_LOGE(TAG, "WakeNet handle not found model=%s", s_detector.active_model);
        result = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    s_detector.model_data = s_detector.iface->create(s_detector.active_model, DET_MODE_95);
    if (!s_detector.model_data) {
        ESP_LOGE(TAG, "WakeNet create failed model=%s", s_detector.active_model);
        result = ESP_FAIL;
        goto fail;
    }
    s_detector.feed_chunksize = s_detector.iface->get_samp_chunksize(s_detector.model_data);
    s_detector.sample_rate = s_detector.iface->get_samp_rate(s_detector.model_data);
    s_detector.initialized = true;
    ESP_LOGI(TAG,
             "WakeNet initialized model=%s sample_rate=%d feed_chunksize=%d partition=%s",
             s_detector.active_model,
             s_detector.sample_rate,
             s_detector.feed_chunksize,
             CONFIG_NUWAX_WAKE_WORD_MODEL_PARTITION);
    return ESP_OK;
#endif

fail:
    ESP_LOGE(TAG,
             "wake detector init rollback err=%s",
             esp_err_to_name(result));
    detector_release_resources();
    return result;
#else
    (void)callback;
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t nuwax_wake_word_detector_start_with_context(
    const nuwax_wake_arm_context_t *context) {
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
    if (!context ||
        (context->mode != NUWAX_WAKE_MODE_SLEEP &&
         context->mode != NUWAX_WAKE_MODE_PLAYBACK_INTERRUPT) ||
        context->mode_token == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    if (context->mode == NUWAX_WAKE_MODE_PLAYBACK_INTERRUPT) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    (void)detector_reap_exited_task();

    detector_lock();
    if (!s_detector.initialized || s_detector.deinitializing) {
        detector_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_detector.task_state == NUWAX_WAKE_TASK_RUNNING) {
        detector_unlock();
        return ESP_OK;
    }
    if (s_detector.task_state != NUWAX_WAKE_TASK_STOPPED || s_detector.task) {
        ESP_LOGW(TAG,
                 "wake detector start rejected while task state=%s handle=%p",
                 detector_task_state_name(s_detector.task_state),
                 s_detector.task);
        detector_unlock();
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    const bool backend_missing =
        !s_detector.mn_iface || !s_detector.mn_data ||
        !s_detector.command_configured;
    if (!s_detector.mn_iface || s_detector.config.count == 0U) {
        detector_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    /* Do not turn an advisory lifetime/arm-cycle deadline into a synchronous
     * model rebuild here. This function is on the sleep wake-up critical path;
     * a healthy backend can be cleaned and re-armed without a 5-6 second gap.
     * Hard rebuilds remain mandatory when the backend is missing or a prior
     * rebuild/configuration operation failed. */
    if (backend_missing || s_detector.mn_rebuild_pending) {
        const char *rebuild_reason = backend_missing
                                         ? "backend_missing"
                                         : "worker_requested";
        const esp_err_t rebuild_err = rebuild_multinet_backend_locked(
            &s_detector.config, rebuild_reason);
        if (rebuild_err != ESP_OK) {
            detector_unlock();
            return rebuild_err;
        }
    }
#else
    if (!s_detector.iface || !s_detector.model_data) {
        detector_unlock();
        return ESP_ERR_INVALID_STATE;
    }
#endif
    atomic_store_explicit(&s_detector.stop_requested,
                          false,
                          memory_order_release);
    atomic_store_explicit(&s_detector.ready, false, memory_order_release);
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    /* Each arm is a new recognition transaction. Half a command retained
     * across a sleep/playback handoff must never complete in the new mode. */
    s_detector.mn_iface->clean(s_detector.mn_data);
#endif
    s_detector.arm_context = *context;
    if (!s_detector.task_tcb) {
        s_detector.task_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_detector.task_stack) {
        s_detector.task_stack = heap_caps_malloc(CONFIG_NUWAX_WAKE_WORD_TASK_STACK_BYTES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_detector.task_tcb || !s_detector.task_stack) {
        ESP_LOGE(TAG,
                 "wake detector task allocation failed stack=%d tcb=%p stack_ptr=%p free_internal=%u largest_internal=%u free_psram=%u largest_psram=%u",
                 CONFIG_NUWAX_WAKE_WORD_TASK_STACK_BYTES,
                 s_detector.task_tcb,
                 s_detector.task_stack,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_detector.task_tcb) {
            heap_caps_free(s_detector.task_tcb);
        }
        if (s_detector.task_stack) {
            heap_caps_free(s_detector.task_stack);
        }
        s_detector.task_tcb = NULL;
        s_detector.task_stack = NULL;
        detector_unlock();
        return ESP_ERR_NO_MEM;
    }
    s_detector.task_state = NUWAX_WAKE_TASK_RUNNING;
    s_detector.task = xTaskCreateStaticPinnedToCore(
        wake_word_task,
        "nuwax_wake",
        CONFIG_NUWAX_WAKE_WORD_TASK_STACK_BYTES,
        NULL,
        CONFIG_NUWAX_WAKE_WORD_TASK_PRIORITY,
        s_detector.task_stack,
        s_detector.task_tcb,
        NUWAX_WAKE_AUDIO_TASK_CORE);
    if (!s_detector.task) {
        s_detector.task_state = NUWAX_WAKE_TASK_STOPPED;
        heap_caps_free(s_detector.task_tcb);
        heap_caps_free(s_detector.task_stack);
        s_detector.task_tcb = NULL;
        s_detector.task_stack = NULL;
        detector_unlock();
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    s_detector.mn_arm_cycles++;
    if (s_detector.mn_arm_cycles >= NUWAX_WAKE_MULTINET_MAX_ARM_CYCLES) {
        s_detector.mn_maintenance_due = true;
    }
#endif
    ESP_LOGI(TAG,
             "wake detector task started stack=%d priority=%d core=%u backend_generation=%lu backend_arm_cycles=%lu maintenance_due=%d free_internal=%u largest_internal=%u free_psram=%u largest_psram=%u",
             CONFIG_NUWAX_WAKE_WORD_TASK_STACK_BYTES,
             CONFIG_NUWAX_WAKE_WORD_TASK_PRIORITY,
             (unsigned)NUWAX_WAKE_AUDIO_TASK_CORE,
#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
             (unsigned long)s_detector.mn_generation,
             (unsigned long)s_detector.mn_arm_cycles,
             s_detector.mn_maintenance_due ? 1 : 0,
#else
             0UL,
             0UL,
             0,
#endif
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    detector_unlock();
    return ESP_OK;
#else
    (void)context;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t nuwax_wake_word_detector_start(void) {
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
    nuwax_wake_arm_context_t context = {
        .mode = NUWAX_WAKE_MODE_SLEEP,
    };
    detector_lock();
    context.mode_token = s_detector.arm_context.mode_token + 1U;
    if (context.mode_token == 0U) {
        context.mode_token = 1U;
    }
    detector_unlock();
    return nuwax_wake_word_detector_start_with_context(&context);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void nuwax_wake_word_detector_request_stop(void) {
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
    detector_lock();
    if (s_detector.task_state == NUWAX_WAKE_TASK_RUNNING) {
        atomic_store_explicit(&s_detector.ready, false, memory_order_release);
        atomic_store_explicit(&s_detector.stop_requested,
                              true,
                              memory_order_release);
        s_detector.task_state = NUWAX_WAKE_TASK_STOP_REQUESTED;
    }
    detector_unlock();
#endif
}

esp_err_t nuwax_wake_word_detector_stop_with_timeout(uint32_t timeout_ms) {
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
    if (timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    nuwax_wake_word_detector_request_stop();
    detector_lock();
    const bool called_from_worker =
        s_detector.task && s_detector.task == xTaskGetCurrentTaskHandle();
    detector_unlock();
    if (called_from_worker) {
        return ESP_ERR_NOT_FINISHED;
    }
    const uint32_t wait_started_ms = detector_monotonic_ms();
    while (nuwax_wake_word_detector_is_running()) {
        if ((uint32_t)(detector_monotonic_ms() - wait_started_ms) >=
            timeout_ms) {
            detector_lock();
            const nuwax_wake_task_state_t state = s_detector.task_state;
            detector_unlock();
            ESP_LOGE(TAG,
                     "wake detector stop timed out state=%s timeout_ms=%lu",
                     detector_task_state_name(state),
                     (unsigned long)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
#else
    (void)timeout_ms;
    return ESP_OK;
#endif
}

void nuwax_wake_word_detector_stop(void) {
    const esp_err_t err = nuwax_wake_word_detector_stop_with_timeout(
        NUWAX_WAKE_WORD_STOP_TIMEOUT_MS);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGE(TAG, "wake detector bounded stop failed: %s",
                 esp_err_to_name(err));
    }
}

bool nuwax_wake_word_detector_is_running(void) {
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
    (void)detector_reap_exited_task();
    detector_lock();
    const bool running = s_detector.task_state != NUWAX_WAKE_TASK_STOPPED;
    detector_unlock();
    return running;
#else
    return false;
#endif
}

bool nuwax_wake_word_detector_is_ready(void) {
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
    (void)detector_reap_exited_task();
    detector_lock();
    const bool ready =
        s_detector.task_state == NUWAX_WAKE_TASK_RUNNING &&
        atomic_load_explicit(&s_detector.ready, memory_order_acquire);
    detector_unlock();
    return ready;
#else
    return false;
#endif
}

void nuwax_wake_word_detector_deinit(void) {
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
    detector_lock();
    const bool called_from_worker =
        s_detector.task && s_detector.task == xTaskGetCurrentTaskHandle();
    if (!called_from_worker) {
        s_detector.deinitializing = true;
    }
    detector_unlock();
    if (called_from_worker) {
        nuwax_wake_word_detector_request_stop();
        ESP_LOGE(TAG,
                 "wake detector deinit requested from worker callback; deferred to owner");
        return;
    }
    const esp_err_t stop_err = nuwax_wake_word_detector_stop_with_timeout(
        NUWAX_WAKE_WORD_STOP_TIMEOUT_MS);
    if (stop_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "wake detector deinit retained resources after stop failure: %s",
                 esp_err_to_name(stop_err));
        return;
    }
    detector_lock();
    const bool safe_to_release =
        s_detector.task_state == NUWAX_WAKE_TASK_STOPPED && !s_detector.task;
    detector_unlock();
    if (!safe_to_release) {
        ESP_LOGE(TAG,
                 "wake detector deinit refused because task is not stopped");
        return;
    }
    detector_release_resources();
#endif
}

esp_err_t nuwax_wake_word_detector_set_command(const char *command,
                                               bool persist) {
#if !CONFIG_NUWAX_WAKE_WORD_ENABLE || !CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    (void)command;
    (void)persist;
    return ESP_ERR_NOT_SUPPORTED;
#else
    nuwax_wake_word_config_t config = {0};
    if (!nuwax_wake_word_config_from_legacy(command, &config)) {
        return ESP_ERR_INVALID_ARG;
    }
    return nuwax_wake_word_detector_set_config(&config, persist);
#endif
}

#if CONFIG_NUWAX_WAKE_WORD_ENABLE && CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
static esp_err_t apply_wake_word_config(
    const nuwax_wake_word_config_t *config,
    nuwax_wake_persist_action_t persist_action) {
    nuwax_wake_word_config_t normalized = {0};
    if (!nuwax_wake_word_config_normalize(config, &normalized)) {
        return ESP_ERR_INVALID_ARG;
    }
    detector_lock();
    const bool initialized = s_detector.initialized && !s_detector.deinitializing;
    const bool was_running = initialized &&
                             (s_detector.task_state != NUWAX_WAKE_TASK_STOPPED ||
                              s_detector.task != NULL);
    nuwax_wake_word_config_t previous = s_detector.config;
    nuwax_wake_arm_context_t restart_context = s_detector.arm_context;
    detector_unlock();
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    nuwax_wake_word_persisted_snapshot_t persisted_snapshot = {0};
    if (persist_action != NUWAX_WAKE_PERSIST_NONE) {
        const esp_err_t snapshot_err =
            capture_persisted_snapshot(&persisted_snapshot);
        if (snapshot_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "wake word update aborted: NVS snapshot failed err=%s",
                     esp_err_to_name(snapshot_err));
            return snapshot_err;
        }
    }

    if (nuwax_wake_word_configs_equal(&previous, &normalized)) {
        esp_err_t persist_err = ESP_OK;
        if (persist_action == NUWAX_WAKE_PERSIST_WRITE) {
            persist_err = persist_config(&normalized);
        } else if (persist_action == NUWAX_WAKE_PERSIST_ERASE) {
            persist_err = erase_persisted_config();
        }
        if (persist_err != ESP_OK &&
            persist_action != NUWAX_WAKE_PERSIST_NONE) {
            const esp_err_t restore_err =
                restore_persisted_snapshot(&persisted_snapshot);
            ESP_LOGE(TAG,
                     "wake word unchanged but NVS action failed err=%s restore=%s",
                     esp_err_to_name(persist_err),
                     esp_err_to_name(restore_err));
        }
        return persist_err;
    }

    if (was_running) {
        const esp_err_t stop_err =
            nuwax_wake_word_detector_stop_with_timeout(
                NUWAX_WAKE_WORD_STOP_TIMEOUT_MS);
        if (stop_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "wake word update aborted: detector stop failed err=%s",
                     esp_err_to_name(stop_err));
            return stop_err;
        }
        (void)detector_reap_exited_task();
    }

    detector_lock();
    const bool stopped = s_detector.task_state == NUWAX_WAKE_TASK_STOPPED &&
                         s_detector.task == NULL;
    detector_unlock();
    if (!stopped) {
        ESP_LOGE(TAG, "wake word update aborted: detector did not stop");
        return ESP_ERR_INVALID_STATE;
    }

    detector_lock();
    esp_err_t err = rebuild_multinet_backend_locked(
        &normalized, "command_update");
    if (err == ESP_OK) {
        s_detector.config = normalized;
        s_detector.command_configured = true;
    }
    detector_unlock();
    if (err != ESP_OK) {
        /* Rebuild removes the previous opaque decoder instance first. Restore
         * the old config before any restart so the detector can never run with
         * a missing backend or an empty command tree. */
        detector_lock();
        const esp_err_t restore_err = rebuild_multinet_backend_locked(
            &previous, "command_update_rollback");
        if (restore_err == ESP_OK) {
            s_detector.config = previous;
            s_detector.command_configured = true;
        } else {
            memset(&s_detector.config, 0, sizeof(s_detector.config));
            s_detector.command_configured = false;
        }
        detector_unlock();
        ESP_LOGW(TAG,
                 "wake word update rejected primary=%s variants=%u err=%s restore=%s",
                 normalized.variants[0],
                 (unsigned)normalized.count,
                 esp_err_to_name(err),
                 esp_err_to_name(restore_err));
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "wake word command restore failed; detector remains stopped");
            return restore_err;
        }
        if (was_running) {
            const esp_err_t restart_err =
                nuwax_wake_word_detector_start_with_context(&restart_context);
            if (restart_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "previous wake detector restart failed after rejected update err=%s",
                         esp_err_to_name(restart_err));
                return restart_err;
            }
        }
        return err;
    }

    if (persist_action == NUWAX_WAKE_PERSIST_WRITE) {
        err = persist_config(&normalized);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "wake word NVS persist failed err=%s; restoring previous config",
                     esp_err_to_name(err));
        }
    } else if (persist_action == NUWAX_WAKE_PERSIST_ERASE) {
        err = erase_persisted_config();
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "wake word NVS reset failed err=%s; restoring previous config",
                     esp_err_to_name(err));
        }
    }

    if (err == ESP_OK && was_running) {
        err = nuwax_wake_word_detector_start_with_context(&restart_context);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wake detector restart failed err=%s; restoring previous command",
                     esp_err_to_name(err));
        }
    }

    if (err != ESP_OK) {
        detector_lock();
        const esp_err_t restore_err = rebuild_multinet_backend_locked(
            &previous, "command_persist_rollback");
        if (restore_err == ESP_OK) {
            s_detector.config = previous;
            s_detector.command_configured = true;
        } else {
            memset(&s_detector.config, 0, sizeof(s_detector.config));
            s_detector.command_configured = false;
        }
        detector_unlock();
        if (persist_action != NUWAX_WAKE_PERSIST_NONE) {
            const esp_err_t persist_restore_err =
                restore_persisted_snapshot(&persisted_snapshot);
            if (persist_restore_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "previous wake word NVS snapshot restore failed err=%s",
                         esp_err_to_name(persist_restore_err));
            }
        }
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "previous wake command restore failed; detector remains stopped");
            return restore_err;
        }
        if (was_running) {
            const esp_err_t restart_err =
                nuwax_wake_word_detector_start_with_context(&restart_context);
            if (restart_err != ESP_OK) {
                ESP_LOGE(TAG, "previous wake detector restart failed err=%s",
                         esp_err_to_name(restart_err));
                return restart_err;
            }
        }
    }
    return err;
}
#endif

esp_err_t nuwax_wake_word_detector_set_config(
    const nuwax_wake_word_config_t *config,
    bool persist) {
#if !CONFIG_NUWAX_WAKE_WORD_ENABLE || !CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    (void)config;
    (void)persist;
    return ESP_ERR_NOT_SUPPORTED;
#else
    return apply_wake_word_config(config,
                                  persist ? NUWAX_WAKE_PERSIST_WRITE
                                          : NUWAX_WAKE_PERSIST_NONE);
#endif
}

esp_err_t nuwax_wake_word_detector_reset_to_default(void) {
#if !CONFIG_NUWAX_WAKE_WORD_ENABLE || !CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    return ESP_ERR_NOT_SUPPORTED;
#else
    nuwax_wake_word_config_t factory_config = {0};
    if (!nuwax_wake_word_config_from_legacy(
            CONFIG_NUWAX_CUSTOM_WAKE_WORD_COMMAND,
            &factory_config)) {
        return ESP_ERR_INVALID_ARG;
    }
    return apply_wake_word_config(&factory_config,
                                  NUWAX_WAKE_PERSIST_ERASE);
#endif
}

void nuwax_wake_word_detector_get_status(nuwax_wake_word_status_t *out_status) {
    if (!out_status) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
#if CONFIG_NUWAX_WAKE_WORD_ENABLE
    detector_lock();
    out_status->enabled = true;
    out_status->initialized = s_detector.initialized;
    out_status->detections = s_detector.detections;
    out_status->weak_rejections = s_detector.weak_rejections;
    out_status->partial_reads = s_detector.partial_reads;
    out_status->read_timeouts = s_detector.read_timeouts;
    out_status->input_gaps = s_detector.input_gaps;
    out_status->max_feed_gap_ms = s_detector.max_feed_gap_ms;
    out_status->feed_chunksize = s_detector.feed_chunksize;
    out_status->sample_rate = s_detector.sample_rate;
    strlcpy(out_status->active_model, s_detector.active_model, sizeof(out_status->active_model));
    strlcpy(out_status->active_command,
            s_detector.config.variants[0],
            sizeof(out_status->active_command));
    out_status->active_variant_count = s_detector.config.count;
    strlcpy(out_status->last_wake_word, s_detector.last_wake_word, sizeof(out_status->last_wake_word));
    strlcpy(out_status->backend, s_detector.backend, sizeof(out_status->backend));
    detector_unlock();
#endif
}
