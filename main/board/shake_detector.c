#include "board/shake_detector.h"
#include "board/shake_motion_policy.h"

#include <math.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "board/board_drivers.h"
#include "device_error_report.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nuwax_shake";

/* The sensor runs at a modest ODR, while this worker consumes only 20 samples
 * per second. A qualified event therefore costs one 12-byte I2C read every
 * 50 ms and no continuous queue or frame buffer. */
/* Reserve 1 KiB more PSRAM for periodic diagnostics and the I2C call chain;
 * no additional internal-heap allocation or per-sample allocation. */
#define NUWAX_SHAKE_TASK_STACK_BYTES 3072U
/* Release the shared I2C mutex promptly after its interrupt completes even
 * while LVGL (7) is rendering. Audio capture (10/11) keeps precedence. This
 * worker blocks between samples and performs no animation/network work. */
#define NUWAX_SHAKE_TASK_PRIORITY 8U
#define NUWAX_INTERACTION_GATE_FAILSAFE_MS 35000U
#define NUWAX_SHAKE_HEALTH_PERIOD_MS 10000U

_Static_assert(NUWAX_SHAKE_WINDOW_MS % NUWAX_SHAKE_SAMPLE_PERIOD_MS == 0U,
               "shake qualification must align to the sensor sample period");

typedef enum {
    NUWAX_INTERACTION_GATE_NONE = 0,
    NUWAX_INTERACTION_GATE_HEAD_TOUCH,
    NUWAX_INTERACTION_GATE_SHAKE,
} nuwax_interaction_gate_owner_t;

typedef struct {
    nuwax_shake_detector_event_cb_t callback;
    void *ctx;
    StaticTask_t *tcb;
    StackType_t *stack;
    esp_err_t startup_error;
} nuwax_shake_detector_runtime_t;

static nuwax_shake_detector_runtime_t s_runtime;
typedef enum {
    SHAKE_STOPPED = 0, SHAKE_STARTING, SHAKE_RUNNING, SHAKE_FAILED,
} shake_task_state_t;
static atomic_int s_task_state = ATOMIC_VAR_INIT(SHAKE_STOPPED);
static portMUX_TYPE s_interaction_gate_mux = portMUX_INITIALIZER_UNLOCKED;
static nuwax_interaction_gate_owner_t s_interaction_gate_owner;
static uint32_t s_interaction_gate_deadline_ms;
static uint32_t s_interaction_cooldown_until_ms;

static uint32_t interaction_cooldown_ms(
    nuwax_interaction_gate_owner_t owner) {
    return owner == NUWAX_INTERACTION_GATE_HEAD_TOUCH
               ? NUWAX_HEAD_TOUCH_INTERACTION_COOLDOWN_MS
               : NUWAX_SHAKE_INTERACTION_COOLDOWN_MS;
}

static bool interaction_gate_claim(nuwax_interaction_gate_owner_t owner,
                                   uint32_t now_ms) {
    bool claimed = false;
    taskENTER_CRITICAL(&s_interaction_gate_mux);
    if (s_interaction_gate_owner != NUWAX_INTERACTION_GATE_NONE &&
        (int32_t)(now_ms - s_interaction_gate_deadline_ms) >= 0) {
        s_interaction_gate_owner = NUWAX_INTERACTION_GATE_NONE;
        s_interaction_gate_deadline_ms = 0U;
    }
    const bool cooldown_active = s_interaction_cooldown_until_ms != 0U &&
        (int32_t)(now_ms - s_interaction_cooldown_until_ms) < 0;
    if (!cooldown_active) {
        s_interaction_cooldown_until_ms = 0U;
    }
    if (!cooldown_active &&
        s_interaction_gate_owner == NUWAX_INTERACTION_GATE_NONE) {
        s_interaction_gate_owner = owner;
        s_interaction_gate_deadline_ms =
            now_ms + NUWAX_INTERACTION_GATE_FAILSAFE_MS;
        if (s_interaction_gate_deadline_ms == 0U) {
            s_interaction_gate_deadline_ms = 1U;
        }
        const uint32_t cooldown_ms = interaction_cooldown_ms(owner);
        if (cooldown_ms > 0U) {
            s_interaction_cooldown_until_ms = now_ms + cooldown_ms;
            if (s_interaction_cooldown_until_ms == 0U) {
                s_interaction_cooldown_until_ms = 1U;
            }
        } else {
            s_interaction_cooldown_until_ms = 0U;
        }
        claimed = true;
    }
    taskEXIT_CRITICAL(&s_interaction_gate_mux);
    return claimed;
}

static void interaction_gate_release(nuwax_interaction_gate_owner_t owner) {
    taskENTER_CRITICAL(&s_interaction_gate_mux);
    if (s_interaction_gate_owner == owner) {
        s_interaction_gate_owner = NUWAX_INTERACTION_GATE_NONE;
        s_interaction_gate_deadline_ms = 0U;
    }
    taskEXIT_CRITICAL(&s_interaction_gate_mux);
}

