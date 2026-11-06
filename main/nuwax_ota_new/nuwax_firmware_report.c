#include "nuwax_ota_new/nuwax_firmware_report.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "json_resource_budget.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nuwax_ota_new/nuwax_firmware_identity_policy.h"
#include "nuwax_ota_new/nuwax_firmware_report_policy.h"
#include "nuwax_ota_new/nuwax_ota_main.h"
#include "nuwax_provision_new/nuwax_cloud_config.h"
#include "nuwax_provision_new/nuwax_device_uid.h"
#include "sdkconfig.h"

#define FIRMWARE_REPORT_SEND_BUDGET_MS 200U
#define FIRMWARE_IDENTITY_TASK_STACK_BYTES 5120U
#define FIRMWARE_IDENTITY_STACK_WARN_BYTES 1024U
#define FIRMWARE_IDENTITY_INTERNAL_RESERVE_BYTES 1024U
#define FIRMWARE_IDENTITY_TASK_PRIORITY    1U
#define FIRMWARE_IDENTITY_TASK_CORE        0
#define NUWAX_FIRMWARE_TYPE "NuwaxAIPartner_V2"

static const char *TAG = "nuwax_fw_report";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static char s_report_id[NUWAX_FIRMWARE_REPORT_ID_MAX_LEN];
static char s_image_sha256[NUWAX_OTA_RUNNING_IDENTITY_SHA256_LEN];
static nuwax_firmware_report_policy_t s_policy;
static nuwax_firmware_identity_policy_t s_identity_policy;
static TaskHandle_t s_identity_task;
static bool s_identity_task_exited;

static void secure_zero_memory(void *value, size_t size);

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static void firmware_identity_worker(void *argument)
{
    (void)argument;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool claimed;
    portENTER_CRITICAL(&s_lock);
    claimed = nuwax_firmware_identity_policy_begin(&s_identity_policy);
    portEXIT_CRITICAL(&s_lock);

    char image_sha256[NUWAX_OTA_RUNNING_IDENTITY_SHA256_LEN] = {0};
    esp_err_t err = claimed
                        ? nuwax_ota_main_get_running_image_sha256(image_sha256)
                        : ESP_ERR_INVALID_STATE;
    const uint32_t completed_ms = monotonic_ms();

    portENTER_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        strlcpy(s_image_sha256, image_sha256, sizeof(s_image_sha256));
    }
    nuwax_firmware_identity_policy_complete(&s_identity_policy,
                                            err == ESP_OK,
                                            completed_ms);
    s_identity_task_exited = true;
    portEXIT_CRITICAL(&s_lock);

    const UBaseType_t stack_min_free = uxTaskGetStackHighWaterMark(NULL);
    if (stack_min_free < FIRMWARE_IDENTITY_STACK_WARN_BYTES) {
        ESP_LOGE(TAG,
                 "firmware identity worker stack reserve low min_free=%u stack_bytes=%u",
                 (unsigned)stack_min_free,
                 (unsigned)FIRMWARE_IDENTITY_TASK_STACK_BYTES);
    }
    secure_zero_memory(image_sha256, sizeof(image_sha256));
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "firmware identity recovery complete stack_min_free=%u",
                 (unsigned)stack_min_free);
    } else if (err == ESP_ERR_NOT_FINISHED) {
        ESP_LOGI(TAG, "firmware identity shared verification pending; retry deferred");
    } else {
        ESP_LOGE(TAG,
                 "firmware identity recovery failed err=%s; retry is deferred",
                 esp_err_to_name(err));
    }

    vTaskSuspend(NULL);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void firmware_identity_worker_reap(void)
{
    TaskHandle_t task = NULL;
    bool exited = false;
    portENTER_CRITICAL(&s_lock);
    task = s_identity_task;
    exited = s_identity_task_exited;
    portEXIT_CRITICAL(&s_lock);
    if (task == NULL || !exited || eTaskGetState(task) != eSuspended) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_identity_task != task || !s_identity_task_exited) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    s_identity_task = NULL;
    s_identity_task_exited = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDeleteWithCaps(task);
}

