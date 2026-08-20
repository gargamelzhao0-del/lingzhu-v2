#include "battery_service.h"

#include <string.h>

#include "board_drivers.h"
#include "battery_policy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "battery_service";

#define BATTERY_SAMPLE_MIN_MS       5000U
#define BATTERY_SAMPLE_MAX_MS       10000U
#define BATTERY_BACKOFF_DEFAULT_MS  5000U
#define BATTERY_BACKOFF_MAX_MS      60000U
#define BATTERY_RECOVERY_ERRORS     3U
#define BATTERY_TASK_STACK_BYTES    4096U
#define BATTERY_TASK_PRIORITY       4U
#define BATTERY_SNAPSHOT_MAX_PERIODS 3U
#define BATTERY_CHARGING_EXIT_SAMPLES 2U
#define BATTERY_DISPLAY_MAX_STEP_PERCENT 2U

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t stopped;
    TaskHandle_t task;
    nuwax_battery_service_config_t config;
    nuwax_battery_service_snapshot_t snapshot;
    bool running;
    bool stop_requested;
    bool low_latched;
    bool capacity_mismatch_logged;
    bool first_sample_logged;
    bool charging_latched;
    uint8_t charging_inactive_samples;
    bool display_percent_initialized;
    uint8_t display_percent;
    nuwax_battery_low_reason_t low_reason;
    int32_t average_current_ma;
    uint8_t average_current_samples;
} battery_service_context_t;

