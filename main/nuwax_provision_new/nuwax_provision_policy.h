#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NUWAX_PROVISION_WIFI_OWNER_NONE = 0,
    NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY,
    NUWAX_PROVISION_WIFI_OWNER_APP,
} nuwax_provision_wifi_owner_t;

static inline bool nuwax_provision_boot_profile_required(
    bool reprovision_state_valid, bool force_reprovision,
    bool cloud_config_ready, bool verified_marker_present,
    bool verified_marker_matches, bool app_wifi_credentials_present,
    bool low_memory_recovery_requested) {
    return !reprovision_state_valid || force_reprovision ||
           low_memory_recovery_requested || !cloud_config_ready ||
           !verified_marker_present || !verified_marker_matches ||
           !app_wifi_credentials_present;
}

static inline bool nuwax_provision_pair_ready(
    bool wifi_credentials_present, bool wifi_verified,
    bool cloud_config_ready) {
    return wifi_credentials_present && wifi_verified && cloud_config_ready;
}

static inline bool nuwax_provision_memory_below_reserve(
    size_t internal_free, size_t internal_largest, size_t dma_free,
    size_t internal_free_min, size_t internal_largest_min,
    size_t dma_free_min) {
    return internal_free < internal_free_min ||
           internal_largest < internal_largest_min ||
           dma_free < dma_free_min;
}

static inline uint32_t nuwax_provision_fnv1a_update(
    uint32_t hash, const uint8_t *value, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        hash ^= value[i];
        hash *= 16777619U;
    }
    return hash;
}

static inline uint32_t nuwax_provision_credentials_hash(
    const uint8_t *ssid, size_t ssid_length,
    const uint8_t *password, size_t password_length) {
    uint32_t hash = 2166136261U;
    hash = nuwax_provision_fnv1a_update(hash, ssid, ssid_length);
    const uint8_t separator = 0xff;
    hash = nuwax_provision_fnv1a_update(hash, &separator, 1);
    return nuwax_provision_fnv1a_update(hash, password, password_length);
}

static inline bool nuwax_provision_should_try_old_wifi(
    bool focused_wait_elapsed, bool has_old_wifi,
    bool ble_connected, bool security_secured,
    bool new_wifi_attempt_active,
    nuwax_provision_wifi_owner_t owner) {
    return focused_wait_elapsed && has_old_wifi && !ble_connected &&
           !security_secured && !new_wifi_attempt_active &&
           owner == NUWAX_PROVISION_WIFI_OWNER_NONE;
}

static inline bool nuwax_provision_rollback_record_can_restore(
    uint32_t version, uint32_t expected_version,
    uint32_t flags, uint32_t required_flags, bool has_ssid) {
    return version == expected_version &&
           (flags & required_flags) == required_flags && has_ssid;
}

static inline bool nuwax_provision_old_wifi_is_terminal_candidate(
    bool force_reprovision, bool rollback_record_ready,
    bool cloud_config_active) {
    return force_reprovision && rollback_record_ready && cloud_config_active;
}

static inline bool nuwax_provision_wifi_attempt_timed_out(
    bool attempt_active, bool got_ip, int64_t received_us,
    int64_t now_us, int64_t timeout_us) {
    return attempt_active && !got_ip && received_us > 0 &&
           now_us >= received_us && now_us - received_us >= timeout_us;
}

static inline bool nuwax_provision_failed_session_release_due(
    bool release_pending, bool attempt_active, bool credentials_started,
    int64_t deadline_us, int64_t now_us) {
    return release_pending && !attempt_active && !credentials_started &&
           deadline_us > 0 && now_us >= deadline_us;
}

static inline bool nuwax_provision_accepts_got_ip(
    nuwax_provision_wifi_owner_t owner, bool new_wifi_attempt_active) {
    return owner == NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY ||
           (owner == NUWAX_PROVISION_WIFI_OWNER_APP &&
            new_wifi_attempt_active);
}

