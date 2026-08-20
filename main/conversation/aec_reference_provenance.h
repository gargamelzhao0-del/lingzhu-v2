#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NUWAX_AEC_PROVENANCE_NONE = 0,
    NUWAX_AEC_PROVENANCE_HARDWARE,
    NUWAX_AEC_PROVENANCE_SOFTWARE,
} nuwax_aec_provenance_kind_t;

typedef struct {
    nuwax_aec_provenance_kind_t kind;
    uint32_t generation;
    bool snapshot;
} nuwax_aec_reference_provenance_t;

static inline nuwax_aec_reference_provenance_t
nuwax_aec_reference_select(bool hardware_reference_active,
                           bool playback_snapshot_stable,
                           uint32_t playback_generation,
                           bool software_reference_snapshot,
                           uint32_t software_reference_generation) {
    if (hardware_reference_active) {
        if (playback_snapshot_stable && playback_generation != 0U) {
            return (nuwax_aec_reference_provenance_t){
                .kind = NUWAX_AEC_PROVENANCE_HARDWARE,
                .generation = playback_generation,
                .snapshot = true,
            };
        }

        /* The capture path already selected hardware for this frame, so an
         * unbound hardware snapshot must fail closed instead of claiming the
         * software reference that was not copied. */
        return (nuwax_aec_reference_provenance_t){0};
    }

    if (software_reference_snapshot && software_reference_generation != 0U) {
        return (nuwax_aec_reference_provenance_t){
            .kind = NUWAX_AEC_PROVENANCE_SOFTWARE,
            .generation = software_reference_generation,
            .snapshot = true,
        };
    }

    return (nuwax_aec_reference_provenance_t){0};
}

static inline bool nuwax_aec_reference_same_epoch(
    nuwax_aec_reference_provenance_t left,
    nuwax_aec_reference_provenance_t right) {
    return left.snapshot && right.snapshot && left.kind == right.kind &&
           left.generation == right.generation;
}

static inline bool nuwax_aec_reference_epoch_contiguous(
    nuwax_aec_reference_provenance_t current,
    nuwax_aec_reference_provenance_t expected,
    bool capture_sequence_contiguous) {
    if (!current.snapshot || !capture_sequence_contiguous) {
        return false;
    }
    return !expected.snapshot ||
           nuwax_aec_reference_same_epoch(current, expected);
}
