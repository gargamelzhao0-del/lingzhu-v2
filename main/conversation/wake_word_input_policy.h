#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline size_t nuwax_wake_input_samples_needed(size_t filled,
                                                     size_t frame_samples) {
    return filled < frame_samples ? frame_samples - filled : 0U;
}

static inline size_t nuwax_wake_input_accept_count(size_t filled,
                                                  size_t received,
                                                  size_t frame_samples) {
    const size_t needed = nuwax_wake_input_samples_needed(filled, frame_samples);
    return received < needed ? received : needed;
}

static inline bool nuwax_wake_input_gap_is_discontinuous(
    uint32_t last_feed_ms,
    uint32_t now_ms,
    uint32_t max_feed_gap_ms) {
    return last_feed_ms != 0U &&
           (uint32_t)(now_ms - last_feed_ms) > max_feed_gap_ms;
}

static inline uint32_t nuwax_wake_input_next_generation(uint32_t current) {
    current++;
    return current == 0U ? 1U : current;
}

static inline bool nuwax_wake_input_generation_changed(
    uint32_t active_generation,
    uint32_t received_generation) {
    return active_generation != 0U &&
           active_generation != received_generation;
}

static inline bool nuwax_wake_input_capture_exit_expected(
    bool stop_requested,
    bool capture_exited,
    bool capture_initialized) {
    return stop_requested && capture_exited && capture_initialized;
}

static inline bool nuwax_wake_input_cooperate_due(
    bool playback_interrupt_mode,
    uint32_t last_block_ms,
    uint32_t now_ms,
    uint32_t interval_ms) {
    return playback_interrupt_mode && interval_ms > 0U &&
           (uint32_t)(now_ms - last_block_ms) >= interval_ms;
}
