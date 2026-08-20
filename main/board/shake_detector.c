#include "board/shake_detector.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "board/board_drivers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nuwax_shake";

/* The sensor runs at a modest ODR, while this worker consumes only 20 samples
 * per second. A qualified event therefore costs one 12-byte I2C read every
 * 50 ms and no continuous queue or frame buffer. */
#define NUWAX_SHAKE_SAMPLE_PERIOD_MS 50U
#define NUWAX_SHAKE_REQUIRED_MS 2000U
#define NUWAX_SHAKE_GAP_TOLERANCE_MS 300U
#define NUWAX_SHAKE_ACCEL_DELTA_THRESHOLD_MG 700.0f
#define NUWAX_SHAKE_GYRO_SUM_THRESHOLD_DPS 150.0f
#define NUWAX_SHAKE_TASK_STACK_BYTES 2048U
#define NUWAX_SHAKE_TASK_PRIORITY 2U
#define NUWAX_INTERACTION_GATE_FAILSAFE_MS 35000U

typedef enum {
    NUWAX_INTERACTION_GATE_NONE = 0,
    NUWAX_INTERACTION_GATE_HEAD_TOUCH,
    NUWAX_INTERACTION_GATE_SHAKE,
} nuwax_interaction_gate_owner_t;

typedef struct {
    nuwax_shake_detector_event_cb_t callback;
    void *ctx;
    TaskHandle_t task;
    StaticTask_t *tcb;
    StackType_t *stack;
    bool running;
} nuwax_shake_detector_runtime_t;

static nuwax_shake_detector_runtime_t s_runtime;
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

static bool sample_is_active(const qmi8658a_sensor_data_t *sample,
                             const qmi8658a_sensor_data_t *previous) {
    if (!sample || !previous) {
        return false;
    }
    const float accel_delta_mg =
        (fabsf((float)sample->accel_x - (float)previous->accel_x) +
         fabsf((float)sample->accel_y - (float)previous->accel_y) +
         fabsf((float)sample->accel_z - (float)previous->accel_z)) *
        sample->accel_scale;
    const float gyro_sum_dps =
        (fabsf((float)sample->gyro_x) + fabsf((float)sample->gyro_y) +
         fabsf((float)sample->gyro_z)) * sample->gyro_scale;
    return accel_delta_mg >= NUWAX_SHAKE_ACCEL_DELTA_THRESHOLD_MG ||
           gyro_sum_dps >= NUWAX_SHAKE_GYRO_SUM_THRESHOLD_DPS;
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
        s_runtime.running = false;
        s_runtime.task = NULL;
        /* The static task stack cannot be freed from the task itself. The
         * worker exits once, so this bounded allocation is retained for the
         * lifetime of the firmware and prevents a retry race. */
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG,
             "shake detector enabled sample_ms=%u required_ms=%u shake_cooldown_ms=%u head_touch_cooldown_ms=%u",
             (unsigned)NUWAX_SHAKE_SAMPLE_PERIOD_MS,
             (unsigned)NUWAX_SHAKE_REQUIRED_MS,
             (unsigned)NUWAX_SHAKE_INTERACTION_COOLDOWN_MS,
             (unsigned)NUWAX_HEAD_TOUCH_INTERACTION_COOLDOWN_MS);

    qmi8658a_sensor_data_t previous = {0};
    bool have_previous = false;
    uint32_t active_ms = 0U;
    uint32_t quiet_ms = 0U;
    uint32_t cooldown_until_ms = 0U;
    TickType_t wake_tick = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake_tick,
                        pdMS_TO_TICKS(NUWAX_SHAKE_SAMPLE_PERIOD_MS));
        qmi8658a_sensor_data_t sample = {0};
        const esp_err_t read_err = qmi8658a_read_data(&sample);
        if (read_err != ESP_OK) {
            have_previous = false;
            active_ms = 0U;
            quiet_ms = 0U;
            continue;
        }
        if (!have_previous) {
            previous = sample;
            have_previous = true;
            continue;
        }
        const bool active = sample_is_active(&sample, &previous);
        previous = sample;
        const uint32_t now_ms =
            (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (active) {
            quiet_ms = 0U;
            if (active_ms < NUWAX_SHAKE_REQUIRED_MS) {
                active_ms += NUWAX_SHAKE_SAMPLE_PERIOD_MS;
            }
        } else {
            quiet_ms += NUWAX_SHAKE_SAMPLE_PERIOD_MS;
            if (quiet_ms > NUWAX_SHAKE_GAP_TOLERANCE_MS) {
                active_ms = 0U;
            }
        }
        if (active_ms < NUWAX_SHAKE_REQUIRED_MS ||
            (cooldown_until_ms != 0U &&
             (int32_t)(now_ms - cooldown_until_ms) < 0)) {
            continue;
        }
        active_ms = 0U;
        quiet_ms = 0U;
        cooldown_until_ms =
            now_ms + NUWAX_SHAKE_INTERACTION_COOLDOWN_MS;
        if (cooldown_until_ms == 0U) {
            cooldown_until_ms = 1U;
        }
        if (!interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE, now_ms)) {
            ESP_LOGI(TAG,
                     "qualified shake suppressed by active interaction or shared cooldown");
            continue;
        }
        ESP_LOGI(TAG, "qualified shake duration_ms=%u", (unsigned)NUWAX_SHAKE_REQUIRED_MS);
        if (s_runtime.callback) {
            s_runtime.callback(s_runtime.ctx);
        } else {
            interaction_gate_release(NUWAX_INTERACTION_GATE_SHAKE);
        }
    }
}

esp_err_t nuwax_shake_detector_start(nuwax_shake_detector_event_cb_t callback,
                                     void *ctx) {
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_runtime.task) {
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
        free(s_runtime.tcb);
        free(s_runtime.stack);
        memset(&s_runtime, 0, sizeof(s_runtime));
        return ESP_ERR_NO_MEM;
    }
    s_runtime.task = xTaskCreateStatic(shake_detector_task,
                                       "nuwax_shake",
                                       NUWAX_SHAKE_TASK_STACK_BYTES,
                                       NULL,
                                       NUWAX_SHAKE_TASK_PRIORITY,
                                       s_runtime.stack,
                                       s_runtime.tcb);
    if (!s_runtime.task) {
        free(s_runtime.tcb);
        free(s_runtime.stack);
        memset(&s_runtime, 0, sizeof(s_runtime));
        return ESP_ERR_NO_MEM;
    }
    s_runtime.running = true;
    return ESP_OK;
}

bool nuwax_shake_detector_is_running(void) {
    return s_runtime.running;
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
