#include "ws_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/types.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_tls.h"
#include "esp_tls_crypto.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "sdkconfig.h"
#include "ws_send_failure_policy.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "nuwax_ws";
#define NUWAX_WS_RX_TASK_PRIORITY 10U
static SemaphoreHandle_t s_lifecycle_lock;

static void secure_zero_memory(void *memory, size_t length) {
    volatile uint8_t *cursor = (volatile uint8_t *)memory;
    while (length-- > 0U) {
        *cursor++ = 0U;
    }
}

#define NUWAX_WS_SOCKET_TIMEOUT_MS 3000
#define NUWAX_WS_IO_TOTAL_TIMEOUT_MS 4000
#define NUWAX_WS_REALTIME_INFLIGHT_DRAIN_TIMEOUT_MS 5000U
#define NUWAX_WS_HEARTBEAT_INTERVAL_MS 25000
#define NUWAX_WS_RX_IDLE_TIMEOUT_MS 60000
#define NUWAX_WS_HEARTBEAT_HEALTHY_MS 60000U
#define NUWAX_WS_HANDSHAKE_TIMEOUT_MS 8000
#define NUWAX_WS_LIFECYCLE_LOCK_TIMEOUT_MS 5000
#define NUWAX_WS_HANDLE_LOCK_TIMEOUT_MS 5000
#define NUWAX_WS_RX_TASK_STOP_TIMEOUT_MS 5000
#define NUWAX_WS_INTERRUPT_LOCK_TIMEOUT_MS 50
#define NUWAX_WS_TCP_KEEPIDLE_SECONDS 45
#define NUWAX_WS_TCP_KEEPINTVL_SECONDS 10
#define NUWAX_WS_TCP_KEEPCNT 3
#define NUWAX_WS_MAX_INCOMING_FRAME_BYTES (16U * 1024U)
#define NUWAX_WS_RX_CALLBACK_WARN_MS 40U
#define NUWAX_WS_RX_STACK_WARN_BYTES 4096U
#define NUWAX_WS_RX_STACK_WARN_STEP_BYTES 256U
#define NUWAX_WS_CLOSE_REASON_PREVIEW_BYTES 96U
#define NUWAX_WS_STATUS_LINE_PREVIEW_BYTES 120U

_Static_assert(NUWAX_WS_HEARTBEAT_INTERVAL_MS < NUWAX_WS_RX_IDLE_TIMEOUT_MS,
               "WebSocket heartbeat must run before the receive idle timeout");

#ifndef CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES
#define CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES 16384
#endif

#if CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN > 1024
#define NUWAX_WS_SEND_CHUNK_BYTES (CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN - 128)
#else
#define NUWAX_WS_SEND_CHUNK_BYTES 1024
#endif

static void release_rx_buffer(nuwax_ws_client_t *client) {
    if (!client || !client->rx_buffer) {
        return;
    }
    secure_zero_memory(client->rx_buffer, client->rx_buffer_capacity);
    heap_caps_free(client->rx_buffer);
    client->rx_buffer = NULL;
    client->rx_buffer_capacity = 0U;
}

static esp_err_t ensure_io_lock(nuwax_ws_client_t *client) {
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client->io_lock) {
        return ESP_OK;
    }
    client->io_lock = xSemaphoreCreateMutex();
    return client->io_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

/*
 * The I/O mutex is also used by the RX task.  Owner-side lifecycle operations
 * must never wait forever for it: an in-flight TLS/socket operation may be
 * stuck until its own transport deadline expires.  A failed acquisition means
 * that the caller does not own the handle and must retain the client for a
 * later close retry.
 */
static esp_err_t take_io_lock_with_timeout(nuwax_ws_client_t *client,
                                           uint32_t timeout_ms) {
    if (!client || !client->io_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    return xSemaphoreTake((SemaphoreHandle_t)client->io_lock, timeout_ticks) ==
                   pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static esp_err_t ensure_rx_buffer(nuwax_ws_client_t *client) {
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t required = NUWAX_WS_MAX_INCOMING_FRAME_BYTES + 1U;
    if (client->rx_buffer && client->rx_buffer_capacity >= required) {
        return ESP_OK;
    }
    release_rx_buffer(client);
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(
        required, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const char *heap_name = "psram";
    if (!buffer) {
        buffer = (uint8_t *)heap_caps_malloc(required, MALLOC_CAP_8BIT);
        heap_name = "8bit";
    }
    if (!buffer) {
        ESP_LOGE(TAG,
                 "No memory for connection RX buffer bytes=%u psram_free=%u internal_free=%u",
                 (unsigned)required,
                 (unsigned)heap_caps_get_free_size(
                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    client->rx_buffer = buffer;
    client->rx_buffer_capacity = required;
    ESP_LOGI(TAG,
             "WebSocket connection RX buffer ready bytes=%u heap=%s",
             (unsigned)required,
             heap_name);
    return ESP_OK;
}

#if CONFIG_NUWAX_TLS_TRYCLOUDFLARE_CA_ENABLE
static const char NUWAX_GTS_ROOT_R4_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYDVQQGEwJVUzEi\n"
    "MCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQw\n"
    "HhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZ\n"
    "R29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjO\n"
    "PQIBBgUrgQQAIgNiAATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzu\n"
    "hXyiQHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvRHYqjQjBA\n"
    "MA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBSATNbrdP9JNqPV2Py1\n"
    "PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/C\n"
    "r8deVl5c1RxYIigL9zC2L7F8AjEA8GE8p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh\n"
    "4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

static bool is_trycloudflare_host(const char *host) {
    const char suffix[] = ".trycloudflare.com";
    if (!host) {
        return false;
    }
    if (strcmp(host, "trycloudflare.com") == 0) {
        return true;
    }
    size_t host_len = strlen(host);
    size_t suffix_len = strlen(suffix);
    return host_len > suffix_len && strcmp(host + host_len - suffix_len, suffix) == 0;
}
#endif

typedef struct {
    char host[128];
    char path[256];
    int port;
    bool secure;
} parsed_ws_url_t;

static const char BASE64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode_16(const uint8_t input[16], char output[25]) {
    int out = 0;
    for (int i = 0; i < 16; i += 3) {
        uint32_t value = ((uint32_t)input[i] << 16);
        int remaining = 16 - i;
        if (remaining > 1) {
            value |= ((uint32_t)input[i + 1] << 8);
        }
        if (remaining > 2) {
            value |= input[i + 2];
        }
        output[out++] = BASE64_ALPHABET[(value >> 18) & 0x3f];
        output[out++] = BASE64_ALPHABET[(value >> 12) & 0x3f];
        output[out++] = remaining > 1 ? BASE64_ALPHABET[(value >> 6) & 0x3f] : '=';
        output[out++] = remaining > 2 ? BASE64_ALPHABET[value & 0x3f] : '=';
    }
    output[out] = '\0';
}

static esp_err_t websocket_expected_accept(const char *key, char output[29]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char source[sizeof(guid) + 25];
    int source_len = snprintf(source, sizeof(source), "%s%s", key, guid);
    if (source_len <= 0 || source_len >= (int)sizeof(source)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[20];
    if (esp_crypto_sha1((const unsigned char *)source, (size_t)source_len, digest) != 0) {
        return ESP_FAIL;
    }
    size_t encoded_len = 0;
    memset(output, 0, 29);
    if (esp_crypto_base64_encode((unsigned char *)output,
                                 29,
                                 &encoded_len,
                                 digest,
                                 sizeof(digest)) != 0 ||
        encoded_len != 28) {
        return ESP_FAIL;
    }
    output[encoded_len] = '\0';
    return ESP_OK;
}

static bool http_response_header_value(const char *response,
                                       const char *name,
                                       char *value,
                                       size_t value_size) {
    if (!response || !name || !value || value_size == 0) {
        return false;
    }
    const size_t name_len = strlen(name);
    const char *line = strstr(response, "\r\n");
    if (!line) {
        return false;
    }
    line += 2;
    while (line[0] != '\0' && !(line[0] == '\r' && line[1] == '\n')) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end) {
            return false;
        }
        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon && (size_t)(colon - line) == name_len && strncasecmp(line, name, name_len) == 0) {
            const char *start = colon + 1;
            while (start < line_end && (*start == ' ' || *start == '\t')) {
                start++;
            }
            const char *end = line_end;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
                end--;
            }
            const size_t len = (size_t)(end - start);
            if (len >= value_size) {
                return false;
            }
            memcpy(value, start, len);
            value[len] = '\0';
            return true;
        }
        line = line_end + 2;
    }
    return false;
}

static bool http_header_has_token(const char *value, const char *token) {
    if (!value || !token) {
        return false;
    }
    const size_t token_len = strlen(token);
    const char *cursor = value;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }
        const char *end = cursor;
        while (*end != '\0' && *end != ',') {
            end++;
        }
        const char *trimmed_end = end;
        while (trimmed_end > cursor && (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t')) {
            trimmed_end--;
        }
        if ((size_t)(trimmed_end - cursor) == token_len &&
            strncasecmp(cursor, token, token_len) == 0) {
            return true;
        }
        cursor = end;
    }
    return false;
}

static esp_err_t parse_ws_url(const char *url, parsed_ws_url_t *parsed) {
    if (!url || !parsed) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(parsed, 0, sizeof(*parsed));
    size_t scheme_len = 0;
    if (strncmp(url, "ws://", 5) == 0) {
        parsed->secure = false;
        parsed->port = 80;
        scheme_len = 5;
    } else if (strncmp(url, "wss://", 6) == 0) {
        parsed->secure = true;
        parsed->port = 443;
        scheme_len = 6;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    const char *host_start = url + scheme_len;
    const char *slash_start = strchr(host_start, '/');
    const char *query_start = strchr(host_start, '?');
    const char *path_start = NULL;
    if (slash_start && query_start) {
        path_start = slash_start < query_start ? slash_start : query_start;
    } else {
        path_start = slash_start ? slash_start : query_start;
    }
    const char *host_end = path_start ? path_start : url + strlen(url);
    const char *port_start = memchr(host_start, ':', (size_t)(host_end - host_start));
    size_t host_len = port_start ? (size_t)(port_start - host_start) : (size_t)(host_end - host_start);

    if (host_len == 0 || host_len >= sizeof(parsed->host)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(parsed->host, host_start, host_len);
    parsed->host[host_len] = '\0';

    if (port_start) {
        parsed->port = atoi(port_start + 1);
        if (parsed->port <= 0 || parsed->port > 65535) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (path_start && *path_start == '?') {
        int len = snprintf(parsed->path, sizeof(parsed->path), "/%s", path_start);
        if (len <= 0 || len >= (int)sizeof(parsed->path)) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        strlcpy(parsed->path, path_start ? path_start : "/", sizeof(parsed->path));
    }
    return ESP_OK;
}

static bool transport_has_handle(const nuwax_ws_client_t *client) {
    if (!client) {
        return false;
    }
    return client->use_tls ? client->tls != NULL : client->sock >= 0;
}

static bool transport_owns_any_handle(const nuwax_ws_client_t *client) {
    return client && (client->tls != NULL || client->sock >= 0);
}

static bool close_requested_for_current_epoch(
    const nuwax_ws_client_t *client) {
    if (!client) {
        return false;
    }
    const uint32_t epoch = (uint32_t)atomic_load_explicit(
        &client->connection_epoch, memory_order_acquire);
    const uint32_t requested_epoch = (uint32_t)atomic_load_explicit(
        &client->close_requested_epoch, memory_order_acquire);
    return nuwax_ws_close_requested_for_epoch(epoch, requested_epoch);
}

static bool close_requested_for_epoch(const nuwax_ws_client_t *client,
                                      uint32_t expected_epoch) {
    if (!client || expected_epoch == 0U) {
        return false;
    }
    const uint32_t current_epoch = nuwax_ws_connection_epoch(client);
    const uint32_t requested_epoch = (uint32_t)atomic_load_explicit(
        &client->close_requested_epoch, memory_order_acquire);
    return current_epoch == expected_epoch &&
           nuwax_ws_close_requested_for_epoch(current_epoch,
                                              requested_epoch);
}

static void tx_stage_store(nuwax_ws_client_t *client,
                           nuwax_ws_tx_stage_t stage,
                           uint32_t epoch) {
    if (!client) {
        return;
    }
    atomic_store_explicit(&client->tx_stage_epoch, epoch,
                          memory_order_relaxed);
    atomic_store_explicit(
        &client->tx_stage_since_ms,
        (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL),
        memory_order_relaxed);
    atomic_store_explicit(&client->tx_stage, (int)stage,
                          memory_order_release);
}

static bool transport_is_open(const nuwax_ws_client_t *client) {
    /* Handle fields are protected by io_lock. Public readiness queries must
     * not race their mutation; send/read paths validate the handle again
     * while holding the lock. */
    return client &&
           atomic_load_explicit(&client->connected, memory_order_acquire) &&
           !close_requested_for_current_epoch(client);
}

static ssize_t transport_write_unlocked(nuwax_ws_client_t *client,
                                        const uint8_t *data,
                                        size_t len) {
    if (!transport_has_handle(client)) {
        return -1;
    }
    if (client->use_tls) {
        return esp_tls_conn_write(client->tls, data, len);
    }
    return send(client->sock, data, len, 0);
}

static ssize_t transport_read_unlocked(nuwax_ws_client_t *client,
                                       uint8_t *data,
                                       size_t len) {
    if (!transport_has_handle(client)) {
        return -1;
    }
    if (client->use_tls) {
        return esp_tls_conn_read(client->tls, data, len);
    }
    return recv(client->sock, data, len, 0);
}

static esp_err_t transport_close(nuwax_ws_client_t *client) {
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!transport_owns_any_handle(client)) {
        client->use_tls = false;
        return ESP_OK;
    }
    SemaphoreHandle_t io_lock = (SemaphoreHandle_t)client->io_lock;
    const esp_err_t lock_err = take_io_lock_with_timeout(
        client, NUWAX_WS_HANDLE_LOCK_TIMEOUT_MS);
    if (lock_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "WebSocket transport close could not acquire I/O lock: %s; handle retained",
                 esp_err_to_name(lock_err));
        return lock_err == ESP_ERR_TIMEOUT ? ESP_ERR_NOT_FINISHED : lock_err;
    }
    if (client->tls) {
        esp_tls_conn_destroy(client->tls);
        client->tls = NULL;
        client->sock = -1;
    } else if (client->sock >= 0) {
        shutdown(client->sock, SHUT_RDWR);
        close(client->sock);
        client->sock = -1;
    }
    client->use_tls = false;
    xSemaphoreGive(io_lock);
    return ESP_OK;
}

static void transport_interrupt_if_epoch(nuwax_ws_client_t *client,
                                         uint32_t expected_epoch) {
    if (!client) {
        return;
    }
    SemaphoreHandle_t io_lock = (SemaphoreHandle_t)client->io_lock;
    if (io_lock &&
        xSemaphoreTake(io_lock,
                       pdMS_TO_TICKS(NUWAX_WS_INTERRUPT_LOCK_TIMEOUT_MS)) !=
            pdTRUE) {
        /* connected=false is also observed by the non-blocking RX loop. Do
         * not race a live TLS/socket call merely to force an early wakeup. */
        return;
    }
    if (expected_epoch != 0U &&
        nuwax_ws_connection_epoch(client) != expected_epoch) {
        if (io_lock) {
            xSemaphoreGive(io_lock);
        }
        return;
    }
    const int sock = client->sock;
    if (sock >= 0) {
        (void)shutdown(sock, SHUT_RDWR);
    }
    if (io_lock) {
        xSemaphoreGive(io_lock);
    }
}

static void transport_interrupt(nuwax_ws_client_t *client) {
    transport_interrupt_if_epoch(client, 0U);
}

static void configure_socket_timeouts(int sock) {
    if (sock < 0) {
        return;
    }
    struct timeval timeout = {
        .tv_sec = NUWAX_WS_SOCKET_TIMEOUT_MS / 1000,
        .tv_usec = (NUWAX_WS_SOCKET_TIMEOUT_MS % 1000) * 1000,
    };
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGW(TAG, "set SO_SNDTIMEO failed errno=%d", errno);
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGW(TAG, "set SO_RCVTIMEO failed errno=%d", errno);
    }
    const int tcp_nodelay = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &tcp_nodelay, sizeof(tcp_nodelay)) != 0) {
        ESP_LOGW(TAG, "set TCP_NODELAY failed errno=%d", errno);
    }
    const int keepalive = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive,
                   sizeof(keepalive)) != 0) {
        ESP_LOGW(TAG, "set SO_KEEPALIVE failed errno=%d", errno);
    }
