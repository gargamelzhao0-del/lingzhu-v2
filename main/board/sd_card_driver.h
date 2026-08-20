#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool mounted;
    uint64_t capacity_bytes;
    uint32_t sector_size;
    uint32_t max_frequency_khz;
} nuwax_sd_card_info_t;

esp_err_t nuwax_sd_card_mount(nuwax_sd_card_info_t *info);
esp_err_t nuwax_sd_card_read_write_selftest(void);
esp_err_t nuwax_sd_card_unmount(void);
bool nuwax_sd_card_is_mounted(void);
