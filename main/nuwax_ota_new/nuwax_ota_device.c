/* Device-facing OTA command controller for the Miaopan 2.0 product build. */
#include "nuwax_ota_device.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board/battery_service.h"
#include "board/nuwax_power_ctrl.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nuwax_ota.h"
#include "nuwax_ota_main.h"
#include "nuwax_ota_power_policy.h"
#include "nuwax_ota_manifest.h"
#include "nuwax_ota_result_ack_policy.h"
#include "sdkconfig.h"

#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "CONFIG_NUWAX_OTA_ENABLE requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y"
#endif

static const char *TAG = "nuwax_ota_device";

#define OTA_COMMAND_JSON_MAX_BYTES    1536U
#define OTA_REQUEST_ID_MAX_BYTES      64U
#define OTA_CONTROLLER_STACK_BYTES     12288U
#define OTA_CONTROLLER_STACK_WARN_BYTES 3072U
#define OTA_CONTROLLER_PRIORITY       4U
#define OTA_CONTEXT_NAMESPACE         "nuwax_ota_ctx"
#define OTA_CONTEXT_REQUEST_KEY       "request_id"
#define OTA_CONTEXT_UPDATE_KEY        "update_id"
#define OTA_CONTEXT_VERSION_KEY       "version"
#define OTA_CONTEXT_SHA_KEY           "image_sha256"
#define OTA_CONTEXT_SEQUENCE_KEY      "release_seq"
#define OTA_CONTEXT_PARTITION_KEY     "target_addr"
#define OTA_CONTEXT_FINGERPRINT_KEY   "manifest_fp"
#define OTA_NOTIFY_CLOUD_READY        (1UL << 0)
#define OTA_NOTIFY_RESULT_ACK         (1UL << 1)
#define OTA_HEALTH_PENDING_LOG_MS      5000U
#define OTA_HEALTH_DETAIL_BYTES        128U
#define OTA_TERMINAL_SEND_ATTEMPTS     3U
#define OTA_TERMINAL_RETRY_BASE_MS      150U
#define OTA_RESULT_ACK_TIMEOUT_MS       2500U

typedef struct {
    char json[OTA_COMMAND_JSON_MAX_BYTES];
    char request_id[OTA_REQUEST_ID_MAX_BYTES];
    char update_id[NUWAX_OTA_MANIFEST_ID_MAX_LEN];
    char version[NUWAX_OTA_MANIFEST_VERSION_MAX_LEN];
    char image_sha256[NUWAX_OTA_MANIFEST_SHA256_LEN];
    uint32_t manifest_fingerprint;
    uint32_t connection_epoch;
} ota_command_item_t;

typedef struct {
    char request_id[OTA_REQUEST_ID_MAX_BYTES];
    char update_id[NUWAX_OTA_MANIFEST_ID_MAX_LEN];
    char version[NUWAX_OTA_MANIFEST_VERSION_MAX_LEN];
    char image_sha256[NUWAX_OTA_MANIFEST_SHA256_LEN];
    uint64_t release_sequence;
    uint32_t manifest_fingerprint;
    uint32_t target_partition_address;
} ota_report_context_t;

typedef enum {
    OTA_BOOT_DECISION_NONE = 0,
    OTA_BOOT_DECISION_CLOUD_READY,
    OTA_BOOT_DECISION_TIMEOUT,
    OTA_BOOT_DECISION_LOCAL_HEALTH_FAILED,
    OTA_BOOT_DECISION_COMPLETE,
} ota_boot_decision_owner_t;

typedef enum {
    OTA_TERMINAL_REPORT_NONE = 0,
    OTA_TERMINAL_REPORT_IN_FLIGHT,
    OTA_TERMINAL_REPORT_COMPLETE,
} ota_terminal_report_state_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static nuwax_ota_device_callbacks_t s_callbacks;
static QueueHandle_t s_commands;
static TaskHandle_t s_controller_task;
static bool s_initialized;
static bool s_cloud_authenticated;
static uint32_t s_cloud_authenticated_epoch;
static bool s_command_pending;
static bool s_active;
static bool s_audio_blocked;
static ota_terminal_report_state_t s_terminal_report_state;
static bool s_terminal_delivery_ok;
static bool s_boot_pending_verify;
static ota_boot_decision_owner_t s_boot_decision_owner;
static uint32_t s_boot_validation_deadline_ms;
static bool s_cloud_ready_event_pending;
static uint32_t s_cloud_ready_event_ms;
static bool s_health_pending_log_valid;
static uint32_t s_health_pending_log_ms;
static char s_health_pending_log_detail[OTA_HEALTH_DETAIL_BYTES];
static int s_last_progress_pct = -1;
static nuwax_ota_state_t s_last_progress_state = NUWAX_OTA_STATE_IDLE;
static ota_report_context_t s_report;
static char s_pending_update_id[NUWAX_OTA_MANIFEST_ID_MAX_LEN];
static char s_active_image_sha256[NUWAX_OTA_MANIFEST_SHA256_LEN];
static bool s_expected_result_ok;
static bool s_result_ack_received;

static uint32_t ota_manifest_fingerprint(
    const nuwax_ota_manifest_t *manifest)
{
    if (manifest == NULL) {
        return 0U;
    }
    const uint8_t *bytes = (const uint8_t *)manifest;
    uint32_t hash = 2166136261U;
    for (size_t i = 0U; i < sizeof(*manifest); ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash ? hash : 1U;
}

static esp_err_t process_cloud_ready_event(uint32_t ready_ms);

/* Caller must hold s_lock. */
static bool cloud_authenticated_for_epoch_locked(uint32_t connection_epoch)
{
    return s_cloud_authenticated && connection_epoch != 0U &&
           s_cloud_authenticated_epoch == connection_epoch;
}

static void log_controller_stack_watermark(const char *stage)
{
    /* ESP-IDF reports task stack depth and high-water marks in bytes. */
    const size_t free_bytes =
        (size_t)uxTaskGetStackHighWaterMark(NULL);
    const size_t internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free_bytes < OTA_CONTROLLER_STACK_WARN_BYTES) {
        ESP_LOGW(TAG,
                 "controller resources stage=%s stack_min_free=%u internal_free=%u internal_largest=%u",
                 stage, (unsigned)free_bytes, (unsigned)internal_free,
                 (unsigned)internal_largest);
    } else {
        ESP_LOGI(TAG,
                 "controller resources stage=%s stack_min_free=%u internal_free=%u internal_largest=%u",
                 stage, (unsigned)free_bytes, (unsigned)internal_free,
                 (unsigned)internal_largest);
    }
}

