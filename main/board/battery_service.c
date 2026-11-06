#include "battery_service.h"
#include "low_battery_shutdown_policy.h"

#include <string.h>

#include "board_drivers.h"
#include "battery_policy.h"
#include "battery_history.h"
#include "i2c_bus_manager.h"
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
#define BATTERY_DIAGNOSTIC_INTERVAL_MS 30000U

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
static nuwax_battery_history_t s_charge_history;
static StaticSemaphore_t s_service_lock_storage;
static StaticSemaphore_t s_service_stopped_storage;
static StaticSemaphore_t s_stop_lock_storage;
static SemaphoreHandle_t s_stop_lock;
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
    if (!s_stop_lock) s_stop_lock = xSemaphoreCreateMutexStatic(&s_stop_lock_storage);
    const bool ready = s_service.lock != NULL && s_service.stopped != NULL && s_stop_lock != NULL;
    taskEXIT_CRITICAL(&s_service_init_mux);
    return ready ? ESP_OK : ESP_ERR_NO_MEM;
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

typedef struct {
    bool initialized;
    uint32_t state;
    uint64_t last_log_ms;
} battery_diagnostic_t;

/* Only the battery task owns this history. Log existing samples outside the
 * service/I2C locks; diagnostics must not add bus traffic or change policy. */
static void log_charge_diagnostic(battery_diagnostic_t *history,
                                  const nuwax_battery_service_snapshot_t *sample,
                                  const bq27220_battery_status_t *raw,
                                  esp_err_t read_error)
{
    const uint64_t current_ms = now_ms();
    const bool flags_valid = raw->power_sample_valid;
    const uint16_t flags = flags_valid ? raw->battery_status_flags : 0U;
    /* Preserve temperature advisories independently of measured charging. */
    const uint32_t state = (flags & 0x0F01U) |
        ((uint32_t)flags_valid << 16) |
        ((uint32_t)raw->telemetry_valid << 17) |
        ((uint32_t)sample->valid << 18) |
        ((uint32_t)sample->stale << 19) |
        ((uint32_t)sample->charging << 20) |
        ((uint32_t)sample->full << 21) |
        ((uint32_t)sample->power_valid << 22) |
        ((uint32_t)sample->power_stale << 23) |
        ((uint32_t)sample->soc_trusted << 24) |
        ((uint32_t)sample->battery_present << 25);
    const bool changed = history->initialized && history->state != state;
    if (history->initialized && !changed &&
        current_ms >= history->last_log_ms &&
        current_ms - history->last_log_ms < BATTERY_DIAGNOSTIC_INTERVAL_MS) {
        return;
    }
    const char *reason = !history->initialized ? "first" :
                         changed ? "state_change" : "periodic";
    const int64_t age_ms = sample->sample_timestamp_ms != 0U &&
                          current_ms >= sample->sample_timestamp_ms
                              ? (int64_t)(current_ms - sample->sample_timestamp_ms)
                              : -1;
    ESP_LOGI(TAG,
             "charge diag reason=%s valid=%d stale=%d telemetry_valid=%d age_ms=%lld soc=%u%% display=%u%% voltage=%umV current=%dmA avg_current=%dmA avg_valid=%d remaining=%umAh fcc=%umAh design=%umAh temp=%dC charging=%d full=%d trusted=%d flags_valid=%d flags=0x%04x FC=%d CHGINH=%d DSG=%d OTC=%d OTD=%d operation=0x%04x power_valid=%d power_stale=%d err=%s",
             reason, sample->valid, sample->stale, raw->telemetry_valid,
             (long long)age_ms, sample->percent, sample->display_percent,
             sample->voltage_mv, sample->current_ma,
             sample->average_current_ma, sample->average_current_valid,
             sample->remaining_mah, sample->full_charge_mah,
             sample->design_capacity_mah, sample->temperature_c,
             sample->charging, sample->full, sample->soc_trusted,
             flags_valid, flags,
             flags_valid ? !!(flags & (1U << 9)) : -1,
             flags_valid ? !!(flags & (1U << 8)) : -1,
             flags_valid ? !!(flags & (1U << 0)) : -1,
             flags_valid ? !!(flags & (1U << 11)) : -1,
             flags_valid ? !!(flags & (1U << 10)) : -1,
             raw->operation_status_flags, sample->power_valid,
             sample->power_stale, esp_err_to_name(read_error));
    history->initialized = true;
    history->state = state;
    history->last_log_ms = current_ms;
}

/* Capture every attempt, including failures, before periodic logging. No
 * additional I2C, storage writes, USB detection or dynamic allocation. A
 * reconnecting console receives the retained records at the next timed dump. */
