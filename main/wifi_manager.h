#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#define NUWAX_WIFI_MAX_SCAN_RESULTS 8
#define NUWAX_WIFI_MAX_SAVED_NETWORKS 8

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} nuwax_wifi_scan_result_t;

typedef struct {
    char ssid[33];
} nuwax_wifi_saved_network_t;

esp_err_t nuwax_wifi_connect(void);
esp_err_t nuwax_wifi_init(void);
/* Boot-only, before local wake starts: normalize the saved STA policy without
 * associating. Never alters an already connected or provisioning-owned STA. */
esp_err_t nuwax_wifi_prepare_boot_config(void);
esp_err_t nuwax_wifi_recover(void);
esp_err_t nuwax_wifi_set_simulated_offline(bool enabled);
bool nuwax_wifi_simulated_offline(void);
esp_err_t nuwax_wifi_start_fallback_watcher(void);
/* Publishes the runtime-provisioning intent without waiting for an in-flight
 * STA operation. New reconnect, fallback and scan work is fenced immediately;
 * the provisioning worker later claims the serialized driver owner. */
void nuwax_wifi_request_provisioning_handoff(void);
/* Cancels a handoff that has not yet acquired the Wi-Fi manager owner. This
 * operation is intentionally non-blocking and must not be used after
 * nuwax_wifi_set_provisioning_active(true) succeeds. */
bool nuwax_wifi_cancel_provisioning_handoff(void);
/* Gives the provisioning manager exclusive ownership of the STA driver.
 * While active, ordinary reconnect, fallback and scan requests fail closed. */
esp_err_t nuwax_wifi_set_provisioning_active(bool active);
bool nuwax_wifi_provisioning_active(void);
bool nuwax_wifi_provisioning_owner_active(void);
bool nuwax_wifi_is_connected(void);
/* Read-only station signal query for low-rate UI status updates. */
esp_err_t nuwax_wifi_get_rssi(int8_t *out_rssi_dbm);
/* NVS-only query used to migrate devices that were successfully provisioned
 * before the explicit verified marker was introduced. */
bool nuwax_wifi_has_saved_credentials(void);
bool nuwax_wifi_saved_credentials_match(const char *ssid, const char *password);
esp_err_t nuwax_wifi_notify_cloud_unreachable(void);
esp_err_t nuwax_wifi_scan(nuwax_wifi_scan_result_t *results, int max_results, int *out_count);
esp_err_t nuwax_wifi_reconnect_to(const char *ssid, const char *password);
esp_err_t nuwax_wifi_get_saved_networks(nuwax_wifi_saved_network_t *results, int max_results, int *out_count);
esp_err_t nuwax_wifi_connect_saved_index(int index);
esp_err_t nuwax_wifi_get_status(char *ssid, size_t ssid_len, char *ip, size_t ip_len);
/* Imports the ESP-IDF STA configuration written by network_provisioning into
 * the application's saved-network list. */
esp_err_t nuwax_wifi_import_driver_credentials(void);
