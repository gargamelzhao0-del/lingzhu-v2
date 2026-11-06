#include "device_error_report.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "wake_word_detector.h"
#include "offline_audio_spool.h"
#include "nuwax_lvgl_port.h"

#define ERROR_REPORT_QUEUE_CAPACITY 16U
#define ERROR_REPORT_EVENT_ID_BYTES 48U
#define ERROR_REPORT_SEVERITY_BYTES 12U
#define ERROR_REPORT_COMPONENT_BYTES 24U
#define ERROR_REPORT_CODE_BYTES 48U
#define ERROR_REPORT_RESULT_BYTES NUWAX_DEVICE_ERROR_RESULT_BYTES
#define ERROR_REPORT_STATE_BYTES 24U
#define ERROR_REPORT_TURN_ID_BYTES 48U
#define ERROR_REPORT_ACK_TIMEOUT_MS 1500U
#define ERROR_REPORT_RETRY_BACKOFF_MS 250U
#define ERROR_REPORT_MAX_ATTEMPTS 3U
#define ERROR_REPORT_SEND_BUDGET_MS 20U
#define ERROR_REPORT_DEDUP_WINDOW_MS 3000U
#define ERROR_REPORT_WARNING_DEDUP_WINDOW_MS 60000U
#define ERROR_REPORT_WARNING_RECENT_CAPACITY 16U
#define ERROR_REPORT_PERSIST_MAGIC 0x45525250U
#define ERROR_REPORT_PERSIST_VERSION 2U
#define ERROR_REPORT_PERSIST_RETRY_MS 5000U
/* NVS exhaustion is a hard stop; retrying only reopens the Flash window. */
#define ERROR_REPORT_PERSIST_NO_SPACE_MAX_FAILURES 1U
#define ERROR_REPORT_PERSIST_RETRY_MAX_MS (5U * 60U * 1000U)
/* Coalesce durable snapshots so an error storm cannot repeatedly stop and
 * restart the wake detector in the idle path. Live WebSocket delivery remains
 * independent and continues at its existing cadence. */
#define ERROR_REPORT_PERSIST_INTERVAL_MS 60000U
#define ERROR_REPORT_FLASH_GUARD_TIMEOUT_MS 500U
#define ERROR_REPORT_NVS_NAMESPACE "nuwax_err"
#define ERROR_REPORT_NVS_KEY "queue"

static const char *TAG = "device_error_report";

typedef struct {
    bool occupied;
    bool in_flight;
    uint32_t attempts;
    uint32_t last_attempt_ms;
    uint32_t attempt_epoch;
    uint32_t key_hash;
    uint32_t occurred_ms;
    uint32_t dedup_ms;
    char event_id[ERROR_REPORT_EVENT_ID_BYTES];
    char severity[ERROR_REPORT_SEVERITY_BYTES];
    char component[ERROR_REPORT_COMPONENT_BYTES];
    char code[ERROR_REPORT_CODE_BYTES];
    char result[ERROR_REPORT_RESULT_BYTES];
    char state[ERROR_REPORT_STATE_BYTES];
    char turn_id[ERROR_REPORT_TURN_ID_BYTES];
    uint32_t generation;
} error_report_event_t;

/* Version-1 on-flash layout retained only for one-way migration after the
 * result field was enlarged.  Do not use this type for new queue entries. */
typedef struct {
    bool occupied;
    bool in_flight;
    uint32_t attempts;
    uint32_t last_attempt_ms;
    uint32_t attempt_epoch;
    uint32_t key_hash;
    uint32_t occurred_ms;
    uint32_t dedup_ms;
    char event_id[ERROR_REPORT_EVENT_ID_BYTES];
    char severity[ERROR_REPORT_SEVERITY_BYTES];
    char component[ERROR_REPORT_COMPONENT_BYTES];
    char code[ERROR_REPORT_CODE_BYTES];
    char result[32U];
    char state[ERROR_REPORT_STATE_BYTES];
    char turn_id[ERROR_REPORT_TURN_ID_BYTES];
    uint32_t generation;
} error_report_event_v1_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t boot_nonce;
    uint32_t head;
    uint32_t count;
    error_report_event_v1_t events[ERROR_REPORT_QUEUE_CAPACITY];
    uint32_t crc32;
} error_report_persisted_queue_v1_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t boot_nonce;
    uint32_t head;
    uint32_t count;
    error_report_event_t events[ERROR_REPORT_QUEUE_CAPACITY];
    uint32_t crc32;
} error_report_persisted_queue_t;

static error_report_event_t s_events[ERROR_REPORT_QUEUE_CAPACITY];
typedef struct {
    bool occupied;
    uint32_t key_hash;
    uint32_t timestamp_ms;
} warning_recent_entry_t;
static warning_recent_entry_t s_warning_recent[
    ERROR_REPORT_WARNING_RECENT_CAPACITY];
