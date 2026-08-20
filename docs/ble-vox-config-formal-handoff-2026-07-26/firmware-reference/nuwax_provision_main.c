#include "nuwax_provision_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_srp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs.h"
#include "protocomm.h"
#include "protocomm_security.h"

#include "nuwax_device_uid.h"
#include "nuwax_cloud_config.h"

static const char *TAG = "nuwax_provision";
static const char *const DEVICE_INFO_EP = "device-info";
static const char *const VOX_CONFIG_EP = "vox-config";
#define CREDENTIAL_OBSERVATION_TIMEOUT_US 30000000LL
#ifndef CONFIG_NUWAX_PROVISION_SUCCESS_BLE_HOLD_MS
#define CONFIG_NUWAX_PROVISION_SUCCESS_BLE_HOLD_MS 15000
#endif
static bool s_initialized;
static bool s_started;
static bool s_wifi_provisioned;
static char s_service_name[16];
static char s_qr_payload[256];

typedef struct {
    bool got_ip;
    bool has_wifi_disconnect_reason;
    uint8_t wifi_disconnect_reason;
    int64_t credentials_received_us;
    nuwax_provision_outcome_t outcome;
    char reason[48];
} provision_state_t;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static provision_state_t s_state = {
    .outcome = NUWAX_PROVISION_OUTCOME_NONE,
};

typedef struct {
    uint8_t *input;
    ssize_t input_len;
    uint8_t *output;
    ssize_t output_len;
    esp_err_t result;
    SemaphoreHandle_t done;
} vox_config_job_t;

static QueueHandle_t s_vox_config_jobs;
static SemaphoreHandle_t s_vox_config_gate;
static bool s_accept_vox_config_jobs;

#define REPROVISION_NVS_NAMESPACE "nuwax_reprov"
#define REPROVISION_NVS_KEY "request"
#define REPROVISION_RECORD_MAGIC 0x4e585250U

typedef struct {
    uint32_t magic;
    wifi_config_t wifi_config;
} reprovision_record_t;

static bool s_force_reprovision;
static reprovision_record_t s_reprovision_record;

/* Development-only credentials. Production builds must replace this path with
 * factory-injected salt/verifier and must never embed the PoP in firmware. */
#ifndef CONFIG_NUWAX_PROVISION_SEC2_DEV_POP
#define CONFIG_NUWAX_PROVISION_SEC2_DEV_POP "Nuwax7-Dev-PoP"
#endif

static char *s_sec2_salt;
static char *s_sec2_verifier;
static int s_sec2_salt_len;
static int s_sec2_verifier_len;
static network_prov_security2_params_t s_sec2_params;
#if !CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE
static char s_factory_salt[32];
static char s_factory_verifier[384];
#endif

