#include "display_brightness.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "wake_word_detector.h"
#include "device_error_report.h"

static const char *TAG = "display_brightness";
static uint8_t s_percent = 100U;
static uint8_t s_saved = 100U;
static int64_t s_save_after_us;
static bool s_dirty;
static bool s_save_uncertain;

void nuwax_display_brightness_load(void)
{
    nvs_handle_t handle;
    uint8_t value = 100U;
    esp_err_t err = nvs_open("display_ui", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, "brightness", &value);
        nvs_close(handle);
    }
    s_percent = err == ESP_OK && nuwax_display_brightness_valid(value) ? value : 100U;
    s_saved = s_percent;
    s_dirty = false;
    s_save_uncertain = false;
}

uint8_t nuwax_display_brightness_percent(void) { return s_percent; }

esp_err_t nuwax_display_brightness_apply(uint8_t percent)
{
    if (!nuwax_display_brightness_valid(percent)) return ESP_ERR_INVALID_ARG;
    if (percent == s_percent) return ESP_OK;
    const esp_err_t err = nuwax_display_backlight_set_percent(percent);
    if (err != ESP_OK) return err;
    s_percent = percent;
    s_dirty = s_percent != s_saved || s_save_uncertain;
    s_save_after_us = esp_timer_get_time() + 1000000LL;
    return ESP_OK;
}

void nuwax_display_brightness_save(bool force)
{
    if (!s_dirty || (!force && esp_timer_get_time() < s_save_after_us)) return;
    esp_err_t err = ESP_FAIL;
    /* The main owner serializes this service. Never write Flash while the
     * wake worker can be executing the Flash-mapped model. */
    for (unsigned attempt = 0; attempt < 2U; ++attempt) {
        err = nuwax_wake_word_detector_begin_flash_operation(500U);
        if (err != ESP_OK) break;
        nvs_handle_t handle;
        err = nvs_open("display_ui", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            /* A failed commit does not prove that set_u8 left Flash intact. */
            s_save_uncertain = true;
            err = nvs_set_u8(handle, "brightness", s_percent);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        nuwax_wake_word_detector_end_flash_operation();
        if (err != ESP_ERR_NVS_NOT_ENOUGH_SPACE || attempt != 0U) break;
        /* Release optional diagnostics, never user credentials or settings.
         * End our guard first: the release operation owns its own guard. */
        if (nuwax_device_error_report_release_persisted_storage() != ESP_OK) break;
    }
    if (err == ESP_OK) {
        s_saved = s_percent;
        s_dirty = false;
        s_save_uncertain = false;
    } else {
        s_save_after_us = esp_timer_get_time() + 5000000LL;
        ESP_LOGW(TAG, "brightness save deferred: %s", esp_err_to_name(err));
    }
}
