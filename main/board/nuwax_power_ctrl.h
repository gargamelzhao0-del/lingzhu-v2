/* Power latch and SW3 key handling for the Miaopan 2.0 validation board. */
#ifndef NUWAX_POWER_CTRL_H
#define NUWAX_POWER_CTRL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NUWAX_POWER_KEY_EVENT_SHORT_PRESS = 0,
    NUWAX_POWER_KEY_EVENT_LONG_PRESS,
    NUWAX_POWER_KEY_EVENT_LONG_RELEASE,
} nuwax_power_key_event_t;

typedef void (*nuwax_power_key_event_cb_t)(
    void *ctx,
    nuwax_power_key_event_t event);

/* Assert PG2 immediately, then arm the PG1 key task. The listener only receives
 * semantic events; it must be non-blocking and must not touch audio, LVGL or
 * WebSocket state from the power-key task. */
esp_err_t nuwax_power_ctrl_init(void);

void nuwax_power_ctrl_set_key_event_listener(
    nuwax_power_key_event_cb_t callback,
    void *ctx);

/* Called by the application owner only after all non-power owners have
 * quiesced. The normal three-second press may still be held; the driver arms
 * release as the intermediate wake level so the same press cannot restart the
 * device. Emergency force-off remains owned by the power task. */
bool nuwax_power_ctrl_commit_shutdown(bool force);

/* Application-owner transaction: close admission before draining resources.
 * Repeated begin is idempotent; abort is only used before the terminal commit. */
bool nuwax_power_ctrl_prepare_shutdown(void);
bool nuwax_power_ctrl_shutdown_preparing(void);
void nuwax_power_ctrl_abort_shutdown(void);

typedef enum {
    NUWAX_POWER_INHIBIT_OTA_BOOT = 1U << 0,
    NUWAX_POWER_INHIBIT_OTA_UPDATE = 1U << 1,
    NUWAX_POWER_INHIBIT_REPROVISION = 1U << 2,
    NUWAX_POWER_INHIBIT_BATTERY_PROFILE = 1U << 3,
} nuwax_power_inhibit_owner_t;

/* Owners acquire and release only their own bit. This prevents one operation
 * from reopening SW3 while another operation still needs atomic power. */
bool nuwax_power_ctrl_acquire_shutdown_inhibit(
    nuwax_power_inhibit_owner_t owner);
bool nuwax_power_ctrl_release_shutdown_inhibit(
    nuwax_power_inhibit_owner_t owner);
uint32_t nuwax_power_ctrl_shutdown_inhibit_owners(void);
bool nuwax_power_ctrl_shutdown_is_inhibited(void);

#ifdef __cplusplus
}
#endif

#endif /* NUWAX_POWER_CTRL_H */
