#include "test_controls.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wake_word_detector.h"
#include "wifi_saved_network_list_internal.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_FALLBACK_BIT BIT2
#define WIFI_SCAN_DONE_BIT BIT3
#define WIFI_RECONNECT_BIT BIT4
#define WIFI_STA_STOPPED_BIT BIT5
#define WIFI_STA_STARTED_BIT BIT6
#define MAX_WIFI_RETRY 8
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WIFI_ONLINE_SCAN_ACTIVE_MIN_MS 20U
#define WIFI_ONLINE_SCAN_ACTIVE_MAX_MS 80U
#define WIFI_ONLINE_SCAN_HOME_DWELL_MS 30U
#define WIFI_ONLINE_SCAN_TIMEOUT_MS 8000U
#define WIFI_OPERATION_LOCK_TIMEOUT_MS 35000U
#define WIFI_PROVISIONING_CLAIM_LOCK_TIMEOUT_MS 12000U
#define WIFI_PROVISIONING_RELEASE_LOCK_TIMEOUT_MS 2000U
#define WIFI_RECONNECT_LOCK_TIMEOUT_MS 250U
#define WIFI_RECONNECT_RETRY_MS 100U
#define WIFI_RECONNECT_BACKOFF_BASE_MS 100U
#define WIFI_RECONNECT_BACKOFF_MAX_MS 1600U
#define WIFI_RECONNECT_MAX_DRIVER_FAILURES 5U
#define WIFI_NVS_NAMESPACE "nuwax_wifi"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "password"
#define WIFI_NVS_KEY_SSID_B "ssid_b"
#define WIFI_NVS_KEY_PASSWORD_B "password_b"
#define WIFI_NVS_KEY_LAST_WORKING "last_working"  /* 0 = primary, 1 = backup */
#define WIFI_NVS_KEY_SAVED_COUNT "saved_count"
#define WIFI_NVS_KEY_SAVED_PREFIX "saved_"
#define WIFI_NVS_KEY_SAVED_SSID_SUFFIX "_ssid"
#define WIFI_NVS_KEY_SAVED_PASSWORD_SUFFIX "_pwd"
#define WIFI_NVS_FLASH_GUARD_TIMEOUT_MS 500U

static const char *TAG = "nuwax_wifi";
static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_wifi_operation_mutex;
static atomic_int s_wifi_retry_count = ATOMIC_VAR_INIT(0);
/* Only the synchronous operation owner may consume this one-shot refresh.
 * Event callbacks signal it; they never reconfigure the driver or touch NVS. */
static atomic_bool s_join_refresh_allowed;
static atomic_bool s_join_refresh_pending;
static atomic_uint s_join_failure_streak;
static bool s_wifi_initialized;
static atomic_bool s_wifi_connect_requested = ATOMIC_VAR_INIT(false);
static atomic_bool s_wifi_driver_connecting = ATOMIC_VAR_INIT(false);
static atomic_bool s_sta_restart_needs_start = ATOMIC_VAR_INIT(false);
static atomic_uint s_wifi_intent_revision = ATOMIC_VAR_INIT(0U);
static atomic_bool s_wifi_simulated_offline = ATOMIC_VAR_INIT(false);
static atomic_int s_active_ssid_index = ATOMIC_VAR_INIT(0);
static atomic_bool s_fallback_in_progress = ATOMIC_VAR_INIT(false);
static atomic_bool s_synchronous_connect_active = ATOMIC_VAR_INIT(false);
static atomic_bool s_wifi_scan_in_progress = ATOMIC_VAR_INIT(false);
static atomic_bool s_reconnect_after_scan = ATOMIC_VAR_INIT(false);
static bool s_watcher_started;
static char s_current_ssid[33];
static char s_current_ip[16];
static wifi_ap_record_t s_scan_records[24];
static char s_saved_network_ssids[NUWAX_WIFI_MAX_SAVED_NETWORKS][NUWAX_WIFI_SAVED_SSID_BYTES];
static char s_saved_network_passwords[NUWAX_WIFI_MAX_SAVED_NETWORKS][NUWAX_WIFI_SAVED_PASSWORD_BYTES];
static int s_saved_network_count;
static atomic_bool s_wifi_scan_succeeded = ATOMIC_VAR_INIT(false);
typedef enum {
    WIFI_PROVISIONING_NONE = 0,
    WIFI_PROVISIONING_HANDOFF,
    WIFI_PROVISIONING_OWNER,
} wifi_provisioning_state_t;
static atomic_int s_provisioning_state =
    ATOMIC_VAR_INIT(WIFI_PROVISIONING_NONE);
static atomic_bool s_provisioning_saved_connect_requested =
    ATOMIC_VAR_INIT(false);
static atomic_uint s_deferred_reconnect_driver_failures = ATOMIC_VAR_INIT(0U);

static void wifi_fallback_watcher_task(void *arg);
static esp_err_t nuwax_wifi_run_fallback(void);

/* Driver attempts are shared by event retries, the watcher and recovery.
 * Publish before esp_wifi_connect: callbacks may arrive before it returns. */
