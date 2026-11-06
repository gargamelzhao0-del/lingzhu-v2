#pragma once
/* Legacy storage fixtures model files, not a native SD controller. New linked
 * recovery tests override these hooks and exercise the real DMA adapter. */
#include "board/sd_dma_bulk.h"
#ifndef NUWAX_SD_READ_RECOVERY_HOST_LINKED
bool nuwax_sd_bulk_read_recovery_begin(uint32_t timeout_ms)
{ return timeout_ms != 0U; }
void nuwax_sd_bulk_read_recovery_end(bool validated_original)
{ (void)validated_original; }
bool nuwax_sd_bulk_read_is_degraded(void) { return false; }
#endif