static esp_err_t build_qr_payload(void) {
    const char *username = nuwax_device_uid_string();
    if (!username[0] || !s_service_name[0]) {
        s_qr_payload[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    bool json_ok = cJSON_AddStringToObject(root, "ver", "v1") != NULL &&
                   cJSON_AddStringToObject(root, "name", s_service_name) != NULL &&
                   cJSON_AddStringToObject(root, "transport", "ble") != NULL &&
                   cJSON_AddStringToObject(root, "network", "wifi") != NULL &&
                   cJSON_AddNumberToObject(root, "security", 2) != NULL &&
                   cJSON_AddStringToObject(root, "username", username) != NULL;
#if CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE
    json_ok = json_ok &&
              cJSON_AddStringToObject(root, "pop",
                                      CONFIG_NUWAX_PROVISION_SEC2_DEV_POP) != NULL;
#else
    /* Production devices retain only the SRP salt/verifier. The plaintext PoP
     * remains on the controlled factory label and is never reconstructed here. */
#endif
    if (!json_ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!encoded) {
        return ESP_ERR_NO_MEM;
    }
    const size_t encoded_len = strlen(encoded);
    if (encoded_len >= sizeof(s_qr_payload)) {
        cJSON_free(encoded);
        s_qr_payload[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_qr_payload, encoded, encoded_len + 1);
    cJSON_free(encoded);
    return ESP_OK;
}

static void set_reason_locked(const char *reason) {
    strlcpy(s_state.reason, reason ? reason : "-", sizeof(s_state.reason));
}

static provision_state_t get_state_snapshot(void) {
    provision_state_t snapshot;
    portENTER_CRITICAL(&s_state_lock);
    snapshot = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return snapshot;
}

static void reset_provision_state(const char *reason) {
    portENTER_CRITICAL(&s_state_lock);
    memset(&s_state, 0, sizeof(s_state));
    s_state.outcome = NUWAX_PROVISION_OUTCOME_NONE;
    set_reason_locked(reason);
    portEXIT_CRITICAL(&s_state_lock);
}

static nuwax_provision_outcome_t map_managed_fail_reason(
    network_prov_wifi_sta_fail_reason_t reason) {
    if (reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) {
        return NUWAX_PROVISION_OUTCOME_AUTH_FAILED;
    }
    if (reason == NETWORK_PROV_WIFI_STA_AP_NOT_FOUND) {
        return NUWAX_PROVISION_OUTCOME_NETWORK_NOT_FOUND;
    }
    return NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
}

static nuwax_provision_outcome_t map_raw_wifi_fail_reason(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return NUWAX_PROVISION_OUTCOME_NETWORK_NOT_FOUND;
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return NUWAX_PROVISION_OUTCOME_AUTH_FAILED;
    default:
        return NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
    }
}

static const char *raw_wifi_reason_name(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND: return "no_ap_found";
    case WIFI_REASON_AUTH_EXPIRE: return "auth_expire";
    case WIFI_REASON_AUTH_FAIL: return "auth_fail";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "four_way_handshake_timeout";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake_timeout";
    case WIFI_REASON_MIC_FAILURE: return "mic_failure";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1x_auth_failed";
    default: return "unmapped";
    }
}

static void provisioning_manager_app_event_handler(void *user_data,
                                                   network_prov_cb_event_t event,
                                                   void *event_data) {
    (void)user_data;
    if (event != NETWORK_PROV_SET_WIFI_STA_CONFIG || !event_data) {
        return;
    }

    wifi_config_t *wifi_config = (wifi_config_t *)event_data;
    const size_t ssid_len = strnlen((const char *)wifi_config->sta.ssid,
                                    sizeof(wifi_config->sta.ssid));
    const size_t password_len = strnlen((const char *)wifi_config->sta.password,
                                        sizeof(wifi_config->sta.password));

    /* The device-list scan is completed before credentials are applied. Use
     * the same fast connection scan as the proven normal STA path here so a
     * transition-mode SSID with multiple BSSIDs is not re-selected by a second
     * full scan immediately before authentication. */
    wifi_config->sta.scan_method = WIFI_FAST_SCAN;
    wifi_config->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
#if CONFIG_ESP_WIFI_ENABLE_WPA3_SAE
    /* A zero-initialized IDF 6.0 wifi_config_t leaves SAE PWE unspecified.
     * Explicitly accept both WPA3 derivation methods for transition-mode APs. */
    wifi_config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#endif
#ifdef CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
    /* Field validation on the target AP showed that IDF's WPA3 compatibility
     * path can stall in 802.11 authentication.  A transition-mode AP still
     * accepts the proven WPA2 association path when this override is disabled. */
    wifi_config->sta.disable_wpa3_compatible_mode = 1;
#endif
    ESP_LOGI(TAG,
             "STA provisioning policy applied ssid_len=%u password_len=%u scan=%d wpa3_compat_disabled=%u internal_free=%u internal_largest=%u",
             (unsigned)ssid_len, (unsigned)password_len,
             (int)wifi_config->sta.scan_method,
             (unsigned)wifi_config->sta.disable_wpa3_compatible_mode,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static const char *outcome_name(nuwax_provision_outcome_t outcome) {
    switch (outcome) {
    case NUWAX_PROVISION_OUTCOME_SUCCESS: return "success";
    case NUWAX_PROVISION_OUTCOME_AUTH_FAILED: return "auth_failed";
    case NUWAX_PROVISION_OUTCOME_NETWORK_NOT_FOUND: return "network_not_found";
    case NUWAX_PROVISION_OUTCOME_TIMEOUT: return "timeout";
    case NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR: return "internal_error";
    case NUWAX_PROVISION_OUTCOME_STOPPED: return "stopped";
    case NUWAX_PROVISION_OUTCOME_NONE:
    default: return "none";
    }
}

static void provisioning_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data) {
    (void)arg;
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            portENTER_CRITICAL(&s_state_lock);
            set_reason_locked("started");
            portEXIT_CRITICAL(&s_state_lock);
            break;
        case NETWORK_PROV_WIFI_CRED_RECV:
            portENTER_CRITICAL(&s_state_lock);
            s_state.credentials_received_us = esp_timer_get_time();
            s_state.has_wifi_disconnect_reason = false;
            s_state.outcome = NUWAX_PROVISION_OUTCOME_NONE;
            set_reason_locked("credentials_received");
            portEXIT_CRITICAL(&s_state_lock);
            break;
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            provision_state_t snapshot = get_state_snapshot();
            nuwax_provision_outcome_t outcome =
                NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
            char failure_reason[48] = "wifi_failure_missing_reason";
            if (snapshot.has_wifi_disconnect_reason) {
                outcome = map_raw_wifi_fail_reason(snapshot.wifi_disconnect_reason);
                snprintf(failure_reason, sizeof(failure_reason), "wifi_reason_%u_%s",
                         (unsigned)snapshot.wifi_disconnect_reason,
                         raw_wifi_reason_name(snapshot.wifi_disconnect_reason));
            } else if (event_data) {
                const network_prov_wifi_sta_fail_reason_t managed_reason =
                    *(network_prov_wifi_sta_fail_reason_t *)event_data;
                outcome = map_managed_fail_reason(managed_reason);
                strlcpy(failure_reason, outcome_name(outcome),
                        sizeof(failure_reason));
            }
            portENTER_CRITICAL(&s_state_lock);
            s_state.outcome = outcome;
            s_state.credentials_received_us = 0;
            set_reason_locked(failure_reason);
            portEXIT_CRITICAL(&s_state_lock);
            /* Keep the official service alive so the app can retry in the
             * same BLE session and query the latest status. */
            (void)network_prov_mgr_reset_wifi_sm_state_on_failure();
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            portENTER_CRITICAL(&s_state_lock);
            s_state.got_ip = true;
            s_state.outcome = NUWAX_PROVISION_OUTCOME_SUCCESS;
            s_state.credentials_received_us = 0;
            set_reason_locked("dhcp_success");
            portEXIT_CRITICAL(&s_state_lock);
            break;
        case NETWORK_PROV_END:
            break;
        default:
            break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED &&
               event_data) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        portENTER_CRITICAL(&s_state_lock);
        if (s_state.credentials_received_us != 0) {
            s_state.has_wifi_disconnect_reason = true;
            s_state.wifi_disconnect_reason = disconnected->reason;
        }
        portEXIT_CRITICAL(&s_state_lock);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        portENTER_CRITICAL(&s_state_lock);
        s_state.got_ip = true;
        portEXIT_CRITICAL(&s_state_lock);
    } else if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        portENTER_CRITICAL(&s_state_lock);
        if (event_id == PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH) {
            s_state.outcome = NUWAX_PROVISION_OUTCOME_AUTH_FAILED;
            set_reason_locked("security_credentials_mismatch");
        } else if (event_id == PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS) {
            s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
            set_reason_locked("invalid_security_params");
        }
        portEXIT_CRITICAL(&s_state_lock);
    }
}