#ifdef TCP_KEEPIDLE
    const int keepidle = NUWAX_WS_TCP_KEEPIDLE_SECONDS;
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle,
                   sizeof(keepidle)) != 0) {
        ESP_LOGW(TAG, "set TCP_KEEPIDLE failed errno=%d", errno);
    }
#endif
#ifdef TCP_KEEPINTVL
    const int keepintvl = NUWAX_WS_TCP_KEEPINTVL_SECONDS;
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl,
                   sizeof(keepintvl)) != 0) {
        ESP_LOGW(TAG, "set TCP_KEEPINTVL failed errno=%d", errno);
    }
#endif
#ifdef TCP_KEEPCNT
    const int keepcnt = NUWAX_WS_TCP_KEEPCNT;
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt,
                   sizeof(keepcnt)) != 0) {
        ESP_LOGW(TAG, "set TCP_KEEPCNT failed errno=%d", errno);
    }
#endif
}

static esp_err_t configure_socket_nonblocking(int sock) {
    if (sock < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        ESP_LOGE(TAG, "get socket flags failed errno=%d", errno);
        return ESP_FAIL;
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) != 0) {
        ESP_LOGE(TAG, "set socket non-blocking failed errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool io_retryable(const nuwax_ws_client_t *client,
                         ssize_t ret,
                         int operation_errno) {
    if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return true;
    }
    if (client && client->use_tls && ret == ESP_TLS_ERR_SSL_TIMEOUT) {
        return true;
    }
    return client && !client->use_tls && ret < 0 &&
           (operation_errno == EAGAIN || operation_errno == EWOULDBLOCK ||
            operation_errno == EINTR);
}

static esp_err_t transport_wait_write_ready_until(
    const nuwax_ws_client_t *client,
    int64_t deadline_us) {
    if (!client || client->sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        const int64_t remaining_us = deadline_us - now_us;
        struct timeval wait = {
            .tv_sec = (time_t)(remaining_us / 1000000LL),
            .tv_usec = (suseconds_t)(remaining_us % 1000000LL),
        };
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(client->sock, &write_fds);
        errno = 0;
        const int result = select(client->sock + 1,
                                  NULL,
                                  &write_fds,
                                  NULL,
                                  &wait);
        const int operation_errno = errno;
        const bool socket_writable =
            result > 0 && FD_ISSET(client->sock, &write_fds);
        if (socket_writable) {
            return ESP_OK;
        }
        const bool retryable_error =
            result < 0 &&
            (operation_errno == EINTR || operation_errno == EAGAIN ||
             operation_errno == EWOULDBLOCK);
        if (result == 0) {
            return ESP_ERR_TIMEOUT;
        }
        if (nuwax_ws_write_probe_should_defer(result,
                                              socket_writable,
                                              retryable_error)) {
            continue;
        }
        ESP_LOGW(TAG,
                 "WebSocket socket write readiness check failed errno=%d (%s)",
                 operation_errno,
                 strerror(operation_errno));
        return ESP_FAIL;
    }
}

static int64_t io_deadline_after_ms(uint32_t timeout_ms) {
    return esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
}

static bool io_deadline_expired(int64_t deadline_us) {
    return esp_timer_get_time() >= deadline_us;
}

static bool transport_io_lock_until(nuwax_ws_client_t *client,
                                    int64_t deadline_us) {
    if (!client || !client->io_lock) {
        return false;
    }
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return false;
    }
    uint64_t wait_ms = ((uint64_t)remaining_us + 999ULL) / 1000ULL;
    if (wait_ms > UINT32_MAX) {
        wait_ms = UINT32_MAX;
    }
    TickType_t ticks = pdMS_TO_TICKS((uint32_t)wait_ms);
    if (ticks == 0) {
        ticks = 1;
    }
    SemaphoreHandle_t lock = (SemaphoreHandle_t)client->io_lock;
    if (xSemaphoreTake(lock, ticks) != pdTRUE) {
        return false;
    }
    if (io_deadline_expired(deadline_us)) {
        xSemaphoreGive(lock);
        return false;
    }
    return true;
}

static void transport_io_unlock(nuwax_ws_client_t *client) {
    if (client && client->io_lock) {
        xSemaphoreGive((SemaphoreHandle_t)client->io_lock);
    }
}

static esp_err_t transport_read_until(nuwax_ws_client_t *client,
                                      uint8_t *data,
                                      size_t len,
                                      int64_t deadline_us,
                                      ssize_t *result,
                                      int *operation_errno) {
    if (!data || !result || !operation_errno) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!transport_io_lock_until(client, deadline_us)) {
        return ESP_ERR_TIMEOUT;
    }
    if (atomic_load_explicit(&client->rx_task_running,
                             memory_order_acquire) &&
        !atomic_load_explicit(&client->connected,
                              memory_order_acquire)) {
        transport_io_unlock(client);
        return ESP_ERR_INVALID_STATE;
    }
    errno = 0;
    *result = transport_read_unlocked(client, data, len);
    *operation_errno = errno;
    transport_io_unlock(client);
    /* A successful read owns the bytes it returned, even when the clock
     * crosses the deadline while the socket/TLS call is unwinding. Returning
     * TIMEOUT here would make recv_exact_until() discard a consumed frame
     * header byte and start the next WebSocket frame one byte late. The
     * caller still enforces the deadline before its next read when a partial
     * operation did not satisfy the requested length. */
    if (*result > 0) {
        return ESP_OK;
    }
    return io_deadline_expired(deadline_us) ? ESP_ERR_TIMEOUT : ESP_OK;
}

