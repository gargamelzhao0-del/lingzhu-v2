#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct esp_tls esp_tls_t;

typedef void (*nuwax_ws_text_handler_t)(const char *payload, int len, void *user_ctx);
typedef void (*nuwax_ws_binary_handler_t)(const uint8_t *payload, int len, void *user_ctx);
typedef void (*nuwax_ws_activity_handler_t)(void *user_ctx);

typedef struct {
    const char *url;
    const char *token;
    nuwax_ws_text_handler_t on_text;
    nuwax_ws_binary_handler_t on_binary;
    nuwax_ws_activity_handler_t on_rx_activity;
    void *user_ctx;
} nuwax_ws_config_t;

typedef struct {
    int sock;
    esp_tls_t *tls;
    bool use_tls;
    bool connected;
    int last_http_status;
    nuwax_ws_text_handler_t on_text;
    nuwax_ws_binary_handler_t on_binary;
    nuwax_ws_activity_handler_t on_rx_activity;
    void *user_ctx;
    void *rx_task;
    volatile bool rx_task_running;
} nuwax_ws_client_t;

void nuwax_ws_init(nuwax_ws_client_t *client);
esp_err_t nuwax_ws_connect(nuwax_ws_client_t *client, const nuwax_ws_config_t *config);
esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *client, const char *payload);
esp_err_t nuwax_ws_send_binary(nuwax_ws_client_t *client, const uint8_t *payload, size_t len);
esp_err_t nuwax_ws_send_ping(nuwax_ws_client_t *client);
void nuwax_ws_close(nuwax_ws_client_t *client);
bool nuwax_ws_is_connected(const nuwax_ws_client_t *client);
int nuwax_ws_last_http_status(const nuwax_ws_client_t *client);
