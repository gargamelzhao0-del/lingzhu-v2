#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ws_transport.h"

/*
 * The wire envelope remains device_error_report.  This bound only controls
 * the existing result string; it does not add a message field or change ACKs.
 */
#define NUWAX_DEVICE_ERROR_RESULT_BYTES 160U

/*
 * Device errors are deliberately smaller than the diagnostic log.  The
 * event_id embeds the boot nonce, so a separate boot_id is not required on
 * the wire.  State/generation/turn_id are optional context for a
 * generation-bound audio failure.
 */
bool nuwax_device_error_report_enqueue(const char *severity,
                                       const char *component,
                                       const char *code,
                                       esp_err_t result,
                                       const char *state,
                                       uint32_t generation,
                                       const char *turn_id);

/* Same wire field as above, with a bounded detailed result string.  Passing
 * NULL preserves the legacy esp_err_to_name(result) behavior. */
bool nuwax_device_error_report_enqueue_detail(
    const char *severity,
    const char *component,
    const char *code,
    esp_err_t result,
    const char *result_detail,
    const char *state,
    uint32_t generation,
    const char *turn_id);

/* Reports a recoverable, actionable warning through the same bounded queue.
 * This is intentionally explicit; ordinary diagnostic ESP_LOGW lines are not
 * mirrored automatically to avoid flooding the queue. */
bool nuwax_device_warning_report_enqueue(const char *component,
                                         const char *code,
                                         esp_err_t result,
                                         const char *state,
                                         uint32_t generation,
                                         const char *turn_id);

bool nuwax_device_warning_report_enqueue_detail(
    const char *component,
    const char *code,
    esp_err_t result,
    const char *result_detail,
    const char *state,
    uint32_t generation,
    const char *turn_id);

/* Must run after nvs_flash_init(). Restores the bounded queue from the last
 * committed snapshot; corruption is ignored and the live queue starts empty. */
esp_err_t nuwax_device_error_report_init(void);

/*
 * Releases the persisted diagnostic queue when a critical provisioning
 * transaction is blocked by NVS exhaustion.  Error reports are best-effort;
 * Wi-Fi/cloud credentials and the provisioning rollback record take priority.
 */
esp_err_t nuwax_device_error_report_release_persisted_storage(void);

/* Flushes a pending queue snapshot without touching the network. May block on
 * Flash GC: the owner must call this only outside conversation/audio/OTA and
 * after queued lifecycle events have been handled. Also usable while offline. */
void nuwax_device_error_report_housekeeping(void);

/* Called by the connection owner from its normal service loop.  It sends at
 * most one bounded event, never writes Flash, and never waits for the cloud ACK
 * while holding the queue lock. Persistence is separate housekeeping. */
void nuwax_device_error_report_service(nuwax_ws_client_t *ws);

/* Returns true when payload is a device_error_report_ack and consumes the
 * matching in-flight event. */
bool nuwax_device_error_report_handle_ack(const char *payload,
                                          int len,
                                          uint32_t ws_epoch);
