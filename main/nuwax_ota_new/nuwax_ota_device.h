/* Device-facing OTA command controller for the Miaopan 2.0 product build. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    esp_err_t (*send_json)(void *ctx, const char *json);
    /* Force a clean WebSocket reconnect when a durable terminal result could
     * not be delivered. The next authenticated session replays NVS context. */
    esp_err_t (*request_cloud_reconnect)(void *ctx);
    bool (*continuous_audio_busy)(void *ctx);
    esp_err_t (*prepare_audio_for_update)(void *ctx);
    /* OTA progress is device-local. This callback may update the UI but must
     * never send a WebSocket progress message. */
    void (*show_local_progress)(void *ctx, const char *phase, int percent,
                                const char *detail);
    /* ESP_OK means every required local subsystem is healthy.
     * ESP_ERR_NOT_FINISHED requests another check before the validation
     * deadline (for example while waiting for the first fuel-gauge sample).
     * Any other error is a deterministic local-health failure. */
    esp_err_t (*check_boot_health)(void *ctx, char *detail,
                                   size_t detail_size);
} nuwax_ota_device_callbacks_t;

esp_err_t nuwax_ota_device_init(const nuwax_ota_device_callbacks_t *callbacks);

/* Call as the first app_main action, before the SW3 task is created. This is
 * intentionally limited to the lock-free power gate; partition/NVS inspection
 * remains in nuwax_ota_device_init(), after ESP-IDF startup is complete. */
bool nuwax_ota_device_inhibit_shutdown_during_boot(void);

/* Called from the WebSocket text callback. Returns true for ota_update or a
 * matching ota_result_ack, including malformed/rejected OTA messages. The source
 * epoch prevents delayed work accepted on an old connection from being
 * authorized by a later reconnect. */
bool nuwax_ota_device_handle_text(const char *payload, int len,
                                  uint32_t connection_epoch);

/* Ordinary capture/playback paths must remain stopped while this is true. */
bool nuwax_ota_device_blocks_audio(void);
bool nuwax_ota_device_is_active(void);
bool nuwax_ota_device_worker_idle(void);
bool nuwax_ota_device_terminal_complete(void);

/* OTA application-layer readiness fence. Set false on every connection
 * attempt and true only after a validated positive cloud response to hello:
 * strict/legacy hello OK, pong, or a well-formed device command. */
void nuwax_ota_device_set_cloud_authenticated(bool authenticated,
                                               uint32_t connection_epoch);
bool nuwax_ota_device_cloud_authenticated(uint32_t connection_epoch);

/* Call after positive cloud activity. It confirms a pending image only after
 * the normal application has reached its healthy cloud path. */
esp_err_t nuwax_ota_device_cloud_ready(uint32_t connection_epoch);

#ifdef __cplusplus
}
#endif
