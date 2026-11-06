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
    NUWAX_PROVISION_OUTCOME_AUTH_TIMEOUT,
    NUWAX_PROVISION_OUTCOME_RESOURCE_EXHAUSTED,
    NUWAX_PROVISION_OUTCOME_OLD_WIFI_RECOVERED,
} nuwax_provision_outcome_t;

/* Selects the PSRAM-backed LVGL provisioning boot profile. This query is safe
 * after NVS initialization and before the Wi-Fi driver is initialized. */
bool nuwax_provision_main_requires_low_memory_boot_profile(void);

/* Persists a one-shot fail-safe request used when the real Wi-Fi driver state
 * reveals provisioning only after the normal boot profile was selected. */
esp_err_t nuwax_provision_main_request_low_memory_boot_profile(void);

/* Initializes the official network_provisioning manager after Wi-Fi init. */
esp_err_t nuwax_provision_main_init(void);

/* True when the official Wi-Fi provisioning marker is absent. */
bool nuwax_provision_main_should_auto_enter(void);

/* True only when verified Wi-Fi credentials and an active cloud configuration
 * form one complete bootable pair. */
bool nuwax_provision_main_has_complete_pair(void);

/* Starts BLE provisioning. Only the official provisioning service is exposed. */
esp_err_t nuwax_provision_main_start(const char *reason);

/* True while the official BLE provisioning service is running. This query is
 * safe from UI, USB and cloud-control tasks and lets those callers join an
 * existing boot provisioning session instead of starting a competing runtime
 * reprovision transaction. */
bool nuwax_provision_main_is_active(void);

/* Epoch validation belongs to the cloud owner. This nonblocking notification
 * latches a confirmed rejection; the existing guarded rollback transaction
 * persists it before restarting. Only a committed replacement pair clears it.
 * A transport handshake or an old Wi-Fi connection must never clear it. */
void nuwax_provision_main_note_binding_rejected(void);
bool nuwax_provision_main_binding_required(void);

/* Requests a cooperative stop of reversible provisioning. The request is
 * accepted only while a durable, non-rejected Wi-Fi/cloud pair can be restored and
 * before commit of the replacement pair begins. The provisioning supervisor
 * performs the actual stop so BLE callbacks, Wi-Fi ownership and rollback stay
 * serialized on their existing owner task. */
bool nuwax_provision_main_request_runtime_cancel(void);

/* Normal boot retains the durable restart fail-safe. Runtime reprovisioning
 * disables it so an unrecoverable supervisor error returns to its caller for
 * in-process rollback instead of rebooting the product. */
void nuwax_provision_main_set_restart_on_unrecoverable(bool enabled);

/* Keeps BLE provisioning available until new credentials commit or a saved
 * pair without a known binding rejection is recovered. focused_wait_ms is the BLE-only period before saved
 * Wi-Fi recovery attempts are admitted; it is not a provisioning timeout. */
nuwax_provision_outcome_t nuwax_provision_main_run(uint32_t focused_wait_ms);

/* Runtime rebind is an explicit request for a new Wi-Fi/cloud credential
 * pair. Keep the existing APP protocol and BLE service active while the APP
 * makes progress, but do not convert a recovered legacy Wi-Fi link into a
 * successful rebind outcome. A device-side inactivity deadline returns to the
 * durable previous pair if the APP abandons the flow. */
nuwax_provision_outcome_t nuwax_provision_main_run_runtime_rebind(
    uint32_t focused_wait_ms);

/* Optional bounded runner for diagnostics and explicitly bounded callers.
 * Product provisioning uses nuwax_provision_main_run() so BLE remains
 * available until the APP succeeds or saved Wi-Fi is recovered. */
nuwax_provision_outcome_t nuwax_provision_main_run_bounded(
    uint32_t focused_wait_ms,
    uint32_t overall_timeout_ms);

/* Monotonic heartbeat updated by the continuous supervisor. A zero value means
 * the supervisor has not entered its run loop. The outer product watchdog uses
 * this to distinguish a healthy indefinite BLE wait from a stalled task. */
int64_t nuwax_provision_main_supervisor_heartbeat_us(void);

/* Stops the service and releases the manager. */
esp_err_t nuwax_provision_main_stop(void);

/* Persists the STA snapshot and any latched binding rejection before rebind.
 * A rejected binding keeps BLE available and prohibits old-pair rollback. */
esp_err_t nuwax_provision_main_reset_wifi_credentials(void);

/* Abandon only an uncommitted RAM rollback snapshot before disconnecting the
 * current network. A durable rollback record is never discarded here. */
void nuwax_provision_main_discard_volatile_rollback(void);

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