static size_t s_head;
static size_t s_count;
static uint32_t s_sequence;
static uint32_t s_boot_nonce;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_persist_loaded;
static bool s_persist_busy;
static bool s_persist_dirty;
static bool s_persist_storage_blocked;
static bool s_persist_cleanup_pending;
static uint32_t s_persist_no_space_failures;
static uint32_t s_persist_retry_delay_ms;
static uint32_t s_persist_revision;
static uint32_t s_persist_next_retry_ms;
static error_report_persisted_queue_t *s_persist_snapshot;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0U) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static void copy_result_text(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0U) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    const size_t length = strlen(src);
    if (length < dst_size) {
        memcpy(dst, src, length + 1U);
        return;
    }
    /* Keep truncation visible to the cloud instead of silently hiding the
     * tail of a diagnostic result. */
    if (dst_size <= 4U) {
        memset(dst, '.', dst_size - 1U);
        dst[dst_size - 1U] = '\0';
        return;
    }
    memcpy(dst, src, dst_size - 4U);
    memcpy(dst + dst_size - 4U, "...", 4U);
}

static uint32_t hash_key(const char *severity,
                         const char *component,
                         const char *code,
                         esp_err_t result,
                         uint32_t generation) {
    uint32_t hash = 2166136261U;
    const char *parts[] = {
        severity ? severity : "",
        component ? component : "",
        code ? code : "",
    };
    for (size_t i = 0U; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        for (const unsigned char *p = (const unsigned char *)parts[i]; *p; ++p) {
            hash ^= *p;
            hash *= 16777619U;
        }
        hash ^= 0xffU;
        hash *= 16777619U;
    }
    hash ^= (uint32_t)result;
    hash *= 16777619U;
    hash ^= generation;
    hash *= 16777619U;
    return hash ? hash : 1U;
}

static uint32_t dedup_window_ms_for_severity(const char *severity) {
    return severity && strcmp(severity, "warning") == 0
               ? ERROR_REPORT_WARNING_DEDUP_WINDOW_MS
               : ERROR_REPORT_DEDUP_WINDOW_MS;
}

static bool warning_recent_matches_locked(uint32_t key_hash,
                                          uint32_t timestamp_ms) {
    for (size_t i = 0U; i < ERROR_REPORT_WARNING_RECENT_CAPACITY; ++i) {
        const warning_recent_entry_t *entry = &s_warning_recent[i];
        if (entry->occupied && entry->key_hash == key_hash &&
            (uint32_t)(timestamp_ms - entry->timestamp_ms) <
                ERROR_REPORT_WARNING_DEDUP_WINDOW_MS) {
            return true;
        }
    }
    return false;
}

static void warning_recent_record_locked(uint32_t key_hash,
                                         uint32_t timestamp_ms) {
    size_t target = 0U;
    uint32_t oldest_age = 0U;
    bool found_empty = false;
    for (size_t i = 0U; i < ERROR_REPORT_WARNING_RECENT_CAPACITY; ++i) {
        warning_recent_entry_t *entry = &s_warning_recent[i];
        if (entry->occupied && entry->key_hash == key_hash) {
            entry->timestamp_ms = timestamp_ms;
            return;
        }
        if (!entry->occupied && !found_empty) {
            target = i;
            found_empty = true;
        } else if (!found_empty) {
            const uint32_t age =
                (uint32_t)(timestamp_ms - entry->timestamp_ms);
            if (age >= oldest_age) {
                oldest_age = age;
                target = i;
            }
        }
    }
    s_warning_recent[target] = (warning_recent_entry_t) {
        .occupied = true,
        .key_hash = key_hash,
        .timestamp_ms = timestamp_ms,
    };
}

static bool event_is_warning(const error_report_event_t *event);

static void pop_head_locked(void) {
    if (s_count == 0U) {
        return;
    }
    const bool durable_event = !event_is_warning(&s_events[s_head]);
    memset(&s_events[s_head], 0, sizeof(s_events[s_head]));
    s_head = (s_head + 1U) % ERROR_REPORT_QUEUE_CAPACITY;
    s_count--;
    if (durable_event) {
        s_persist_dirty = true;
        s_persist_revision++;
    }
}

static bool head_matches_locked(const char *event_id) {
    return s_count > 0U && event_id &&
           strcmp(s_events[s_head].event_id, event_id) == 0;
}

static bool event_is_warning(const error_report_event_t *event) {
    return event && strcmp(event->severity, "warning") == 0;
}

static bool queue_has_persistable_error_locked(void) {
    for (size_t i = 0U; i < s_count; ++i) {
        const size_t index = (s_head + i) % ERROR_REPORT_QUEUE_CAPACITY;
        if (s_events[index].occupied && !event_is_warning(&s_events[index])) {
            return true;
        }
    }
    return false;
}

static void reset_event_runtime_fields(error_report_event_t *event);

/* Keep warning events in the live upload queue, but remove them from the
 * restored snapshot view. This also sanitizes snapshots written by older
 * firmware that persisted warnings. */
