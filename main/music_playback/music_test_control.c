#include "music_test_control.h"
#include "music_playback.h"
#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "cJSON.h"
#include "json_resource_budget.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static QueueHandle_t s_commands;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_ack[80], s_reason[48];
static bool s_accepted;
static const char *field(cJSON *json, const char *name) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, name);
    return cJSON_IsString(value) ? value->valuestring : "";
}
bool nuwax_music_test_control_take(nuwax_music_test_command_t *out) {
    if (!s_commands || xQueueReceive(s_commands, out, 0) != pdTRUE) return false;
    portENTER_CRITICAL(&s_lock);
    const bool duplicate = strcmp(out->request_id, s_ack) == 0;
    portEXIT_CRITICAL(&s_lock);
    return !duplicate;
}
void nuwax_music_test_control_result(const char *id, bool accepted, const char *reason) {
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_ack, id, sizeof(s_ack));
    strlcpy(s_reason, reason ? reason : "", sizeof(s_reason));
    s_accepted = accepted;
    portEXIT_CRITICAL(&s_lock);
}
static uint32_t control_retry_delay(bool healthy, unsigned *failures) {
    if (healthy) { *failures = 0; return 500U; }
    if (*failures < 4U) ++*failures;
    const uint32_t delay = 500U << *failures;
    return delay > 5000U ? 5000U : delay;
}
static void control_task(void *unused) {
    (void)unused;
    uint8_t mac[6]; esp_efuse_mac_get_default(mac);
    char device[24], boot[16];
    snprintf(device, sizeof(device), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(boot, sizeof(boot), "%08lx", (unsigned long)esp_random());
    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/poll", CONFIG_NUWAX_MUSIC_CONTROL_TEST_URL);
    unsigned failures = 0;
    while (true) {
        if (!nuwax_wifi_is_connected()) {
            failures = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        bool healthy = false;
        nuwax_music_snapshot_t snapshot; nuwax_music_playback_snapshot(&snapshot);
        char ack[80], reason[48]; bool accepted;
        portENTER_CRITICAL(&s_lock);
        strlcpy(ack, s_ack, sizeof(ack)); strlcpy(reason, s_reason, sizeof(reason));
        accepted = s_accepted;
        portEXIT_CRITICAL(&s_lock);
        cJSON *json = cJSON_CreateObject();
        if (json) {
            cJSON_AddStringToObject(json, "device_id", device);
            cJSON_AddStringToObject(json, "boot_id", boot);
            cJSON_AddStringToObject(json, "ack", ack);
            cJSON_AddBoolToObject(json, "accepted", accepted);
            cJSON_AddStringToObject(json, "reason", reason);
            cJSON_AddStringToObject(json, "request_id", snapshot.request_id);
            cJSON_AddStringToObject(json, "state", snapshot.state);
            cJSON_AddStringToObject(json, "playback_reason", snapshot.reason);
            cJSON_AddNumberToObject(json, "sample_rate", snapshot.sample_rate);
            char *body = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            if (body) {
                esp_http_client_config_t config = {
                    .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 800,
                    .buffer_size = 1024, .buffer_size_tx = 1024,
                    .disable_auto_redirect = true,
                };
                esp_http_client_handle_t client = esp_http_client_init(&config);
                if (client) {
                    esp_http_client_set_header(client, "Content-Type", "application/json");
                    const size_t bytes = strlen(body);
                    if (esp_http_client_open(client, (int)bytes) == ESP_OK &&
                        esp_http_client_write(client, body, (int)bytes) == (int)bytes) {
                        const int64_t size = esp_http_client_fetch_headers(client);
                        char response[512];
                        if (esp_http_client_get_status_code(client) == 200 && size > 0 && size < sizeof(response)) {
                            const int n = esp_http_client_read_response(client, response, (int)size);
                            if (n == size) {
                                response[n] = 0;
                                cJSON *reply = nuwax_json_parse_with_length(response, (size_t)n);
                                if (cJSON_IsObject(reply)) {
                                    healthy = true;
                                    const char *id = field(reply, "request_id");
                                    const char *path = field(reply, "path");
                                    const char *format = field(reply, "format");
                                    const char *command = field(reply, "command");
                                    const bool stop = strcmp(command, "stop_music") == 0;
                                    if (id[0] && strlen(id) < 80U && strcmp(id, ack) &&
                                        (stop || (strcmp(command, "play_music") == 0 &&
                                         strncmp(path, "/sources/", 9) == 0 && strlen(path) < 160U &&
                                         !strstr(path, "..") && !strchr(path, '?') &&
                                         (strcmp(format, "mp3") == 0 || strcmp(format, "pcm") == 0)))) {
                                        nuwax_music_test_command_t next = {.stop = stop};
                                        strlcpy(next.request_id, id, sizeof(next.request_id));
                                        strlcpy(next.path, path, sizeof(next.path));
                                        strlcpy(next.format, format, sizeof(next.format));
                                        /* The owner deduplicates against its current ACK.
                                         * The ACK sent with this HTTP request may already
                                         * be stale when the response arrives. Never stop
                                         * audio here before that deduplication. */
                                        xQueueOverwrite(s_commands, &next);
                                    }
                                }
                                cJSON_Delete(reply);
                            }
                        }
                    }
                    esp_http_client_cleanup(client);
                }
                cJSON_free(body);
            }
        }
        const unsigned previous_failures = failures;
        const uint32_t delay = control_retry_delay(healthy, &failures);
        if (!healthy && !previous_failures)
            ESP_LOGW("music_test", "gateway poll failed; backing off url=%s", url);
        if (healthy && previous_failures)
            ESP_LOGI("music_test", "gateway poll recovered url=%s", url);
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}
esp_err_t nuwax_music_test_control_init(void) {
    if (s_commands) return ESP_OK;
    s_commands = xQueueCreate(1U, sizeof(nuwax_music_test_command_t));
    if (!s_commands) return ESP_ERR_NO_MEM;
    TaskHandle_t task = NULL;
    if (xTaskCreateWithCaps(control_task, "music_test", 6144U, NULL, 2U, &task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(s_commands); s_commands = NULL; return ESP_ERR_NO_MEM;
    }
    ESP_LOGI("music_test", "music control gateway=%s", CONFIG_NUWAX_MUSIC_CONTROL_TEST_URL);
    return ESP_OK;
}