static battery_service_context_t s_service;
static StaticSemaphore_t s_service_lock_storage;
static StaticSemaphore_t s_service_stopped_storage;
static portMUX_TYPE s_service_init_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t ensure_service_sync_objects(void)
{
    taskENTER_CRITICAL(&s_service_init_mux);
    if (!s_service.lock) {
        s_service.lock = xSemaphoreCreateMutexStatic(&s_service_lock_storage);
    }
    if (!s_service.stopped) {
        s_service.stopped =
            xSemaphoreCreateBinaryStatic(&s_service_stopped_storage);
    }
    const bool ready = s_service.lock != NULL && s_service.stopped != NULL;
    taskEXIT_CRITICAL(&s_service_init_mux);
    return ready ? ESP_OK : ESP_ERR_NO_MEM;
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint32_t saturating_double(uint32_t value, uint32_t limit)
{
    if (value >= limit || value > UINT32_MAX / 2U) return limit;
    value *= 2U;
    return value > limit ? limit : value;
}

static nuwax_battery_service_config_t resolve_config(
    const nuwax_battery_service_config_t *requested)
{
    nuwax_battery_service_config_t config = NUWAX_BATTERY_SERVICE_DEFAULT_CONFIG();
    if (!requested) return config;

    if (requested->sample_interval_ms) {
        config.sample_interval_ms = requested->sample_interval_ms;
    }
    if (requested->error_backoff_initial_ms) {
        config.error_backoff_initial_ms = requested->error_backoff_initial_ms;
    }
    if (requested->error_backoff_max_ms) {
        config.error_backoff_max_ms = requested->error_backoff_max_ms;
    }
    if (requested->low_enter_percent) {
        config.low_enter_percent = requested->low_enter_percent;
    }
    if (requested->low_exit_percent) {
        config.low_exit_percent = requested->low_exit_percent;
    }
    if (requested->hard_low_enter_mv) {
        config.hard_low_enter_mv = requested->hard_low_enter_mv;
    }
    if (requested->hard_low_exit_mv) {
        config.hard_low_exit_mv = requested->hard_low_exit_mv;
    }
    if (requested->estimated_low_enter_mv) {
        config.estimated_low_enter_mv = requested->estimated_low_enter_mv;
    }
    if (requested->estimated_low_exit_mv) {
        config.estimated_low_exit_mv = requested->estimated_low_exit_mv;
    }
    if (requested->design_capacity_mah) {
        config.design_capacity_mah = requested->design_capacity_mah;
    }
    config.listener = requested->listener;
    config.listener_user_data = requested->listener_user_data;
    return config;
}

static esp_err_t validate_config(const nuwax_battery_service_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (config->sample_interval_ms < BATTERY_SAMPLE_MIN_MS ||
        config->sample_interval_ms > BATTERY_SAMPLE_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->error_backoff_initial_ms < BATTERY_SAMPLE_MIN_MS ||
        config->error_backoff_max_ms < config->error_backoff_initial_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->low_enter_percent > 100U ||
        config->low_exit_percent > 100U ||
        config->low_exit_percent <= config->low_enter_percent) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->hard_low_enter_mv < 3000U ||
        config->hard_low_exit_mv <= config->hard_low_enter_mv ||
        config->estimated_low_enter_mv < config->hard_low_enter_mv ||
        config->estimated_low_exit_mv <= config->estimated_low_enter_mv ||
        config->estimated_low_exit_mv > 4300U) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool stop_requested(void)
{
    bool requested = true;
    if (!s_service.lock) return true;
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    requested = s_service.stop_requested;
    xSemaphoreGive(s_service.lock);
    return requested;
}

static void notify_listener(const nuwax_battery_service_snapshot_t *snapshot)
{
    nuwax_battery_service_listener_t listener = NULL;
    void *user_data = NULL;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    listener = s_service.config.listener;
    user_data = s_service.config.listener_user_data;
    xSemaphoreGive(s_service.lock);

    if (listener) listener(snapshot, user_data);
}

static nuwax_battery_power_state_t classify_power(
    const bq27220_battery_status_t *status,
    bool charging,
    bool low)
{
    /* BATTPRES only describes the gauge's battery detection state.  This
     * board has no MCU-visible VBUS signal, so an absent battery cannot be
     * used as evidence that stable external power is present. */
    if (!status->battery_present) return NUWAX_BATTERY_POWER_UNKNOWN;
    if (charging) return NUWAX_BATTERY_POWER_CHARGING;
    if (low) return NUWAX_BATTERY_POWER_LOW;
    return NUWAX_BATTERY_POWER_BATTERY;
}

static nuwax_battery_service_snapshot_t commit_success(
    const bq27220_battery_status_t *status)
{
    nuwax_battery_service_snapshot_t published;
    const uint8_t percent = status->soc > 100U ? 100U : status->soc;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    const bool capacity_matches = status->design_capacity_mah ==
                                  s_service.config.design_capacity_mah;
    const bool gauge_ready = status->initialization_complete &&
                             !status->config_update_mode;
    const bool measurements_plausible =
        status->voltage_mv >= 2500U && status->voltage_mv <= 5000U &&
        status->temperature_c >= -30 && status->temperature_c <= 90 &&
        status->design_capacity_mah >= 100U &&
        status->design_capacity_mah <= 20000U &&
        status->full_charge_mah >= status->design_capacity_mah / 5U &&
        status->full_charge_mah <=
            (uint32_t)status->design_capacity_mah * 5U / 4U &&
        status->remaining_mah <= status->full_charge_mah + 50U;
    const bool sample_valid = status->battery_present && gauge_ready &&
                              measurements_plausible;
    const bool soc_trusted = sample_valid && capacity_matches &&
                             status->profile_verified;
    const nuwax_battery_low_policy_input_t low_input = {
        .battery_present = status->battery_present,
        .soc_trusted = soc_trusted,
        .gauge_low_alarm = status->is_low,
        .percent = percent,
        .voltage_mv = status->voltage_mv,
        .low_enter_percent = s_service.config.low_enter_percent,
        .low_exit_percent = s_service.config.low_exit_percent,
        .hard_low_enter_mv = s_service.config.hard_low_enter_mv,
        .hard_low_exit_mv = s_service.config.hard_low_exit_mv,
        .estimated_low_enter_mv = s_service.config.estimated_low_enter_mv,
        .estimated_low_exit_mv = s_service.config.estimated_low_exit_mv,
    };
    nuwax_battery_low_policy_result_t low_result = {
        .low = s_service.low_latched,
        .hard_low_voltage = s_service.snapshot.hard_low_voltage,
        .reason = s_service.low_reason,
    };
    if (!status->battery_present) {
        low_result = nuwax_battery_low_policy_update(
            false, NUWAX_BATTERY_LOW_REASON_NONE, &low_input);
    } else if (sample_valid) {
        low_result = nuwax_battery_low_policy_update(
            s_service.low_latched, s_service.low_reason, &low_input);
    }
    const bool low_changed = s_service.low_latched != low_result.low ||
                             s_service.low_reason != low_result.reason;
    s_service.low_latched = low_result.low;
    s_service.low_reason = low_result.reason;
    if (!sample_valid || status->is_full) {
        s_service.charging_latched = false;
        s_service.charging_inactive_samples = 0U;
    } else {
        s_service.charging_latched = nuwax_battery_charging_latch_update(
            s_service.charging_latched, status->is_charging,
            &s_service.charging_inactive_samples,
            BATTERY_CHARGING_EXIT_SAMPLES);
    }
    if (sample_valid) {
        const uint8_t display_input = status->is_full ? 100U : percent;
        s_service.display_percent = nuwax_battery_display_percent_update(
            s_service.display_percent_initialized,
            soc_trusted || status->is_full,
            s_service.display_percent, display_input,
            BATTERY_DISPLAY_MAX_STEP_PERCENT);
        s_service.display_percent_initialized = true;
    } else if (!status->battery_present) {
        s_service.display_percent_initialized = false;
        s_service.display_percent = 0U;
    }
    const bool log_capacity_mismatch = !capacity_matches &&
                                       !s_service.capacity_mismatch_logged;
    const bool log_first_sample = !s_service.first_sample_logged;
    const bool current_direction_changed =
        (s_service.average_current_ma > 20 && status->current_ma < -20) ||
        (s_service.average_current_ma < -20 && status->current_ma > 20);
    if (s_service.average_current_samples == 0U || current_direction_changed) {
        s_service.average_current_ma = status->current_ma;
        s_service.average_current_samples = 1U;
    } else {
        s_service.average_current_ma =
            (s_service.average_current_ma * 3 + status->current_ma) / 4;
        if (s_service.average_current_samples < UINT8_MAX) {
            s_service.average_current_samples++;
        }
    }
    s_service.capacity_mismatch_logged = !capacity_matches;
    s_service.first_sample_logged = true;
    s_service.snapshot.valid = sample_valid;
    s_service.snapshot.sample_available = true;
    s_service.snapshot.stale = false;
    s_service.snapshot.battery_present = status->battery_present;
    s_service.snapshot.gauge_ready = gauge_ready;
    s_service.snapshot.soc_trusted = soc_trusted;
    s_service.snapshot.charging = s_service.charging_latched;
    s_service.snapshot.full = status->is_full;
    s_service.snapshot.low = s_service.low_latched;
    s_service.snapshot.hard_low_voltage = low_result.hard_low_voltage;
    s_service.snapshot.percent = percent;
    s_service.snapshot.display_percent = s_service.display_percent;
    s_service.snapshot.voltage_mv = status->voltage_mv;
    s_service.snapshot.current_ma = status->current_ma;
    s_service.snapshot.average_current_valid =
        s_service.average_current_samples >= 4U;
    s_service.snapshot.average_current_ma =
        (int16_t)s_service.average_current_ma;
    s_service.snapshot.temperature_c = status->temperature_c;
    s_service.snapshot.remaining_mah = status->remaining_mah;
    s_service.snapshot.full_charge_mah = status->full_charge_mah;
    s_service.snapshot.design_capacity_mah = status->design_capacity_mah;
    s_service.snapshot.cycle_count = status->cycle_count;
    s_service.snapshot.power_state = classify_power(
        status, s_service.charging_latched, s_service.low_latched);
    s_service.snapshot.sample_timestamp_ms = status->timestamp_ms;
    s_service.snapshot.last_attempt_timestamp_ms = now_ms();
    s_service.snapshot.consecutive_errors = 0;
    s_service.snapshot.last_error = ESP_OK;
    published = s_service.snapshot;
    xSemaphoreGive(s_service.lock);
    if (log_capacity_mismatch) {
        ESP_LOGW(TAG, "fuel-gauge profile capacity mismatch actual=%u expected=%u; SOC remains best-effort",
                 status->design_capacity_mah,
                 s_service.config.design_capacity_mah);
    }
    if (low_changed) {
        ESP_LOGI(TAG,
                 "product low state=%d reason=%d trusted=%d soc=%u%% voltage=%umV gauge_low=%d",
                 published.low ? 1 : 0, (int)low_result.reason,
                 published.soc_trusted ? 1 : 0, published.percent,
                 published.voltage_mv, status->is_low ? 1 : 0);
    }
    if (log_first_sample) {
        ESP_LOGI(TAG,
                 "first sample valid=%d plausible=%d trusted=%d soc=%u%% display=%u%% voltage=%umV current=%dmA remaining=%umAh full=%umAh design=%umAh temp=%dC profile_verified=%d battery_present=%d cfgupdate=%d operation=0x%04x",
                 published.valid ? 1 : 0,
                 measurements_plausible ? 1 : 0,
                 published.soc_trusted ? 1 : 0,
                 published.percent,
                 published.display_percent,
                 published.voltage_mv,
                 published.current_ma,
                 published.remaining_mah,
                 published.full_charge_mah,
                 status->design_capacity_mah,
                 published.temperature_c,
                 status->profile_verified ? 1 : 0,
                 status->battery_present ? 1 : 0,
                 status->config_update_mode ? 1 : 0,
                 status->operation_status_flags);
    }
    return published;
}

static nuwax_battery_service_snapshot_t commit_error(esp_err_t error)
{
    nuwax_battery_service_snapshot_t published;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    /* Do not clear the last valid measurements. A consumer can keep showing
     * them while stale/consecutive_errors expose the degraded I2C state. */
    const uint64_t current_ms = now_ms();
    const uint64_t stale_after_ms =
        (uint64_t)s_service.config.sample_interval_ms *
        BATTERY_SNAPSHOT_MAX_PERIODS;
    s_service.snapshot.stale =
        s_service.snapshot.sample_available &&
        (s_service.snapshot.sample_timestamp_ms == 0U ||
         current_ms < s_service.snapshot.sample_timestamp_ms ||
         current_ms - s_service.snapshot.sample_timestamp_ms > stale_after_ms);
    s_service.snapshot.last_attempt_timestamp_ms = current_ms;
    if (s_service.snapshot.consecutive_errors < UINT32_MAX) {
        s_service.snapshot.consecutive_errors++;
    }
    s_service.snapshot.last_error = error;
    published = s_service.snapshot;
    xSemaphoreGive(s_service.lock);
    return published;
}

static void wait_or_stop(uint32_t delay_ms)
{
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
}

static void battery_task(void *argument)
{
    (void)argument;
    bool driver_ready = false;
    bool resource_logged = false;
    uint32_t backoff_ms = s_service.config.error_backoff_initial_ms;

    while (!stop_requested()) {
        esp_err_t err = ESP_OK;
        if (!driver_ready) {
            const bq27220_config_t driver_config = {
                .i2c_address = 0,
                .design_capacity = s_service.config.design_capacity_mah,
                .low_battery_threshold = s_service.config.low_enter_percent,
            };
            err = bq27220_init(&driver_config);
            driver_ready = err == ESP_OK;
        }

        bq27220_battery_status_t raw = {0};
        if (err == ESP_OK) err = bq27220_get_status(&raw);
        if (!resource_logged) {
            resource_logged = true;
            ESP_LOGI(TAG,
                     "task resources stack=psram bytes=%u stack_min_free=%u",
                     (unsigned)BATTERY_TASK_STACK_BYTES,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        if (err == ESP_OK) {
            const nuwax_battery_service_snapshot_t published = commit_success(&raw);
            notify_listener(&published);
            backoff_ms = s_service.config.error_backoff_initial_ms;
            wait_or_stop(s_service.config.sample_interval_ms);
            continue;
        }

        const nuwax_battery_service_snapshot_t published = commit_error(err);
        ESP_LOGW(TAG,
                 "sample failed err=%s consecutive=%lu retry_ms=%lu retained_valid=%d",
                 esp_err_to_name(err),
                 (unsigned long)published.consecutive_errors,
                 (unsigned long)backoff_ms,
                 published.valid ? 1 : 0);
        notify_listener(&published);

        if (published.consecutive_errors >= BATTERY_RECOVERY_ERRORS && driver_ready) {
            esp_err_t deinit_err = bq27220_deinit();
            if (deinit_err == ESP_OK) {
                driver_ready = false;
            } else {
                ESP_LOGE(TAG, "fuel-gauge recovery deinit failed: %s",
                         esp_err_to_name(deinit_err));
            }
        }
        wait_or_stop(backoff_ms);
        backoff_ms = saturating_double(backoff_ms,
                                       s_service.config.error_backoff_max_ms);
    }

    if (driver_ready) {
        esp_err_t deinit_err = bq27220_deinit();
        if (deinit_err != ESP_OK) {
            ESP_LOGE(TAG, "fuel-gauge stop deinit failed: %s",
                     esp_err_to_name(deinit_err));
        }
    }
    ESP_LOGI(TAG, "stopped task_stack=psram stack_min_free=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(s_service.stopped);
    /* WithCaps stacks are owned by the creating API.  Park here so the
     * stopping task can delete us without the self-delete cleanup task and
     * its temporary internal stack. */
    vTaskSuspend(NULL);
}

esp_err_t nuwax_battery_service_start(const nuwax_battery_service_config_t *config)
{
    const nuwax_battery_service_config_t resolved = resolve_config(config);
    esp_err_t err = validate_config(&resolved);
    if (err != ESP_OK) return err;

    err = ensure_service_sync_objects();
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.running) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreTake(s_service.stopped, 0);
    s_service.config = resolved;
    memset(&s_service.snapshot, 0, sizeof(s_service.snapshot));
    s_service.snapshot.power_state = NUWAX_BATTERY_POWER_UNKNOWN;
    s_service.snapshot.last_error = ESP_ERR_INVALID_STATE;
    s_service.stop_requested = false;
    s_service.low_latched = false;
    s_service.capacity_mismatch_logged = false;
    s_service.first_sample_logged = false;
    s_service.average_current_ma = 0;
    s_service.average_current_samples = 0U;
    s_service.charging_latched = false;
    s_service.charging_inactive_samples = 0U;
    s_service.display_percent_initialized = false;
    s_service.display_percent = 0U;
    s_service.low_reason = NUWAX_BATTERY_LOW_REASON_NONE;

    BaseType_t created = xTaskCreateWithCaps(
        battery_task,
        "battery_service",
        BATTERY_TASK_STACK_BYTES,
        NULL,
        BATTERY_TASK_PRIORITY,
        &s_service.task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_service.task = NULL;
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_NO_MEM;
    }
    s_service.running = true;
    xSemaphoreGive(s_service.lock);

    ESP_LOGI(TAG,
             "started sample_ms=%lu backoff_ms=%lu..%lu low=%u/%u voltage=%u/%u/%u/%u task_stack=psram bytes=%u",
             (unsigned long)resolved.sample_interval_ms,
             (unsigned long)resolved.error_backoff_initial_ms,
             (unsigned long)resolved.error_backoff_max_ms,
             resolved.low_enter_percent,
             resolved.low_exit_percent,
             resolved.hard_low_enter_mv,
             resolved.hard_low_exit_mv,
             resolved.estimated_low_enter_mv,
             resolved.estimated_low_exit_mv,
             (unsigned)BATTERY_TASK_STACK_BYTES);
    return ESP_OK;
}

esp_err_t nuwax_battery_service_stop(uint32_t timeout_ms)
{
    if (!s_service.lock || !s_service.stopped) return ESP_OK;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (!s_service.running) {
        xSemaphoreGive(s_service.lock);
        return ESP_OK;
    }
    s_service.stop_requested = true;
    TaskHandle_t task = s_service.task;
    xSemaphoreGive(s_service.lock);

    if (task) xTaskNotifyGive(task);
    if (xSemaphoreTake(s_service.stopped, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (task) {
        vTaskDeleteWithCaps(task);
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.task == task) {
        s_service.task = NULL;
        s_service.running = false;
    }
    xSemaphoreGive(s_service.lock);
    return ESP_OK;
}

esp_err_t nuwax_battery_service_get_snapshot(nuwax_battery_service_snapshot_t *snapshot)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    if (!s_service.lock) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    *snapshot = s_service.snapshot;
    const uint32_t sample_interval_ms = s_service.config.sample_interval_ms;
    xSemaphoreGive(s_service.lock);

    if (snapshot->sample_available) {
        const uint64_t current_ms = now_ms();
        const uint64_t max_age_ms =
            (uint64_t)sample_interval_ms * BATTERY_SNAPSHOT_MAX_PERIODS;
        if (snapshot->sample_timestamp_ms == 0 ||
            current_ms < snapshot->sample_timestamp_ms ||
            current_ms - snapshot->sample_timestamp_ms > max_age_ms) {
            snapshot->stale = true;
        }
    }
    return ESP_OK;
}

bool nuwax_battery_service_is_running(void)
{
    if (!s_service.lock) return false;
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    const bool running = s_service.running;
    xSemaphoreGive(s_service.lock);
    return running;
}