static esp_err_t firmware_identity_ensure_async(void)
{
    firmware_identity_worker_reap();

    const uint32_t now_ms = monotonic_ms();
    portENTER_CRITICAL(&s_lock);
    if (s_image_sha256[0] != '\0') {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);

    const size_t internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal_largest < FIRMWARE_IDENTITY_TASK_STACK_BYTES ||
        internal_free < FIRMWARE_IDENTITY_TASK_STACK_BYTES +
                            FIRMWARE_IDENTITY_INTERNAL_RESERVE_BYTES) {
        /* Identity reporting is reliable but non-gating. Do not consume its
         * retry transaction or compete with Wi-Fi/audio startup while the
         * internal heap cannot satisfy the complete task allocation. A later
         * idle service pass retries without delaying normal operation. */
        return ESP_ERR_NOT_FINISHED;
    }
    bool start_worker = false;
    portENTER_CRITICAL(&s_lock);
    start_worker = nuwax_firmware_identity_policy_request(&s_identity_policy,
                                                          now_ms);
    portEXIT_CRITICAL(&s_lock);

    if (!start_worker) {
        return ESP_ERR_NOT_FINISHED;
    }

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        firmware_identity_worker,
        "fw_identity",
        FIRMWARE_IDENTITY_TASK_STACK_BYTES,
        NULL,
        FIRMWARE_IDENTITY_TASK_PRIORITY,
        &task,
        FIRMWARE_IDENTITY_TASK_CORE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS || task == NULL) {
        portENTER_CRITICAL(&s_lock);
        nuwax_firmware_identity_policy_complete(&s_identity_policy,
                                                false,
                                                monotonic_ms());
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG,
                 "firmware identity worker allocation deferred stack_bytes=%u internal_free=%u internal_largest=%u",
                 (unsigned)FIRMWARE_IDENTITY_TASK_STACK_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                    MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NOT_FINISHED;
    }

    portENTER_CRITICAL(&s_lock);
    s_identity_task = task;
    s_identity_task_exited = false;
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(task);
    ESP_LOGI(TAG,
             "firmware identity recovery scheduled stack_bytes=%u priority=%u core=%d",
             (unsigned)FIRMWARE_IDENTITY_TASK_STACK_BYTES,
             (unsigned)FIRMWARE_IDENTITY_TASK_PRIORITY,
             FIRMWARE_IDENTITY_TASK_CORE);
    return ESP_ERR_NOT_FINISHED;
}

static void secure_zero_memory(void *value, size_t size)
{
    volatile unsigned char *cursor = (volatile unsigned char *)value;
    while (size-- > 0U) {
        *cursor++ = 0U;
    }
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:  return "task_watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

static bool parse_end_is_clean(const char *payload, int len,
                               const char *parse_end)
{
    if (payload == NULL || len <= 0 || parse_end == NULL) {
        return false;
    }
    const char *end = payload + len;
    while (parse_end < end && isspace((unsigned char)*parse_end)) {
        ++parse_end;
    }
    return parse_end == end;
}

static bool valid_id(const cJSON *item, size_t capacity)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    const size_t length = strlen(item->valuestring);
    if (length == 0U || length >= capacity) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)item->valuestring[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':')) {
            return false;
        }
    }
    return true;
}

static unsigned json_key_count(const cJSON *object, const char *name)
{
    unsigned count = 0U;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        if (item->string != NULL && strcmp(item->string, name) == 0) {
            ++count;
        }
    }
    return count;
}

static bool firmware_report_ack_keys_allowed(const cJSON *object)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        if (item->string == NULL ||
            (strcmp(item->string, "type") != 0 &&
             strcmp(item->string, "report_id") != 0 &&
             strcmp(item->string, "persisted") != 0)) {
            return false;
        }
    }
    return true;
}

