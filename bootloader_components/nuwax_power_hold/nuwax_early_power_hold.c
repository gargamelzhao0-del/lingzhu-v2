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

/* Referenced by the standard bootloader link so this component's strong hook
 * overrides the weak default implementation. */
void bootloader_hooks_include(void)
{
}

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

void bootloader_before_init(void)
{
    const uint32_t hold_pin = CONFIG_BOOTLOADER_NUWAX_POWER_HOLD_GPIO;

    /* PG1 has an external pull-up (R8), so it is safe to sample before normal
     * bootloader initialization. Only an explicit application power-off marker
     * may suppress PG2; reset-reason heuristics are intentionally avoided. */
    gpio_ll_func_sel(&GPIO, NUWAX_BOOT_POWER_KEY_GPIO, PIN_FUNC_GPIO);
    gpio_ll_input_enable(&GPIO, NUWAX_BOOT_POWER_KEY_GPIO);
    const bool key_pressed =
        gpio_ll_get_level(&GPIO, NUWAX_BOOT_POWER_KEY_GPIO) == 0;
    const bool intentional_off =
        REG_READ(RTC_CNTL_STORE0_REG) == NUWAX_POWER_OFF_MARKER;
    const bool sw3_deep_sleep_wake =
        intentional_off &&
        esp_rom_get_reset_reason(0) == RESET_REASON_CORE_DEEP_SLEEP;

    if (key_pressed || sw3_deep_sleep_wake || !intentional_off) {
        REG_WRITE(RTC_CNTL_STORE0_REG, 0);
        assert_pg2_early(hold_pin);
    }
}
#endif