static ota_command_item_t *allocate_command_item(void)
{
    ota_command_item_t *item = heap_caps_calloc(
        1, sizeof(*item), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (item == NULL) {
        item = heap_caps_calloc(
            1, sizeof(*item), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return item;
}

static void free_command_item(ota_command_item_t *item)
{
    if (item != NULL) {
        memset(item, 0, sizeof(*item));
        heap_caps_free(item);
    }
}

/* Caller must hold s_lock. Acquire new owners before releasing old owners so
 * a boot-verification to OTA-command transition never briefly opens SW3. */
static bool apply_interlocks_locked(void)
{
    const bool update_inhibit = s_command_pending || s_active;
    const bool boot_inhibit = s_boot_pending_verify;
    bool applied = true;

    if (boot_inhibit && !nuwax_power_ctrl_acquire_shutdown_inhibit(
                            NUWAX_POWER_INHIBIT_OTA_BOOT)) {
        applied = false;
    }
    if (update_inhibit && !nuwax_power_ctrl_acquire_shutdown_inhibit(
                              NUWAX_POWER_INHIBIT_OTA_UPDATE)) {
        applied = false;
    }
    if (!update_inhibit) {
        (void)nuwax_power_ctrl_release_shutdown_inhibit(
            NUWAX_POWER_INHIBIT_OTA_UPDATE);
    }
    if (!boot_inhibit) {
        (void)nuwax_power_ctrl_release_shutdown_inhibit(
            NUWAX_POWER_INHIBIT_OTA_BOOT);
    }
    s_audio_blocked = update_inhibit || boot_inhibit;
    return applied;
}

static bool refresh_interlocks(void)
{
    bool inhibit;
    bool applied;
    portENTER_CRITICAL(&s_lock);
    inhibit = s_command_pending || s_active || s_boot_pending_verify;
    applied = apply_interlocks_locked();
    portEXIT_CRITICAL(&s_lock);
    if (!applied && inhibit) {
        ESP_LOGW(TAG, "OTA interlock lost race with an accepted SW3 shutdown");
    }
    return applied;
}

static bool begin_terminal_report(void)
{
    bool claimed = false;
    portENTER_CRITICAL(&s_lock);
    if (s_terminal_report_state == OTA_TERMINAL_REPORT_NONE) {
        s_terminal_report_state = OTA_TERMINAL_REPORT_IN_FLIGHT;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return claimed;
}

static void complete_terminal_report(esp_err_t delivery_result)
{
    portENTER_CRITICAL(&s_lock);
    s_terminal_report_state = delivery_result == ESP_OK
                                  ? OTA_TERMINAL_REPORT_COMPLETE
                                  : OTA_TERMINAL_REPORT_NONE;
    s_terminal_delivery_ok = delivery_result == ESP_OK;
    portEXIT_CRITICAL(&s_lock);
}

bool nuwax_ota_device_inhibit_shutdown_during_boot(void)
{
    const bool applied = nuwax_power_ctrl_acquire_shutdown_inhibit(
        NUWAX_POWER_INHIBIT_OTA_BOOT);
    if (!applied) {
        ESP_LOGE(TAG, "OTA boot interlock lost race with an accepted SW3 shutdown");
    }
    return applied;
}

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool deadline_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static bool should_log_health_pending(uint32_t now, const char *detail)
{
    const char *safe_detail = detail ? detail : "";
    bool should_log = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_health_pending_log_valid ||
        strcmp(s_health_pending_log_detail, safe_detail) != 0 ||
        deadline_reached(now,
                         s_health_pending_log_ms + OTA_HEALTH_PENDING_LOG_MS)) {
        s_health_pending_log_valid = true;
        s_health_pending_log_ms = now;
        strlcpy(s_health_pending_log_detail, safe_detail,
                sizeof(s_health_pending_log_detail));
        should_log = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return should_log;
}

static void reset_health_pending_log(void)
{
    portENTER_CRITICAL(&s_lock);
    s_health_pending_log_valid = false;
    s_health_pending_log_ms = 0;
    s_health_pending_log_detail[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
}

static bool valid_identifier(const char *value, size_t max_bytes)
{
    if (value == NULL || value[0] == '\0' || strlen(value) >= max_bytes ||
        !isalnum((unsigned char)value[0])) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' ||
              *p == '+')) {
            return false;
        }
    }
    return true;
}

static bool valid_sha256(const char *value)
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

static bool result_ack_key_allowed(const char *name)
{
    return name != NULL &&
           (strcmp(name, "type") == 0 || strcmp(name, "update_id") == 0 ||
            strcmp(name, "ok") == 0);
}

static esp_err_t send_json_object(cJSON *root)
{
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nuwax_ota_device_callbacks_t callbacks;
    portENTER_CRITICAL(&s_lock);
    callbacks = s_callbacks;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t err = callbacks.send_json != NULL
                        ? callbacks.send_json(callbacks.ctx, json)
                        : ESP_ERR_INVALID_STATE;
    free(json);
    return err;
}

static ota_report_context_t report_context_snapshot(void)
{
    ota_report_context_t context;
    portENTER_CRITICAL(&s_lock);
    context = s_report;
    portEXIT_CRITICAL(&s_lock);
    return context;
}

static esp_err_t emit_ack(const char *request_id, const char *update_id,
                          bool accepted, bool duplicate,
                          const char *state, const char *code,
                          const char *message)
{
    (void)request_id;
    (void)duplicate;
    (void)state;
    if (!accepted) {
        ESP_LOGW(TAG, "OTA command rejected update_id=%s reason=%s detail=%s",
                 update_id ? update_id : "-", code ? code : "bad_request",
                 message ? message : "rejected");
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "ota_ack");
    cJSON_AddStringToObject(root, "update_id", update_id ? update_id : "");
    cJSON_AddBoolToObject(root, "accepted", accepted);
    return send_json_object(root);
}

static esp_err_t emit_result(const char *status, const char *message)
{
    const ota_report_context_t context = report_context_snapshot();
    const bool ok = status != NULL &&
                    (strcmp(status, "staged") == 0 ||
                     strcmp(status, "committed") == 0);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "ota_result");
    cJSON_AddStringToObject(root, "update_id", context.update_id);
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    return send_json_object(root);
}

static void arm_result_ack(bool ok)
{
    portENTER_CRITICAL(&s_lock);
    s_expected_result_ok = ok;
    s_result_ack_received = false;
    portEXIT_CRITICAL(&s_lock);
}

static bool result_ack_received(void)
{
    bool received;
    portENTER_CRITICAL(&s_lock);
    received = s_result_ack_received;
    portEXIT_CRITICAL(&s_lock);
    return received;
}

static esp_err_t wait_for_result_ack(void)
{
    const uint32_t deadline = monotonic_ms() + OTA_RESULT_ACK_TIMEOUT_MS;
    for (;;) {
        if (result_ack_received()) {
            return ESP_OK;
        }
        const uint32_t now = monotonic_ms();
        if (deadline_reached(now, deadline)) {
            return ESP_ERR_TIMEOUT;
        }
        const uint32_t remaining_ms = deadline - now;
        uint32_t notified = 0U;
        (void)xTaskNotifyWait(0U, OTA_NOTIFY_RESULT_ACK, &notified,
                              pdMS_TO_TICKS(remaining_ms));
    }
}

static esp_err_t emit_result_reliably(const char *status,
                                      const char *message)
{
    const bool ok = status != NULL &&
                    (strcmp(status, "staged") == 0 ||
                     strcmp(status, "committed") == 0);
    esp_err_t err = ESP_FAIL;
    /* Arm once for the complete retry window. A delayed ACK can arrive while
     * the controller is between attempts and must not be cleared. */
    arm_result_ack(ok);
    for (unsigned attempt = 0; attempt < OTA_TERMINAL_SEND_ATTEMPTS; ++attempt) {
        if (result_ack_received()) {
            return ESP_OK;
        }
        err = emit_result(status, message);
        if (err == ESP_OK) {
            err = wait_for_result_ack();
            if (err == ESP_OK) {
                return ESP_OK;
            }
        }
        ESP_LOGW(TAG,
                 "terminal OTA result delivery attempt %u/%u failed: %s",
                 attempt + 1U, OTA_TERMINAL_SEND_ATTEMPTS,
                 esp_err_to_name(err));
        if (attempt + 1U < OTA_TERMINAL_SEND_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(
                OTA_TERMINAL_RETRY_BASE_MS << attempt));
        }
    }
    return err;
}