static inline bool nuwax_provision_attempt_belongs_to_owner(
    uint32_t attempt_owner_generation, uint32_t owner_generation) {
    return attempt_owner_generation != 0 &&
           attempt_owner_generation == owner_generation;
}

static inline bool nuwax_provision_app_can_claim_wifi(bool old_wifi_got_ip) {
    return !old_wifi_got_ip;
}

static inline bool nuwax_provision_app_can_accept_ble(
    bool provisioning_complete, bool old_wifi_got_ip,
    bool reset_pending, bool app_abort_pending,
    nuwax_provision_wifi_owner_t owner) {
    return !provisioning_complete && !old_wifi_got_ip &&
           !reset_pending && !app_abort_pending &&
           owner != NUWAX_PROVISION_WIFI_OWNER_APP;
}

static inline uint32_t nuwax_provision_reset_retry_delay_ms(
    unsigned consecutive_failures, uint32_t base_ms, uint32_t maximum_ms) {
    if (consecutive_failures == 0 || base_ms >= maximum_ms) {
        return base_ms < maximum_ms ? base_ms : maximum_ms;
    }
    uint32_t delay_ms = base_ms;
    for (unsigned i = 1; i < consecutive_failures; ++i) {
        if (delay_ms >= maximum_ms / 2U) {
            return maximum_ms;
        }
        delay_ms *= 2U;
    }
    return delay_ms < maximum_ms ? delay_ms : maximum_ms;
}

static inline bool nuwax_provision_old_owner_matches(
    nuwax_provision_wifi_owner_t owner, uint32_t current_generation,
    uint32_t expected_generation) {
    return owner == NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY &&
           current_generation == expected_generation;
}

static inline bool nuwax_provision_app_session_active(
    bool ble_connected, bool app_claim_active, bool app_abort_pending,
    nuwax_provision_wifi_owner_t owner) {
    return ble_connected && app_claim_active && !app_abort_pending &&
           owner == NUWAX_PROVISION_WIFI_OWNER_APP;
}

static inline bool nuwax_provision_app_session_idle_timed_out(
    bool ble_connected, bool security_secured,
    bool wifi_config_admitted, bool new_wifi_attempt_active,
    int64_t last_progress_us, int64_t now_us,
    uint32_t preconfig_timeout_ms, uint32_t postconfig_timeout_ms) {
    const uint32_t timeout_ms = wifi_config_admitted
                                    ? postconfig_timeout_ms
                                    : preconfig_timeout_ms;
    return ble_connected && security_secured && !new_wifi_attempt_active &&
           last_progress_us > 0 && now_us >= last_progress_us &&
           (uint64_t)(now_us - last_progress_us) >=
               (uint64_t)timeout_ms * 1000U;
}

static inline uint32_t nuwax_provision_app_session_idle_timeout_ms(
    bool wifi_config_admitted, uint32_t preconfig_timeout_ms,
    uint32_t postconfig_timeout_ms) {
    return wifi_config_admitted ? postconfig_timeout_ms
                                : preconfig_timeout_ms;
}

static inline bool nuwax_provision_runtime_idle_timed_out(
    bool runtime_rebind, bool new_wifi_attempt_active,
    int64_t last_activity_us, int64_t now_us, uint32_t timeout_ms) {
    return runtime_rebind && !new_wifi_attempt_active && timeout_ms > 0U &&
           last_activity_us > 0 && now_us >= last_activity_us &&
           (uint64_t)(now_us - last_activity_us) >=
               (uint64_t)timeout_ms * 1000U;
}

static inline bool nuwax_provision_supervisor_stalled(
    int64_t heartbeat_us, int64_t now_us, uint32_t timeout_ms) {
    if (heartbeat_us <= 0) {
        return true;
    }
    if (now_us < heartbeat_us) {
        return false;
    }
    return (uint64_t)(now_us - heartbeat_us) >=
           (uint64_t)timeout_ms * 1000U;
}
