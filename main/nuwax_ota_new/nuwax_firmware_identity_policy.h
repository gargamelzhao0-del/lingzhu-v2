#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUWAX_FIRMWARE_IDENTITY_RETRY_BASE_MS 5000U
#define NUWAX_FIRMWARE_IDENTITY_RETRY_MAX_MS  60000U

typedef enum {
    NUWAX_FIRMWARE_IDENTITY_EMPTY = 0,
    NUWAX_FIRMWARE_IDENTITY_REQUESTED,
    NUWAX_FIRMWARE_IDENTITY_RUNNING,
    NUWAX_FIRMWARE_IDENTITY_READY,
    NUWAX_FIRMWARE_IDENTITY_FAILED,
} nuwax_firmware_identity_state_t;

typedef struct {
    nuwax_firmware_identity_state_t state;
    uint32_t failure_count;
    uint32_t retry_at_ms;
} nuwax_firmware_identity_policy_t;

static inline bool nuwax_firmware_identity_deadline_reached(uint32_t now_ms,
                                                            uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static inline void nuwax_firmware_identity_policy_init(
    nuwax_firmware_identity_policy_t *policy)
{
    if (policy == NULL) return;
    policy->state = NUWAX_FIRMWARE_IDENTITY_EMPTY;
    policy->failure_count = 0U;
    policy->retry_at_ms = 0U;
}

static inline bool nuwax_firmware_identity_policy_request(
    nuwax_firmware_identity_policy_t *policy,
    uint32_t now_ms)
{
    if (policy == NULL ||
        policy->state == NUWAX_FIRMWARE_IDENTITY_REQUESTED ||
        policy->state == NUWAX_FIRMWARE_IDENTITY_RUNNING ||
        policy->state == NUWAX_FIRMWARE_IDENTITY_READY) {
        return false;
    }
    if (policy->state == NUWAX_FIRMWARE_IDENTITY_FAILED &&
        !nuwax_firmware_identity_deadline_reached(now_ms,
                                                  policy->retry_at_ms)) {
        return false;
    }
    policy->state = NUWAX_FIRMWARE_IDENTITY_REQUESTED;
    return true;
}

static inline bool nuwax_firmware_identity_policy_begin(
    nuwax_firmware_identity_policy_t *policy)
{
    if (policy == NULL ||
        policy->state != NUWAX_FIRMWARE_IDENTITY_REQUESTED) {
        return false;
    }
    policy->state = NUWAX_FIRMWARE_IDENTITY_RUNNING;
    return true;
}

static inline uint32_t nuwax_firmware_identity_retry_delay_ms(
    uint32_t failure_count)
{
    uint32_t delay_ms = NUWAX_FIRMWARE_IDENTITY_RETRY_BASE_MS;
    uint32_t shifts = failure_count > 0U ? failure_count - 1U : 0U;
    while (shifts-- > 0U &&
           delay_ms < NUWAX_FIRMWARE_IDENTITY_RETRY_MAX_MS) {
        if (delay_ms > NUWAX_FIRMWARE_IDENTITY_RETRY_MAX_MS / 2U) {
            delay_ms = NUWAX_FIRMWARE_IDENTITY_RETRY_MAX_MS;
            break;
        }
        delay_ms *= 2U;
    }
    return delay_ms > NUWAX_FIRMWARE_IDENTITY_RETRY_MAX_MS
               ? NUWAX_FIRMWARE_IDENTITY_RETRY_MAX_MS
               : delay_ms;
}

static inline void nuwax_firmware_identity_policy_complete(
    nuwax_firmware_identity_policy_t *policy,
    bool success,
    uint32_t now_ms)
{
    if (policy == NULL ||
        (policy->state != NUWAX_FIRMWARE_IDENTITY_RUNNING &&
         policy->state != NUWAX_FIRMWARE_IDENTITY_REQUESTED)) {
        return;
    }
    if (success) {
        policy->state = NUWAX_FIRMWARE_IDENTITY_READY;
        policy->failure_count = 0U;
        policy->retry_at_ms = 0U;
        return;
    }
    if (policy->failure_count != UINT32_MAX) {
        policy->failure_count++;
    }
    policy->state = NUWAX_FIRMWARE_IDENTITY_FAILED;
    policy->retry_at_ms =
        now_ms + nuwax_firmware_identity_retry_delay_ms(policy->failure_count);
}

#ifdef __cplusplus
}
#endif
