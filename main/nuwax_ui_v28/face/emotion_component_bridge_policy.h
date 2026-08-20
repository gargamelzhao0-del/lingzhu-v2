#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "emotion_asset_pack.h"

typedef struct {
    uint16_t close_ms;
    uint16_t reshape_ms;
    uint16_t open_ms;
} nuwax_emotion_component_bridge_timing_t;

static inline bool nuwax_emotion_component_bridge_strong_state_asset(
    nuwax_emotion_asset_id_t asset_id)
{
    return asset_id == NUWAX_EMOTION_ASSET_SLEEPING ||
           asset_id == NUWAX_EMOTION_ASSET_PROVISIONING ||
           asset_id == NUWAX_EMOTION_ASSET_DISCONNECTED ||
           asset_id == NUWAX_EMOTION_ASSET_MEETING ||
           asset_id == NUWAX_EMOTION_ASSET_MONITORING;
}

static inline nuwax_emotion_component_bridge_timing_t
nuwax_emotion_component_bridge_timing(
    nuwax_emotion_asset_id_t source,
    nuwax_emotion_asset_id_t target,
    uint16_t default_close_ms,
    uint16_t default_reshape_ms,
    uint16_t default_open_ms)
{
    nuwax_emotion_component_bridge_timing_t timing = {
        .close_ms = default_close_ms,
        .reshape_ms = default_reshape_ms,
        .open_ms = default_open_ms,
    };

    /* Strong system modes keep their authored weight. Conversation routes
     * are deliberately shorter so the face follows the already-committed
     * lifecycle boundary without replacing the asynchronous decode path. */
    if (nuwax_emotion_component_bridge_strong_state_asset(source) ||
        nuwax_emotion_component_bridge_strong_state_asset(target)) {
        return timing;
    }
    if (target == NUWAX_EMOTION_ASSET_LISTENING) {
        return (nuwax_emotion_component_bridge_timing_t){50U, 25U, 75U};
    }
    if (source == NUWAX_EMOTION_ASSET_LISTENING) {
        return (nuwax_emotion_component_bridge_timing_t){60U, 35U, 105U};
    }
    if (source == NUWAX_EMOTION_ASSET_QUESTION) {
        return (nuwax_emotion_component_bridge_timing_t){55U, 30U, 90U};
    }
    return (nuwax_emotion_component_bridge_timing_t){75U, 40U, 135U};
}

static inline bool nuwax_emotion_component_bridge_allowed(
    nuwax_emotion_asset_id_t source, nuwax_emotion_asset_id_t target)
{
    return source >= NUWAX_EMOTION_ASSET_DIZZINESS &&
        source <= NUWAX_EMOTION_ASSET_QUESTION &&
        target >= NUWAX_EMOTION_ASSET_DIZZINESS &&
        target <= NUWAX_EMOTION_ASSET_QUESTION &&
        source != target &&
        !(source == NUWAX_EMOTION_ASSET_THINKING &&
          target == NUWAX_EMOTION_ASSET_THOUGHT_COMPLETE);
}

static inline bool nuwax_emotion_component_bridge_prefers_target(
    uint32_t elapsed_ms, uint32_t duration_ms)
{
    if (duration_ms == 0U) return true;
    return (uint64_t)elapsed_ms * 100U >= (uint64_t)duration_ms * 48U;
}

static inline bool nuwax_emotion_component_bridge_ready_to_animate(
    bool source_ready, bool target_ready)
{
    return source_ready && target_ready;
}

static inline bool nuwax_emotion_component_asset_request_is_satisfied(
    nuwax_emotion_asset_id_t requested,
    nuwax_emotion_asset_id_t queued,
    nuwax_emotion_asset_id_t current,
    bool bridge_active,
    nuwax_emotion_asset_id_t bridge_source)
{
    if (requested == queued) return true;
    if (queued != 0U || requested != current) return false;

    /* While a bridge is active, current still names its source. A request to
     * return to that source must cancel the in-flight target instead of being
     * coalesced as an already-visible asset. */
    return !bridge_active || requested != bridge_source;
}

static inline uint32_t nuwax_emotion_component_bridge_next_elapsed_us(
    uint32_t elapsed_us, uint32_t duration_us, uint32_t frame_period_us)
{
    if (elapsed_us >= duration_us) return duration_us;
    const uint32_t remaining_us = duration_us - elapsed_us;
    return elapsed_us +
        (frame_period_us < remaining_us ? frame_period_us : remaining_us);
}

