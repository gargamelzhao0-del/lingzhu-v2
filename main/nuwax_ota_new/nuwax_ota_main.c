/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "nuwax_ota_main.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nuwax_ota.h"
#include "nuwax_ota_identity_recovery_policy.h"
#include "nuwax_ota_manifest.h"
#include "nuwax_ota_release_policy.h"
#include "nuwax_ota_transport.h"
#include "wake_word_detector.h"

static const char *TAG = "nuwax_ota_main";
static const char *const NVS_NAMESPACE = "nuwax_ota";
static const char *const NVS_KEY_LAST = "last_status";
static const char *const NVS_KEY_PENDING = "pending_ver";
static const char *const NVS_KEY_ACCEPTED_SEQ = "accepted_seq";
static const char *const NVS_KEY_PENDING_SEQ = "pending_seq";
static const char *const NVS_KEY_PENDING_TXN = "pending_txn";
static const char *const NVS_KEY_ACCEPTED_TXN = "accepted_txn";

#define NUWAX_OTA_PENDING_TXN_MAGIC   0x4f544150U
#define NUWAX_OTA_PENDING_TXN_VERSION 3U
#define NUWAX_OTA_IDENTITY_HASH_CHUNK_BYTES 1024U
#define NUWAX_OTA_IDENTITY_HASH_YIELD_BYTES 4096U

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static nuwax_ota_main_callbacks_t s_callbacks;
static bool s_initialized;
static bool s_transport_idle = true;
static bool s_transport_armed;
static nuwax_ota_manifest_t s_transport_manifest;
static char s_status_text[128] = "not_initialized";
static int s_last_emitted_pct = -1;
static nuwax_ota_state_t s_last_emitted_state = NUWAX_OTA_STATE_IDLE;
static uint64_t s_effective_release_sequence =
    NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE;

/* Volatile, boot-scoped digest of the immutable RUNNING app. Never cache a
 * manifest's claimed digest, nor a partially read image. Other partitions
 * cannot use this cache; a reboot naturally discards it. */
static struct {
    bool busy;
    bool valid;
    uint32_t address;
    uint32_t size;
    char version[32];
    uint8_t elf_sha256[32];
    char digest[NUWAX_OTA_MANIFEST_SHA256_LEN];
} s_running_hash;

#define OTA_NVS_FLASH_GUARD_TIMEOUT_MS 500U

/* All OTA metadata reads and writes go through this pair. The wake detector
 * guard is a no-op before detector initialization and is re-entrant for the
 * nested metadata helpers used during reconciliation. */
static esp_err_t ota_nvs_open(const char *namespace_name,
                              nvs_open_mode_t mode,
                              nvs_handle_t *out_handle)
{
    const esp_err_t guard_err =
        nuwax_wake_word_detector_begin_flash_operation(
            OTA_NVS_FLASH_GUARD_TIMEOUT_MS);
    if (guard_err != ESP_OK) {
        return guard_err;
    }
    const esp_err_t err = nvs_open(namespace_name, mode, out_handle);
    if (err != ESP_OK) {
        nuwax_wake_word_detector_end_flash_operation();
    }
    return err;
}

static void ota_nvs_close(nvs_handle_t handle)
{
    nvs_close(handle);
    nuwax_wake_word_detector_end_flash_operation();
}

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t record_size;
    uint64_t release_sequence;
    uint32_t target_partition_address;
    uint32_t image_size;
    char version[NUWAX_OTA_MANIFEST_VERSION_MAX_LEN];
    char image_sha256[NUWAX_OTA_MANIFEST_SHA256_LEN];
} nuwax_ota_pending_txn_t;

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t record_size;
    uint64_t release_sequence;
    uint32_t target_partition_address;
    uint32_t reserved;
    char version[NUWAX_OTA_MANIFEST_VERSION_MAX_LEN];
    char image_sha256[NUWAX_OTA_MANIFEST_SHA256_LEN];
} nuwax_ota_pending_txn_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t record_size;
    uint64_t release_sequence;
    uint32_t target_partition_address;
    uint32_t reserved;
    char version[NUWAX_OTA_MANIFEST_VERSION_MAX_LEN];
} nuwax_ota_pending_txn_v1_t;