static size_t filter_persisted_warnings_locked(void) {
    if (!s_persist_snapshot) {
        return s_count;
    }
    size_t kept = 0U;
    for (size_t i = 0U; i < s_count; ++i) {
        const size_t index = (s_head + i) % ERROR_REPORT_QUEUE_CAPACITY;
        if (!s_events[index].occupied || event_is_warning(&s_events[index])) {
            continue;
        }
        s_persist_snapshot->events[kept] = s_events[index];
        reset_event_runtime_fields(&s_persist_snapshot->events[kept]);
        kept++;
    }
    memset(s_events, 0, sizeof(s_events));
    if (kept > 0U) {
        memcpy(s_events,
               s_persist_snapshot->events,
               kept * sizeof(s_persist_snapshot->events[0]));
    }
    s_head = 0U;
    s_count = kept;
    return kept;
}

static void reset_event_runtime_fields(error_report_event_t *event) {
    if (!event) {
        return;
    }
    /* Attempt state is never durable. A reboot must retry the event on the
     * next WebSocket epoch instead of inheriting an in-flight timeout. */
    event->in_flight = false;
    event->attempts = 0U;
    event->last_attempt_ms = 0U;
    event->attempt_epoch = 0U;
    event->dedup_ms = 0U;
}

static bool queue_has_error_after_head_locked(void) {
    for (size_t i = 1U; i < s_count; ++i) {
        const size_t index = (s_head + i) % ERROR_REPORT_QUEUE_CAPACITY;
        if (s_events[index].occupied && !event_is_warning(&s_events[index])) {
            return true;
        }
    }
    return false;
}

static uint32_t persisted_crc(const error_report_persisted_queue_t *snapshot) {
    return esp_rom_crc32_le(0U,
                            (const uint8_t *)snapshot,
                            offsetof(error_report_persisted_queue_t, crc32));
}

static uint32_t persisted_crc_v1(
    const error_report_persisted_queue_v1_t *snapshot) {
    return esp_rom_crc32_le(
        0U,
        (const uint8_t *)snapshot,
        offsetof(error_report_persisted_queue_v1_t, crc32));
}

static esp_err_t load_persisted_queue(void) {
    if (!s_persist_snapshot) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ERROR_REPORT_NVS_NAMESPACE,
                             NVS_READONLY,
                             &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = 0U;
    err = nvs_get_blob(handle, ERROR_REPORT_NVS_KEY, NULL, &size);
    if (err == ESP_OK && size == sizeof(error_report_persisted_queue_t)) {
        error_report_persisted_queue_t *snapshot = s_persist_snapshot;
        memset(snapshot, 0, sizeof(*snapshot));
        err = nvs_get_blob(handle,
                           ERROR_REPORT_NVS_KEY,
                           snapshot,
                           &size);
        if (err == ESP_OK &&
            (snapshot->magic != ERROR_REPORT_PERSIST_MAGIC ||
             snapshot->version != ERROR_REPORT_PERSIST_VERSION ||
             snapshot->head >= ERROR_REPORT_QUEUE_CAPACITY ||
             snapshot->count > ERROR_REPORT_QUEUE_CAPACITY ||
             persisted_crc(snapshot) != snapshot->crc32)) {
            ESP_LOGW(TAG, "persisted error queue rejected as invalid");
            err = ESP_ERR_INVALID_SIZE;
        }
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            memset(s_events, 0, sizeof(s_events));
            for (size_t i = 0U; i < ERROR_REPORT_QUEUE_CAPACITY; ++i) {
                s_events[i] = snapshot->events[i];
                /* An event that was in-flight before reset must be retried
                 * from the beginning on the new WebSocket connection. */
                s_events[i].in_flight = false;
                s_events[i].attempts = 0U;
                s_events[i].last_attempt_ms = 0U;
                s_events[i].attempt_epoch = 0U;
                s_events[i].dedup_ms = 0U;
            }
            s_head = snapshot->head;
            s_count = snapshot->count;
            const size_t restored_count = s_count;
            const size_t durable_count = filter_persisted_warnings_locked();
            /* Keep restored event IDs intact, but start a new ID namespace
             * for events generated after this reboot. */
            s_sequence = 0U;
            s_boot_nonce = 0U;
            s_persist_dirty = durable_count != restored_count;
            s_persist_cleanup_pending = durable_count != restored_count;
            s_persist_revision = s_persist_dirty ? 1U : 0U;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG,
                     "restored persisted error queue count=%u sequence=%lu",
                     (unsigned)s_count,
                     (unsigned long)snapshot->sequence);
        }
    } else if (err == ESP_OK && size == sizeof(error_report_persisted_queue_v1_t)) {
        /* Version-1 snapshots are large (they contain all legacy strings).
         * Keep the migration scratch buffer off the caller's task stack; this
         * function runs during boot where stack headroom is most constrained. */
        error_report_persisted_queue_v1_t *legacy = heap_caps_calloc(
            1U,
            sizeof(*legacy),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!legacy) {
            err = ESP_ERR_NO_MEM;
        } else {
            err = nvs_get_blob(handle, ERROR_REPORT_NVS_KEY, legacy, &size);
        }
        if (err == ESP_OK &&
            (legacy->magic != ERROR_REPORT_PERSIST_MAGIC ||
             legacy->version != 1U ||
             legacy->head >= ERROR_REPORT_QUEUE_CAPACITY ||
             legacy->count > ERROR_REPORT_QUEUE_CAPACITY ||
             persisted_crc_v1(legacy) != legacy->crc32)) {
            ESP_LOGW(TAG, "persisted version-1 error queue rejected as invalid");
            err = ESP_ERR_INVALID_SIZE;
        }
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            memset(s_events, 0, sizeof(s_events));
            for (size_t i = 0U; i < ERROR_REPORT_QUEUE_CAPACITY; ++i) {
                s_events[i].occupied = legacy->events[i].occupied;
                s_events[i].key_hash = legacy->events[i].key_hash;
                s_events[i].occurred_ms = legacy->events[i].occurred_ms;
                copy_text(s_events[i].event_id,
                          sizeof(s_events[i].event_id),
                          legacy->events[i].event_id);
                copy_text(s_events[i].severity,
                          sizeof(s_events[i].severity),
                          legacy->events[i].severity);
                copy_text(s_events[i].component,
                          sizeof(s_events[i].component),
                          legacy->events[i].component);
                copy_text(s_events[i].code,
                          sizeof(s_events[i].code),
                          legacy->events[i].code);
                copy_text(s_events[i].result,
                          sizeof(s_events[i].result),
                          legacy->events[i].result);
                copy_text(s_events[i].state,
                          sizeof(s_events[i].state),
                          legacy->events[i].state);
                copy_text(s_events[i].turn_id,
                          sizeof(s_events[i].turn_id),
                          legacy->events[i].turn_id);
                s_events[i].generation = legacy->events[i].generation;
                s_events[i].in_flight = false;
                s_events[i].attempts = 0U;
                s_events[i].last_attempt_ms = 0U;
                s_events[i].attempt_epoch = 0U;
                s_events[i].dedup_ms = 0U;
            }
            s_head = legacy->head;
            s_count = legacy->count;
            (void)filter_persisted_warnings_locked();
            s_sequence = 0U;
            s_boot_nonce = 0U;
            /* Rewrite the migrated layout on the next housekeeping pass. */
            s_persist_dirty = true;
            s_persist_cleanup_pending = true;
            s_persist_revision = 1U;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG,
                     "migrated persisted error queue version=1 count=%u sequence=%lu",
                     (unsigned)s_count,
                     (unsigned long)legacy->sequence);
        }
        free(legacy);
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(handle);
    return err;
}

