#include "nuwax_theme.h"
#include "esp_log.h"

static const char *TAG = "nuwax_theme";

void nuwax_theme_init(void) {
    ESP_LOGI(TAG, "Theme initialized: %dx%d circular display", NUWAX_SCREEN_W, NUWAX_SCREEN_H);
}
