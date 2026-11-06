#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct {
    uint32_t commands;
    uint32_t sectors;
    uint32_t fallback_sectors;
    uint32_t command_us;
    uint32_t copy_us;
    uint32_t allocation_us;
    uint32_t native_calls;
    uint32_t native_sectors;
    uint32_t read_calls, read_sectors, read_us, read_errors;
    /* Disjoint driver-call wall-time buckets: <=2/5/10/20/>20 ms.
     * Includes scheduler delay; not a measurement of physical wire time. */
    uint32_t read_le2ms, read_le5ms, read_le10ms, read_le20ms, read_gt20ms;
} nuwax_sd_bulk_stats_t;
/* Diagnostic counters, not synchronization or durability state. */
nuwax_sd_bulk_stats_t nuwax_sd_bulk_stats(void);

/* Optional synchronous scratch-only witness. No payload is retained/logged.
 * Eight initial sectors (including repeats) expose whether bytes changed
 * between a successful SDK write and a later SDK read of the SAME LBA.
 * A matching checksum is diagnostic, never permission to bypass file CRC. */
typedef struct {
    uint64_t sector;
    uint32_t crc;
    bool valid;
} nuwax_sd_io_sector_t;
typedef struct {
    uint32_t reads, writes, sdk_errors, write_witnesses, read_matches, read_mismatches;
    uint64_t first_bad_sector;
    uint32_t first_expected_crc, first_read_crc;
    nuwax_sd_io_sector_t sectors[8];
} nuwax_sd_io_probe_t;
/* Caller-owned storage remains alive until end; the SDK calls are synchronous.
 * A competing/nested owner is skipped. No allocation and no background task. */
bool nuwax_sd_io_probe_begin(nuwax_sd_io_probe_t *probe);
void nuwax_sd_io_probe_end(void);

/* Only a match with the original application CRC may call end(true).
 * Healthy media keep bulk reads; compatibility mode lasts for this boot. */
bool nuwax_sd_bulk_read_recovery_begin(uint32_t timeout_ms);
void nuwax_sd_bulk_read_recovery_end(bool validated_original);
bool nuwax_sd_bulk_read_is_degraded(void);
