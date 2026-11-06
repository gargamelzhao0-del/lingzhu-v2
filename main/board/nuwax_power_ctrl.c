/* Power latch and SW3 handling for the Miaopan 2.0 board. */
#include "board/nuwax_power_ctrl.h"

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "nuwax_power";

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0 && (CONFIG_NUWAX_POWER_CTRL_GPIO >= 0)

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "board/esp_vocat_v1_0_pins.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

#define NUWAX_BOOT_MAGIC                  0x4E575842u /* 'NWXB' */
#define NUWAX_POWER_KEY_TASK_STACK_BYTES 3072U
#define NUWAX_POWER_KEY_TASK_PRIORITY    (configMAX_PRIORITIES - 1U)
#define NUWAX_POWER_SHUTDOWN_QUIET_US    20000U
/* ESP32-S3 ROM declares RTC STORE0 reserved; IDF 6.0 has no production use. */
#define NUWAX_POWER_OFF_MARKER             0x4E57584Fu /* 'NWXO' */

#ifndef CONFIG_NUWAX_POWER_KEY_LONG_PRESS_MS
#define CONFIG_NUWAX_POWER_KEY_LONG_PRESS_MS 3000
#endif

/* RTC-retained boot diagnostics survive reset and USB-backed deep sleep, but
 * reset after a real battery power-off. */
static RTC_NOINIT_ATTR uint32_t s_boot_count;
static RTC_NOINIT_ATTR uint32_t s_boot_magic;

#define NUWAX_SHUTDOWN_GATE_CLAIMED (UINT32_C(1) << 31)
#define NUWAX_SHUTDOWN_GATE_PREPARING (UINT32_C(1) << 30)
#define NUWAX_SHUTDOWN_INHIBIT_OWNER_MASK                                  \
    ((uint32_t)NUWAX_POWER_INHIBIT_OTA_BOOT |                              \
     (uint32_t)NUWAX_POWER_INHIBIT_OTA_UPDATE |                            \
     (uint32_t)NUWAX_POWER_INHIBIT_REPROVISION |                          \
     (uint32_t)NUWAX_POWER_INHIBIT_BATTERY_PROFILE)

/* The owner bits and the terminal shutdown claim share one atomic word so an
 * owner cannot appear after SW3 has already committed the power transition. */
static atomic_uint_fast32_t s_shutdown_gate = ATOMIC_VAR_INIT(0U);

bool nuwax_power_ctrl_prepare_shutdown(void)
{
    uint_fast32_t expected = 0U;
    return atomic_compare_exchange_strong_explicit(
               &s_shutdown_gate, &expected, NUWAX_SHUTDOWN_GATE_PREPARING,
               memory_order_acq_rel, memory_order_acquire) ||
           expected == NUWAX_SHUTDOWN_GATE_PREPARING;
}

bool nuwax_power_ctrl_shutdown_preparing(void)
{
    return (atomic_load_explicit(&s_shutdown_gate, memory_order_acquire) &
            (NUWAX_SHUTDOWN_GATE_PREPARING | NUWAX_SHUTDOWN_GATE_CLAIMED)) != 0U;
}

void nuwax_power_ctrl_abort_shutdown(void)
{
    uint_fast32_t expected = NUWAX_SHUTDOWN_GATE_PREPARING;
    (void)atomic_compare_exchange_strong_explicit(
        &s_shutdown_gate, &expected, 0U, memory_order_acq_rel,
        memory_order_acquire);
}

#if CONFIG_NUWAX_POWER_KEY_ENABLE
static TaskHandle_t s_power_key_task_handle;
static StaticTask_t s_power_key_task_tcb;
static StackType_t
    s_power_key_task_stack[NUWAX_POWER_KEY_TASK_STACK_BYTES /
                           sizeof(StackType_t)];
static bool s_shutdown_started;
static bool s_recovery_boot_press_forces_shutdown;
static _Atomic(nuwax_power_key_event_cb_t) s_key_event_callback =
    ATOMIC_VAR_INIT(NULL);