esp_err_t nuwax_device_error_report_init(void) {
    portENTER_CRITICAL(&s_lock);
    if (s_persist_loaded) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    s_persist_loaded = true;
    portEXIT_CRITICAL(&s_lock);

    s_persist_snapshot = heap_caps_calloc(
        1U,
        sizeof(*s_persist_snapshot),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_persist_snapshot) {
        ESP_LOGW(TAG,
                 "PSRAM allocation for persisted error queue unavailable; using RAM queue only");
        return ESP_OK;
    }

    const esp_err_t err = load_persisted_queue();
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    /* A malformed snapshot must never prevent the product from booting. The
     * next mutation will replace it with a fresh valid snapshot. */
    portENTER_CRITICAL(&s_lock);
    memset(s_events, 0, sizeof(s_events));
    s_head = 0U;
    s_count = 0U;
    s_persist_dirty = true;
    s_persist_cleanup_pending = true;
    s_persist_revision++;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "persisted error queue unavailable err=%s; starting empty",
             esp_err_to_name(err));
    return ESP_OK;
}

static void persist_snapshot_if_dirty(void) {
    static uint32_t ui_deferred_since_ms;
    /* Delivery from RAM continues; diagnostic cache writes can wait. Flash
     * guard stops MultiNet and NVS GC can stall both cores during SD capture
     * or replay. Never create that interference in the archive hot path. */
    if (nuwax_offline_audio_storage_busy() || nuwax_offline_audio_replay_running()) return;
    const uint32_t timestamp_ms = now_ms();
    error_report_persisted_queue_t *snapshot = s_persist_snapshot;
    uint32_t revision = 0U;
    bool persist_blocked = false;
    uint32_t no_space_failures = 0U;
    uint32_t retry_delay_ms = 0U;

    if (!snapshot) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    if (!s_persist_loaded || !s_persist_dirty || s_persist_busy ||
        (s_persist_next_retry_ms != 0U &&
         (int32_t)(timestamp_ms - s_persist_next_retry_ms) < 0)) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (s_persist_storage_blocked) {
        /* Keep the dirty bit so release_persisted_storage() can explicitly
         * re-enable durability after a critical NVS transaction. */
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (!queue_has_persistable_error_locked() && !s_persist_cleanup_pending) {
        /* Warnings remain available to the live WebSocket queue, but are
         * intentionally volatile. Never create an NVS transaction for a
         * warning-only queue. */
        s_persist_dirty = false;
        s_persist_next_retry_ms = 0U;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    /* Keep live delivery running while replacing the diagnostic Flash cache
     * waits for a quiet UI interval. Bound the postponement to 30 s even under
     * continuous input; the existing Flash guard still owns safety. */
    if (nuwax_lvgl_port_background_work_deferred()) {
        if (ui_deferred_since_ms == 0U) ui_deferred_since_ms = timestamp_ms ? timestamp_ms : 1U;
        if ((uint32_t)(timestamp_ms - ui_deferred_since_ms) < 30000U) {
            portEXIT_CRITICAL(&s_lock);
            return;
        }
    }
    ui_deferred_since_ms = 0U;
    s_persist_busy = true;
    revision = s_persist_revision;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = ERROR_REPORT_PERSIST_MAGIC;
    snapshot->version = ERROR_REPORT_PERSIST_VERSION;
    snapshot->sequence = s_sequence;
    snapshot->boot_nonce = s_boot_nonce;
    snapshot->head = 0U;
    snapshot->count = 0U;
    for (size_t i = 0U; i < s_count; ++i) {
        const size_t index = (s_head + i) % ERROR_REPORT_QUEUE_CAPACITY;
        if (!s_events[index].occupied || event_is_warning(&s_events[index])) {
            continue;
        }
        if (snapshot->count >= ERROR_REPORT_QUEUE_CAPACITY) {
            break;
        }
        snapshot->events[snapshot->count] = s_events[index];
        reset_event_runtime_fields(&snapshot->events[snapshot->count]);
        snapshot->count++;
    }
    snapshot->crc32 = persisted_crc(snapshot);
    portEXIT_CRITICAL(&s_lock);

    /* ESP-IDF disables the Flash cache while NVS commits. MultiNet may execute
     * from a Flash-mapped model on Core 1, so never start a transaction while
     * the wake detector can be inside detect(). The guard also fences the wake
     * supervisor from re-arming the worker until end_flash_operation(). */
    const esp_err_t guard_err =
        nuwax_wake_word_detector_begin_flash_operation(
            ERROR_REPORT_FLASH_GUARD_TIMEOUT_MS);
    if (guard_err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_persist_busy = false;
        s_persist_next_retry_ms = timestamp_ms + ERROR_REPORT_PERSIST_RETRY_MS;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG,
                 "persisting error queue deferred before Flash guard err=%s retry_ms=%u",
                 esp_err_to_name(guard_err),
                 (unsigned)ERROR_REPORT_PERSIST_RETRY_MS);
        return;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ERROR_REPORT_NVS_NAMESPACE,
                             NVS_READWRITE,
                             &handle);
    if (err == ESP_OK) {
        if (snapshot->count == 0U) {
            err = nvs_erase_key(handle, ERROR_REPORT_NVS_KEY);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        } else {
            err = nvs_set_blob(handle,
                               ERROR_REPORT_NVS_KEY,
                               snapshot,
                               sizeof(*snapshot));
            if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
                /* Diagnostics are optional. Never erase/compact and rewrite
                 * the namespace inside the online owner. The no-space policy
                 * below blocks persistence; RAM delivery remains available. */
                ESP_LOGW(TAG, "diagnostic NVS full; retaining RAM reports without online compaction");
            }
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    nuwax_wake_word_detector_end_flash_operation();

    portENTER_CRITICAL(&s_lock);
    s_persist_busy = false;
    if (err == ESP_OK) {
        if (revision == s_persist_revision) {
            s_persist_dirty = false;
        }
        s_persist_storage_blocked = false;
        s_persist_cleanup_pending = false;
        s_persist_no_space_failures = 0U;
        s_persist_retry_delay_ms = ERROR_REPORT_PERSIST_RETRY_MS;
        /* Coalesce durable error events without coupling warning uploads to
         * flash. The live queue remains independent of this snapshot. */
        s_persist_next_retry_ms = timestamp_ms +
                                  ERROR_REPORT_PERSIST_INTERVAL_MS;
    } else if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        if (s_persist_no_space_failures < UINT32_MAX) {
            s_persist_no_space_failures++;
        }
        if (s_persist_retry_delay_ms == 0U) {
            s_persist_retry_delay_ms = ERROR_REPORT_PERSIST_RETRY_MS;
        } else if (s_persist_retry_delay_ms <
                   ERROR_REPORT_PERSIST_RETRY_MAX_MS / 2U) {
            s_persist_retry_delay_ms *= 2U;
        } else {
            s_persist_retry_delay_ms = ERROR_REPORT_PERSIST_RETRY_MAX_MS;
        }
        if (s_persist_no_space_failures >=
            ERROR_REPORT_PERSIST_NO_SPACE_MAX_FAILURES) {
            s_persist_storage_blocked = true;
            s_persist_next_retry_ms = 0U;
        } else {
            s_persist_next_retry_ms = timestamp_ms +
                                      s_persist_retry_delay_ms;
        }
    } else {
        s_persist_next_retry_ms = timestamp_ms +
                                  (s_persist_retry_delay_ms != 0U
                                       ? s_persist_retry_delay_ms
                                       : ERROR_REPORT_PERSIST_RETRY_MS);
    }
    persist_blocked = s_persist_storage_blocked;
    no_space_failures = s_persist_no_space_failures;
    retry_delay_ms = s_persist_retry_delay_ms;
    portEXIT_CRITICAL(&s_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "persisting error queue deferred err=%s failures=%lu retry_ms=%lu blocked=%d",
                 esp_err_to_name(err),
                 (unsigned long)no_space_failures,
                 (unsigned long)retry_delay_ms,
                 persist_blocked ? 1 : 0);
    }
}

