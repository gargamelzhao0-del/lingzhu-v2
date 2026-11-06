#pragma once
#include <stdbool.h>
#include <stddef.h>

enum { NUWAX_SD_INTERNAL_RESERVE = 16384U, NUWAX_SD_VERIFY_MAX = 4096U,
       NUWAX_SD_SDK_BOUNCE = 512U, NUWAX_SD_ALLOC_ALLOWANCE = 128U };

/* The observed first bad LBAs were all 64-sector aligned. Keep ordinary
 * multi-sector transfers, but never cross that physical 32 KiB boundary in
 * one native command. This is independent of the FAT cluster alignment. */
static inline size_t nuwax_sd_native_chunk(size_t sector, size_t count)
{
    const size_t left = 64U - (sector & 63U);
    return count < left ? count : left;
}

/* The verifier's independent read buffer is live while the outer write bounce
 * is held. The SDK fallback may borrow one more sector for an unaligned/source
 * PSRAM request. Account for all three before lending the outer buffer. */
static inline size_t nuwax_sd_write_internal_floor(void)
{
    return NUWAX_SD_INTERNAL_RESERVE + NUWAX_SD_SDK_BOUNCE + NUWAX_SD_ALLOC_ALLOWANCE;
}

static inline size_t nuwax_sd_bulk_internal_floor(size_t bytes, bool verified_write)
{
    if (!verified_write) return NUWAX_SD_INTERNAL_RESERVE;
    const size_t readback = bytes < NUWAX_SD_VERIFY_MAX ? bytes : NUWAX_SD_VERIFY_MAX;
    return nuwax_sd_write_internal_floor() + readback + NUWAX_SD_ALLOC_ALLOWANCE;
}