static void record_charge_history(
    const nuwax_battery_service_snapshot_t *sample,
    const bq27220_battery_status_t *raw, esp_err_t error)
{
    const nuwax_battery_history_sample_t entry = {
        .timestamp_ms = now_ms(),
        .error = error,
        .voltage_mv = sample->voltage_mv,
        .current_ma = sample->current_ma,
        .remaining_mah = sample->remaining_mah,
        .full_charge_mah = sample->full_charge_mah,
        .flags = raw->power_sample_valid ? raw->battery_status_flags : 0U,
        .temperature_c = sample->temperature_c,
        .percent = sample->percent,
        .display_percent = sample->display_percent,
        .quality = (raw->telemetry_valid ? 1U : 0U) |
                   (raw->power_sample_valid ? 2U : 0U) |
                   (sample->valid ? 4U : 0U) | (sample->stale ? 8U : 0U),
        .state = (sample->charging ? 1U : 0U) | (sample->full ? 2U : 0U),
    };
    nuwax_battery_history_append(&s_charge_history, entry);
    if (!nuwax_battery_history_dump_due(&s_charge_history, entry.timestamp_ms)) return;

    ESP_LOGI(TAG, "charge history begin revision=charge-history-v1 count=%u now_ms=%llu columns=seq,t_ms,mV,mA,soc,display,temp_C,remaining_mAh,fcc_mAh,flags_hex,quality_hex,state_hex,err_hex",
             (unsigned)s_charge_history.count, (unsigned long long)entry.timestamp_ms);
    for (unsigned i = 0; i < s_charge_history.count; ++i) {
        const nuwax_battery_history_sample_t *row =
            nuwax_battery_history_get(&s_charge_history, i);
        ESP_LOGI(TAG, "charge history row=%lu,%llu,%u,%d,%u,%u,%d,%u,%u,%04x,%02x,%02x,%lx",
                 (unsigned long)row->sequence, (unsigned long long)row->timestamp_ms,
                 row->voltage_mv, row->current_ma, row->percent, row->display_percent,
                 row->temperature_c, row->remaining_mah, row->full_charge_mah,
                 row->flags, row->quality, row->state, (unsigned long)row->error);
        /* Yield between groups; all service and I2C locks have been released. */
        if ((i & 7U) == 7U) vTaskDelay(1);
    }
    ESP_LOGI(TAG, "charge history end latest_seq=%lu",
             (unsigned long)(s_charge_history.next_sequence - 1U));
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
    if (config->design_capacity_mah != 1200U) return ESP_ERR_NOT_SUPPORTED;
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
    const bool power_valid = status->power_sample_valid && status->battery_present &&
                             gauge_ready && status->voltage_mv >= 2500U &&
                             status->voltage_mv <= 5000U;
    const bool sample_valid = status->telemetry_valid && power_valid &&
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
    if (status->power_sample_valid && !status->battery_present) {
        low_result = nuwax_battery_low_policy_update(
            false, NUWAX_BATTERY_LOW_REASON_NONE, &low_input);
    } else {
        low_result = nuwax_battery_low_with_validity(
            low_result, power_valid, sample_valid, &low_input);
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
    if (!status->telemetry_valid) {
        s_service.average_current_samples = 0U;
    } else if (s_service.average_current_samples == 0U || current_direction_changed) {
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
    s_service.snapshot.power_valid = power_valid;
    s_service.snapshot.power_stale = false;
    s_service.snapshot.power_timestamp_ms = status->power_timestamp_ms;
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
    s_service.snapshot.power_stale = !s_service.snapshot.power_valid ||
        s_service.snapshot.power_timestamp_ms == 0U ||
        current_ms < s_service.snapshot.power_timestamp_ms ||
        current_ms - s_service.snapshot.power_timestamp_ms > stale_after_ms;
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
    uint64_t profile_retry_after_ms = now_ms() + 60000U;
    unsigned profile_retries = 0U;
    uint64_t next_bus_report_ms = now_ms() + 60000U;
    battery_diagnostic_t diagnostic = {0};
    nuwax_battery_history_reset(&s_charge_history);
    ESP_LOGI(TAG, "charging indication revision=measured-current-v1 threshold_ma=20 fc_stops=1 gauge_advisories_preserved=1");
    ESP_LOGI(TAG, "charge history enabled revision=charge-history-v1 samples=%u bytes=%u dump_ms=%u volatile=1",
             NUWAX_BATTERY_HISTORY_CAPACITY, (unsigned)sizeof(s_charge_history),
             NUWAX_BATTERY_HISTORY_DUMP_INTERVAL_MS);

    while (!stop_requested()) {
        if (now_ms() >= next_bus_report_ms) {
            nuwax_i2c_bus_stats_t stats;
            nuwax_i2c_bus_get_stats(&stats);
            ESP_LOGI(TAG, "I2C totals transactions=%lu lock_timeouts=%lu recoveries=%lu wait_max_us=%lu hold_max_us=%lu wait_total_us=%llu hold_total_us=%llu",
                     (unsigned long)stats.transactions, (unsigned long)stats.lock_timeouts,
                     (unsigned long)stats.recoveries, (unsigned long)stats.max_wait_us,
                     (unsigned long)stats.max_hold_us, (unsigned long long)stats.total_wait_us,
                     (unsigned long long)stats.total_hold_us);
            ESP_LOGI(TAG, "I2C contention long_holds=%lu wait_task=%s hold_task=%s timeout_task=%s",
                     (unsigned long)stats.long_holds,
                     stats.max_wait_task, stats.max_hold_task,
                     stats.last_timeout_task);
            ESP_LOGI(TAG, "I2C sample calls=%lu errors=%lu busy_skips=%lu driver_max_us=%lu release_max_us=%lu window_driver_max_us=%lu window_release_max_us=%lu",
                     (unsigned long)stats.sample_calls, (unsigned long)stats.sample_errors,
                     (unsigned long)stats.try_skips, (unsigned long)stats.sample_driver_max_us,
                     (unsigned long)stats.sample_release_max_us,
                     (unsigned long)stats.sample_window_driver_max_us,
                     (unsigned long)stats.sample_window_release_max_us);
            static const char *const clients[] = {"other", "touch", "battery", "qmi"};
            for (unsigned i = 0; i < NUWAX_I2C_CLIENT_COUNT; ++i) {
                const nuwax_i2c_client_window_t *window = &stats.clients[i];
                ESP_LOGI(TAG, "I2C window client=%s calls=%lu errors=%lu long_holds=%lu hold_max_us=%lu driver_max_us=%lu release_max_us=%lu",
                         clients[i], (unsigned long)window->calls,
                         (unsigned long)window->errors, (unsigned long)window->long_holds,
                         (unsigned long)window->hold_max_us, (unsigned long)window->driver_max_us,
                         (unsigned long)window->release_max_us);
            }
            next_bus_report_ms = now_ms() + 60000U;
        }
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
        if (err == ESP_OK) err = bq27220_get_status_partial(&raw);
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
            log_charge_diagnostic(&diagnostic, &published, &raw, ESP_OK);
            record_charge_history(&published, &raw, ESP_OK);
            if (!raw.profile_verified && profile_retries < 3U &&
                now_ms() >= profile_retry_after_ms && !stop_requested()) {
                const esp_err_t retry_err = bq27220_revalidate_profile();
                if (retry_err != ESP_ERR_NOT_FINISHED) profile_retries++;
                profile_retry_after_ms = now_ms() + 60000U;
                ESP_LOGI(TAG, "profile revalidation attempt=%u err=%s", profile_retries,
                         esp_err_to_name(retry_err));
            }
            backoff_ms = s_service.config.error_backoff_initial_ms;
            /* Near empty, notice charger insertion/removal during the 3 s
             * warning. Only this existing task reads the gauge; normal SOC
             * keeps the configured 5-10 s cadence and error backoff. */
            wait_or_stop(published.valid && published.percent <= 2U
                ? NUWAX_LOW_BATTERY_FAST_SAMPLE_MS
                : s_service.config.sample_interval_ms);
            continue;
        }

        if (raw.power_sample_valid) {
            /* Preserve this attempt's safety group despite a later telemetry
             * failure. Restore the error streak reset by commit_success. */
            xSemaphoreTake(s_service.lock, portMAX_DELAY);
            const uint32_t prior_errors = s_service.snapshot.consecutive_errors;
            xSemaphoreGive(s_service.lock);
            (void)commit_success(&raw);
            xSemaphoreTake(s_service.lock, portMAX_DELAY);
            s_service.snapshot.consecutive_errors = prior_errors;
            xSemaphoreGive(s_service.lock);
        }
        const nuwax_battery_service_snapshot_t published = commit_error(err);
        ESP_LOGW(TAG,
                 "sample failed err=%s consecutive=%lu retry_ms=%lu retained_valid=%d",
                 esp_err_to_name(err),
                 (unsigned long)published.consecutive_errors,
                 (unsigned long)backoff_ms,
                 published.valid ? 1 : 0);
        notify_listener(&published);

        log_charge_diagnostic(&diagnostic, &published, &raw, err);
        record_charge_history(&published, &raw, err);

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
    if (xSemaphoreTake(s_stop_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    if (xSemaphoreTake(s_service.lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        xSemaphoreGive(s_stop_lock);
        return ESP_ERR_TIMEOUT;
    }
    if (!s_service.running) {
        xSemaphoreGive(s_service.lock);
        xSemaphoreGive(s_stop_lock);
        return ESP_OK;
    }
    if (s_service.task == xTaskGetCurrentTaskHandle()) {
        xSemaphoreGive(s_service.lock);
        xSemaphoreGive(s_stop_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_service.stop_requested = true;
    TaskHandle_t task = s_service.task;
    xSemaphoreGive(s_service.lock);

    if (task) xTaskNotifyGive(task);
    if (xSemaphoreTake(s_service.stopped, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        xSemaphoreGive(s_stop_lock);
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
    xSemaphoreGive(s_stop_lock);
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
    if (snapshot->power_valid) {
        const uint64_t current_ms = now_ms();
        snapshot->power_stale = snapshot->power_timestamp_ms == 0U ||
            current_ms < snapshot->power_timestamp_ms ||
            current_ms - snapshot->power_timestamp_ms >
                (uint64_t)sample_interval_ms * BATTERY_SNAPSHOT_MAX_PERIODS;
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
