#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Application rejections are delivered by the RX task while connection
 * ownership stays with app_main. Keep the handoff epoch-bound so a late frame
 * from a retired RX task cannot change the state of a newer connection. */
static inline bool nuwax_cloud_epoch_is_current(uint32_t active_epoch,
                                                uint32_t candidate_epoch)
{
    return active_epoch != 0U && active_epoch == candidate_epoch;
}

static inline bool nuwax_cloud_epoch_record_provision_required(
    uint32_t active_epoch,
    uint32_t rejected_epoch,
    uint32_t *pending_epoch)
{
    if (pending_epoch == NULL ||
        !nuwax_cloud_epoch_is_current(active_epoch, rejected_epoch)) {
        return false;
    }
    *pending_epoch = rejected_epoch;
    return true;
}

static inline bool nuwax_cloud_epoch_accepts_positive(
    uint32_t active_epoch,
    uint32_t message_epoch,
    uint32_t provision_required_epoch)
{
    return nuwax_cloud_epoch_is_current(active_epoch, message_epoch) &&
           provision_required_epoch != message_epoch;
}

static inline bool nuwax_cloud_epoch_consume_provision_required(
    uint32_t expected_epoch,
    uint32_t *pending_epoch)
{
    if (pending_epoch == NULL || expected_epoch == 0U ||
        *pending_epoch != expected_epoch) {
        return false;
    }
    *pending_epoch = 0U;
    return true;
}