static esp_err_t wifi_driver_connect_once(void) {
    if (nuwax_wifi_provisioning_active() ||
        atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (nuwax_wifi_is_connected()) return ESP_OK;
    if (atomic_exchange_explicit(&s_wifi_driver_connecting, true, memory_order_acq_rel)) return ESP_OK;
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
        atomic_store_explicit(&s_wifi_driver_connecting, false, memory_order_release);
    return err == ESP_ERR_WIFI_CONN ? ESP_OK : err;
}

static esp_err_t wifi_driver_disconnect(void) {
    atomic_store_explicit(&s_wifi_driver_connecting, false, memory_order_release);
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    memset(s_current_ip, 0, sizeof(s_current_ip));
    return esp_wifi_disconnect();
}

/* Exclude missing AP and explicit authentication rejection: resetting those
 * cannot repair credentials or radio coverage. Eligible transient join errors
 * are accumulated within one bounded join. Missing-AP scans between two
 * authentication timeouts do not erase evidence that this AP was heard. */
static bool wifi_join_refresh_needed(int reason) {
    if (reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_STA_LEAVING) {
        return false;
    }
    bool transient = reason == WIFI_REASON_AUTH_EXPIRE ||
                     reason == WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY ||
                     reason == WIFI_REASON_ASSOC_FAIL ||
                     reason == WIFI_REASON_CONNECTION_FAIL;
    if (!transient) {
        atomic_store_explicit(&s_join_failure_streak, 0U, memory_order_release);
        return false;
    }
    unsigned count = atomic_fetch_add_explicit(&s_join_failure_streak, 1U, memory_order_acq_rel) + 1U;
    return count >= 2U && atomic_exchange_explicit(&s_join_refresh_allowed, false, memory_order_acq_rel);
}

static uint32_t wifi_reconnect_backoff_ms(uint32_t failure_count) {
    uint32_t delay_ms = WIFI_RECONNECT_BACKOFF_BASE_MS;
    for (uint32_t i = 1U;
         i < failure_count && delay_ms < WIFI_RECONNECT_BACKOFF_MAX_MS;
         ++i) {
        delay_ms *= 2U;
        if (delay_ms > WIFI_RECONNECT_BACKOFF_MAX_MS) {
            delay_ms = WIFI_RECONNECT_BACKOFF_MAX_MS;
        }
    }
    return delay_ms;
}

static esp_err_t wifi_operation_lock_for(uint32_t timeout_ms) {
    if (!s_wifi_operation_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTakeRecursive(
               s_wifi_operation_mutex,
               pdMS_TO_TICKS(timeout_ms)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static esp_err_t wifi_operation_lock(void) {
    return wifi_operation_lock_for(WIFI_OPERATION_LOCK_TIMEOUT_MS);
}

static void wifi_operation_unlock(void) {
    if (s_wifi_operation_mutex) {
        xSemaphoreGiveRecursive(s_wifi_operation_mutex);
    }
}

/* NVS is backed by the same flash cache used by the wake model.  Every
 * runtime NVS handle in this module therefore owns one detector guard level;
 * the guard is re-entrant for helpers such as history-save -> history-load.
 */
static esp_err_t wifi_nvs_open(nvs_open_mode_t mode, nvs_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t guard_err =
        nuwax_wake_word_detector_begin_flash_operation(
            WIFI_NVS_FLASH_GUARD_TIMEOUT_MS);
    if (guard_err != ESP_OK) {
        return guard_err;
    }
    const esp_err_t err =
        nvs_open(WIFI_NVS_NAMESPACE, mode, out_handle);
    if (err != ESP_OK) {
        nuwax_wake_word_detector_end_flash_operation();
    }
    return err;
}

static void wifi_nvs_close(nvs_handle_t handle)
{
    nvs_close(handle);
    nuwax_wake_word_detector_end_flash_operation();
}

static esp_err_t wifi_load_saved_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size) {
    if (!ssid || ssid_size == 0 || !password || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    password[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = wifi_nvs_open(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required = ssid_size;
    err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid, &required);
    if (err != ESP_OK || ssid[0] == '\0') {
        wifi_nvs_close(handle);
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    required = password_size;
    err = nvs_get_str(handle, WIFI_NVS_KEY_PASSWORD, password, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        password[0] = '\0';
        err = ESP_OK;
    }
    wifi_nvs_close(handle);
    return err;
}

static esp_err_t wifi_save_credentials(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = wifi_nvs_open(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Wi-Fi credential persistence deferred/open failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_NVS_KEY_PASSWORD, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    wifi_nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved Wi-Fi credentials SSID: %s", ssid);
    } else {
        ESP_LOGW(TAG, "save Wi-Fi credentials failed: %s", esp_err_to_name(err));
    }
    return err;
}

static int wifi_find_saved_network_index(const char *ssid) {
    if (!ssid || ssid[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < s_saved_network_count; i++) {
        if (strncmp(s_saved_network_ssids[i], ssid, sizeof(s_saved_network_ssids[i])) == 0) {
            return i;
        }
    }
    return -1;
}

static esp_err_t wifi_load_saved_network_list(void) {
    if (s_saved_network_count > 0) {
        return ESP_OK;
    }
    nvs_handle_t handle;
    esp_err_t err = wifi_nvs_open(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t saved_count = 0;
    err = nvs_get_u8(handle, WIFI_NVS_KEY_SAVED_COUNT, &saved_count);
    if (err != ESP_OK) {
        wifi_nvs_close(handle);
        return err;
    }
    if (saved_count > NUWAX_WIFI_MAX_SAVED_NETWORKS) {
        saved_count = NUWAX_WIFI_MAX_SAVED_NETWORKS;
    }

    memset(s_saved_network_ssids, 0, sizeof(s_saved_network_ssids));
    memset(s_saved_network_passwords, 0, sizeof(s_saved_network_passwords));
    s_saved_network_count = 0;
    for (uint8_t i = 0; i < saved_count; i++) {
        char key[32];
        char ssid[NUWAX_WIFI_SAVED_SSID_BYTES] = {0};
        char password[NUWAX_WIFI_SAVED_PASSWORD_BYTES] = {0};
        size_t required = sizeof(ssid);
        snprintf(key, sizeof(key), "%s%u%s", WIFI_NVS_KEY_SAVED_PREFIX, (unsigned)i, WIFI_NVS_KEY_SAVED_SSID_SUFFIX);
        esp_err_t item_err = nvs_get_str(handle, key, ssid, &required);
        if (item_err != ESP_OK || ssid[0] == '\0') {
            continue;
        }
        required = sizeof(password);
        snprintf(key, sizeof(key), "%s%u%s", WIFI_NVS_KEY_SAVED_PREFIX, (unsigned)i, WIFI_NVS_KEY_SAVED_PASSWORD_SUFFIX);
        item_err = nvs_get_str(handle, key, password, &required);
        if (item_err == ESP_ERR_NVS_NOT_FOUND) {
            password[0] = '\0';
            item_err = ESP_OK;
        }
        if (item_err == ESP_OK) {
            (void)nuwax_wifi_saved_network_append(
                s_saved_network_ssids, s_saved_network_passwords,
                NUWAX_WIFI_MAX_SAVED_NETWORKS, &s_saved_network_count,
                ssid, password);
        }
    }
    wifi_nvs_close(handle);
    return ESP_OK;
}

static esp_err_t wifi_save_network_name(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = wifi_nvs_open(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "save Wi-Fi history deferred/open failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = wifi_load_saved_network_list();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "load saved network list failed: %s", esp_err_to_name(err));
        memset(s_saved_network_ssids, 0, sizeof(s_saved_network_ssids));
        memset(s_saved_network_passwords, 0, sizeof(s_saved_network_passwords));
        s_saved_network_count = 0;
    }
    err = ESP_OK;

    int existing = wifi_find_saved_network_index(ssid);
    if (existing < 0 && s_saved_network_count < NUWAX_WIFI_MAX_SAVED_NETWORKS) {
        existing = s_saved_network_count++;
    }
    if (existing < 0) {
        existing = NUWAX_WIFI_MAX_SAVED_NETWORKS - 1;
    }

    if (existing >= 0 && existing < NUWAX_WIFI_MAX_SAVED_NETWORKS) {
        strlcpy(s_saved_network_ssids[existing], ssid, sizeof(s_saved_network_ssids[existing]));
        strlcpy(s_saved_network_passwords[existing], password ? password : "", sizeof(s_saved_network_passwords[existing]));
        for (int i = 0; i < s_saved_network_count && err == ESP_OK; i++) {
            char key[32];
            snprintf(key, sizeof(key), "%s%d%s", WIFI_NVS_KEY_SAVED_PREFIX,
                     i, WIFI_NVS_KEY_SAVED_SSID_SUFFIX);
            err = nvs_set_str(handle, key, s_saved_network_ssids[i]);
            if (err == ESP_OK) {
                snprintf(key, sizeof(key), "%s%d%s",
                         WIFI_NVS_KEY_SAVED_PREFIX, i,
                         WIFI_NVS_KEY_SAVED_PASSWORD_SUFFIX);
                err = nvs_set_str(handle, key,
                                  s_saved_network_passwords[i]);
            }
        }
        if (err == ESP_OK) {
            uint8_t count = (uint8_t)s_saved_network_count;
            if (count < 1) count = 1;
            err = nvs_set_u8(handle, WIFI_NVS_KEY_SAVED_COUNT, count);
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
    }

    wifi_nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved Wi-Fi history SSID: %s slot=%d", ssid, existing);
    } else {
        ESP_LOGW(TAG, "save Wi-Fi history failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void wifi_persist_last_working_index(uint8_t index) {
    nvs_handle_t handle = 0;
    esp_err_t err = wifi_nvs_open(NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, WIFI_NVS_KEY_LAST_WORKING, index);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        wifi_nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "last-working Wi-Fi index persistence failed: %s",
                 esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        const wifi_event_sta_scan_done_t *event =
            (const wifi_event_sta_scan_done_t *)event_data;
        atomic_store_explicit(&s_wifi_scan_succeeded,
                              event && event->status == 0,
                              memory_order_release);
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        atomic_store_explicit(&s_wifi_driver_connecting, false, memory_order_release);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_STA_STARTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_STA_STOPPED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_STA_STOPPED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_STA_STARTED_BIT);
        if (!atomic_load_explicit(&s_wifi_simulated_offline,
                                  memory_order_acquire) &&
            atomic_load_explicit(&s_wifi_connect_requested,
                                 memory_order_acquire)) {
            wifi_driver_connect_once();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        /* A queued local disconnect can arrive after a new connect is armed.
         * Its owner already cleared link state; never consume a new attempt
         * or clear its driver latch from that obsolete notification. */
        if (event && event->reason == WIFI_REASON_STA_LEAVING &&
            !nuwax_wifi_provisioning_active()) {
            ESP_LOGI(TAG, "Wi-Fi local disconnect completed; retry budget unchanged");
            return;
        }
        atomic_store_explicit(&s_wifi_driver_connecting, false, memory_order_release);
        const bool connect_requested = atomic_load_explicit(
            &s_wifi_connect_requested, memory_order_acquire);
        if (event) {
            ESP_LOGW(TAG, "Wi-Fi link failure reason=%u rssi=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x driver_pending=%d",
                     event->reason, event->rssi, event->bssid[0], event->bssid[1],
                     event->bssid[2], event->bssid[3], event->bssid[4], event->bssid[5],
                     connect_requested);
        }
        memset(s_current_ip, 0, sizeof(s_current_ip));
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        if (!atomic_load_explicit(&s_wifi_simulated_offline,
                                  memory_order_acquire) &&
            !nuwax_wifi_provisioning_active() &&
            connect_requested &&
            atomic_load_explicit(&s_wifi_retry_count,
                                 memory_order_acquire) < MAX_WIFI_RETRY) {
            const int retry_count = atomic_fetch_add_explicit(
                                        &s_wifi_retry_count,
                                        1,
                                        memory_order_acq_rel) +
                                    1;
            ESP_LOGW(TAG,
                     "Wi-Fi disconnected reason=%d, retrying (%d/%d)",
                     event ? event->reason : -1,
                     retry_count,
                     MAX_WIFI_RETRY);
            if (atomic_load_explicit(&s_synchronous_connect_active, memory_order_acquire) &&
                !atomic_load_explicit(&s_wifi_scan_in_progress, memory_order_acquire) &&
                wifi_join_refresh_needed(event ? event->reason : -1)) {
                atomic_store_explicit(&s_wifi_connect_requested, false, memory_order_release);
                atomic_store_explicit(&s_join_refresh_pending, true, memory_order_release);
                ESP_LOGW(TAG, "Wi-Fi transient join stalled; requesting one STA restart reason=%d", event ? event->reason : -1);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            } else if (atomic_load_explicit(&s_wifi_scan_in_progress,
                                     memory_order_acquire)) {
                atomic_store_explicit(&s_reconnect_after_scan,
                                      true,
                                      memory_order_release);
                ESP_LOGI(TAG,
                         "Wi-Fi reconnect deferred until active scan completes");
            } else {
                wifi_driver_connect_once();
            }
        } else if (!atomic_load_explicit(&s_wifi_simulated_offline,
                                         memory_order_acquire) &&
                   !nuwax_wifi_provisioning_active() &&
                   connect_requested &&
                   s_wifi_event_group) {
            ESP_LOGE(TAG,
                     "Wi-Fi disconnected reason=%d, retries exhausted",
                     event ? event->reason : -1);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            if (strlen(CONFIG_NUWAX_WIFI_SSID_B) > 0 &&
                !atomic_load_explicit(&s_synchronous_connect_active,
                                      memory_order_acquire) &&
                !atomic_exchange_explicit(&s_fallback_in_progress,
                                          true,
                                          memory_order_acq_rel)) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FALLBACK_BIT);
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        atomic_store_explicit(&s_join_failure_streak, 0U, memory_order_release);
        const wifi_event_sta_connected_t *event = event_data;
        if (event) {
            ESP_LOGI(TAG, "Wi-Fi associated channel=%u authmode=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x; waiting for DHCP",
                     event->channel, (unsigned)event->authmode,
                     event->bssid[0], event->bssid[1], event->bssid[2],
                     event->bssid[3], event->bssid[4], event->bssid[5]);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        atomic_store_explicit(&s_join_refresh_allowed, false, memory_order_release);
        atomic_store_explicit(&s_wifi_driver_connecting, false, memory_order_release);
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        atomic_store_explicit(&s_wifi_retry_count, 0, memory_order_release);
        atomic_store_explicit(&s_fallback_in_progress,
                              false,
                              memory_order_release);
        wifi_config_t active = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &active) == ESP_OK) {
            strlcpy(s_current_ssid, (const char *)active.sta.ssid,
                    sizeof(s_current_ssid));
        }
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t wifi_init_once(void) {
    if (s_wifi_initialized) {
        if (atomic_load_explicit(&s_sta_restart_needs_start, memory_order_acquire)) {
            ESP_RETURN_ON_ERROR(wifi_operation_lock(), TAG, "STA restart recovery owner busy");
            esp_err_t err = ESP_OK;
            if (atomic_load_explicit(&s_sta_restart_needs_start, memory_order_acquire)) {
                if (nuwax_wifi_provisioning_active() ||
                    atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire)) {
                    err = ESP_ERR_INVALID_STATE;
                } else {
                    err = esp_wifi_start();
                    if (err == ESP_OK) {
                        atomic_store_explicit(&s_sta_restart_needs_start, false, memory_order_release);
                    }
                }
            }
            wifi_operation_unlock();
            return err;
        }
        return ESP_OK;
    }

    bool mutex_created = false;
    bool event_group_created = false;
    bool event_loop_created_here = false;
    bool wifi_driver_initialized = false;
    bool wifi_started = false;
    bool wifi_handler_registered = false;
    bool ip_handler_registered = false;
    esp_netif_t *sta_netif = NULL;
    esp_event_handler_instance_t wifi_handler = NULL;
    esp_event_handler_instance_t ip_handler = NULL;
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "Wi-Fi init begin");
    if (!s_wifi_operation_mutex) {
        s_wifi_operation_mutex = xSemaphoreCreateRecursiveMutex();
        if (!s_wifi_operation_mutex) {
            ESP_LOGE(TAG, "Wi-Fi operation mutex create failed");
            return ESP_ERR_NO_MEM;
        }
        mutex_created = true;
    }
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            ESP_LOGE(TAG, "Wi-Fi event group create failed");
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        event_group_created = true;
    }

    ESP_LOGI(TAG, "Wi-Fi init step: esp_netif_init");
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        goto fail;
    }

    ESP_LOGI(TAG, "Wi-Fi init step: event loop");
    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        event_loop_created_here = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        goto fail;
    }

    ESP_LOGI(TAG, "Wi-Fi init step: create default STA netif");
    sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        err = ESP_FAIL;
        goto fail;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "Wi-Fi init step: esp_wifi_init");
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        goto fail;
    }
    wifi_driver_initialized = true;

    ESP_LOGI(TAG, "Wi-Fi init step: register WIFI_EVENT handler");
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
                                              &wifi_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT handler failed: %s", esp_err_to_name(err));
        goto fail;
    }
    wifi_handler_registered = true;

    ESP_LOGI(TAG, "Wi-Fi init step: register IP_EVENT handler");
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
                                              &ip_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT handler failed: %s", esp_err_to_name(err));
        goto fail;
    }
    ip_handler_registered = true;

    ESP_LOGI(TAG, "Wi-Fi init step: set STA mode");
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        goto fail;
    }

    ESP_LOGI(TAG, "Wi-Fi init step: esp_wifi_start");
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        goto fail;
    }
    wifi_started = true;