static _Atomic(void *) s_key_event_context = ATOMIC_VAR_INIT(NULL);

_Static_assert((NUWAX_POWER_KEY_TASK_STACK_BYTES % sizeof(StackType_t)) == 0U,
               "power-key stack size must be StackType_t aligned");
#endif

#if CONFIG_NUWAX_POWER_CTRL_GPIO != NUWAX_VOCAT_POWER_CTRL_GPIO
#error "V2 product power-hold GPIO must match the board contract"
#endif
#if CONFIG_NUWAX_POWER_KEY_ENABLE && \
    CONFIG_NUWAX_POWER_KEY_GPIO != NUWAX_VOCAT_POWER_KEY_GPIO
#error "V2 product SW3 GPIO must match the board contract"
#endif
#if CONFIG_NUWAX_POWER_KEY_ENABLE && !CONFIG_NUWAX_POWER_KEY_ACTIVE_LOW
#error "V2 product SW3 must remain active-low"
#endif

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "OTHER";
    }
}

static int power_hold_level(void)
{
#if CONFIG_NUWAX_POWER_CTRL_HOLD_LOW
    return 0;
#else
    return 1;
#endif
}

static esp_err_t configure_power_hold_output(int pin, int hold_level)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    /* Preload the output state while any deep-sleep pad hold is still active.
     * Releasing the pad hold afterwards cannot create a low pulse on PG2. */
    esp_err_t err = gpio_set_level((gpio_num_t)pin, hold_level);
    if (err == ESP_OK) {
        err = gpio_config(&io);
    }
    if (err == ESP_OK) {
        err = gpio_set_level((gpio_num_t)pin, hold_level);
    }
    /* A failed preload/configuration must not release the retained supply. */
    if (err != ESP_OK) return err;
    gpio_deep_sleep_hold_dis();
    const esp_err_t hold_err = gpio_hold_dis((gpio_num_t)pin);
    if (err == ESP_OK) {
        err = hold_err;
    }
    if (err == ESP_OK) {
        err = gpio_set_level((gpio_num_t)pin, hold_level);
    }
    return err;
}

#if CONFIG_NUWAX_POWER_KEY_ENABLE

static int power_key_active_level(void)
{
#if CONFIG_NUWAX_POWER_KEY_ACTIVE_LOW
    return 0;
#else
    return 1;
#endif
}