esp_err_t nuwax_firmware_report_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);

    char report_id[NUWAX_FIRMWARE_REPORT_ID_MAX_LEN] = {0};
    const uint32_t report_hi = esp_random();
    const uint32_t report_lo = esp_random();
    snprintf(report_id, sizeof(report_id), "fw_%08lx%08lx",
             (unsigned long)report_hi, (unsigned long)report_lo);

    portENTER_CRITICAL(&s_lock);
    if (!s_initialized) {
        memcpy(s_report_id, report_id, sizeof(s_report_id));
        nuwax_firmware_report_policy_init(&s_policy);
        nuwax_firmware_identity_policy_init(&s_identity_policy);
        s_initialized = true;
    }
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "boot-scoped firmware report initialized report_id=%s",
             s_report_id);
    return ESP_OK;
}

static esp_err_t build_report_json(char **out_json)
{
    if (out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    char image_sha256[NUWAX_OTA_RUNNING_IDENTITY_SHA256_LEN] = {0};
    portENTER_CRITICAL(&s_lock);
    strlcpy(image_sha256, s_image_sha256, sizeof(image_sha256));
    portEXIT_CRITICAL(&s_lock);
    if (image_sha256[0] == '\0') {
        return ESP_ERR_NOT_FINISHED;
    }

    nuwax_cloud_config_t cloud_config = {0};
    esp_err_t config_err = nuwax_cloud_config_get(&cloud_config);
    const size_t secret_len =
        strnlen(cloud_config.device_secret, sizeof(cloud_config.device_secret));
    if (config_err != ESP_OK || secret_len == 0U ||
        secret_len >= sizeof(cloud_config.device_secret)) {
        secure_zero_memory(&cloud_config, sizeof(cloud_config));
        ESP_LOGE(TAG, "firmware report requires active device credentials");
        return config_err == ESP_OK ? ESP_ERR_INVALID_STATE : config_err;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *firmware = cJSON_CreateObject();
    if (root == NULL || firmware == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(firmware);
        secure_zero_memory(&cloud_config, sizeof(cloud_config));
        return ESP_ERR_NO_MEM;
    }

    const char *device_id = nuwax_device_uid_string();
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON_AddStringToObject(root, "type", "firmware_report");
    cJSON_AddStringToObject(root, "report_id", s_report_id);
    cJSON_AddStringToObject(root, "device_id",
                            device_id != NULL && device_id[0] != '\0'
                                ? device_id
                                : CONFIG_NUWAX_DEVICE_ID);
    cJSON_AddStringToObject(root, "device_secret",
                            cloud_config.device_secret);
    cJSON_AddStringToObject(root, "reset_reason",
                            reset_reason_name(esp_reset_reason()));
    cJSON_AddStringToObject(firmware, "firmware_type",
                            NUWAX_FIRMWARE_TYPE);
    cJSON_AddStringToObject(firmware, "version",
                            app != NULL ? app->version : "unknown");
    cJSON_AddStringToObject(firmware, "image_sha256", image_sha256);
    cJSON_AddItemToObject(root, "firmware", firmware);

    *out_json = cJSON_PrintUnformatted(root);
    cJSON *secret_item = cJSON_GetObjectItemCaseSensitive(root,
                                                          "device_secret");
    if (cJSON_IsString(secret_item) && secret_item->valuestring != NULL) {
        secure_zero_memory(secret_item->valuestring,
                           strlen(secret_item->valuestring));
    }
    cJSON_Delete(root);
    secure_zero_memory(&cloud_config, sizeof(cloud_config));
    return *out_json != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t nuwax_firmware_report_service(nuwax_ws_client_t *ws,
                                        uint32_t connection_epoch,
                                        uint32_t now_ms)
{
    if (ws == NULL || connection_epoch == 0U ||
        nuwax_ws_connection_epoch(ws) != connection_epoch ||
        !nuwax_ws_is_connected(ws)) {
        return ESP_ERR_INVALID_STATE;
    }

    bool initialized = false;
    bool should_send = false;
    portENTER_CRITICAL(&s_lock);
    initialized = s_initialized;
    if (initialized) {
        should_send = nuwax_firmware_report_policy_should_send(
            &s_policy, connection_epoch, now_ms);
    }
    portEXIT_CRITICAL(&s_lock);
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!should_send) {
        return ESP_OK;
    }

    const esp_err_t identity_err = firmware_identity_ensure_async();
    if (identity_err != ESP_OK) {
        if (identity_err != ESP_ERR_NOT_FINISHED) {
            ESP_LOGE(TAG, "cannot identify running firmware image: %s",
                     esp_err_to_name(identity_err));
        }
        return identity_err;
    }

    portENTER_CRITICAL(&s_lock);
    /* Arm the epoch before send so an immediate RX ACK cannot race the
     * successful send return. Identity preparation does not consume a report
     * attempt, so completion is published on the next service pass. */
    nuwax_firmware_report_policy_note_attempt(&s_policy,
                                              connection_epoch,
                                              now_ms);
    portEXIT_CRITICAL(&s_lock);

    char *json = NULL;
    esp_err_t err = build_report_json(&json);
    if (err == ESP_OK) {
        const int64_t deadline_us =
            esp_timer_get_time() +
            (int64_t)FIRMWARE_REPORT_SEND_BUDGET_MS * 1000LL;
        err = nuwax_ws_send_text_until_if_epoch(ws, json, deadline_us,
                                                connection_epoch);
    }
    if (json != NULL) {
        secure_zero_memory(json, strlen(json));
        free(json);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "firmware report sent report_id=%s epoch=%lu",
                 s_report_id, (unsigned long)connection_epoch);
    } else if (err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG,
                 "firmware report send deferred report_id=%s epoch=%lu err=%s",
                 s_report_id, (unsigned long)connection_epoch,
                 esp_err_to_name(err));
    }
    return err;
}