void nuwax_device_error_report_housekeeping(void) {
    persist_snapshot_if_dirty();
}

esp_err_t nuwax_device_error_report_release_persisted_storage(void) {
    const esp_err_t guard_err =
        nuwax_wake_word_detector_begin_flash_operation(
            ERROR_REPORT_FLASH_GUARD_TIMEOUT_MS);
    if (guard_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "diagnostic NVS release deferred before Flash guard err=%s",
                 esp_err_to_name(guard_err));
        return guard_err;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ERROR_REPORT_NVS_NAMESPACE,
                             NVS_READWRITE,
                             &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        portENTER_CRITICAL(&s_lock);
        s_persist_storage_blocked = false;
        s_persist_cleanup_pending = false;
        s_persist_no_space_failures = 0U;
        s_persist_retry_delay_ms = 0U;
        s_persist_next_retry_ms = 0U;
        portEXIT_CRITICAL(&s_lock);
        nuwax_wake_word_detector_end_flash_operation();
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nuwax_wake_word_detector_end_flash_operation();
        return err;
    }

    err = nvs_erase_key(handle, ERROR_REPORT_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        /* A stale page can reject a single erase entry. Reopen and erase the
         * diagnostic namespace as a whole; it contains no user credentials. */
        err = nvs_open(ERROR_REPORT_NVS_NAMESPACE,
                       NVS_READWRITE,
                       &handle);
        if (err == ESP_OK) {
            err = nvs_erase_all(handle);
            if (err == ESP_OK) {
                err = nvs_commit(handle);
            }
            nvs_close(handle);
        }
    }
    if (err != ESP_OK) {
        nuwax_wake_word_detector_end_flash_operation();
        return err;
    }

    /* The queue is intentionally best-effort. Drop the in-memory copy too,
     * otherwise housekeeping would immediately recreate the large blob while
     * the provisioning transaction is trying to reserve NVS space. */
    portENTER_CRITICAL(&s_lock);
    memset(s_events, 0, sizeof(s_events));
    s_head = 0U;
    s_count = 0U;
    s_persist_dirty = false;
    s_persist_busy = false;
    s_persist_storage_blocked = false;
    s_persist_cleanup_pending = false;
    s_persist_no_space_failures = 0U;
    s_persist_retry_delay_ms = 0U;
    s_persist_next_retry_ms = 0U;
    s_persist_revision++;
    portEXIT_CRITICAL(&s_lock);
    nuwax_wake_word_detector_end_flash_operation();
    ESP_LOGW(TAG, "persisted diagnostic queue released for critical NVS operation");
    return ESP_OK;
}

