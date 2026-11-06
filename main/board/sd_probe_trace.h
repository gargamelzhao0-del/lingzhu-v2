#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Explicit synchronous qualification scopes only. Failure diagnostics never
 * change the qualification verdict, write sectors, or authorize recording. */
bool nuwax_sd_probe_trace_begin(void);
bool nuwax_sd_probe_trace_active(void);
void nuwax_sd_probe_trace_finish(const char *scope, int result,
    uint64_t bad_lba, uint32_t expected_crc, uint32_t observed_crc);

/* Called by the guarded SDK copy. The inactive path only checks the owner;
 * it does not allocate, log, touch payload bytes or issue any card command. */
void nuwax_sd_probe_trace_command(const void *card, uint32_t opcode,
    uint32_t argument, uint32_t response, int result, const void *buffer, size_t bytes);
