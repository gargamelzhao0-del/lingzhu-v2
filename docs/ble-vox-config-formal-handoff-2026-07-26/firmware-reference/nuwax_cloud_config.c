#include "nuwax_cloud_config.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

#include "nuwax_device_uid.h"

static const char *TAG = "nuwax_cloud_cfg";
static const char *const NVS_NAMESPACE = "nuwax_cloud";
static const char *const NVS_ACTIVE_KEY = "active";
static const char *const NVS_LEGACY_PENDING_KEY = "pending";

static bool s_initialized;
static bool s_has_config;
static nuwax_cloud_config_t s_config;

static bool string_is_terminated(const char *value, size_t capacity) {
    return value && memchr(value, '\0', capacity) != NULL;
}

static bool valid_token(const char *value, size_t capacity) {
    if (!string_is_terminated(value, capacity) || value[0] == '\0') {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; ++cursor) {
        if (*cursor <= 0x20 || *cursor > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool valid_cloud_url(const char *url) {
    if (!string_is_terminated(url, NUWAX_CLOUD_URL_MAX)) {
        return false;
    }
    const char *authority = NULL;
    if (strncmp(url, "wss://", 6) == 0) {
        authority = url + 6;
#if CONFIG_NUWAX_CLOUD_CONFIG_ALLOW_INSECURE_WS
    } else if (strncmp(url, "ws://", 5) == 0) {
        authority = url + 5;
#endif
    } else {
        return false;
    }
    if (!authority[0] || strchr(authority, '#')) {
        return false;
    }
    const char *authority_end = strpbrk(authority, "/?");
    if (!authority_end) {
        authority_end = authority + strlen(authority);
    }
    const size_t authority_len = (size_t)(authority_end - authority);
    if (authority_len == 0 || authority_len >= 128 ||
        memchr(authority, '@', authority_len) ||
        memchr(authority, '[', authority_len) ||
        memchr(authority, ']', authority_len)) {
        return false;
    }
    const char *port_separator = memchr(authority, ':', authority_len);
    size_t host_len = authority_len;
    if (port_separator) {
        host_len = (size_t)(port_separator - authority);
        const char *port = port_separator + 1;
        if (host_len == 0 || port == authority_end ||
            memchr(port, ':', (size_t)(authority_end - port))) {
            return false;
        }
        unsigned port_value = 0;
        for (const char *cursor = port; cursor < authority_end; ++cursor) {
            if (*cursor < '0' || *cursor > '9') {
                return false;
            }
            port_value = port_value * 10U + (unsigned)(*cursor - '0');
            if (port_value > 65535U) {
                return false;
            }
        }
        if (port_value == 0) {
            return false;
        }
    }
    if (host_len == 0 || authority[0] == '.' || authority[host_len - 1] == '.') {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)url; *cursor; ++cursor) {
        if (*cursor <= 0x20 || *cursor > 0x7e) {
            return false;
        }
    }
    return true;
}

static esp_err_t validate_config(const nuwax_cloud_config_t *config) {
    if (!config || config->version != 1 ||
        !valid_token(config->device_id, sizeof(config->device_id)) ||
        strcmp(config->device_id, nuwax_device_uid_string()) != 0 ||
        !valid_cloud_url(config->cloud_url) ||
        !valid_token(config->device_secret, sizeof(config->device_secret))) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool recoverable_slot_error(esp_err_t err) {
    return err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NVS_INVALID_LENGTH;
}

esp_err_t nuwax_cloud_config_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(nuwax_device_uid_init(), TAG, "device UID init failed");

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG,
                        "open cloud config NVS failed");

    size_t length = sizeof(s_config);
    memset(&s_config, 0, sizeof(s_config));
    esp_err_t read_err = nvs_get_blob(nvs, NVS_ACTIVE_KEY, &s_config, &length);
    if (read_err == ESP_OK &&
        (length != sizeof(s_config) || validate_config(&s_config) != ESP_OK)) {
        read_err = ESP_ERR_INVALID_STATE;
    }

    bool changed = false;
    if (read_err == ESP_ERR_NVS_NOT_FOUND) {
        read_err = ESP_OK;
    } else if (recoverable_slot_error(read_err)) {
        ESP_LOGW(TAG, "erasing incompatible legacy cloud configuration");
        esp_err_t erase_err = nvs_erase_key(nvs, NVS_ACTIVE_KEY);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(nvs);
            return erase_err;
        }
        memset(&s_config, 0, sizeof(s_config));
        changed = true;
        read_err = ESP_OK;
    }

    esp_err_t legacy_err = nvs_erase_key(nvs, NVS_LEGACY_PENDING_KEY);
    if (legacy_err == ESP_OK) {
        changed = true;
    } else if (legacy_err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return legacy_err;
    }
    if (changed) {
        read_err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(read_err, TAG, "read cloud config failed");

    s_has_config = s_config.version == 1;
    s_initialized = true;
    ESP_LOGI(TAG, "dynamic cloud config ready active=%d", s_has_config);
    return ESP_OK;
}

esp_err_t nuwax_cloud_config_set(const nuwax_cloud_config_t *config) {
    ESP_RETURN_ON_ERROR(nuwax_cloud_config_init(), TAG, "cloud config init failed");
    ESP_RETURN_ON_ERROR(validate_config(config), TAG, "cloud config validation failed");

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG,
                        "open cloud config NVS failed");
    esp_err_t err = nvs_set_blob(nvs, NVS_ACTIVE_KEY, config, sizeof(*config));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    s_config = *config;
    s_has_config = true;
    ESP_LOGI(TAG, "dynamic cloud config updated device=%s url=%s",
             config->device_id, config->cloud_url);
    return ESP_OK;
}

esp_err_t nuwax_cloud_config_get(nuwax_cloud_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(nuwax_cloud_config_init(), TAG, "cloud config init failed");
    if (!s_has_config) {
        return ESP_ERR_NOT_FOUND;
    }
    *config = s_config;
    return ESP_OK;
}

bool nuwax_cloud_config_has_config(void) {
    return nuwax_cloud_config_init() == ESP_OK && s_has_config;
}
