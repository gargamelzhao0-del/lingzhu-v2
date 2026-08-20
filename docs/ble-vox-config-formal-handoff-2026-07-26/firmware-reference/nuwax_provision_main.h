#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NUWAX_PROVISION_OUTCOME_NONE = 0,
    NUWAX_PROVISION_OUTCOME_SUCCESS,
    NUWAX_PROVISION_OUTCOME_AUTH_FAILED,
    NUWAX_PROVISION_OUTCOME_NETWORK_NOT_FOUND,
    NUWAX_PROVISION_OUTCOME_TIMEOUT,
    NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR,
    NUWAX_PROVISION_OUTCOME_STOPPED,
} nuwax_provision_outcome_t;

/* Initializes the official network_provisioning manager after Wi-Fi init. */
esp_err_t nuwax_provision_main_init(void);

/* True when the official Wi-Fi provisioning marker is absent. */
bool nuwax_provision_main_should_auto_enter(void);

/* Starts BLE provisioning. Only the official provisioning service is exposed. */
esp_err_t nuwax_provision_main_start(const char *reason);

/* Runs the first-boot provisioning flow until success, failure or timeout. */
nuwax_provision_outcome_t nuwax_provision_main_run(uint32_t timeout_ms);

/* Stops the service and releases the manager. */
esp_err_t nuwax_provision_main_stop(void);

/* Persists the current STA configuration and requests a reversible BLE rebind. */
esp_err_t nuwax_provision_main_reset_wifi_credentials(void);

/* True when BLE provisioning was entered from the local rebind action. */
bool nuwax_provision_main_is_reprovisioning(void);

/* Commits new Wi-Fi credentials or restores the saved credentials on failure. */
esp_err_t nuwax_provision_main_commit_wifi_credentials(void);
esp_err_t nuwax_provision_main_rollback_wifi_credentials(void);

/* Stable values used by the app QR payload and the device-info endpoint. */
const char *nuwax_provision_main_service_name(void);
const char *nuwax_provision_main_qr_payload(void);
nuwax_provision_outcome_t nuwax_provision_main_last_outcome(void);
const char *nuwax_provision_main_last_reason(void);

#ifdef __cplusplus
}
#endif
