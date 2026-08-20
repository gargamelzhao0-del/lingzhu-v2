#include <stdio.h>
#include <string.h>

#include "nuwax_provision_policy.h"

static int failures;

static void expect_true(const char *name, bool value) {
    if (!value) {
        fprintf(stderr, "[FAIL] %s\n", name);
        ++failures;
    } else {
        printf("[PASS] %s\n", name);
    }
}

int main(void) {
    expect_true("verified normal boot keeps the runtime profile",
                !nuwax_provision_boot_profile_required(
                    true, false, true, true, true, true, false));
    expect_true("missing app Wi-Fi credentials selects provisioning profile",
                nuwax_provision_boot_profile_required(
                    true, false, true, true, true, false, false));
    expect_true("late provisioning recovery selects provisioning profile",
                nuwax_provision_boot_profile_required(
                    true, false, true, true, true, true, true));
    expect_true("invalid reprovision state fails closed",
                nuwax_provision_boot_profile_required(
                    false, false, true, true, true, true, false));
    expect_true("incomplete cloud/marker state selects provisioning profile",
                nuwax_provision_boot_profile_required(
                    true, false, false, true, true, true, false) &&
                nuwax_provision_boot_profile_required(
                    true, false, true, false, false, true, false));
    expect_true("verified Wi-Fi and cloud config complete the boot pair",
                nuwax_provision_pair_ready(true, true, true));
    expect_true("verified Wi-Fi without cloud config remains unprovisioned",
                !nuwax_provision_pair_ready(true, true, false));
    expect_true("unverified Wi-Fi cannot complete the boot pair",
                !nuwax_provision_pair_ready(true, false, true));

    expect_true("reserve boundary is accepted",
                !nuwax_provision_memory_below_reserve(
                    32768, 15360, 16384, 32768, 15360, 16384));
    expect_true("low total internal memory is rejected",
                nuwax_provision_memory_below_reserve(
                    32767, 32768, 32768, 32768, 16384, 16384));
    expect_true("small internal block is rejected",
                nuwax_provision_memory_below_reserve(
                    65536, 15359, 32768, 32768, 15360, 16384));
    expect_true("low DMA memory is rejected",
                nuwax_provision_memory_below_reserve(
                    65536, 32768, 16383, 32768, 16384, 16384));

    const uint8_t ssid[] = "OfficeWiFi";
    const uint8_t old_password[] = "correct-password";
    const uint8_t new_password[] = "new-correct-password";
    const uint32_t old_hash = nuwax_provision_credentials_hash(
        ssid, strlen((const char *)ssid), old_password,
        strlen((const char *)old_password));
    const uint32_t same_hash = nuwax_provision_credentials_hash(
        ssid, strlen((const char *)ssid), old_password,
        strlen((const char *)old_password));
    const uint32_t new_hash = nuwax_provision_credentials_hash(
        ssid, strlen((const char *)ssid), new_password,
        strlen((const char *)new_password));
    expect_true("credential hash is deterministic", old_hash == same_hash);
    expect_true("same SSID with new password invalidates verification",
                old_hash != new_hash);

    expect_true("old Wi-Fi is suppressed during focused BLE wait",
                !nuwax_provision_should_try_old_wifi(
                    false, true, false, false, false,
                    NUWAX_PROVISION_WIFI_OWNER_NONE));
    expect_true("old Wi-Fi recovery starts after focused wait",
                nuwax_provision_should_try_old_wifi(
                    true, true, false, false, false,
                    NUWAX_PROVISION_WIFI_OWNER_NONE));
    expect_true("versioned rollback record with all flags is restorable",
                nuwax_provision_rollback_record_can_restore(
                    2, 2, 0x3, 0x3, true));
    expect_true("legacy rollback record is not silently trusted",
                !nuwax_provision_rollback_record_can_restore(
                    1, 2, 0x3, 0x3, true));
    expect_true("rollback record requires every durability flag",
                !nuwax_provision_rollback_record_can_restore(
                    2, 2, 0x1, 0x3, true));
    expect_true("old Wi-Fi can finish only with active cloud config",
                nuwax_provision_old_wifi_is_terminal_candidate(
                    true, true, true));
    expect_true("missing cloud config keeps BLE provisioning active",
                !nuwax_provision_old_wifi_is_terminal_candidate(
                    true, true, false));
    expect_true("BLE connection suppresses old Wi-Fi recovery",
                !nuwax_provision_should_try_old_wifi(
                    true, true, true, false, false,
                    NUWAX_PROVISION_WIFI_OWNER_NONE));
    expect_true("secured APP session suppresses old Wi-Fi recovery",
                !nuwax_provision_should_try_old_wifi(
                    true, true, false, true, false,
                    NUWAX_PROVISION_WIFI_OWNER_NONE));
    expect_true("APP GOT_IP requires active new credentials",
                !nuwax_provision_accepts_got_ip(
                    NUWAX_PROVISION_WIFI_OWNER_APP, false));
    expect_true("active APP credentials accept GOT_IP",
                nuwax_provision_accepts_got_ip(
                    NUWAX_PROVISION_WIFI_OWNER_APP, true));
    expect_true("Wi-Fi attempt belongs to the current owner generation",
                nuwax_provision_attempt_belongs_to_owner(9, 9));
    expect_true("stale Wi-Fi attempt cannot commit for a new owner",
                !nuwax_provision_attempt_belongs_to_owner(8, 9));
    expect_true("unassigned Wi-Fi attempt cannot commit",
                !nuwax_provision_attempt_belongs_to_owner(0, 0));
    expect_true("old owner accepts GOT_IP",
                nuwax_provision_accepts_got_ip(
                    NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY, false));
    expect_true("APP cannot steal an already connected old network",
                !nuwax_provision_app_can_claim_wifi(true));
    expect_true("BLE APP can connect while provisioning is idle",
                nuwax_provision_app_can_accept_ble(
                    false, false, false, false,
                    NUWAX_PROVISION_WIFI_OWNER_NONE));
    expect_true("BLE APP can preempt an old Wi-Fi attempt before GOT_IP",
                nuwax_provision_app_can_accept_ble(
                    false, false, false, false,
                    NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY));
    expect_true("BLE APP cannot race a Wi-Fi state reset",
                !nuwax_provision_app_can_accept_ble(
                    false, false, true, false,
                    NUWAX_PROVISION_WIFI_OWNER_NONE));
    expect_true("BLE APP cannot cancel an aborted session cleanup",
                !nuwax_provision_app_can_accept_ble(
                    false, false, false, true,
                    NUWAX_PROVISION_WIFI_OWNER_APP));
    expect_true("BLE APP cannot replace an owner before disconnect cleanup",
                !nuwax_provision_app_can_accept_ble(
                    false, false, false, false,
                    NUWAX_PROVISION_WIFI_OWNER_APP));
    expect_true("BLE APP cannot claim after provisioning completes",
                !nuwax_provision_app_can_accept_ble(
                    true, false, false, false,
                    NUWAX_PROVISION_WIFI_OWNER_NONE));
    expect_true("matching old Wi-Fi generation can release ownership",
                nuwax_provision_old_owner_matches(
                    NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY, 7, 7));
    expect_true("stale old Wi-Fi cleanup cannot release APP ownership",
                !nuwax_provision_old_owner_matches(
                    NUWAX_PROVISION_WIFI_OWNER_APP, 8, 7));
    expect_true("stale generation cannot release a newer old Wi-Fi attempt",
                !nuwax_provision_old_owner_matches(
                    NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY, 8, 7));
    expect_true("secured APP events require a live claimed BLE session",
                nuwax_provision_app_session_active(
                    true, true, false, NUWAX_PROVISION_WIFI_OWNER_APP));
    expect_true("late security event is rejected after APP abort",
                !nuwax_provision_app_session_active(
                    true, true, true, NUWAX_PROVISION_WIFI_OWNER_APP));
    expect_true("late security event is rejected after ownership release",
                !nuwax_provision_app_session_active(
                    true, false, false, NUWAX_PROVISION_WIFI_OWNER_NONE));
    expect_true("pre-config APP session allows the Wi-Fi selection flow",
                !nuwax_provision_app_session_idle_timed_out(
                    true, true, false, false, 1000000, 61000000,
                    300000, 60000));
    expect_true("pre-config APP session expires after five minutes",
                nuwax_provision_app_session_idle_timed_out(
                    true, true, false, false, 1000000, 301000000,
                    300000, 60000));
    expect_true("staged config uses the shorter recovery timeout",
                nuwax_provision_app_session_idle_timed_out(
                    true, true, true, false, 1000000, 61000000,
                    300000, 60000));
    expect_true("recent staged-config progress does not expire",
                !nuwax_provision_app_session_idle_timed_out(
                    true, true, true, false, 2000000, 61000000,
                    300000, 60000));
    expect_true("active Wi-Fi attempt is not interrupted by APP idle timeout",
                !nuwax_provision_app_session_idle_timed_out(
                    true, true, true, true, 1000000, 301000000,
                    300000, 60000));
    expect_true("Wi-Fi attempt times out after GOT_IP is lost",
                nuwax_provision_wifi_attempt_timed_out(
                    true, false, 1000000, 31000000, 30000000));
    expect_true("connected Wi-Fi is not reset by observation timeout",
                !nuwax_provision_wifi_attempt_timed_out(
                    true, true, 1000000, 31000000, 30000000));
    expect_true("failed APP session is retained during status hold",
                !nuwax_provision_failed_session_release_due(
                    true, false, false, 6000000, 5999999));
    expect_true("failed APP session is released when status hold expires",
                nuwax_provision_failed_session_release_due(
                    true, false, false, 6000000, 6000000));
    expect_true("corrected credentials cancel failed-session release",
                !nuwax_provision_failed_session_release_due(
                    true, false, true, 6000000, 7000000));
    expect_true("active corrected attempt cannot be released as stale",
                !nuwax_provision_failed_session_release_due(
                    true, true, false, 6000000, 7000000));
    expect_true("cleared failed-session deadline cannot release owner",
                !nuwax_provision_failed_session_release_due(
                    true, false, false, 0, 7000000));
    expect_true("unsecured session uses the Security 2 setup timeout",
                !nuwax_provision_app_session_idle_timed_out(
                    true, false, false, false, 1000000, 301000000,
                    300000, 60000));
    expect_true("pre-config timeout selector returns five minutes",
                nuwax_provision_app_session_idle_timeout_ms(
                    false, 300000, 60000) == 300000);
    expect_true("post-config timeout selector returns one minute",
                nuwax_provision_app_session_idle_timeout_ms(
                    true, 300000, 60000) == 60000);
    expect_true("normal boot provisioning has no runtime-rebind idle exit",
                !nuwax_provision_runtime_idle_timed_out(
                    false, false, 1000000, 601000000, 600000));
    expect_true("runtime rebind remains active before inactivity deadline",
                !nuwax_provision_runtime_idle_timed_out(
                    true, false, 1000000, 600999999, 600000));
    expect_true("runtime rebind exits at inactivity deadline",
                nuwax_provision_runtime_idle_timed_out(
                    true, false, 1000000, 601000000, 600000));
    expect_true("active Wi-Fi attempt owns its result timeout",
                !nuwax_provision_runtime_idle_timed_out(
                    true, true, 1000000, 901000000, 600000));
    expect_true("new APP progress renews runtime rebind idle budget",
                !nuwax_provision_runtime_idle_timed_out(
                    true, false, 500000000, 601000000, 600000));
    expect_true("reset retry starts at the base delay",
                nuwax_provision_reset_retry_delay_ms(1, 250, 4000) == 250);
    expect_true("reset retry uses bounded exponential backoff",
                nuwax_provision_reset_retry_delay_ms(4, 250, 4000) == 2000);
    expect_true("reset retry delay is capped",
                nuwax_provision_reset_retry_delay_ms(8, 250, 4000) == 4000);
    expect_true("fresh provisioning supervisor heartbeat remains healthy",
                !nuwax_provision_supervisor_stalled(
                    1000000, 30999999, 30000));
    expect_true("provisioning supervisor heartbeat expires at deadline",
                nuwax_provision_supervisor_stalled(
                    1000000, 31000000, 30000));
    expect_true("missing provisioning supervisor heartbeat fails closed",
                nuwax_provision_supervisor_stalled(0, 31000000, 30000));
    expect_true("future heartbeat is not treated as stalled",
                !nuwax_provision_supervisor_stalled(
                    32000000, 31000000, 30000));

    if (failures != 0) {
        fprintf(stderr, "provisioning policy host check: FAIL (%d failures)\n",
                failures);
        return 1;
    }
    printf("provisioning policy host check: PASS\n");
    return 0;
}
