#include "ws_transport.h"

#include <errno.h>
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
#include "sdkconfig.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "nuwax_ws";
#define NUWAX_WS_RX_TASK_PRIORITY 10U
static SemaphoreHandle_t s_lifecycle_lock;

#define NUWAX_WS_SOCKET_TIMEOUT_MS 3000
#define NUWAX_WS_IO_TOTAL_TIMEOUT_MS 4000
#define NUWAX_WS_RX_IDLE_TIMEOUT_MS 60000
#define NUWAX_WS_HANDSHAKE_TIMEOUT_MS 8000
#define NUWAX_WS_LIFECYCLE_LOCK_TIMEOUT_MS 5000
#define NUWAX_WS_RX_TASK_STOP_TIMEOUT_MS 5000
#define NUWAX_WS_MAX_INCOMING_FRAME_BYTES (16U * 1024U)

#ifndef CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES
#define CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES 12288
#endif

#if CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN > 1024
#define NUWAX_WS_SEND_CHUNK_BYTES (CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN - 128)
#else
#define NUWAX_WS_SEND_CHUNK_BYTES 1024
#endif

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

static bool transport_is_open(const nuwax_ws_client_t *client) {
    return client && client->connected && transport_has_handle(client);
}

static ssize_t transport_write(nuwax_ws_client_t *client, const uint8_t *data, size_t len) {
    if (!transport_has_handle(client)) {
        return -1;
    }
    if (client->use_tls) {
        return esp_tls_conn_write(client->tls, data, len);
    }
    return send(client->sock, data, len, 0);
}

static ssize_t transport_read(nuwax_ws_client_t *client, uint8_t *data, size_t len) {
    if (!transport_has_handle(client)) {
        return -1;
    }
    if (client->use_tls) {
        return esp_tls_conn_read(client->tls, data, len);
    }
    return recv(client->sock, data, len, 0);
}

static void transport_close(nuwax_ws_client_t *client) {
    if (!client) {
        return;
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
}

static void transport_interrupt(nuwax_ws_client_t *client) {
    if (client && client->sock >= 0) {
        (void)shutdown(client->sock, SHUT_RDWR);
    }
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
}

static bool io_retryable(const nuwax_ws_client_t *client, ssize_t ret) {
    if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return true;
    }
    if (client && client->use_tls && ret == ESP_TLS_ERR_SSL_TIMEOUT) {
        return true;
    }
    return client && !client->use_tls && ret < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
}

static int64_t io_deadline_after_ms(uint32_t timeout_ms) {
    return esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
}

static bool io_deadline_expired(int64_t deadline_us) {
    return esp_timer_get_time() >= deadline_us;
}

