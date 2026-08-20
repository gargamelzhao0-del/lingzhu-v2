#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NUWAX_HEAD_TOUCH_INTERACTION_COOLDOWN_MS 2000U
#define NUWAX_SHAKE_INTERACTION_COOLDOWN_MS 4000U

typedef void (*nuwax_shake_detector_event_cb_t)(void *ctx);

/* Starts the low-rate QMI8658A polling worker. The callback is invoked once
 * per qualified shake and must be non-blocking. */
esp_err_t nuwax_shake_detector_start(nuwax_shake_detector_event_cb_t callback,
                                     void *ctx);

bool nuwax_shake_detector_is_running(void);

/* Shared physical-interaction gate. A successful claim starts an owner-specific
 * cooldown while the gate itself keeps touch and shake transactions mutually
 * exclusive until the active owner releases it. */
bool nuwax_shake_detector_claim_head_touch(void);
void nuwax_shake_detector_release_head_touch(void);
void nuwax_shake_detector_release_shake(void);

#ifdef __cplusplus
}
#endif
