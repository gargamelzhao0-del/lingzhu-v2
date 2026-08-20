/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nuwax_device_uid.c — implementation, see header for design notes.
 */
#include "nuwax_device_uid.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nuwax_uid";

static const char *const NVS_NAMESPACE = "nuwax_uid";
static const char *const NVS_KEY       = "device_uid_v1";
static const char *const QR_PREFIX     = "NUWAX7:";

static nuwax_device_uid_t s_uid;
static bool               s_initialised;

/* ---------------- helpers ---------------- */

static void format_uid_string(char out[NUWAX_DEVICE_UID_STRING_LEN]) {
    snprintf(out, NUWAX_DEVICE_UID_STRING_LEN,
             "NX-%02X%02X-%02X%02X-%02X%02X",
             s_uid.raw[0], s_uid.raw[1], s_uid.raw[2],
             s_uid.raw[3], s_uid.raw[4], s_uid.raw[5]);
}

static void format_qr_payload(char out[NUWAX_DEVICE_UID_QR_LEN]) {
    snprintf(out, NUWAX_DEVICE_UID_QR_LEN,
             "%s%s", QR_PREFIX, s_uid.uid_string);
}

static esp_err_t fill_uid_from_eFuse(void) {
    uint8_t entropy[16] = {0};
    uint8_t hash[32] = {0};

    /* IDF 6.0 exposes the factory base MAC as the stable public chip identity.
     * Hash it with a product-domain separator rather than exposing it raw. */
    esp_err_t err = esp_efuse_mac_get_default(entropy);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_efuse_mac_get_default failed (%s)",
                 esp_err_to_name(err));
        err = esp_read_mac(entropy, ESP_MAC_WIFI_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "stable device MAC unavailable (%s)",
                     esp_err_to_name(err));
            return err;
        }
    }
    memcpy(entropy + 6, "NUWAX7-UID", 10);

    size_t hash_len = 0;
    psa_status_t status = psa_crypto_init();
    if (status == PSA_SUCCESS) {
        status = psa_hash_compute(PSA_ALG_SHA_256, entropy, sizeof(entropy),
                                  hash, sizeof(hash), &hash_len);
    }
    if (status != PSA_SUCCESS || hash_len != sizeof(hash)) {
        ESP_LOGE(TAG, "SHA-256 failed (%ld); refusing weak device identity",
                 (long)status);
        return ESP_FAIL;
    }
    memcpy(s_uid.raw, hash, sizeof(s_uid.raw));
    s_uid.from_nvs_cache = false;
    return ESP_OK;
}

static esp_err_t persist_uid_to_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) for write failed: %s",
                 NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }
    char hex[sizeof(s_uid.raw) * 2 + 1] = {0};
    for (size_t i = 0; i < sizeof(s_uid.raw); i++) {
        snprintf(hex + 2 * i, 3, "%02x", s_uid.raw[i]);
    }
    err = nvs_set_str(h, NVS_KEY, hex);
    if (err == ESP_OK) {
        err = nvs_commit(h);   /* explicit — write-once */
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "device_uid persisted to NVS: %.32s", hex);
    } else {
        ESP_LOGW(TAG, "nvs commit for device_uid failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t load_uid_from_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t required = 0;
    err = nvs_get_str(h, NVS_KEY, NULL, &required);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    if (required != sizeof(s_uid.raw) * 2 + 1) {
        nvs_close(h);
        return ESP_ERR_INVALID_SIZE;
    }
    char hex[sizeof(s_uid.raw) * 2 + 1] = {0};
    err = nvs_get_str(h, NVS_KEY, hex, &required);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < sizeof(s_uid.raw); i++) {
        unsigned int byte = 0;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1) {
            return ESP_FAIL;
        }
        s_uid.raw[i] = (uint8_t)byte;
    }
    s_uid.from_nvs_cache = true;
    return ESP_OK;
}

/* ---------------- public API ---------------- */

esp_err_t nuwax_device_uid_init(void) {
    if (s_initialised) {
        return ESP_OK;
    }
    memset(&s_uid, 0, sizeof(s_uid));

    esp_err_t err = load_uid_from_nvs();
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS read failed (%s), re-deriving from eFuse",
                     esp_err_to_name(err));
        }
        ESP_RETURN_ON_ERROR(fill_uid_from_eFuse(), TAG,
                            "derive stable device UID failed");
        /* Persistence is best-effort: even if it fails the in-memory copy
         * is valid and we can recover on next boot. */
        (void)persist_uid_to_nvs();
    }

    format_uid_string(s_uid.uid_string);
    format_qr_payload(s_uid.qr_payload);
    s_initialised = true;

    ESP_LOGI(TAG, "device uid: %s (source=%s)",
             s_uid.uid_string, s_uid.from_nvs_cache ? "NVS" : "eFuse");
    return ESP_OK;
}

const char *nuwax_device_uid_string(void) {
    return s_initialised ? s_uid.uid_string : "";
}

const char *nuwax_device_uid_qr_payload(void) {
    return s_initialised ? s_uid.qr_payload : "";
}

const nuwax_device_uid_t *nuwax_device_uid_get(void) {
    return s_initialised ? &s_uid : NULL;
}
