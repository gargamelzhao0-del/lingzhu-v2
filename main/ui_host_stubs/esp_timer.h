#pragma once

#include <stdint.h>

static inline int64_t esp_timer_get_time(void)
{
    static int64_t now_us;
    now_us += 25000;
    return now_us;
}