static inline uint16_t nuwax_emotion_component_handoff_frame(
    nuwax_emotion_asset_id_t asset_id)
{
    switch (asset_id) {
        case NUWAX_EMOTION_ASSET_SLEEPING:
            /* Source frame 30 relative to persistent loop frame 18. */
            return 12U;
        case NUWAX_EMOTION_ASSET_TOUCH:
            /* Source frame 18 relative to transient start frame 13. */
            return 5U;
        default:
            return 0U;
    }
}

static inline uint32_t nuwax_emotion_component_region_area(
    nuwax_emotion_asset_region_t region)
{
    return (uint32_t)region.width * region.height;
}

static inline bool nuwax_emotion_component_regions_overlap(
    nuwax_emotion_asset_region_t first,
    nuwax_emotion_asset_region_t second)
{
    return (uint32_t)first.x < (uint32_t)second.x + second.width &&
        (uint32_t)second.x < (uint32_t)first.x + first.width &&
        (uint32_t)first.y < (uint32_t)second.y + second.height &&
        (uint32_t)second.y < (uint32_t)first.y + first.height;
}

static inline nuwax_emotion_asset_region_t
nuwax_emotion_component_region_union(
    nuwax_emotion_asset_region_t first,
    nuwax_emotion_asset_region_t second)
{
    const uint32_t x0 = first.x < second.x ? first.x : second.x;
    const uint32_t y0 = first.y < second.y ? first.y : second.y;
    const uint32_t first_x1 = (uint32_t)first.x + first.width;
    const uint32_t second_x1 = (uint32_t)second.x + second.width;
    const uint32_t first_y1 = (uint32_t)first.y + first.height;
    const uint32_t second_y1 = (uint32_t)second.y + second.height;
    const uint32_t x1 = first_x1 > second_x1 ? first_x1 : second_x1;
    const uint32_t y1 = first_y1 > second_y1 ? first_y1 : second_y1;
    return (nuwax_emotion_asset_region_t){
        .x = (uint16_t)x0,
        .y = (uint16_t)y0,
        .width = (uint16_t)(x1 - x0),
        .height = (uint16_t)(y1 - y0),
    };
}

static inline uint8_t nuwax_emotion_component_regions_compact(
    nuwax_emotion_asset_region_t *regions, uint8_t region_count,
    uint8_t maximum_regions)
{
    if (!regions || maximum_regions == 0U) return 0U;

    bool merged = true;
    while (merged || region_count > maximum_regions) {
        merged = false;
        for (uint8_t left = 0U; left < region_count && !merged; ++left) {
            for (uint8_t right = left + 1U; right < region_count; ++right) {
                if (!nuwax_emotion_component_regions_overlap(
                        regions[left], regions[right])) {
                    continue;
                }
                regions[left] = nuwax_emotion_component_region_union(
                    regions[left], regions[right]);
                memmove(&regions[right], &regions[right + 1U],
                        (region_count - right - 1U) * sizeof(*regions));
                region_count--;
                merged = true;
                break;
            }
        }
        if (merged || region_count <= maximum_regions) continue;

        uint8_t best_left = 0U;
        uint8_t best_right = 1U;
        uint32_t best_penalty = UINT32_MAX;
        for (uint8_t left = 0U; left < region_count; ++left) {
            for (uint8_t right = left + 1U; right < region_count; ++right) {
                const nuwax_emotion_asset_region_t joined =
                    nuwax_emotion_component_region_union(
                        regions[left], regions[right]);
                const uint32_t penalty =
                    nuwax_emotion_component_region_area(joined) -
                    nuwax_emotion_component_region_area(regions[left]) -
                    nuwax_emotion_component_region_area(regions[right]);
                if (penalty < best_penalty) {
                    best_penalty = penalty;
                    best_left = left;
                    best_right = right;
                }
            }
        }
        regions[best_left] = nuwax_emotion_component_region_union(
            regions[best_left], regions[best_right]);
        memmove(&regions[best_right], &regions[best_right + 1U],
                (region_count - best_right - 1U) * sizeof(*regions));
        region_count--;
        merged = true;
    }

    for (uint8_t index = 1U; index < region_count; ++index) {
        const nuwax_emotion_asset_region_t value = regions[index];
        uint8_t cursor = index;
        while (cursor > 0U &&
               (regions[cursor - 1U].y > value.y ||
                (regions[cursor - 1U].y == value.y &&
                 regions[cursor - 1U].x > value.x))) {
            regions[cursor] = regions[cursor - 1U];
            cursor--;
        }
        regions[cursor] = value;
    }
    return region_count;
}
