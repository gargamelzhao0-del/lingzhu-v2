#pragma once

#define ESP_LOGE(tag, format, ...) \
    do {                            \
        (void)(tag);                \
        (void)(format);             \
    } while (0)
#define ESP_LOGW(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)