static void shake_detector_task(void *arg) {
    (void)arg;
    const qmi8658a_config_t config = {
        .i2c_address = 0,
        .accel_range = QMI8658A_ACCEL_RANGE_8G,
        .gyro_range = QMI8658A_GYRO_RANGE_256DPS,
        .accel_odr = 62,
        .gyro_odr = 62,
        .use_interrupt = false,
        .int_gpio = -1,
    };
    const esp_err_t init_err = qmi8658a_init(&config);
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "QMI8658A shake detector unavailable err=%s; conversation continues without shake input",
                 esp_err_to_name(init_err));
        (void)nuwax_device_warning_report_enqueue(
            "motion", "sensor_unavailable", init_err, "shake", 0U, NULL);
        s_runtime.startup_error = init_err;
        atomic_store_explicit(&s_task_state, SHAKE_FAILED, memory_order_release);
        /* The static task stack cannot be freed from the task itself. The
         * worker exits once, so this bounded allocation is retained for the
         * lifetime of this boot. FAILED is terminal: a later start cannot
         * overwrite these pointers or reuse a task not yet reaped by idle. */
        vTaskDelete(NULL);
        return;
    }
    atomic_store_explicit(&s_task_state, SHAKE_RUNNING, memory_order_release);
    ESP_LOGI(TAG,
             "shake detector enabled sample_ms=%u window_ms=%u shake_cooldown_ms=%u head_touch_cooldown_ms=%u revision=20260928-shake-rearm-v3 sampling_revision=20260928-qmi-bounded-v2",
             (unsigned)NUWAX_SHAKE_SAMPLE_PERIOD_MS,
             (unsigned)NUWAX_SHAKE_WINDOW_MS,
             (unsigned)NUWAX_SHAKE_INTERACTION_COOLDOWN_MS,
             (unsigned)NUWAX_HEAD_TOUCH_INTERACTION_COOLDOWN_MS);

    ESP_LOGI(TAG, "shake pattern peak_mg=%u peaks=%u min_span_ms=%u peak_interval_ms=%u..%u rearm_still_ms=%u gyro_only=0",
             (unsigned)NUWAX_SHAKE_DYNAMIC_PEAK_MG, (unsigned)NUWAX_SHAKE_REQUIRED_PEAKS,
             (unsigned)NUWAX_SHAKE_MIN_SPAN_MS, (unsigned)NUWAX_SHAKE_MIN_PEAK_INTERVAL_MS,
             (unsigned)NUWAX_SHAKE_MAX_PEAK_INTERVAL_MS, (unsigned)NUWAX_SHAKE_REARM_STILL_MS);
    nuwax_shake_motion_t motion = {0};
    uint32_t attempts = 0U, read_errors = 0U, late_samples = 0U;
    uint32_t busy_skips = 0U;
    uint32_t slow_reads = 0U, read_max_ms = 0U;
    uint32_t qualified = 0U, suppressed = 0U, gyro_peak_dps = 0U;
    uint32_t health_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    esp_err_t last_error = ESP_OK;
    TickType_t wake_tick = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake_tick, pdMS_TO_TICKS(NUWAX_SHAKE_SAMPLE_PERIOD_MS));
        const TickType_t sample_tick = xTaskGetTickCount();
        if ((TickType_t)(sample_tick - wake_tick) >=
            pdMS_TO_TICKS(NUWAX_SHAKE_SAMPLE_PERIOD_MS)) {
            wake_tick = sample_tick;
            nuwax_shake_motion_invalidate(&motion);
            late_samples++;
        }
        qmi8658a_sensor_data_t sample = {0};
        const uint32_t read_started_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        const esp_err_t read_err = qmi8658a_read_data(&sample);
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        const uint32_t read_ms = now_ms - read_started_ms;
        if (read_ms > read_max_ms) read_max_ms = read_ms;
        const bool stale_sample = read_ms >= NUWAX_SHAKE_SAMPLE_PERIOD_MS;
        if (stale_sample) {
            /* An end-to-end late read cannot supply motion or quiet evidence.
             * Wait a full period; do not burst-read to catch up. */
            wake_tick = xTaskGetTickCount();
            nuwax_shake_motion_invalidate(&motion);
            slow_reads++;
        }
        attempts++;
        bool ready = false;
        if (read_err != ESP_OK) {
            if (read_err == ESP_ERR_NOT_FINISHED && !stale_sample) {
                nuwax_shake_motion_skip(&motion);
                busy_skips++;
            } else {
                nuwax_shake_motion_invalidate(&motion);
                read_errors++;
            }
            last_error = read_err;
        } else if (!stale_sample) {
            const float accel_mg[3] = {
                (float)sample.accel_x * sample.accel_scale,
                (float)sample.accel_y * sample.accel_scale,
                (float)sample.accel_z * sample.accel_scale,
            };
            const float gyro_sum_dps =
                (fabsf((float)sample.gyro_x) + fabsf((float)sample.gyro_y) +
                 fabsf((float)sample.gyro_z)) * sample.gyro_scale;
            if (gyro_sum_dps > (float)gyro_peak_dps)
                gyro_peak_dps = (uint32_t)gyro_sum_dps;
            ready = nuwax_shake_motion_observe(&motion, now_ms, accel_mg,
                        gyro_sum_dps, NUWAX_SHAKE_INTERACTION_COOLDOWN_MS);
        }
        if ((uint32_t)(now_ms - health_ms) >= NUWAX_SHAKE_HEALTH_PERIOD_MS) {
            ESP_LOGI(TAG,
                     "shake health attempts=%lu read_errors=%lu late=%lu candidate_peaks=%u peak_high=%u window_resets=%lu invalid_resets=%lu dynamic_peak_mg=%lu gyro_peak_dps=%lu qualified=%lu suppressed=%lu last_err=%s stack_min_free=%u slow_reads=%lu read_max_ms=%lu inhibited=%d busy_skips=%lu quiet_valid_ms=%lu",
                     (unsigned long)attempts, (unsigned long)read_errors,
                     (unsigned long)late_samples, (unsigned)motion.peaks,
                     (unsigned)motion.peak_high_water, (unsigned long)motion.window_resets,
                     (unsigned long)motion.invalid_resets, (unsigned long)motion.dynamic_peak_mg,
                     (unsigned long)gyro_peak_dps, (unsigned long)qualified,
                     (unsigned long)suppressed, esp_err_to_name(last_error),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned long)slow_reads, (unsigned long)read_max_ms,
                     motion.inhibited ? 1 : 0, (unsigned long)busy_skips,
                     (unsigned long)motion.quiet_valid_ms);
            health_ms = now_ms;
            attempts = read_errors = late_samples = qualified = suppressed = 0U;
            slow_reads = read_max_ms = 0U;
            busy_skips = 0U;
            motion.dynamic_peak_mg = gyro_peak_dps = 0U;
            motion.peak_high_water = motion.peaks;
            motion.window_resets = motion.invalid_resets = 0U;
            last_error = ESP_OK;
        }
        if (!ready) continue;
        if (!interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE, now_ms)) {
            suppressed++;
            ESP_LOGI(TAG, "qualified shake suppressed by active interaction or shared cooldown");
            continue;
        }
        qualified++;
        ESP_LOGI(TAG, "qualified shake pattern span_ms=%lu axis=%u",
                 (unsigned long)motion.last_gesture_ms, (unsigned)motion.axis);
        if (s_runtime.callback) s_runtime.callback(s_runtime.ctx);
        else interaction_gate_release(NUWAX_INTERACTION_GATE_SHAKE);
    }

}

