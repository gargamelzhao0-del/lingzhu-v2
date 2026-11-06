#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint32_t writes, verified_sectors, rewrites, recovered, failures, memory_denied;
    uint32_t max_us;
    uint32_t reserve_uses, single_read_recoveries;
} nuwax_sd_write_verify_stats_t;
/* Existing FatFs/SDK owner and its volume lock remain held. Only the exact
 * requested sectors may be rewritten; source storage stays caller-owned. */
esp_err_t nuwax_sd_write_verified(void *card, const void *source, size_t sector, size_t count);
nuwax_sd_write_verify_stats_t nuwax_sd_write_verify_stats(void);
void nuwax_sd_write_verify_set_single_reads(bool enabled);
bool nuwax_sd_write_verify_single_reads(void);
