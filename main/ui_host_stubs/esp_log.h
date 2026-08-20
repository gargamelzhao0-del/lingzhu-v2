#pragma once

static inline void esp_log_host_sink(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGE(tag, format, ...) \
    esp_log_host_sink((tag), (format), ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)