static esp_err_t device_info_handler(uint32_t session_id, const uint8_t *inbuf,
                                     ssize_t inlen, uint8_t **outbuf,
                                     ssize_t *outlen, void *priv_data) {
    (void)session_id;
    (void)priv_data;
    if (!inbuf || inlen <= 0 || !outbuf || !outlen) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *request = cJSON_ParseWithLength((const char *)inbuf, (size_t)inlen);
    if (!cJSON_IsObject(request)) {
        cJSON_Delete(request);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(request);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    bool json_ok = cJSON_AddStringToObject(root, "protocolVersion", "1.0") != NULL &&
                   cJSON_AddStringToObject(root, "serialNumber",
                                           nuwax_device_uid_string()) != NULL &&
                   cJSON_AddStringToObject(root, "productModel",
                                           "Nuwax AI Partner") != NULL &&
                   cJSON_AddStringToObject(root, "hardwareVersion",
                                           "CyberVoc V1.2") != NULL &&
                   cJSON_AddStringToObject(root, "firmwareVersion", "7.0.0") != NULL;
    cJSON *caps = cJSON_AddArrayToObject(root, "capabilities");
    cJSON *wifi_prov = cJSON_CreateString("wifi_prov");
    cJSON *wifi_scan = cJSON_CreateString("wifi_scan");
    cJSON *cloud_prov = cJSON_CreateString("cloud_prov");
    if (!json_ok || !caps || !wifi_prov || !wifi_scan || !cloud_prov) {
        cJSON_Delete(wifi_prov);
        cJSON_Delete(wifi_scan);
        cJSON_Delete(cloud_prov);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToArray(caps, wifi_prov)) {
        cJSON_Delete(wifi_prov);
        cJSON_Delete(wifi_scan);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToArray(caps, wifi_scan)) {
        cJSON_Delete(wifi_scan);
        cJSON_Delete(cloud_prov);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToArray(caps, cloud_prov)) {
        cJSON_Delete(cloud_prov);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    const provision_state_t snapshot = get_state_snapshot();
    json_ok = cJSON_AddStringToObject(root, "lastProvisioningOutcome",
                                      outcome_name(snapshot.outcome)) != NULL &&
              cJSON_AddStringToObject(root, "lastProvisioningReason",
                                      snapshot.reason[0] ? snapshot.reason : "-") != NULL &&
              cJSON_AddStringToObject(root, "cloudConfigState",
                                      nuwax_cloud_config_has_config()
                                          ? "active" : "missing") != NULL;
    if (json_ok) {
        if (snapshot.has_wifi_disconnect_reason) {
            json_ok = cJSON_AddNumberToObject(root, "wifiDisconnectReason",
                                              snapshot.wifi_disconnect_reason) != NULL &&
                      cJSON_AddStringToObject(
                          root, "wifiDisconnectReasonName",
                          raw_wifi_reason_name(snapshot.wifi_disconnect_reason)) != NULL;
        } else {
            json_ok = cJSON_AddNullToObject(root, "wifiDisconnectReason") != NULL &&
                      cJSON_AddNullToObject(root, "wifiDisconnectReasonName") != NULL;
        }
    }
    if (!json_ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    *outbuf = (uint8_t *)json;
    *outlen = (ssize_t)strlen(json);
    return ESP_OK;
}

static bool copy_json_string(cJSON *root, const char *name,
                             char *destination, size_t capacity) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
        return false;
    }
    const size_t length = strlen(item->valuestring);
    if (length >= capacity || memchr(item->valuestring, '\0', length) != NULL) {
        return false;
    }
    memcpy(destination, item->valuestring, length + 1);
    return true;
}

static esp_err_t vox_config_response(bool ok, const char *error,
                                     const nuwax_cloud_config_t *config,
                                     uint8_t **outbuf, ssize_t *outlen) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    bool json_ok = cJSON_AddBoolToObject(root, "ok", ok) != NULL;
    if (ok && config) {
        json_ok = json_ok &&
                  cJSON_AddStringToObject(root, "state", "active") != NULL &&
                  cJSON_AddStringToObject(root, "deviceId",
                                          config->device_id) != NULL;
    } else {
        json_ok = json_ok &&
                  cJSON_AddStringToObject(root, "error",
                                          error ? error : "VOX_CONFIG_INVALID") != NULL;
    }
    if (!json_ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    *outbuf = (uint8_t *)json;
    *outlen = (ssize_t)strlen(json);
    return ESP_OK;
}

static esp_err_t vox_config_process(const uint8_t *inbuf, ssize_t inlen,
                                    uint8_t **outbuf, ssize_t *outlen) {
    if (!inbuf || inlen <= 0 || !outbuf || !outlen) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *request = cJSON_ParseWithLength((const char *)inbuf, (size_t)inlen);
    if (!cJSON_IsObject(request)) {
        cJSON_Delete(request);
        return vox_config_response(false, "VOX_CONFIG_INVALID", NULL,
                                   outbuf, outlen);
    }

    nuwax_cloud_config_t config = {0};
    const bool parsed = copy_json_string(request, "deviceId", config.device_id,
                                         sizeof(config.device_id)) &&
                        copy_json_string(request, "gatewayUrl", config.cloud_url,
                                         sizeof(config.cloud_url)) &&
                        copy_json_string(request, "deviceSecret", config.device_secret,
                                         sizeof(config.device_secret));
    if (parsed) {
        config.version = 1;
    }
    cJSON_Delete(request);
    if (!parsed) {
        return vox_config_response(false, "VOX_CONFIG_INVALID", NULL,
                                   outbuf, outlen);
    }
    if (strcmp(config.device_id, nuwax_device_uid_string()) != 0) {
        return vox_config_response(false, "DEVICE_ID_MISMATCH", NULL,
                                   outbuf, outlen);
    }

    const esp_err_t err = nuwax_cloud_config_set(&config);
    if (err != ESP_OK) {
        const char *code = err == ESP_ERR_INVALID_ARG
                               ? "VOX_CONFIG_INVALID"
                               : "VOX_CONFIG_PERSIST_FAILED";
        ESP_LOGE(TAG, "vox-config rejected: %s", esp_err_to_name(err));
        return vox_config_response(false, code, NULL, outbuf, outlen);
    }
    return vox_config_response(true, NULL, &config, outbuf, outlen);
}

static esp_err_t ensure_vox_config_queue(void) {
    if (s_vox_config_jobs && s_vox_config_gate) {
        return ESP_OK;
    }
    if (!s_vox_config_jobs) {
        s_vox_config_jobs = xQueueCreateWithCaps(
            1, sizeof(vox_config_job_t *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_vox_config_jobs) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_vox_config_gate) {
        s_vox_config_gate = xSemaphoreCreateMutex();
    }
    if (!s_vox_config_gate) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "vox-config queue ready; NVS write runs on internal main task");
    return ESP_OK;
}

static void process_vox_config_job(TickType_t wait_ticks) {
    vox_config_job_t *job = NULL;
    if (!s_vox_config_jobs ||
        xQueueReceive(s_vox_config_jobs, &job, wait_ticks) != pdTRUE || !job) {
        return;
    }
    job->result = vox_config_process(job->input, job->input_len,
                                     &job->output, &job->output_len);
    xSemaphoreGive(job->done);
}

static void set_vox_config_accepting(bool accepting) {
    if (!s_vox_config_gate) {
        s_accept_vox_config_jobs = false;
        return;
    }
    if (xSemaphoreTake(s_vox_config_gate, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_accept_vox_config_jobs = accepting;
        xSemaphoreGive(s_vox_config_gate);
    }
}

static void close_and_drain_vox_config_jobs(void) {
    set_vox_config_accepting(false);
    while (s_vox_config_jobs && uxQueueMessagesWaiting(s_vox_config_jobs) > 0) {
        process_vox_config_job(0);
    }
}

static esp_err_t vox_config_handler(uint32_t session_id, const uint8_t *inbuf,
                                    ssize_t inlen, uint8_t **outbuf,
                                    ssize_t *outlen, void *priv_data) {
    (void)session_id;
    (void)priv_data;
    if (!inbuf || inlen <= 0 || !outbuf || !outlen || !s_vox_config_jobs ||
        !s_vox_config_gate) {
        return ESP_ERR_INVALID_STATE;
    }

    vox_config_job_t *job = heap_caps_calloc(
        1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    job->input = heap_caps_malloc((size_t)inlen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    job->done = xSemaphoreCreateBinary();
    if (!job->input || !job->done) {
        free(job->input);
        if (job->done) {
            vSemaphoreDelete(job->done);
        }
        free(job);
        return ESP_ERR_NO_MEM;
    }
    memcpy(job->input, inbuf, (size_t)inlen);
    job->input_len = inlen;

    if (xSemaphoreTake(s_vox_config_gate, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(job->input);
        vSemaphoreDelete(job->done);
        free(job);
        return ESP_ERR_TIMEOUT;
    }
    const bool accepting = s_accept_vox_config_jobs;
    const BaseType_t queued = accepting
                                  ? xQueueSend(s_vox_config_jobs, &job, 0)
                                  : pdFALSE;
    xSemaphoreGive(s_vox_config_gate);
    if (queued != pdTRUE) {
        free(job->input);
        vSemaphoreDelete(job->done);
        free(job);
        return accepting ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(job->done, portMAX_DELAY);
    const esp_err_t result = job->result;
    *outbuf = job->output;
    *outlen = job->output_len;
    free(job->input);
    vSemaphoreDelete(job->done);
    free(job);
    return result;
}

static esp_err_t prepare_security2(void) {
    const char *username = nuwax_device_uid_string();
#if CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE
    const char *pop = CONFIG_NUWAX_PROVISION_SEC2_DEV_POP;
    if (!username[0] || !pop[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sec2_salt) {
        free(s_sec2_salt);
        s_sec2_salt = NULL;
    }
    if (s_sec2_verifier) {
        free(s_sec2_verifier);
        s_sec2_verifier = NULL;
    }
    esp_err_t err = esp_srp_gen_salt_verifier(username, (int)strlen(username),
                                               pop, (int)strlen(pop),
                                               &s_sec2_salt, 16,
                                               &s_sec2_verifier,
                                               &s_sec2_verifier_len);
    if (err != ESP_OK) {
        return err;
    }
    s_sec2_salt_len = 16;
    s_sec2_params.salt = s_sec2_salt;
    s_sec2_params.salt_len = (uint16_t)s_sec2_salt_len;
    s_sec2_params.verifier = s_sec2_verifier;
    s_sec2_params.verifier_len = (uint16_t)s_sec2_verifier_len;
    ESP_RETURN_ON_ERROR(build_qr_payload(), TAG, "build provisioning QR failed");
    return ESP_OK;
#else
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("nuwax_factory", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    size_t salt_len = sizeof(s_factory_salt);
    size_t verifier_len = sizeof(s_factory_verifier);
    err = nvs_get_blob(nvs, "sec2_salt", s_factory_salt, &salt_len);
    if (err == ESP_OK) {
        err = nvs_get_blob(nvs, "sec2_verifier", s_factory_verifier,
                           &verifier_len);
    }
    nvs_close(nvs);
    if (err != ESP_OK || salt_len == 0 || verifier_len == 0) {
        ESP_LOGE(TAG, "factory Security 2 salt/verifier missing");
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    s_sec2_params.salt = s_factory_salt;
    s_sec2_params.salt_len = (uint16_t)salt_len;
    s_sec2_params.verifier = s_factory_verifier;
    s_sec2_params.verifier_len = (uint16_t)verifier_len;
    build_qr_payload();
    return ESP_OK;
#endif
}

static esp_err_t clear_reprovision_record(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, REPROVISION_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        memset(&s_reprovision_record, 0, sizeof(s_reprovision_record));
        s_force_reprovision = false;
    }
    return err;
}

static esp_err_t save_reprovision_record(void) {
    wifi_config_t wifi_config = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &wifi_config), TAG,
                        "read current Wi-Fi config for reversible reprovision failed");
    if (wifi_config.sta.ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    reprovision_record_t record = {
        .magic = REPROVISION_RECORD_MAGIC,
        .wifi_config = wifi_config,
    };
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open reprovision NVS failed");
    esp_err_t err = nvs_set_blob(nvs, REPROVISION_NVS_KEY, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_reprovision_record = record;
        s_force_reprovision = true;
    }
    return err;
}

static esp_err_t load_reprovision_record(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open reprovision NVS failed");
    reprovision_record_t record = {0};
    size_t length = sizeof(record);
    err = nvs_get_blob(nvs, REPROVISION_NVS_KEY, &record, &length);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK || length != sizeof(record) ||
        record.magic != REPROVISION_RECORD_MAGIC ||
        record.wifi_config.sta.ssid[0] == '\0') {
        ESP_LOGW(TAG, "discarding invalid reversible reprovision record");
        return clear_reprovision_record();
    }
    s_reprovision_record = record;
    s_force_reprovision = true;
    return ESP_OK;
}

esp_err_t nuwax_provision_main_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(nuwax_device_uid_init(), TAG, "uid init failed");
    ESP_RETURN_ON_ERROR(nuwax_cloud_config_init(), TAG, "cloud config init failed");
    uint8_t sta_mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, sta_mac), TAG,
                        "read STA MAC failed");
    snprintf(s_service_name, sizeof(s_service_name), "PROV_%02X%02X%02X",
             sta_mac[3], sta_mac[4], sta_mac[5]);
    build_qr_payload();

    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETWORK_PROV_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   provisioning_event_handler, NULL),
                        TAG, "register provisioning events failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   provisioning_event_handler, NULL),
                        TAG, "register security events failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   provisioning_event_handler, NULL),
                        TAG, "register IP event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT,
                                                   WIFI_EVENT_STA_DISCONNECTED,
                                                   provisioning_event_handler, NULL),
                        TAG, "register Wi-Fi disconnect event failed");

    const char *caps[] = {"wifi_prov", "wifi_scan", "cloud_prov"};
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE,
        .app_event_handler = {
            .event_cb = provisioning_manager_app_event_handler,
            .user_data = NULL,
        },
        .network_prov_wifi_conn_cfg = {.wifi_conn_attempts = 3},
    };
    ESP_RETURN_ON_ERROR(network_prov_mgr_init(config), TAG, "manager init failed");
    ESP_RETURN_ON_ERROR(network_prov_mgr_set_app_info("nuwax", "7.0.0",
                                                      caps, 3), TAG,
                        "set app info failed");

    static uint8_t service_uuid[16] = {
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    };
    ESP_RETURN_ON_ERROR(network_prov_scheme_ble_set_service_uuid(service_uuid),
                        TAG, "set service uuid failed");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_create(DEVICE_INFO_EP), TAG,
                        "create device-info endpoint failed");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_create(VOX_CONFIG_EP), TAG,
                        "create vox-config endpoint failed");

    bool provisioned = false;
    ESP_RETURN_ON_ERROR(network_prov_mgr_is_wifi_provisioned(&provisioned), TAG,
                        "read provisioned state failed");
    s_wifi_provisioned = provisioned;
    ESP_RETURN_ON_ERROR(load_reprovision_record(), TAG,
                        "load reversible reprovision request failed");
    if (s_wifi_provisioned && !s_force_reprovision &&
        !nuwax_cloud_config_has_config()) {
        ESP_LOGW(TAG, "Wi-Fi exists without a valid cloud config; opening reversible BLE recovery");
        ESP_RETURN_ON_ERROR(save_reprovision_record(), TAG,
                            "stage automatic cloud recovery failed");
    }
    s_initialized = true;
    return ESP_OK;
}

