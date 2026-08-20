#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "nuwax_ui_v28/face/emotion_component_bridge_policy.h"

static int s_failures;

static void expect_true(const char *name, bool value)
{
    if (value) {
        printf("[PASS] %s\n", name);
    } else {
        fprintf(stderr, "[FAIL] %s\n", name);
        s_failures++;
    }
}

static uint32_t timing_duration_ms(
    nuwax_emotion_component_bridge_timing_t timing)
{
    return (uint32_t)timing.close_ms + timing.reshape_ms + timing.open_ms;
}

static bool regions_are_disjoint(
    const nuwax_emotion_asset_region_t *regions, uint8_t count)
{
    for (uint8_t left = 0U; left < count; ++left) {
        for (uint8_t right = left + 1U; right < count; ++right) {
            if (nuwax_emotion_component_regions_overlap(
                    regions[left], regions[right])) {
                return false;
            }
        }
    }
    return true;
}

int main(void)
{
    expect_true("ordinary emotions use component bridge",
                nuwax_emotion_component_bridge_allowed(
                    NUWAX_EMOTION_ASSET_HAPPY,
                    NUWAX_EMOTION_ASSET_EXCITED));
    expect_true("same emotion does not bridge",
                !nuwax_emotion_component_bridge_allowed(
                    NUWAX_EMOTION_ASSET_HAPPY,
                    NUWAX_EMOTION_ASSET_HAPPY));
    expect_true("thinking completion is a direct handoff",
                !nuwax_emotion_component_bridge_allowed(
                    NUWAX_EMOTION_ASSET_THINKING,
                    NUWAX_EMOTION_ASSET_THOUGHT_COMPLETE));
    expect_true("thought complete can bridge to a normal emotion",
                nuwax_emotion_component_bridge_allowed(
                    NUWAX_EMOTION_ASSET_THOUGHT_COMPLETE,
                    NUWAX_EMOTION_ASSET_HAPPY));
    expect_true("invalid emotion cannot bridge",
                !nuwax_emotion_component_bridge_allowed(
                    (nuwax_emotion_asset_id_t)0,
                    NUWAX_EMOTION_ASSET_HAPPY));
    expect_true("question exits to listening in 150 ms",
                timing_duration_ms(nuwax_emotion_component_bridge_timing(
                    NUWAX_EMOTION_ASSET_QUESTION,
                    NUWAX_EMOTION_ASSET_LISTENING,
                    140U, 70U, 230U)) == 150U);
    expect_true("cloud emotion exits to listening in 150 ms",
                timing_duration_ms(nuwax_emotion_component_bridge_timing(
                    NUWAX_EMOTION_ASSET_EXCITED,
                    NUWAX_EMOTION_ASSET_LISTENING,
                    140U, 70U, 230U)) == 150U);
    expect_true("listening enters question in 200 ms",
                timing_duration_ms(nuwax_emotion_component_bridge_timing(
                    NUWAX_EMOTION_ASSET_LISTENING,
                    NUWAX_EMOTION_ASSET_QUESTION,
                    140U, 70U, 230U)) == 200U);
    expect_true("question exits to cloud emotion in 175 ms",
                timing_duration_ms(nuwax_emotion_component_bridge_timing(
                    NUWAX_EMOTION_ASSET_QUESTION,
                    NUWAX_EMOTION_ASSET_EXCITED,
                    140U, 70U, 230U)) == 175U);
    expect_true("ordinary emotion bridge remains smooth at 250 ms",
                timing_duration_ms(nuwax_emotion_component_bridge_timing(
                    NUWAX_EMOTION_ASSET_HAPPY,
                    NUWAX_EMOTION_ASSET_PLAYFUL,
                    140U, 70U, 230U)) == 250U);
    expect_true("sleep transition preserves authored timing",
                timing_duration_ms(nuwax_emotion_component_bridge_timing(
                    NUWAX_EMOTION_ASSET_SLEEPING,
                    NUWAX_EMOTION_ASSET_LISTENING,
                    170U, 70U, 230U)) == 470U);
    expect_true("interrupted bridge stays on source before visual midpoint",
                !nuwax_emotion_component_bridge_prefers_target(479U,
                                                                1000U));
    expect_true("interrupted bridge commits target at visual midpoint",
                nuwax_emotion_component_bridge_prefers_target(480U,
                                                               1000U));
    expect_true("zero-duration bridge commits target",
                nuwax_emotion_component_bridge_prefers_target(0U, 0U));
    expect_true("bridge waits for source snapshot",
                !nuwax_emotion_component_bridge_ready_to_animate(false,
                                                                  true));
    expect_true("bridge waits for target decode",
                !nuwax_emotion_component_bridge_ready_to_animate(true,
                                                                  false));
    expect_true("bridge starts after both frames are ready",
                nuwax_emotion_component_bridge_ready_to_animate(true,
                                                                 true));
    expect_true("stable current asset request is coalesced",
                nuwax_emotion_component_asset_request_is_satisfied(
                    NUWAX_EMOTION_ASSET_LISTENING,
                    (nuwax_emotion_asset_id_t)0,
                    NUWAX_EMOTION_ASSET_LISTENING,
                    false,
                    (nuwax_emotion_asset_id_t)0));
    expect_true("bridge source request cancels stale target",
                !nuwax_emotion_component_asset_request_is_satisfied(
                    NUWAX_EMOTION_ASSET_LISTENING,
                    (nuwax_emotion_asset_id_t)0,
                    NUWAX_EMOTION_ASSET_LISTENING,
                    true,
                    NUWAX_EMOTION_ASSET_LISTENING));
    expect_true("repeated queued bridge cancellation is coalesced",
                nuwax_emotion_component_asset_request_is_satisfied(
                    NUWAX_EMOTION_ASSET_LISTENING,
                    NUWAX_EMOTION_ASSET_LISTENING,
                    NUWAX_EMOTION_ASSET_LISTENING,
                    true,
                    NUWAX_EMOTION_ASSET_LISTENING));
    expect_true("late render advances one visual pose",
                nuwax_emotion_component_bridge_next_elapsed_us(
                    50000U, 500000U, 25000U) == 75000U);
    expect_true("visual pose saturates at duration",
                nuwax_emotion_component_bridge_next_elapsed_us(
                    490000U, 500000U, 25000U) == 500000U);
    expect_true("ordinary emotion hands off at loop key",
                nuwax_emotion_component_handoff_frame(
                    NUWAX_EMOTION_ASSET_HAPPY) == 0U);
    expect_true("sleeping matches simulator handoff frame",
                nuwax_emotion_component_handoff_frame(
                    NUWAX_EMOTION_ASSET_SLEEPING) == 12U);
    expect_true("touch matches simulator handoff frame",
                nuwax_emotion_component_handoff_frame(
                    NUWAX_EMOTION_ASSET_TOUCH) == 5U);

    nuwax_emotion_asset_region_t touching[2] = {
        {.x = 0, .y = 0, .width = 10, .height = 10},
        {.x = 10, .y = 0, .width = 10, .height = 10},
    };
    expect_true("edge-touching regions remain independent",
                nuwax_emotion_component_regions_compact(touching, 2U, 5U) ==
                    2U && regions_are_disjoint(touching, 2U));

    nuwax_emotion_asset_region_t cascade[3] = {
        {.x = 0, .y = 0, .width = 10, .height = 10},
        {.x = 8, .y = 0, .width = 10, .height = 10},
        {.x = 17, .y = 0, .width = 10, .height = 10},
    };
    const uint8_t cascade_count =
        nuwax_emotion_component_regions_compact(cascade, 3U, 5U);
    expect_true("cascade overlap closes after each merge",
                cascade_count == 1U && cascade[0].x == 0U &&
                    cascade[0].width == 27U);

    nuwax_emotion_asset_region_t many[8] = {
        {.x = 4, .y = 4, .width = 8, .height = 8},
        {.x = 40, .y = 4, .width = 8, .height = 8},
        {.x = 76, .y = 4, .width = 8, .height = 8},
        {.x = 112, .y = 4, .width = 8, .height = 8},
        {.x = 148, .y = 4, .width = 8, .height = 8},
        {.x = 184, .y = 4, .width = 8, .height = 8},
        {.x = 220, .y = 4, .width = 8, .height = 8},
        {.x = 256, .y = 4, .width = 8, .height = 8},
    };
    const uint8_t compact_count =
        nuwax_emotion_component_regions_compact(many, 8U, 5U);
    expect_true("region budget is capped at five",
                compact_count == 5U);
    expect_true("budget compaction leaves disjoint patches",
                regions_are_disjoint(many, compact_count));
    expect_true("compacted regions are in stable display order",
                many[0].y <= many[1].y && many[0].x <= many[1].x);

    printf("emotion component bridge policy host check: %s (%d failures)\n",
           s_failures ? "FAIL" : "PASS", s_failures);
    return s_failures ? 1 : 0;
}
