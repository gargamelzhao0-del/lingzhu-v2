#include "p3_software_ref.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
#define CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS
#define CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS 2000
#endif

#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_LOG_INTERVAL_MS
#define CONFIG_NUWAX_P3_SOFTWARE_REF_LOG_INTERVAL_MS 1000
#endif

#define NUWAX_P3_SOFTWARE_REF_MIN_CAPACITY_MS 100

#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
static const char *TAG = "nuwax_p3_soft_ref";
#define NUWAX_P3_SOFTWARE_REF_STALE_MS 250U

typedef struct {
    SemaphoreHandle_t lock;
    int16_t *buffer;
    size_t capacity_samples;
    uint32_t sample_rate_hz;
    uint32_t capacity_ms;
    uint64_t write_index;
    uint32_t generation;
    uint64_t write_calls;
    uint64_t overwritten_samples;
    uint64_t read_calls;
    uint64_t underrun_samples;
    uint32_t last_write_ms;
    uint32_t last_log_ms;
    float last_rms;
    int32_t last_peak;
    bool initialized;
} software_ref_state_t;

static software_ref_state_t s_ref;

static void advance_generation_locked(void) {
    s_ref.generation++;
    if (s_ref.generation == 0) {
        s_ref.generation = 1;
    }
}

static uint32_t monotonic_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static size_t available_samples_locked(void) {
    if (!s_ref.initialized || s_ref.capacity_samples == 0) {
        return 0;
    }
    if (s_ref.write_index < s_ref.capacity_samples) {
        return (size_t)s_ref.write_index;
    }
    return s_ref.capacity_samples;
}

static int32_t abs_pcm16(int16_t sample) {
    const int32_t value = sample;
    return value < 0 ? -value : value;
}

static void write_samples_locked(const int16_t *samples, size_t sample_count, size_t stride) {
    uint64_t sum_sq = 0;
    int32_t peak = 0;
    const uint64_t initial_write_index = s_ref.write_index;
    uint64_t free_until_full = 0;
    if (initial_write_index < s_ref.capacity_samples) {
        free_until_full = s_ref.capacity_samples - initial_write_index;
    }
    /* Keep a rolling buffer cursor instead of performing a 64-bit modulo for
     * every sample. This path runs for every speaker PCM frame during TTS. */
    size_t buffer_index = (size_t)(initial_write_index % s_ref.capacity_samples);
    for (size_t i = 0; i < sample_count; i++) {
        const int16_t sample = samples[i * stride];
        const int32_t abs_sample = abs_pcm16(sample);
        if (abs_sample > peak) {
            peak = abs_sample;
        }
        sum_sq += (uint64_t)((int32_t)sample * (int32_t)sample);
        s_ref.buffer[buffer_index] = sample;
        if (++buffer_index == s_ref.capacity_samples) {
            buffer_index = 0;
        }
        s_ref.write_index++;
    }

    if (sample_count > free_until_full) {
        s_ref.overwritten_samples += (uint64_t)(sample_count - free_until_full);
    }
    s_ref.write_calls++;
    s_ref.last_write_ms = monotonic_ms();
    s_ref.last_peak = peak;
    s_ref.last_rms = sample_count > 0 ? (float)sqrt((double)sum_sq / (double)sample_count) : 0.0f;
}

