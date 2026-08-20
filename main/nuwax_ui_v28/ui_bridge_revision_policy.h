#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool nuwax_ui_bridge_revision_is_newer(uint32_t candidate,
                                                     uint32_t accepted)
{
    return candidate != 0U &&
           (accepted == 0U || (int32_t)(candidate - accepted) > 0);
}

static inline bool nuwax_ui_bridge_owned_revision_is_rejected(
    uint32_t candidate,
    uint32_t reject_through)
{
    return candidate != 0U && reject_through != 0U &&
           !nuwax_ui_bridge_revision_is_newer(candidate, reject_through);
}

static inline uint32_t nuwax_ui_bridge_filter_newer_fields(
    uint32_t candidate,
    uint32_t fields,
    uint32_t *field_revisions,
    uint32_t field_count)
{
    if (!field_revisions || candidate == 0U) return 0U;
    uint32_t accepted_fields = 0U;
    for (uint32_t index = 0U; index < field_count && index < 32U;
         ++index) {
        const uint32_t field = 1UL << index;
        if ((fields & field) != 0U &&
            nuwax_ui_bridge_revision_is_newer(
                candidate, field_revisions[index])) {
            field_revisions[index] = candidate;
            accepted_fields |= field;
        }
    }
    return accepted_fields;
}
