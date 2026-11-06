#pragma once
#include <stdint.h>

/* Scratch files only. A fresh challenge prevents a previous successful
 * probe's sectors from satisfying a later write/readback after a lost write. */
static inline uint8_t nuwax_sd_probe_byte(uint32_t offset, uint64_t challenge)
{
    return (uint8_t)(offset * 31U + (offset >> 8U)) ^
        (uint8_t)(challenge >> ((offset & 7U) * 8U));
}
