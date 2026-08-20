#include "nuwax_provision_usb_control.h"

#include <stdint.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NUWAX_USB_CONTROL_TASK_STACK_BYTES 3072U
#define NUWAX_USB_CONTROL_TASK_PRIORITY 3U
#define NUWAX_USB_CONTROL_COMMAND "NUWAX_CTRL start_ble_provisioning"

static const char *TAG = "nuwax_prov_usb";
static nuwax_provision_usb_request_fn s_request_reprovision;
static void *s_request_ctx;

static void write_response(const char *response) {
    (void)usb_serial_jtag_write_bytes(response, strlen(response),
                                      pdMS_TO_TICKS(100));
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
}

static void usb_control_task(void *arg) {
    (void)arg;
    char line[64] = {0};
    size_t length = 0;

    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    esp_err_t driver_err = usb_serial_jtag_driver_install(&config);
    if (driver_err != ESP_OK && driver_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "USB control driver init failed: %s",
                 esp_err_to_name(driver_err));
        vTaskDeleteWithCaps(NULL);
        return;
    }

    ESP_LOGW(TAG,
             "integration USB provisioning control enabled task_stack=psram bytes=%u stack_min_free=%u",
             (unsigned)NUWAX_USB_CONTROL_TASK_STACK_BYTES,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    while (true) {
        uint8_t input[32];
        int received = usb_serial_jtag_read_bytes(input, sizeof(input),
                                                  pdMS_TO_TICKS(20));
        if (received <= 0) {
            continue;
        }
        for (int index = 0; index < received; index++) {
            const int value = input[index];
            if (value == '\r' || value == '\n') {
                if (length == 0) {
                    continue;
                }
                line[length] = '\0';
                const char *response =
                    "NUWAX_CTRL ERROR unsupported_command\n";
                if (strcmp(line, NUWAX_USB_CONTROL_COMMAND) == 0) {
                    const bool accepted =
                        s_request_reprovision &&
                        s_request_reprovision(s_request_ctx);
                    response = accepted
                                   ? "NUWAX_CTRL ACK start_ble_provisioning\n"
                                   : "NUWAX_CTRL BUSY start_ble_provisioning\n";
                }
                write_response(response);
                length = 0;
                continue;
            }
            if (value >= 0x20 && value <= 0x7e) {
                if (length + 1 < sizeof(line)) {
                    line[length++] = (char)value;
                } else {
                    length = 0;
                }
            }
        }
    }
}

esp_err_t nuwax_provision_usb_control_start(
    nuwax_provision_usb_request_fn request_reprovision,
    void *ctx) {
    if (!request_reprovision) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_request_reprovision) {
        return ESP_ERR_INVALID_STATE;
    }
    s_request_reprovision = request_reprovision;
    s_request_ctx = ctx;
    if (xTaskCreateWithCaps(usb_control_task, "nuwax_usb_ctrl",
                            NUWAX_USB_CONTROL_TASK_STACK_BYTES, NULL,
                            NUWAX_USB_CONTROL_TASK_PRIORITY, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_request_reprovision = NULL;
        s_request_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
