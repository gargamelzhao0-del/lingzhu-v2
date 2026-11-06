/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "nuwax_ota_transport.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "device_error_report.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nuwax_ota.h"
#include "nuwax_ota_release_policy.h"
#include "sdkconfig.h"

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "nuwax OTA requires CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y"
#endif

static const char *TAG = "nuwax_ota_transport";

#define NUWAX_OTA_IO_BUFFER_FALLBACK_BYTES 2048U
#define NUWAX_OTA_HTTP_TIMEOUT_MS 15000
#define NUWAX_OTA_STALL_TIMEOUT_MS 30000
#define NUWAX_OTA_DOWNLOAD_ATTEMPTS 3U
#define NUWAX_OTA_RETRY_BASE_MS 1000U
#define NUWAX_OTA_RESOURCE_LOG_BYTES (64U * 1024U)

typedef struct {
    int system_errno;
    int socket_errno;
    esp_err_t tls_error;
    int tls_code;
    int tls_flags;
    size_t internal_free;
    size_t internal_largest;
    size_t dma_free;
    size_t dma_largest;
    size_t psram_free;
    size_t psram_largest;
} nuwax_ota_http_error_t;

typedef struct {
    wifi_ps_type_t previous;
    bool changed;
} nuwax_ota_wifi_ps_guard_t;

static void disable_wifi_power_save(nuwax_ota_wifi_ps_guard_t *guard)
{
    if (guard == NULL) {
        return;
    }
    memset(guard, 0, sizeof(*guard));

    esp_err_t err = esp_wifi_get_ps(&guard->previous);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot query Wi-Fi power save: %s",
                 esp_err_to_name(err));
        return;
    }
    if (guard->previous == WIFI_PS_NONE) {
        ESP_LOGI(TAG, "Wi-Fi power save already disabled for OTA");
        return;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot disable Wi-Fi power save for OTA: %s",
                 esp_err_to_name(err));
        return;
    }
    guard->changed = true;
    ESP_LOGI(TAG, "Wi-Fi power save disabled for OTA previous=%d",
             (int)guard->previous);
}

static void restore_wifi_power_save(nuwax_ota_wifi_ps_guard_t *guard)
{
    if (guard == NULL || !guard->changed) {
        return;
    }

    const esp_err_t err = esp_wifi_set_ps(guard->previous);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot restore Wi-Fi power save after OTA: %s",
                 esp_err_to_name(err));
        return;
    }
    guard->changed = false;
    ESP_LOGI(TAG, "Wi-Fi power save restored mode=%d", (int)guard->previous);
}