bool nuwax_provision_main_should_auto_enter(void) {
    return s_initialized && (!s_wifi_provisioned || s_force_reprovision);
}

esp_err_t nuwax_provision_main_start(const char *reason) {
    (void)reason;
    if (!s_initialized || s_started || (s_wifi_provisioned && !s_force_reprovision)) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(ensure_vox_config_queue(), TAG,
                        "create vox-config queue failed");
    ESP_RETURN_ON_ERROR(prepare_security2(), TAG, "prepare security2 failed");
    static uint8_t service_uuid[16] = {
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    };
    ESP_RETURN_ON_ERROR(network_prov_scheme_ble_set_service_uuid(service_uuid), TAG,
                        "set service uuid failed");
    reset_provision_state("starting");
    /* nuwax_provision_main_run() already keeps the status endpoint available
     * for 15 seconds after GOT_IP. The manager parameter is only the transport
     * cleanup delay after stop; keeping it at 30 seconds would unnecessarily
     * block normal UI/business startup after provisioning succeeds. */
    ESP_RETURN_ON_ERROR(network_prov_mgr_disable_auto_stop(1000), TAG,
                        "disable auto stop failed");
    ESP_RETURN_ON_ERROR(network_prov_mgr_start_provisioning(
                            NETWORK_PROV_SECURITY_2, &s_sec2_params,
                            s_service_name, NULL), TAG,
                        "start provisioning failed");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(DEVICE_INFO_EP,
                                                           device_info_handler,
                                                           NULL), TAG,
                        "register device-info endpoint failed");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(VOX_CONFIG_EP,
                                                           vox_config_handler,
                                                           NULL), TAG,
                        "register vox-config endpoint failed");
    set_vox_config_accepting(true);
    s_started = true;
    return ESP_OK;
}

