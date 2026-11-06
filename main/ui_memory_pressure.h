#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Preserve the existing 24 KB draw-buffer target. Only a known, short SD
 * scratch allocation can defer one release decision. Critical pressure or
 * pressure persisting for 50 ms still releases the second draw buffer. */
typedef struct { int64_t deadline_us; } nuwax_ui_memory_pressure_t;
static inline bool nuwax_ui_memory_release_due(nuwax_ui_memory_pressure_t *p,
    int64_t now_us, size_t free_bytes, size_t target_bytes, uint32_t loan_bytes)
{
    if (free_bytes >= target_bytes) { p->deadline_us = 0; return false; }
    if (free_bytes < 16384U || !loan_bytes || loan_bytes > 8192U ||
        target_bytes - free_bytes > loan_bytes) {
        p->deadline_us = 0;
        return true;
    }
    if (!p->deadline_us) p->deadline_us = now_us + 50000;
    return now_us >= p->deadline_us;
}
