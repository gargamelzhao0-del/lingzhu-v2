#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/reset_reasons.h"
#include "soc/soc.h"

#define NUWAX_BOOT_POWER_KEY_GPIO 16
/* ESP32-S3 ROM declares RTC STORE0 reserved; IDF 6.0 has no production use. */
#define NUWAX_POWER_OFF_MARKER    0x4E57584Fu /* Must match the application. */

/* Keep the early safety path enabled even when an older bootloader
 * sdkconfig does not yet contain the optional component symbols. The Kconfig
 * values override these fallbacks once the bootloader configuration is
 * regenerated. */
#ifndef CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_OFF
#define CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_OFF 1
#endif
#ifndef CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_GPIO
#define CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_GPIO 3
#endif
#ifndef CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_ACTIVE_LOW
#define CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_ACTIVE_LOW 0
#endif

/* Referenced by the standard bootloader link so this component's strong hook
 * overrides the weak default implementation. */
void bootloader_hooks_include(void)
{
}

#if CONFIG_BOOTLOADER_NUWAX_EARLY_POWER_HOLD || \
    CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_OFF
#if CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_OFF
static inline void force_display_backlight_off_early(uint32_t pin)
{
#if CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_ACTIVE_LOW
    const uint32_t off_level = 1U;
#else
    const uint32_t off_level = 0U;
#endif
    /* Preload the inactive level before routing or output-enable so the
     * backlight gate cannot briefly pulse during bootloader GPIO setup. */
    gpio_ll_set_level(&GPIO, pin, off_level);
    gpio_ll_set_output_signal_matrix_source(&GPIO, pin, SIG_GPIO_OUT_IDX, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, pin, false, false);
    gpio_ll_func_sel(&GPIO, pin, PIN_FUNC_GPIO);
    gpio_ll_od_disable(&GPIO, pin);
    gpio_ll_output_enable(&GPIO, pin);
    gpio_ll_set_level(&GPIO, pin, off_level);
}
#endif

#if CONFIG_BOOTLOADER_NUWAX_EARLY_POWER_HOLD
static inline void assert_pg2_early(uint32_t pin)
{
    /* Preload HIGH before routing or output-enable, so PG2 cannot pulse low. */
    gpio_ll_set_level(&GPIO, pin, 1);
    gpio_ll_set_output_signal_matrix_source(&GPIO, pin, SIG_GPIO_OUT_IDX, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, pin, false, false);
    gpio_ll_func_sel(&GPIO, pin, PIN_FUNC_GPIO);
    gpio_ll_od_disable(&GPIO, pin);
    gpio_ll_output_enable(&GPIO, pin);
    gpio_ll_set_level(&GPIO, pin, 1);
}
#endif

void bootloader_before_init(void)
{
#if CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_OFF
    force_display_backlight_off_early(
        CONFIG_BOOTLOADER_NUWAX_DISPLAY_BACKLIGHT_GPIO);
#endif
#if !CONFIG_BOOTLOADER_NUWAX_EARLY_POWER_HOLD
    return;
#else
    const uint32_t hold_pin = CONFIG_BOOTLOADER_NUWAX_POWER_HOLD_GPIO;

    /* PG1 has an external pull-up (R8), so it is safe to sample before normal
     * bootloader initialization. A tap that wakes the chip may be released by
     * the time this hook runs, so use the reset reason as the second half of
     * the wake decision instead of requiring a three-second hold here. */
    gpio_ll_func_sel(&GPIO, NUWAX_BOOT_POWER_KEY_GPIO, PIN_FUNC_GPIO);
    gpio_ll_input_enable(&GPIO, NUWAX_BOOT_POWER_KEY_GPIO);
    const bool key_pressed =
        gpio_ll_get_level(&GPIO, NUWAX_BOOT_POWER_KEY_GPIO) == 0;
    const bool intentional_off =
        REG_READ(RTC_CNTL_STORE0_REG) == NUWAX_POWER_OFF_MARKER;
    const soc_reset_reason_t reset_reason = esp_rom_get_reset_reason(0);
    const bool deep_sleep_wakeup =
        reset_reason == RESET_REASON_CORE_DEEP_SLEEP;
    /* The application arms EXT1 high while the shutdown key is still held,
     * so releasing it wakes the chip only to return to deep sleep. It then
     * rearms EXT1 low before sleeping again. A later tap wakes on low and must
     * be admitted as the real boot request even though the off marker remains
     * in RTC memory. */
    const bool press_wakeup_armed =
        (REG_READ(RTC_CNTL_EXT_WAKEUP_CONF_REG) &
         RTC_CNTL_EXT_WAKEUP1_LV) == 0U;
    const bool tap_wakeup = deep_sleep_wakeup && press_wakeup_armed;

    if (!key_pressed && intentional_off && !tap_wakeup) {
        /* The application deliberately powered off. Do not turn the rail back
         * on for an unrelated reset while it is still in the off transaction. */
        return;
    }

    if (intentional_off && tap_wakeup) {
        REG_WRITE(RTC_CNTL_STORE0_REG, 0);
    }

    /* Startup is now edge/tap based. The application task owns debounce and
     * long-press shutdown after the rail is latched; the bootloader must only
     * make the rail survive long enough for that task to start. */
    assert_pg2_early(hold_pin);
#endif
}
#endif