static esp_err_t configure_power_key_input(void)
{
    const gpio_num_t pin = (gpio_num_t)CONFIG_NUWAX_POWER_KEY_GPIO;

    if (rtc_gpio_is_valid_gpio(pin)) {
        /* EXT1 routes the pad through RTC IO while sleeping. Return it to the
         * digital GPIO matrix before the runtime key task starts. */
        const esp_err_t rtc_err = rtc_gpio_deinit(pin);
        if (rtc_err != ESP_OK) {
            return rtc_err;
        }
    }

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_NUWAX_POWER_KEY_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

static bool power_key_is_pressed(void)
{
    return gpio_get_level((gpio_num_t)CONFIG_NUWAX_POWER_KEY_GPIO) ==
           power_key_active_level();
}

static bool key_event_listener_present(void)
{
    return atomic_load_explicit(&s_key_event_callback,
                                memory_order_acquire) != NULL;
}

static void emit_key_event(nuwax_power_key_event_t event)
{
    const nuwax_power_key_event_cb_t callback = atomic_load_explicit(
        &s_key_event_callback, memory_order_acquire);
    if (!callback) {
        return;
    }
    void *const context = atomic_load_explicit(&s_key_event_context,
                                               memory_order_acquire);
    callback(context, event);
}

static void force_output_level(gpio_num_t pin, int level)
{
    esp_err_t err = gpio_set_level(pin, level);
    if (err == ESP_OK) {
        err = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    }
    if (err == ESP_OK) {
        (void)gpio_set_level(pin, level);
    }
}

static void silence_outputs_for_shutdown(void)
{
    /* PA_CTRL is active-high. Force it low instead of entering the audio
     * driver's teardown path from an independent power-key task. */
    force_output_level((gpio_num_t)NUWAX_VOCAT_PA_CTRL_GPIO, 0);

#if CONFIG_NUWAX_DISPLAY_ENABLE
#if CONFIG_NUWAX_DISPLAY_BL_ACTIVE_LOW
    const int backlight_off = 1;
#else
    const int backlight_off = 0;
#endif
    /* Do not enter LEDC/LVGL teardown from the emergency task. Reclaiming the
     * pin as GPIO is sufficient before PG2 is released and cannot wait on a
     * display-owned mutex. */
    force_output_level((gpio_num_t)CONFIG_NUWAX_DISPLAY_PIN_BL,
                       backlight_off);
#endif
}

static esp_err_t configure_shutdown_wakeup(bool key_is_held)
{
    const gpio_num_t key_pin = (gpio_num_t)CONFIG_NUWAX_POWER_KEY_GPIO;
    esp_err_t err;

    if (!rtc_gpio_is_valid_gpio(key_pin)) {
        ESP_LOGE(TAG, "GPIO%d is not an RTC IO; refusing unsafe shutdown",
                 (int)key_pin);
        return ESP_ERR_INVALID_ARG;
    }

    err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_sleep_disable_ext1_wakeup_io(0);
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_NUWAX_POWER_KEY_ACTIVE_LOW
    err = rtc_gpio_pulldown_dis(key_pin);
    if (err == ESP_OK) {
        err = rtc_gpio_pullup_en(key_pin);
    }
    if (err == ESP_OK) {
        err = esp_sleep_enable_ext1_wakeup_io(
            1ULL << key_pin,
            key_is_held ? ESP_EXT1_WAKEUP_ANY_HIGH
                        : ESP_EXT1_WAKEUP_ANY_LOW);
    }
#else
    err = rtc_gpio_pullup_dis(key_pin);
    if (err == ESP_OK) {
        err = rtc_gpio_pulldown_en(key_pin);
    }
    if (err == ESP_OK) {
        err = esp_sleep_enable_ext1_wakeup_io(
            1ULL << key_pin,
            key_is_held ? ESP_EXT1_WAKEUP_ANY_LOW
                        : ESP_EXT1_WAKEUP_ANY_HIGH);
    }
#endif
    return err;
}

static void clear_power_off_marker(void)
{
    REG_WRITE(RTC_CNTL_STORE0_REG, 0);
}

static esp_err_t honor_power_off_marker_before_hold(int hold_pin)
{
    if (REG_READ(RTC_CNTL_STORE0_REG) != NUWAX_POWER_OFF_MARKER) {
        return ESP_OK;
    }

    esp_err_t err = configure_power_key_input();
    if (err != ESP_OK) {
        clear_power_off_marker();
        return err;
    }

    if (power_key_is_pressed()) {
        clear_power_off_marker();
        return ESP_OK;
    }

    err = configure_shutdown_wakeup(false);
    if (err != ESP_OK) {
        clear_power_off_marker();
        return err;
    }

    const gpio_num_t pin = (gpio_num_t)hold_pin;
    const int release_level = !power_hold_level();
    err = gpio_set_level(pin, release_level);
    if (err == ESP_OK) {
        err = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    }
    if (err == ESP_OK) {
        err = gpio_set_level(pin, release_level);
    }
    if (err != ESP_OK) {
        clear_power_off_marker();
        return err;
    }
    esp_deep_sleep_start();

    clear_power_off_marker();
    return ESP_FAIL;
}

static bool claim_shutdown_gate(bool force,
                                uint_fast32_t *previous_owners)
{
    uint_fast32_t current = atomic_load_explicit(&s_shutdown_gate,
                                                 memory_order_acquire);
    for (;;) {
        if ((current & NUWAX_SHUTDOWN_GATE_CLAIMED) != 0U) {
            return true;
        }
        const uint_fast32_t owners =
            current & NUWAX_SHUTDOWN_INHIBIT_OWNER_MASK;
        if (owners != 0U && !force) {
            if (previous_owners) {
                *previous_owners = current;
            }
            return false;
        }
        uint_fast32_t expected = current;
        if (atomic_compare_exchange_strong_explicit(
                &s_shutdown_gate, &expected, NUWAX_SHUTDOWN_GATE_CLAIMED,
                memory_order_acq_rel, memory_order_acquire)) {
            if (previous_owners) {
                *previous_owners = current;
            }
            return true;
        }
        current = expected;
    }
}

static void restore_shutdown_gate(uint_fast32_t previous_owners)
{
    atomic_store_explicit(
        &s_shutdown_gate,
        previous_owners & (NUWAX_SHUTDOWN_INHIBIT_OWNER_MASK |
                           NUWAX_SHUTDOWN_GATE_PREPARING),
        memory_order_release);
}

static bool enter_power_off_state(bool force)
{
    if (s_shutdown_started) {
        return true;
    }

    uint_fast32_t previous_owners = 0U;
    if (!claim_shutdown_gate(force, &previous_owners)) {
        return false;
    }

    const bool key_is_held = power_key_is_pressed();
    /* At the normal three-second threshold SW3 is intentionally still held.
     * Arm the opposite level in that case so this same press cannot wake the
     * device immediately after deep-sleep entry. A release wake retains the
     * off marker; the early boot/application handoff then rearms the next
     * press as the real power-on request. */
    esp_err_t err = configure_shutdown_wakeup(key_is_held);
    if (err != ESP_OK) {
        restore_shutdown_gate(previous_owners);
        return false;
    }

    s_shutdown_started = true;
    silence_outputs_for_shutdown();
    /* This terminal path must not depend on scheduler progress after the key
     * has been accepted. The PA quiet interval is therefore a bounded ROM
     * delay instead of a task delay. */
    esp_rom_delay_us(NUWAX_POWER_SHUTDOWN_QUIET_US);

    const gpio_num_t hold_pin = (gpio_num_t)CONFIG_NUWAX_POWER_CTRL_GPIO;
    const int release_level = !power_hold_level();

    /* The battery rail normally disappears as soon as PG2 is released. R9 is
     * the external PG2/Q4 pull-down, so no deep-sleep pad hold is needed. If a
     * connected USB cable keeps the MCU powered, deep sleep keeps the device
     * visibly off until the next SW3 press. */
    REG_WRITE(RTC_CNTL_STORE0_REG, NUWAX_POWER_OFF_MARKER);
    ESP_LOGI(TAG,
             "power-off committed key_held=%d wake_on=%s",
             key_is_held ? 1 : 0,
             key_is_held ? "release" : "press");
    (void)gpio_set_level(hold_pin, release_level);
    esp_deep_sleep_start();

    clear_power_off_marker();
    s_shutdown_started = false;
    restore_shutdown_gate(previous_owners);
    return false;
}

static TickType_t milliseconds_to_ticks_ceil(uint32_t milliseconds)
{
    uint64_t ticks = ((uint64_t)milliseconds * (uint64_t)configTICK_RATE_HZ +
                      999ULL) /
                     1000ULL;
    if (ticks == 0U) {
        ticks = 1U;
    }
    if (ticks > (uint64_t)portMAX_DELAY) {
        ticks = (uint64_t)portMAX_DELAY;
    }
    return (TickType_t)ticks;
}

static bool reset_press_is_shutdown_recovery(esp_reset_reason_t reason)
{
    (void)reason;
    /*
     * A key that is still held while the chip reboots is the same physical
     * startup gesture that the bootloader has already qualified.  Treating
     * WDT/panic/brownout recovery as an implicit shutdown request can turn a
     * transient boot failure into a power-cycle loop.  The runtime task still
     * emits LONG_PRESS after a fresh debounced press, and the bootloader keeps
     * the explicit three-second startup qualification.
     */
    return false;
}

static void power_key_task(void *arg)
{
    (void)arg;
    enum {
        WAIT_BOOT_RELEASE = 0,
        ARMED,
        PRESSED,
    } state;

    const TickType_t poll_ticks = milliseconds_to_ticks_ceil(
        CONFIG_NUWAX_POWER_KEY_POLL_MS);
    const TickType_t debounce_ticks = milliseconds_to_ticks_ceil(
        CONFIG_NUWAX_POWER_KEY_DEBOUNCE_MS);
    const TickType_t force_hold_ticks = milliseconds_to_ticks_ceil(
        CONFIG_NUWAX_POWER_KEY_FORCE_OFF_HOLD_MS);
    const TickType_t long_press_ticks = milliseconds_to_ticks_ceil(
        CONFIG_NUWAX_POWER_KEY_LONG_PRESS_MS);
    TickType_t now = xTaskGetTickCount();
    TickType_t last_wake = now;
    TickType_t sampled_since = now;
    TickType_t press_started = now;
    bool sampled_pressed = power_key_is_pressed();
    bool force_on_release =
        sampled_pressed && s_recovery_boot_press_forces_shutdown;
    bool long_press_emitted = false;
    state = force_on_release ? PRESSED : WAIT_BOOT_RELEASE;

    for (;;) {
        const bool pressed = power_key_is_pressed();
        now = xTaskGetTickCount();
        if (pressed != sampled_pressed) {
            sampled_pressed = pressed;
            sampled_since = now;
        }
        const TickType_t stable_ticks = now - sampled_since;

        if (state == WAIT_BOOT_RELEASE) {
            if (!sampled_pressed && stable_ticks >= debounce_ticks) {
                state = ARMED;
                sampled_since = now;
            }
        } else if (state == ARMED) {
            if (sampled_pressed && stable_ticks >= debounce_ticks) {
                state = PRESSED;
                /* Start the product threshold after the debounced edge. This
                 * prevents contact chatter from shortening the configured
                 * three-second long-press contract. */
                press_started = now;
                force_on_release = false;
                long_press_emitted = false;
                sampled_since = now;
            }
        } else {
            const TickType_t held_ticks = now - press_started;
            if (sampled_pressed && held_ticks >= force_hold_ticks) {
                force_on_release = true;
            }
            if (sampled_pressed && !long_press_emitted &&
                held_ticks >= long_press_ticks) {
                long_press_emitted = true;
                emit_key_event(NUWAX_POWER_KEY_EVENT_LONG_PRESS);
            }
            if (!sampled_pressed && stable_ticks >= debounce_ticks) {
                const bool force = force_on_release ||
                                   held_ticks >= force_hold_ticks;
                if (force) {
                    /* Emergency force-off remains a hardware-owned fallback
                     * and is intentionally independent of the app owner. */
                    (void)enter_power_off_state(true);
                } else if (long_press_emitted) {
                    if (key_event_listener_present()) {
                        emit_key_event(NUWAX_POWER_KEY_EVENT_LONG_RELEASE);
                    } else {
                        /* Preserve the legacy fail-safe when the application
                         * has not installed a semantic listener yet. */
                        (void)enter_power_off_state(false);
                    }
                } else if (key_event_listener_present()) {
                    emit_key_event(NUWAX_POWER_KEY_EVENT_SHORT_PRESS);
                } else {
                    (void)enter_power_off_state(false);
                }
                state = ARMED;
                force_on_release = false;
                long_press_emitted = false;
                sampled_since = now;
            }
        }

        xTaskDelayUntil(&last_wake, poll_ticks);
    }
}

void nuwax_power_ctrl_set_key_event_listener(
    nuwax_power_key_event_cb_t callback,
    void *ctx)
{
    atomic_store_explicit(&s_key_event_context, ctx, memory_order_release);
    atomic_store_explicit(&s_key_event_callback, callback, memory_order_release);
}

bool nuwax_power_ctrl_commit_shutdown(bool force)
{
    return enter_power_off_state(force);
}

static esp_err_t start_power_key_task(void)
{
    if (s_power_key_task_handle) {
        return ESP_OK;
    }
    esp_err_t err = configure_power_key_input();
    if (err != ESP_OK) {
        return err;
    }

    s_power_key_task_handle = xTaskCreateStatic(
        power_key_task,
        "nuwax_power_key",
        NUWAX_POWER_KEY_TASK_STACK_BYTES,
        NULL,
        NUWAX_POWER_KEY_TASK_PRIORITY,
        s_power_key_task_stack,
        &s_power_key_task_tcb);
    if (!s_power_key_task_handle) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif /* CONFIG_NUWAX_POWER_KEY_ENABLE */

#if !CONFIG_NUWAX_POWER_CTRL_OBSERVE
static bool s_restart_hold_registered;

/* esp_restart is also used by provisioning and OTA. Latch PG2 across the
 * reset boundary; configure_power_hold_output preloads HIGH before releasing
 * this pad hold on the next boot. Deep-sleep shutdown does not call this. */
static void preserve_power_on_restart(void)
{
    if (nuwax_power_ctrl_shutdown_preparing()) return;
    const gpio_num_t pin = (gpio_num_t)CONFIG_NUWAX_POWER_CTRL_GPIO;
    const esp_err_t level_err = gpio_set_level(pin, power_hold_level());
    const esp_err_t hold_err = level_err == ESP_OK ? gpio_hold_en(pin) : level_err;
    if (hold_err != ESP_OK) {
        ESP_LOGE(TAG, "restart PG2 pad hold failed: %s", esp_err_to_name(hold_err));
    }
}
#endif

esp_err_t nuwax_power_ctrl_init(void)
{
    const int pin = CONFIG_NUWAX_POWER_CTRL_GPIO;
    const int hold_level = power_hold_level();
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    esp_err_t err = ESP_OK;

#if CONFIG_NUWAX_POWER_KEY_ENABLE
    err = honor_power_off_marker_before_hold(pin);
    if (err != ESP_OK) {
        return err;
    }
    s_recovery_boot_press_forces_shutdown =
        reset_press_is_shutdown_recovery(reset_reason);
#endif

    if (s_boot_magic != NUWAX_BOOT_MAGIC || reset_reason == ESP_RST_POWERON) {
        s_boot_magic = NUWAX_BOOT_MAGIC;
        s_boot_count = 0;
    }
    s_boot_count++;

#if CONFIG_NUWAX_POWER_CTRL_OBSERVE
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&io);
    ESP_LOGW(TAG, "mode=OBSERVE: PG2 GPIO%d reads %d; drives nothing",
             pin, gpio_get_level((gpio_num_t)pin));
#else
    err = configure_power_hold_output(pin, hold_level);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_restart_hold_registered) {
        err = esp_register_shutdown_handler(preserve_power_on_restart);
        if (err != ESP_OK) return err;
        s_restart_hold_registered = true;
    }
#if CONFIG_NUWAX_POWER_KEY_ENABLE
    clear_power_off_marker();
#endif

#if CONFIG_NUWAX_POWER_KEY_ENABLE
    err = start_power_key_task();
    if (err != ESP_OK) {
        return err;
    }
#endif
    ESP_LOGI(TAG, "mode=HOLD: PG2 GPIO%d=%d", pin, hold_level);
#endif
    /* The physical worker is alive before any console output below. A wedged
     * console can therefore stall startup diagnostics without disabling SW3. */
    ESP_LOGI(TAG, "Miaopan power: PG2=GPIO%d boot#=%lu reset=%s wake_mask=0x%08lx",
             pin, (unsigned long)s_boot_count, reset_reason_str(reset_reason),
             (unsigned long)esp_sleep_get_wakeup_causes());
    if (reset_reason == ESP_RST_BROWNOUT) {
        ESP_LOGE(TAG,
                 "BROWNOUT detected; check supply current and PG2 latch polarity");
    }
    return ESP_OK;
}

