#include "nuwax_pair_commit.h"

#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_check.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "wifi_manager.h"
#include "nuwax_device_uid.h"

static const char *TAG = "nuwax_pair_commit";
#define PAIR_NVS_NAMESPACE "nuwax_reprov"
#define PAIR_NVS_KEY "commit_pending"
#define PAIR_REQUEST_KEY "request"
#define PAIR_COMMIT_MAGIC 0x4e585043U
#define PAIR_COMMIT_VERSION 1U

/* The old 8-byte Wi-Fi-only proof cannot establish the cloud generation.
 * Keep it (and malformed records) as a durable APP-repair fence until a fresh
 * validated APP pair replaces it. Never infer success from pending absence. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t transaction_id;
    wifi_config_t wifi;
    nuwax_cloud_config_t cloud;
    uint32_t crc32;
} pair_commit_record_t;

static pair_commit_record_t s_target;
static atomic_bool s_commit_pending = ATOMIC_VAR_INIT(false);
static atomic_bool s_repair_required = ATOMIC_VAR_INIT(false);

bool nuwax_pair_commit_pending(void) {
    return atomic_load_explicit(&s_commit_pending, memory_order_acquire);
}

bool nuwax_pair_commit_requires_app(void) {
    return atomic_load_explicit(&s_repair_required, memory_order_acquire);
}

bool nuwax_pair_commit_blocks_rollback(void) {
    return nuwax_pair_commit_pending() || nuwax_pair_commit_requires_app();
}

esp_err_t nuwax_pair_commit_require_app(void) {
    if (nuwax_pair_commit_pending()) return ESP_ERR_INVALID_STATE;
    if (nuwax_pair_commit_requires_app()) return ESP_OK;
    /* An intentionally incomplete record is a durable repair fence. It is
     * replaced only by a new complete target after verified APP provisioning. */
    const uint32_t repair[2] = {PAIR_COMMIT_MAGIC, 0U};
    atomic_store_explicit(&s_repair_required, true, memory_order_release);
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(PAIR_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG,
                        "open pair repair fence failed");
    esp_err_t err = nvs_set_blob(nvs, PAIR_NVS_KEY, repair, sizeof(repair));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static bool valid_record(const pair_commit_record_t *r) {
    return r->magic == PAIR_COMMIT_MAGIC && r->version == PAIR_COMMIT_VERSION &&
        r->wifi.sta.ssid[0] != '\0' &&
        r->crc32 == esp_crc32_le(0, (const uint8_t *)r,
                                offsetof(pair_commit_record_t, crc32)) &&
        nuwax_cloud_config_validate(&r->cloud) == ESP_OK;
}

esp_err_t nuwax_pair_commit_load(void) {
    /* Do not replace an in-flight RAM target after an uncertain NVS error. */
    if (nuwax_pair_commit_pending()) return ESP_OK;
    ESP_RETURN_ON_ERROR(nuwax_device_uid_init(), TAG, "initialize pair identity failed");
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PAIR_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    size_t length = 0;
    err = nvs_get_blob(nvs, PAIR_NVS_KEY, NULL, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return ESP_OK;
    }
    bool invalid = err == ESP_OK && length != sizeof(s_target);
    if (err == ESP_OK && !invalid) {
        err = nvs_get_blob(nvs, PAIR_NVS_KEY, &s_target, &length);
        invalid = err == ESP_OK && !valid_record(&s_target);
    }
    nvs_close(nvs);
    if (err != ESP_OK) return err; /* I/O failure is not absence or corruption. */
    if (invalid) {
        memset(&s_target, 0, sizeof(s_target));
        atomic_store_explicit(&s_repair_required, true, memory_order_release);
        ESP_LOGW(TAG, "incomplete legacy/corrupt pair record requires fresh APP provisioning");
    } else {
        atomic_store_explicit(&s_commit_pending, true, memory_order_release);
        ESP_LOGW(TAG, "recoverable Wi-Fi/cloud pair found transaction=%08lx",
                 (unsigned long)s_target.transaction_id);
    }
    return ESP_OK;
}

