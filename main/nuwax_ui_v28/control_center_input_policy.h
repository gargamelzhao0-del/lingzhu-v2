#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_CC_TOUCH_SLOP_PX 16
#define NUWAX_CC_VOLUME_FRAME_MS 25U
#define NUWAX_CC_VELOCITY_WINDOW_MS 100U
#define NUWAX_CC_PAGE_DRAG_RANGE_PX 360
#define NUWAX_CC_DRAWER_COMMIT_PX 47
#define NUWAX_CC_DRAWER_FLICK_PX_PER_S 340

static inline bool nuwax_cc_contact_moved(int dx, int dy) {
    return dx < -NUWAX_CC_TOUCH_SLOP_PX || dx > NUWAX_CC_TOUCH_SLOP_PX ||
           dy < -NUWAX_CC_TOUCH_SLOP_PX || dy > NUWAX_CC_TOUCH_SLOP_PX;
}

static inline bool nuwax_cc_sequence_after(uint32_t value, uint32_t previous) {
    return value != 0U && (previous == 0U || (int32_t)(value - previous) > 0);
}

static inline bool nuwax_cc_drawer_release_close(bool opened_by_pull, int dy,
                                                int velocity, int final_y) {
    if (velocity >= NUWAX_CC_DRAWER_FLICK_PX_PER_S) return false;
    if (velocity <= -NUWAX_CC_DRAWER_FLICK_PX_PER_S) return true;
    if (opened_by_pull) return dy < NUWAX_CC_DRAWER_COMMIT_PX;
    if (dy >= NUWAX_CC_DRAWER_COMMIT_PX) return false;
    return dy <= -NUWAX_CC_DRAWER_COMMIT_PX || final_y < -58;
}