#if CONFIG_NUWAX_WIFI_REALTIME_DISABLE_POWER_SAVE
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "failed to disable Wi-Fi power save for realtime audio: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi power save disabled for realtime audio");
    }
#endif

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi STA initialized");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Wi-Fi init rollback after failure: %s", esp_err_to_name(err));
    if (wifi_started) {
        (void)esp_wifi_stop();
    }
    if (ip_handler_registered) {
        (void)esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
    }
    if (wifi_handler_registered) {
        (void)esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    }
    if (sta_netif) {
        esp_netif_destroy_default_wifi(sta_netif);
    }
    if (wifi_driver_initialized) {
        (void)esp_wifi_deinit();
    }
    if (event_loop_created_here) {
        (void)esp_event_loop_delete_default();
    }
    if (event_group_created && s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    if (mutex_created && s_wifi_operation_mutex) {
        vSemaphoreDelete(s_wifi_operation_mutex);
        s_wifi_operation_mutex = NULL;
    }
    return err;
}

static void wifi_fallback_watcher_task(void *arg) {
    (void)arg;
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_FALLBACK_BIT | WIFI_RECONNECT_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(1000));
        if (bits & WIFI_RECONNECT_BIT) {
            if (atomic_load_explicit(&s_wifi_simulated_offline,
                                     memory_order_acquire) ||
                nuwax_wifi_provisioning_active() ||
                !atomic_load_explicit(&s_wifi_connect_requested,
                                      memory_order_acquire) ||
                nuwax_wifi_is_connected()) {
                atomic_store_explicit(&s_deferred_reconnect_driver_failures,
                                      0U, memory_order_release);
                continue;
            }
            const esp_err_t owner_err =
                wifi_operation_lock_for(WIFI_RECONNECT_LOCK_TIMEOUT_MS);
            if (owner_err != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_RETRY_MS));
                if (!atomic_load_explicit(&s_wifi_simulated_offline,
                                          memory_order_acquire) &&
                    !nuwax_wifi_provisioning_active() &&
                    atomic_load_explicit(&s_wifi_connect_requested,
                                         memory_order_acquire) &&
                    !nuwax_wifi_is_connected() &&
                    s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
                }
                continue;
            }
            bool retry_driver_connect = false;
            uint32_t retry_delay_ms = 0U;
            if (!atomic_load_explicit(&s_wifi_simulated_offline,
                                      memory_order_acquire) &&
                !nuwax_wifi_provisioning_active() &&
                atomic_load_explicit(&s_wifi_connect_requested,
                                     memory_order_acquire) &&
                !nuwax_wifi_is_connected()) {
                const esp_err_t reconnect_err = wifi_driver_connect_once();
                if (reconnect_err != ESP_OK &&
                    reconnect_err != ESP_ERR_WIFI_CONN) {
                    const uint32_t failures = atomic_fetch_add_explicit(
                                                  &s_deferred_reconnect_driver_failures,
                                                  1U, memory_order_acq_rel) +
                                              1U;
                    if (failures < WIFI_RECONNECT_MAX_DRIVER_FAILURES) {
                        retry_driver_connect = true;
                        retry_delay_ms = wifi_reconnect_backoff_ms(failures);
                        ESP_LOGW(TAG,
                                 "deferred reconnect after BLE provisioning failed: %s failure=%lu/%u retry_ms=%lu",
                                 esp_err_to_name(reconnect_err),
                                 (unsigned long)failures,
                                 (unsigned)WIFI_RECONNECT_MAX_DRIVER_FAILURES,
                                 (unsigned long)retry_delay_ms);
                    } else {
                        ESP_LOGE(TAG,
                                 "deferred reconnect after BLE provisioning exhausted driver retry budget: %s failures=%lu; outer Wi-Fi recovery remains responsible",
                                 esp_err_to_name(reconnect_err),
                                 (unsigned long)failures);
                    }
                } else {
                    atomic_store_explicit(
                        &s_deferred_reconnect_driver_failures, 0U,
                        memory_order_release);
                    ESP_LOGI(TAG,
                             "deferred STA reconnect started or joined");
                }
            }
            wifi_operation_unlock();
            if (retry_driver_connect) {
                vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
                if (!atomic_load_explicit(&s_wifi_simulated_offline,
                                          memory_order_acquire) &&
                    !nuwax_wifi_provisioning_active() &&
                    atomic_load_explicit(&s_wifi_connect_requested,
                                         memory_order_acquire) &&
                    !nuwax_wifi_is_connected() &&
                    s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
                } else {
                    atomic_store_explicit(
                        &s_deferred_reconnect_driver_failures, 0U,
                        memory_order_release);
                }
            }
        } else if (bits & WIFI_FALLBACK_BIT) {
            if (atomic_load_explicit(&s_wifi_simulated_offline,
                                     memory_order_acquire) ||
                nuwax_wifi_provisioning_active()) {
                ESP_LOGI(TAG,
                         "Wi-Fi fallback suppressed while BLE provisioning owns STA");
                atomic_store_explicit(&s_fallback_in_progress,
                                      false,
                                      memory_order_release);
                continue;
            }
            if (CONFIG_NUWAX_WIFI_SSID_B[0] == '\0') {
                ESP_LOGW(TAG, "Fallback bit set but no backup SSID configured");
                atomic_store_explicit(&s_fallback_in_progress,
                                      false,
                                      memory_order_release);
                continue;
            }
            ESP_LOGW(TAG, "Wi-Fi fallback watcher: starting fallback connect");
            const esp_err_t fallback_err = nuwax_wifi_run_fallback();
            if (fallback_err != ESP_OK) {
                atomic_store_explicit(&s_fallback_in_progress,
                                      false,
                                      memory_order_release);
                ESP_LOGW(TAG,
                         "Wi-Fi fallback attempt failed: %s",
                         esp_err_to_name(fallback_err));
            }
        }
    }
}