nuwax_provision_outcome_t nuwax_provision_main_run(uint32_t timeout_ms) {
    if (!s_started) {
        return get_state_snapshot().outcome;
    }
    const int64_t window_deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (true) {
        process_vox_config_job(pdMS_TO_TICKS(100));
        provision_state_t snapshot = get_state_snapshot();
        if (snapshot.got_ip) {
            break;
        }
        const int64_t now = esp_timer_get_time();
        const bool attempt_active = snapshot.credentials_received_us != 0 &&
                                    snapshot.outcome == NUWAX_PROVISION_OUTCOME_NONE;
        const int64_t attempt_deadline = attempt_active
                                             ? snapshot.credentials_received_us +
                                                   CREDENTIAL_OBSERVATION_TIMEOUT_US
                                             : 0;
        const int64_t effective_deadline = attempt_deadline > window_deadline
                                               ? attempt_deadline
                                               : window_deadline;
        if (now >= effective_deadline) {
            break;
        }
        if (attempt_active && now >= attempt_deadline) {
            portENTER_CRITICAL(&s_state_lock);
            if (s_state.credentials_received_us == snapshot.credentials_received_us &&
                s_state.outcome == NUWAX_PROVISION_OUTCOME_NONE) {
                s_state.outcome = NUWAX_PROVISION_OUTCOME_TIMEOUT;
                set_reason_locked("status_timeout");
                s_state.credentials_received_us = 0;
            }
            portEXIT_CRITICAL(&s_state_lock);
            (void)network_prov_mgr_reset_wifi_sm_state_on_failure();
        }
    }
    provision_state_t final_state = get_state_snapshot();
    if (!final_state.got_ip && final_state.outcome == NUWAX_PROVISION_OUTCOME_NONE) {
        portENTER_CRITICAL(&s_state_lock);
        if (!s_state.got_ip && s_state.outcome == NUWAX_PROVISION_OUTCOME_NONE) {
            s_state.outcome = NUWAX_PROVISION_OUTCOME_TIMEOUT;
            set_reason_locked("status_timeout");
        }
        portEXIT_CRITICAL(&s_state_lock);
        final_state = get_state_snapshot();
    }
    if (final_state.got_ip) {
        /* Keep the official status endpoint reachable long enough for the app
         * to observe success. Custom endpoint jobs remain serviced during the
         * hold so a late idempotent request cannot strand the BLE host task. */
        const int64_t hold_deadline = esp_timer_get_time() +
            (int64_t)CONFIG_NUWAX_PROVISION_SUCCESS_BLE_HOLD_MS * 1000;
        while (esp_timer_get_time() < hold_deadline) {
            process_vox_config_job(pdMS_TO_TICKS(100));
        }
    }
    close_and_drain_vox_config_jobs();
    return get_state_snapshot().outcome;
}

