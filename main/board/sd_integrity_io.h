#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define NUWAX_SD_REPAIR_BUDGET_MS 8000U

typedef struct {
    uint32_t write_calls, read_calls, sdk_errors, foreign_writes;
    uint32_t covered_sectors, unknown_sectors, matched_sectors, mismatched_sectors;
    uint32_t evictions, source_mutations, raw_attempts;
    uint32_t expected_crc, observed_crc, raw_crc;
    int32_t raw_error;
    uint64_t bad_lba;
    bool mismatch_seen, raw_valid;
} nuwax_sd_integrity_io_report_t;

/* One persistent optional PSRAM observer. Audio buffers are never retained;
 * task/card identity is only used as a tag, never dereferenced after a call.
 * Normal SDK results and caller data are preserved. */
bool nuwax_sd_integrity_io_init(void);
void nuwax_sd_integrity_io_arm(void);
void nuwax_sd_integrity_io_read_phase(bool reading);
void nuwax_sd_integrity_io_freeze(nuwax_sd_integrity_io_report_t *out);
/* Called inside the existing FatFs/SDK operation, so a failure-only raw read
 * is serialized by the same volume lock. Never call this as a raw user API. */
esp_err_t nuwax_sd_integrity_io_transfer(void *card, void *buffer,
    size_t sector, size_t count, bool write);
/* Limits diagnostic evidence export without changing the mounted card's host.
 * An owner-local descriptor copy bounds every SDK transaction in this scope. */
bool nuwax_sd_integrity_io_budget_begin(uint32_t timeout_ms);
void nuwax_sd_integrity_io_budget_end(void);

#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
/* Writer-only scope, not a raw I/O API. Each underlying write checks the
 * existing card/task tag and native CMD13 readiness inside the volume lock.
 * An aborted/failed scope rejects further writes through FILE cleanup. */
bool nuwax_sd_integrity_io_repair_begin(void);
void nuwax_sd_integrity_io_repair_abort(void);
void nuwax_sd_integrity_io_repair_end(void);
#endif