static bool sha256_hex_valid(const char *value)
{
    if (value == NULL) {
        return false;
    }
    for (size_t i = 0; i < 64U; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return value[64] == '\0';
}

static esp_err_t nvs_set_status(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = ota_nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(handle, key, value != NULL ? value : "");
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    ota_nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist %s failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t nvs_erase_if_present(nvs_handle_t handle, const char *key)
{
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    return err;
}

static esp_err_t nvs_get_status(const char *key, char *value, size_t value_size)
{
    if (value == NULL || value_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = ota_nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t required = value_size;
    err = nvs_get_str(handle, key, value, &required);
    ota_nvs_close(handle);
    return err;
}

static esp_err_t nvs_set_sequence(const char *key, uint64_t value)
{
    nvs_handle_t handle;
    esp_err_t err = ota_nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u64(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    ota_nvs_close(handle);
    return err;
}

static esp_err_t nvs_get_sequence(const char *key, uint64_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = ota_nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u64(handle, key, value);
    ota_nvs_close(handle);
    return err;
}

static esp_err_t nvs_get_accepted_sequence(uint64_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *value = NUWAX_OTA_BASE_RELEASE_SEQUENCE;
    esp_err_t err = nvs_get_sequence(NVS_KEY_ACCEPTED_SEQ, value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *value = NUWAX_OTA_BASE_RELEASE_SEQUENCE;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    return *value >= NUWAX_OTA_BASE_RELEASE_SEQUENCE
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
}

static bool pending_txn_valid(const nuwax_ota_pending_txn_t *record)
{
    return record != NULL &&
           record->magic == NUWAX_OTA_PENDING_TXN_MAGIC &&
           record->format_version == NUWAX_OTA_PENDING_TXN_VERSION &&
           record->record_size == sizeof(*record) &&
           record->release_sequence > NUWAX_OTA_BASE_RELEASE_SEQUENCE &&
           record->target_partition_address != 0U &&
           record->image_size > 0U &&
           record->image_size <= NUWAX_OTA_MANIFEST_MAX_SIZE &&
           record->version[0] != '\0' &&
           memchr(record->version, '\0', sizeof(record->version)) != NULL &&
           sha256_hex_valid(record->image_sha256);
}

static esp_err_t nvs_set_pending_txn(const nuwax_ota_pending_txn_t *record)
{
    if (!pending_txn_valid(record)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = ota_nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY_PENDING_TXN, record, sizeof(*record));
    if (err == ESP_OK) {
        err = nvs_erase_if_present(handle, NVS_KEY_PENDING);
    }
    if (err == ESP_OK) {
        err = nvs_erase_if_present(handle, NVS_KEY_PENDING_SEQ);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    ota_nvs_close(handle);
    return err;
}

static esp_err_t nvs_set_accepted_txn(const nuwax_ota_pending_txn_t *record)
{
    if (!pending_txn_valid(record)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = ota_nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY_ACCEPTED_TXN, record, sizeof(*record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    ota_nvs_close(handle);
    return err;
}

static esp_err_t nvs_get_accepted_txn(nuwax_ota_pending_txn_t *record)
{
    if (record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(record, 0, sizeof(*record));
    nvs_handle_t handle;
    esp_err_t err = ota_nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t size = sizeof(*record);
    err = nvs_get_blob(handle, NVS_KEY_ACCEPTED_TXN, record, &size);
    ota_nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(*record)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (record->magic == NUWAX_OTA_PENDING_TXN_MAGIC &&
        record->format_version == 2U &&
        record->record_size == sizeof(nuwax_ota_pending_txn_v2_t)) {
        return ESP_ERR_INVALID_CRC;
    }
    return pending_txn_valid(record) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t nvs_get_pending_txn(nuwax_ota_pending_txn_t *record,
                                     bool *out_legacy)
{
    if (record == NULL || out_legacy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(record, 0, sizeof(*record));
    *out_legacy = false;

    nvs_handle_t handle;
    esp_err_t err = ota_nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = 0;
        err = nvs_get_blob(handle, NVS_KEY_PENDING_TXN, NULL, &size);
        if (err == ESP_OK && size == sizeof(*record)) {
            err = nvs_get_blob(handle, NVS_KEY_PENDING_TXN, record, &size);
            if (err == ESP_OK &&
                record->magic == NUWAX_OTA_PENDING_TXN_MAGIC &&
                record->format_version == 2U &&
                record->record_size == sizeof(nuwax_ota_pending_txn_v2_t)) {
                *out_legacy = true;
            }
        } else if (err == ESP_OK && size == sizeof(nuwax_ota_pending_txn_v1_t)) {
            nuwax_ota_pending_txn_v1_t old = {0};
            size_t old_size = sizeof(old);
            err = nvs_get_blob(handle, NVS_KEY_PENDING_TXN, &old, &old_size);
            if (err == ESP_OK &&
                old.magic == NUWAX_OTA_PENDING_TXN_MAGIC &&
                old.format_version == 1U && old.record_size == sizeof(old) &&
                old.release_sequence > NUWAX_OTA_BASE_RELEASE_SEQUENCE &&
                old.target_partition_address != 0U && old.version[0] != '\0') {
                if (memchr(old.version, '\0', sizeof(old.version)) == NULL) {
                    err = ESP_ERR_INVALID_STATE;
                } else {
                    record->magic = old.magic;
                    record->format_version = old.format_version;
                    record->record_size = old.record_size;
                    record->release_sequence = old.release_sequence;
                    record->target_partition_address = old.target_partition_address;
                    strlcpy(record->version, old.version,
                            sizeof(record->version));
                    *out_legacy = true;
                }
            } else if (err == ESP_OK) {
                err = ESP_ERR_INVALID_STATE;
            }
        } else if (err == ESP_OK) {
            err = ESP_ERR_INVALID_SIZE;
        }
        ota_nvs_close(handle);
        if (err == ESP_OK) {
            if (!*out_legacy && !pending_txn_valid(record)) {
                return ESP_ERR_INVALID_STATE;
            }
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            return err;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    char legacy_version[NUWAX_OTA_MANIFEST_VERSION_MAX_LEN] = {0};
    uint64_t legacy_sequence = 0;
    esp_err_t version_err = nvs_get_status(NVS_KEY_PENDING, legacy_version,
                                           sizeof(legacy_version));
    esp_err_t sequence_err = nvs_get_sequence(NVS_KEY_PENDING_SEQ,
                                              &legacy_sequence);
    if (version_err == ESP_ERR_NVS_NOT_FOUND &&
        sequence_err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (version_err != ESP_OK || sequence_err != ESP_OK ||
        legacy_version[0] == '\0' ||
        legacy_sequence <= NUWAX_OTA_BASE_RELEASE_SEQUENCE) {
        return ESP_ERR_INVALID_STATE;
    }

    record->magic = NUWAX_OTA_PENDING_TXN_MAGIC;
    record->format_version = NUWAX_OTA_PENDING_TXN_VERSION;
    record->record_size = (uint16_t)sizeof(*record);
    record->release_sequence = legacy_sequence;
    strlcpy(record->version, legacy_version, sizeof(record->version));
    *out_legacy = true;
    return ESP_OK;
}

static esp_err_t nvs_clear_pending_txn(void)
{
    nvs_handle_t handle;
    esp_err_t err = ota_nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_if_present(handle, NVS_KEY_PENDING_TXN);
    if (err == ESP_OK) {
        err = nvs_erase_if_present(handle, NVS_KEY_PENDING);
    }
    if (err == ESP_OK) {
        err = nvs_erase_if_present(handle, NVS_KEY_PENDING_SEQ);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    ota_nvs_close(handle);
    return err;
}

static uint64_t effective_release_sequence(void)
{
    uint64_t sequence;
    portENTER_CRITICAL(&s_lock);
    sequence = s_effective_release_sequence;
    portEXIT_CRITICAL(&s_lock);
    return sequence;
}

static void set_effective_release_sequence(uint64_t sequence)
{
    portENTER_CRITICAL(&s_lock);
    if (sequence > s_effective_release_sequence) {
        s_effective_release_sequence = sequence;
    }
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t make_pending_txn(const nuwax_ota_manifest_t *manifest,
                                  const esp_partition_t *target,
                                  nuwax_ota_pending_txn_t *record)
{
    if (manifest == NULL || target == NULL || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(record, 0, sizeof(*record));
    record->magic = NUWAX_OTA_PENDING_TXN_MAGIC;
    record->format_version = NUWAX_OTA_PENDING_TXN_VERSION;
    record->record_size = (uint16_t)sizeof(*record);
    const uint64_t current_sequence = effective_release_sequence();
    if (current_sequence == UINT64_MAX) {
        return ESP_ERR_INVALID_VERSION;
    }
    /* The cloud no longer supplies a release sequence. This value is only a
     * durable transaction generation used while the old image owns the
     * update. The new image commits its own compiled release sequence after
     * the boot health gate succeeds. */
    record->release_sequence = current_sequence + 1U;
    record->target_partition_address = target->address;
    record->image_size = manifest->size_bytes;
    strlcpy(record->version, manifest->version, sizeof(record->version));
    strlcpy(record->image_sha256, manifest->image_sha256,
            sizeof(record->image_sha256));
    return pending_txn_valid(record) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t partition_sha256_hex(
    const esp_partition_t *partition,
    uint32_t image_size,
    char out_hex[NUWAX_OTA_MANIFEST_SHA256_LEN])
{
    if (partition == NULL || out_hex == NULL || image_size == 0U ||
        image_size > partition->size ||
        image_size > NUWAX_OTA_MANIFEST_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    nuwax_ota_sha256_t hash = {0};
    esp_err_t err = nuwax_ota_sha256_init(&hash);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t buffer[NUWAX_OTA_IDENTITY_HASH_CHUNK_BYTES];
    uint32_t offset = 0U;
    while (offset < image_size) {
        const size_t remaining = (size_t)(image_size - offset);
        const size_t chunk = remaining < sizeof(buffer) ? remaining
                                                        : sizeof(buffer);
        err = esp_partition_read(partition, offset, buffer, chunk);
        if (err == ESP_OK) {
            err = nuwax_ota_sha256_update(&hash, buffer, chunk);
        }
        if (err != ESP_OK) {
            nuwax_ota_sha256_abort(&hash);
            return err;
        }
        offset += (uint32_t)chunk;
        /* Identity recovery is a maintenance operation, not a realtime flash
         * transaction. Bound its CPU/flash duty cycle so UI, Wi-Fi, audio and
         * the task watchdog retain deterministic scheduling windows. */
        if (offset < image_size &&
            offset % NUWAX_OTA_IDENTITY_HASH_YIELD_BYTES == 0U) {
            vTaskDelay(1);
        }
    }
    return nuwax_ota_sha256_final(&hash, out_hex);
}

static esp_err_t running_partition_sha256(
    const esp_partition_t *partition, uint32_t image_size,
    char out_hex[NUWAX_OTA_MANIFEST_SHA256_LEN], bool allow_compute)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    if (partition == NULL || running == NULL || app == NULL || out_hex == NULL ||
        partition->address != running->address || partition->size != running->size ||
        image_size == 0U || image_size > running->size ||
        image_size > NUWAX_OTA_MANIFEST_MAX_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    out_hex[0] = '\0';
    portENTER_CRITICAL(&s_lock);
    const bool matches = s_running_hash.valid &&
        s_running_hash.address == running->address && s_running_hash.size == image_size &&
        memcmp(s_running_hash.version, app->version, sizeof(s_running_hash.version)) == 0 &&
        memcmp(s_running_hash.elf_sha256, app->app_elf_sha256,
               sizeof(s_running_hash.elf_sha256)) == 0;
    if (matches) {
        memcpy(out_hex, s_running_hash.digest, sizeof(s_running_hash.digest));
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    if (!allow_compute || s_running_hash.busy) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    s_running_hash.busy = true;
    s_running_hash.valid = false;
    portEXIT_CRITICAL(&s_lock);

    const int64_t started = esp_timer_get_time();
    ESP_LOGI(TAG, "running hash begin revision=ota-identity-v1 partition=%s bytes=%lu",
             running->label, (unsigned long)image_size);
    char digest[NUWAX_OTA_MANIFEST_SHA256_LEN] = {0};
    const esp_err_t err = partition_sha256_hex(running, image_size, digest);
    portENTER_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        s_running_hash.address = running->address;
        s_running_hash.size = image_size;
        memcpy(s_running_hash.version, app->version, sizeof(s_running_hash.version));
        memcpy(s_running_hash.elf_sha256, app->app_elf_sha256, sizeof(s_running_hash.elf_sha256));
        memcpy(s_running_hash.digest, digest, sizeof(digest));
        memcpy(out_hex, digest, sizeof(digest));
        s_running_hash.valid = true;
    }
    s_running_hash.busy = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "running hash complete revision=ota-identity-v1 err=%s elapsed_ms=%lu",
             esp_err_to_name(err), (unsigned long)((esp_timer_get_time() - started) / 1000));
    return err;
}

static nuwax_ota_identity_condition_t running_identity_condition(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NVS_NOT_FOUND:
        return NUWAX_OTA_IDENTITY_MISSING;
    case ESP_ERR_NVS_TYPE_MISMATCH:
    case ESP_ERR_NVS_INVALID_LENGTH:
    case ESP_ERR_INVALID_CRC:
    case ESP_ERR_INVALID_SIZE:
    case ESP_ERR_INVALID_STATE:
        return NUWAX_OTA_IDENTITY_STALE;
    default:
        return NUWAX_OTA_IDENTITY_STORAGE_FAILURE;
    }
}

esp_err_t nuwax_ota_main_get_running_image_sha256(
    char out_sha256[NUWAX_OTA_RUNNING_IDENTITY_SHA256_LEN])
{
    if (out_sha256 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out_sha256[0] = '\0';

    bool pending_verify = false;
    esp_err_t err = nuwax_ota_running_image_pending_verify(&pending_verify);
    if (err != ESP_OK) {
        return err;
    }

    nuwax_ota_running_identity_t identity = {0};
    err = nuwax_ota_main_get_running_identity(&identity);
    if (err == ESP_OK) {
        strlcpy(out_sha256, identity.image_sha256,
                NUWAX_OTA_RUNNING_IDENTITY_SHA256_LEN);
        return ESP_OK;
    }
    const nuwax_ota_identity_condition_t identity_condition =
        running_identity_condition(err);
    if (!nuwax_ota_identity_partition_fallback_allowed(
            pending_verify, identity_condition)) {
        return err;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG,
             "running identity metadata unavailable err=%s; hashing active partition label=%s address=0x%08lx",
             esp_err_to_name(err), running->label,
             (unsigned long)running->address);
    const esp_partition_pos_t position = {
        .offset = running->address,
        .size = running->size,
    };
    esp_image_metadata_t metadata = {0};
    err = esp_image_get_metadata(&position, &metadata);
    if (err != ESP_OK || metadata.image_len == 0U ||
        metadata.image_len > running->size) {
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    err = running_partition_sha256(running, metadata.image_len, out_sha256, true);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "running image identity recovered label=%s address=0x%08lx image_len=%lu",
                 running->label, (unsigned long)running->address,
                 (unsigned long)metadata.image_len);
    }
    return err;
}

static bool pending_txn_metadata_matches_running(
    const nuwax_ota_pending_txn_t *record,
    const esp_partition_t *running_partition,
    const esp_app_desc_t *running_app)
{
    return pending_txn_valid(record) && running_partition != NULL &&
           running_app != NULL &&
           record->target_partition_address == running_partition->address &&
           strcmp(record->version, running_app->version) == 0;
}

static esp_err_t pending_txn_matches_running(
    const nuwax_ota_pending_txn_t *record,
    const esp_partition_t *running_partition,
    const esp_app_desc_t *running_app,
    bool *out_matches)
{
    if (out_matches == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_matches = false;
    if (!pending_txn_metadata_matches_running(record, running_partition,
                                              running_app)) {
        return ESP_OK;
    }
    char actual_sha256[NUWAX_OTA_MANIFEST_SHA256_LEN] = {0};
    esp_err_t err = running_partition_sha256(running_partition,
                                         record->image_size,
                                         actual_sha256, true);
    if (err == ESP_OK) {
        *out_matches = strcmp(record->image_sha256, actual_sha256) == 0;
    }
    memset(actual_sha256, 0, sizeof(actual_sha256));
    return err;
}

static esp_err_t reconcile_pending_txn(bool running_pending,
                                       uint64_t accepted_sequence)
{
    nuwax_ota_pending_txn_t record = {0};
    bool legacy = false;
    esp_err_t err = nvs_get_pending_txn(&record, &legacy);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pending OTA transaction is unreadable: %s",
                 esp_err_to_name(err));
        /* Preserve unreadable metadata. Erasing it could discard the only
         * durable sequence source after an image was already made valid. A
         * later start may atomically replace it; otherwise service tooling
         * can diagnose the retained NVS error. */
        return err;
    }

    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_app_desc_t *running_app = esp_app_get_description();
    bool matches_running = false;
    if (running_pending) {
        if (legacy) {
            return ESP_ERR_INVALID_CRC;
        }
        if (!nuwax_ota_release_pending_is_newer(record.release_sequence,
                                                accepted_sequence)) {
            ESP_LOGE(TAG,
                     "pending release sequence is not newer accepted=%llu pending=%llu",
                     (unsigned long long)accepted_sequence,
                     (unsigned long long)record.release_sequence);
            return ESP_ERR_INVALID_VERSION;
        }
        if (!nuwax_ota_release_running_meets_floor(
                NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE, accepted_sequence)) {
            ESP_LOGE(TAG,
                     "running firmware sequence is below accepted floor accepted=%llu firmware=%llu",
                     (unsigned long long)accepted_sequence,
                     (unsigned long long)NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE);
            return ESP_ERR_INVALID_VERSION;
        }
        /* Startup checks only the durable binding. The controller performs
         * the complete digest check before staged ACK / image confirmation;
         * audio and rollback interlocks stay closed until then. */
        matches_running = pending_txn_metadata_matches_running(
            &record, running_partition, running_app);
        if (!matches_running) {
            ESP_LOGE(TAG,
                     "pending image does not match OTA transaction identity");
            return ESP_ERR_INVALID_CRC;
        }
        ESP_LOGI(TAG, "pending image hash deferred to controller revision=ota-identity-v1");
        return ESP_OK;
    }

    if (!legacy) {
        err = pending_txn_matches_running(&record, running_partition,
                                          running_app, &matches_running);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!legacy && matches_running) {
        if (NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE < accepted_sequence) {
            ESP_LOGW(TAG,
                     "discarding stale lower pending identity accepted=%llu firmware=%llu",
                     (unsigned long long)accepted_sequence,
                     (unsigned long long)NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE);
            set_effective_release_sequence(accepted_sequence);
            err = nvs_clear_pending_txn();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "stale pending cleanup deferred: %s",
                         esp_err_to_name(err));
            }
            return err;
        }
        uint64_t recovered_sequence = accepted_sequence;
        if (NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE > recovered_sequence) {
            recovered_sequence = NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE;
        }
        set_effective_release_sequence(recovered_sequence);
        if (accepted_sequence < recovered_sequence) {
            err = nvs_set_sequence(NVS_KEY_ACCEPTED_SEQ, recovered_sequence);
            if (err != ESP_OK) {
                ESP_LOGE(TAG,
                         "deferred accepted sequence commit failed; recovery record retained: %s",
                         esp_err_to_name(err));
                strlcpy(s_status_text, "release_sequence_commit_deferred",
                        sizeof(s_status_text));
                return err;
            }
            ESP_LOGW(TAG, "recovered accepted release sequence=%llu",
                     (unsigned long long)recovered_sequence);
        }
        record.release_sequence = recovered_sequence;
        err = nvs_set_accepted_txn(&record);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "accepted image identity recovery failed: %s",
                     esp_err_to_name(err));
            strlcpy(s_status_text, "accepted_identity_commit_deferred",
                    sizeof(s_status_text));
            return err;
        }
    } else {
        ESP_LOGW(TAG,
                 "discarding stale pending OTA transaction after rollback/interruption");
    }

    err = nvs_clear_pending_txn();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pending transaction cleanup deferred: %s",
                 esp_err_to_name(err));
    }
    return err;
}

static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789abcdef";
    size_t used = 0;
    if (dst_size == 0U) {
        return 0U;
    }
    for (const unsigned char *p = (const unsigned char *)(src != NULL ? src : "");
         *p != 0U; ++p) {
        if (*p == '"' || *p == '\\') {
            if (used + 2U >= dst_size) {
                break;
            }
            dst[used++] = '\\';
            dst[used++] = (char)*p;
        } else if (*p < 0x20U) {
            if (used + 6U >= dst_size) {
                break;
            }
            dst[used++] = '\\';
            dst[used++] = 'u';
            dst[used++] = '0';
            dst[used++] = '0';
            dst[used++] = hex[*p >> 4U];
            dst[used++] = hex[*p & 0x0fU];
        } else {
            if (used + 1U >= dst_size) {
                break;
            }
            dst[used++] = (char)*p;
        }
    }
    dst[used] = '\0';
    return used;
}

static void emit_local(nuwax_ota_state_t state, int pct, const char *detail)
{
    nuwax_ota_main_callbacks_t callbacks;
    portENTER_CRITICAL(&s_lock);
    callbacks = s_callbacks;
    portEXIT_CRITICAL(&s_lock);
    if (callbacks.post_local_event == NULL) {
        return;
    }

    char escaped_detail[160];
    (void)json_escape(detail, escaped_detail, sizeof(escaped_detail));
    char json[320];
    int length = snprintf(json, sizeof(json),
                          "{\"type\":\"ota_local_progress\",\"state\":\"%s\","
                          "\"pct\":%d,\"detail\":\"%s\"}",
                          nuwax_ota_state_name(state), pct, escaped_detail);
    if (length > 0 && (size_t)length < sizeof(json)) {
        esp_err_t err = callbacks.post_local_event(callbacks.ctx, json);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "local progress callback failed: %s",
                     esp_err_to_name(err));
        }
    }
}

static void on_progress(void *ctx, nuwax_ota_state_t state, int pct,
                        const char *detail)
{
    (void)ctx;
    bool changed;
    portENTER_CRITICAL(&s_lock);
    changed = state != s_last_emitted_state || pct != s_last_emitted_pct;
    if (changed) {
        s_last_emitted_state = state;
        s_last_emitted_pct = pct;
    }
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        emit_local(state, pct, detail);
    }
}

static void on_state_change(void *ctx, nuwax_ota_state_t state,
                            const char *reason)
{
    (void)ctx;
    char persisted[128];
    if (state == NUWAX_OTA_STATE_DONE) {
        snprintf(persisted, sizeof(persisted), "staged:%s",
                 s_transport_manifest.version);
        (void)nvs_set_status(NVS_KEY_LAST, persisted);
    } else if (state == NUWAX_OTA_STATE_FAILED) {
        snprintf(persisted, sizeof(persisted), "failed:%s",
                 reason != NULL ? reason : "unknown");
        (void)nvs_set_status(NVS_KEY_LAST, persisted);
        esp_err_t clear_err = nvs_clear_pending_txn();
        if (clear_err != ESP_OK) {
            ESP_LOGE(TAG, "failed OTA pending transaction cleanup failed: %s",
                     esp_err_to_name(clear_err));
        }
    }
    emit_local(state, state == NUWAX_OTA_STATE_DONE ? 100 : 0, reason);
}

static nuwax_ota_callbacks_t s_ota_callbacks = {
    .ctx = NULL,
    .on_progress = on_progress,
    .on_state_change = on_state_change,
};

esp_err_t nuwax_ota_main_register(const nuwax_ota_main_callbacks_t *callbacks)
{
    if (callbacks == NULL || callbacks->post_local_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    s_callbacks = *callbacks;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t nuwax_ota_main_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    esp_err_t err = nuwax_ota_init();
    if (err != ESP_OK) {
        return err;
    }
    err = nuwax_ota_bind(&s_ota_callbacks);
    if (err != ESP_OK) {
        return err;
    }

    bool pending = false;
    err = nuwax_ota_running_image_pending_verify(&pending);
    if (err != ESP_OK) {
        return err;
    }
    if (pending) {
        strlcpy(s_status_text, "running_image_pending_verify",
                sizeof(s_status_text));
        (void)nvs_set_status(NVS_KEY_LAST, s_status_text);
        ESP_LOGW(TAG, "running image requires explicit health confirmation");
    } else {
        strlcpy(s_status_text, "ready", sizeof(s_status_text));
    }
    uint64_t accepted_sequence = NUWAX_OTA_BASE_RELEASE_SEQUENCE;
    err = nvs_get_accepted_sequence(&accepted_sequence);
    if (err != ESP_OK) {
        return err;
    }
    const uint64_t initial_sequence =
        nuwax_ota_release_floor(accepted_sequence);
    portENTER_CRITICAL(&s_lock);
    s_effective_release_sequence = initial_sequence;
    portEXIT_CRITICAL(&s_lock);
    err = reconcile_pending_txn(pending, accepted_sequence);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA transaction reconciliation failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t nuwax_ota_main_start(const char *manifest_json)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    bool transport_running = !s_transport_idle;
    portEXIT_CRITICAL(&s_lock);
    if (transport_running) {
        return ESP_ERR_INVALID_STATE;
    }
    nuwax_ota_state_t state = nuwax_ota_state();
    if (state != NUWAX_OTA_STATE_IDLE && state != NUWAX_OTA_STATE_FAILED) {
        return ESP_ERR_INVALID_STATE;
    }
    bool running_pending = false;
    esp_err_t err = nuwax_ota_running_image_pending_verify(&running_pending);
    if (err != ESP_OK || running_pending) {
        return err == ESP_OK ? ESP_ERR_OTA_ROLLBACK_INVALID_STATE : err;
    }

    /* A previous confirmation may have made the image valid while its
     * accepted sequence commit was temporarily unavailable. Reconcile that
     * record before allowing a new update to overwrite it. */
    uint64_t durable_sequence = NUWAX_OTA_BASE_RELEASE_SEQUENCE;
    err = nvs_get_accepted_sequence(&durable_sequence);
    if (err != ESP_OK) {
        return err;
    }
    err = reconcile_pending_txn(false, durable_sequence);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "accepted identity reconciliation required: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = nvs_get_accepted_sequence(&durable_sequence);
    if (err != ESP_OK ||
        !nuwax_ota_release_can_start(durable_sequence,
                                     effective_release_sequence())) {
        ESP_LOGE(TAG,
                 "new OTA blocked until accepted sequence is durable current=%llu effective=%llu err=%s",
                 (unsigned long long)durable_sequence,
                 (unsigned long long)effective_release_sequence(),
                 esp_err_to_name(err));
        (void)nvs_set_status(NVS_KEY_LAST,
                             "release_sequence_commit_required");
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    nuwax_ota_manifest_t manifest = {0};
    err = nuwax_ota_manifest_parse(
        manifest_json, manifest_json != NULL ? strlen(manifest_json) : 0U,
        &manifest);
    if (err != ESP_OK) {
        (void)nvs_set_status(NVS_KEY_LAST, "manifest_invalid");
        return err;
    }

    const esp_partition_t *target_partition =
        esp_ota_get_next_update_partition(NULL);
    if (target_partition == NULL) {
        (void)nvs_set_status(NVS_KEY_LAST, "ota_partition_not_found");
        return ESP_ERR_NOT_FOUND;
    }
    nuwax_ota_pending_txn_t pending_record = {0};
    err = make_pending_txn(&manifest, target_partition, &pending_record);
    if (err == ESP_OK) {
        err = nvs_set_pending_txn(&pending_record);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot persist pending OTA transaction: %s",
                 esp_err_to_name(err));
        (void)nvs_set_status(NVS_KEY_LAST, "pending_metadata_persist_failed");
        return err;
    }

    err = nuwax_ota_start(manifest_json, manifest.size_bytes);
    if (err != ESP_OK) {
        esp_err_t clear_err = nvs_clear_pending_txn();
        if (clear_err != ESP_OK) {
            ESP_LOGE(TAG, "cannot clear unstarted OTA transaction: %s",
                     esp_err_to_name(clear_err));
        }
        return err;
    }
    s_transport_manifest = manifest;
    nuwax_ota_set_target_version(manifest.version);
    nuwax_ota_set_target_sha256(manifest.image_sha256);
    (void)nvs_set_status(NVS_KEY_LAST, "downloading");

    portENTER_CRITICAL(&s_lock);
    s_transport_idle = false;
    s_transport_armed = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t nuwax_ota_main_run_transport(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    const bool armed = s_transport_armed && !s_transport_idle;
    if (armed) {
        s_transport_armed = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!armed) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "running OTA transport on controller task");
    esp_err_t err = nuwax_ota_transport_run(&s_transport_manifest);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA transport finished with %s", esp_err_to_name(err));
        const nuwax_ota_state_t state = nuwax_ota_state();
        if (state != NUWAX_OTA_STATE_DONE &&
            state != NUWAX_OTA_STATE_FAILED) {
            nuwax_ota_fail("transport_failed");
        }
    }

    /* The controller must not observe idle until HTTP and OTA handles have
     * been closed or aborted by nuwax_ota_transport_run(). */
    portENTER_CRITICAL(&s_lock);
    s_transport_idle = true;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t nuwax_ota_main_cancel(const char *reason)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return nuwax_ota_cancel(reason != NULL ? reason : "cancelled");
}

esp_err_t nuwax_ota_main_tick(uint32_t current_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)nuwax_ota_tick(current_ms);

    nuwax_ota_stats_t stats;
    nuwax_ota_get_stats(&stats);
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status_text, sizeof(s_status_text),
             "state=%s pct=%d bytes=%lu/%lu",
             nuwax_ota_state_name(stats.state), stats.pct,
             (unsigned long)stats.bytes_downloaded,
             (unsigned long)stats.total_bytes);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t nuwax_ota_main_reboot_staged_image(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return nuwax_ota_ack_and_reboot();
}

esp_err_t nuwax_ota_main_running_image_pending_verify(bool *out_pending)
{
    return nuwax_ota_running_image_pending_verify(out_pending);
}

esp_err_t nuwax_ota_main_confirm_running_image(void)
{
    bool pending = false;
    esp_err_t err = nuwax_ota_running_image_pending_verify(&pending);
    if (err != ESP_OK || !pending) {
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    nuwax_ota_pending_txn_t record = {0};
    bool legacy = false;
    err = nvs_get_pending_txn(&record, &legacy);
    if (err != ESP_OK) {
        return err;
    }
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_app_desc_t *running_app = esp_app_get_description();
    if (running_partition == NULL || running_app == NULL ||
        strcmp(record.version, running_app->version) != 0) {
        return ESP_ERR_INVALID_VERSION;
    }

    if (legacy) {
        ESP_LOGE(TAG, "legacy pending transaction has no verified SHA-256");
        return ESP_ERR_INVALID_CRC;
    }
    bool matches_running = false;
    err = pending_txn_matches_running(&record, running_partition, running_app,
                                      &matches_running);
    if (err != ESP_OK || !matches_running) {
        return err == ESP_OK ? ESP_ERR_INVALID_CRC : err;
    }

    /* Mark the image valid first. If power fails after this call, the intact
     * transaction record identifies the exact running partition and init()
     * completes the sequence commit on the next boot. If power fails before
     * this call, the bootloader rolls back and init() discards the record
     * without advancing the accepted sequence. */
    err = nuwax_ota_confirm_running_image();
    if (err != ESP_OK) {
        return err;
    }

    uint64_t committed_sequence = effective_release_sequence();
    if (NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE > committed_sequence) {
        committed_sequence = NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE;
    }
    set_effective_release_sequence(committed_sequence);
    err = nvs_set_sequence(NVS_KEY_ACCEPTED_SEQ, committed_sequence);
    if (err != ESP_OK) {
        /* The image is already valid and therefore cannot be rolled back.
         * Keep the transaction as a durable recovery source and fail closed
         * against older releases in RAM until the next boot retries NVS. */
        ESP_LOGE(TAG,
                 "accepted sequence commit deferred after image validation: %s",
                 esp_err_to_name(err));
        strlcpy(s_status_text, "release_sequence_commit_deferred",
                sizeof(s_status_text));
        return err;
    }

    record.release_sequence = committed_sequence;
    err = nvs_set_accepted_txn(&record);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "accepted image identity commit deferred: %s",
                 esp_err_to_name(err));
        strlcpy(s_status_text, "accepted_identity_commit_deferred",
                sizeof(s_status_text));
        return err;
    }

    esp_err_t clear_err = nvs_clear_pending_txn();
    if (clear_err != ESP_OK) {
        ESP_LOGE(TAG, "validated transaction cleanup deferred: %s",
                 esp_err_to_name(clear_err));
    }
    (void)nvs_set_status(NVS_KEY_LAST, "running_image_valid");
    strlcpy(s_status_text, "running_image_valid", sizeof(s_status_text));
    return ESP_OK;
}

esp_err_t nuwax_ota_main_rollback_running_image_and_reboot(void)
{
    (void)nvs_set_status(NVS_KEY_LAST, "rollback_requested");
    /* Keep the transaction until the previous image boots. Its partition
     * mismatch is the durable proof that rollback actually occurred. */
    return nuwax_ota_rollback_running_image_and_reboot();
}

esp_err_t nuwax_ota_main_get_release_sequence(uint64_t *out_sequence)
{
    if (out_sequence == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool initialized;
    portENTER_CRITICAL(&s_lock);
    initialized = s_initialized;
    *out_sequence = s_effective_release_sequence;
    portEXIT_CRITICAL(&s_lock);
    if (initialized) {
        return ESP_OK;
    }

    uint64_t accepted_sequence = NUWAX_OTA_BASE_RELEASE_SEQUENCE;
    const esp_err_t err = nvs_get_accepted_sequence(&accepted_sequence);
    if (err == ESP_OK) {
        *out_sequence = nuwax_ota_release_floor(accepted_sequence);
    }
    return err;
}

static esp_err_t get_running_identity(
    nuwax_ota_running_identity_t *out_identity, bool allow_compute)
{
    if (out_identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_identity, 0, sizeof(*out_identity));

    bool pending = false;
    esp_err_t err = nuwax_ota_running_image_pending_verify(&pending);
    if (err != ESP_OK) {
        return err;
    }

    nuwax_ota_pending_txn_t record = {0};
    if (pending) {
        bool legacy = false;
        err = nvs_get_pending_txn(&record, &legacy);
        if (err != ESP_OK) {
            return err;
        }
        if (legacy) {
            return ESP_ERR_INVALID_CRC;
        }
    } else {
        uint64_t accepted_sequence = NUWAX_OTA_BASE_RELEASE_SEQUENCE;
        err = nvs_get_accepted_sequence(&accepted_sequence);
        if (err != ESP_OK) {
            return err;
        }
        if (allow_compute) {
            err = reconcile_pending_txn(false, accepted_sequence);
            if (err != ESP_OK) {
                return err;
            }
        }
        err = nvs_get_accepted_txn(&record);
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_get_accepted_sequence(&accepted_sequence);
        if (err != ESP_OK) {
            return err;
        }
        if (record.release_sequence != accepted_sequence) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_app_desc_t *running_app = esp_app_get_description();
    bool matches_running;
    if (pending) {
        matches_running = false;
        if (!pending_txn_metadata_matches_running(&record, running_partition, running_app)) {
            return ESP_ERR_INVALID_CRC;
        }
        char digest[NUWAX_OTA_MANIFEST_SHA256_LEN] = {0};
        err = running_partition_sha256(running_partition, record.image_size,
                                       digest, allow_compute);
        if (err != ESP_OK) {
            return err;
        }
        matches_running = strcmp(record.image_sha256, digest) == 0;
    } else {
        /* A valid running image was already verified by esp_ota_end(), the
         * bootloader and the confirmation transaction. Re-hashing the entire
         * multi-megabyte image on every WebSocket hello only stalls reconnects
         * and adds no protection against an untrusted flash writer. */
        matches_running = pending_txn_metadata_matches_running(
            &record, running_partition, running_app);
    }
    if (!matches_running) {
        return ESP_ERR_INVALID_CRC;
    }

    strlcpy(out_identity->image_sha256, record.image_sha256,
            sizeof(out_identity->image_sha256));
    out_identity->release_sequence = record.release_sequence;
    out_identity->target_partition_address = record.target_partition_address;
    out_identity->pending_verify = pending;
    return ESP_OK;
}

esp_err_t nuwax_ota_main_get_running_identity(nuwax_ota_running_identity_t *out_identity)
{
    return get_running_identity(out_identity, true);
}

esp_err_t nuwax_ota_main_peek_running_identity(nuwax_ota_running_identity_t *out_identity)
{
    return get_running_identity(out_identity, false);
}

esp_err_t nuwax_ota_main_pending_identity_matches(
    uint32_t target_partition_address,
    const char *image_sha256,
    bool *out_matches)
{
    if (target_partition_address == 0U || image_sha256 == NULL ||
        out_matches == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_matches = false;
    nuwax_ota_pending_txn_t record = {0};
    bool legacy = false;
    esp_err_t err = nvs_get_pending_txn(&record, &legacy);
    if (err != ESP_OK) {
        return err;
    }
    if (legacy) {
        return ESP_ERR_INVALID_CRC;
    }
    if (record.target_partition_address != target_partition_address ||
        strcmp(record.image_sha256, image_sha256) != 0) {
        return ESP_OK;
    }
    return pending_txn_matches_running(
        &record, esp_ota_get_running_partition(), esp_app_get_description(),
        out_matches);
}

esp_err_t nuwax_ota_main_accepted_identity_matches(
    uint32_t target_partition_address,
    const char *image_sha256,
    bool *out_matches)
{
    if (target_partition_address == 0U || image_sha256 == NULL ||
        out_matches == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_matches = false;
    nuwax_ota_running_identity_t identity = {0};
    esp_err_t err = nuwax_ota_main_get_running_identity(&identity);
    if (err != ESP_OK) {
        return err;
    }
    *out_matches = !identity.pending_verify &&
                   identity.target_partition_address == target_partition_address &&
                   strcmp(identity.image_sha256, image_sha256) == 0;
    return ESP_OK;
}

bool nuwax_ota_main_worker_idle(void)
{
    bool idle;
    portENTER_CRITICAL(&s_lock);
    idle = s_transport_idle;
    portEXIT_CRITICAL(&s_lock);
    return idle;
}

bool nuwax_ota_main_terminal_complete(void)
{
    if (!nuwax_ota_main_worker_idle()) {
        return false;
    }
    const nuwax_ota_state_t state = nuwax_ota_state();
    return state == NUWAX_OTA_STATE_DONE || state == NUWAX_OTA_STATE_FAILED;
}

const char *nuwax_ota_main_last_status(void)
{
    return s_status_text;
}
