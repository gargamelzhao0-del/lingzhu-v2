#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"

typedef struct {
    bool pending;
    nuwax_app_state_t state;
    uint32_t generation;
} nuwax_ui_sync_pending_t;

static inline void nuwax_ui_sync_submit(nuwax_ui_sync_pending_t *sync,
                                        nuwax_app_state_t state,
                                        uint32_t generation) {
    if (!sync) {
        return;
    }
    sync->pending = true;
    sync->state = state;
    sync->generation = generation;
}

static inline bool nuwax_ui_sync_matches(const nuwax_ui_sync_pending_t *sync,
                                         nuwax_app_state_t state,
                                         uint32_t generation) {
    return sync && sync->pending && sync->state == state &&
           sync->generation == generation;
}

static inline bool nuwax_ui_sync_complete(nuwax_ui_sync_pending_t *sync,
                                          nuwax_app_state_t state,
                                          uint32_t generation) {
    if (!nuwax_ui_sync_matches(sync, state, generation)) {
        return false;
    }
    sync->pending = false;
    return true;
}
