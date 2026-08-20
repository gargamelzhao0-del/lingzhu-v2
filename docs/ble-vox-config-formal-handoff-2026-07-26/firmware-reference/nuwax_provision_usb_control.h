#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef bool (*nuwax_provision_usb_request_fn)(void *ctx);

/**
 * Start the physical-USB integration command listener.
 *
 * The only accepted command is "NUWAX_CTRL start_ble_provisioning". The
 * callback must enqueue the normal reversible BLE reprovisioning path; it must
 * not erase Wi-Fi or perform flash work in the USB driver task.
 */
esp_err_t nuwax_provision_usb_control_start(
    nuwax_provision_usb_request_fn request_reprovision,
    void *ctx);
