#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ws_transport.h"

esp_err_t nuwax_music_status_init(void);
/* Reserve before accepting a cloud request. Never evict another request. */
bool nuwax_music_status_reserve(nuwax_ws_client_t *ws, uint32_t epoch, const char *id);
void nuwax_music_status_post(nuwax_ws_client_t *ws, uint32_t epoch,
                             const char *id, const char *state, const char *reason);
/* Status sender only; playback/control producers never perform network I/O. */
void nuwax_music_status_service(nuwax_ws_client_t *ws);