static bool valid_inhibit_owner(nuwax_power_inhibit_owner_t owner)
{
    const uint32_t value = (uint32_t)owner;
    return value != 0U &&
           (value & ~NUWAX_SHUTDOWN_INHIBIT_OWNER_MASK) == 0U &&
           (value & (value - 1U)) == 0U;
}

bool nuwax_power_ctrl_acquire_shutdown_inhibit(
    nuwax_power_inhibit_owner_t owner)
{
    if (!valid_inhibit_owner(owner)) {
        return false;
    }
    uint_fast32_t current = atomic_load_explicit(&s_shutdown_gate,
                                                 memory_order_acquire);
    for (;;) {
        if ((current & (NUWAX_SHUTDOWN_GATE_CLAIMED |
                        NUWAX_SHUTDOWN_GATE_PREPARING)) != 0U) {
            return false;
        }
        /* Profile maintenance may coexist with boot validation, but never
         * with an update/reprovision transaction that can write or reboot. */
        const uint_fast32_t maintenance = NUWAX_POWER_INHIBIT_BATTERY_PROFILE;
        const uint_fast32_t writers = NUWAX_POWER_INHIBIT_OTA_UPDATE |
                                     NUWAX_POWER_INHIBIT_REPROVISION;
        if (((uint32_t)owner == maintenance && (current & writers) != 0U) ||
            (((uint32_t)owner & writers) != 0U && (current & maintenance) != 0U)) {
            return false;
        }
        const uint_fast32_t desired = current | (uint32_t)owner;
        if (current == desired) {
            return true;
        }
        if (atomic_compare_exchange_weak_explicit(
                &s_shutdown_gate, &current, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            return true;
        }
    }
}

bool nuwax_power_ctrl_release_shutdown_inhibit(
    nuwax_power_inhibit_owner_t owner)
{
    if (!valid_inhibit_owner(owner)) {
        return false;
    }
    uint_fast32_t current = atomic_load_explicit(&s_shutdown_gate,
                                                 memory_order_acquire);
    for (;;) {
        if ((current & NUWAX_SHUTDOWN_GATE_CLAIMED) != 0U) {
            return false;
        }
        const uint_fast32_t desired = current & ~(uint32_t)owner;
        if (current == desired) {
            return true;
        }
        if (atomic_compare_exchange_weak_explicit(
                &s_shutdown_gate, &current, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            return true;
        }
    }
}

uint32_t nuwax_power_ctrl_shutdown_inhibit_owners(void)
{
    return (uint32_t)atomic_load_explicit(&s_shutdown_gate,
                                          memory_order_acquire) &
           NUWAX_SHUTDOWN_INHIBIT_OWNER_MASK;
}

bool nuwax_power_ctrl_shutdown_is_inhibited(void)
{
    return nuwax_power_ctrl_shutdown_inhibit_owners() != 0U;
}

#else /* not Miaopan or POWER_CTRL disabled */

bool nuwax_power_ctrl_prepare_shutdown(void) { return true; }
bool nuwax_power_ctrl_shutdown_preparing(void) { return false; }
void nuwax_power_ctrl_abort_shutdown(void) {}

esp_err_t nuwax_power_ctrl_init(void)
{
    ESP_LOGD(TAG, "Miaopan power control skipped for this board/profile");
    return ESP_OK;
}

void nuwax_power_ctrl_set_key_event_listener(
    nuwax_power_key_event_cb_t callback,
    void *ctx)
{
    (void)callback;
    (void)ctx;
}

bool nuwax_power_ctrl_commit_shutdown(bool force)
{
    (void)force;
    return false;
}

bool nuwax_power_ctrl_acquire_shutdown_inhibit(
    nuwax_power_inhibit_owner_t owner)
{
    (void)owner;
    return true;
}

bool nuwax_power_ctrl_release_shutdown_inhibit(
    nuwax_power_inhibit_owner_t owner)
{
    (void)owner;
    return true;
}

uint32_t nuwax_power_ctrl_shutdown_inhibit_owners(void)
{
    return 0U;
}

bool nuwax_power_ctrl_shutdown_is_inhibited(void)
{
    return false;
}

#endif