static bool handle_result_ack(const cJSON *root, const char *parse_end,
                              const char *payload, int len,
                              uint32_t connection_epoch)
{
    const cJSON *update = cJSON_GetObjectItemCaseSensitive(root, "update_id");
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    while (parse_end != NULL && parse_end < payload + len &&
           isspace((unsigned char)*parse_end)) {
        ++parse_end;
    }
    bool keys_valid = json_key_count(root, "type") == 1U &&
                      json_key_count(root, "update_id") == 1U &&
                      json_key_count(root, "ok") == 1U;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        keys_valid = keys_valid && result_ack_key_allowed(item->string);
    }
    if (!keys_valid || !cJSON_IsString(update) ||
        update->valuestring == NULL ||
        !valid_identifier(update->valuestring,
                          NUWAX_OTA_MANIFEST_ID_MAX_LEN) ||
        !cJSON_IsBool(ok) ||
        parse_end != payload + len) {
        ESP_LOGW(TAG, "invalid ota_result_ack envelope");
        return true;
    }

    const bool received_ok = cJSON_IsTrue(ok);
    bool matches = false;
    bool authenticated_epoch_matches = false;
    bool update_id_matches = false;
    bool result_ok_matches = false;
    bool expected_result_ok = false;
    nuwax_ota_result_ack_decision_t ack_decision =
        NUWAX_OTA_RESULT_ACK_REJECT;
    ota_terminal_report_state_t report_state = OTA_TERMINAL_REPORT_NONE;
    char expected_update_id[NUWAX_OTA_MANIFEST_ID_MAX_LEN] = {0};
    TaskHandle_t controller = NULL;
    portENTER_CRITICAL(&s_lock);
    authenticated_epoch_matches =
        cloud_authenticated_for_epoch_locked(connection_epoch);
    update_id_matches =
        strcmp(update->valuestring, s_report.update_id) == 0;
    expected_result_ok = s_expected_result_ok;
    ack_decision = nuwax_ota_result_ack_policy_decide(expected_result_ok,
                                                       received_ok);
    result_ok_matches = ack_decision != NUWAX_OTA_RESULT_ACK_REJECT;
    report_state = s_terminal_report_state;
    strlcpy(expected_update_id, s_report.update_id,
            sizeof(expected_update_id));
    matches = authenticated_epoch_matches && update_id_matches &&
              result_ok_matches &&
              report_state == OTA_TERMINAL_REPORT_IN_FLIGHT;
    if (matches) {
        s_result_ack_received = true;
        controller = s_controller_task;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!matches) {
        ESP_LOGW(TAG,
                 "ignoring unmatched ota_result_ack epoch_ok=%d report_state=%d update_ok=%d expected_update=%s received_update=%s result_ok=%d expected_ok=%d received_ok=%d",
                 authenticated_epoch_matches ? 1 : 0,
                 (int)report_state,
                 update_id_matches ? 1 : 0,
                 expected_update_id[0] ? expected_update_id : "-",
                 update->valuestring,
                 result_ok_matches ? 1 : 0,
                 expected_result_ok ? 1 : 0,
                 received_ok ? 1 : 0);
        return true;
    }
    if (ack_decision == NUWAX_OTA_RESULT_ACK_PERSISTED_SUCCESS_COMPAT) {
        ESP_LOGW(TAG,
                 "accepted ota_result_ack persistence compatibility update_id=%s expected_ok=0 received_ok=1",
                 update->valuestring);
    }
    if (controller != NULL) {
        (void)xTaskNotify(controller, OTA_NOTIFY_RESULT_ACK, eSetBits);
    }
    return true;
}

static void request_reconnect_after_terminal_failure(esp_err_t delivery_result)
{
    if (delivery_result == ESP_OK) {
        return;
    }
    nuwax_ota_device_callbacks_t callbacks;
    portENTER_CRITICAL(&s_lock);
    callbacks = s_callbacks;
    portEXIT_CRITICAL(&s_lock);
    if (callbacks.request_cloud_reconnect == NULL) {
        ESP_LOGE(TAG, "terminal OTA result is durable but reconnect callback is unavailable");
        return;
    }
    const esp_err_t reconnect_err =
        callbacks.request_cloud_reconnect(callbacks.ctx);
    if (reconnect_err != ESP_OK) {
        ESP_LOGE(TAG, "cannot force OTA terminal-result reconnect: %s",
                 esp_err_to_name(reconnect_err));
    }
}

static void request_firmware_metadata_refresh(void)
{
    nuwax_ota_device_callbacks_t callbacks;
    portENTER_CRITICAL(&s_lock);
    callbacks = s_callbacks;
    portEXIT_CRITICAL(&s_lock);
    if (callbacks.request_cloud_reconnect == NULL) {
        ESP_LOGE(TAG, "cannot refresh firmware metadata: reconnect callback unavailable");
        return;
    }
    const esp_err_t reconnect_err =
        callbacks.request_cloud_reconnect(callbacks.ctx);
    if (reconnect_err != ESP_OK) {
        ESP_LOGE(TAG, "cannot refresh committed firmware metadata: %s",
                 esp_err_to_name(reconnect_err));
    }
}