bool nuwax_device_error_report_enqueue_detail(
    const char *severity,
    const char *component,
    const char *code,
    esp_err_t result,
    const char *result_detail,
    const char *state,
    uint32_t generation,
    const char *turn_id) {
    if (!component || !component[0] || !code || !code[0]) {
        return false;
    }

    const uint32_t timestamp_ms = now_ms();
    const uint32_t key_hash = hash_key(severity, component, code, result,
                                       generation);
    const uint32_t dedup_window_ms =
        dedup_window_ms_for_severity(severity);
    portENTER_CRITICAL(&s_lock);
    if (severity && strcmp(severity, "warning") == 0 &&
        warning_recent_matches_locked(key_hash, timestamp_ms)) {
        portEXIT_CRITICAL(&s_lock);
        return true;
    }
    for (size_t i = 0U; i < s_count; ++i) {
        const size_t index = (s_head + i) % ERROR_REPORT_QUEUE_CAPACITY;
        const error_report_event_t *queued = &s_events[index];
        if (queued->occupied && queued->key_hash == key_hash &&
            queued->dedup_ms != 0U &&
            (uint32_t)(timestamp_ms - queued->dedup_ms) <
                dedup_window_ms) {
            portEXIT_CRITICAL(&s_lock);
            return true;
        }
    }

    if (s_count >= ERROR_REPORT_QUEUE_CAPACITY) {
        if (severity && strcmp(severity, "warning") == 0) {
            /* Warnings are advisory. Never evict an older error just to keep
             * a warning, and never block the caller when the queue is full.
             * Remember this dropped key as well: otherwise a saturated error
             * queue turns every repeated warning into another local log and
             * amplifies the exact pressure this path is intended to contain. */
            warning_recent_record_locked(key_hash, timestamp_ms);
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG, "warning report queue full; dropping component=%s code=%s",
                     component, code);
            return false;
        }
        /* Never evict an in-flight event.  A full queue is intentionally
         * bounded; dropping a new event is preferable to blocking audio. */
        if (!s_events[s_head].in_flight) {
            pop_head_locked();
        } else {
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG, "error report queue full; dropping component=%s code=%s",
                     component, code);
            return false;
        }
    }

    const size_t index = (s_head + s_count) % ERROR_REPORT_QUEUE_CAPACITY;
    error_report_event_t *event = &s_events[index];
    memset(event, 0, sizeof(*event));
    if (s_boot_nonce == 0U) {
        s_boot_nonce = esp_random();
        if (s_boot_nonce == 0U) {
            s_boot_nonce = timestamp_ms ? timestamp_ms : 1U;
        }
    }
    const uint32_t sequence = ++s_sequence;
    snprintf(event->event_id,
             sizeof(event->event_id),
             "e_%08lx_%06lu",
             (unsigned long)s_boot_nonce,
             (unsigned long)sequence);
    copy_text(event->severity,
              sizeof(event->severity),
              severity && severity[0] ? severity : "error");
    copy_text(event->component, sizeof(event->component), component);
    copy_text(event->code, sizeof(event->code), code);
    if (result_detail && result_detail[0]) {
        copy_result_text(event->result,
                         sizeof(event->result),
                         result_detail);
    } else if (result != ESP_OK) {
        copy_text(event->result,
                  sizeof(event->result),
                  esp_err_to_name(result));
    }
    copy_text(event->state, sizeof(event->state), state);
    copy_text(event->turn_id, sizeof(event->turn_id), turn_id);
    event->generation = generation;
    event->key_hash = key_hash;
    event->occurred_ms = timestamp_ms;
    event->dedup_ms = timestamp_ms;
    event->occupied = true;
    s_count++;
    if (event_is_warning(event)) {
        warning_recent_record_locked(key_hash, timestamp_ms);
    }
    if (!event_is_warning(event)) {
        s_persist_dirty = true;
        s_persist_revision++;
    }
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool nuwax_device_error_report_enqueue(const char *severity,
                                       const char *component,
                                       const char *code,
                                       esp_err_t result,
                                       const char *state,
                                       uint32_t generation,
                                       const char *turn_id) {
    return nuwax_device_error_report_enqueue_detail(
        severity, component, code, result, NULL, state, generation, turn_id);
}