esp_err_t nuwax_shake_detector_start(nuwax_shake_detector_event_cb_t callback,
                                     void *ctx) {
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    int expected = SHAKE_STOPPED;
    if (!atomic_compare_exchange_strong_explicit(&s_task_state, &expected,
            SHAKE_STARTING, memory_order_acq_rel, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.callback = callback;
    s_runtime.ctx = ctx;
    s_runtime.tcb = heap_caps_malloc(sizeof(StaticTask_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_runtime.stack = heap_caps_malloc(NUWAX_SHAKE_TASK_STACK_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_runtime.tcb || !s_runtime.stack) {
        ESP_LOGW(TAG, "shake detector task memory unavailable; feature disabled");
        (void)nuwax_device_warning_report_enqueue(
            "motion", "task_memory_unavailable", ESP_ERR_NO_MEM,
            "shake", 0U, NULL);
        free(s_runtime.tcb);
        free(s_runtime.stack);
        memset(&s_runtime, 0, sizeof(s_runtime));
        atomic_store_explicit(&s_task_state, SHAKE_STOPPED, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    TaskHandle_t created = xTaskCreateStatic(shake_detector_task,
                                       "nuwax_shake",
                                       NUWAX_SHAKE_TASK_STACK_BYTES,
                                       NULL,
                                       NUWAX_SHAKE_TASK_PRIORITY,
                                       s_runtime.stack,
                                       s_runtime.tcb);
    if (!created) {
        free(s_runtime.tcb);
        free(s_runtime.stack);
        memset(&s_runtime, 0, sizeof(s_runtime));
        atomic_store_explicit(&s_task_state, SHAKE_STOPPED, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    if (atomic_load_explicit(&s_task_state, memory_order_acquire) == SHAKE_FAILED)
        return s_runtime.startup_error;
    return ESP_OK;
}

bool nuwax_shake_detector_is_running(void) {
    return atomic_load_explicit(&s_task_state, memory_order_acquire) == SHAKE_RUNNING;
}

bool nuwax_shake_detector_claim_head_touch(void) {
    return interaction_gate_claim(
        NUWAX_INTERACTION_GATE_HEAD_TOUCH,
        (uint32_t)(esp_timer_get_time() / 1000ULL));
}

void nuwax_shake_detector_release_head_touch(void) {
    interaction_gate_release(NUWAX_INTERACTION_GATE_HEAD_TOUCH);
}

void nuwax_shake_detector_release_shake(void) {
    interaction_gate_release(NUWAX_INTERACTION_GATE_SHAKE);
}
