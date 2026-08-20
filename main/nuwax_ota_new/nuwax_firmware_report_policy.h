#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_FIRMWARE_REPORT_RETRY_INITIAL_MS 10000U
#define NUWAX_FIRMWARE_REPORT_RETRY_MAX_MS     60000U

typedef struct {
    bool acked;
    uint32_t last_connection_epoch;
    uint32_t last_attempt_ms;
    uint8_t attempts_on_connection;
} nuwax_firmware_report_policy_t;

static inline void nuwax_firmware_report_policy_init(
    nuwax_firmware_report_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }
    policy->acked = false;
    policy->last_connection_epoch = 0U;
    policy->last_attempt_ms = 0U;
    policy->attempts_on_connection = 0U;
}

static inline uint32_t nuwax_firmware_report_policy_retry_ms(
    const nuwax_firmware_report_policy_t *policy)
{
    if (policy == NULL || policy->attempts_on_connection == 0U) {
        return 0U;
    }
    uint32_t retry_ms = NUWAX_FIRMWARE_REPORT_RETRY_INITIAL_MS;
    uint8_t shifts = (uint8_t)(policy->attempts_on_connection - 1U);
    while (shifts-- > 0U && retry_ms < NUWAX_FIRMWARE_REPORT_RETRY_MAX_MS) {
        retry_ms <<= 1U;
    }
    return retry_ms > NUWAX_FIRMWARE_REPORT_RETRY_MAX_MS
               ? NUWAX_FIRMWARE_REPORT_RETRY_MAX_MS
               : retry_ms;
}

static inline bool nuwax_firmware_report_policy_should_send(
    const nuwax_firmware_report_policy_t *policy,
    uint32_t connection_epoch,
    uint32_t now_ms)
{
    if (policy == NULL || policy->acked || connection_epoch == 0U) {
        return false;
    }
    if (policy->last_connection_epoch != connection_epoch ||
        policy->attempts_on_connection == 0U) {
        return true;
    }
    return (uint32_t)(now_ms - policy->last_attempt_ms) >=
           nuwax_firmware_report_policy_retry_ms(policy);
}

static inline void nuwax_firmware_report_policy_note_attempt(
    nuwax_firmware_report_policy_t *policy,
    uint32_t connection_epoch,
    uint32_t now_ms)
{
    if (policy == NULL || connection_epoch == 0U || policy->acked) {
        return;
    }
    if (policy->last_connection_epoch != connection_epoch) {
        policy->last_connection_epoch = connection_epoch;
        policy->attempts_on_connection = 1U;
    } else if (policy->attempts_on_connection < UINT8_MAX) {
        policy->attempts_on_connection++;
    }
    policy->last_attempt_ms = now_ms;
}

static inline bool nuwax_firmware_report_policy_accept_ack(
    nuwax_firmware_report_policy_t *policy,
    uint32_t connection_epoch,
    bool persisted)
{
    if (policy == NULL || policy->acked || !persisted ||
        connection_epoch == 0U ||
        policy->last_connection_epoch != connection_epoch ||
        policy->attempts_on_connection == 0U) {
        return false;
    }
    policy->acked = true;
    return true;
}