static void log_ota_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "heap stage=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static nuwax_ota_http_error_t capture_http_error(
    esp_http_client_handle_t client,
    int system_errno,
    const char *stage,
    uint32_t received)
{
    nuwax_ota_http_error_t diagnostic = {
        .system_errno = system_errno,
        .socket_errno = 0,
        .tls_error = ESP_OK,
        .tls_code = 0,
        .tls_flags = 0,
        .internal_free = 0,
        .internal_largest = 0,
        .dma_free = 0,
        .dma_largest = 0,
        .psram_free = 0,
        .psram_largest = 0,
    };
    if (client != NULL) {
        /* Both getters clear their captured error. Read them once, on the
         * controller task, before close/cleanup destroys the evidence. */
        diagnostic.socket_errno = esp_http_client_get_errno(client);
        diagnostic.tls_error =
            esp_http_client_get_and_clear_last_tls_error(
                client, &diagnostic.tls_code, &diagnostic.tls_flags);
    }
    diagnostic.internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    diagnostic.internal_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    diagnostic.dma_free = heap_caps_get_free_size(
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    diagnostic.dma_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    diagnostic.psram_free = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    diagnostic.psram_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGE(TAG,
             "HTTP failure stage=%s received=%lu system_errno=%d(%s) "
             "socket_errno=%d(%s) tls_error=0x%x(%s) tls_code=0x%x "
             "tls_flags=0x%x stack_min_free=%u",
             stage,
             (unsigned long)received,
             diagnostic.system_errno,
             diagnostic.system_errno != 0
                 ? strerror(diagnostic.system_errno) : "none",
             diagnostic.socket_errno,
             diagnostic.socket_errno != 0
                 ? strerror(diagnostic.socket_errno) : "none",
             (unsigned)diagnostic.tls_error,
             esp_err_to_name(diagnostic.tls_error),
             (unsigned)diagnostic.tls_code,
             (unsigned)diagnostic.tls_flags,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    log_ota_heap(stage);
    return diagnostic;
}

static void format_http_failure(char *destination,
                                size_t destination_size,
                                const char *base_reason,
                                uint32_t received,
                                const nuwax_ota_http_error_t *diagnostic)
{
    snprintf(destination, destination_size,
             "%s:r=%lx,e=%x,c=%x,f=%x,df=%x,dl=%x,if=%x,il=%x,s=%x,n=%x,pf=%x,pl=%x",
             base_reason,
             (unsigned long)received,
             (unsigned)diagnostic->tls_error,
             (unsigned)diagnostic->tls_code,
             (unsigned)diagnostic->tls_flags,
             (unsigned)diagnostic->dma_free,
             (unsigned)diagnostic->dma_largest,
             (unsigned)diagnostic->internal_free,
             (unsigned)diagnostic->internal_largest,
             (unsigned)diagnostic->system_errno,
             (unsigned)diagnostic->socket_errno,
             (unsigned)diagnostic->psram_free,
             (unsigned)diagnostic->psram_largest);
}

static bool errno_is_transient(int value)
{
    return value == EAGAIN || value == EWOULDBLOCK || value == EINTR;
}

static bool http_error_is_transient(
    const nuwax_ota_http_error_t *diagnostic)
{
    return diagnostic->tls_error == ESP_OK && diagnostic->tls_code == 0 &&
           (errno_is_transient(diagnostic->system_errno) ||
            errno_is_transient(diagnostic->socket_errno));
}

static bool elapsed_at_least(int64_t since_us, uint32_t timeout_ms)
{
    return (uint64_t)(esp_timer_get_time() - since_us) >=
           (uint64_t)timeout_ms * 1000ULL;
}

static esp_err_t check_transport_control(int64_t started_us,
                                         const char **failure_reason)
{
    const esp_err_t wdt_err = esp_task_wdt_reset();
    if (wdt_err != ESP_OK && wdt_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG,
                 "OTA transport watchdog checkpoint failed: %s",
                 esp_err_to_name(wdt_err));
    }
    if (nuwax_ota_cancel_requested()) {
        *failure_reason = "cancelled";
        return ESP_ERR_INVALID_STATE;
    }

    (void)nuwax_ota_tick((uint32_t)(esp_timer_get_time() / 1000ULL));
    if (elapsed_at_least(started_us,
                         (uint32_t)CONFIG_NUWAX_OTA_TOTAL_TIMEOUT_MS) ||
        nuwax_ota_state() == NUWAX_OTA_STATE_FAILED) {
        nuwax_ota_fail("total_timeout");
        *failure_reason = "ota_total_timeout";
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t nuwax_ota_transport_run(const nuwax_ota_manifest_t *manifest)
{
    esp_err_t err = nuwax_ota_manifest_validate(manifest);
    if (err != ESP_OK) {
        nuwax_ota_fail("manifest_invalid");
        return err;
    }
    if (nuwax_ota_state() != NUWAX_OTA_STATE_DOWNLOADING) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_app_desc_t *running_app = esp_app_get_description();
    if (running_app == NULL ||
        strcmp(NUWAX_OTA_TARGET_CHIP, CONFIG_IDF_TARGET) != 0) {
        ESP_LOGE(TAG, "local OTA target configuration is invalid chip=%s idf_target=%s",
                 NUWAX_OTA_TARGET_CHIP, CONFIG_IDF_TARGET);
        nuwax_ota_fail("hardware_target_mismatch");
        return ESP_ERR_INVALID_VERSION;
    }
    if (nuwax_ota_manifest_version_compare(manifest->version,
                                           running_app->version) <= 0) {
        ESP_LOGE(TAG, "version %s is not newer than running %s",
                 manifest->version, running_app->version);
        nuwax_ota_fail("version_not_newer");
        return ESP_ERR_INVALID_VERSION;
    }

    bool pending_verify = false;
    err = nuwax_ota_running_image_pending_verify(&pending_verify);
    if (err != ESP_OK) {
        nuwax_ota_fail("running_image_state_failed");
        return err;
    }
    if (pending_verify) {
        nuwax_ota_fail("running_image_pending_verify");
        return ESP_ERR_OTA_ROLLBACK_INVALID_STATE;
    }

    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *ota_data = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (esp_ota_get_app_partition_count() != 2 || ota_data == NULL) {
        nuwax_ota_fail("partition_schema_mismatch");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (update_partition == NULL) {
        nuwax_ota_fail("ota_partition_not_found");
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_partition == NULL || update_partition->address == running_partition->address) {
        nuwax_ota_fail("update_targets_running_partition");
        return ESP_ERR_INVALID_STATE;
    }
    if (manifest->size_bytes > update_partition->size) {
        nuwax_ota_fail("image_exceeds_partition");
        return ESP_ERR_INVALID_SIZE;
    }

    nuwax_ota_set_total_bytes(manifest->size_bytes);
    nuwax_ota_set_target_version(manifest->version);
    nuwax_ota_set_target_sha256(manifest->image_sha256);

    ESP_LOGI(TAG,
             "begin version=%s bytes=%lu partition=%s offset=0x%08lx",
             manifest->version, (unsigned long)manifest->size_bytes,
             update_partition->label, (unsigned long)update_partition->address);
    log_ota_heap("before_https");

    esp_http_client_config_t http_config = {
        .url = manifest->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = NUWAX_OTA_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .buffer_size = CONFIG_NUWAX_OTA_IO_BUFFER_BYTES,
        .buffer_size_tx = 512,
        .skip_cert_common_name_check = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = NULL;
    bool http_open = false;
    esp_ota_handle_t ota_handle = 0;
    bool ota_active = false;
    nuwax_ota_sha256_t sha = {0};
    uint8_t *buffer = NULL;
    const char *failure_reason = "ota_failed";
    char failure_detail[192] = {0};
    uint32_t received = 0;
    unsigned download_attempt = 0U;
    bool entered_writing = false;
    bool retry_download = false;
    size_t io_buffer_bytes = CONFIG_NUWAX_OTA_IO_BUFFER_BYTES;
    nuwax_ota_wifi_ps_guard_t wifi_ps_guard = {0};
    const int64_t transport_started_us = esp_timer_get_time();

    disable_wifi_power_save(&wifi_ps_guard);

download_attempt_start:
    ++download_attempt;
    client = NULL;
    http_open = false;
    ota_handle = 0;
    ota_active = false;
    memset(&sha, 0, sizeof(sha));
    buffer = NULL;
    received = 0U;
    retry_download = false;
    failure_reason = "ota_failed";
    failure_detail[0] = '\0';
    if (entered_writing) {
        nuwax_ota_set_downloaded_bytes(0U);
    }
    ESP_LOGI(TAG, "download attempt %u/%u", download_attempt,
             NUWAX_OTA_DOWNLOAD_ATTEMPTS);

    err = check_transport_control(transport_started_us, &failure_reason);
    if (err != ESP_OK) {
        goto cleanup;
    }

    /* Reserve a small internal-DRAM source buffer before HTTP/TLS consumes
     * the fragmented internal heap. A generic malloc() may silently fall
     * back to PSRAM; esp_ota_write() then has to copy and flash in 32-byte
     * fragments while caches are repeatedly suspended. */
    buffer = heap_caps_malloc(io_buffer_bytes,
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if ((buffer == NULL || !esp_ptr_in_dram(buffer)) &&
        io_buffer_bytes > NUWAX_OTA_IO_BUFFER_FALLBACK_BYTES) {
        free(buffer);
        buffer = NULL;
        io_buffer_bytes = NUWAX_OTA_IO_BUFFER_FALLBACK_BYTES;
        ESP_LOGW(TAG, "falling back to %u-byte internal OTA buffer",
                 (unsigned)io_buffer_bytes);
        buffer = heap_caps_malloc(io_buffer_bytes,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buffer == NULL || !esp_ptr_in_dram(buffer)) {
        free(buffer);
        buffer = NULL;
        err = ESP_ERR_NO_MEM;
        failure_reason = "internal_ota_buffer_unavailable";
        goto cleanup;
    }
    http_config.buffer_size = (int)io_buffer_bytes;
    ESP_LOGI(TAG, "OTA buffer=%p bytes=%u internal_dram=true",
              buffer, (unsigned)io_buffer_bytes);

    client = esp_http_client_init(&http_config);
    if (client == NULL) {
        err = ESP_ERR_NO_MEM;
        failure_reason = "http_init_failed";
        goto cleanup;
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        const int system_errno = errno;
        const nuwax_ota_http_error_t diagnostic = capture_http_error(
            client, system_errno, "https_open_failed", received);
        format_http_failure(failure_detail, sizeof(failure_detail),
                            "https_open_failed", received, &diagnostic);
        failure_reason = failure_detail;
        retry_download = true;
        goto cleanup;
    }
    http_open = true;
    log_ota_heap("https_open");
    err = check_transport_control(transport_started_us, &failure_reason);
    if (err != ESP_OK) {
        goto cleanup;
    }

    int64_t content_length;
    const int64_t headers_started_us = esp_timer_get_time();
    do {
        content_length = esp_http_client_fetch_headers(client);
        if (content_length != -ESP_ERR_HTTP_EAGAIN) {
            break;
        }
        err = check_transport_control(transport_started_us, &failure_reason);
        if (err != ESP_OK) {
            goto cleanup;
        }
        if (elapsed_at_least(headers_started_us,
                             NUWAX_OTA_STALL_TIMEOUT_MS)) {
            err = ESP_ERR_TIMEOUT;
            failure_reason = "http_headers_stalled";
            retry_download = true;
            goto cleanup;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (true);
    if (content_length < 0) {
        const int system_errno = errno;
        const nuwax_ota_http_error_t diagnostic = capture_http_error(
            client, system_errno, "http_headers_failed", received);
        format_http_failure(failure_detail, sizeof(failure_detail),
                            "http_headers_failed", received, &diagnostic);
        err = ESP_ERR_INVALID_RESPONSE;
        failure_reason = failure_detail;
        retry_download = true;
        goto cleanup;
    }
    int http_status = esp_http_client_get_status_code(client);
    if (http_status != 200) {
        ESP_LOGE(TAG, "unexpected HTTP status %d", http_status);
        err = ESP_ERR_INVALID_RESPONSE;
        failure_reason = "http_status_not_200";
        goto cleanup;
    }
    if (content_length != (int64_t)manifest->size_bytes) {
        ESP_LOGE(TAG, "Content-Length=%lld expected=%lu",
                 (long long)content_length, (unsigned long)manifest->size_bytes);
        err = ESP_ERR_INVALID_SIZE;
        failure_reason = "content_length_mismatch";
        goto cleanup;
    }
    err = check_transport_control(transport_started_us, &failure_reason);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = nuwax_ota_sha256_init(&sha);
    if (err != ESP_OK) {
        failure_reason = "sha256_init_failed";
        goto cleanup;
    }

    if (!entered_writing) {
        err = nuwax_ota_enter_writing(0);
        if (err != ESP_OK) {
            failure_reason = nuwax_ota_cancel_requested()
                                 ? "cancelled" : "state_write_failed";
            goto cleanup;
        }
        entered_writing = true;
    }
    /* Erase sectors immediately before their sequential write. Opening the
     * HTTPS response and then erasing a multi-megabyte range in one blocking
     * operation can leave the peer stalled long enough to close the socket. */
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES,
                        &ota_handle);
    if (err != ESP_OK) {
        failure_reason = "ota_begin_failed";
        goto cleanup;
    }
    ota_active = true;

    const int64_t transfer_started_us = esp_timer_get_time();
    int64_t last_data_us = transfer_started_us;
    int64_t max_read_us = 0;
    int64_t max_write_us = 0;
    int64_t total_read_us = 0;
    int64_t total_hash_us = 0;
    int64_t total_write_us = 0;
    uint32_t read_calls = 0;
    uint32_t zero_reads = 0;
    uint32_t next_resource_log = NUWAX_OTA_RESOURCE_LOG_BYTES;
    for (;;) {
        err = check_transport_control(transport_started_us, &failure_reason);
        if (err != ESP_OK) {
            goto cleanup;
        }

        const int64_t read_started_us = esp_timer_get_time();
        int count = esp_http_client_read(client, (char *)buffer,
                                         (int)io_buffer_bytes);
        const int system_errno = errno;
        const int64_t read_elapsed_us = esp_timer_get_time() - read_started_us;
        ++read_calls;
        total_read_us += read_elapsed_us;
        if (read_elapsed_us > max_read_us) {
            max_read_us = read_elapsed_us;
        }
        if (count > 0) {
            last_data_us = esp_timer_get_time();
            if ((uint64_t)received + (uint32_t)count > manifest->size_bytes) {
                err = ESP_ERR_INVALID_SIZE;
                failure_reason = "response_larger_than_manifest";
                goto cleanup;
            }
            const int64_t hash_started_us = esp_timer_get_time();
            err = nuwax_ota_sha256_update(&sha, buffer, (size_t)count);
            total_hash_us += esp_timer_get_time() - hash_started_us;
            if (err != ESP_OK) {
                failure_reason = "sha256_update_failed";
                goto cleanup;
            }
            const int64_t write_started_us = esp_timer_get_time();
            err = esp_ota_write(ota_handle, buffer, (size_t)count);
            const int64_t write_elapsed_us =
                esp_timer_get_time() - write_started_us;
            if (write_elapsed_us > max_write_us) {
                max_write_us = write_elapsed_us;
            }
            total_write_us += write_elapsed_us;
            if (err != ESP_OK) {
                failure_reason = "ota_write_failed";
                goto cleanup;
            }
            received += (uint32_t)count;
            nuwax_ota_set_downloaded_bytes(received);
            if (received >= next_resource_log) {
                ESP_LOGI(TAG,
                         "transfer attempt=%u received=%lu/%lu calls=%lu "
                         "zero=%lu read_ms=%lld hash_ms=%lld write_ms=%lld "
                         "max_read_us=%lld max_write_us=%lld stack_min_free=%u "
                         "internal_free=%u internal_largest=%u",
                         download_attempt,
                         (unsigned long)received,
                         (unsigned long)manifest->size_bytes,
                         (unsigned long)read_calls,
                         (unsigned long)zero_reads,
                         (long long)(total_read_us / 1000LL),
                         (long long)(total_hash_us / 1000LL),
                         (long long)(total_write_us / 1000LL),
                         (long long)max_read_us,
                         (long long)max_write_us,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL),
                         (unsigned)heap_caps_get_free_size(
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                do {
                    next_resource_log += NUWAX_OTA_RESOURCE_LOG_BYTES;
                } while (received >= next_resource_log);
            }
            continue;
        }

        ++zero_reads;
        if (count == 0 && esp_http_client_is_complete_data_received(client)) {
            break;
        }
        if (count < 0 && count != -ESP_ERR_HTTP_EAGAIN) {
            const nuwax_ota_http_error_t diagnostic = capture_http_error(
                client, system_errno, "https_read_failed", received);
            if (http_error_is_transient(&diagnostic)) {
                ESP_LOGW(TAG,
                         "retrying transient HTTP read failure at byte %lu",
                         (unsigned long)received);
                if (elapsed_at_least(last_data_us,
                                     NUWAX_OTA_STALL_TIMEOUT_MS)) {
                    err = ESP_ERR_TIMEOUT;
                    failure_reason = "https_read_stalled";
                    retry_download = true;
                    goto cleanup;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            format_http_failure(failure_detail, sizeof(failure_detail),
                                "https_read_failed", received, &diagnostic);
            err = ESP_FAIL;
            failure_reason = failure_detail;
            retry_download = true;
            goto cleanup;
        }
        if (elapsed_at_least(last_data_us, NUWAX_OTA_STALL_TIMEOUT_MS)) {
            err = ESP_ERR_TIMEOUT;
            failure_reason = "https_read_stalled";
            retry_download = true;
            goto cleanup;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    const int64_t transfer_elapsed_us =
        esp_timer_get_time() - transfer_started_us;
    const uint32_t effective_bytes_per_second = transfer_elapsed_us > 0
        ? (uint32_t)(((uint64_t)received * 1000000ULL) /
                     (uint64_t)transfer_elapsed_us)
        : 0U;
    ESP_LOGI(TAG,
             "transfer complete bytes=%lu elapsed_ms=%lld bytes_per_second=%lu "
             "calls=%lu zero=%lu read_ms=%lld hash_ms=%lld write_ms=%lld "
             "buffer=%u",
             (unsigned long)received,
             (long long)(transfer_elapsed_us / 1000LL),
             (unsigned long)effective_bytes_per_second,
             (unsigned long)read_calls,
             (unsigned long)zero_reads,
             (long long)(total_read_us / 1000LL),
             (long long)(total_hash_us / 1000LL),
             (long long)(total_write_us / 1000LL),
             (unsigned)io_buffer_bytes);

    if (!esp_http_client_is_complete_data_received(client)) {
        err = ESP_ERR_INVALID_RESPONSE;
        failure_reason = "response_incomplete";
        goto cleanup;
    }
    if (received != manifest->size_bytes) {
        ESP_LOGE(TAG, "received=%lu expected=%lu", (unsigned long)received,
                 (unsigned long)manifest->size_bytes);
        err = ESP_ERR_INVALID_SIZE;
        failure_reason = "received_length_mismatch";
        goto cleanup;
    }
    err = check_transport_control(transport_started_us, &failure_reason);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = nuwax_ota_enter_verifying(99);
    if (err != ESP_OK) {
        failure_reason = nuwax_ota_cancel_requested() ? "cancelled"
                                                       : "state_verify_failed";
        goto cleanup;
    }

    char actual_sha256[NUWAX_OTA_MANIFEST_SHA256_LEN] = {0};
    err = nuwax_ota_sha256_final(&sha, actual_sha256);
    if (err != ESP_OK) {
        failure_reason = "sha256_final_failed";
        goto cleanup;
    }
    if (memcmp(actual_sha256, manifest->image_sha256, 64U) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch got=%s expected=%s", actual_sha256,
                 manifest->image_sha256);
        err = ESP_ERR_INVALID_CRC;
        failure_reason = "sha256_mismatch";
        goto cleanup;
    }

    err = check_transport_control(transport_started_us, &failure_reason);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = esp_ota_end(ota_handle);
    ota_active = false;
    if (err != ESP_OK) {
        failure_reason = "image_validation_failed";
        goto cleanup;
    }

    esp_app_desc_t staged_app = {0};
    err = esp_ota_get_partition_description(update_partition, &staged_app);
    if (err != ESP_OK) {
        failure_reason = "staged_descriptor_failed";
        goto cleanup;
    }
    if (strcmp(staged_app.project_name, running_app->project_name) != 0) {
        ESP_LOGE(TAG, "staged project=%s running project=%s",
                 staged_app.project_name, running_app->project_name);
        err = ESP_ERR_INVALID_VERSION;
        failure_reason = "staged_project_mismatch";
        goto cleanup;
    }
    if (strcmp(staged_app.version, manifest->version) != 0) {
        ESP_LOGE(TAG, "staged version=%s manifest=%s", staged_app.version,
                 manifest->version);
        err = ESP_ERR_INVALID_VERSION;
        failure_reason = "staged_version_mismatch";
        goto cleanup;
    }
    err = check_transport_control(transport_started_us, &failure_reason);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        failure_reason = "set_boot_partition_failed";
        goto cleanup;
    }

    if (http_open) {
        (void)esp_http_client_close(client);
        http_open = false;
    }
    esp_http_client_cleanup(client);
    client = NULL;
    free(buffer);
    buffer = NULL;
    restore_wifi_power_save(&wifi_ps_guard);

    err = nuwax_ota_enter_done(100);
    if (err != ESP_OK) {
        nuwax_ota_fail("state_done_failed");
        return err;
    }
    ESP_LOGI(TAG, "staged and verified version=%s sha256=%s",
             manifest->version, actual_sha256);
    return ESP_OK;

cleanup:
    if (ota_active) {
        esp_err_t abort_err = esp_ota_abort(ota_handle);
        if (abort_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_ota_abort failed: %s",
                     esp_err_to_name(abort_err));
            /* A live or unknown OTA handle must never overlap a retry. */
            retry_download = false;
            err = abort_err;
            failure_reason = "ota_abort_failed";
        }
        ota_active = false;
    }
    nuwax_ota_sha256_abort(&sha);
    if (http_open) {
        (void)esp_http_client_close(client);
        http_open = false;
    }
    if (client != NULL) {
        esp_http_client_cleanup(client);
        client = NULL;
    }
    free(buffer);
    buffer = NULL;

    if (retry_download &&
        download_attempt < NUWAX_OTA_DOWNLOAD_ATTEMPTS &&
        !nuwax_ota_cancel_requested()) {
        const char *control_reason = failure_reason;
        esp_err_t control_err = check_transport_control(
            transport_started_us, &control_reason);
        if (control_err == ESP_OK) {
            const uint32_t backoff_ms =
                NUWAX_OTA_RETRY_BASE_MS << (download_attempt - 1U);
            ESP_LOGW(TAG,
                     "download attempt %u/%u failed reason=%s err=%s; "
                     "restarting from byte zero in %lu ms",
                     download_attempt, NUWAX_OTA_DOWNLOAD_ATTEMPTS,
                     failure_reason, esp_err_to_name(err),
                     (unsigned long)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            goto download_attempt_start;
        }
        err = control_err;
        failure_reason = control_reason;
    }

    restore_wifi_power_save(&wifi_ps_guard);
    ESP_LOGE(TAG, "OTA failed reason=%s err=%s", failure_reason,
             esp_err_to_name(err));
    (void)nuwax_device_error_report_enqueue(
        "error", "ota", "transport", err, "ota", 0U, NULL);
    log_ota_heap("failure_cleanup");
    nuwax_ota_fail(failure_reason);
    return err;
}
