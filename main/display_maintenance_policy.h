#pragma once
#include <stdbool.h>
#include <stdint.h>

/* LVGL owner only. Preserve the periodic refresh, spread its dirty area over
 * small strips, and pause on interaction. This never owns pixels or objects. */
typedef struct {
    uintptr_t screen;
    int64_t completed_us, next_strip_us;
    uint16_t row;
    bool initialized, active;
} nuwax_display_maintenance_t;

static inline bool nuwax_display_maintenance_next(
    nuwax_display_maintenance_t *s, uintptr_t screen, int64_t now,
    bool busy, uint16_t height, uint16_t *first, uint16_t *last)
{
    if (!screen || !height) { *s = (nuwax_display_maintenance_t){0}; return false; }
    if (!s->initialized || s->screen != screen) {
        *s = (nuwax_display_maintenance_t){.screen=screen, .completed_us=now, .initialized=true};
        return false; /* Screen load itself already requests a full redraw. */
    }
    if (!s->active && now - s->completed_us >= 120000000LL) {
        s->active=true; s->row=0; s->next_strip_us=now;
    }
    if (!s->active || busy || now < s->next_strip_us) return false;
    *first=s->row;
    uint32_t end=(uint32_t)s->row + 8U;
    if (end>height) end=height;
    *last=(uint16_t)(end-1U);
    s->row=(uint16_t)end;
    s->next_strip_us=now+50000LL;
    if (end==height) { s->active=false; s->completed_us=now; }
    return true;
}