static esp_err_t send_all_until(nuwax_ws_client_t *client,
                                const uint8_t *data,
                                size_t len,
                                int64_t deadline_us) {
    size_t sent = 0;
    while (sent < len) {
        if (io_deadline_expired(deadline_us)) {
            return ESP_ERR_TIMEOUT;
        }
        ssize_t ret = transport_write(client, data + sent, len - sent);
        if (io_retryable(client, ret)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        sent += (size_t)ret;
    }
    return ESP_OK;
}

static esp_err_t send_all(nuwax_ws_client_t *client, const uint8_t *data, size_t len) {
    return send_all_until(client, data, len, io_deadline_after_ms(NUWAX_WS_IO_TOTAL_TIMEOUT_MS));
}

static esp_err_t recv_exact_until(nuwax_ws_client_t *client,
                                  uint8_t *data,
                                  size_t len,
                                  int64_t deadline_us) {
    size_t received = 0;
    while (received < len) {
        if (io_deadline_expired(deadline_us)) {
            return ESP_ERR_TIMEOUT;
        }
        ssize_t ret = transport_read(client, data + received, len - received);
        if (io_retryable(client, ret)) {
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

    client->tls = tls;
    client->sock = -1;
    int sock = -1;
    if (esp_tls_get_conn_sockfd(tls, &sock) == ESP_OK && sock >= 0) {
        client->sock = sock;
        configure_socket_timeouts(sock);
    } else {
        ESP_LOGW(TAG, "TLS socket fd unavailable; close interrupt will rely on I/O timeout");
    }
    client->use_tls = true;
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
    memset(request, 0, sizeof(request));
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
        ESP_LOGE(TAG, "WebSocket handshake rejected status_line_len=%u", (unsigned)status_line_len);
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

static esp_err_t send_frame_unlocked(nuwax_ws_client_t *client, uint8_t opcode, const uint8_t *payload, size_t len) {
    if (!transport_is_open(client)) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool control_frame = (opcode & 0x08U) != 0;
    if ((opcode != 0x1 && opcode != 0x2 && opcode != 0x8 && opcode != 0x9 && opcode != 0xA) ||
        (control_frame && len > 125U) ||
        (len > 0 && !payload)) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t send_deadline_us = io_deadline_after_ms(NUWAX_WS_IO_TOTAL_TIMEOUT_MS);

    uint8_t header[14];
    size_t header_len = 0;
    header[header_len++] = 0x80 | opcode;
    if (len < 126) {
        header[header_len++] = 0x80 | (uint8_t)len;
    } else if (len <= 0xffff) {
        header[header_len++] = 0x80 | 126;
        header[header_len++] = (uint8_t)((len >> 8) & 0xff);
        header[header_len++] = (uint8_t)(len & 0xff);
    } else {
        header[header_len++] = 0x80 | 127;
        uint64_t big_len = (uint64_t)len;
        for (int shift = 56; shift >= 0; shift -= 8) {
            header[header_len++] = (uint8_t)((big_len >> shift) & 0xff);
        }
    }

    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    memcpy(&header[header_len], mask, sizeof(mask));
    header_len += sizeof(mask);

    ESP_RETURN_ON_ERROR(send_all_until(client, header, header_len, send_deadline_us),
                        TAG,
                        "frame header send failed");
    uint8_t chunk[NUWAX_WS_SEND_CHUNK_BYTES];
    size_t offset = 0;
    while (offset < len) {
        size_t chunk_len = len - offset;
        if (chunk_len > sizeof(chunk)) {
            chunk_len = sizeof(chunk);
        }
        for (size_t i = 0; i < chunk_len; i++) {
            chunk[i] = payload[offset + i] ^ mask[(offset + i) % 4];
        }
        ESP_RETURN_ON_ERROR(send_all_until(client, chunk, chunk_len, send_deadline_us),
                            TAG,
                            "frame payload send failed");
        offset += chunk_len;
    }
    return ESP_OK;
}

static esp_err_t send_frame(nuwax_ws_client_t *client, uint8_t opcode, const uint8_t *payload, size_t len) {
    ESP_RETURN_ON_ERROR(ensure_lifecycle_lock(), TAG, "create lifecycle lock failed");
    if (xSemaphoreTake(s_lifecycle_lock,
                       pdMS_TO_TICKS(NUWAX_WS_LIFECYCLE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        if (client) {
            client->connected = false;
            transport_interrupt(client);
        }
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = send_frame_unlocked(client, opcode, payload, len);
    if (err != ESP_OK && client) {
        client->connected = false;
        transport_interrupt(client);
    }
    xSemaphoreGive(s_lifecycle_lock);
    return err;
}

static void transport_close_serialized(nuwax_ws_client_t *client) {
    if (!client) {
        return;
    }
    if (ensure_lifecycle_lock() != ESP_OK) {
        transport_close(client);
        return;
    }
    if (xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY) == pdTRUE) {
        transport_close(client);
        xSemaphoreGive(s_lifecycle_lock);
    }
}

static void wait_for_rx_task_stop(nuwax_ws_client_t *client, TickType_t timeout_ticks) {
    if (!client || !client->rx_task_running) {
        return;
    }

    TickType_t started = xTaskGetTickCount();
    while (client->rx_task_running) {
        if (timeout_ticks != portMAX_DELAY && (xTaskGetTickCount() - started) >= timeout_ticks) {
            ESP_LOGW(TAG, "Timed out waiting for WebSocket rx task to stop");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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

static void websocket_receive_task(void *arg) {
    nuwax_ws_client_t *client = (nuwax_ws_client_t *)arg;
    ESP_LOGI(TAG,
             "WebSocket receive task started stack=%d",
             CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES);
    while (client->connected) {
        uint8_t head[2];
        if (recv_exact_until(client,
                             head,
                             1,
                             io_deadline_after_ms(NUWAX_WS_RX_IDLE_TIMEOUT_MS)) != ESP_OK) {
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
            break;
        }
        if (payload_len > NUWAX_WS_MAX_INCOMING_FRAME_BYTES) {
            ESP_LOGW(TAG, "Incoming WebSocket frame too large: %llu", (unsigned long long)payload_len);
            websocket_send_close_code(client, 1009);
            break;
        }

        const size_t payload_size = (size_t)payload_len;
        uint8_t *payload = malloc(payload_size + 1U);
        if (!payload) {
            ESP_LOGE(TAG, "No memory for incoming WebSocket frame");
            websocket_send_close_code(client, 1011);
            break;
        }
        if (payload_size > 0 &&
            recv_exact_until(client, payload, payload_size, frame_deadline_us) != ESP_OK) {
            free(payload);
            break;
        }
        payload[payload_size] = '\0';

        if (client->on_rx_activity) {
            client->on_rx_activity(client->user_ctx);
        }

        if (opcode == 0x1 && client->on_text) {
            client->on_text((const char *)payload, (int)payload_size, client->user_ctx);
        } else if (opcode == 0x2 && client->on_binary) {
            client->on_binary(payload, (int)payload_size, client->user_ctx);
        } else if (opcode == 0x8) {
            ESP_LOGW(TAG, "WebSocket close frame received");
            if (client->connected) {
                (void)send_frame(client, 0x8, payload, payload_size);
            }
            free(payload);
            break;
        } else if (opcode == 0x9) {
            ESP_LOGD(TAG, "WebSocket ping received len=%u", (unsigned)payload_size);
            ESP_ERROR_CHECK_WITHOUT_ABORT(send_frame(client, 0xA, payload, payload_size));
        } else if (opcode == 0xA) {
            ESP_LOGD(TAG, "WebSocket pong received len=%u", (unsigned)payload_size);
        }
        free(payload);
    }

    client->connected = false;
    transport_interrupt(client);
    transport_close_serialized(client);
    client->rx_task_running = false;
    ESP_LOGW(TAG, "WebSocket receive task stopped");
    vTaskSuspend(NULL);
}

static void cleanup_rx_task(nuwax_ws_client_t *client) {
    if (!client || !client->rx_task) {
        if (client) {
            client->rx_task_running = false;
        }
        return;
    }

    TaskHandle_t rx_task = (TaskHandle_t)client->rx_task;
    if (rx_task == xTaskGetCurrentTaskHandle()) {
        ESP_LOGW(TAG, "WebSocket rx task cleanup requested from rx task context");
        return;
    }

    wait_for_rx_task_stop(client, pdMS_TO_TICKS(NUWAX_WS_RX_TASK_STOP_TIMEOUT_MS));
    vTaskDeleteWithCaps(rx_task);
    client->rx_task = NULL;
    client->rx_task_running = false;
}

void nuwax_ws_init(nuwax_ws_client_t *client) {
    if (!client) {
        return;
    }
    memset(client, 0, sizeof(*client));
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

    nuwax_ws_close(client);
    client->connected = false;
    client->use_tls = url.secure;
    client->last_http_status = 0;

    if (url.secure) {
        ESP_RETURN_ON_ERROR(connect_tls(client, &url), TAG, "TLS connect failed");
    } else {
        int sock = connect_tcp(&url);
        if (sock < 0) {
            ESP_LOGE(TAG, "TCP connect failed to %s:%d", url.host, url.port);
            return ESP_FAIL;
        }
        client->sock = sock;
        client->use_tls = false;
    }

    esp_err_t err = websocket_handshake(client, &url, config->token);
    if (err != ESP_OK) {
        transport_close_serialized(client);
        return err;
    }
    ESP_LOGI(TAG,
             "WebSocket handshake ok free_internal=%u largest_internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    client->connected = true;
    client->on_text = config->on_text;
    client->on_binary = config->on_binary;
    client->on_rx_activity = config->on_rx_activity;
    client->user_ctx = config->user_ctx;

    TaskHandle_t rx_task = NULL;
    client->rx_task_running = true;
    BaseType_t task_ok = xTaskCreateWithCaps(websocket_receive_task,
                                             "nuwax_ws_rx",
                                             CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES,
                                             client,
                                             NUWAX_WS_RX_TASK_PRIORITY,
                                             &rx_task,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS || !rx_task) {
        ESP_LOGE(TAG, "WebSocket rx task create failed stack_bytes=%d", CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES);
        client->rx_task_running = false;
        transport_close_serialized(client);
        client->connected = false;
        return ESP_ERR_NO_MEM;
    }
    client->rx_task = rx_task;

    ESP_LOGI(TAG, "%s WebSocket connected to %s", url.secure ? "Secure" : "Plain", config->url);
    return ESP_OK;
}

esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *client, const char *payload) {
    if (!payload) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_frame(client, 0x1, (const uint8_t *)payload, strlen(payload));
}

esp_err_t nuwax_ws_send_binary(nuwax_ws_client_t *client, const uint8_t *payload, size_t len) {
    if (!payload) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_frame(client, 0x2, payload, len);
}

esp_err_t nuwax_ws_send_ping(nuwax_ws_client_t *client) {
    const char payload[] = "nuwax";
    return send_frame(client, 0x9, (const uint8_t *)payload, sizeof(payload) - 1);
}

void nuwax_ws_close(nuwax_ws_client_t *client) {
    if (!client) {
        return;
    }
    client->connected = false;
    transport_interrupt(client);
    cleanup_rx_task(client);
    transport_close_serialized(client);
    client->on_text = NULL;
    client->on_binary = NULL;
    client->on_rx_activity = NULL;
    client->user_ctx = NULL;
}

bool nuwax_ws_is_connected(const nuwax_ws_client_t *client) {
    return transport_is_open(client);
}

int nuwax_ws_last_http_status(const nuwax_ws_client_t *client) {
    return client ? client->last_http_status : 0;
}