bool nuwax_device_warning_report_enqueue(const char *component,
                                         const char *code,
                                         esp_err_t result,
                                         const char *state,
                                         uint32_t generation,
                                         const char *turn_id) {
    return nuwax_device_warning_report_enqueue_detail(
        component, code, result, NULL, state, generation, turn_id);
}

bool nuwax_device_warning_report_enqueue_detail(
    const char *component,
    const char *code,
    esp_err_t result,
    const char *result_detail,
    const char *state,
    uint32_t generation,
    const char *turn_id) {
    return nuwax_device_error_report_enqueue_detail(
        "warning",
        component,
        code,
        result,
        result_detail,
        state,
        generation,
        turn_id);
}

static esp_err_t send_event(nuwax_ws_client_t *ws,
                            uint32_t epoch,
                            const error_report_event_t *event) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "device_error_report");
    cJSON_AddStringToObject(root, "event_id", event->event_id);
    cJSON_AddStringToObject(root, "severity", event->severity);
    cJSON_AddStringToObject(root, "component", event->component);
    cJSON_AddStringToObject(root, "code", event->code);
    if (event->result[0]) {
        cJSON_AddStringToObject(root, "result", event->result);
    }
    cJSON_AddNumberToObject(root, "uptime_ms", (double)event->occurred_ms);
    if (event->state[0]) {
        cJSON_AddStringToObject(root, "state", event->state);
    }
    if (event->generation != 0U) {
        cJSON_AddNumberToObject(root, "generation", event->generation);
    }
    if (event->turn_id[0]) {
        cJSON_AddStringToObject(root, "turn_id", event->turn_id);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = nuwax_ws_send_text_until_if_epoch(
        ws,
        json,
        esp_timer_get_time() + ERROR_REPORT_SEND_BUDGET_MS * 1000LL,
        epoch);
    free(json);
    return err;
}

static bool payload_contains_literal(const char *payload,
                                     size_t payload_len,
                                     const char *literal) {
    if (!payload || !literal || literal[0] == '\0') {
        return false;
    }
    const size_t literal_len = strlen(literal);
    if (literal_len > payload_len) {
        return false;
    }
    for (size_t offset = 0U; offset + literal_len <= payload_len; ++offset) {
        if (payload[offset] != literal[0]) {
            continue;
        }
        if (memcmp(payload + offset, literal, literal_len) == 0) {
            return true;
        }
    }
    return false;
}