bool nuwax_firmware_report_handle_text(const char *payload,
                                       int len,
                                       uint32_t connection_epoch)
{
    if (payload == NULL || len <= 0) {
        return false;
    }
    const char *parse_end = NULL;
    cJSON *root = nuwax_json_parse_with_length_opts(payload, (size_t)len,
                                            &parse_end, false);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL ||
        strcmp(type->valuestring, "firmware_report_ack") != 0) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *report_id = cJSON_GetObjectItemCaseSensitive(root,
                                                              "report_id");
    const cJSON *persisted = cJSON_GetObjectItemCaseSensitive(root,
                                                              "persisted");
    const bool envelope_valid = parse_end_is_clean(payload, len, parse_end) &&
        firmware_report_ack_keys_allowed(root) &&
        json_key_count(root, "type") == 1U &&
        json_key_count(root, "report_id") == 1U &&
        json_key_count(root, "persisted") == 1U &&
        valid_id(report_id, sizeof(s_report_id)) &&
        cJSON_IsBool(persisted);
    if (!envelope_valid) {
        ESP_LOGW(TAG, "invalid firmware_report_ack envelope");
        cJSON_Delete(root);
        return true;
    }

    bool accepted = false;
    portENTER_CRITICAL(&s_lock);
    const bool ids_match = s_initialized &&
        strcmp(report_id->valuestring, s_report_id) == 0;
    if (ids_match) {
        accepted = nuwax_firmware_report_policy_accept_ack(
            &s_policy, connection_epoch, cJSON_IsTrue(persisted));
    }
    portEXIT_CRITICAL(&s_lock);
    if (accepted) {
        ESP_LOGI(TAG,
                 "firmware report persisted report_id=%s epoch=%lu",
                 s_report_id, (unsigned long)connection_epoch);
    } else {
        ESP_LOGW(TAG,
                 "firmware report ACK ignored ids_match=%d persisted=%d epoch=%lu",
                 ids_match ? 1 : 0, cJSON_IsTrue(persisted) ? 1 : 0,
                 (unsigned long)connection_epoch);
    }
    cJSON_Delete(root);
    return true;
}

bool nuwax_firmware_report_is_acked(void)
{
    bool acked;
    portENTER_CRITICAL(&s_lock);
    acked = s_initialized && s_policy.acked;
    portEXIT_CRITICAL(&s_lock);
    return acked;
}

const char *nuwax_firmware_report_id(void)
{
    return s_report_id;
}
