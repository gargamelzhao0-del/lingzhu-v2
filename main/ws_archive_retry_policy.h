#pragma once

#include <stdint.h>

/* Only the independent plaintext bulk sender opts in. These are failure
 * ceilings, not sleeps: successful writes continue immediately. Keep the
 * frame deadline absolute so a trickle cannot monopolize the transport. */
#define NUWAX_WS_ARCHIVE_NO_PROGRESS_MS 6000U
#define NUWAX_WS_ARCHIVE_TOTAL_MS 12000U

static inline uint32_t nuwax_ws_archive_extra_ms(uint32_t soft_ms)
{
    return soft_ms < NUWAX_WS_ARCHIVE_TOTAL_MS
        ? NUWAX_WS_ARCHIVE_TOTAL_MS - soft_ms : 0U;
}

static inline int64_t nuwax_ws_archive_progress_deadline(int64_t observed_us,
                                                       int64_t hard_us)
{
    const int64_t progress_us = observed_us +
        (int64_t)NUWAX_WS_ARCHIVE_NO_PROGRESS_MS * 1000LL;
    return progress_us < hard_us ? progress_us : hard_us;
}
