#pragma once

#include <stdio.h>

#define ESP_LOG_IMPL(tag, format, ...) \
    do {                                \
        (void)(tag);                    \
        if (0) {                        \
            fprintf(stderr, format, ##__VA_ARGS__); \
        }                               \
    } while (0)

#define ESP_LOGD(tag, format, ...) ESP_LOG_IMPL(tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) ESP_LOG_IMPL(tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) ESP_LOG_IMPL(tag, format, ##__VA_ARGS__)
#define ESP_LOGE(tag, format, ...) ESP_LOG_IMPL(tag, format, ##__VA_ARGS__)
