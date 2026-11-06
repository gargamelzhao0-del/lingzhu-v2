#include "diagnostics.h"

#include <stdio.h>

#include "device_error_report.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* These are current-allocation safety floors. The minimum-free counters are
 * lifetime diagnostics and can include short boot-time valleys, so they must
 * not turn every healthy runtime checkpoint into a warning. */
#define NUWAX_HEAP_WARN_INTERNAL_FREE_BYTES (16U * 1024U)
#define NUWAX_HEAP_WARN_INTERNAL_LARGEST_BYTES (10U * 1024U)
#define NUWAX_HEAP_WARN_DMA_LARGEST_BYTES (8U * 1024U)
#define NUWAX_HEAP_WARN_PSRAM_LARGEST_BYTES (512U * 1024U)

void nuwax_diagnostics_log_heap_label(const char *tag, const char *label) {
    const char *checkpoint = label ? label : "-";
    const size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t min_free_8bit = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(tag,
             "heap checkpoint=%s free=%u min_free=%u internal=%u internal_min=%u internal_largest=%u dma=%u dma_largest=%u psram=%u psram_min=%u psram_largest=%u",
             checkpoint,
             (unsigned)free_8bit,
             (unsigned)min_free_8bit,
             (unsigned)internal,
             (unsigned)internal_min,
             (unsigned)internal_largest,
             (unsigned)dma,
             (unsigned)dma_largest,
             (unsigned)psram,
             (unsigned)psram_min,
             (unsigned)psram_largest);

    if (internal < NUWAX_HEAP_WARN_INTERNAL_FREE_BYTES ||
        internal_largest < NUWAX_HEAP_WARN_INTERNAL_LARGEST_BYTES ||
        dma_largest < NUWAX_HEAP_WARN_DMA_LARGEST_BYTES ||
        psram_largest < NUWAX_HEAP_WARN_PSRAM_LARGEST_BYTES) {
        const char *trigger =
            internal < NUWAX_HEAP_WARN_INTERNAL_FREE_BYTES
                ? "internal_free"
                : internal_largest < NUWAX_HEAP_WARN_INTERNAL_LARGEST_BYTES
                      ? "internal_largest"
                      : dma_largest < NUWAX_HEAP_WARN_DMA_LARGEST_BYTES
                            ? "dma_largest"
                            : "psram_largest";
        char detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
        snprintf(detail,
                 sizeof(detail),
                 "ESP_OK;internal=%u;largest=%u;dma_largest=%u;psram_largest=%u;trigger=%s",
                 (unsigned)internal,
                 (unsigned)internal_largest,
                 (unsigned)dma_largest,
                 (unsigned)psram_largest,
                 trigger);
        ESP_LOGW(tag,
                 "current heap safety warning checkpoint=%s internal=%u historical_min=%u largest=%u dma_largest=%u psram_largest=%u thresholds internal>=%u largest>=%u dma_largest>=%u psram_largest>=%u",
                 checkpoint,
                 (unsigned)internal,
                 (unsigned)internal_min,
                 (unsigned)internal_largest,
                 (unsigned)dma_largest,
                 (unsigned)psram_largest,
                 NUWAX_HEAP_WARN_INTERNAL_FREE_BYTES,
                 NUWAX_HEAP_WARN_INTERNAL_LARGEST_BYTES,
                 NUWAX_HEAP_WARN_DMA_LARGEST_BYTES,
                 NUWAX_HEAP_WARN_PSRAM_LARGEST_BYTES);
        (void)nuwax_device_warning_report_enqueue_detail(
            "memory", "heap_safety", ESP_OK, detail, checkpoint, 0U, NULL);
    }
}

void nuwax_diagnostics_log_heap(const char *tag) {
    nuwax_diagnostics_log_heap_label(tag, "-");
}

void nuwax_diagnostics_log_current_task_stack(const char *tag, const char *label) {
    UBaseType_t high_water = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(tag,
             "stack task=%s label=%s high_water=%u",
             pcTaskGetName(NULL),
             label ? label : "-",
             (unsigned)high_water);
}

/*
 * P3-0: structured AEC ref health log line.  All values on one line so
 * scripts can grep for "P3-0-AEC-REF-HEALTH" and parse CSV-like fields.
 */
void nuwax_diagnostics_log_aec_ref_health(const char *tag,
                                           int status,
                                           float score,
                                           float tone_amp,
                                           float quiet_rms,
                                           float corr,
                                           int delay_samples,
                                           float clip_ratio,
                                           int slot0_rms,
                                           bool passed)
{
    ESP_LOGW(tag ? tag : "diag",
             "P3-0-AEC-REF-HEALTH status=%d score=%.3f tone_amp=%.1f quiet_rms=%.1f "
             "corr=%.3f delay_samples=%d clip_ratio=%.3f slot0_rms=%d passed=%d",
             status, (double)score, (double)tone_amp, (double)quiet_rms,
             (double)corr, delay_samples, (double)clip_ratio, slot0_rms, passed ? 1 : 0);
    if (!passed) {
        (void)nuwax_device_warning_report_enqueue(
            "audio", "aec_ref_health", ESP_OK, "diagnostic", 0U, NULL);
    }
}