esp_err_t nuwax_wifi_start_fallback_watcher(void) {
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init failed");
    if (s_watcher_started) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(wifi_fallback_watcher_task, "wifi_fb_watch",
                                2048, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }
    s_watcher_started = true;
    return ESP_OK;
}

void nuwax_wifi_request_provisioning_handoff(void) {
    atomic_fetch_add_explicit(&s_wifi_intent_revision, 1U, memory_order_acq_rel);
    atomic_store_explicit(&s_deferred_reconnect_driver_failures, 0U,
                          memory_order_release);
    int expected = WIFI_PROVISIONING_NONE;
    const bool handoff_started = atomic_compare_exchange_strong_explicit(
        &s_provisioning_state, &expected, WIFI_PROVISIONING_HANDOFF,
        memory_order_acq_rel, memory_order_acquire);
    const bool connect_requested = atomic_exchange_explicit(
        &s_wifi_connect_requested, false, memory_order_acq_rel) ||
        atomic_load_explicit(&s_synchronous_connect_active,
                             memory_order_acquire);
    if (handoff_started) {
        atomic_store_explicit(&s_provisioning_saved_connect_requested,
                              connect_requested,
                              memory_order_release);
    }
    atomic_store_explicit(&s_fallback_in_progress,
                          false,
                          memory_order_release);
    if (s_wifi_event_group) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupClearBits(s_wifi_event_group, WIFI_FALLBACK_BIT);
    }
}

bool nuwax_wifi_cancel_provisioning_handoff(void) {
    /* This is deliberately an atomic-only operation. The caller uses it only
     * before the serialized Wi-Fi owner has been claimed, so waiting for the
     * manager mutex here would make a UI/control callback block behind an
     * unrelated STA operation. */
    int expected = WIFI_PROVISIONING_HANDOFF;
    if (atomic_compare_exchange_strong_explicit(
        &s_provisioning_state, &expected, WIFI_PROVISIONING_NONE,
        memory_order_acq_rel, memory_order_acquire)) {
        atomic_fetch_add_explicit(&s_wifi_intent_revision, 1U, memory_order_acq_rel);
        const bool reconnect_armed = atomic_exchange_explicit(
            &s_provisioning_saved_connect_requested, false,
            memory_order_acq_rel);
        atomic_store_explicit(&s_wifi_retry_count, 0, memory_order_release);
        atomic_store_explicit(&s_fallback_in_progress, false,
                              memory_order_release);
        atomic_store_explicit(&s_wifi_connect_requested, reconnect_armed,
                              memory_order_release);
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group,
                                 WIFI_FAIL_BIT | WIFI_FALLBACK_BIT);
            if (reconnect_armed) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
            }
        }
        return true;
    }
    return expected == WIFI_PROVISIONING_NONE;
}

esp_err_t nuwax_wifi_set_provisioning_active(bool active) {
    if (active) {
        nuwax_wifi_request_provisioning_handoff();
    }
    const esp_err_t owner_err = wifi_operation_lock_for(
        active ? WIFI_PROVISIONING_CLAIM_LOCK_TIMEOUT_MS
               : WIFI_PROVISIONING_RELEASE_LOCK_TIMEOUT_MS);
    if (owner_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "BLE provisioning could not claim Wi-Fi manager owner: %s",
                 esp_err_to_name(owner_err));
        return owner_err;
    }
    if (active) {
        int expected = WIFI_PROVISIONING_HANDOFF;
        if (!atomic_compare_exchange_strong_explicit(
                &s_provisioning_state, &expected, WIFI_PROVISIONING_OWNER,
                memory_order_acq_rel, memory_order_acquire) &&
            expected != WIFI_PROVISIONING_OWNER) {
            wifi_operation_unlock();
            ESP_LOGW(TAG,
                     "BLE provisioning Wi-Fi handoff was cancelled before owner claim");
            return ESP_ERR_INVALID_STATE;
        }
        atomic_store_explicit(&s_provisioning_saved_connect_requested, false,
                              memory_order_release);
    } else {
        atomic_store_explicit(&s_provisioning_state, WIFI_PROVISIONING_NONE,
                              memory_order_release);
        atomic_fetch_add_explicit(&s_wifi_intent_revision, 1U, memory_order_acq_rel);
        atomic_store_explicit(&s_provisioning_saved_connect_requested, false,
                              memory_order_release);
    }
    if (active) {
        atomic_store_explicit(&s_wifi_connect_requested, false,
                              memory_order_release);
        atomic_store_explicit(&s_wifi_retry_count, 0, memory_order_release);
        atomic_store_explicit(&s_fallback_in_progress,
                              false,
                              memory_order_release);
        atomic_store_explicit(&s_deferred_reconnect_driver_failures, 0U,
                              memory_order_release);
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group,
                                 WIFI_FAIL_BIT | WIFI_FALLBACK_BIT);
        }
    } else {
        atomic_store_explicit(&s_wifi_retry_count, 0, memory_order_release);
        atomic_store_explicit(&s_fallback_in_progress,
                              false,
                              memory_order_release);
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group,
                                 WIFI_FAIL_BIT | WIFI_FALLBACK_BIT);
        }
        wifi_config_t current = {0};
        const bool reconnect_armed = s_wifi_initialized &&
            esp_wifi_get_config(WIFI_IF_STA, &current) == ESP_OK &&
            current.sta.ssid[0] != '\0';
        atomic_store_explicit(&s_wifi_connect_requested, reconnect_armed,
                              memory_order_release);
        if (reconnect_armed && !nuwax_wifi_is_connected()) {
            const esp_err_t reconnect_err = wifi_driver_connect_once();
            if (reconnect_err != ESP_OK &&
                reconnect_err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG,
                         "Wi-Fi reconnect rearm after BLE provisioning failed: %s",
                         esp_err_to_name(reconnect_err));
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
                }
            } else {
                atomic_store_explicit(
                    &s_deferred_reconnect_driver_failures, 0U,
                    memory_order_release);
            }
        }
        ESP_LOGI(TAG,
                 "BLE provisioning released STA ownership reconnect_armed=%d connected=%d",
                 reconnect_armed ? 1 : 0,
                 nuwax_wifi_is_connected() ? 1 : 0);
    }
    ESP_LOGI(TAG, "BLE provisioning STA ownership active=%d", active ? 1 : 0);
    wifi_operation_unlock();
    return ESP_OK;
}

