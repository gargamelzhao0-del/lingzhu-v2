#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Development-only one-shot boot test. The main application owns admission
 * and wake-detector transitions; the worker owns only HTTP and PCM ingress. */
esp_err_t nuwax_music_url_test_init(void);
void nuwax_music_url_test_observe_cloud_ready(bool ready, uint32_t now_ms);
bool nuwax_music_url_test_delay_elapsed(uint32_t now_ms);
esp_err_t nuwax_music_url_test_start(void);
void nuwax_music_url_test_request_stop(const char *reason);
bool nuwax_music_url_test_active(void);
bool nuwax_music_url_test_take_terminal(esp_err_t *result_out);

