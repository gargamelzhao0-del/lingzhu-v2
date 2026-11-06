/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure release-sequence rules shared by the ESP implementation and host checks.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Protocol floor for validating persisted OTA transaction records. */
#define NUWAX_OTA_BASE_RELEASE_SEQUENCE 7000000ULL
/* Release identity compiled into this firmware image. */
#ifndef NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE
#define NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE 7000103ULL
#endif

#if NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE < NUWAX_OTA_BASE_RELEASE_SEQUENCE
#error "Firmware release sequence must not be below the OTA protocol floor"
#endif

static inline uint64_t nuwax_ota_release_floor(uint64_t accepted_sequence)
{
    return accepted_sequence > NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE
               ? accepted_sequence
               : NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE;
}

static inline bool nuwax_ota_release_can_start(uint64_t durable_sequence,
                                               uint64_t effective_sequence)
{
    return nuwax_ota_release_floor(durable_sequence) >= effective_sequence;
}

static inline bool nuwax_ota_release_pending_matches_firmware(
    uint64_t pending_sequence)
{
    return pending_sequence == NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE;
}

static inline bool nuwax_ota_release_pending_is_newer(
    uint64_t pending_sequence, uint64_t accepted_sequence)
{
    return pending_sequence > accepted_sequence;
}

static inline bool nuwax_ota_release_running_meets_floor(
    uint64_t firmware_sequence, uint64_t accepted_sequence)
{
    return firmware_sequence >= accepted_sequence;
}

static inline bool nuwax_ota_release_pending_may_recover(
    uint64_t pending_sequence, uint64_t accepted_sequence)
{
    return pending_sequence >= accepted_sequence;
}

static inline uint64_t nuwax_ota_release_advance(uint64_t current_sequence,
                                                 uint64_t candidate_sequence)
{
    return candidate_sequence >= NUWAX_OTA_BASE_RELEASE_SEQUENCE &&
                   candidate_sequence > current_sequence
               ? candidate_sequence
               : current_sequence;
}