void nuwax_device_error_report_service(nuwax_ws_client_t *ws) {
    /* This runs on the conversation owner, including during playback. NVS
     * GC is not bounded by the network send budget and must only run through
     * housekeeping after the owner has checked that audio is idle. */
    if (!ws || !nuwax_ws_is_connected(ws)) {
        return;
    }
    const uint32_t epoch = nuwax_ws_connection_epoch(ws);
    if (epoch == 0U) {
        return;
    }

    error_report_event_t event = {0};
    const uint32_t timestamp_ms = now_ms();
    portENTER_CRITICAL(&s_lock);
    if (s_count == 0U) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    error_report_event_t *head = &s_events[s_head];
    if (!head->in_flight) {
        const bool warning_priority_drop =
            event_is_warning(head) && head->attempts > 0U &&
            queue_has_error_after_head_locked();
        if (head->attempts >= ERROR_REPORT_MAX_ATTEMPTS ||
            warning_priority_drop) {
            char dropped_event_id[ERROR_REPORT_EVENT_ID_BYTES];
            const uint32_t dropped_attempts = head->attempts;
            copy_text(dropped_event_id,
                      sizeof(dropped_event_id),
                      head->event_id);
            pop_head_locked();
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG,
                     "error report send retry limit event=%s attempts=%lu reason=%s; dropping",
                     dropped_event_id,
                     (unsigned long)dropped_attempts,
                     warning_priority_drop ? "warning_priority" : "retry_exhausted");
            return;
        }
    } else {
        const uint32_t age_ms = timestamp_ms - head->last_attempt_ms;
        if (age_ms < ERROR_REPORT_ACK_TIMEOUT_MS) {
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        const bool warning_priority_drop =
            event_is_warning(head) && queue_has_error_after_head_locked();
        if (head->attempts >= ERROR_REPORT_MAX_ATTEMPTS ||
            warning_priority_drop) {
            char dropped_event_id[ERROR_REPORT_EVENT_ID_BYTES];
            const uint32_t dropped_attempts = head->attempts;
            copy_text(dropped_event_id,
                      sizeof(dropped_event_id),
                      head->event_id);
            pop_head_locked();
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG,
                     "error report ACK timeout event=%s attempts=%lu reason=%s; dropping",
                     dropped_event_id,
                     (unsigned long)dropped_attempts,
                     warning_priority_drop ? "warning_priority" : "retry_exhausted");
            return;
        }
        head->in_flight = false;
    }
    if (head->last_attempt_ms != 0U &&
        (uint32_t)(timestamp_ms - head->last_attempt_ms) <
            ERROR_REPORT_RETRY_BACKOFF_MS) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    head->in_flight = true;
    head->attempts++;
    head->last_attempt_ms = timestamp_ms;
    head->attempt_epoch = epoch;
    event = *head;
    portEXIT_CRITICAL(&s_lock);

    const esp_err_t err = send_event(ws, epoch, &event);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "error report sent event=%s attempt=%lu epoch=%lu",
                 event.event_id, (unsigned long)event.attempts,
                 (unsigned long)epoch);
        return;
    }

    portENTER_CRITICAL(&s_lock);
    if (head_matches_locked(event.event_id)) {
        s_events[s_head].in_flight = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "error report send deferred event=%s err=%s",
                 event.event_id, esp_err_to_name(err));
    }
}

bool nuwax_device_error_report_handle_ack(const char *payload,
                                          int len,
                                          uint32_t ws_epoch) {
    if (!payload || len <= 0) {
        return false;
    }
    /* Most cloud text frames are conversation/control messages. Avoid a full
     * cJSON allocation and parse unless the bounded payload contains the ACK
     * discriminator; the exact schema validation below remains authoritative. */
    if (!payload_contains_literal(payload,
                                  (size_t)len,
                                  "device_error_report_ack")) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(payload, (size_t)len);
    if (!root) {
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) ||
        strcmp(type->valuestring, "device_error_report_ack") != 0) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *event_id =
        cJSON_GetObjectItemCaseSensitive(root, "event_id");
    const cJSON *accepted =
        cJSON_GetObjectItemCaseSensitive(root, "accepted");
    if (!cJSON_IsString(event_id) || !event_id->valuestring ||
        event_id->valuestring[0] == '\0' || !cJSON_IsBool(accepted)) {
        ESP_LOGW(TAG, "invalid error report ACK ignored");
        cJSON_Delete(root);
        return true;
    }
    const bool accepted_value = cJSON_IsTrue(accepted);
    bool matched = false;
    portENTER_CRITICAL(&s_lock);
    if (head_matches_locked(event_id->valuestring) &&
        s_events[s_head].attempt_epoch == ws_epoch) {
        matched = true;
        pop_head_locked();
    }
    portEXIT_CRITICAL(&s_lock);
    if (matched && !accepted_value) {
        ESP_LOGW(TAG, "cloud rejected error report event=%s",
                 event_id->valuestring);
    }
    cJSON_Delete(root);
    return true;
}