esp_err_t nuwax_pair_commit_begin(const wifi_config_t *wifi,
                                  const nuwax_cloud_config_t *cloud) {
    if (!wifi || !wifi->sta.ssid[0] || nuwax_pair_commit_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(nuwax_cloud_config_validate(cloud), TAG, "invalid APP cloud pair");
    memset(&s_target, 0, sizeof(s_target));
    s_target.magic = PAIR_COMMIT_MAGIC;
    s_target.version = PAIR_COMMIT_VERSION;
    s_target.transaction_id = esp_random();
    s_target.wifi = *wifi;
    s_target.cloud = *cloud;
    s_target.crc32 = esp_crc32_le(0, (const uint8_t *)&s_target,
                                offsetof(pair_commit_record_t, crc32));
    /* Fence cancellation even if persistence reports an error after writing. */
    atomic_store_explicit(&s_commit_pending, true, memory_order_release);
    return ESP_OK;
}

static esp_err_t persist_target(void) {
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(PAIR_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG,
                        "open pair journal failed");
    esp_err_t err = nvs_set_blob(nvs, PAIR_NVS_KEY, &s_target, sizeof(s_target));
    if (err == ESP_OK) err = nvs_commit(nvs);
    pair_commit_record_t actual = {0};
    size_t length = sizeof(actual);
    if (err == ESP_OK) err = nvs_get_blob(nvs, PAIR_NVS_KEY, &actual, &length);
    if (err == ESP_OK && (length != sizeof(actual) ||
                         memcmp(&actual, &s_target, sizeof(actual)) != 0)) {
        err = ESP_ERR_INVALID_STATE;
    }
    memset(&actual, 0, sizeof(actual));
    nvs_close(nvs);
    return err;
}

static esp_err_t retire_target(void) {
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(PAIR_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG,
                        "open pair retirement failed");
    /* Each key is durable separately. Retire rollback FIRST, journal LAST.
     * Recovery must find the journal even when request has already gone. */
    esp_err_t err = nvs_erase_key(nvs, PAIR_REQUEST_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (err == ESP_OK) err = nvs_erase_key(nvs, PAIR_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t nuwax_pair_commit_complete(
    esp_err_t (*mark_verified)(const wifi_config_t *),
    bool (*marker_matches)(const wifi_config_t *)) {
    if (!nuwax_pair_commit_pending() || !mark_verified || !marker_matches ||
        !valid_record(&s_target)) return ESP_ERR_INVALID_STATE;
    /* First durable write: the FULL target, including initial provisioning.
     * Reapplying the same target after any later cut is safe and idempotent. */
    ESP_RETURN_ON_ERROR(persist_target(), TAG, "persist pair intent failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG,
                        "select committed Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &s_target.wifi), TAG,
                        "apply committed Wi-Fi failed");
    ESP_RETURN_ON_ERROR(nuwax_wifi_import_driver_credentials(), TAG,
                        "persist committed application Wi-Fi failed");
    ESP_RETURN_ON_ERROR(nuwax_cloud_config_set(&s_target.cloud), TAG,
                        "persist committed cloud pair failed");
    ESP_RETURN_ON_ERROR(mark_verified(&s_target.wifi), TAG,
                        "mark committed Wi-Fi verified failed");
    wifi_config_t actual_wifi = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &actual_wifi), TAG,
                        "read back committed Wi-Fi failed");
    char ssid[sizeof(s_target.wifi.sta.ssid) + 1] = {0};
    char password[sizeof(s_target.wifi.sta.password) + 1] = {0};
    memcpy(ssid, s_target.wifi.sta.ssid, sizeof(s_target.wifi.sta.ssid));
    memcpy(password, s_target.wifi.sta.password, sizeof(s_target.wifi.sta.password));
    const bool wifi_matches =
        memcmp(actual_wifi.sta.ssid, s_target.wifi.sta.ssid,
               sizeof(actual_wifi.sta.ssid)) == 0 &&
        memcmp(actual_wifi.sta.password, s_target.wifi.sta.password,
               sizeof(actual_wifi.sta.password)) == 0 &&
        nuwax_wifi_saved_credentials_match(ssid, password) &&
        marker_matches(&s_target.wifi);
    memset(password, 0, sizeof(password));
    if (!wifi_matches) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(nuwax_cloud_config_verify_persisted(&s_target.cloud), TAG,
                        "read back committed cloud pair failed");
    ESP_RETURN_ON_ERROR(retire_target(), TAG, "retire committed pair failed");
    ESP_LOGI(TAG, "Wi-Fi/cloud pair committed transaction=%08lx",
             (unsigned long)s_target.transaction_id);
    memset(&s_target, 0, sizeof(s_target));
    atomic_store_explicit(&s_repair_required, false, memory_order_release);
    atomic_store_explicit(&s_commit_pending, false, memory_order_release);
    return ESP_OK;
}
