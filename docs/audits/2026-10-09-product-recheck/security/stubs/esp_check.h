#pragma once
#define ESP_RETURN_ON_ERROR(expr, tag, message, ...) do { esp_err_t e_ = (expr); (void)(tag); if (e_ != ESP_OK) return e_; } while (0)