bool nuwax_wifi_provisioning_active(void) {
    return atomic_load_explicit(&s_provisioning_state, memory_order_acquire) !=
           WIFI_PROVISIONING_NONE;
}

bool nuwax_wifi_provisioning_owner_active(void) {
    return atomic_load_explicit(&s_provisioning_state, memory_order_acquire) ==
           WIFI_PROVISIONING_OWNER;
}

/* Protected by s_wifi_operation_mutex. Remember driver normalization after a
 * write, but verify current driver state before reuse (BLE can replace it). */
static wifi_config_t s_last_requested_wifi_config;
static wifi_config_t s_last_applied_wifi_config;
static bool s_last_wifi_config_valid;

static esp_err_t wifi_set_config_guarded(wifi_config_t *config) {
    /* Reconnect is not a credential update. Compare the complete driver config
     * after accounting for the SDK's documented automatic auth threshold.
     * A conservative mismatch still takes the existing Flash safety fence. */
    wifi_config_t current = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &current) == ESP_OK) {
        if (s_last_wifi_config_valid &&
            memcmp(config, &s_last_requested_wifi_config, sizeof(*config)) == 0 &&
            memcmp(&current, &s_last_applied_wifi_config, sizeof(current)) == 0) {
            ESP_LOGI(TAG, "Wi-Fi STA configuration unchanged; reuse without Flash fence");
            return ESP_OK;
        }
        wifi_config_t expected = *config;
        if (expected.sta.threshold.authmode == WIFI_AUTH_OPEN &&
            strnlen((const char *)expected.sta.password,
                    sizeof(expected.sta.password)) >= 8U) {
            expected.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        }
        if (memcmp(&current, config, sizeof(current)) == 0 ||
            memcmp(&current, &expected, sizeof(current)) == 0) {
            ESP_LOGI(TAG, "Wi-Fi STA configuration unchanged; reuse without Flash fence");
            return ESP_OK;
        }
    }
    /* Driver storage may still be WIFI_STORAGE_FLASH after provisioning.
     * esp_wifi_set_config() can therefore write NVS without wifi_nvs_open().
     * Use the same stop fence as explicit NVS writes, including busy retries. */
    const esp_err_t guard_err = nuwax_wake_word_detector_begin_flash_operation(
        WIFI_NVS_FLASH_GUARD_TIMEOUT_MS);
    if (guard_err != ESP_OK) return guard_err;
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, config);
    nuwax_wake_word_detector_end_flash_operation();
    s_last_wifi_config_valid = false;
    if (err == ESP_OK) {
        memset(&s_last_applied_wifi_config, 0, sizeof(s_last_applied_wifi_config));
        if (esp_wifi_get_config(WIFI_IF_STA, &s_last_applied_wifi_config) == ESP_OK) {
            s_last_requested_wifi_config = *config;
            s_last_wifi_config_valid = true;
        }
    }
    return err;
}

static void wifi_make_station_config(wifi_config_t *config,
                                     const char *ssid, const char *password)
{
    memset(config, 0, sizeof(*config));
    strlcpy((char *)config->sta.ssid, ssid, sizeof(config->sta.ssid));
    strlcpy((char *)config->sta.password, password ? password : "",
            sizeof(config->sta.password));
    config->sta.scan_method = WIFI_FAST_SCAN;
    config->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
#if CONFIG_ESP_WIFI_ENABLE_WPA3_SAE
    config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#endif
#if defined(CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT) && \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    config->sta.disable_wpa3_compatible_mode = 1;
#endif
}

esp_err_t nuwax_wifi_prepare_boot_config(void)
{
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(wifi_operation_lock(), TAG, "Wi-Fi prepare owner busy");
    esp_err_t err = ESP_OK;
    if (!nuwax_wifi_provisioning_active() && !nuwax_wifi_is_connected() &&
        !atomic_load_explicit(&s_wifi_connect_requested, memory_order_acquire) &&
        !atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire)) {
        wifi_config_t current = {0};
        err = esp_wifi_get_config(WIFI_IF_STA, &current);
        if (err == ESP_OK && current.sta.ssid[0] != '\0') {
            /* SSID/password arrays in the SDK need not be NUL terminated. */
            char ssid[33] = {0}, password[65] = {0};
            memcpy(ssid, current.sta.ssid, sizeof(current.sta.ssid));
            memcpy(password, current.sta.password, sizeof(current.sta.password));
            wifi_config_t desired;
            wifi_make_station_config(&desired, ssid, password);
            err = wifi_set_config_guarded(&desired);
        }
    }
    wifi_operation_unlock();
    ESP_LOGI(TAG, "boot STA configuration prepared before wake err=%s",
             esp_err_to_name(err));
    return err;
}

static esp_err_t wifi_connect_with_credentials_locked(const char *ssid,
                                                       const char *password,
                                                       int64_t deadline_us,
                                                       unsigned intent_revision) {
    if (nuwax_wifi_provisioning_active() ||
        atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire) ||
        atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire) != intent_revision ||
        esp_timer_get_time() >= deadline_us) {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load_explicit(&s_wifi_simulated_offline,
                             memory_order_acquire)) {
        ESP_LOGI(TAG, "Wi-Fi connect suppressed while simulated offline is active");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Wi-Fi connect begin SSID: %s", ssid);
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init failed");
    ESP_LOGI(TAG, "Wi-Fi init complete, configuring SSID: %s", ssid);

    wifi_config_t wifi_config = {0};
    wifi_make_station_config(&wifi_config, ssid, password);

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    atomic_store_explicit(&s_wifi_retry_count, 0, memory_order_release);
    atomic_store_explicit(&s_wifi_connect_requested, false,
                          memory_order_release);
    if (nuwax_wifi_provisioning_active() ||
        atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire) ||
        atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire) != intent_revision ||
        esp_timer_get_time() >= deadline_us) {
        ESP_LOGI(TAG,
                 "Wi-Fi connect aborted before driver reconfiguration: provisioning handoff pending");
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_current_ssid, 0, sizeof(s_current_ssid));
    memset(s_current_ip, 0, sizeof(s_current_ip));
    esp_err_t err = wifi_driver_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "wifi disconnect before reconnect failed: %s", esp_err_to_name(err));
    }

    esp_err_t cfg_err = ESP_OK;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (nuwax_wifi_provisioning_active() ||
        atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire) ||
        atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire) != intent_revision ||
        esp_timer_get_time() >= deadline_us) {
            ESP_LOGI(TAG,
                     "Wi-Fi connect aborted during driver reconfiguration: provisioning handoff pending");
            return ESP_ERR_INVALID_STATE;
        }
        cfg_err = wifi_set_config_guarded(&wifi_config);
        if (cfg_err == ESP_OK) {
            break;
        }
        if (cfg_err != ESP_ERR_WIFI_STATE) {
            ESP_RETURN_ON_ERROR(cfg_err, TAG, "set wifi config failed");
        }
        ESP_LOGW(TAG, "Wi-Fi STA busy before set config, retrying (%d/10)", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    ESP_RETURN_ON_ERROR(cfg_err, TAG, "set wifi config failed");
    ESP_LOGI(TAG, "Wi-Fi config set SSID: %s", ssid);
    if (nuwax_wifi_provisioning_active() ||
        atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire) ||
        atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire) != intent_revision ||
        esp_timer_get_time() >= deadline_us) {
        ESP_LOGI(TAG,
                 "Wi-Fi connect aborted before STA connect: provisioning handoff pending");
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_wifi_connect_requested, true,
                          memory_order_release);
    ESP_RETURN_ON_ERROR(wifi_driver_connect_once(), TAG, "wifi connect failed");
    if (nuwax_wifi_provisioning_active() ||
        atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire) ||
        atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire) != intent_revision ||
        esp_timer_get_time() >= deadline_us) {
        atomic_store_explicit(&s_wifi_connect_requested, false,
                              memory_order_release);
        (void)wifi_driver_disconnect();
        ESP_LOGI(TAG,
                 "Wi-Fi connect cancelled after STA start: provisioning handoff pending");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s", ssid);
    EventBits_t bits = 0;
    while (esp_timer_get_time() < deadline_us) {
        if (nuwax_wifi_provisioning_active() ||
            atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire) ||
            atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire) != intent_revision) {
            atomic_store_explicit(&s_wifi_connect_requested, false, memory_order_release);
            (void)wifi_driver_disconnect();
            return ESP_ERR_INVALID_STATE;
        }
        int64_t left_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) break;
        bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                  pdFALSE, pdFALSE, pdMS_TO_TICKS(left_ms < 100 ? left_ms : 100));
        if (bits & (WIFI_CONNECTED_BIT | WIFI_FAIL_BIT)) break;
    }

    if (bits & WIFI_CONNECTED_BIT) {
        strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));
        ESP_LOGI(TAG, "Wi-Fi connect complete SSID: %s IP: %s", s_current_ssid, s_current_ip);
        return ESP_OK;
    }
    if (!(bits & WIFI_FAIL_BIT)) {
        ESP_LOGE(TAG, "Wi-Fi connect timed out after %d ms SSID: %s", WIFI_CONNECT_TIMEOUT_MS, ssid);
        atomic_store_explicit(&s_wifi_connect_requested, false,
                              memory_order_release);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGE(TAG, "Failed to connect to Wi-Fi SSID: %s", ssid);
    atomic_store_explicit(&s_wifi_connect_requested, false,
                          memory_order_release);
    return ESP_FAIL;
}

