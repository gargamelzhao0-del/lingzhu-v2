#pragma once

#include <stdbool.h>

typedef enum {
    NUWAX_OTA_RESULT_ACK_REJECT = 0,
    NUWAX_OTA_RESULT_ACK_STRICT_ECHO,
    NUWAX_OTA_RESULT_ACK_PERSISTED_SUCCESS_COMPAT,
} nuwax_ota_result_ack_decision_t;

/* The formal protocol echoes ota_result.ok. Deployed gateways may instead use
 * true to mean that a negative result was durably persisted. This compatibility
 * is safe only after the caller has matched the authenticated epoch, update ID,
 * and in-flight terminal-report transaction. */
static inline nuwax_ota_result_ack_decision_t
nuwax_ota_result_ack_policy_decide(bool expected_result_ok,
                                   bool received_ack_ok)
{
    if (received_ack_ok == expected_result_ok) {
        return NUWAX_OTA_RESULT_ACK_STRICT_ECHO;
    }
    if (!expected_result_ok && received_ack_ok) {
        return NUWAX_OTA_RESULT_ACK_PERSISTED_SUCCESS_COMPAT;
    }
    return NUWAX_OTA_RESULT_ACK_REJECT;
}
