#pragma once

int nuwax_sim_log(const char *level, const char *tag, const char *fmt, ...);

#define ESP_LOGI(tag, fmt, ...) nuwax_sim_log("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) nuwax_sim_log("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) nuwax_sim_log("E", tag, fmt, ##__VA_ARGS__)