/* Called only under the operation mutex with retries disarmed. Drain STOP
 * and START events before admitting a new attempt; never wait in callbacks. */
static esp_err_t wifi_restart_sta_locked(int64_t deadline_us) {
    atomic_store_explicit(&s_wifi_connect_requested, false, memory_order_release);
    atomic_store_explicit(&s_wifi_driver_connecting, false, memory_order_release);
    xEventGroupClearBits(s_wifi_event_group,
                        WIFI_STA_STOPPED_BIT | WIFI_STA_STARTED_BIT | WIFI_CONNECTED_BIT);
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) return err;
    atomic_store_explicit(&s_sta_restart_needs_start, true, memory_order_release);
    EventBits_t stopped = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_STA_STOPPED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    /* Restore the interface even if the event loop was slow. Handoff to BLE
     * may have arrived while stopping; START must never auto-connect here. */
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    atomic_store_explicit(&s_sta_restart_needs_start, false, memory_order_release);
    EventBits_t started = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_STA_STARTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    if (!(stopped & WIFI_STA_STOPPED_BIT) || !(started & WIFI_STA_STARTED_BIT) ||
        esp_timer_get_time() >= deadline_us) return ESP_ERR_TIMEOUT;
    ESP_LOGI(TAG, "Wi-Fi STA stop/start completed with event fences");
    return ESP_OK;
}

static esp_err_t wifi_connect_with_credentials(const char *ssid,
                                               const char *password) {
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(wifi_operation_lock(), TAG,
                        "Wi-Fi operation owner busy");
    atomic_store_explicit(&s_synchronous_connect_active,
                          true,
                          memory_order_release);
    const int64_t started_us = esp_timer_get_time();
    const int64_t deadline_us = started_us + (int64_t)WIFI_CONNECT_TIMEOUT_MS * 1000;
    const unsigned intent_revision = atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire);
    atomic_store_explicit(&s_join_failure_streak, 0U, memory_order_release);
    atomic_store_explicit(&s_join_refresh_pending, false, memory_order_release);
    atomic_store_explicit(&s_join_refresh_allowed, true, memory_order_release);
    esp_err_t err = wifi_connect_with_credentials_locked(ssid, password, deadline_us, intent_revision);
    if (err != ESP_OK && atomic_exchange_explicit(&s_join_refresh_pending, false, memory_order_acq_rel) &&
        !nuwax_wifi_provisioning_active() &&
        !atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire) &&
        atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire) == intent_revision &&
        esp_timer_get_time() + 2000000 < deadline_us) {
        ESP_LOGI(TAG, "Wi-Fi STA restart elapsed_ms=%lu", (unsigned long)((esp_timer_get_time() - started_us) / 1000));
        err = wifi_restart_sta_locked(deadline_us);
        if (err == ESP_OK) {
            err = wifi_connect_with_credentials_locked(ssid, password, deadline_us, intent_revision);
        }
    }
    atomic_store_explicit(&s_join_refresh_allowed, false, memory_order_release);
    atomic_store_explicit(&s_join_refresh_pending, false, memory_order_release);
    ESP_LOGI(TAG, "Wi-Fi join finished elapsed_ms=%lu err=%s", (unsigned long)((esp_timer_get_time() - started_us) / 1000), esp_err_to_name(err));
    atomic_store_explicit(&s_synchronous_connect_active,
                          false,
                          memory_order_release);
    wifi_operation_unlock();
    return err;
}