static esp_err_t send_all_until(nuwax_ws_client_t *client,
                                const uint8_t *data,
                                size_t len,
                                int64_t soft_deadline_us,
                                int64_t hard_deadline_us,
                                bool inflight_drain_enabled,
                                size_t *frame_bytes_written,
                                bool *transport_write_attempted) {
    if (!client || (!data && len > 0U) || soft_deadline_us <= 0 ||
        hard_deadline_us < soft_deadline_us) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0U) {
        return ESP_OK;
    }
    const bool entered_with_unsafe_progress =
        nuwax_ws_send_should_drain_inflight(
            inflight_drain_enabled,
            frame_bytes_written ? *frame_bytes_written : 0U,
            transport_write_attempted ? *transport_write_attempted : false,
            client->use_tls);
    const uint32_t tx_epoch = nuwax_ws_connection_epoch(client);
    tx_stage_store(client, NUWAX_WS_TX_STAGE_WAIT_IO, tx_epoch);
    if (!transport_io_lock_until(
            client,
            entered_with_unsafe_progress ? hard_deadline_us
                                         : soft_deadline_us)) {
        tx_stage_store(client, NUWAX_WS_TX_STAGE_IDLE, 0U);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = ESP_OK;
    size_t sent = 0;
    tx_stage_store(client, NUWAX_WS_TX_STAGE_WRITING, tx_epoch);
    while (sent < len) {
        if (close_requested_for_epoch(client, tx_epoch)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (atomic_load_explicit(&client->rx_task_running,
                                 memory_order_acquire) &&
            !atomic_load_explicit(&client->connected,
                                  memory_order_acquire)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        const bool unsafe_progress =
            nuwax_ws_send_should_drain_inflight(
                inflight_drain_enabled,
                frame_bytes_written ? *frame_bytes_written : 0U,
                transport_write_attempted
                    ? *transport_write_attempted
                    : false,
                client->use_tls);
        const int64_t active_deadline_us =
            unsafe_progress ? hard_deadline_us : soft_deadline_us;
        if (io_deadline_expired(active_deadline_us)) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        errno = 0;
        if (transport_write_attempted) {
            *transport_write_attempted = true;
        }
        const ssize_t ret =
            transport_write_unlocked(client, data + sent, len - sent);
        const int operation_errno = errno;
        if (ret > 0) {
            sent += (size_t)ret;
            if (frame_bytes_written) {
                *frame_bytes_written += (size_t)ret;
            }
            if (sent == len) {
                break;
            }
            const bool partial_frame_inflight =
                nuwax_ws_send_should_drain_inflight(
                    inflight_drain_enabled,
                    frame_bytes_written ? *frame_bytes_written : 0U,
                    transport_write_attempted
                        ? *transport_write_attempted
                        : false,
                    client->use_tls);
            if (io_deadline_expired(partial_frame_inflight
                                        ? hard_deadline_us
                                        : soft_deadline_us)) {
                result = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        const bool attempted_frame_inflight =
            nuwax_ws_send_should_drain_inflight(
                inflight_drain_enabled,
                frame_bytes_written ? *frame_bytes_written : 0U,
                transport_write_attempted
                    ? *transport_write_attempted
                    : false,
                client->use_tls);
        if (io_deadline_expired(attempted_frame_inflight
                                    ? hard_deadline_us
                                    : soft_deadline_us)) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        if (io_retryable(client, ret, operation_errno)) {
            tx_stage_store(client, NUWAX_WS_TX_STAGE_RETRYING, tx_epoch);
            vTaskDelay(pdMS_TO_TICKS(10));
            tx_stage_store(client, NUWAX_WS_TX_STAGE_WRITING, tx_epoch);
            continue;
        }
        if (ret <= 0) {
            result = ESP_FAIL;
            break;
        }
    }
    transport_io_unlock(client);
    tx_stage_store(client, NUWAX_WS_TX_STAGE_IDLE, 0U);
    return result;
}

static esp_err_t send_all(nuwax_ws_client_t *client, const uint8_t *data, size_t len) {
    const int64_t deadline_us =
        io_deadline_after_ms(NUWAX_WS_IO_TOTAL_TIMEOUT_MS);
    return send_all_until(client, data, len, deadline_us, deadline_us, false,
                          NULL, NULL);
}

static esp_err_t recv_exact_until(nuwax_ws_client_t *client,
                                  uint8_t *data,
                                  size_t len,
                                  int64_t deadline_us) {
    size_t received = 0;
    while (received < len) {
        if (atomic_load_explicit(&client->rx_task_running,
                                 memory_order_acquire) &&
            !atomic_load_explicit(&client->connected,
                                  memory_order_acquire)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (io_deadline_expired(deadline_us)) {
            return ESP_ERR_TIMEOUT;
        }
        ssize_t ret = 0;
        int operation_errno = 0;
        const esp_err_t read_err = transport_read_until(client,
                                                        data + received,
                                                        len - received,
                                                        deadline_us,
                                                        &ret,
                                                        &operation_errno);
        if (read_err != ESP_OK) {
            return read_err;
        }
        if (io_retryable(client, ret, operation_errno)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    return ESP_OK;
}

static int connect_tcp(const parsed_ws_url_t *url) {
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%d", url->port);

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    int err = getaddrinfo(url->host, port_text, &hints, &result);
    if (err != 0 || !result) {
        ESP_LOGE(TAG, "DNS lookup failed for %s:%s", url->host, port_text);
        return -1;
    }

    int sock = -1;
    int last_errno = 0;
    for (struct addrinfo *entry = result; entry; entry = entry->ai_next) {
        sock = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (sock < 0) {
            last_errno = errno;
            ESP_LOGW(TAG,
                     "TCP socket create failed for %s:%d errno=%d (%s)",
                     url->host,
                     url->port,
                     last_errno,
                     strerror(last_errno));
            continue;
        }
        if (connect(sock, entry->ai_addr, entry->ai_addrlen) == 0) {
            configure_socket_timeouts(sock);
            break;
        }
        last_errno = errno;
        ESP_LOGW(TAG,
                 "TCP connect attempt failed to %s:%d errno=%d (%s)",
                 url->host,
                 url->port,
                 last_errno,
                 strerror(last_errno));
        close(sock);
        sock = -1;
    }
    freeaddrinfo(result);
    if (sock < 0) {
        ESP_LOGE(TAG,
                 "TCP connect exhausted for %s:%d last_errno=%d (%s)",
                 url->host,
                 url->port,
                 last_errno,
                 strerror(last_errno));
    }
    return sock;
}

static esp_err_t connect_tls(nuwax_ws_client_t *client, const parsed_ws_url_t *url) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

#if CONFIG_NUWAX_TLS_TRYCLOUDFLARE_CA_ENABLE
    if (is_trycloudflare_host(url->host)) {
        ESP_LOGW(TAG, "Using pinned trycloudflare TLS CA GTS Root R4 for %s", url->host);
        cfg.crt_bundle_attach = NULL;
        cfg.cacert_buf = (const unsigned char *)NUWAX_GTS_ROOT_R4_PEM;
        cfg.cacert_bytes = sizeof(NUWAX_GTS_ROOT_R4_PEM);
    }
#endif

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        return ESP_ERR_NO_MEM;
    }

    int ret = esp_tls_conn_new_sync(url->host, (int)strlen(url->host), url->port, &cfg, tls);
    if (ret != 1) {
        ESP_LOGE(TAG, "TLS connect failed to %s:%d", url->host, url->port);
        esp_tls_conn_destroy(tls);
        return ESP_FAIL;
    }

    int sock = -1;
    if (esp_tls_get_conn_sockfd(tls, &sock) != ESP_OK || sock < 0) {
        ESP_LOGE(TAG,
                 "TLS socket fd unavailable; bounded non-blocking I/O cannot be guaranteed");
        esp_tls_conn_destroy(tls);
        return ESP_ERR_NOT_SUPPORTED;
    }
    configure_socket_timeouts(sock);
    SemaphoreHandle_t io_lock = (SemaphoreHandle_t)client->io_lock;
    const esp_err_t lock_err = take_io_lock_with_timeout(
        client, NUWAX_WS_HANDLE_LOCK_TIMEOUT_MS);
    if (lock_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "TLS handle publication could not acquire I/O lock: %s",
                 esp_err_to_name(lock_err));
        esp_tls_conn_destroy(tls);
        return lock_err;
    }
    client->tls = tls;
    client->sock = sock;
    client->use_tls = true;
    xSemaphoreGive(io_lock);
    return ESP_OK;
#else
    ESP_LOGE(TAG, "wss:// requires CONFIG_MBEDTLS_CERTIFICATE_BUNDLE");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t websocket_handshake(nuwax_ws_client_t *client,
                                     const parsed_ws_url_t *url,
                                     const char *token) {
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    char key[25];
    base64_encode_16(nonce, key);
    char expected_accept[29];
    ESP_RETURN_ON_ERROR(websocket_expected_accept(key, expected_accept),
                        TAG,
                        "compute WebSocket accept failed");

    char request[1024];
    int len = snprintf(request,
                       sizeof(request),
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s:%d\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Key: %s\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "X-Bridge-Token: %s\r\n"
                       "\r\n",
                       url->path,
                       url->host,
                       url->port,
                       key,
                       token);
    if (len <= 0 || len >= (int)sizeof(request)) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t send_err = send_all(client, (const uint8_t *)request, (size_t)len);
    secure_zero_memory(request, sizeof(request));
    ESP_RETURN_ON_ERROR(send_err, TAG, "handshake send failed");

    char response[768] = {0};
    size_t used = 0;
    bool headers_complete = false;
    const int64_t handshake_deadline_us = io_deadline_after_ms(NUWAX_WS_HANDSHAKE_TIMEOUT_MS);
    while (used + 1 < sizeof(response)) {
        ESP_RETURN_ON_ERROR(recv_exact_until(client,
                                             (uint8_t *)response + used,
                                             1,
                                             handshake_deadline_us),
                            TAG,
                            "handshake response read failed");
        used++;
        response[used] = '\0';
        if (strstr(response, "\r\n\r\n")) {
            headers_complete = true;
            break;
        }
    }
    if (!headers_complete) {
        ESP_LOGE(TAG, "WebSocket handshake headers incomplete or too large bytes=%u", (unsigned)used);
        return ESP_ERR_INVALID_RESPONSE;
    }
    bool status_101 = false;
    const char *line_end = strstr(response, "\r\n");
    size_t status_line_len = line_end ? (size_t)(line_end - response) : strlen(response);
    if (status_line_len >= 12 && strncmp(response, "HTTP/1.", 7) == 0 &&
        (response[7] == '0' || response[7] == '1') && response[8] == ' ') {
        const char *status = response + 9;
        while ((size_t)(status - response) < status_line_len && *status == ' ') {
            status++;
        }
        if (status_line_len - (size_t)(status - response) >= 3 &&
            status[0] >= '0' && status[0] <= '9' &&
            status[1] >= '0' && status[1] <= '9' &&
            status[2] >= '0' && status[2] <= '9') {
            client->last_http_status = (status[0] - '0') * 100 +
                                       (status[1] - '0') * 10 +
                                       (status[2] - '0');
        }
        status_101 = (status_line_len - (size_t)(status - response) >= 3 &&
                      status[0] == '1' && status[1] == '0' && status[2] == '1' &&
                      ((size_t)(status + 3 - response) == status_line_len || status[3] == ' ' || status[3] == '\t'));
    }
    if (!status_101) {
        char status_preview[NUWAX_WS_STATUS_LINE_PREVIEW_BYTES + 1U];
        const size_t preview_length =
            status_line_len < NUWAX_WS_STATUS_LINE_PREVIEW_BYTES
                ? status_line_len
                : NUWAX_WS_STATUS_LINE_PREVIEW_BYTES;
        for (size_t i = 0; i < preview_length; ++i) {
            const unsigned char value = (unsigned char)response[i];
            status_preview[i] = value >= 0x20U && value <= 0x7eU
                                    ? (char)value
                                    : '?';
        }
        status_preview[preview_length] = '\0';
        ESP_LOGE(TAG,
                 "WebSocket handshake rejected http_status=%d status_line=\"%s\" status_line_len=%u",
                 client->last_http_status,
                 status_preview,
                 (unsigned)status_line_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char upgrade[32];
    char connection[96];
    char accept[64];
    if (!http_response_header_value(response, "Upgrade", upgrade, sizeof(upgrade)) ||
        !http_header_has_token(upgrade, "websocket") ||
        !http_response_header_value(response, "Connection", connection, sizeof(connection)) ||
        !http_header_has_token(connection, "upgrade") ||
        !http_response_header_value(response, "Sec-WebSocket-Accept", accept, sizeof(accept)) ||
        strcmp(accept, expected_accept) != 0) {
        ESP_LOGE(TAG, "WebSocket handshake validation failed upgrade/connection/accept mismatch");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t ensure_lifecycle_lock(void) {
    if (s_lifecycle_lock) {
        return ESP_OK;
    }
    s_lifecycle_lock = xSemaphoreCreateMutex();
    return s_lifecycle_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t send_frame_unlocked(nuwax_ws_client_t *client,
                                     uint8_t opcode,
                                     const uint8_t *payload,
                                     size_t len,
                                     int64_t soft_deadline_us,
                                     int64_t hard_deadline_us,
                                     bool inflight_drain_enabled,
                                     size_t *frame_bytes_written,
                                     bool *transport_write_attempted) {
    if (frame_bytes_written) {
        *frame_bytes_written = 0U;
    }
    if (transport_write_attempted) {
        *transport_write_attempted = false;
    }
    if (!transport_is_open(client)) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool control_frame = (opcode & 0x08U) != 0;
    if ((opcode != 0x1 && opcode != 0x2 && opcode != 0x8 && opcode != 0x9 && opcode != 0xA) ||
        (control_frame && len > 125U) ||
        (len > 0 && !payload)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t chunk[NUWAX_WS_SEND_CHUNK_BYTES];
    size_t header_len = 0;
    chunk[header_len++] = 0x80 | opcode;
    if (len < 126) {
        chunk[header_len++] = 0x80 | (uint8_t)len;
    } else if (len <= 0xffff) {
        chunk[header_len++] = 0x80 | 126;
        chunk[header_len++] = (uint8_t)((len >> 8) & 0xff);
        chunk[header_len++] = (uint8_t)(len & 0xff);
    } else {
        chunk[header_len++] = 0x80 | 127;
        uint64_t big_len = (uint64_t)len;
        for (int shift = 56; shift >= 0; shift -= 8) {
            chunk[header_len++] = (uint8_t)((big_len >> shift) & 0xff);
        }
    }

    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    memcpy(&chunk[header_len], mask, sizeof(mask));
    header_len += sizeof(mask);

    const size_t first_payload_capacity = sizeof(chunk) - header_len;
    size_t first_payload_len = len;
    if (first_payload_len > first_payload_capacity) {
        first_payload_len = first_payload_capacity;
    }
    for (size_t i = 0; i < first_payload_len; i++) {
        chunk[header_len + i] = payload[i] ^ mask[i % 4];
    }
    const esp_err_t first_chunk_err =
        send_all_until(client,
                       chunk,
                       header_len + first_payload_len,
                       soft_deadline_us,
                       hard_deadline_us,
                       inflight_drain_enabled,
                       frame_bytes_written,
                       transport_write_attempted);
    if (first_chunk_err != ESP_OK) {
        if (first_chunk_err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG,
                     "frame header/first payload send failed: %s",
                     esp_err_to_name(first_chunk_err));
        }
        return first_chunk_err;
    }

    size_t offset = first_payload_len;
    while (offset < len) {
        size_t chunk_len = len - offset;
        if (chunk_len > sizeof(chunk)) {
            chunk_len = sizeof(chunk);
        }
        for (size_t i = 0; i < chunk_len; i++) {
            chunk[i] = payload[offset + i] ^ mask[(offset + i) % 4];
        }
        const esp_err_t payload_err =
            send_all_until(client, chunk, chunk_len,
                           soft_deadline_us,
                           hard_deadline_us,
                           inflight_drain_enabled,
                           frame_bytes_written,
                           transport_write_attempted);
        if (payload_err != ESP_OK) {
            if (payload_err != ESP_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "frame payload send failed: %s",
                         esp_err_to_name(payload_err));
            }
            return payload_err;
        }
        offset += chunk_len;
    }
    return ESP_OK;
}

static esp_err_t send_frame_with_timeout(nuwax_ws_client_t *client,
                                         uint8_t opcode,
                                         const uint8_t *payload,
                                         size_t len,
                                         uint32_t lock_timeout_ms,
                                         uint32_t io_timeout_ms,
                                         bool interrupt_on_lock_timeout,
                                         uint32_t expected_epoch) {
    ESP_RETURN_ON_ERROR(ensure_lifecycle_lock(), TAG, "create lifecycle lock failed");
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    if (expected_epoch == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t operation_started_us = esp_timer_get_time();
    tx_stage_store(client, NUWAX_WS_TX_STAGE_WAIT_LIFECYCLE,
                   expected_epoch);
    if (xSemaphoreTake(s_lifecycle_lock,
                       pdMS_TO_TICKS(lock_timeout_ms)) != pdTRUE) {
        (void)interrupt_on_lock_timeout;
        tx_stage_store(client, NUWAX_WS_TX_STAGE_IDLE, 0U);
        return ESP_ERR_TIMEOUT;
    }
    if (nuwax_ws_connection_epoch(client) != expected_epoch) {
        tx_stage_store(client, NUWAX_WS_TX_STAGE_IDLE, 0U);
        xSemaphoreGive(s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t effective_io_timeout_ms = io_timeout_ms;
    if (!interrupt_on_lock_timeout) {
        const int64_t lock_acquired_us = esp_timer_get_time();
        const uint64_t lock_wait_us =
            lock_acquired_us >= operation_started_us
                ? (uint64_t)(lock_acquired_us - operation_started_us)
                : 0U;
        const uint64_t total_budget_us =
            (uint64_t)lock_timeout_ms * 1000ULL;
        if (lock_wait_us >= total_budget_us) {
            tx_stage_store(client, NUWAX_WS_TX_STAGE_IDLE, 0U);
            xSemaphoreGive(s_lifecycle_lock);
            return ESP_ERR_TIMEOUT;
        }
        const uint64_t remaining_us = total_budget_us - lock_wait_us;
        const uint32_t remaining_ms =
            (uint32_t)((remaining_us + 999ULL) / 1000ULL);
        if (effective_io_timeout_ms > remaining_ms) {
            effective_io_timeout_ms = remaining_ms;
        }
    }
    size_t frame_bytes_written = 0U;
    bool transport_write_attempted = false;
    const bool secure_transport = client->use_tls;
    const bool inflight_drain_enabled = !interrupt_on_lock_timeout;
    const char *send_class =
        inflight_drain_enabled ? "realtime" : "reliable";
    const int64_t soft_deadline_us =
        io_deadline_after_ms(effective_io_timeout_ms);
    const uint32_t heartbeat_remaining_ms =
        nuwax_ws_heartbeat_remaining_ms_if_epoch(client, expected_epoch);
    const uint32_t inflight_drain_budget_ms =
        nuwax_ws_inflight_drain_budget_ms(
            NUWAX_WS_REALTIME_INFLIGHT_DRAIN_TIMEOUT_MS,
            heartbeat_remaining_ms);
    const int64_t hard_deadline_us =
        inflight_drain_enabled
            ? soft_deadline_us +
                  (int64_t)inflight_drain_budget_ms *
                      1000LL
            : soft_deadline_us;
    /* A realtime TLS frame may be discarded only before mbedTLS sees its
     * input buffer. Once ssl_write returns WANT_WRITE/READ, the same pointer
     * and length must remain valid until completion. Wait for socket capacity
     * inside the frame's remaining soft budget before handing the buffer to
     * TLS; control/reliable traffic keeps its existing delivery policy. */
    esp_err_t err = ESP_OK;
    if (inflight_drain_enabled && secure_transport) {
        err = transport_wait_write_ready_until(client, soft_deadline_us);
    }
    if (err == ESP_OK) {
        err = send_frame_unlocked(client, opcode, payload, len,
                                  soft_deadline_us,
                                  hard_deadline_us,
                                  inflight_drain_enabled,
                                  &frame_bytes_written,
                                  &transport_write_attempted);
    }
    const int64_t operation_finished_us = esp_timer_get_time();
    const bool unsafe_progress =
        nuwax_ws_send_has_unsafe_transport_progress(
            frame_bytes_written,
            transport_write_attempted,
            secure_transport);
    const bool heartbeat_healthy =
        nuwax_ws_heartbeat_healthy_if_epoch(client, expected_epoch);
    if (err == ESP_OK && inflight_drain_enabled && unsafe_progress &&
        operation_finished_us > soft_deadline_us) {
        ESP_LOGW(TAG,
                 "realtime WebSocket in-flight frame recovered after soft deadline opcode=0x%02x epoch=%lu elapsed_ms=%lld soft_budget_ms=%lu drain_budget_ms=%u secure=%d attempted=%d bytes=%u heartbeat_healthy=%d",
                 (unsigned)opcode,
                 (unsigned long)expected_epoch,
                 (long long)((operation_finished_us - operation_started_us) /
                             1000LL),
                 (unsigned long)effective_io_timeout_ms,
                 (unsigned)inflight_drain_budget_ms,
                 secure_transport ? 1 : 0,
                 transport_write_attempted ? 1 : 0,
                 (unsigned)frame_bytes_written,
                 heartbeat_healthy ? 1 : 0);
    }
    const bool disconnect =
        err != ESP_OK && client &&
        nuwax_ws_send_failure_requires_disconnect(
            err == ESP_ERR_TIMEOUT,
            frame_bytes_written,
            transport_write_attempted,
            secure_transport,
            true);
    if (disconnect) {
        ESP_LOGE(TAG,
                 "%s WebSocket frame failure requires reconnect opcode=0x%02x epoch=%lu err=%s elapsed_ms=%lld soft_budget_ms=%lu drain_budget_ms=%u secure=%d attempted=%d bytes=%u heartbeat_healthy=%d",
                 send_class,
                 (unsigned)opcode,
                 (unsigned long)expected_epoch,
                 esp_err_to_name(err),
                 (long long)((operation_finished_us - operation_started_us) /
                             1000LL),
                 (unsigned long)effective_io_timeout_ms,
                 inflight_drain_enabled
                     ? (unsigned)inflight_drain_budget_ms
                     : 0U,
                 secure_transport ? 1 : 0,
                 transport_write_attempted ? 1 : 0,
                 (unsigned)frame_bytes_written,
                 heartbeat_healthy ? 1 : 0);
        atomic_store_explicit(&client->connected, false,
                              memory_order_release);
        transport_interrupt_if_epoch(client, expected_epoch);
    } else if (err == ESP_ERR_TIMEOUT) {
        if (inflight_drain_enabled && opcode == 0x2U) {
            /* The realtime uplink aggregates these drops once per second and
             * reports exact frame totals at stream completion. Avoid duplicate
             * per-packet WARN formatting on the latency-sensitive TX path. */
            ESP_LOGD(TAG,
                     "unsent realtime binary frame expired before transport progress epoch=%lu secure=%d attempted=%d; connection retained",
                     (unsigned long)expected_epoch,
                     secure_transport ? 1 : 0,
                     transport_write_attempted ? 1 : 0);
        } else {
            ESP_LOGW(TAG,
                     "unsent %s WebSocket frame expired before unsafe transport progress opcode=0x%02x epoch=%lu secure=%d attempted=%d bytes=0 heartbeat_healthy=%d; connection retained",
                     inflight_drain_enabled ? "realtime" : "reliable",
                     (unsigned)opcode,
                     (unsigned long)expected_epoch,
                     secure_transport ? 1 : 0,
                     transport_write_attempted ? 1 : 0,
                     heartbeat_healthy ? 1 : 0);
        }
    }
    tx_stage_store(client, NUWAX_WS_TX_STAGE_IDLE, 0U);
    xSemaphoreGive(s_lifecycle_lock);
    return err;
}

static esp_err_t send_frame_until(nuwax_ws_client_t *client,
                                  uint8_t opcode,
                                  const uint8_t *payload,
                                  size_t len,
                                  int64_t deadline_us,
                                  uint32_t expected_epoch) {
    ESP_RETURN_ON_ERROR(ensure_lifecycle_lock(), TAG,
                        "create lifecycle lock failed");
    if (!client || deadline_us <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (expected_epoch == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return ESP_ERR_TIMEOUT;
    }
    uint64_t wait_ms = ((uint64_t)remaining_us + 999ULL) / 1000ULL;
    if (wait_ms > UINT32_MAX) {
        wait_ms = UINT32_MAX;
    }
    TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)wait_ms);
    if (wait_ticks == 0) {
        wait_ticks = 1;
    }
    tx_stage_store(client, NUWAX_WS_TX_STAGE_WAIT_LIFECYCLE,
                   expected_epoch);
    if (xSemaphoreTake(s_lifecycle_lock, wait_ticks) != pdTRUE) {
        tx_stage_store(client, NUWAX_WS_TX_STAGE_IDLE, 0U);
        return ESP_ERR_TIMEOUT;
    }
    if (nuwax_ws_connection_epoch(client) != expected_epoch) {
        tx_stage_store(client, NUWAX_WS_TX_STAGE_IDLE, 0U);
        xSemaphoreGive(s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (io_deadline_expired(deadline_us)) {
        tx_stage_store(client, NUWAX_WS_TX_STAGE_IDLE, 0U);
        xSemaphoreGive(s_lifecycle_lock);
        return ESP_ERR_TIMEOUT;
    }

    size_t frame_bytes_written = 0U;
    bool transport_write_attempted = false;
    const bool secure_transport = client->use_tls;
    const esp_err_t err =
        send_frame_unlocked(client, opcode, payload, len, deadline_us,
                            deadline_us, false,
                            &frame_bytes_written,
                            &transport_write_attempted);
    const bool disconnect =
        err != ESP_OK &&
        nuwax_ws_reliable_send_failure_requires_disconnect(
            err == ESP_ERR_TIMEOUT,
            frame_bytes_written,
            transport_write_attempted,
            secure_transport);
    if (disconnect) {
        ESP_LOGW(TAG,
                 "reliable WebSocket frame failure requires reconnect opcode=0x%02x epoch=%lu err=%s secure=%d attempted=%d bytes=%u",
                 (unsigned)opcode,
                 (unsigned long)expected_epoch,
                 esp_err_to_name(err),
                 secure_transport ? 1 : 0,
                 transport_write_attempted ? 1 : 0,
                 (unsigned)frame_bytes_written);
        atomic_store_explicit(&client->connected, false,
                              memory_order_release);
        transport_interrupt(client);
    } else if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG,
                 "reliable WebSocket frame deferred before transport progress opcode=0x%02x epoch=%lu secure=%d attempted=%d bytes=0; connection retained",
                 (unsigned)opcode,
                 (unsigned long)expected_epoch,
                 secure_transport ? 1 : 0,
                 transport_write_attempted ? 1 : 0);
    }
    tx_stage_store(client, NUWAX_WS_TX_STAGE_IDLE, 0U);
    xSemaphoreGive(s_lifecycle_lock);
    return err;
}

static esp_err_t send_frame(nuwax_ws_client_t *client,
                            uint8_t opcode,
                            const uint8_t *payload,
                            size_t len) {
    return send_frame_with_timeout(client,
                                   opcode,
                                   payload,
                                   len,
                                   NUWAX_WS_LIFECYCLE_LOCK_TIMEOUT_MS,
                                   NUWAX_WS_IO_TOTAL_TIMEOUT_MS,
                                   true,
                                   nuwax_ws_connection_epoch(client));
}

static void lifecycle_store(nuwax_ws_client_t *client,
                            nuwax_ws_lifecycle_state_t state) {
    if (client) {
        atomic_store_explicit(&client->lifecycle_state,
                              (int)state,
                              memory_order_release);
    }
}

static uint32_t advance_connection_epoch(nuwax_ws_client_t *client) {
    uint32_t epoch = (uint32_t)(atomic_fetch_add_explicit(
                                    &client->connection_epoch,
                                    1U,
                                    memory_order_acq_rel) +
                                1U);
    if (epoch == 0U) {
        epoch = 1U;
        atomic_store_explicit(&client->connection_epoch,
                              epoch,
                              memory_order_release);
    }
    return epoch;
}

static esp_err_t transport_close_serialized(nuwax_ws_client_t *client) {
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!transport_owns_any_handle(client)) {
        client->use_tls = false;
        return ESP_OK;
    }
    const esp_err_t lock_err = ensure_lifecycle_lock();
    if (lock_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "WebSocket owner could not create lifecycle lock; transport retained");
        return lock_err;
    }
    if (xSemaphoreTake(
            s_lifecycle_lock,
            pdMS_TO_TICKS(NUWAX_WS_LIFECYCLE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        const esp_err_t close_err = transport_close(client);
        xSemaphoreGive(s_lifecycle_lock);
        return close_err;
    }
    ESP_LOGE(TAG,
             "WebSocket owner timed out acquiring lifecycle lock; transport retained");
    return ESP_ERR_NOT_FINISHED;
}

static bool wait_for_rx_task_stop(nuwax_ws_client_t *client,
                                  TickType_t timeout_ticks) {
    if (!client) {
        return true;
    }

    TickType_t started = xTaskGetTickCount();
    /* RX publishes running=false only after it has finished touching client.
     * CLOSE_REQUESTED may legitimately overwrite RX_EXITED before the owner
     * joins, so the running flag is the join predicate. */
    while (atomic_load_explicit(&client->rx_task_running,
                                memory_order_acquire)) {
        if (timeout_ticks != portMAX_DELAY && (xTaskGetTickCount() - started) >= timeout_ticks) {
            ESP_LOGW(TAG,
                     "Timed out waiting for WebSocket rx task to stop running=%d state=%d",
                     atomic_load_explicit(&client->rx_task_running,
                                          memory_order_acquire)
                         ? 1
                         : 0,
                     (int)nuwax_ws_lifecycle_state(client));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static void websocket_send_close_code(nuwax_ws_client_t *client, uint16_t code) {
    uint8_t payload[2] = {
        (uint8_t)(code >> 8),
        (uint8_t)(code & 0xff),
    };
    (void)send_frame(client, 0x8, payload, sizeof(payload));
}

static bool websocket_opcode_supported(uint8_t opcode) {
    return opcode == 0x1 || opcode == 0x2 || opcode == 0x8 || opcode == 0x9 || opcode == 0xA;
}

static void websocket_close_reason_preview(const uint8_t *payload,
                                           size_t payload_size,
                                           char *preview,
                                           size_t preview_size) {
    if (!preview || preview_size == 0U) {
        return;
    }
    preview[0] = '\0';
    if (!payload || payload_size <= 2U) {
        return;
    }
    size_t reason_size = payload_size - 2U;
    if (reason_size >= preview_size) {
        reason_size = preview_size - 1U;
    }
    for (size_t i = 0; i < reason_size; ++i) {
        const uint8_t value = payload[i + 2U];
        preview[i] = value < 0x20U || value == 0x7fU
                         ? '?'
                         : (char)value;
    }
    preview[reason_size] = '\0';
}

static void websocket_receive_task(void *arg) {
    nuwax_ws_client_t *client = (nuwax_ws_client_t *)arg;
    /* xTaskCreateWithCaps() may schedule this higher-priority task before the
     * creator can publish rx_task and OPEN. Wait until those owner fields are
     * visible so an immediate peer close cannot make lifecycle state go
     * RX_EXITED -> OPEN. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int64_t last_rx_activity_us = esp_timer_get_time();
    int64_t last_heartbeat_us = last_rx_activity_us;
    const char *stop_reason = "connection_flag_cleared";
    size_t last_warned_stack_free = (size_t)-1;
    ESP_LOGI(TAG,
             "WebSocket receive task started stack=%d",
             CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES);
    while (transport_is_open(client)) {
        const int64_t rx_idle_deadline_us =
            last_rx_activity_us + (int64_t)NUWAX_WS_RX_IDLE_TIMEOUT_MS * 1000LL;
        const int64_t heartbeat_deadline_us =
            last_heartbeat_us + (int64_t)NUWAX_WS_HEARTBEAT_INTERVAL_MS * 1000LL;
        const int64_t next_deadline_us =
            heartbeat_deadline_us < rx_idle_deadline_us
                ? heartbeat_deadline_us
                : rx_idle_deadline_us;
        uint8_t head[2];
        esp_err_t first_byte_err = recv_exact_until(client, head, 1, next_deadline_us);
        if (first_byte_err != ESP_OK) {
            const int64_t now_us = esp_timer_get_time();
            if (first_byte_err == ESP_ERR_TIMEOUT && now_us < rx_idle_deadline_us &&
                now_us >= heartbeat_deadline_us) {
                static const uint8_t heartbeat_payload[] = "nuwax";
                esp_err_t ping_err = send_frame(client,
                                                0x9,
                                                heartbeat_payload,
                                                sizeof(heartbeat_payload) - 1U);
                if (ping_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "WebSocket keepalive ping failed: %s",
                             esp_err_to_name(ping_err));
                    stop_reason = "keepalive_ping_failed";
                    break;
                }
                last_heartbeat_us = now_us;
                ESP_LOGI(TAG, "WebSocket keepalive ping sent");
                continue;
            }
            if (first_byte_err == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG,
                         "WebSocket receive idle timeout elapsed_ms=%lld",
                         (long long)((now_us - last_rx_activity_us) / 1000LL));
                stop_reason = "rx_idle_timeout";
            } else {
                ESP_LOGW(TAG,
                         "WebSocket receive failed before frame header: %s",
                         esp_err_to_name(first_byte_err));
                stop_reason = "frame_header_read_failed";
            }
            break;
        }
        const int64_t frame_deadline_us = io_deadline_after_ms(NUWAX_WS_IO_TOTAL_TIMEOUT_MS);
        if (recv_exact_until(client, &head[1], 1, frame_deadline_us) != ESP_OK) {
            break;
        }

        const bool fin = (head[0] & 0x80U) != 0;
        const uint8_t rsv = head[0] & 0x70U;
        const uint8_t opcode = head[0] & 0x0fU;
        const bool masked = (head[1] & 0x80U) != 0;
        const uint8_t length_code = head[1] & 0x7fU;
        const bool control_frame = (opcode & 0x08U) != 0;
        if (!fin || rsv != 0 || masked || !websocket_opcode_supported(opcode) ||
            (control_frame && length_code > 125U)) {
            ESP_LOGW(TAG,
                     "WebSocket protocol error fin=%d rsv=0x%02x masked=%d opcode=0x%02x len_code=%u",
                     fin ? 1 : 0,
                     rsv,
                     masked ? 1 : 0,
                     opcode,
                     (unsigned)length_code);
            websocket_send_close_code(client, 1002);
            stop_reason = "peer_protocol_error";
            break;
        }

        uint64_t payload_len = length_code;
        if (length_code == 126U) {
            uint8_t ext[2];
            if (recv_exact_until(client, ext, sizeof(ext), frame_deadline_us) != ESP_OK) {
                break;
            }
            payload_len = ((uint64_t)ext[0] << 8) | ext[1];
            if (payload_len < 126U) {
                ESP_LOGW(TAG, "WebSocket non-canonical 16-bit payload length=%llu",
                         (unsigned long long)payload_len);
                websocket_send_close_code(client, 1002);
                break;
            }
        } else if (length_code == 127U) {
            uint8_t ext[8];
            if (recv_exact_until(client, ext, sizeof(ext), frame_deadline_us) != ESP_OK) {
                break;
            }
            if ((ext[0] & 0x80U) != 0) {
                ESP_LOGW(TAG, "WebSocket invalid 64-bit payload length high bit set");
                websocket_send_close_code(client, 1002);
                break;
            }
            payload_len = 0;
            for (int i = 0; i < 8; i++) {
                payload_len = (payload_len << 8) | ext[i];
            }
            if (payload_len <= 0xffffU) {
                ESP_LOGW(TAG, "WebSocket non-canonical 64-bit payload length=%llu",
                         (unsigned long long)payload_len);
                websocket_send_close_code(client, 1002);
                break;
            }
        }

        if (opcode == 0x8 && payload_len == 1U) {
            ESP_LOGW(TAG, "WebSocket close frame has invalid one-byte payload");
            websocket_send_close_code(client, 1002);
            stop_reason = "invalid_peer_close";
            break;
        }
        if (payload_len > NUWAX_WS_MAX_INCOMING_FRAME_BYTES) {
            ESP_LOGW(TAG, "Incoming WebSocket frame too large: %llu", (unsigned long long)payload_len);
            websocket_send_close_code(client, 1009);
            stop_reason = "incoming_frame_too_large";
            break;
        }

        const size_t payload_size = (size_t)payload_len;
        uint8_t *payload = client->rx_buffer;
        if (!payload || client->rx_buffer_capacity < payload_size + 1U) {
            ESP_LOGE(TAG,
                     "WebSocket RX buffer unavailable capacity=%u required=%u",
                     (unsigned)client->rx_buffer_capacity,
                     (unsigned)(payload_size + 1U));
            websocket_send_close_code(client, 1011);
            stop_reason = "rx_buffer_unavailable";
            break;
        }
        if (payload_size > 0 &&
            recv_exact_until(client, payload, payload_size, frame_deadline_us) != ESP_OK) {
            break;
        }
        payload[payload_size] = '\0';

        last_rx_activity_us = esp_timer_get_time();

        const int64_t callback_started_us = esp_timer_get_time();
        const char *callback_name = NULL;
        if (client->on_rx_activity) {
            client->on_rx_activity(client->user_ctx);
        }

        if (opcode == 0x1 && client->on_text) {
            callback_name = "text";
            client->on_text((const char *)payload, (int)payload_size, client->user_ctx);
        } else if (opcode == 0x2 && client->on_binary) {
            callback_name = "binary";
            client->on_binary(payload, (int)payload_size, client->user_ctx);
        } else if (opcode == 0x8) {
            const uint16_t close_code =
                payload_size >= 2U
                    ? (uint16_t)(((uint16_t)payload[0] << 8U) | payload[1])
                    : 0U;
            char close_reason[NUWAX_WS_CLOSE_REASON_PREVIEW_BYTES];
            websocket_close_reason_preview(payload,
                                           payload_size,
                                           close_reason,
                                           sizeof(close_reason));
            const size_t close_reason_bytes =
                payload_size > 2U ? payload_size - 2U : 0U;
            if (close_code == 1000U || close_code == 1001U) {
                ESP_LOGI(TAG,
                         "WebSocket peer close frame received code=%u reason_bytes=%u reason=%s",
                         (unsigned)close_code,
                         (unsigned)close_reason_bytes,
                         close_reason[0] ? close_reason : "-");
            } else {
                ESP_LOGW(TAG,
                         "WebSocket peer close frame received code=%u payload_bytes=%u reason_bytes=%u reason=%s",
                         (unsigned)close_code,
                         (unsigned)payload_size,
                         (unsigned)close_reason_bytes,
                         close_reason[0] ? close_reason : "-");
            }
            if (atomic_load_explicit(&client->connected,
                                     memory_order_acquire)) {
                (void)send_frame(client, 0x8, payload, payload_size);
            }
            stop_reason = "peer_close";
            break;
        } else if (opcode == 0x9) {
            ESP_LOGD(TAG, "WebSocket ping received len=%u", (unsigned)payload_size);
            const esp_err_t pong_err =
                send_frame(client, 0xA, payload, payload_size);
            if (pong_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "WebSocket pong reply failed: %s",
                         esp_err_to_name(pong_err));
                stop_reason = "pong_reply_failed";
                break;
            }
        } else if (opcode == 0xA) {
            nuwax_ws_note_heartbeat_if_epoch(
                client, nuwax_ws_connection_epoch(client));
            ESP_LOGI(TAG, "WebSocket keepalive pong received len=%u", (unsigned)payload_size);
        }
        const int64_t callback_finished_us = esp_timer_get_time();
        last_rx_activity_us = callback_finished_us;
        if (callback_name) {
            const size_t stack_min_free =
                (size_t)uxTaskGetStackHighWaterMark(NULL);
            if (stack_min_free < NUWAX_WS_RX_STACK_WARN_BYTES &&
                (last_warned_stack_free == (size_t)-1 ||
                 stack_min_free + NUWAX_WS_RX_STACK_WARN_STEP_BYTES <=
                     last_warned_stack_free)) {
                ESP_LOGW(TAG,
                         "WebSocket RX stack reserve low type=%s stack_min_free=%u configured=%u",
                         callback_name,
                         (unsigned)stack_min_free,
                         (unsigned)CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES);
                last_warned_stack_free = stack_min_free;
            }
            const int64_t callback_elapsed_us =
                callback_finished_us - callback_started_us;
            if (callback_elapsed_us >=
                (int64_t)NUWAX_WS_RX_CALLBACK_WARN_MS * 1000LL) {
                ESP_LOGW(TAG,
                         "WebSocket RX callback slow type=%s payload_bytes=%u elapsed_us=%lld",
                         callback_name,
                         (unsigned)payload_size,
                         (long long)callback_elapsed_us);
            }
        }
        if (!transport_is_open(client)) {
            stop_reason = "callback_or_sender_closed_connection";
        }
    }

    atomic_store_explicit(&client->connected, false, memory_order_release);
    ESP_LOGI(TAG, "WebSocket receive task stopped reason=%s", stop_reason);
    lifecycle_store(client, NUWAX_WS_LIFECYCLE_RX_EXITED);
    /* Publish running=false last. The owner may delete this suspended task as
     * soon as it observes false, so the RX task must not touch client again. */
    atomic_store_explicit(&client->rx_task_running, false,
                          memory_order_release);
    vTaskSuspend(NULL);
}

static esp_err_t cleanup_rx_task(nuwax_ws_client_t *client) {
    if (!client || !client->rx_task) {
        if (client) {
            atomic_store_explicit(&client->rx_task_running, false,
                                  memory_order_release);
        }
        return client ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    TaskHandle_t rx_task = (TaskHandle_t)client->rx_task;
    if (rx_task == xTaskGetCurrentTaskHandle()) {
        ESP_LOGW(TAG, "WebSocket rx task cleanup requested from rx task context");
        return ESP_ERR_INVALID_STATE;
    }

    if (!wait_for_rx_task_stop(
            client, pdMS_TO_TICKS(NUWAX_WS_RX_TASK_STOP_TIMEOUT_MS))) {
        ESP_LOGE(TAG,
                 "WebSocket RX did not exit after shutdown; retaining task and transport");
        return ESP_ERR_TIMEOUT;
    }
    vTaskDeleteWithCaps(rx_task);
    client->rx_task = NULL;
    atomic_store_explicit(&client->rx_task_running, false,
                          memory_order_release);
    return ESP_OK;
}

static void owner_close_status_store(nuwax_ws_client_t *client,
                                     nuwax_ws_close_failure_stage_t stage,
                                     esp_err_t result) {
    if (!client) {
        return;
    }
    atomic_store_explicit(&client->close_failure_stage,
                          (int)stage,
                          memory_order_release);
    atomic_store_explicit(&client->last_close_result,
                          (int)result,
                          memory_order_release);
}

static esp_err_t close_failed_connect_transport(nuwax_ws_client_t *client,
                                                esp_err_t connect_result) {
    const esp_err_t close_err = transport_close_serialized(client);
    if (close_err != ESP_OK) {
        owner_close_status_store(client,
                                 NUWAX_WS_CLOSE_FAILURE_TRANSPORT_LOCK,
                                 close_err);
        lifecycle_store(client, NUWAX_WS_LIFECYCLE_FAULTED);
        ESP_LOGE(TAG,
                 "WebSocket failed-connect cleanup incomplete connect=%s close=%s; owner must retain and retry client",
                 esp_err_to_name(connect_result),
                 esp_err_to_name(close_err));
        return close_err;
    }
    release_rx_buffer(client);
    owner_close_status_store(client, NUWAX_WS_CLOSE_FAILURE_NONE, ESP_OK);
    lifecycle_store(client, NUWAX_WS_LIFECYCLE_IDLE);
    return connect_result;
}

void nuwax_ws_init(nuwax_ws_client_t *client) {
    if (!client) {
        return;
    }
    memset(client, 0, sizeof(*client));
    atomic_init(&client->connected, false);
    atomic_init(&client->rx_task_running, false);
    atomic_init(&client->lifecycle_state, NUWAX_WS_LIFECYCLE_IDLE);
    atomic_init(&client->close_failure_stage,
                NUWAX_WS_CLOSE_FAILURE_NONE);
    atomic_init(&client->last_close_result, ESP_OK);
    atomic_init(&client->connection_epoch, 0U);
    atomic_init(&client->last_heartbeat_epoch, 0U);
    atomic_init(&client->last_heartbeat_ms, 0U);
    atomic_init(&client->close_requested_epoch, 0U);
    atomic_init(&client->tx_stage, NUWAX_WS_TX_STAGE_IDLE);
    atomic_init(&client->tx_stage_epoch, 0U);
    atomic_init(&client->tx_stage_since_ms, 0U);
    client->sock = -1;
}

esp_err_t nuwax_ws_connect(nuwax_ws_client_t *client, const nuwax_ws_config_t *config) {
    if (!client || !config || !config->url || !config->token ||
        !config->token[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_lifecycle_lock(), TAG, "create WebSocket lifecycle lock failed");
    ESP_LOGI(TAG,
             "WebSocket connect begin free_internal=%u largest_internal=%u free_psram=%u largest_psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    parsed_ws_url_t url;
    ESP_RETURN_ON_ERROR(parse_ws_url(config->url, &url), TAG, "invalid WebSocket URL");

    const esp_err_t previous_close_err = nuwax_ws_owner_close(client);
    if (previous_close_err != ESP_OK) {
        lifecycle_store(client, NUWAX_WS_LIFECYCLE_FAULTED);
        ESP_LOGE(TAG,
                 "WebSocket previous transport cleanup failed: %s; refusing handle reuse",
                 esp_err_to_name(previous_close_err));
        return previous_close_err;
    }
    if (!nuwax_ws_owner_resources_released(client)) {
        lifecycle_store(client, NUWAX_WS_LIFECYCLE_FAULTED);
        owner_close_status_store(client,
                                 NUWAX_WS_CLOSE_FAILURE_TRANSPORT_LOCK,
                                 ESP_ERR_INVALID_STATE);
        ESP_LOGE(TAG,
                 "WebSocket previous transport still owns resources; refusing handle reuse");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(
            s_lifecycle_lock,
            pdMS_TO_TICKS(NUWAX_WS_LIFECYCLE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        lifecycle_store(client, NUWAX_WS_LIFECYCLE_FAULTED);
        owner_close_status_store(client,
                                 NUWAX_WS_CLOSE_FAILURE_TRANSPORT_LOCK,
                                 ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    const uint32_t connection_epoch = advance_connection_epoch(client);
    atomic_store_explicit(&client->last_heartbeat_ms,
                          0U,
                          memory_order_relaxed);
    atomic_store_explicit(&client->last_heartbeat_epoch,
                          0U,
                          memory_order_release);
    atomic_store_explicit(&client->close_requested_epoch,
                          0U,
                          memory_order_release);
    lifecycle_store(client, NUWAX_WS_LIFECYCLE_CONNECTING);
    atomic_store_explicit(&client->connected, false, memory_order_release);
    xSemaphoreGive(s_lifecycle_lock);
    client->last_http_status = 0;

    esp_err_t err = ensure_io_lock(client);
    if (err != ESP_OK) {
        return close_failed_connect_transport(client, err);
    }
    if (url.secure) {
        err = connect_tls(client, &url);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TLS connect failed: %s", esp_err_to_name(err));
        }
    } else {
        int sock = connect_tcp(&url);
        if (sock < 0) {
            ESP_LOGE(TAG, "TCP connect failed to %s:%d", url.host, url.port);
            err = ESP_FAIL;
        } else {
            SemaphoreHandle_t io_lock =
                (SemaphoreHandle_t)client->io_lock;
            const esp_err_t lock_err = take_io_lock_with_timeout(
                client, NUWAX_WS_HANDLE_LOCK_TIMEOUT_MS);
            if (lock_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "TCP handle publication could not acquire I/O lock: %s",
                         esp_err_to_name(lock_err));
                close(sock);
                err = lock_err;
            } else {
                client->sock = sock;
                client->use_tls = false;
                xSemaphoreGive(io_lock);
            }
        }
    }
    if (err != ESP_OK) {
        return close_failed_connect_transport(client, err);
    }

    err = websocket_handshake(client, &url, config->token);
    if (err != ESP_OK) {
        return close_failed_connect_transport(client, err);
    }
    err = configure_socket_nonblocking(client->sock);
    if (err != ESP_OK) {
        return close_failed_connect_transport(client, err);
    }
    ESP_LOGI(TAG,
             "WebSocket handshake ok free_internal=%u largest_internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    err = ensure_rx_buffer(client);
    if (err != ESP_OK) {
        return close_failed_connect_transport(client, err);
    }

    if (config->on_handshake_verified) {
        config->on_handshake_verified(config->user_ctx);
    }

    atomic_store_explicit(&client->connected, true, memory_order_release);
    client->on_text = config->on_text;
    client->on_binary = config->on_binary;
    client->on_rx_activity = config->on_rx_activity;
    client->user_ctx = config->user_ctx;

    TaskHandle_t rx_task = NULL;
    atomic_store_explicit(&client->rx_task_running, true,
                          memory_order_release);
    BaseType_t task_ok = xTaskCreateWithCaps(websocket_receive_task,
                                             "nuwax_ws_rx",
                                             CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES,
                                             client,
                                             NUWAX_WS_RX_TASK_PRIORITY,
                                             &rx_task,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS || !rx_task) {
        ESP_LOGE(TAG, "WebSocket rx task create failed stack_bytes=%d", CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES);
        atomic_store_explicit(&client->rx_task_running, false,
                              memory_order_release);
        atomic_store_explicit(&client->connected, false,
                              memory_order_release);
        return close_failed_connect_transport(client, ESP_ERR_NO_MEM);
    }
    client->rx_task = rx_task;
    lifecycle_store(client, NUWAX_WS_LIFECYCLE_OPEN);
    xTaskNotifyGive(rx_task);

    ESP_LOGI(TAG,
             "%s WebSocket connected to %s epoch=%lu",
             url.secure ? "Secure" : "Plain",
             config->url,
             (unsigned long)connection_epoch);
    return ESP_OK;
}

esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *client, const char *payload) {
    if (!payload) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_frame(client, 0x1, (const uint8_t *)payload, strlen(payload));
}

esp_err_t nuwax_ws_send_text_until(nuwax_ws_client_t *client,
                                   const char *payload,
                                   int64_t deadline_us) {
    if (!client || !payload || deadline_us <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_frame_until(client,
                            0x1,
                            (const uint8_t *)payload,
                            strlen(payload),
                            deadline_us,
                            nuwax_ws_connection_epoch(client));
}

esp_err_t nuwax_ws_send_text_until_if_epoch(nuwax_ws_client_t *client,
                                            const char *payload,
                                            int64_t deadline_us,
                                            uint32_t expected_epoch) {
    if (!client || !payload || deadline_us <= 0 || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_frame_until(client,
                            0x1,
                            (const uint8_t *)payload,
                            strlen(payload),
                            deadline_us,
                            expected_epoch);
}

esp_err_t nuwax_ws_send_text_with_timeout(nuwax_ws_client_t *client,
                                          const char *payload,
                                          uint32_t timeout_ms) {
    if (!client || !payload || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return nuwax_ws_send_text_until(client,
                                    payload,
                                    io_deadline_after_ms(timeout_ms));
}

esp_err_t nuwax_ws_send_text_with_timeout_if_epoch(
    nuwax_ws_client_t *client,
    const char *payload,
    uint32_t timeout_ms,
    uint32_t expected_epoch) {
    if (!client || !payload || timeout_ms == 0U || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Interactive control frames share one absolute lock/I/O budget. If TLS
     * has already accepted unsafe progress, the transport gets the existing
     * bounded drain window before reconnect is considered. */
    return send_frame_with_timeout(client,
                                   0x1,
                                   (const uint8_t *)payload,
                                   strlen(payload),
                                   timeout_ms,
                                   timeout_ms,
                                   false,
                                   expected_epoch);
}

esp_err_t nuwax_ws_send_binary(nuwax_ws_client_t *client, const uint8_t *payload, size_t len) {
    if (!payload) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_frame(client, 0x2, payload, len);
}

esp_err_t nuwax_ws_send_binary_if_epoch(nuwax_ws_client_t *client,
                                        const uint8_t *payload,
                                        size_t len,
                                        uint32_t expected_epoch) {
    if (!client || !payload || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_frame_with_timeout(client,
                                   0x2,
                                   payload,
                                   len,
                                   NUWAX_WS_LIFECYCLE_LOCK_TIMEOUT_MS,
                                   NUWAX_WS_IO_TOTAL_TIMEOUT_MS,
                                   true,
                                   expected_epoch);
}

esp_err_t nuwax_ws_send_binary_with_timeout(nuwax_ws_client_t *client,
                                            const uint8_t *payload,
                                            size_t len,
                                            uint32_t timeout_ms) {
    if (!payload || timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_frame_with_timeout(client,
                                   0x2,
                                   payload,
                                   len,
                                   timeout_ms,
                                   timeout_ms,
                                   false,
                                   nuwax_ws_connection_epoch(client));
}

esp_err_t nuwax_ws_send_binary_with_timeout_if_epoch(
    nuwax_ws_client_t *client,
    const uint8_t *payload,
    size_t len,
    uint32_t timeout_ms,
    uint32_t expected_epoch) {
    if (!client || !payload || timeout_ms == 0U || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_frame_with_timeout(client,
                                   0x2,
                                   payload,
                                   len,
                                   timeout_ms,
                                   timeout_ms,
                                   false,
                                   expected_epoch);
}

esp_err_t nuwax_ws_send_ping(nuwax_ws_client_t *client) {
    const char payload[] = "nuwax";
    return send_frame(client, 0x9, (const uint8_t *)payload, sizeof(payload) - 1);
}

void nuwax_ws_note_heartbeat_if_epoch(nuwax_ws_client_t *client,
                                      uint32_t expected_epoch) {
    if (!client || expected_epoch == 0U ||
        nuwax_ws_connection_epoch(client) != expected_epoch ||
        !nuwax_ws_is_connected(client)) {
        return;
    }
    const uint32_t now_ms =
        (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
    atomic_store_explicit(&client->last_heartbeat_ms,
                          now_ms,
                          memory_order_relaxed);
    atomic_store_explicit(&client->last_heartbeat_epoch,
                          expected_epoch,
                          memory_order_release);
}

bool nuwax_ws_heartbeat_healthy_if_epoch(const nuwax_ws_client_t *client,
                                         uint32_t expected_epoch) {
    if (!client || expected_epoch == 0U ||
        nuwax_ws_connection_epoch(client) != expected_epoch ||
        !nuwax_ws_is_connected(client)) {
        return false;
    }
    const uint32_t heartbeat_epoch = (uint32_t)atomic_load_explicit(
        &client->last_heartbeat_epoch, memory_order_acquire);
    if (heartbeat_epoch != expected_epoch) {
        return false;
    }
    const uint32_t last_ms = (uint32_t)atomic_load_explicit(
        &client->last_heartbeat_ms, memory_order_relaxed);
    const uint32_t now_ms =
        (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
    return (uint32_t)(now_ms - last_ms) <= NUWAX_WS_HEARTBEAT_HEALTHY_MS;
}

uint32_t nuwax_ws_heartbeat_remaining_ms_if_epoch(
    const nuwax_ws_client_t *client,
    uint32_t expected_epoch) {
    if (!client || expected_epoch == 0U ||
        nuwax_ws_connection_epoch(client) != expected_epoch ||
        !nuwax_ws_is_connected(client)) {
        return 0U;
    }
    const uint32_t heartbeat_epoch = (uint32_t)atomic_load_explicit(
        &client->last_heartbeat_epoch, memory_order_acquire);
    if (heartbeat_epoch != expected_epoch) {
        return 0U;
    }
    const uint32_t last_ms = (uint32_t)atomic_load_explicit(
        &client->last_heartbeat_ms, memory_order_relaxed);
    const uint32_t now_ms =
        (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
    const uint32_t age_ms = (uint32_t)(now_ms - last_ms);
    return age_ms < NUWAX_WS_HEARTBEAT_HEALTHY_MS
               ? NUWAX_WS_HEARTBEAT_HEALTHY_MS - age_ms
               : 0U;
}

esp_err_t nuwax_ws_owner_close(nuwax_ws_client_t *client) {
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t epoch = nuwax_ws_connection_epoch(client);
    /* A close request has already passed the epoch/heartbeat admission gate
     * in request_close_if_epoch().  Once that mailbox is set, the owner must
     * be allowed to join RX and release the transport even if a late Pong
     * arrives before cleanup.  Without this check, an admitted ownership or
     * transport close could be rejected by a stale healthy-heartbeat sample,
     * leaving the client permanently non-reusable. */
    if (nuwax_ws_heartbeat_healthy_if_epoch(client, epoch) &&
        !close_requested_for_current_epoch(client)) {
        ESP_LOGE(TAG,
                 "WebSocket owner close rejected while heartbeat is healthy epoch=%lu; required or ownership close admission is required",
                 (unsigned long)epoch);
        return ESP_ERR_INVALID_STATE;
    }
    owner_close_status_store(client,
                             NUWAX_WS_CLOSE_FAILURE_NONE,
                             ESP_ERR_NOT_FINISHED);
    lifecycle_store(client, NUWAX_WS_LIFECYCLE_CLOSE_REQUESTED);
    atomic_store_explicit(&client->connected, false, memory_order_release);
    transport_interrupt(client);
    const esp_err_t join_err = cleanup_rx_task(client);
    if (join_err != ESP_OK) {
        owner_close_status_store(client,
                                 NUWAX_WS_CLOSE_FAILURE_RX_JOIN,
                                 join_err);
        lifecycle_store(client, NUWAX_WS_LIFECYCLE_FAULTED);
        return join_err;
    }
    const esp_err_t transport_err = transport_close_serialized(client);
    if (transport_err != ESP_OK) {
        owner_close_status_store(client,
                                 NUWAX_WS_CLOSE_FAILURE_TRANSPORT_LOCK,
                                 transport_err);
        lifecycle_store(client, NUWAX_WS_LIFECYCLE_FAULTED);
        return transport_err;
    }
    release_rx_buffer(client);
    client->on_text = NULL;
    client->on_binary = NULL;
    client->on_rx_activity = NULL;
    client->user_ctx = NULL;
    owner_close_status_store(client, NUWAX_WS_CLOSE_FAILURE_NONE, ESP_OK);
    lifecycle_store(client, NUWAX_WS_LIFECYCLE_IDLE);
    atomic_store_explicit(&client->close_requested_epoch,
                          0U,
                          memory_order_release);
    return ESP_OK;
}

void nuwax_ws_close(nuwax_ws_client_t *client) {
    const esp_err_t err = nuwax_ws_owner_close(client);
    if (err != ESP_OK && client) {
        ESP_LOGE(TAG,
                 "Compatibility WebSocket close incomplete stage=%d result=%s; client storage must be retained",
                 (int)nuwax_ws_owner_close_failure_stage(client),
                 esp_err_to_name(err));
    }
}

void nuwax_ws_request_close(nuwax_ws_client_t *client) {
    if (!client) {
        return;
    }
    const uint32_t epoch = nuwax_ws_connection_epoch(client);
    (void)nuwax_ws_advisory_close_if_epoch(client, epoch);
}

static bool request_close_if_epoch(nuwax_ws_client_t *client,
                                   uint32_t expected_epoch,
                                   nuwax_ws_close_cause_t cause) {
    if (!client || expected_epoch == 0U || (int)cause < 0 ||
        cause > NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER) {
        return false;
    }
    const bool heartbeat_healthy =
        nuwax_ws_heartbeat_healthy_if_epoch(client, expected_epoch);
    if (!nuwax_ws_close_policy_admits(cause, heartbeat_healthy)) {
        const uint32_t last_ms = (uint32_t)atomic_load_explicit(
            &client->last_heartbeat_ms, memory_order_relaxed);
        const uint32_t now_ms =
            (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
        ESP_LOGW(TAG,
                 "WebSocket close suppressed while heartbeat is healthy epoch=%lu heartbeat_age_ms=%lu cause=%s",
                 (unsigned long)expected_epoch,
                 (unsigned long)(uint32_t)(now_ms - last_ms),
                 nuwax_ws_close_cause_name(cause));
        return false;
    }

    uint_fast32_t observed = atomic_load_explicit(
        &client->close_requested_epoch, memory_order_acquire);
    for (;;) {
        const uint32_t current_epoch = nuwax_ws_connection_epoch(client);
        if (current_epoch != expected_epoch) {
            ESP_LOGI(TAG,
                     "stale WebSocket close ignored expected_epoch=%lu current_epoch=%lu",
                     (unsigned long)expected_epoch,
                     (unsigned long)current_epoch);
            return false;
        }
        if ((uint32_t)observed == expected_epoch ||
            atomic_compare_exchange_weak_explicit(
                &client->close_requested_epoch,
                &observed,
                expected_epoch,
                memory_order_acq_rel,
                memory_order_acquire)) {
            break;
        }
    }
    if (nuwax_ws_connection_epoch(client) != expected_epoch) {
        return false;
    }
    lifecycle_store(client, NUWAX_WS_LIFECYCLE_CLOSE_REQUESTED);
    transport_interrupt_if_epoch(client, expected_epoch);
    ESP_LOGW(TAG,
             "WebSocket close admitted epoch=%lu cause=%s heartbeat_healthy=%d",
             (unsigned long)expected_epoch,
             nuwax_ws_close_cause_name(cause),
             heartbeat_healthy ? 1 : 0);
    return true;
}

bool nuwax_ws_advisory_close_if_epoch(nuwax_ws_client_t *client,
                                      uint32_t expected_epoch) {
    return request_close_if_epoch(client,
                                  expected_epoch,
                                  NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY);
}

bool nuwax_ws_request_close_if_epoch(nuwax_ws_client_t *client,
                                     uint32_t expected_epoch) {
    return request_close_if_epoch(client,
                                  expected_epoch,
                                  NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY);
}

bool nuwax_ws_close_if_epoch(nuwax_ws_client_t *client,
                             uint32_t expected_epoch,
                             nuwax_ws_close_cause_t cause) {
    return request_close_if_epoch(client, expected_epoch, cause);
}

bool nuwax_ws_force_close_if_epoch(nuwax_ws_client_t *client,
                                   uint32_t expected_epoch) {
    return request_close_if_epoch(client,
                                  expected_epoch,
                                  NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER);
}

uint32_t nuwax_ws_connection_epoch(const nuwax_ws_client_t *client) {
    return client
               ? (uint32_t)atomic_load_explicit(&client->connection_epoch,
                                                 memory_order_acquire)
               : 0U;
}

bool nuwax_ws_owner_resources_released(const nuwax_ws_client_t *client) {
    return client && client->rx_task == NULL &&
           !atomic_load_explicit(&client->rx_task_running,
                                 memory_order_acquire) &&
           client->rx_buffer == NULL &&
           client->rx_buffer_capacity == 0U &&
           !transport_owns_any_handle(client) &&
           nuwax_ws_lifecycle_state(client) == NUWAX_WS_LIFECYCLE_IDLE;
}

nuwax_ws_tx_diagnostics_t nuwax_ws_tx_diagnostics(
    const nuwax_ws_client_t *client) {
    nuwax_ws_tx_diagnostics_t diagnostics = {
        .stage = NUWAX_WS_TX_STAGE_IDLE,
    };
    if (!client) {
        return diagnostics;
    }
    diagnostics.stage = (nuwax_ws_tx_stage_t)atomic_load_explicit(
        &client->tx_stage, memory_order_acquire);
    diagnostics.epoch = (uint32_t)atomic_load_explicit(
        &client->tx_stage_epoch, memory_order_relaxed);
    const uint32_t since_ms = (uint32_t)atomic_load_explicit(
        &client->tx_stage_since_ms, memory_order_relaxed);
    const uint32_t now_ms =
        (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
    diagnostics.stage_age_ms = diagnostics.stage == NUWAX_WS_TX_STAGE_IDLE
                                   ? 0U
                                   : (uint32_t)(now_ms - since_ms);
    return diagnostics;
}

nuwax_ws_close_failure_stage_t nuwax_ws_owner_close_failure_stage(
    const nuwax_ws_client_t *client) {
    if (!client) {
        return NUWAX_WS_CLOSE_FAILURE_NONE;
    }
    const int stage = atomic_load_explicit(&client->close_failure_stage,
                                           memory_order_acquire);
    return stage >= NUWAX_WS_CLOSE_FAILURE_NONE &&
                   stage <= NUWAX_WS_CLOSE_FAILURE_TRANSPORT_LOCK
               ? (nuwax_ws_close_failure_stage_t)stage
               : NUWAX_WS_CLOSE_FAILURE_TRANSPORT_LOCK;
}

esp_err_t nuwax_ws_owner_last_close_result(const nuwax_ws_client_t *client) {
    return client
               ? (esp_err_t)atomic_load_explicit(&client->last_close_result,
                                                  memory_order_acquire)
               : ESP_ERR_INVALID_ARG;
}

bool nuwax_ws_is_connected(const nuwax_ws_client_t *client) {
    return transport_is_open(client);
}

int nuwax_ws_last_http_status(const nuwax_ws_client_t *client) {
    return client ? client->last_http_status : 0;
}

nuwax_ws_lifecycle_state_t nuwax_ws_lifecycle_state(
    const nuwax_ws_client_t *client) {
    if (!client) {
        return NUWAX_WS_LIFECYCLE_FAULTED;
    }
    const int state = atomic_load_explicit(&client->lifecycle_state,
                                           memory_order_acquire);
    return state >= NUWAX_WS_LIFECYCLE_IDLE &&
                   state <= NUWAX_WS_LIFECYCLE_FAULTED
               ? (nuwax_ws_lifecycle_state_t)state
               : NUWAX_WS_LIFECYCLE_FAULTED;
}