static esp_err_t persist_report_context(const ota_report_context_t *context)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_CONTEXT_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, OTA_CONTEXT_REQUEST_KEY, context->request_id);
    if (err == ESP_OK) err = nvs_set_str(handle, OTA_CONTEXT_UPDATE_KEY,
                                         context->update_id);
    if (err == ESP_OK) err = nvs_set_str(handle, OTA_CONTEXT_VERSION_KEY,
                                         context->version);
    if (err == ESP_OK) err = nvs_set_str(handle, OTA_CONTEXT_SHA_KEY,
                                         context->image_sha256);
    if (err == ESP_OK) err = nvs_set_u64(handle, OTA_CONTEXT_SEQUENCE_KEY,
                                         context->release_sequence);
    if (err == ESP_OK) err = nvs_set_u32(handle, OTA_CONTEXT_PARTITION_KEY,
                                         context->target_partition_address);
    if (err == ESP_OK) err = nvs_set_u32(handle, OTA_CONTEXT_FINGERPRINT_KEY,
                                         context->manifest_fingerprint);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t clear_report_context(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_CONTEXT_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        memset(&s_report, 0, sizeof(s_report));
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

static void load_report_context(void)
{
    ota_report_context_t context = {0};
    nvs_handle_t handle;
    if (nvs_open(OTA_CONTEXT_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t request_size = sizeof(context.request_id);
    size_t update_size = sizeof(context.update_id);
    size_t version_size = sizeof(context.version);
    size_t sha_size = sizeof(context.image_sha256);
    esp_err_t err = nvs_get_str(handle, OTA_CONTEXT_REQUEST_KEY,
                                context.request_id, &request_size);
    if (err == ESP_OK) err = nvs_get_str(handle, OTA_CONTEXT_UPDATE_KEY,
                                         context.update_id, &update_size);
    if (err == ESP_OK) err = nvs_get_str(handle, OTA_CONTEXT_VERSION_KEY,
                                         context.version, &version_size);
    if (err == ESP_OK) err = nvs_get_str(handle, OTA_CONTEXT_SHA_KEY,
                                         context.image_sha256, &sha_size);
    if (err == ESP_OK) err = nvs_get_u64(handle, OTA_CONTEXT_SEQUENCE_KEY,
                                         &context.release_sequence);
    if (err == ESP_OK) err = nvs_get_u32(handle, OTA_CONTEXT_PARTITION_KEY,
                                         &context.target_partition_address);
    if (err == ESP_OK) {
        const esp_err_t fingerprint_err = nvs_get_u32(
            handle,
            OTA_CONTEXT_FINGERPRINT_KEY,
            &context.manifest_fingerprint);
        if (fingerprint_err != ESP_OK &&
            fingerprint_err != ESP_ERR_NVS_NOT_FOUND) {
            err = fingerprint_err;
        }
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_report = context;
        portEXIT_CRITICAL(&s_lock);
    }
}

static bool report_context_present(void)
{
    bool present;
    portENTER_CRITICAL(&s_lock);
    present = s_report.update_id[0] != '\0' && s_report.version[0] != '\0' &&
              valid_sha256(s_report.image_sha256) &&
              s_report.target_partition_address != 0U;
    portEXIT_CRITICAL(&s_lock);
    return present;
}

static esp_err_t ota_core_local_event(void *ctx, const char *json_event)
{
    (void)ctx;
    (void)json_event;
    nuwax_ota_stats_t stats = {0};
    nuwax_ota_get_stats(&stats);

    bool progress_changed;
    portENTER_CRITICAL(&s_lock);
    progress_changed = stats.state != s_last_progress_state ||
                       stats.pct != s_last_progress_pct;
    if (progress_changed) {
        s_last_progress_state = stats.state;
        s_last_progress_pct = stats.pct;
    }
    portEXIT_CRITICAL(&s_lock);

    if (progress_changed) {
        nuwax_ota_device_callbacks_t callbacks;
        portENTER_CRITICAL(&s_lock);
        callbacks = s_callbacks;
        portEXIT_CRITICAL(&s_lock);
        if (callbacks.show_local_progress != NULL) {
            callbacks.show_local_progress(
                callbacks.ctx, nuwax_ota_state_name(stats.state), stats.pct,
                stats.last_reason);
        }
    }
    return ESP_OK;
}

static const char *battery_preflight_code(void)
{
    nuwax_battery_service_snapshot_t battery = {0};
    const bool snapshot_available =
        nuwax_battery_service_get_snapshot(&battery) == ESP_OK;
    const nuwax_ota_power_result_t result = nuwax_ota_power_evaluate(
        snapshot_available && battery.sample_available,
        battery.stale,
        battery.battery_present,
        battery.power_state == NUWAX_BATTERY_POWER_EXTERNAL,
        battery.valid,
        battery.low,
        battery.percent,
        CONFIG_NUWAX_OTA_MIN_BATTERY_PERCENT);
    return nuwax_ota_power_result_code(result);
}

static void release_active_update(bool clear_persisted_context)
{
    if (!nuwax_ota_main_worker_idle()) {
        ESP_LOGE(TAG, "refusing to release OTA interlocks before worker cleanup");
        return;
    }
    if (clear_persisted_context) {
        (void)clear_report_context();
    }
    nuwax_ota_device_callbacks_t callbacks;
    portENTER_CRITICAL(&s_lock);
    callbacks = s_callbacks;
    s_command_pending = false;
    s_active = false;
    s_pending_update_id[0] = '\0';
    s_active_image_sha256[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    refresh_interlocks();
    if (callbacks.show_local_progress != NULL) {
        callbacks.show_local_progress(callbacks.ctx, "idle", 0,
                                      "update inactive");
    }
}

static void release_queued_command(void)
{
    portENTER_CRITICAL(&s_lock);
    s_command_pending = false;
    s_pending_update_id[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    refresh_interlocks();
}

static bool accept_queued_update(const ota_command_item_t *item)
{
    if (!nuwax_ota_device_cloud_authenticated(item->connection_epoch)) {
        release_queued_command();
        ESP_LOGW(TAG,
                 "discarding OTA command from stale WebSocket epoch=%lu",
                 (unsigned long)item->connection_epoch);
        return false;
    }

    nuwax_ota_manifest_t manifest = {0};
    esp_err_t err = nuwax_ota_manifest_parse(item->json, strlen(item->json),
                                             &manifest);
    if (err != ESP_OK || strcmp(manifest.update_id, item->update_id) != 0 ||
        strcmp(manifest.version, item->version) != 0 ||
        strcmp(manifest.image_sha256, item->image_sha256) != 0 ||
        ota_manifest_fingerprint(&manifest) != item->manifest_fingerprint) {
        release_queued_command();
        (void)emit_ack(item->request_id, item->update_id, false, false,
                       "rejected", "bad_request", "invalid OTA manifest");
        return false;
    }

    bool pending_verify = false;
    err = nuwax_ota_main_running_image_pending_verify(&pending_verify);
    if (err != ESP_OK || pending_verify) {
        release_queued_command();
        (void)emit_ack(item->request_id, item->update_id, false, false,
                       "rejected", "rollback_pending",
                       "current image is not confirmed");
        return false;
    }

    const esp_partition_t *target_partition =
        esp_ota_get_next_update_partition(NULL);
    if (target_partition == NULL) {
        release_queued_command();
        (void)emit_ack(item->request_id, item->update_id, false, false,
                       "rejected", "wrong_partition_schema",
                       "OTA target partition missing");
        return false;
    }

    nuwax_ota_device_callbacks_t callbacks;
    portENTER_CRITICAL(&s_lock);
    callbacks = s_callbacks;
    portEXIT_CRITICAL(&s_lock);
    if (callbacks.continuous_audio_busy(callbacks.ctx)) {
        release_queued_command();
        (void)emit_ack(item->request_id, item->update_id, false, false,
                       "rejected", "audio_busy",
                       "stop meeting or monitoring mode first");
        return false;
    }
    const char *power_code = battery_preflight_code();
    if (power_code != NULL) {
        release_queued_command();
        (void)emit_ack(item->request_id, item->update_id, false, false,
                       "rejected", power_code,
                       "battery state is not safe for OTA");
        return false;
    }

    ota_report_context_t context = {0};
    strlcpy(context.request_id, item->request_id, sizeof(context.request_id));
    strlcpy(context.update_id, item->update_id, sizeof(context.update_id));
    strlcpy(context.version, manifest.version, sizeof(context.version));
    strlcpy(context.image_sha256, manifest.image_sha256,
            sizeof(context.image_sha256));
    context.release_sequence = 0U;
    context.manifest_fingerprint = item->manifest_fingerprint;
    context.target_partition_address = target_partition->address;
    err = persist_report_context(&context);
    if (err != ESP_OK) {
        release_queued_command();
        (void)emit_ack(item->request_id, item->update_id, false, false,
                       "rejected", "storage_error",
                       "cannot persist OTA context");
        return false;
    }

    bool reservation_valid;
    portENTER_CRITICAL(&s_lock);
    reservation_valid = s_command_pending && !s_active;
    if (reservation_valid) {
        s_report = context;
        strlcpy(s_active_image_sha256, manifest.image_sha256,
                sizeof(s_active_image_sha256));
        s_command_pending = false;
        s_pending_update_id[0] = '\0';
        s_active = true;
        s_audio_blocked = true;
        s_terminal_report_state = OTA_TERMINAL_REPORT_NONE;
        s_terminal_delivery_ok = false;
        s_last_progress_pct = -1;
        s_last_progress_state = NUWAX_OTA_STATE_IDLE;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!reservation_valid) {
        (void)clear_report_context();
        release_queued_command();
        (void)emit_ack(item->request_id, item->update_id, false, false,
                       "rejected", "internal_error",
                       "OTA reservation was lost");
        return false;
    }
    refresh_interlocks();

    err = emit_ack(item->request_id, item->update_id, true, false,
                   "accepted", "ok", "accepted");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA acceptance ACK failed; preserving context for reconnect replay");
        request_reconnect_after_terminal_failure(err);
        release_active_update(false);
        return false;
    }
    return true;
}

/* Keep the transport path out of the persistent controller loop's frame.
 * Cloud-ready recovery and update execution are mutually exclusive paths. */
static __attribute__((noinline)) void process_update(
    const ota_command_item_t *item)
{
    if (!accept_queued_update(item)) {
        return;
    }
    nuwax_ota_device_callbacks_t callbacks;
    portENTER_CRITICAL(&s_lock);
    callbacks = s_callbacks;
    portEXIT_CRITICAL(&s_lock);
    if (callbacks.show_local_progress != NULL) {
        callbacks.show_local_progress(callbacks.ctx, "quiescing", 0,
                                      "preparing audio");
    }
    if (callbacks.prepare_audio_for_update != NULL) {
        esp_err_t quiesce_err = callbacks.prepare_audio_for_update(callbacks.ctx);
        if (quiesce_err != ESP_OK) {
            ESP_LOGE(TAG, "audio quiesce failed: %s", esp_err_to_name(quiesce_err));
            if (begin_terminal_report()) {
                esp_err_t report_err = emit_result_reliably(
                    "failed", "audio did not become idle");
                complete_terminal_report(report_err);
                request_reconnect_after_terminal_failure(report_err);
            }
            bool delivered;
            portENTER_CRITICAL(&s_lock);
            delivered = s_terminal_delivery_ok;
            portEXIT_CRITICAL(&s_lock);
            release_active_update(delivered);
            return;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(CONFIG_NUWAX_OTA_AUDIO_QUIESCE_MS));
    if (callbacks.continuous_audio_busy(callbacks.ctx) ||
        callbacks.prepare_audio_for_update(callbacks.ctx) != ESP_OK) {
        ESP_LOGE(TAG, "audio paths did not remain quiescent through guard interval");
        if (begin_terminal_report()) {
            esp_err_t report_err = emit_result_reliably(
                "failed", "audio quiesce guard failed");
            complete_terminal_report(report_err);
            request_reconnect_after_terminal_failure(report_err);
        }
        bool delivered;
        portENTER_CRITICAL(&s_lock);
        delivered = s_terminal_delivery_ok;
        portEXIT_CRITICAL(&s_lock);
        release_active_update(delivered);
        return;
    }
    esp_err_t err = nuwax_ota_main_start(item->json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA start failed: %s", esp_err_to_name(err));
        if (begin_terminal_report()) {
            esp_err_t report_err = emit_result_reliably(
                "failed", esp_err_to_name(err));
            complete_terminal_report(report_err);
            request_reconnect_after_terminal_failure(report_err);
        }
        bool delivered;
        portENTER_CRITICAL(&s_lock);
        delivered = s_terminal_delivery_ok;
        portEXIT_CRITICAL(&s_lock);
        release_active_update(delivered);
        return;
    }
    bool transport_wdt_registered = false;
    bool transport_wdt_added_by_controller = false;
    esp_err_t wdt_err = esp_task_wdt_status(NULL);
    if (wdt_err == ESP_ERR_NOT_FOUND) {
        wdt_err = esp_task_wdt_add(NULL);
        transport_wdt_registered = wdt_err == ESP_OK;
        transport_wdt_added_by_controller = transport_wdt_registered;
    } else if (wdt_err == ESP_OK) {
        transport_wdt_registered = true;
    }
    if (!transport_wdt_registered) {
        ESP_LOGW(TAG,
                 "OTA transport watchdog registration failed: %s; bounded transport controls remain active",
                 esp_err_to_name(wdt_err));
    } else {
        (void)esp_task_wdt_reset();
    }
    log_controller_stack_watermark("transport_begin");
    err = nuwax_ota_main_run_transport();
    log_controller_stack_watermark("transport_complete");
    if (transport_wdt_added_by_controller &&
        esp_task_wdt_delete(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "OTA transport watchdog deregistration failed");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA transport returned: %s", esp_err_to_name(err));
    }

    if (!nuwax_ota_main_terminal_complete()) {
        const nuwax_ota_state_t state = nuwax_ota_state();
        ESP_LOGE(TAG, "OTA transport returned without terminal state: %s",
                 nuwax_ota_state_name(state));
        if (begin_terminal_report()) {
            esp_err_t report_err = emit_result_reliably(
                "failed", "transport_returned_without_terminal_state");
            complete_terminal_report(report_err);
            request_reconnect_after_terminal_failure(report_err);
        }
        bool delivered;
        portENTER_CRITICAL(&s_lock);
        delivered = s_terminal_delivery_ok;
        portEXIT_CRITICAL(&s_lock);
        release_active_update(delivered);
        return;
    }

    if (nuwax_ota_state() == NUWAX_OTA_STATE_DONE) {
        if (begin_terminal_report()) {
            esp_err_t report_err = emit_result_reliably(
                "staged", "verified");
            complete_terminal_report(report_err);
        }
        if (callbacks.show_local_progress != NULL) {
            callbacks.show_local_progress(callbacks.ctx, "rebooting", 100,
                                          "image verified");
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_NUWAX_OTA_REBOOT_DELAY_MS));
        err = nuwax_ota_main_reboot_staged_image();
        ESP_LOGE(TAG, "OTA staged reboot unexpectedly returned: %s",
                 esp_err_to_name(err));
        /* Keep SW3 and audio inhibited. The boot partition has already been
         * selected, so resuming ordinary operation is not a safe fallback. */
        return;
    }

    nuwax_ota_stats_t stats = {0};
    nuwax_ota_get_stats(&stats);
    if (begin_terminal_report()) {
        esp_err_t report_err = emit_result_reliably(
            "failed", stats.last_reason);
        complete_terminal_report(report_err);
        request_reconnect_after_terminal_failure(report_err);
    }
    bool delivered;
    portENTER_CRITICAL(&s_lock);
    delivered = s_terminal_delivery_ok;
    portEXIT_CRITICAL(&s_lock);
    release_active_update(delivered);
}

static bool claim_boot_timeout_decision(uint32_t now)
{
    bool claimed = false;
    portENTER_CRITICAL(&s_lock);
    if (s_boot_pending_verify &&
        s_boot_decision_owner == OTA_BOOT_DECISION_NONE &&
        deadline_reached(now, s_boot_validation_deadline_ms)) {
        s_boot_decision_owner = OTA_BOOT_DECISION_TIMEOUT;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return claimed;
}

static ota_boot_decision_owner_t claim_cloud_boot_decision(uint32_t now,
                                                            bool *out_claimed)
{
    ota_boot_decision_owner_t owner = OTA_BOOT_DECISION_COMPLETE;
    *out_claimed = false;
    portENTER_CRITICAL(&s_lock);
    if (s_boot_pending_verify &&
        s_boot_decision_owner == OTA_BOOT_DECISION_NONE) {
        owner = deadline_reached(now, s_boot_validation_deadline_ms)
                    ? OTA_BOOT_DECISION_TIMEOUT
                    : OTA_BOOT_DECISION_CLOUD_READY;
        s_boot_decision_owner = owner;
        *out_claimed = true;
    } else if (s_boot_pending_verify) {
        owner = s_boot_decision_owner;
    }
    portEXIT_CRITICAL(&s_lock);
    return owner;
}

static esp_err_t execute_boot_rollback(ota_boot_decision_owner_t owner,
                                       const char *code,
                                       const char *detail)
{
    ESP_LOGE(TAG, "new OTA image rejected by boot health gate code=%s detail=%s",
             code, detail);
    /* Do not publish a terminal result before rollback has actually happened.
     * The previous image retains the report context and publishes the
     * canonical rolled_back result after reboot. */
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_err_t err = nuwax_ota_main_rollback_running_image_and_reboot();
    ESP_LOGE(TAG, "OTA rollback call returned: %s; retrying",
             esp_err_to_name(err));
    portENTER_CRITICAL(&s_lock);
    if (s_boot_pending_verify &&
        s_boot_decision_owner == owner) {
        s_boot_decision_owner = OTA_BOOT_DECISION_NONE;
        s_boot_validation_deadline_ms = monotonic_ms() + 5000U;
        if (s_cloud_authenticated) {
            s_cloud_ready_event_pending = true;
            s_cloud_ready_event_ms = monotonic_ms();
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return err;
}

static esp_err_t execute_timeout_rollback(void)
{
    return execute_boot_rollback(OTA_BOOT_DECISION_TIMEOUT,
                                 "validation_timeout",
                                 "cloud and local health gates did not pass before deadline");
}

static void ota_controller_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "controller started stack=%u command_item=%u bytes",
             (unsigned)OTA_CONTROLLER_STACK_BYTES,
             (unsigned)sizeof(ota_command_item_t));
    for (;;) {
        ota_command_item_t *item = NULL;
        if (xQueueReceive(s_commands, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (item != NULL) {
                process_update(item);
                free_command_item(item);
            }
            log_controller_stack_watermark("command_complete");
        }

        uint32_t notifications = 0;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &notifications, 0);
        bool inspect_cloud_ready =
            (notifications & OTA_NOTIFY_CLOUD_READY) != 0U;
        portENTER_CRITICAL(&s_lock);
        inspect_cloud_ready = inspect_cloud_ready || s_cloud_ready_event_pending;
        portEXIT_CRITICAL(&s_lock);
        if (inspect_cloud_ready) {
            uint32_t ready_ms = 0;
            bool event_pending;
            portENTER_CRITICAL(&s_lock);
            event_pending = s_cloud_ready_event_pending;
            if (event_pending) {
                ready_ms = s_cloud_ready_event_ms;
                s_cloud_ready_event_pending = false;
            }
            portEXIT_CRITICAL(&s_lock);
            if (event_pending) {
                esp_err_t ready_err = process_cloud_ready_event(ready_ms);
                if (ready_err != ESP_OK &&
                    ready_err != ESP_ERR_INVALID_STATE &&
                    ready_err != ESP_ERR_NOT_FINISHED) {
                    ESP_LOGE(TAG, "deferred cloud-ready handling failed: %s",
                             esp_err_to_name(ready_err));
                }
                log_controller_stack_watermark("cloud_ready_complete");
            }
        }

        const uint32_t now = monotonic_ms();
        if (claim_boot_timeout_decision(now)) {
            (void)execute_timeout_rollback();
        }
    }
}

static void rollback_device_init_interlocks(void)
{
    portENTER_CRITICAL(&s_lock);
    s_boot_pending_verify = false;
    s_boot_decision_owner = OTA_BOOT_DECISION_NONE;
    s_boot_validation_deadline_ms = 0U;
    s_cloud_ready_event_pending = false;
    s_cloud_ready_event_ms = 0U;
    s_health_pending_log_valid = false;
    s_health_pending_log_ms = 0U;
    s_health_pending_log_detail[0] = '\0';
    (void)apply_interlocks_locked();
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t nuwax_ota_device_init(const nuwax_ota_device_callbacks_t *callbacks)
{
    if (callbacks == NULL || callbacks->send_json == NULL ||
        callbacks->request_cloud_reconnect == NULL ||
        callbacks->continuous_audio_busy == NULL ||
        callbacks->prepare_audio_for_update == NULL ||
        callbacks->show_local_progress == NULL ||
        callbacks->check_boot_health == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_OK;
    }

    s_commands = xQueueCreate(1, sizeof(ota_command_item_t *));
    if (s_commands == NULL) {
        rollback_device_init_interlocks();
        return ESP_ERR_NO_MEM;
    }
    s_callbacks = *callbacks;
    const nuwax_ota_main_callbacks_t core_callbacks = {
        .ctx = NULL,
        .post_local_event = ota_core_local_event,
    };
    esp_err_t err = nuwax_ota_main_register(&core_callbacks);
    if (err == ESP_OK) err = nuwax_ota_main_init();
    if (err != ESP_OK) {
        rollback_device_init_interlocks();
        vQueueDelete(s_commands);
        s_commands = NULL;
        return err;
    }

    load_report_context();
    bool pending = false;
    err = nuwax_ota_main_running_image_pending_verify(&pending);
    if (err != ESP_OK) {
        rollback_device_init_interlocks();
        vQueueDelete(s_commands);
        s_commands = NULL;
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    s_boot_pending_verify = pending;
    s_boot_decision_owner = OTA_BOOT_DECISION_NONE;
    s_health_pending_log_valid = false;
    s_health_pending_log_ms = 0;
    s_health_pending_log_detail[0] = '\0';
    s_boot_validation_deadline_ms =
        monotonic_ms() + CONFIG_NUWAX_OTA_BOOT_VALIDATION_TIMEOUT_MS;
    const bool interlock_applied = apply_interlocks_locked();
    portEXIT_CRITICAL(&s_lock);
    if (pending && !interlock_applied) {
        ESP_LOGE(TAG, "pending-verify boot lost race with SW3 shutdown");
        rollback_device_init_interlocks();
        vQueueDelete(s_commands);
        s_commands = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t created = xTaskCreate(ota_controller_task, "ota_controller",
                                     OTA_CONTROLLER_STACK_BYTES, NULL,
                                     OTA_CONTROLLER_PRIORITY,
                                     &s_controller_task);
    if (created != pdPASS) {
        rollback_device_init_interlocks();
        vQueueDelete(s_commands);
        s_commands = NULL;
        s_controller_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "ready pending_verify=%d", pending ? 1 : 0);
    return ESP_OK;
}

bool nuwax_ota_device_handle_text(const char *payload, int len,
                                  uint32_t connection_epoch)
{
    if (payload == NULL || len <= 0) {
        return false;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(payload, (size_t)len,
                                            &parse_end, false);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    const bool is_result_ack =
        cJSON_IsString(type) &&
        strcmp(type->valuestring, "ota_result_ack") == 0;
    if (is_result_ack) {
        const bool handled = handle_result_ack(root, parse_end, payload, len,
                                               connection_epoch);
        cJSON_Delete(root);
        return handled;
    }
    const bool is_device_command =
        cJSON_IsString(type) && cJSON_IsString(command) &&
        strcmp(type->valuestring, "device_command") == 0;
    const bool is_ota_update =
        is_device_command && strcmp(command->valuestring, "ota_update") == 0;
    if (!is_ota_update) {
        cJSON_Delete(root);
        return false;
    }

    char request_id[OTA_REQUEST_ID_MAX_BYTES] = {0};
    char update_id[NUWAX_OTA_MANIFEST_ID_MAX_LEN] = {0};
    const cJSON *update = cJSON_GetObjectItemCaseSensitive(root, "update_id");
    const bool update_ok = cJSON_IsString(update) &&
                           update->valuestring != NULL &&
                           valid_identifier(update->valuestring,
                                            sizeof(update_id));
    while (parse_end != NULL && parse_end < payload + len &&
           isspace((unsigned char)*parse_end)) {
        parse_end++;
    }
    const bool json_complete = parse_end == payload + len;
    if (update_ok) {
        strlcpy(update_id, update->valuestring, sizeof(update_id));
        strlcpy(request_id, update->valuestring, sizeof(request_id));
    }
    cJSON_Delete(root);

    if (!s_initialized) {
        ESP_LOGE(TAG, "OTA command received before controller init");
        return true;
    }
    if (!update_ok || !json_complete ||
        (size_t)len >= OTA_COMMAND_JSON_MAX_BYTES) {
        ESP_LOGW(TAG, "invalid OTA command envelope");
        if (update_ok) {
            (void)emit_ack(request_id, update_id, false, false, "rejected",
                           "bad_request", "invalid OTA envelope");
        }
        return true;
    }
    if (!nuwax_ota_device_cloud_authenticated(connection_epoch)) {
        (void)emit_ack(request_id, update_id, false, false, "rejected",
                       "auth_required",
                       "current authenticated connection is required before OTA");
        return true;
    }

    nuwax_ota_manifest_t manifest = {0};
    esp_err_t err = nuwax_ota_manifest_parse(payload, (size_t)len, &manifest);
    if (err != ESP_OK || strcmp(manifest.update_id, update_id) != 0) {
        (void)emit_ack(request_id, update_id, false, false, "rejected",
                       "bad_request", "invalid OTA manifest");
        return true;
    }
    bool active;
    bool command_pending;
    const bool terminal_report_pending = report_context_present();
    bool duplicate = false;
    bool conflict = false;
    portENTER_CRITICAL(&s_lock);
    active = s_active;
    command_pending = s_command_pending;
    if (active && strcmp(s_report.update_id, manifest.update_id) == 0) {
        duplicate = s_report.manifest_fingerprint != 0U &&
                    s_report.manifest_fingerprint ==
                        ota_manifest_fingerprint(&manifest);
        conflict = !duplicate;
    }
    portEXIT_CRITICAL(&s_lock);
    if (duplicate) {
        (void)emit_ack(request_id, update_id, true, true, "in_progress",
                       "ok", "already accepted");
        return true;
    }
    if (active || command_pending || terminal_report_pending) {
        (void)emit_ack(request_id, update_id, false, false, "rejected",
                       conflict ? "update_id_conflict" : "ota_busy",
                       conflict ? "update_id already has another image" :
                       terminal_report_pending ?
                                   "previous OTA result is awaiting cloud acknowledgement" :
                                   "another update is active");
        return true;
    }

    nuwax_ota_device_callbacks_t callbacks;
    portENTER_CRITICAL(&s_lock);
    callbacks = s_callbacks;
    portEXIT_CRITICAL(&s_lock);
    if (callbacks.continuous_audio_busy != NULL &&
        callbacks.continuous_audio_busy(callbacks.ctx)) {
        (void)emit_ack(request_id, update_id, false, false, "rejected",
                       "audio_busy", "stop meeting or monitoring mode first");
        return true;
    }
    const char *power_code = battery_preflight_code();
    if (power_code != NULL) {
        (void)emit_ack(request_id, update_id, false, false, "rejected",
                       power_code, "battery state is not safe for OTA");
        return true;
    }

    ota_command_item_t *item = allocate_command_item();
    if (item == NULL) {
        (void)emit_ack(request_id, update_id, false, false, "rejected",
                       "internal_error", "cannot allocate OTA command");
        return true;
    }
    memcpy(item->json, payload, (size_t)len);
    item->json[len] = '\0';
    strlcpy(item->request_id, request_id, sizeof(item->request_id));
    strlcpy(item->update_id, update_id, sizeof(item->update_id));
    strlcpy(item->version, manifest.version, sizeof(item->version));
    strlcpy(item->image_sha256, manifest.image_sha256,
            sizeof(item->image_sha256));
    item->manifest_fingerprint = ota_manifest_fingerprint(&manifest);
    item->connection_epoch = connection_epoch;

    bool reserved = false;
    bool authenticated_now = false;
    bool shutdown_conflict = false;
    portENTER_CRITICAL(&s_lock);
    if (cloud_authenticated_for_epoch_locked(connection_epoch) &&
        !s_active && !s_command_pending) {
        s_command_pending = true;
        strlcpy(s_pending_update_id, update_id, sizeof(s_pending_update_id));
        if (apply_interlocks_locked()) {
            reserved = true;
        } else {
            s_command_pending = false;
            s_pending_update_id[0] = '\0';
            s_audio_blocked = s_boot_pending_verify;
            shutdown_conflict = true;
        }
    }
    authenticated_now =
        cloud_authenticated_for_epoch_locked(connection_epoch);
    portEXIT_CRITICAL(&s_lock);
    if (!reserved) {
        free_command_item(item);
        (void)emit_ack(request_id, update_id, false, false, "rejected",
                       authenticated_now ? "ota_busy" : "auth_required",
                       shutdown_conflict ? "device shutdown already started" :
                       authenticated_now ? "another update is active" :
                                            "positive cloud activity is required before OTA");
        return true;
    }

    if (xQueueSend(s_commands, &item, 0) != pdTRUE) {
        free_command_item(item);
        release_queued_command();
        (void)emit_ack(request_id, update_id, false, false, "rejected",
                       "internal_error", "cannot queue OTA controller");
    }
    return true;
}

bool nuwax_ota_device_blocks_audio(void)
{
    bool blocked;
    portENTER_CRITICAL(&s_lock);
    blocked = s_audio_blocked;
    portEXIT_CRITICAL(&s_lock);
    return blocked;
}

bool nuwax_ota_device_is_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_lock);
    active = s_command_pending || s_active || s_boot_pending_verify;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

bool nuwax_ota_device_worker_idle(void)
{
    return nuwax_ota_main_worker_idle();
}

bool nuwax_ota_device_terminal_complete(void)
{
    return nuwax_ota_main_terminal_complete();
}

void nuwax_ota_device_set_cloud_authenticated(bool authenticated,
                                               uint32_t connection_epoch)
{
    portENTER_CRITICAL(&s_lock);
    s_cloud_authenticated = authenticated && connection_epoch != 0U;
    s_cloud_authenticated_epoch =
        s_cloud_authenticated ? connection_epoch : 0U;
    portEXIT_CRITICAL(&s_lock);
}

bool nuwax_ota_device_cloud_authenticated(uint32_t connection_epoch)
{
    bool authenticated;
    portENTER_CRITICAL(&s_lock);
    authenticated = cloud_authenticated_for_epoch_locked(connection_epoch);
    portEXIT_CRITICAL(&s_lock);
    return authenticated;
}

esp_err_t nuwax_ota_device_cloud_ready(uint32_t connection_epoch)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t controller;
    uint32_t event_ms;
    portENTER_CRITICAL(&s_lock);
    if (!cloud_authenticated_for_epoch_locked(connection_epoch)) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    controller = s_controller_task;
    if (!s_cloud_ready_event_pending) {
        s_cloud_ready_event_pending = true;
        s_cloud_ready_event_ms = monotonic_ms();
    }
    event_ms = s_cloud_ready_event_ms;
    portEXIT_CRITICAL(&s_lock);

    if (controller == NULL) {
        portENTER_CRITICAL(&s_lock);
        if (s_cloud_ready_event_pending &&
            s_cloud_ready_event_ms == event_ms) {
            s_cloud_ready_event_pending = false;
        }
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskNotify(controller, OTA_NOTIFY_CLOUD_READY, eSetBits) == pdPASS) {
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_cloud_ready_event_pending && s_cloud_ready_event_ms == event_ms) {
        s_cloud_ready_event_pending = false;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_FAIL;
}

static esp_err_t process_cloud_ready_event(uint32_t ready_ms)
{
    bool pending = false;
    esp_err_t err = nuwax_ota_main_running_image_pending_verify(&pending);
    if (err != ESP_OK) {
        return err;
    }
    if (pending) {
        nuwax_ota_device_callbacks_t callbacks;
        portENTER_CRITICAL(&s_lock);
        callbacks = s_callbacks;
        portEXIT_CRITICAL(&s_lock);

        char health_detail[128] = "local health unavailable";
        esp_err_t health_err = callbacks.check_boot_health(
            callbacks.ctx, health_detail, sizeof(health_detail));
        const uint32_t now = monotonic_ms();
        if (health_err == ESP_ERR_NOT_FINISHED &&
            !deadline_reached(now, s_boot_validation_deadline_ms)) {
            portENTER_CRITICAL(&s_lock);
            if (s_boot_pending_verify &&
                s_boot_decision_owner == OTA_BOOT_DECISION_NONE) {
                s_cloud_ready_event_pending = true;
                s_cloud_ready_event_ms = ready_ms;
            }
            portEXIT_CRITICAL(&s_lock);
            if (should_log_health_pending(now, health_detail)) {
                ESP_LOGI(TAG, "local boot health pending: %s", health_detail);
            }
            return ESP_ERR_NOT_FINISHED;
        }
        reset_health_pending_log();
        if (health_err != ESP_OK) {
            ota_boot_decision_owner_t owner = OTA_BOOT_DECISION_COMPLETE;
            bool claimed = false;
            portENTER_CRITICAL(&s_lock);
            if (s_boot_pending_verify &&
                s_boot_decision_owner == OTA_BOOT_DECISION_NONE) {
                owner = deadline_reached(now, s_boot_validation_deadline_ms)
                            ? OTA_BOOT_DECISION_TIMEOUT
                            : OTA_BOOT_DECISION_LOCAL_HEALTH_FAILED;
                s_boot_decision_owner = owner;
                claimed = true;
            }
            portEXIT_CRITICAL(&s_lock);
            if (!claimed) {
                return ESP_ERR_INVALID_STATE;
            }
            if (owner == OTA_BOOT_DECISION_TIMEOUT) {
                return execute_timeout_rollback();
            }
            return execute_boot_rollback(
                OTA_BOOT_DECISION_LOCAL_HEALTH_FAILED,
                "local_health_failed", health_detail);
        }

        bool decision_claimed = false;
        ota_boot_decision_owner_t owner =
            claim_cloud_boot_decision(now, &decision_claimed);
        if (owner == OTA_BOOT_DECISION_TIMEOUT && decision_claimed) {
            return execute_timeout_rollback();
        }
        if (owner != OTA_BOOT_DECISION_CLOUD_READY || !decision_claimed) {
            return ESP_ERR_INVALID_STATE;
        }

        const ota_report_context_t context = report_context_snapshot();
        bool identity_matches = false;
        err = nuwax_ota_main_pending_identity_matches(
            context.target_partition_address, context.image_sha256,
            &identity_matches);
        if (err != ESP_OK || !identity_matches) {
            portENTER_CRITICAL(&s_lock);
            if (s_boot_decision_owner == OTA_BOOT_DECISION_CLOUD_READY) {
                s_boot_decision_owner = OTA_BOOT_DECISION_NONE;
            }
            portEXIT_CRITICAL(&s_lock);
            return err == ESP_OK ? ESP_ERR_INVALID_CRC : err;
        }

        /* The old image may have rebooted after a successful local write but
         * before the gateway durably observed staged. Replaying staged is
         * idempotent and its persistence ACK must precede image confirmation. */
        if (!begin_terminal_report()) {
            portENTER_CRITICAL(&s_lock);
            if (s_boot_decision_owner == OTA_BOOT_DECISION_CLOUD_READY) {
                s_boot_decision_owner = OTA_BOOT_DECISION_NONE;
            }
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        err = emit_result_reliably("staged",
                                   "running image pending validation");
        if (err != ESP_OK) {
            complete_terminal_report(err);
            portENTER_CRITICAL(&s_lock);
            if (s_boot_decision_owner == OTA_BOOT_DECISION_CLOUD_READY) {
                s_boot_decision_owner = OTA_BOOT_DECISION_NONE;
            }
            portEXIT_CRITICAL(&s_lock);
            request_reconnect_after_terminal_failure(err);
            return err;
        }
        err = nuwax_ota_main_confirm_running_image();
        if (err != ESP_OK) {
            complete_terminal_report(err);
            bool still_pending = true;
            (void)nuwax_ota_main_running_image_pending_verify(&still_pending);
            portENTER_CRITICAL(&s_lock);
            if (!still_pending) {
                s_boot_pending_verify = false;
                s_boot_decision_owner = OTA_BOOT_DECISION_COMPLETE;
            } else if (s_boot_decision_owner == OTA_BOOT_DECISION_CLOUD_READY) {
                s_boot_decision_owner = OTA_BOOT_DECISION_NONE;
            }
            portEXIT_CRITICAL(&s_lock);
            refresh_interlocks();
            request_reconnect_after_terminal_failure(err);
            return err;
        }
        err = emit_result_reliably("committed", "health gate passed");
        complete_terminal_report(err);
        if (err == ESP_OK) (void)clear_report_context();
        request_reconnect_after_terminal_failure(err);
        portENTER_CRITICAL(&s_lock);
        s_boot_pending_verify = false;
        s_boot_decision_owner = OTA_BOOT_DECISION_COMPLETE;
        portEXIT_CRITICAL(&s_lock);
        refresh_interlocks();
        if (err == ESP_OK) {
            /* The connection hello was built while this image was still
             * pending verification. Reconnect once after the durable commit
             * ACK so the next hello reports ota_state=valid. */
            request_firmware_metadata_refresh();
        }
        return err;
    }

    if (nuwax_ota_device_is_active()) {
        ESP_LOGI(TAG, "positive cloud activity during OTA reconnect; keep active context");
        return ESP_OK;
    }

    if (!report_context_present()) {
        return ESP_OK;
    }
    if (!begin_terminal_report()) {
        return ESP_OK;
    }
    const ota_report_context_t context = report_context_snapshot();
    const esp_app_desc_t *running = esp_app_get_description();
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    bool accepted_identity_matches = false;
    esp_err_t identity_err = nuwax_ota_main_accepted_identity_matches(
        context.target_partition_address, context.image_sha256,
        &accepted_identity_matches);
    const bool running_context_matches =
        running != NULL && running_partition != NULL &&
        running_partition->address == context.target_partition_address &&
        strcmp(running->version, context.version) == 0;
    if (running_context_matches &&
        identity_err != ESP_OK) {
        err = identity_err;
        complete_terminal_report(err);
        request_reconnect_after_terminal_failure(err);
        return err;
    }
    if (running != NULL && running_partition != NULL &&
        accepted_identity_matches &&
        running_partition->address == context.target_partition_address &&
        strcmp(running->version, context.version) == 0 &&
        valid_sha256(context.image_sha256)) {
        err = emit_result_reliably("committed", "already confirmed");
    } else {
        bool rolled_back = false;
        const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
        if (invalid != NULL) {
            esp_app_desc_t invalid_app = {0};
            rolled_back = esp_ota_get_partition_description(invalid,
                                                             &invalid_app) == ESP_OK &&
                          invalid->address == context.target_partition_address &&
                          strcmp(invalid_app.version, context.version) == 0 &&
                          valid_sha256(context.image_sha256);
        }
        err = emit_result_reliably(
            rolled_back ? "rolled_back" : "failed",
            rolled_back ? "new image rolled back" :
                          "update interrupted before activation");
    }
    complete_terminal_report(err);
    if (err == ESP_OK) (void)clear_report_context();
    request_reconnect_after_terminal_failure(err);
    return err;
}
