#include "time_sync.h"

#include <stdbool.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

static const char *TAG = "nuwax_time";
static bool s_sntp_started;
static bool s_time_synced;

static bool is_system_time_valid(void) {
    time_t now = 0;
    time(&now);
    return now > 1704067200;
}

esp_err_t nuwax_time_sync_once(void) {
    if (s_time_synced || is_system_time_valid()) {
        s_time_synced = true;
        return ESP_OK;
    }

    if (!s_sntp_started) {
        ESP_LOGI(TAG, "Starting SNTP time sync server=%s", CONFIG_NUWAX_TIME_SYNC_SERVER);
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_NUWAX_TIME_SYNC_SERVER);
        ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&config), TAG, "SNTP init failed");
        s_sntp_started = true;
    }

    const int wait_step_ms = 1500;
    const int timeout_ms = CONFIG_NUWAX_TIME_SYNC_TIMEOUT_MS;
    const int retry_count = (timeout_ms + wait_step_ms - 1) / wait_step_ms;
    for (int retry = 1; retry <= retry_count; retry++) {
        const int elapsed_budget_ms = (retry - 1) * wait_step_ms;
        const int remaining_ms = timeout_ms - elapsed_budget_ms;
        const int current_wait_ms = remaining_ms < wait_step_ms ? remaining_ms : wait_step_ms;
        esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(current_wait_ms));
        if (err == ESP_OK || is_system_time_valid()) {
            time_t now = 0;
            time(&now);
            ESP_LOGI(TAG, "System time synced epoch=%lld", (long long)now);
            s_time_synced = true;
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Waiting for SNTP time sync (%d/%d): %s", retry, retry_count, esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "SNTP time sync timed out; TLS certificate validation may fail");
    return ESP_ERR_TIMEOUT;
}
