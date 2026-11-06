/* Read-only production-source inclusion; NVS is an isolated in-memory stub.
 * Synthetic identity and credentials below are not production credentials. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "nvs.h"
#include "nuwax_cloud_config.c"

static nuwax_cloud_config_t active_slot, pending_slot;
static bool active_present, pending_present;
static unsigned erased_active, commits;

esp_err_t nuwax_device_uid_init(void) { return ESP_OK; }
const char *nuwax_device_uid_string(void) { return "NX-FAKE-TEST-0001"; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) {
    (void)mode; assert(strcmp(name, "nuwax_cloud") == 0); *h = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
    (void)h; bool is_active = strcmp(key, "active") == 0;
    if (!(is_active ? active_present : pending_present)) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < sizeof(active_slot)) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, is_active ? &active_slot : &pending_slot, sizeof(active_slot));
    *len = sizeof(active_slot); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len) {
    (void)h; assert(len == sizeof(active_slot));
    if (strcmp(key, "active") == 0) { memcpy(&active_slot, data, len); active_present = true; }
    else { assert(strcmp(key, "pending") == 0); memcpy(&pending_slot, data, len); pending_present = true; }
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h;
    if (strcmp(key, "active") == 0) { erased_active++; active_present = false; }
    else { assert(strcmp(key, "pending") == 0); pending_present = false; }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; commits++; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

int main(int argc, char **argv) {
    assert(argc == 2);
    active_slot.version = 1;
    strcpy(active_slot.device_id, nuwax_device_uid_string());
    strcpy(active_slot.device_secret, "synthetic-test-credential");
    strcpy(active_slot.cloud_url, strcmp(argv[1], "legacy_ws") == 0
           ? "ws://legacy.example.invalid/device" : "wss://valid.example.invalid/device");
    active_present = true;
    assert(nuwax_cloud_config_init() == ESP_OK);
    const bool should_keep = CONFIG_NUWAX_CLOUD_CONFIG_ALLOW_INSECURE_WS || strcmp(argv[1], "legacy_ws") != 0;
    assert(nuwax_cloud_config_has_config() == should_keep);
    assert(erased_active == (should_keep ? 0U : 1U));
    printf("PASS persisted=%s allow_ws=%d active_retained=%d erased_active=%u commits=%u\n",
           argv[1], CONFIG_NUWAX_CLOUD_CONFIG_ALLOW_INSECURE_WS, should_keep, erased_active, commits);
    nuwax_cloud_config_t candidate = { .version = 1 };
    strcpy(candidate.device_id, nuwax_device_uid_string());
    strcpy(candidate.cloud_url, "wss://unlisted.example.invalid/device");
    strcpy(candidate.device_secret, "synthetic-unverified-cloud-credential");
    assert(nuwax_cloud_config_stage(&candidate) == ESP_OK);
    assert(nuwax_cloud_config_commit_pending() == ESP_OK);
    assert(nuwax_cloud_config_has_config());
    puts("PASS syntactically_valid_unlisted_wss_and_unverified_secret_commit_locally");
    strcpy(candidate.device_id, "NX-DIFFERENT-DEVICE");
    assert(nuwax_cloud_config_stage(&candidate) == ESP_ERR_INVALID_ARG);
    puts("PASS mismatched_device_id_rejected");
    return 0;
}