static void write_samples(const int16_t *samples, size_t sample_count, size_t stride) {
    if (!samples || sample_count == 0 || stride == 0 || !s_ref.initialized || !s_ref.buffer) {
        return;
    }

    bool should_log = false;
    nuwax_p3_software_ref_stats_t stats = {0};
    if (s_ref.lock) {
        xSemaphoreTake(s_ref.lock, portMAX_DELAY);
    }
    if (s_ref.initialized && s_ref.buffer && s_ref.capacity_samples > 0) {
        write_samples_locked(samples, sample_count, stride);
        const uint32_t now_ms = s_ref.last_write_ms;
        if (CONFIG_NUWAX_P3_SOFTWARE_REF_LOG_INTERVAL_MS > 0 &&
            (s_ref.last_log_ms == 0 ||
             (uint32_t)(now_ms - s_ref.last_log_ms) >= CONFIG_NUWAX_P3_SOFTWARE_REF_LOG_INTERVAL_MS)) {
            s_ref.last_log_ms = now_ms;
            stats.enabled = true;
            stats.initialized = true;
            stats.sample_rate_hz = s_ref.sample_rate_hz;
            stats.capacity_ms = s_ref.capacity_ms;
            stats.capacity_samples = s_ref.capacity_samples;
            stats.available_samples = available_samples_locked();
            stats.write_samples = s_ref.write_index;
            stats.write_calls = s_ref.write_calls;
            stats.overwritten_samples = s_ref.overwritten_samples;
            stats.read_calls = s_ref.read_calls;
            stats.underrun_samples = s_ref.underrun_samples;
            stats.last_write_ms = s_ref.last_write_ms;
            stats.last_rms = s_ref.last_rms;
            stats.last_peak = s_ref.last_peak;
            should_log = true;
        }
    }
    if (s_ref.lock) {
        xSemaphoreGive(s_ref.lock);
    }

    if (should_log) {
        ESP_LOGI(TAG,
                 "P3-0S-SOFTWARE-REF samples=%llu calls=%llu available=%u capacity=%u rms=%.1f peak=%ld overwritten=%llu last_write_ms=%lu",
                 (unsigned long long)stats.write_samples,
                 (unsigned long long)stats.write_calls,
                 (unsigned)stats.available_samples,
                 (unsigned)stats.capacity_samples,
                 (double)stats.last_rms,
                 (long)stats.last_peak,
                 (unsigned long long)stats.overwritten_samples,
                 (unsigned long)stats.last_write_ms);
    }
}
#endif

