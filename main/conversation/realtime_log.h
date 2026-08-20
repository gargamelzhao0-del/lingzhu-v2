#pragma once

#include "esp_log.h"

/* Synchronous console output can consume more than one 20 ms audio period.
 * Keep frame-level diagnostics compiled and type-checked, but silent in the
 * production realtime path unless explicitly enabled for a bench run. */
#if defined(CONFIG_NUWAX_REALTIME_AUDIO_DIAGNOSTIC_LOG_ENABLE) && \
    CONFIG_NUWAX_REALTIME_AUDIO_DIAGNOSTIC_LOG_ENABLE
#define NUWAX_RT_DIAG_LOGI(tag, format, ...) \
    ESP_LOGI(tag, format, ##__VA_ARGS__)
#define NUWAX_RT_DIAG_LOGW(tag, format, ...) \
    ESP_LOGW(tag, format, ##__VA_ARGS__)
#else
#define NUWAX_RT_DIAG_LOGI(tag, format, ...) \
    do {                                         \
        if (0) {                                 \
            ESP_LOGI(tag, format, ##__VA_ARGS__); \
        }                                        \
    } while (0)
#define NUWAX_RT_DIAG_LOGW(tag, format, ...) \
    do {                                         \
        if (0) {                                 \
            ESP_LOGW(tag, format, ##__VA_ARGS__); \
        }                                        \
    } while (0)
#endif