esp_err_t nuwax_wifi_connect(void) {
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init failed");
    if (atomic_load_explicit(&s_wifi_simulated_offline,
                             memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(wifi_operation_lock(), TAG,
                        "Wi-Fi connect owner busy");
    esp_err_t result = ESP_FAIL;

    /* The official BLE provisioning manager persists the STA configuration
     * through esp_wifi_set_config(). Prefer that value on every later boot. */
    wifi_config_t provisioned = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &provisioned) == ESP_OK &&
        provisioned.sta.ssid[0] != '\0') {
        ESP_LOGI(TAG, "Using provisioned Wi-Fi SSID: %s",
                 (const char *)provisioned.sta.ssid);
        result = wifi_connect_with_credentials(
            (const char *)provisioned.sta.ssid,
            (const char *)provisioned.sta.password);
        if (result == ESP_OK || CONFIG_NUWAX_WIFI_SSID_B[0] == '\0' ||
            strcmp((const char *)provisioned.sta.ssid,
                   CONFIG_NUWAX_WIFI_SSID_B) == 0) {
            wifi_operation_unlock();
            return result;
        }
        ESP_LOGW(TAG,
                 "Provisioned Wi-Fi failed; trying explicit backup SSID: %s",
                 CONFIG_NUWAX_WIFI_SSID_B);
        atomic_store_explicit(&s_active_ssid_index, 1, memory_order_release);
        result = wifi_connect_with_credentials(CONFIG_NUWAX_WIFI_SSID_B,
                                               CONFIG_NUWAX_WIFI_PASSWORD_B);
        wifi_operation_unlock();
        return result;
    }

    if (strlen(CONFIG_NUWAX_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "No provisioned or build-time Wi-Fi credentials available");
        wifi_operation_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    /* Load last-working SSID index (0 = primary, 1 = backup) from NVS.
     * On a fresh device this defaults to primary. */
    nvs_handle_t h = 0;
    if (wifi_nvs_open(NVS_READONLY, &h) == ESP_OK) {
        uint8_t idx = 0;
        if (nvs_get_u8(h, WIFI_NVS_KEY_LAST_WORKING, &idx) == ESP_OK && idx <= 1) {
            atomic_store_explicit(&s_active_ssid_index,
                                  idx,
                                  memory_order_release);
        }
        wifi_nvs_close(h);
    }
    int active_index = atomic_load_explicit(&s_active_ssid_index,
                                            memory_order_acquire);
    ESP_LOGI(TAG, "Initial active SSID index: %d (%s)",
             active_index,
             active_index == 0 ? CONFIG_NUWAX_WIFI_SSID : CONFIG_NUWAX_WIFI_SSID_B);

    /* First try the active index. On failure, switch to the other SSID and retry once.
     * The WIFI_FALLBACK_BIT mechanism in the event handler also drives the same
     * switch on runtime disconnect-retry-exhausted. */
    for (int attempt = 0; attempt < 2; attempt++) {
        active_index = atomic_load_explicit(&s_active_ssid_index,
                                            memory_order_acquire);
        const char *ssid = (active_index == 0) ? CONFIG_NUWAX_WIFI_SSID : CONFIG_NUWAX_WIFI_SSID_B;
        const char *password = (active_index == 0) ? CONFIG_NUWAX_WIFI_PASSWORD : CONFIG_NUWAX_WIFI_PASSWORD_B;
        if (ssid[0] == '\0') {
            ESP_LOGW(TAG, "Active SSID slot %d is empty, skipping", active_index);
            atomic_store_explicit(&s_active_ssid_index,
                                  1 - active_index,
                                  memory_order_release);
            continue;
        }

        /* Saved credentials override build-time defaults when present. */
        char saved_ssid[33] = {0};
        char saved_password[65] = {0};
        esp_err_t saved_err = wifi_load_saved_credentials(saved_ssid, sizeof(saved_ssid),
                                                          saved_password, sizeof(saved_password));
        if (saved_err == ESP_OK && saved_ssid[0] != '\0' && strcmp(saved_ssid, ssid) == 0) {
            ssid = saved_ssid;
            password = saved_password;
        }

        ESP_LOGI(TAG, "Trying Wi-Fi SSID: %s (index=%d)", ssid, active_index);
        result = wifi_connect_with_credentials(ssid, password);
        if (result == ESP_OK) {
            wifi_persist_last_working_index((uint8_t)active_index);
            atomic_store_explicit(&s_fallback_in_progress,
                                  false,
                                  memory_order_release);
            wifi_operation_unlock();
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Wi-Fi connect failed on SSID index %d (err=%s)",
                 active_index, esp_err_to_name(result));
        if (CONFIG_NUWAX_WIFI_SSID_B[0] == '\0' || attempt == 1) {
            wifi_operation_unlock();
            return result;
        }
        /* Flip to the other slot and try again. */
        atomic_store_explicit(&s_active_ssid_index,
                              1 - active_index,
                              memory_order_release);
        atomic_store_explicit(&s_wifi_retry_count, 0, memory_order_release);
    }
    wifi_operation_unlock();
    return result;
}

/* Fallback kicker used by the event handler when retries are exhausted:
 *  flips the active slot and re-runs the connect path with a fresh retry budget. */
esp_err_t nuwax_wifi_run_fallback(void) {
    if (atomic_load_explicit(&s_wifi_simulated_offline,
                             memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        atomic_store_explicit(&s_fallback_in_progress,
                              false,
                              memory_order_release);
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = wifi_operation_lock();
    if (err != ESP_OK) {
        atomic_store_explicit(&s_fallback_in_progress,
                              false,
                              memory_order_release);
        ESP_LOGW(TAG, "Wi-Fi fallback owner busy: %s",
                 esp_err_to_name(err));
        return err;
    }
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_FALLBACK_BIT | WIFI_FAIL_BIT);
    }
    const int current_index = atomic_load_explicit(&s_active_ssid_index,
                                                   memory_order_acquire);
    const int target_index = 1 - current_index;
    const char *target_ssid = target_index == 0 ? CONFIG_NUWAX_WIFI_SSID
                                                : CONFIG_NUWAX_WIFI_SSID_B;
    const char *target_password = target_index == 0
                                      ? CONFIG_NUWAX_WIFI_PASSWORD
                                      : CONFIG_NUWAX_WIFI_PASSWORD_B;
    ESP_LOGW(TAG,
             "Wi-Fi fallback triggered current_index=%d target_index=%d target_ssid=%s",
             current_index,
             target_index,
             target_ssid[0] ? target_ssid : "-");
    if (target_ssid[0] == '\0') {
        atomic_store_explicit(&s_fallback_in_progress,
                              false,
                              memory_order_release);
        wifi_operation_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    atomic_store_explicit(&s_active_ssid_index,
                          target_index,
                          memory_order_release);
    /* Persist the flip to NVS so nuwax_wifi_connect()'s entry-time NVS read
     * doesn't snap the index back to 0. */
    wifi_persist_last_working_index((uint8_t)target_index);
    atomic_store_explicit(&s_wifi_retry_count, 0, memory_order_release);
    atomic_store_explicit(&s_fallback_in_progress,
                          true,
                          memory_order_release);
    atomic_store_explicit(&s_wifi_connect_requested, false,
                          memory_order_release);
    err = wifi_connect_with_credentials(target_ssid, target_password);
    atomic_store_explicit(&s_fallback_in_progress,
                          false,
                          memory_order_release);
    wifi_operation_unlock();
    return err;
}

/* Called by ws_transport when WS connect fails N times in a row.
 *  Distinguishes "PC may be on a different LAN than this SSID" — flip to the
 *  other slot. Threshold mirrors the cloud-side ws_recover backoff: 3 calls. */
esp_err_t nuwax_wifi_notify_cloud_unreachable(void) {
    if (atomic_load_explicit(&s_wifi_simulated_offline,
                             memory_order_acquire) ||
        nuwax_wifi_provisioning_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    static int s_unreachable_count;
    s_unreachable_count++;
    ESP_LOGW(TAG, "Cloud unreachable on active SSID (%d/3)", s_unreachable_count);
    if (s_unreachable_count < 3) {
        return ESP_OK;
    }
    s_unreachable_count = 0;
    if (!s_wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load_explicit(&s_fallback_in_progress,
                             memory_order_acquire)) {
        return ESP_OK;
    }
    if (atomic_exchange_explicit(&s_fallback_in_progress,
                                 true,
                                 memory_order_acq_rel)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Switching Wi-Fi to other slot because current network cannot reach cloud");
    return nuwax_wifi_run_fallback();
}

esp_err_t nuwax_wifi_init(void) {
    return wifi_init_once();
}

bool nuwax_wifi_is_connected(void) {
    if (!s_wifi_event_group) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0 && s_current_ip[0] != '\0';
}

esp_err_t nuwax_wifi_get_rssi(int8_t *out_rssi_dbm) {
    if (!out_rssi_dbm) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_rssi_dbm = -127;
    if (!nuwax_wifi_is_connected() || nuwax_wifi_provisioning_owner_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_ap_record_t ap = {0};
    const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK || ap.rssi > 0) {
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    *out_rssi_dbm = ap.rssi;
    return ESP_OK;
}

bool nuwax_wifi_has_saved_credentials(void) {
    char ssid[33] = {0};
    char password[65] = {0};
    return wifi_load_saved_credentials(ssid, sizeof(ssid),
                                       password, sizeof(password)) == ESP_OK &&
           ssid[0] != '\0';
}

bool nuwax_wifi_saved_credentials_match(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') {
        return false;
    }
    char saved_ssid[33] = {0};
    char saved_password[65] = {0};
    if (wifi_load_saved_credentials(saved_ssid, sizeof(saved_ssid),
                                    saved_password, sizeof(saved_password)) != ESP_OK) {
        return false;
    }
    return strcmp(saved_ssid, ssid) == 0 &&
           strcmp(saved_password, password ? password : "") == 0;
}

esp_err_t nuwax_wifi_recover(void) {
    if (atomic_load_explicit(&s_wifi_simulated_offline,
                             memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (nuwax_wifi_is_connected()) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "Wi-Fi recovery init failed");
    ESP_RETURN_ON_ERROR(wifi_operation_lock(), TAG, "Wi-Fi recovery owner busy");
    const unsigned intent_revision = atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (nuwax_wifi_provisioning_active() ||
        atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire)) goto done;
    if (nuwax_wifi_is_connected()) { result = ESP_OK; goto done; }
    if (atomic_load_explicit(&s_wifi_connect_requested, memory_order_acquire) &&
        !(xEventGroupGetBits(s_wifi_event_group) & WIFI_FAIL_BIT)) {
        /* Join a running or queued attempt. Do not disconnect/set_config while
         * authentication, association or DHCP is already in progress. */
        xEventGroupClearBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
        atomic_store_explicit(&s_synchronous_connect_active, true, memory_order_release);
        result = wifi_driver_connect_once();
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi recovery joining current STA attempt");
            result = ESP_ERR_TIMEOUT;
            const TickType_t started = xTaskGetTickCount();
            while ((TickType_t)(xTaskGetTickCount() - started) < pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)) {
                if (nuwax_wifi_provisioning_active() ||
                    atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire)) {
                    result = ESP_ERR_INVALID_STATE; break;
                }
                const EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                    WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(100));
                if (atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire) != intent_revision) {
                    result = ESP_ERR_INVALID_STATE; break;
                }
                if (bits & WIFI_CONNECTED_BIT) { result = ESP_OK; break; }
                if (bits & WIFI_FAIL_BIT) { result = ESP_FAIL; break; }
            }
        }
        atomic_store_explicit(&s_synchronous_connect_active, false, memory_order_release);
        if (atomic_load_explicit(&s_wifi_intent_revision, memory_order_acquire) != intent_revision)
            result = ESP_ERR_INVALID_STATE;
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            /* A stalled driver must not leave an immortal join flag. A later
             * recovery can start a fresh configuration after this bounded wait. */
            atomic_store_explicit(&s_wifi_connect_requested, false, memory_order_release);
            atomic_store_explicit(&s_wifi_driver_connecting, false, memory_order_release);
        }
    } else {
        ESP_LOGI(TAG, "Wi-Fi recovery restarting STA with current credentials");
        result = nuwax_wifi_connect(); // The operation mutex is recursive.
    }
