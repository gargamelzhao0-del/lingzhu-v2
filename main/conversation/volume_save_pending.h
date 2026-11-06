#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Caller serializes access. Revision protects a newer request arriving while
 * the owner is outside its lock performing a Flash transaction. */
typedef struct {
    uint32_t revision;
    uint64_t due_ms;
    uint8_t value;
    bool pending;
} nuwax_volume_save_pending_t;

static inline void nuwax_volume_save_request(nuwax_volume_save_pending_t *s,
                                              uint8_t value, uint64_t now_ms) {
    if (++s->revision == 0U) ++s->revision;
    s->value = value;
    s->pending = true;
    s->due_ms = now_ms + 1500U;
}

static inline bool nuwax_volume_save_due(const nuwax_volume_save_pending_t *s,
                                          uint64_t now_ms, bool force) {
    return s->pending && (force || now_ms >= s->due_ms);
}

static inline void nuwax_volume_save_complete(nuwax_volume_save_pending_t *s,
    uint32_t revision, bool success, uint64_t now_ms) {
    if (s->revision != revision) return;
    if (success) s->pending = false;
    else s->due_ms = now_ms + 5000U;
}
