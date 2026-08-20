/* Power latch and SW3 key handling for the Miaopan 2.0 validation board. */
#ifndef NUWAX_POWER_CTRL_H
#define NUWAX_POWER_CTRL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Assert PG2 immediately, then arm the PG1 single-click shutdown task after
 * the startup press has been released. Call before all other application
 * initialization. */
esp_err_t nuwax_power_ctrl_init(void);

typedef enum {
    NUWAX_POWER_INHIBIT_OTA_BOOT = 1U << 0,
    NUWAX_POWER_INHIBIT_OTA_UPDATE = 1U << 1,
    NUWAX_POWER_INHIBIT_REPROVISION = 1U << 2,
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