done:
    wifi_operation_unlock();
    return result;
}

bool nuwax_wifi_simulated_offline(void) {
#if NUWAX_TEST_NETWORK_ENABLED
    return atomic_load_explicit(&s_wifi_simulated_offline, memory_order_acquire);
#else
    return false;
#endif
}

esp_err_t nuwax_wifi_set_simulated_offline(bool enabled) {
#if !NUWAX_TEST_NETWORK_ENABLED
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool previous = atomic_exchange_explicit(
        &s_wifi_simulated_offline, enabled, memory_order_acq_rel);
    if (previous == enabled) {
        return ESP_OK;
    }
    atomic_fetch_add_explicit(&s_wifi_intent_revision, 1U, memory_order_acq_rel);
    atomic_store_explicit(&s_wifi_retry_count, 0, memory_order_release);
    atomic_store_explicit(&s_fallback_in_progress, false,
                          memory_order_release);
    atomic_store_explicit(&s_deferred_reconnect_driver_failures, 0U,
                          memory_order_release);
    if (enabled) {
        /* Keep the STA association alive so the host and replay gateway stay
         * on the same LAN; only cloud reconnect work is fenced. */
        atomic_store_explicit(&s_wifi_connect_requested, false,
                              memory_order_release);
        atomic_store_explicit(&s_reconnect_after_scan, false,
                              memory_order_release);
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group,
                                 WIFI_FAIL_BIT | WIFI_FALLBACK_BIT |
                                     WIFI_RECONNECT_BIT);
        }
        ESP_LOGI(TAG,
                 "simulated offline enabled; physical Wi-Fi remains associated=%d",
                 nuwax_wifi_is_connected() ? 1 : 0);
    } else {
        wifi_config_t config = {0};
        const bool has_credentials =
            esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK &&
            config.sta.ssid[0] != '\0';
        atomic_store_explicit(&s_wifi_connect_requested, has_credentials,
                              memory_order_release);
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group,
                                 WIFI_FAIL_BIT | WIFI_FALLBACK_BIT);
            if (has_credentials && !nuwax_wifi_is_connected()) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
            }
        }
        ESP_LOGI(TAG,
                 "simulated offline disabled; Wi-Fi recovery rearmed credentials=%d connected=%d",
                 has_credentials ? 1 : 0,
                 nuwax_wifi_is_connected() ? 1 : 0);
    }
    return ESP_OK;
#endif
}

esp_err_t nuwax_wifi_get_saved_networks(nuwax_wifi_saved_network_t *results, int max_results, int *out_count) {
    if (!results || max_results <= 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    esp_err_t err = wifi_load_saved_network_list();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    int count = s_saved_network_count;
    if (count > max_results) {
        count = max_results;
    }
    for (int i = 0; i < count; i++) {
        strlcpy(results[i].ssid, s_saved_network_ssids[i], sizeof(results[i].ssid));
    }
    *out_count = count;
    return ESP_OK;
}

esp_err_t nuwax_wifi_connect_saved_index(int index) {
    if (index < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = wifi_load_saved_network_list();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    if (index >= s_saved_network_count) {
        return ESP_ERR_INVALID_ARG;
    }
    char ssid[33] = {0};
    char password[65] = {0};
    strlcpy(ssid, s_saved_network_ssids[index], sizeof(ssid));
    strlcpy(password, s_saved_network_passwords[index], sizeof(password));
    if (ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    err = wifi_connect_with_credentials(ssid, password);
    if (err == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_save_credentials(ssid, password));
        ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_save_network_name(ssid, password));
    }
    return err;
}

static esp_err_t wifi_scan_locked(nuwax_wifi_scan_result_t *results,
                                  int max_results,
                                  int *out_count) {
    if (!results || max_results <= 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (nuwax_wifi_provisioning_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init failed");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = WIFI_ONLINE_SCAN_ACTIVE_MIN_MS,
            .max = WIFI_ONLINE_SCAN_ACTIVE_MAX_MS,
        },
        .home_chan_dwell_time = WIFI_ONLINE_SCAN_HOME_DWELL_MS,
    };
    atomic_store_explicit(&s_wifi_scan_succeeded,
                          false,
                          memory_order_release);
    xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_config, false), TAG,
                        "wifi scan start failed");

    const EventBits_t scan_bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_SCAN_DONE_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_ONLINE_SCAN_TIMEOUT_MS));
    if ((scan_bits & WIFI_SCAN_DONE_BIT) == 0) {
        ESP_LOGE(TAG, "Wi-Fi scan timed out after %u ms",
                 (unsigned)WIFI_ONLINE_SCAN_TIMEOUT_MS);
        esp_err_t stop_err = esp_wifi_scan_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_STATE) {
            ESP_LOGW(TAG, "wifi scan stop failed: %s",
                     esp_err_to_name(stop_err));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_clear_ap_list());
        return ESP_ERR_TIMEOUT;
    }
    if (!atomic_load_explicit(&s_wifi_scan_succeeded,
                              memory_order_acquire)) {
        ESP_LOGE(TAG, "Wi-Fi driver reported scan failure");
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_clear_ap_list());
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_count), TAG, "scan count failed");
    if (ap_count == 0) {
        return ESP_OK;
    }
    uint16_t copy_count = ap_count > 24 ? 24 : ap_count;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&copy_count, s_scan_records), TAG, "scan records failed");

    int written = 0;
    for (int i = 0; i < copy_count && written < max_results; i++) {
        if (s_scan_records[i].ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (int j = 0; j < written; j++) {
            if (strncmp(results[j].ssid, (const char *)s_scan_records[i].ssid, sizeof(results[j].ssid)) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        strlcpy(results[written].ssid, (const char *)s_scan_records[i].ssid, sizeof(results[written].ssid));
        results[written].rssi = s_scan_records[i].rssi;
        results[written].authmode = (uint8_t)s_scan_records[i].authmode;
        written++;
    }
    *out_count = written;
    ESP_LOGI(TAG, "Wi-Fi scan found %d networks", written);
    return ESP_OK;
}

esp_err_t nuwax_wifi_scan(nuwax_wifi_scan_result_t *results,
                          int max_results,
                          int *out_count) {
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(wifi_operation_lock(), TAG,
                        "Wi-Fi scan owner busy");
    if (nuwax_wifi_provisioning_active()) {
        wifi_operation_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_wifi_scan_in_progress,
                          true,
                          memory_order_release);
    const esp_err_t result = wifi_scan_locked(results, max_results, out_count);
    atomic_store_explicit(&s_wifi_scan_in_progress,
                          false,
                          memory_order_release);
    if (atomic_exchange_explicit(&s_reconnect_after_scan,
                                 false,
                                 memory_order_acq_rel) &&
        atomic_load_explicit(&s_wifi_connect_requested,
                             memory_order_acquire) &&
        !nuwax_wifi_provisioning_active()) {
        const esp_err_t reconnect_err = wifi_driver_connect_once();
        if (reconnect_err != ESP_OK && reconnect_err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG,
                     "deferred reconnect after scan failed: %s",
                     esp_err_to_name(reconnect_err));
        }
    }
    wifi_operation_unlock();
    return result;
}

esp_err_t nuwax_wifi_reconnect_to(const char *ssid, const char *password) {
    esp_err_t err = wifi_connect_with_credentials(ssid, password);
    if (err == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_save_credentials(ssid, password));
        ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_save_network_name(ssid, password));
    }
    return err;
}

esp_err_t nuwax_wifi_get_status(char *ssid, size_t ssid_len, char *ip, size_t ip_len) {
    if (ssid && ssid_len > 0) {
        strlcpy(ssid, s_current_ssid, ssid_len);
    }
    if (ip && ip_len > 0) {
        strlcpy(ip, s_current_ip, ip_len);
    }
    return ESP_OK;
}

esp_err_t nuwax_wifi_import_driver_credentials(void) {
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init failed");
    wifi_config_t config = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &config), TAG,
                        "get provisioned Wi-Fi config failed");
    char ssid[sizeof(config.sta.ssid) + 1] = {0};
    char password[sizeof(config.sta.password) + 1] = {0};
    memcpy(ssid, config.sta.ssid, sizeof(config.sta.ssid));
    memcpy(password, config.sta.password, sizeof(config.sta.password));
    if (!ssid[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(wifi_save_credentials(ssid, password), TAG,
                        "save provisioned credentials failed");
    ESP_RETURN_ON_ERROR(wifi_save_network_name(ssid, password), TAG,
                        "save provisioned network history failed");
    strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));
    return ESP_OK;
}