esp_err_t nuwax_p3_software_ref_init(uint32_t sample_rate_hz, uint32_t capacity_ms) {
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    if (sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (capacity_ms < NUWAX_P3_SOFTWARE_REF_MIN_CAPACITY_MS) {
        capacity_ms = NUWAX_P3_SOFTWARE_REF_MIN_CAPACITY_MS;
    }
    const uint64_t capacity64 = ((uint64_t)sample_rate_hz * (uint64_t)capacity_ms) / 1000ULL;
    if (capacity64 == 0 || capacity64 > (SIZE_MAX / sizeof(int16_t))) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t capacity_samples = (size_t)capacity64;

    if (!s_ref.lock) {
        s_ref.lock = xSemaphoreCreateMutex();
        if (!s_ref.lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_ref.lock, portMAX_DELAY);
    if (s_ref.initialized &&
        s_ref.sample_rate_hz == sample_rate_hz &&
        s_ref.capacity_ms == capacity_ms &&
        s_ref.capacity_samples == capacity_samples &&
        s_ref.buffer) {
        xSemaphoreGive(s_ref.lock);
        return ESP_OK;
    }

    int16_t *next = (int16_t *)heap_caps_malloc(capacity_samples * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const char *heap_name = "psram";
    if (!next) {
        next = (int16_t *)heap_caps_malloc(capacity_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
        heap_name = "8bit";
    }
    if (!next) {
        xSemaphoreGive(s_ref.lock);
        ESP_LOGE(TAG,
                 "P3-0S software ref allocation failed samples=%u bytes=%u psram_free=%u internal_free=%u",
                 (unsigned)capacity_samples,
                 (unsigned)(capacity_samples * sizeof(int16_t)),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    if (s_ref.buffer) {
        heap_caps_free(s_ref.buffer);
    }
    s_ref.buffer = next;
    s_ref.capacity_samples = capacity_samples;
    s_ref.sample_rate_hz = sample_rate_hz;
    s_ref.capacity_ms = capacity_ms;
    s_ref.write_index = 0;
    advance_generation_locked();
    s_ref.write_calls = 0;
    s_ref.overwritten_samples = 0;
    s_ref.read_calls = 0;
    s_ref.underrun_samples = 0;
    s_ref.last_write_ms = 0;
    s_ref.last_log_ms = 0;
    s_ref.last_rms = 0.0f;
    s_ref.last_peak = 0;
    s_ref.initialized = true;
    memset(s_ref.buffer, 0, capacity_samples * sizeof(int16_t));
    xSemaphoreGive(s_ref.lock);

    ESP_LOGI(TAG,
             "P3-0S software ref initialized sample_rate=%lu capacity_ms=%lu samples=%u heap=%s bytes=%u",
             (unsigned long)sample_rate_hz,
             (unsigned long)capacity_ms,
             (unsigned)capacity_samples,
             heap_name,
             (unsigned)(capacity_samples * sizeof(int16_t)));
    return ESP_OK;
#else
    (void)sample_rate_hz;
    (void)capacity_ms;
    return ESP_OK;
#endif
}

void nuwax_p3_software_ref_reset(void) {
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    if (s_ref.lock) {
        xSemaphoreTake(s_ref.lock, portMAX_DELAY);
    }
    if (s_ref.initialized && s_ref.buffer) {
        memset(s_ref.buffer, 0, s_ref.capacity_samples * sizeof(int16_t));
        s_ref.write_index = 0;
        advance_generation_locked();
        s_ref.write_calls = 0;
        s_ref.overwritten_samples = 0;
        s_ref.read_calls = 0;
        s_ref.underrun_samples = 0;
        s_ref.last_write_ms = 0;
        s_ref.last_log_ms = 0;
        s_ref.last_rms = 0.0f;
        s_ref.last_peak = 0;
    }
    if (s_ref.lock) {
        xSemaphoreGive(s_ref.lock);
    }
#endif
}

void nuwax_p3_software_ref_write(const int16_t *samples, size_t sample_count) {
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    write_samples(samples, sample_count, 1);
#else
    (void)samples;
    (void)sample_count;
#endif
}

void nuwax_p3_software_ref_write_interleaved_lr(const int16_t *samples, size_t frame_count) {
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    write_samples(samples, frame_count, 2);
#else
    (void)samples;
    (void)frame_count;
#endif
}

#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
static size_t copy_ending_at_locked(int16_t *out,
                                    size_t sample_count,
                                    uint64_t target_end,
                                    uint64_t write_index) {
    if (!out || sample_count == 0 || !s_ref.buffer || s_ref.capacity_samples == 0) {
        return 0;
    }
    const uint64_t oldest_index = write_index > s_ref.capacity_samples
                                      ? write_index - s_ref.capacity_samples
                                      : 0;
    const size_t leading_missing =
        target_end >= sample_count ? 0 : (size_t)(sample_count - target_end);
    const uint64_t start_index = target_end >= sample_count ? target_end - sample_count : 0;
    size_t copied = 0;
    for (size_t i = leading_missing; i < sample_count; i++) {
        const uint64_t abs_index = start_index + (uint64_t)(i - leading_missing);
        if (abs_index < oldest_index || abs_index >= target_end || abs_index >= write_index) {
            continue;
        }
        out[i] = s_ref.buffer[abs_index % s_ref.capacity_samples];
        copied++;
    }
    return copied;
}

static size_t copy_delayed_locked(int16_t *out,
                                  size_t sample_count,
                                  uint32_t delay_samples,
                                  uint64_t write_index) {
    if (!out || sample_count == 0 || !s_ref.buffer || s_ref.capacity_samples == 0) {
        return 0;
    }
    const uint64_t target_end = write_index > delay_samples ? write_index - delay_samples : 0;
    return copy_ending_at_locked(out, sample_count, target_end, write_index);
}
#endif

size_t nuwax_p3_software_ref_read_delayed(int16_t *out, size_t sample_count, uint32_t delay_samples) {
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    if (!out || sample_count == 0) {
        return 0;
    }
    memset(out, 0, sample_count * sizeof(int16_t));
    if (!s_ref.initialized || !s_ref.buffer || s_ref.capacity_samples == 0) {
        return 0;
    }

    size_t copied = 0;
    if (s_ref.lock) {
        xSemaphoreTake(s_ref.lock, portMAX_DELAY);
    }
    if (s_ref.initialized && s_ref.buffer && s_ref.capacity_samples > 0) {
        const uint32_t now_ms = monotonic_ms();
        if (s_ref.last_write_ms == 0 ||
            (uint32_t)(now_ms - s_ref.last_write_ms) > NUWAX_P3_SOFTWARE_REF_STALE_MS) {
            s_ref.read_calls++;
            s_ref.underrun_samples += sample_count;
            if (s_ref.lock) {
                xSemaphoreGive(s_ref.lock);
            }
            return 0;
        }
        copied = copy_delayed_locked(out, sample_count, delay_samples, s_ref.write_index);
        s_ref.read_calls++;
        s_ref.underrun_samples += (uint64_t)(sample_count - copied);
    }
    if (s_ref.lock) {
        xSemaphoreGive(s_ref.lock);
    }
    return copied;
#else
    if (out && sample_count > 0) {
        memset(out, 0, sample_count * sizeof(int16_t));
    }
    (void)delay_samples;
    return 0;
#endif
}

size_t nuwax_p3_software_ref_read_delayed_pair(int16_t *first_out,
                                               uint32_t first_delay_samples,
                                               int16_t *second_out,
                                               uint32_t second_delay_samples,
                                               size_t sample_count,
                                               size_t *second_copied) {
    if (second_copied) {
        *second_copied = 0;
    }
    if (!first_out || !second_out || sample_count == 0) {
        return 0;
    }
    memset(first_out, 0, sample_count * sizeof(first_out[0]));
    memset(second_out, 0, sample_count * sizeof(second_out[0]));
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    if (!s_ref.initialized || !s_ref.buffer || s_ref.capacity_samples == 0) {
        return 0;
    }
    size_t first_count = 0;
    size_t second_count = 0;
    if (s_ref.lock) {
        xSemaphoreTake(s_ref.lock, portMAX_DELAY);
    }
    if (s_ref.initialized && s_ref.buffer && s_ref.capacity_samples > 0) {
        const uint32_t now_ms = monotonic_ms();
        if (s_ref.last_write_ms != 0 &&
            (uint32_t)(now_ms - s_ref.last_write_ms) <= NUWAX_P3_SOFTWARE_REF_STALE_MS) {
            const uint64_t write_index = s_ref.write_index;
            first_count = copy_delayed_locked(first_out,
                                              sample_count,
                                              first_delay_samples,
                                              write_index);
            second_count = copy_delayed_locked(second_out,
                                               sample_count,
                                               second_delay_samples,
                                               write_index);
        }
        s_ref.read_calls += 2U;
        s_ref.underrun_samples += (uint64_t)(sample_count - first_count) +
                                  (uint64_t)(sample_count - second_count);
    }
    if (s_ref.lock) {
        xSemaphoreGive(s_ref.lock);
    }
    if (second_copied) {
        *second_copied = second_count;
    }
    return first_count;
#else
    (void)first_delay_samples;
    (void)second_delay_samples;
    return 0;
#endif
}

void nuwax_p3_software_ref_reader_reset(nuwax_p3_software_ref_reader_t *reader) {
    if (!reader) {
        return;
    }
    memset(reader, 0, sizeof(*reader));
}

void nuwax_p3_software_ref_reader_invalidate(nuwax_p3_software_ref_reader_t *reader) {
    if (!reader) {
        return;
    }
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    if (s_ref.lock) {
        xSemaphoreTake(s_ref.lock, portMAX_DELAY);
    }
    if (reader->initialized) {
        reader->resync_events++;
    }
    reader->initialized = false;
    reader->source_end_index = 0;
    if (s_ref.initialized) {
        reader->reanchor_wait_for_write = true;
        reader->reanchor_wait_write_index = s_ref.write_index;
    }
    if (s_ref.lock) {
        xSemaphoreGive(s_ref.lock);
    }
#else
    reader->initialized = false;
    reader->source_end_index = 0;
#endif
}

size_t nuwax_p3_software_ref_read_delayed_pair_continuous(
    nuwax_p3_software_ref_reader_t *reader,
    int16_t *first_out,
    uint32_t first_delay_samples,
    int16_t *second_out,
    uint32_t second_delay_samples,
    size_t sample_count,
    size_t *second_copied) {
    if (second_copied) {
        *second_copied = 0;
    }
    if (!reader || !first_out || !second_out || sample_count == 0) {
        return 0;
    }
    memset(first_out, 0, sample_count * sizeof(first_out[0]));
    memset(second_out, 0, sample_count * sizeof(second_out[0]));
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    if (!s_ref.initialized || !s_ref.buffer || s_ref.capacity_samples == 0) {
        reader->underrun_reads++;
        return 0;
    }

    size_t first_count = 0;
    size_t second_count = 0;
    if (s_ref.lock) {
        xSemaphoreTake(s_ref.lock, portMAX_DELAY);
    }
    if (s_ref.initialized && s_ref.buffer && s_ref.capacity_samples > 0) {
        const uint32_t now_ms = monotonic_ms();
        const bool recent = s_ref.last_write_ms != 0 &&
                            (uint32_t)(now_ms - s_ref.last_write_ms) <=
                                NUWAX_P3_SOFTWARE_REF_STALE_MS;
        if (reader->generation != s_ref.generation) {
            if (reader->generation != 0) {
                reader->generation_resets++;
            }
            reader->generation = s_ref.generation;
            reader->initialized = false;
            reader->source_end_index = 0;
            reader->reanchor_wait_for_write = false;
            reader->reanchor_wait_write_index = 0;
        }

        const bool reanchor_write_ready =
            !reader->reanchor_wait_for_write ||
            s_ref.write_index > reader->reanchor_wait_write_index;
        /* After a future underrun, never replay the same ring tail.  A new
         * speaker write is the only event allowed to establish a new epoch. */
        if (recent && !reader->initialized && reanchor_write_ready) {
            const uint32_t max_delay = first_delay_samples > second_delay_samples
                                           ? first_delay_samples
                                           : second_delay_samples;
            const uint64_t required_samples = (uint64_t)max_delay + (uint64_t)sample_count;
            if (s_ref.write_index >= required_samples) {
                reader->source_end_index = s_ref.write_index;
                reader->initialized = true;
                reader->reanchor_wait_for_write = false;
                reader->reanchor_wait_write_index = 0;
                reader->anchor_events++;
            }
        } else if (recent && !reader->initialized &&
                   reader->reanchor_wait_for_write && !reanchor_write_ready) {
            reader->blocked_reanchor_reads++;
        }

        if (reader->initialized &&
            reader->source_end_index >= first_delay_samples &&
            reader->source_end_index >= second_delay_samples) {
            uint64_t source_end = reader->source_end_index;
            uint64_t first_target_end = source_end - first_delay_samples;
            uint64_t second_target_end = source_end - second_delay_samples;
            const uint64_t oldest_index = s_ref.write_index > s_ref.capacity_samples
                                              ? s_ref.write_index - s_ref.capacity_samples
                                              : 0;
            const uint64_t first_start = first_target_end >= sample_count
                                             ? first_target_end - sample_count
                                             : 0;
            const uint64_t second_start = second_target_end >= sample_count
                                              ? second_target_end - sample_count
                                              : 0;
            if (recent && (first_start < oldest_index || second_start < oldest_index)) {
                source_end = s_ref.write_index;
                reader->source_end_index = source_end;
                first_target_end = source_end - first_delay_samples;
                second_target_end = source_end - second_delay_samples;
                reader->anchor_events++;
                reader->resync_events++;
            }
            first_count = copy_ending_at_locked(first_out,
                                                sample_count,
                                                first_target_end,
                                                s_ref.write_index);
            second_count = copy_ending_at_locked(second_out,
                                                 sample_count,
                                                 second_target_end,
                                                 s_ref.write_index);
            const int64_t phase_error = s_ref.write_index >= source_end
                                            ? (int64_t)(s_ref.write_index - source_end)
                                            : -(int64_t)(source_end - s_ref.write_index);
            if (first_count == sample_count && second_count == sample_count) {
                reader->last_phase_error = phase_error;
                if (reader->successful_reads == 0 || phase_error < reader->min_phase_error) {
                    reader->min_phase_error = phase_error;
                }
                if (reader->successful_reads == 0 || phase_error > reader->max_phase_error) {
                    reader->max_phase_error = phase_error;
                }
                reader->successful_reads++;
                reader->source_end_index += sample_count;
            } else {
                memset(first_out, 0, sample_count * sizeof(first_out[0]));
                memset(second_out, 0, sample_count * sizeof(second_out[0]));
                first_count = 0;
                second_count = 0;
                reader->initialized = false;
                reader->source_end_index = 0;
                reader->reanchor_wait_for_write = true;
                reader->reanchor_wait_write_index = s_ref.write_index;
                reader->resync_events++;
            }
        }

        if (first_count != sample_count || second_count != sample_count) {
            reader->underrun_reads++;
        }
        s_ref.read_calls += 2U;
        s_ref.underrun_samples += (uint64_t)(sample_count - first_count) +
                                  (uint64_t)(sample_count - second_count);
    }
    if (s_ref.lock) {
        xSemaphoreGive(s_ref.lock);
    }
    if (second_copied) {
        *second_copied = second_count;
    }
    return first_count;
#else
    (void)first_delay_samples;
    (void)second_delay_samples;
    reader->underrun_reads++;
    return 0;
#endif
}

void nuwax_p3_software_ref_get_stats(nuwax_p3_software_ref_stats_t *out_stats) {
    if (!out_stats) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    out_stats->enabled = true;
    if (s_ref.lock) {
        xSemaphoreTake(s_ref.lock, portMAX_DELAY);
    }
    out_stats->initialized = s_ref.initialized;
    out_stats->sample_rate_hz = s_ref.sample_rate_hz;
    out_stats->capacity_ms = s_ref.capacity_ms;
    out_stats->capacity_samples = s_ref.capacity_samples;
    out_stats->available_samples = available_samples_locked();
    out_stats->write_samples = s_ref.write_index;
    out_stats->write_calls = s_ref.write_calls;
    out_stats->overwritten_samples = s_ref.overwritten_samples;
    out_stats->read_calls = s_ref.read_calls;
    out_stats->underrun_samples = s_ref.underrun_samples;
    out_stats->last_write_ms = s_ref.last_write_ms;
    out_stats->last_rms = s_ref.last_rms;
    out_stats->last_peak = s_ref.last_peak;
    if (s_ref.lock) {
        xSemaphoreGive(s_ref.lock);
    }
#else
    out_stats->enabled = false;
#endif
}

esp_err_t nuwax_p3_software_ref_read_history_span_at(
    uint32_t expected_generation,
    uint64_t end_index,
    uint32_t min_delay_samples,
    uint32_t max_delay_samples,
    size_t window_samples,
    int16_t *out_span,
    size_t out_capacity_samples,
    nuwax_p3_software_ref_history_span_t *out_meta) {
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
    }
    if (!out_span || expected_generation == 0 || window_samples == 0 ||
        min_delay_samples > max_delay_samples) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t delay_span =
        (uint64_t)max_delay_samples - (uint64_t)min_delay_samples;
    if ((uint64_t)window_samples > UINT64_MAX - delay_span) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t span64 = (uint64_t)window_samples + delay_span;
    if (span64 == 0 || span64 > SIZE_MAX ||
        span64 > (uint64_t)(SIZE_MAX / sizeof(out_span[0]))) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t span_samples = (size_t)span64;
    if (out_capacity_samples < span_samples) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(out_span, 0, span_samples * sizeof(out_span[0]));

#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    if (!s_ref.lock) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_OK;
    xSemaphoreTake(s_ref.lock, portMAX_DELAY);
    if (!s_ref.initialized || !s_ref.buffer || s_ref.capacity_samples == 0) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        const uint64_t write_index = s_ref.write_index;
        const uint64_t oldest_index = write_index > s_ref.capacity_samples
                                          ? write_index - s_ref.capacity_samples
                                          : 0;
        const uint32_t now_ms = monotonic_ms();
        const uint32_t write_age_ms = s_ref.last_write_ms != 0
                                          ? (uint32_t)(now_ms - s_ref.last_write_ms)
                                          : UINT32_MAX;
        if (out_meta) {
            out_meta->generation = s_ref.generation;
            out_meta->end_index = end_index;
            out_meta->write_index_snapshot = write_index;
            out_meta->oldest_index_snapshot = oldest_index;
            out_meta->min_delay_samples = min_delay_samples;
            out_meta->max_delay_samples = max_delay_samples;
            out_meta->window_samples = window_samples;
            out_meta->span_samples = span_samples;
            out_meta->last_write_age_ms = write_age_ms;
            out_meta->recent = s_ref.last_write_ms != 0 &&
                               write_age_ms <= NUWAX_P3_SOFTWARE_REF_STALE_MS;
        }

        const uint64_t required_history =
            (uint64_t)max_delay_samples + (uint64_t)window_samples;
        if (expected_generation != s_ref.generation) {
            result = ESP_ERR_INVALID_STATE;
        } else if (span_samples > s_ref.capacity_samples) {
            result = ESP_ERR_INVALID_SIZE;
        } else if (end_index < required_history) {
            result = ESP_ERR_NOT_FOUND;
        } else {
            const uint64_t span_start = end_index - required_history;
            const uint64_t span_end = end_index - (uint64_t)min_delay_samples;
            if (span_start < oldest_index || span_end > write_index) {
                result = ESP_ERR_NOT_FOUND;
            } else {
                const size_t copied = copy_ending_at_locked(out_span,
                                                            span_samples,
                                                            span_end,
                                                            write_index);
                if (copied != span_samples) {
                    result = ESP_ERR_INVALID_STATE;
                }
            }
        }
    }
    xSemaphoreGive(s_ref.lock);
    if (result != ESP_OK) {
        memset(out_span, 0, span_samples * sizeof(out_span[0]));
    }
    return result;
#else
    (void)end_index;
    (void)out_meta;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