esp_err_t nuwax_provision_main_stop(void) {
    if (!s_initialized) {
        return ESP_OK;
    }
    close_and_drain_vox_config_jobs();
    if (s_started) {
        network_prov_mgr_stop_provisioning();
        s_started = false;
    }
    esp_err_t err = network_prov_mgr_deinit();
    if (s_sec2_salt) {
        free(s_sec2_salt);
        s_sec2_salt = NULL;
    }
    if (s_sec2_verifier) {
        free(s_sec2_verifier);
        s_sec2_verifier = NULL;
    }
    s_initialized = false;
    return err;
}

esp_err_t nuwax_provision_main_reset_wifi_credentials(void) {
    ESP_LOGW(TAG, "local UI reprovision requested; preserving current Wi-Fi for rollback");
    if (s_force_reprovision) {
        return ESP_OK;
    }
    return save_reprovision_record();
}

bool nuwax_provision_main_is_reprovisioning(void) {
    return s_force_reprovision;
}

esp_err_t nuwax_provision_main_commit_wifi_credentials(void) {
    if (!s_force_reprovision) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "new Wi-Fi credentials reached GOT_IP; clearing rollback record");
    return clear_reprovision_record();
}

esp_err_t nuwax_provision_main_rollback_wifi_credentials(void) {
    if (!s_force_reprovision) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "BLE reprovision did not complete; restoring previous Wi-Fi credentials");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG,
                        "set Wi-Fi storage for rollback failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA,
                                           &s_reprovision_record.wifi_config), TAG,
                        "restore previous Wi-Fi config failed");
    esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK && connect_err != ESP_ERR_WIFI_CONN) {
        return connect_err;
    }
    return clear_reprovision_record();
}

const char *nuwax_provision_main_service_name(void) { return s_service_name; }
const char *nuwax_provision_main_qr_payload(void) { return s_qr_payload; }
nuwax_provision_outcome_t nuwax_provision_main_last_outcome(void) {
    return get_state_snapshot().outcome;
}
const char *nuwax_provision_main_last_reason(void) {
    static char reason[48];
    const provision_state_t snapshot = get_state_snapshot();
    strlcpy(reason, snapshot.reason, sizeof(reason));
    return reason;
}
