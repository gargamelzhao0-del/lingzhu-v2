#include "nuwax_provision_main.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "board/nuwax_board.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_srp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs.h"
#include "protocomm.h"
#include "protocomm_security.h"
#include <protocomm_ble.h>

#include "nuwax_device_uid.h"
#include "nuwax_cloud_config.h"
#include "nuwax_provision_policy.h"
#include "nuwax_provision_qr_payload.h"
#include "wake_word_detector.h"
#include "wifi_manager.h"

static const char *TAG = "nuwax_provision";
static const char *const DEVICE_INFO_EP = "device-info";
static const char *const VOX_CONFIG_EP = "vox-config";
#define CREDENTIAL_OBSERVATION_TIMEOUT_US 30000000LL
#define ROLLBACK_CONNECT_TIMEOUT_MS 30000U
#define PROVISION_SUPERVISOR_POLL_MS 50U
#define PROVISION_RESET_RETRY_BASE_MS 250U
#define PROVISION_RESET_RETRY_MAX_MS 4000U
#define PROVISION_RESET_RESTART_FAILURES 8U
#define PROVISION_FINALIZE_RETRY_BASE_MS 250U
#define PROVISION_FINALIZE_RETRY_MAX_MS 4000U
#define PROVISION_FINALIZE_RESTART_FAILURES 8U
#define PROVISION_NVS_FLASH_GUARD_TIMEOUT_MS 500U
#ifndef CONFIG_NUWAX_PROVISION_SECURITY_SESSION_TIMEOUT_MS
#define CONFIG_NUWAX_PROVISION_SECURITY_SESSION_TIMEOUT_MS 15000
#endif
#ifndef CONFIG_NUWAX_PROVISION_APP_SESSION_IDLE_TIMEOUT_MS
#define CONFIG_NUWAX_PROVISION_APP_SESSION_IDLE_TIMEOUT_MS 60000
#endif
#ifndef CONFIG_NUWAX_PROVISION_APP_PRECONFIG_IDLE_TIMEOUT_MS
#define CONFIG_NUWAX_PROVISION_APP_PRECONFIG_IDLE_TIMEOUT_MS 300000
#endif
#ifndef CONFIG_NUWAX_PROVISION_FAILURE_STATUS_HOLD_MS
#define CONFIG_NUWAX_PROVISION_FAILURE_STATUS_HOLD_MS 5000
#endif
#ifndef CONFIG_NUWAX_PROVISION_RUNTIME_IDLE_TIMEOUT_MS
#define CONFIG_NUWAX_PROVISION_RUNTIME_IDLE_TIMEOUT_MS 600000
#endif
#ifndef CONFIG_NUWAX_PROVISION_OLD_WIFI_ATTEMPT_MS
#define CONFIG_NUWAX_PROVISION_OLD_WIFI_ATTEMPT_MS 20000
#endif
#ifndef CONFIG_NUWAX_PROVISION_OLD_WIFI_RETRY_MS
#define CONFIG_NUWAX_PROVISION_OLD_WIFI_RETRY_MS 15000
#endif
#ifndef CONFIG_NUWAX_PROVISION_VOX_CONFIG_JOB_TIMEOUT_MS
#define CONFIG_NUWAX_PROVISION_VOX_CONFIG_JOB_TIMEOUT_MS 15000
#endif
#define PROVISION_INTERNAL_FREE_MIN_BYTES 32768U
/* Runtime reprovisioning reaches this guard only after the wake model,
 * speaker output and live AEC owners have been quiesced, and after NimBLE
 * plus both application endpoints have allocated their steady-state objects.
 * The production heap is quantized in 512-byte blocks; allowing one block of
 * allocator jitter avoids rejecting a valid runtime start when the measured
 * total is 133 bytes below the boot-profile floor.  The strict floor remains
 * in force for boot provisioning and for Wi-Fi recovery diagnostics. */
#define PROVISION_RUNTIME_INTERNAL_FREE_MIN_BYTES 32256U
/* Runtime provisioning retains the internal LVGL and display DMA buffers.
 * On the production S3 heap this leaves a 31 x 512-byte contiguous block
 * after audio, AEC, wake and TLS teardown. NimBLE has already completed its
 * controller/host allocation when this guard runs, so retain one 512-byte
 * allocator quantum below the observed 15872-byte block while keeping the
 * total internal and DMA floors unchanged. */
#define PROVISION_INTERNAL_LARGEST_MIN_BYTES 15360U
#define PROVISION_INTERNAL_STARTUP_RESERVE_BYTES 15872U
#define PROVISION_DMA_FREE_MIN_BYTES 16384U
_Static_assert(PROVISION_RUNTIME_INTERNAL_FREE_MIN_BYTES <
                   PROVISION_INTERNAL_FREE_MIN_BYTES,
               "runtime provisioning floor must remain below the strict boot floor");
#define PERSIST_RETRY_COUNT 3
#define PERSIST_RETRY_DELAY_MS 100U
#ifndef CONFIG_NUWAX_PROVISION_SUCCESS_BLE_HOLD_MS
#define CONFIG_NUWAX_PROVISION_SUCCESS_BLE_HOLD_MS 15000
#endif
static bool s_initialized;
static atomic_bool s_started = ATOMIC_VAR_INIT(false);
static bool s_runtime_start;
typedef enum {
    PROVISION_CANCEL_DISABLED = 0,
    PROVISION_CANCEL_AVAILABLE,
    PROVISION_CANCEL_REQUESTED,
    PROVISION_CANCEL_COMMITTING,
    PROVISION_CANCEL_COMMITTED,
} provision_cancel_state_t;
static atomic_int s_cancel_state = ATOMIC_VAR_INIT(PROVISION_CANCEL_DISABLED);
static bool s_provisioned_pair_ready;
static char s_service_name[16];
static char s_qr_payload[256];

typedef enum {
    PROVISION_WIFI_RESET_NONE = 0,
    PROVISION_WIFI_RESET_FAILED_ATTEMPT,
    PROVISION_WIFI_RESET_ABORT_ATTEMPT,
} provision_wifi_reset_kind_t;

typedef struct {
    bool got_ip;
    bool ble_connected;
    bool security_secured;
    bool app_claim_active;
    bool app_claim_pending;
    bool app_abort_pending;
    bool wifi_reset_pending;
    provision_wifi_reset_kind_t wifi_reset_kind;
    bool new_wifi_attempt_active;
    bool old_wifi_attempt_failed;
    bool old_wifi_got_ip;
    bool wifi_config_admitted;
    bool wifi_credentials_started;
    bool wifi_config_locked;
    bool wifi_marker_clear_pending;
    int64_t ble_connected_us;
    int64_t app_session_progress_us;
    int64_t runtime_activity_us;
    uint16_t ble_conn_handle;
    nuwax_provision_wifi_owner_t wifi_owner;
    uint32_t owner_generation;
    uint32_t wifi_attempt_owner_generation;
    uint32_t success_query_revision;
    uint32_t success_read_owner_generation;
    int64_t success_response_read_us;
    bool wifi_candidate_hash_valid;
    uint32_t wifi_candidate_hash;
    bool failed_attempt_release_pending;
    int64_t failed_attempt_release_deadline_us;
    bool has_wifi_disconnect_reason;
    uint8_t wifi_disconnect_reason;
    bool has_disconnect_memory_snapshot;
    size_t disconnect_internal_free;
    size_t disconnect_internal_largest;
    size_t disconnect_dma_free;
    size_t disconnect_dma_largest;
    int64_t credentials_received_us;
    nuwax_provision_outcome_t outcome;
    char reason[48];
} provision_state_t;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static provision_state_t s_state = {
    .outcome = NUWAX_PROVISION_OUTCOME_NONE,
};
static int64_t s_supervisor_heartbeat_us;

/* Never reuse response tags across reset_state()/BLE connection generations. */
static uint32_t s_success_query_sequence;
extern esp_err_t __real_network_prov_mgr_done(void);

esp_err_t __wrap_network_prov_mgr_done(void) {
    const esp_err_t err = __real_network_prov_mgr_done();
    if (err != ESP_OK) {
        return err;
    }
    /* The Wi-Fi manager calls done only for a CONNECTED status response.
     * The transport separately confirms that this exact response was read. */
    portENTER_CRITICAL(&s_state_lock);
    if (s_state.ble_connected && s_state.security_secured &&
        s_state.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_APP &&
        s_state.got_ip && !s_state.app_abort_pending) {
        if (++s_success_query_sequence == 0) {
            ++s_success_query_sequence;
        }
        s_state.success_query_revision = s_success_query_sequence;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return err;
}

uint32_t nuwax_provision_success_query_revision(void) {
    portENTER_CRITICAL(&s_state_lock);
    const uint32_t revision = s_state.success_query_revision;
    portEXIT_CRITICAL(&s_state_lock);
    return revision;
}

void nuwax_provision_success_response_read(uint32_t revision,
                                           uint16_t conn_handle) {
    portENTER_CRITICAL(&s_state_lock);
    if (revision != 0 && revision == s_state.success_query_revision &&
        s_state.ble_connected && s_state.security_secured &&
        conn_handle == s_state.ble_conn_handle &&
        s_state.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_APP &&
        s_state.got_ip && !s_state.app_abort_pending &&
        s_state.success_response_read_us == 0) {
        s_state.success_response_read_us = esp_timer_get_time();
        s_state.success_read_owner_generation = s_state.owner_generation;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

typedef struct {
    uint8_t *input;
    ssize_t input_len;
    uint8_t *output;
    ssize_t output_len;
    esp_err_t result;
    SemaphoreHandle_t done;
    portMUX_TYPE ref_lock;
    unsigned references;
} vox_config_job_t;

static QueueHandle_t s_vox_config_jobs;
static SemaphoreHandle_t s_vox_config_gate;
static bool s_accept_vox_config_jobs;
static nuwax_cloud_config_t s_staged_vox_config;
static bool s_has_staged_vox_config;
#define REPROVISION_NVS_NAMESPACE "nuwax_reprov"
#define REPROVISION_NVS_KEY "request"
#define REPROVISION_RECORD_MAGIC 0x4e585250U
#define REPROVISION_RECORD_VERSION 2U
#define REPROVISION_RECORD_FLAG_WIFI_VERIFIED (1U << 0)
#define REPROVISION_RECORD_FLAG_CLOUD_ACTIVE (1U << 1)
#define REPROVISION_RECORD_REQUIRED_FLAGS                                      \
    (REPROVISION_RECORD_FLAG_WIFI_VERIFIED |                                  \
     REPROVISION_RECORD_FLAG_CLOUD_ACTIVE)
#define WIFI_VERIFIED_RECORD_MAGIC 0x4e585658U
#define WIFI_VERIFIED_NVS_KEY "wifi_verified"
#define WIFI_COMMIT_RECORD_MAGIC 0x4e585643U
#define WIFI_COMMIT_NVS_KEY "commit_pending"
#define LOW_MEMORY_BOOT_NVS_KEY "lowmem_boot"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    wifi_config_t wifi_config;
} reprovision_record_t;

typedef struct {
    uint32_t magic;
    uint32_t credentials_hash;
} wifi_verified_record_t;

typedef struct {
    uint32_t magic;
    uint32_t credentials_hash;
} wifi_commit_record_t;

static atomic_bool s_force_reprovision = ATOMIC_VAR_INIT(false);
static reprovision_record_t s_reprovision_record;
/* True only when the rollback record has reached flash.  A volatile record is
 * still sufficient to recover a failed in-process rebind, but must never be
 * mistaken for crash-safe state after a reboot. */
static bool s_reprovision_record_durable;
static bool s_event_handlers_registered;
static bool s_manager_initialized;
static bool s_restart_on_unrecoverable = true;

static void provisioning_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data);
static void close_and_drain_vox_config_jobs(void);

/* Development-only credentials. Production builds must replace this path with
 * factory-injected salt/verifier and must never embed the PoP in firmware. */
#ifndef CONFIG_NUWAX_PROVISION_SEC2_DEV_POP
#define CONFIG_NUWAX_PROVISION_SEC2_DEV_POP "Nuwax7-Dev-PoP"
#endif

static char *s_sec2_salt;
static char *s_sec2_verifier;
static int s_sec2_salt_len;
static int s_sec2_verifier_len;
static network_prov_security2_params_t s_sec2_params;
#if !CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE
static char s_factory_salt[32];
static char s_factory_verifier[384];
#endif

static esp_err_t build_qr_payload(void) {
    const char *username = nuwax_device_uid_string();
    if (!username[0] || !s_service_name[0]) {
        s_qr_payload[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE
    const char *pop = CONFIG_NUWAX_PROVISION_SEC2_DEV_POP;
#else
    /* Production devices retain only the SRP salt/verifier. The plaintext PoP
     * remains on the controlled factory label and is never reconstructed here. */
    const char *pop = NULL;
#endif
    if (!nuwax_provision_qr_payload_build(
            s_qr_payload,
            sizeof(s_qr_payload),
            s_service_name,
            username,
            pop)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void set_reason_locked(const char *reason) {
    strlcpy(s_state.reason, reason ? reason : "-", sizeof(s_state.reason));
}

typedef struct {
    size_t internal_free;
    size_t internal_largest;
    size_t dma_free;
    size_t dma_largest;
} provision_memory_snapshot_t;

static provision_memory_snapshot_t provision_memory_snapshot(void) {
    return (provision_memory_snapshot_t) {
        .internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .internal_largest = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        .dma_largest = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
    };
}

static bool provision_memory_is_low(const provision_memory_snapshot_t *memory) {
    return memory && nuwax_provision_memory_below_reserve(
        memory->internal_free, memory->internal_largest, memory->dma_free,
        PROVISION_INTERNAL_FREE_MIN_BYTES,
        PROVISION_INTERNAL_LARGEST_MIN_BYTES,
        PROVISION_DMA_FREE_MIN_BYTES);
}

static bool provision_memory_is_low_for_start(
    const provision_memory_snapshot_t *memory, bool runtime_start) {
    if (!memory) {
        return true;
    }
    const size_t internal_free_min = runtime_start
        ? PROVISION_RUNTIME_INTERNAL_FREE_MIN_BYTES
        : PROVISION_INTERNAL_FREE_MIN_BYTES;
    return nuwax_provision_memory_below_reserve(
        memory->internal_free, memory->internal_largest, memory->dma_free,
        internal_free_min, PROVISION_INTERNAL_LARGEST_MIN_BYTES,
        PROVISION_DMA_FREE_MIN_BYTES);
}

static uint32_t wifi_credentials_hash(const wifi_config_t *config) {
    if (!config) {
        return 0;
    }
    const size_t ssid_length = strnlen((const char *)config->sta.ssid,
                                       sizeof(config->sta.ssid));
    const size_t password_length = strnlen((const char *)config->sta.password,
                                           sizeof(config->sta.password));
    return nuwax_provision_credentials_hash(
        config->sta.ssid, ssid_length,
        config->sta.password, password_length);
}

static esp_err_t clear_wifi_verified_marker(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, WIFI_VERIFIED_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t write_wifi_verified_marker(const wifi_config_t *config) {
    if (!config || config->sta.ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_verified_record_t record = {
        .magic = WIFI_VERIFIED_RECORD_MAGIC,
        .credentials_hash = wifi_credentials_hash(config),
    };
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open Wi-Fi verified marker failed");
    esp_err_t err = nvs_set_blob(nvs, WIFI_VERIFIED_NVS_KEY,
                                 &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static bool wifi_verified_marker_matches(const wifi_config_t *config) {
    if (!config || config->sta.ssid[0] == '\0') {
        return false;
    }
    wifi_verified_record_t record = {0};
    nvs_handle_t nvs;
    if (nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t length = sizeof(record);
    esp_err_t err = nvs_get_blob(nvs, WIFI_VERIFIED_NVS_KEY,
                                 &record, &length);
    nvs_close(nvs);
    return err == ESP_OK && length == sizeof(record) &&
           record.magic == WIFI_VERIFIED_RECORD_MAGIC &&
           record.credentials_hash == wifi_credentials_hash(config);
}

static bool wifi_verified_marker_exists(void) {
    wifi_verified_record_t record = {0};
    nvs_handle_t nvs;
    if (nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t length = sizeof(record);
    const esp_err_t err = nvs_get_blob(nvs, WIFI_VERIFIED_NVS_KEY,
                                       &record, &length);
    nvs_close(nvs);
    return err == ESP_OK && length == sizeof(record) &&
           record.magic == WIFI_VERIFIED_RECORD_MAGIC;
}

static esp_err_t read_low_memory_boot_request(bool *requested) {
    if (!requested) {
        return ESP_ERR_INVALID_ARG;
    }
    *requested = false;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    uint8_t value = 0U;
    err = nvs_get_u8(nvs, LOW_MEMORY_BOOT_NVS_KEY, &value);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_OK) {
        *requested = value == 1U;
    }
    return err;
}

static esp_err_t write_low_memory_boot_request(bool requested) {
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open low-memory boot request failed");
    esp_err_t err = requested
                        ? nvs_set_u8(nvs, LOW_MEMORY_BOOT_NVS_KEY, 1U)
                        : nvs_erase_key(nvs, LOW_MEMORY_BOOT_NVS_KEY);
    if (!requested && err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t nuwax_provision_main_request_low_memory_boot_profile(void) {
    const esp_err_t err = write_low_memory_boot_request(true);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "low-memory provisioning reboot requested");
    }
    return err;
}

static esp_err_t write_wifi_commit_marker(const wifi_config_t *config) {
    if (!config || config->sta.ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const wifi_commit_record_t record = {
        .magic = WIFI_COMMIT_RECORD_MAGIC,
        .credentials_hash = wifi_credentials_hash(config),
    };
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open Wi-Fi commit marker failed");
    esp_err_t err = nvs_set_blob(nvs, WIFI_COMMIT_NVS_KEY,
                                 &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static bool wifi_commit_marker_matches(const wifi_config_t *config) {
    if (!config || config->sta.ssid[0] == '\0') {
        return false;
    }
    wifi_commit_record_t record = {0};
    nvs_handle_t nvs;
    if (nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t length = sizeof(record);
    const esp_err_t err = nvs_get_blob(nvs, WIFI_COMMIT_NVS_KEY,
                                       &record, &length);
    nvs_close(nvs);
    return err == ESP_OK && length == sizeof(record) &&
           record.magic == WIFI_COMMIT_RECORD_MAGIC &&
           record.credentials_hash == wifi_credentials_hash(config);
}

static void unregister_provisioning_handlers(void) {
    if (!s_event_handlers_registered) {
        return;
    }
    (void)esp_event_handler_unregister(NETWORK_PROV_EVENT,
                                       ESP_EVENT_ANY_ID,
                                       provisioning_event_handler);
    (void)esp_event_handler_unregister(PROTOCOMM_SECURITY_SESSION_EVENT,
                                       ESP_EVENT_ANY_ID,
                                       provisioning_event_handler);
    (void)esp_event_handler_unregister(PROTOCOMM_TRANSPORT_BLE_EVENT,
                                       ESP_EVENT_ANY_ID,
                                       provisioning_event_handler);
    (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                       provisioning_event_handler);
    (void)esp_event_handler_unregister(WIFI_EVENT,
                                       WIFI_EVENT_STA_DISCONNECTED,
                                       provisioning_event_handler);
    s_event_handlers_registered = false;
}

static esp_err_t register_provisioning_handlers(void) {
    if (s_event_handlers_registered) {
        return ESP_OK;
    }
    esp_err_t err = esp_event_handler_register(NETWORK_PROV_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               provisioning_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT,
                                         ESP_EVENT_ANY_ID,
                                         provisioning_event_handler, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT,
                                         ESP_EVENT_ANY_ID,
                                         provisioning_event_handler, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         provisioning_event_handler, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT,
                                         WIFI_EVENT_STA_DISCONNECTED,
                                         provisioning_event_handler, NULL);
    }
    if (err != ESP_OK) {
        /* Unregistering an event which was not registered is harmless and keeps
         * every partial-registration failure on one cleanup path. */
        (void)esp_event_handler_unregister(NETWORK_PROV_EVENT,
                                           ESP_EVENT_ANY_ID,
                                           provisioning_event_handler);
        (void)esp_event_handler_unregister(PROTOCOMM_SECURITY_SESSION_EVENT,
                                           ESP_EVENT_ANY_ID,
                                           provisioning_event_handler);
        (void)esp_event_handler_unregister(PROTOCOMM_TRANSPORT_BLE_EVENT,
                                           ESP_EVENT_ANY_ID,
                                           provisioning_event_handler);
        (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                           provisioning_event_handler);
        (void)esp_event_handler_unregister(WIFI_EVENT,
                                           WIFI_EVENT_STA_DISCONNECTED,
                                           provisioning_event_handler);
        return err;
    }
    s_event_handlers_registered = true;
    return ESP_OK;
}

static provision_state_t get_state_snapshot(void) {
    provision_state_t snapshot;
    portENTER_CRITICAL(&s_state_lock);
    snapshot = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return snapshot;
}

bool nuwax_provision_main_is_active(void) {
    return atomic_load_explicit(&s_started, memory_order_acquire);
}

static bool force_reprovision_requested(void) {
    return atomic_load_explicit(&s_force_reprovision, memory_order_acquire);
}

static void set_force_reprovision(bool enabled) {
    atomic_store_explicit(&s_force_reprovision, enabled,
                          memory_order_release);
}

bool nuwax_provision_main_request_runtime_cancel(void) {
    int expected = PROVISION_CANCEL_AVAILABLE;
    if (atomic_compare_exchange_strong_explicit(
            &s_cancel_state, &expected, PROVISION_CANCEL_REQUESTED,
            memory_order_acq_rel, memory_order_acquire)) {
        ESP_LOGW(TAG,
                 "reversible provisioning cancel requested; supervisor will restore previous pair");
        return true;
    }
    if (expected == PROVISION_CANCEL_REQUESTED) {
        return true;
    }
    ESP_LOGW(TAG,
             "provisioning cancel rejected state=%d rollback=%d active=%d",
             expected, force_reprovision_requested() ? 1 : 0,
             nuwax_provision_main_is_active() ? 1 : 0);
    return false;
}

static bool provisioning_cancel_requested(void) {
    return atomic_load_explicit(&s_cancel_state, memory_order_acquire) ==
           PROVISION_CANCEL_REQUESTED;
}

static nuwax_provision_outcome_t finish_runtime_cancel(void) {
    portENTER_CRITICAL(&s_state_lock);
    s_state.outcome = NUWAX_PROVISION_OUTCOME_STOPPED;
    set_reason_locked("runtime_cancelled");
    portEXIT_CRITICAL(&s_state_lock);
    close_and_drain_vox_config_jobs();
    ESP_LOGW(TAG,
             "reversible provisioning cancelled before commit; returning to rollback owner");
    return NUWAX_PROVISION_OUTCOME_STOPPED;
}

static void note_supervisor_heartbeat(void) {
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    s_supervisor_heartbeat_us = now_us;
    portEXIT_CRITICAL(&s_state_lock);
}

int64_t nuwax_provision_main_supervisor_heartbeat_us(void) {
    int64_t heartbeat_us = 0;
    portENTER_CRITICAL(&s_state_lock);
    heartbeat_us = s_supervisor_heartbeat_us;
    portEXIT_CRITICAL(&s_state_lock);
    return heartbeat_us;
}

static void reset_provision_state(const char *reason) {
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    memset(&s_state, 0, sizeof(s_state));
    s_supervisor_heartbeat_us = 0;
    s_state.ble_conn_handle = UINT16_MAX;
    s_state.outcome = NUWAX_PROVISION_OUTCOME_NONE;
    s_state.runtime_activity_us = now_us;
    set_reason_locked(reason);
    portEXIT_CRITICAL(&s_state_lock);
}

static nuwax_provision_outcome_t map_managed_fail_reason(
    network_prov_wifi_sta_fail_reason_t reason) {
    if (reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) {
        return NUWAX_PROVISION_OUTCOME_AUTH_FAILED;
    }
    if (reason == NETWORK_PROV_WIFI_STA_AP_NOT_FOUND) {
        return NUWAX_PROVISION_OUTCOME_NETWORK_NOT_FOUND;
    }
    return NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
}

static nuwax_provision_outcome_t map_raw_wifi_fail_reason(
    uint8_t reason, const provision_state_t *state) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return NUWAX_PROVISION_OUTCOME_NETWORK_NOT_FOUND;
    case WIFI_REASON_AUTH_EXPIRE:
        if (state && state->has_disconnect_memory_snapshot) {
            const provision_memory_snapshot_t memory = {
                .internal_free = state->disconnect_internal_free,
                .internal_largest = state->disconnect_internal_largest,
                .dma_free = state->disconnect_dma_free,
                .dma_largest = state->disconnect_dma_largest,
            };
            if (provision_memory_is_low(&memory)) {
                return NUWAX_PROVISION_OUTCOME_RESOURCE_EXHAUSTED;
            }
        }
        return NUWAX_PROVISION_OUTCOME_AUTH_TIMEOUT;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return NUWAX_PROVISION_OUTCOME_AUTH_FAILED;
    default:
        return NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
    }
}

static const char *raw_wifi_reason_name(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND: return "no_ap_found";
    case WIFI_REASON_AUTH_EXPIRE: return "auth_expire";
    case WIFI_REASON_AUTH_FAIL: return "auth_fail";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "four_way_handshake_timeout";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake_timeout";
    case WIFI_REASON_MIC_FAILURE: return "mic_failure";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1x_auth_failed";
    default: return "unmapped";
    }
}

static void provisioning_manager_app_event_handler(void *user_data,
                                                   network_prov_cb_event_t event,
                                                   void *event_data) {
    (void)user_data;
    if (event != NETWORK_PROV_SET_WIFI_STA_CONFIG || !event_data) {
        return;
    }

    wifi_config_t *wifi_config = (wifi_config_t *)event_data;
    const provision_state_t state = get_state_snapshot();
    if (!nuwax_provision_app_session_active(
            state.ble_connected, state.app_claim_active,
            state.app_abort_pending, state.wifi_owner) ||
        state.app_claim_pending || !state.security_secured ||
        !state.wifi_config_admitted) {
        memset(wifi_config, 0, sizeof(*wifi_config));
        portENTER_CRITICAL(&s_state_lock);
        s_state.wifi_reset_pending = true;
        s_state.wifi_reset_kind = PROVISION_WIFI_RESET_ABORT_ATTEMPT;
        s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
        set_reason_locked("vox_config_required_before_wifi");
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGE(TAG,
                 "Wi-Fi credentials rejected: secured vox-config must complete first");
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_state.wifi_credentials_started = true;
    s_state.success_query_revision = 0;
    s_state.success_response_read_us = 0;
    s_state.success_read_owner_generation = 0;
    s_state.wifi_config_locked = true;
    s_state.wifi_marker_clear_pending = true;
    s_state.wifi_candidate_hash = wifi_credentials_hash(wifi_config);
    s_state.wifi_candidate_hash_valid = true;
    s_state.failed_attempt_release_pending = false;
    s_state.failed_attempt_release_deadline_us = 0;
    s_state.app_session_progress_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_lock);
    const size_t ssid_len = strnlen((const char *)wifi_config->sta.ssid,
                                    sizeof(wifi_config->sta.ssid));
    const size_t password_len = strnlen((const char *)wifi_config->sta.password,
                                        sizeof(wifi_config->sta.password));

    /* The device-list scan is completed before credentials are applied. Use
     * the same fast connection scan as the proven normal STA path here so a
     * transition-mode SSID with multiple BSSIDs is not re-selected by a second
     * full scan immediately before authentication. */
    wifi_config->sta.scan_method = WIFI_FAST_SCAN;
    wifi_config->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    unsigned wpa3_compat_disabled = 0;
#if CONFIG_ESP_WIFI_ENABLE_WPA3_SAE
    /* A zero-initialized IDF 6.0 wifi_config_t leaves SAE PWE unspecified.
     * Explicitly accept both WPA3 derivation methods for transition-mode APs. */
    wifi_config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#endif
#if defined(CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT) && \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    /* Field validation on the target AP showed that IDF's WPA3 compatibility
     * path can stall in 802.11 authentication.  A transition-mode AP still
     * accepts the proven WPA2 association path when this override is disabled. */
    wifi_config->sta.disable_wpa3_compatible_mode = 1;
    wpa3_compat_disabled = 1;
#endif
    ESP_LOGI(TAG,
             "STA provisioning policy applied ssid_len=%u password_len=%u scan=%d wpa3_compat_disabled=%u internal_free=%u internal_largest=%u",
             (unsigned)ssid_len, (unsigned)password_len,
             (int)wifi_config->sta.scan_method,
             wpa3_compat_disabled,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static const char *outcome_name(nuwax_provision_outcome_t outcome) {
    switch (outcome) {
    case NUWAX_PROVISION_OUTCOME_SUCCESS: return "success";
    case NUWAX_PROVISION_OUTCOME_AUTH_FAILED: return "auth_failed";
    case NUWAX_PROVISION_OUTCOME_NETWORK_NOT_FOUND: return "network_not_found";
    case NUWAX_PROVISION_OUTCOME_TIMEOUT: return "timeout";
    case NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR: return "internal_error";
    case NUWAX_PROVISION_OUTCOME_STOPPED: return "stopped";
    case NUWAX_PROVISION_OUTCOME_AUTH_TIMEOUT: return "auth_timeout";
    case NUWAX_PROVISION_OUTCOME_RESOURCE_EXHAUSTED: return "resource_exhausted";
    case NUWAX_PROVISION_OUTCOME_OLD_WIFI_RECOVERED: return "old_wifi_recovered";
    case NUWAX_PROVISION_OUTCOME_NONE:
    default: return "none";
    }
}

static void provisioning_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data) {
    (void)arg;
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            portENTER_CRITICAL(&s_state_lock);
            s_state.runtime_activity_us = esp_timer_get_time();
            set_reason_locked("started");
            portEXIT_CRITICAL(&s_state_lock);
            break;
        case NETWORK_PROV_WIFI_CRED_RECV:
            portENTER_CRITICAL(&s_state_lock);
            if (s_state.wifi_config_admitted &&
                s_state.wifi_credentials_started &&
                !s_state.app_claim_pending && s_state.security_secured &&
                nuwax_provision_app_session_active(
                    s_state.ble_connected, s_state.app_claim_active,
                    s_state.app_abort_pending, s_state.wifi_owner)) {
                s_state.credentials_received_us = esp_timer_get_time();
                s_state.runtime_activity_us = s_state.credentials_received_us;
                s_state.has_wifi_disconnect_reason = false;
                s_state.has_disconnect_memory_snapshot = false;
                s_state.got_ip = false;
                s_state.new_wifi_attempt_active = true;
                s_state.wifi_attempt_owner_generation =
                    s_state.owner_generation;
                s_state.failed_attempt_release_pending = false;
                s_state.failed_attempt_release_deadline_us = 0;
                s_state.wifi_config_locked = true;
                s_state.outcome = NUWAX_PROVISION_OUTCOME_NONE;
                set_reason_locked("credentials_received");
            } else {
                s_state.wifi_reset_pending = true;
                s_state.wifi_reset_kind = PROVISION_WIFI_RESET_ABORT_ATTEMPT;
                s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
                set_reason_locked("vox_config_required_before_wifi");
            }
            portEXIT_CRITICAL(&s_state_lock);
            break;
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            provision_state_t snapshot = get_state_snapshot();
            nuwax_provision_outcome_t outcome =
                NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
            char failure_reason[48] = "wifi_failure_missing_reason";
            if (snapshot.has_wifi_disconnect_reason) {
                outcome = map_raw_wifi_fail_reason(snapshot.wifi_disconnect_reason,
                                                   &snapshot);
                snprintf(failure_reason, sizeof(failure_reason), "wifi_reason_%u_%s",
                         (unsigned)snapshot.wifi_disconnect_reason,
                         raw_wifi_reason_name(snapshot.wifi_disconnect_reason));
            } else if (event_data) {
                const network_prov_wifi_sta_fail_reason_t managed_reason =
                    *(network_prov_wifi_sta_fail_reason_t *)event_data;
                outcome = map_managed_fail_reason(managed_reason);
                strlcpy(failure_reason, outcome_name(outcome),
                        sizeof(failure_reason));
            }
            portENTER_CRITICAL(&s_state_lock);
            s_state.outcome = outcome;
            s_state.runtime_activity_us = esp_timer_get_time();
            s_state.credentials_received_us = 0;
            s_state.got_ip = false;
            s_state.new_wifi_attempt_active = false;
            s_state.wifi_credentials_started = false;
            s_state.wifi_config_locked = true;
            s_state.wifi_reset_pending = true;
            s_state.wifi_reset_kind = PROVISION_WIFI_RESET_FAILED_ATTEMPT;
            s_state.failed_attempt_release_pending = true;
            s_state.failed_attempt_release_deadline_us =
                esp_timer_get_time() +
                (int64_t)CONFIG_NUWAX_PROVISION_FAILURE_STATUS_HOLD_MS * 1000;
            set_reason_locked(failure_reason);
            portEXIT_CRITICAL(&s_state_lock);
            /* The manager invokes this callback while holding its own lock.
             * Let the supervisor perform the reset after the callback returns. */
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            portENTER_CRITICAL(&s_state_lock);
            if (s_state.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_APP &&
                s_state.new_wifi_attempt_active &&
                nuwax_provision_attempt_belongs_to_owner(
                    s_state.wifi_attempt_owner_generation,
                    s_state.owner_generation)) {
                s_state.got_ip = true;
                s_state.runtime_activity_us = esp_timer_get_time();
                set_reason_locked("got_ip_pending_commit");
            }
            portEXIT_CRITICAL(&s_state_lock);
            break;
        case NETWORK_PROV_END:
            break;
        default:
            break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED &&
               event_data) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        const provision_memory_snapshot_t memory = provision_memory_snapshot();
        portENTER_CRITICAL(&s_state_lock);
        if (s_state.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_APP &&
            s_state.credentials_received_us != 0) {
            s_state.has_wifi_disconnect_reason = true;
            s_state.wifi_disconnect_reason = disconnected->reason;
            s_state.has_disconnect_memory_snapshot = true;
            s_state.disconnect_internal_free = memory.internal_free;
            s_state.disconnect_internal_largest = memory.internal_largest;
            s_state.disconnect_dma_free = memory.dma_free;
            s_state.disconnect_dma_largest = memory.dma_largest;
        }
        if (s_state.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY) {
            s_state.old_wifi_attempt_failed = true;
        }
        if (nuwax_provision_main_is_active() &&
            s_state.outcome != NUWAX_PROVISION_OUTCOME_SUCCESS &&
            s_state.outcome != NUWAX_PROVISION_OUTCOME_OLD_WIFI_RECOVERED) {
            s_state.got_ip = false;
            set_reason_locked("wifi_disconnected_before_commit");
        }
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGW(TAG,
                 "STA disconnected reason=%u(%s) internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
                 (unsigned)disconnected->reason,
                 raw_wifi_reason_name(disconnected->reason),
                 (unsigned)memory.internal_free,
                 (unsigned)memory.internal_largest,
                 (unsigned)memory.dma_free,
                 (unsigned)memory.dma_largest);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        portENTER_CRITICAL(&s_state_lock);
        if (s_state.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY &&
            nuwax_provision_attempt_belongs_to_owner(
                s_state.wifi_attempt_owner_generation,
                s_state.owner_generation)) {
            s_state.got_ip = true;
            s_state.old_wifi_got_ip = true;
            set_reason_locked("old_wifi_got_ip");
        }
        portEXIT_CRITICAL(&s_state_lock);
    } else if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        const protocomm_ble_event_t *ble_event =
            (const protocomm_ble_event_t *)event_data;
        bool connection_accepted = false;
        bool connection_rejected = false;
        bool current_connection_disconnected = false;
        uint16_t connection_handle = ble_event ? ble_event->conn_handle : UINT16_MAX;
        portENTER_CRITICAL(&s_state_lock);
        if (event_id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) {
            const bool provisioning_complete =
                s_state.outcome == NUWAX_PROVISION_OUTCOME_SUCCESS ||
                s_state.outcome == NUWAX_PROVISION_OUTCOME_OLD_WIFI_RECOVERED;
            if (nuwax_provision_main_is_active() &&
                nuwax_provision_app_can_accept_ble(
                    provisioning_complete,
                    s_state.old_wifi_got_ip,
                    s_state.wifi_reset_pending,
                    s_state.app_abort_pending,
                    s_state.wifi_owner)) {
                s_state.ble_connected = true;
                s_state.ble_connected_us = esp_timer_get_time();
                s_state.app_session_progress_us = s_state.ble_connected_us;
                s_state.runtime_activity_us = s_state.ble_connected_us;
                s_state.security_secured = false;
                s_state.app_claim_active = true;
                s_state.app_claim_pending = true;
                s_state.app_abort_pending = false;
                s_state.wifi_owner = NUWAX_PROVISION_WIFI_OWNER_APP;
                s_state.owner_generation++;
                s_state.success_query_revision = 0;
                s_state.success_response_read_us = 0;
                s_state.success_read_owner_generation = 0;
                s_state.new_wifi_attempt_active = false;
                s_state.wifi_attempt_owner_generation = 0;
                s_state.wifi_candidate_hash_valid = false;
                s_state.wifi_candidate_hash = 0;
                s_state.failed_attempt_release_pending = false;
                s_state.failed_attempt_release_deadline_us = 0;
                s_state.wifi_config_admitted = false;
                s_state.wifi_credentials_started = false;
                s_state.wifi_config_locked = false;
                set_reason_locked("app_ble_connected");
                if (ble_event) {
                    s_state.ble_conn_handle = ble_event->conn_handle;
                }
                connection_accepted = true;
            } else if (nuwax_provision_main_is_active() &&
                       !provisioning_complete) {
                connection_rejected = true;
            }
        } else if (event_id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
            const bool current_connection = !ble_event ||
                s_state.ble_conn_handle == UINT16_MAX ||
                ble_event->conn_handle == s_state.ble_conn_handle;
            if (current_connection) {
                connection_handle = s_state.ble_conn_handle;
                s_state.ble_connected = false;
                s_state.security_secured = false;
                s_state.ble_connected_us = 0;
                s_state.app_session_progress_us = 0;
                s_state.runtime_activity_us = esp_timer_get_time();
                s_state.ble_conn_handle = UINT16_MAX;
                if (!s_state.got_ip && s_state.app_claim_active) {
                    s_state.app_abort_pending = true;
                }
                current_connection_disconnected = true;
            }
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (connection_accepted) {
            ESP_LOGI(TAG, "BLE provisioning client connected handle=%u",
                     (unsigned)connection_handle);
        } else if (connection_rejected) {
            ESP_LOGW(TAG,
                     "BLE provisioning client rejected while previous session cleanup is pending handle=%u",
                     (unsigned)connection_handle);
            if (connection_handle != UINT16_MAX) {
                const int terminate_rc = ble_gap_terminate(connection_handle, 0x13);
                if (terminate_rc != 0) {
                    ESP_LOGW(TAG, "rejected BLE client terminate failed rc=%d",
                             terminate_rc);
                }
            }
        } else if (current_connection_disconnected) {
            ESP_LOGW(TAG,
                     "BLE provisioning client disconnected handle=%u; session reset queued",
                     (unsigned)connection_handle);
        }
    } else if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        bool security_established = false;
        portENTER_CRITICAL(&s_state_lock);
        const bool active_app_session = nuwax_provision_app_session_active(
            s_state.ble_connected, s_state.app_claim_active,
            s_state.app_abort_pending, s_state.wifi_owner);
        if (active_app_session &&
            event_id == PROTOCOMM_SECURITY_SESSION_SETUP_OK) {
            s_state.security_secured = true;
            s_state.app_claim_active = true;
            s_state.app_session_progress_us = esp_timer_get_time();
            s_state.runtime_activity_us = s_state.app_session_progress_us;
            set_reason_locked("app_security_secured");
            security_established = true;
        } else if (active_app_session &&
                   event_id == PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH) {
            s_state.outcome = NUWAX_PROVISION_OUTCOME_AUTH_FAILED;
            s_state.runtime_activity_us = esp_timer_get_time();
            s_state.app_abort_pending = true;
            set_reason_locked("security_credentials_mismatch");
        } else if (active_app_session &&
                   event_id == PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS) {
            s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
            s_state.runtime_activity_us = esp_timer_get_time();
            s_state.app_abort_pending = true;
            set_reason_locked("invalid_security_params");
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (security_established) {
            ESP_LOGI(TAG, "BLE provisioning Security 2 session established");
        }
    }
}

static esp_err_t device_info_handler(uint32_t session_id, const uint8_t *inbuf,
                                     ssize_t inlen, uint8_t **outbuf,
                                     ssize_t *outlen, void *priv_data) {
    (void)session_id;
    (void)priv_data;
    if (!inbuf || inlen <= 0 || !outbuf || !outlen) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *request = cJSON_ParseWithLength((const char *)inbuf, (size_t)inlen);
    if (!cJSON_IsObject(request)) {
        cJSON_Delete(request);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(request);

    portENTER_CRITICAL(&s_state_lock);
    if (s_state.ble_connected && s_state.security_secured &&
        s_state.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_APP) {
        s_state.app_session_progress_us = esp_timer_get_time();
        s_state.runtime_activity_us = s_state.app_session_progress_us;
    }
    portEXIT_CRITICAL(&s_state_lock);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    const char *firmware_version = app && app->version[0] ? app->version : "unknown";
    bool json_ok = cJSON_AddStringToObject(root, "protocolVersion", "1.0") != NULL &&
                   cJSON_AddStringToObject(root, "serialNumber",
                                           nuwax_device_uid_string()) != NULL &&
                   cJSON_AddStringToObject(root, "productModel",
                                           "Nuwax AI Partner") != NULL &&
                   cJSON_AddStringToObject(root, "hardwareVersion",
                                           nuwax_board_name()) != NULL &&
                   cJSON_AddStringToObject(root, "firmwareVersion",
                                           firmware_version) != NULL;
    cJSON *caps = cJSON_AddArrayToObject(root, "capabilities");
    cJSON *wifi_prov = cJSON_CreateString("wifi_prov");
    cJSON *wifi_scan = cJSON_CreateString("wifi_scan");
    cJSON *cloud_prov = cJSON_CreateString("cloud_prov");
    if (!json_ok || !caps || !wifi_prov || !wifi_scan || !cloud_prov) {
        cJSON_Delete(wifi_prov);
        cJSON_Delete(wifi_scan);
        cJSON_Delete(cloud_prov);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToArray(caps, wifi_prov)) {
        cJSON_Delete(wifi_prov);
        cJSON_Delete(wifi_scan);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToArray(caps, wifi_scan)) {
        cJSON_Delete(wifi_scan);
        cJSON_Delete(cloud_prov);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToArray(caps, cloud_prov)) {
        cJSON_Delete(cloud_prov);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    const provision_state_t snapshot = get_state_snapshot();
    json_ok = cJSON_AddStringToObject(root, "lastProvisioningOutcome",
                                      outcome_name(snapshot.outcome)) != NULL &&
              cJSON_AddStringToObject(root, "lastProvisioningReason",
                                      snapshot.reason[0] ? snapshot.reason : "-") != NULL &&
              cJSON_AddStringToObject(
                  root, "cloudConfigState",
                  nuwax_cloud_config_has_pending()
                      ? "pending"
                      : (nuwax_cloud_config_has_config() ? "active" : "missing")) != NULL;
    if (json_ok) {
        if (snapshot.has_wifi_disconnect_reason) {
            json_ok = cJSON_AddNumberToObject(root, "wifiDisconnectReason",
                                              snapshot.wifi_disconnect_reason) != NULL &&
                      cJSON_AddStringToObject(
                          root, "wifiDisconnectReasonName",
                          raw_wifi_reason_name(snapshot.wifi_disconnect_reason)) != NULL;
        } else {
            json_ok = cJSON_AddNullToObject(root, "wifiDisconnectReason") != NULL &&
                      cJSON_AddNullToObject(root, "wifiDisconnectReasonName") != NULL;
        }
    }
    if (!json_ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    *outbuf = (uint8_t *)json;
    *outlen = (ssize_t)strlen(json);
    return ESP_OK;
}

static bool copy_json_string(cJSON *root, const char *name,
                             char *destination, size_t capacity) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
        return false;
    }
    const size_t length = strlen(item->valuestring);
    if (length >= capacity || memchr(item->valuestring, '\0', length) != NULL) {
        return false;
    }
    memcpy(destination, item->valuestring, length + 1);
    return true;
}

static esp_err_t vox_config_response(bool ok, const char *error,
                                     const nuwax_cloud_config_t *config,
                                     uint8_t **outbuf, ssize_t *outlen) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    bool json_ok = cJSON_AddBoolToObject(root, "ok", ok) != NULL;
    if (ok && config) {
        /* Keep the response compatible with the deployed APP. Internally the
         * candidate remains pending until the new Wi-Fi reaches GOT_IP. */
        json_ok = json_ok &&
                  cJSON_AddStringToObject(root, "state", "active") != NULL &&
                  cJSON_AddStringToObject(root, "deviceId",
                                          config->device_id) != NULL;
    } else {
        json_ok = json_ok &&
                  cJSON_AddStringToObject(root, "error",
                                          error ? error : "VOX_CONFIG_INVALID") != NULL;
    }
    if (!json_ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    *outbuf = (uint8_t *)json;
    *outlen = (ssize_t)strlen(json);
    return ESP_OK;
}

static esp_err_t vox_config_process(const uint8_t *inbuf, ssize_t inlen,
                                    uint8_t **outbuf, ssize_t *outlen) {
    if (!inbuf || inlen <= 0 || !outbuf || !outlen) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *request = cJSON_ParseWithLength((const char *)inbuf, (size_t)inlen);
    if (!cJSON_IsObject(request)) {
        cJSON_Delete(request);
        return vox_config_response(false, "VOX_CONFIG_INVALID", NULL,
                                   outbuf, outlen);
    }

    nuwax_cloud_config_t config = {0};
    const bool parsed = copy_json_string(request, "deviceId", config.device_id,
                                         sizeof(config.device_id)) &&
                        copy_json_string(request, "gatewayUrl", config.cloud_url,
                                         sizeof(config.cloud_url)) &&
                        copy_json_string(request, "deviceSecret", config.device_secret,
                                         sizeof(config.device_secret));
    if (parsed) {
        config.version = 1;
    }
    cJSON_Delete(request);
    if (!parsed) {
        return vox_config_response(false, "VOX_CONFIG_INVALID", NULL,
                                   outbuf, outlen);
    }
    if (strcmp(config.device_id, nuwax_device_uid_string()) != 0) {
        return vox_config_response(false, "DEVICE_ID_MISMATCH", NULL,
                                   outbuf, outlen);
    }

    const provision_state_t state = get_state_snapshot();
    if (!state.security_secured ||
        state.wifi_owner != NUWAX_PROVISION_WIFI_OWNER_APP) {
        return vox_config_response(false, "VOX_CONFIG_SESSION_REQUIRED", NULL,
                                   outbuf, outlen);
    }
    if (state.wifi_config_locked) {
        const bool identical = s_has_staged_vox_config &&
            memcmp(&config, &s_staged_vox_config, sizeof(config)) == 0;
        if (identical) {
            portENTER_CRITICAL(&s_state_lock);
            s_state.app_session_progress_us = esp_timer_get_time();
            s_state.runtime_activity_us = s_state.app_session_progress_us;
            portEXIT_CRITICAL(&s_state_lock);
        }
        return vox_config_response(identical, identical ? NULL : "VOX_CONFIG_LOCKED",
                                   identical ? &config : NULL, outbuf, outlen);
    }

    const esp_err_t err = nuwax_cloud_config_stage(&config);
    if (err != ESP_OK) {
        const char *code = err == ESP_ERR_INVALID_ARG
                               ? "VOX_CONFIG_INVALID"
                               : "VOX_CONFIG_PERSIST_FAILED";
        ESP_LOGE(TAG, "vox-config rejected: %s", esp_err_to_name(err));
        return vox_config_response(false, code, NULL, outbuf, outlen);
    }
    s_staged_vox_config = config;
    s_has_staged_vox_config = true;
    portENTER_CRITICAL(&s_state_lock);
    s_state.wifi_config_admitted = true;
    s_state.app_session_progress_us = esp_timer_get_time();
    s_state.runtime_activity_us = s_state.app_session_progress_us;
    set_reason_locked("vox_config_staged");
    portEXIT_CRITICAL(&s_state_lock);
    return vox_config_response(true, NULL, &config, outbuf, outlen);
}

static void vox_config_job_release(vox_config_job_t *job) {
    if (!job) {
        return;
    }
    bool destroy = false;
    portENTER_CRITICAL(&job->ref_lock);
    if (job->references > 0U) {
        job->references--;
        destroy = job->references == 0U;
    }
    portEXIT_CRITICAL(&job->ref_lock);
    if (!destroy) {
        return;
    }
    free(job->input);
    free(job->output);
    if (job->done) {
        vSemaphoreDelete(job->done);
    }
    free(job);
}

static esp_err_t ensure_vox_config_queue(void) {
    if (s_vox_config_jobs && s_vox_config_gate) {
        return ESP_OK;
    }
    if (!s_vox_config_jobs) {
        s_vox_config_jobs = xQueueCreateWithCaps(
            1, sizeof(vox_config_job_t *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_vox_config_jobs) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_vox_config_gate) {
        s_vox_config_gate = xSemaphoreCreateMutex();
    }
    if (!s_vox_config_gate) {
        /* xQueueCreateWithCaps() owns both the queue control block and its
         * storage; use the matching deleter on every partial-init path. */
        vQueueDeleteWithCaps(s_vox_config_jobs);
        s_vox_config_jobs = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "vox-config queue ready; NVS write runs on internal main task");
    return ESP_OK;
}

static void process_vox_config_job(TickType_t wait_ticks) {
    if (get_state_snapshot().app_claim_pending) {
        return;
    }
    vox_config_job_t *job = NULL;
    if (!s_vox_config_jobs ||
        xQueueReceive(s_vox_config_jobs, &job, wait_ticks) != pdTRUE || !job) {
        return;
    }
    job->result = vox_config_process(job->input, job->input_len,
                                     &job->output, &job->output_len);
    xSemaphoreGive(job->done);
    vox_config_job_release(job);
}

static void set_vox_config_accepting(bool accepting) {
    if (!s_vox_config_gate) {
        s_accept_vox_config_jobs = false;
        return;
    }
    if (xSemaphoreTake(s_vox_config_gate, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_accept_vox_config_jobs = accepting;
        xSemaphoreGive(s_vox_config_gate);
    }
}

static void close_and_drain_vox_config_jobs(void) {
    set_vox_config_accepting(false);
    vox_config_job_t *job = NULL;
    while (s_vox_config_jobs &&
           xQueueReceive(s_vox_config_jobs, &job, 0) == pdTRUE) {
        if (!job) {
            continue;
        }
        job->result = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(job->done);
        vox_config_job_release(job);
        job = NULL;
    }
}

static esp_err_t vox_config_handler(uint32_t session_id, const uint8_t *inbuf,
                                    ssize_t inlen, uint8_t **outbuf,
                                    ssize_t *outlen, void *priv_data) {
    (void)session_id;
    (void)priv_data;
    if (!inbuf || inlen <= 0 || !outbuf || !outlen || !s_vox_config_jobs ||
        !s_vox_config_gate) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The per-job spinlock must remain in internal memory while flash/NVS
     * operations temporarily make external RAM inaccessible. */
    vox_config_job_t *job = heap_caps_calloc(
        1, sizeof(*job), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    job->input = heap_caps_malloc((size_t)inlen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    job->done = xSemaphoreCreateBinary();
    if (!job->input || !job->done) {
        free(job->input);
        if (job->done) {
            vSemaphoreDelete(job->done);
        }
        free(job);
        return ESP_ERR_NO_MEM;
    }
    memcpy(job->input, inbuf, (size_t)inlen);
    job->input_len = inlen;
    portMUX_INITIALIZE(&job->ref_lock);
    job->references = 2U;

    if (xSemaphoreTake(s_vox_config_gate, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(job->input);
        vSemaphoreDelete(job->done);
        free(job);
        return ESP_ERR_TIMEOUT;
    }
    const bool accepting = s_accept_vox_config_jobs;
    const BaseType_t queued = accepting
                                  ? xQueueSend(s_vox_config_jobs, &job, 0)
                                  : pdFALSE;
    xSemaphoreGive(s_vox_config_gate);
    if (queued != pdTRUE) {
        job->references = 1U;
        vox_config_job_release(job);
        return accepting ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(job->done,
                       pdMS_TO_TICKS(CONFIG_NUWAX_PROVISION_VOX_CONFIG_JOB_TIMEOUT_MS)) !=
        pdTRUE) {
        ESP_LOGE(TAG, "vox-config job timed out after %u ms",
                 (unsigned)CONFIG_NUWAX_PROVISION_VOX_CONFIG_JOB_TIMEOUT_MS);
        vox_config_job_release(job);
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result = job->result;
    *outbuf = job->output;
    *outlen = job->output_len;
    job->output = NULL;
    vox_config_job_release(job);
    return result;
}

static esp_err_t prepare_security2(void) {
    const char *username = nuwax_device_uid_string();
#if CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE
    const char *pop = CONFIG_NUWAX_PROVISION_SEC2_DEV_POP;
    if (!username[0] || !pop[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sec2_salt) {
        free(s_sec2_salt);
        s_sec2_salt = NULL;
    }
    if (s_sec2_verifier) {
        free(s_sec2_verifier);
        s_sec2_verifier = NULL;
    }
    esp_err_t err = esp_srp_gen_salt_verifier(username, (int)strlen(username),
                                               pop, (int)strlen(pop),
                                               &s_sec2_salt, 16,
                                               &s_sec2_verifier,
                                               &s_sec2_verifier_len);
    if (err != ESP_OK) {
        return err;
    }
    s_sec2_salt_len = 16;
    s_sec2_params.salt = s_sec2_salt;
    s_sec2_params.salt_len = (uint16_t)s_sec2_salt_len;
    s_sec2_params.verifier = s_sec2_verifier;
    s_sec2_params.verifier_len = (uint16_t)s_sec2_verifier_len;
    ESP_RETURN_ON_ERROR(build_qr_payload(), TAG, "build provisioning QR failed");
    return ESP_OK;
#else
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("nuwax_factory", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    size_t salt_len = sizeof(s_factory_salt);
    size_t verifier_len = sizeof(s_factory_verifier);
    err = nvs_get_blob(nvs, "sec2_salt", s_factory_salt, &salt_len);
    if (err == ESP_OK) {
        err = nvs_get_blob(nvs, "sec2_verifier", s_factory_verifier,
                           &verifier_len);
    }
    nvs_close(nvs);
    if (err != ESP_OK || salt_len == 0 || verifier_len == 0) {
        ESP_LOGE(TAG, "factory Security 2 salt/verifier missing");
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    s_sec2_params.salt = s_factory_salt;
    s_sec2_params.salt_len = (uint16_t)salt_len;
    s_sec2_params.verifier = s_factory_verifier;
    s_sec2_params.verifier_len = (uint16_t)verifier_len;
    build_qr_payload();
    return ESP_OK;
#endif
}

static esp_err_t clear_reprovision_record(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, REPROVISION_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, WIFI_COMMIT_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK || !s_reprovision_record_durable) {
        memset(&s_reprovision_record, 0, sizeof(s_reprovision_record));
        s_reprovision_record_durable = false;
        set_force_reprovision(false);
    }
    return err;
}

static bool reprovision_record_can_restore(
    const reprovision_record_t *record) {
    return record && record->magic == REPROVISION_RECORD_MAGIC &&
           nuwax_provision_rollback_record_can_restore(
               record->version, REPROVISION_RECORD_VERSION,
               record->flags, REPROVISION_RECORD_REQUIRED_FLAGS,
               record->wifi_config.sta.ssid[0] != '\0');
}

static esp_err_t save_reprovision_record(void) {
    wifi_config_t wifi_config = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &wifi_config), TAG,
                        "read current Wi-Fi config for reversible reprovision failed");
    if (wifi_config.sta.ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    if (!nuwax_cloud_config_has_config()) {
        ESP_LOGE(TAG,
                 "refusing rollback record because cloud config is missing");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_verified_marker_matches(&wifi_config)) {
        wifi_ap_record_t ap = {0};
        if (!nuwax_wifi_is_connected() ||
            esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
            strncmp((const char *)ap.ssid,
                    (const char *)wifi_config.sta.ssid,
                    sizeof(ap.ssid)) != 0) {
            ESP_LOGE(TAG,
                     "refusing rollback record for Wi-Fi that is not currently verified");
            return ESP_ERR_INVALID_STATE;
        }
        ESP_RETURN_ON_ERROR(write_wifi_verified_marker(&wifi_config), TAG,
                            "repair Wi-Fi verification before reprovision failed");
        ESP_LOGW(TAG,
                 "repaired missing Wi-Fi verification marker from active connection");
    }
    reprovision_record_t record = {
        .magic = REPROVISION_RECORD_MAGIC,
        .version = REPROVISION_RECORD_VERSION,
        .flags = REPROVISION_RECORD_REQUIRED_FLAGS,
        .wifi_config = wifi_config,
    };
    /* Keep a RAM copy before touching NVS.  If the diagnostic queue or an old
     * stale key has exhausted NVS, the current boot can still complete a
     * bounded rebind and roll back safely without losing the old Wi-Fi. */
    s_reprovision_record = record;
    s_reprovision_record_durable = false;
    set_force_reprovision(true);
    nvs_handle_t nvs;
    esp_err_t open_err = nvs_open(REPROVISION_NVS_NAMESPACE,
                                  NVS_READWRITE,
                                  &nvs);
    if (open_err != ESP_OK) {
        if (open_err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
            ESP_LOGW(TAG,
                     "reprovision rollback record is volatile for this boot; NVS is full");
        } else {
            memset(&s_reprovision_record, 0, sizeof(s_reprovision_record));
            set_force_reprovision(false);
        }
        return open_err;
    }
    esp_err_t err = nvs_set_blob(nvs, REPROVISION_NVS_KEY, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, WIFI_COMMIT_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_reprovision_record_durable = true;
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        ESP_LOGW(TAG,
                 "reprovision rollback record is volatile for this boot; NVS is full");
        return err;
    }
    memset(&s_reprovision_record, 0, sizeof(s_reprovision_record));
    set_force_reprovision(false);
    return err;
}

void nuwax_provision_main_discard_volatile_rollback(void) {
    if (s_reprovision_record_durable) return;
    memset(&s_reprovision_record, 0, sizeof(s_reprovision_record));
    set_force_reprovision(false);
    atomic_store_explicit(&s_cancel_state, PROVISION_CANCEL_DISABLED,
                          memory_order_release);
    ESP_LOGW(TAG,
             "runtime reprovision deferred until rollback can be persisted; current Wi-Fi retained");
}

static esp_err_t load_reprovision_record(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(REPROVISION_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open reprovision NVS failed");
    reprovision_record_t record = {0};
    size_t length = sizeof(record);
    err = nvs_get_blob(nvs, REPROVISION_NVS_KEY, &record, &length);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK || length != sizeof(record) ||
        !reprovision_record_can_restore(&record)) {
        ESP_LOGW(TAG, "discarding invalid reversible reprovision record");
        return clear_reprovision_record();
    }
    s_reprovision_record = record;
    s_reprovision_record_durable = true;
    set_force_reprovision(true);
    return ESP_OK;
}

bool nuwax_provision_main_requires_low_memory_boot_profile(void) {
    const esp_err_t reprovision_err = load_reprovision_record();
    const bool cloud_ready = nuwax_cloud_config_init() == ESP_OK &&
                             nuwax_cloud_config_has_config();
    const bool marker_present = wifi_verified_marker_exists();
    const bool app_wifi_present = nuwax_wifi_has_saved_credentials();
    bool low_memory_recovery_requested = false;
    const esp_err_t recovery_err =
        read_low_memory_boot_request(&low_memory_recovery_requested);
    bool marker_matches = marker_present;

    /* This succeeds only when a caller has already initialized Wi-Fi. Before
     * that point, marker presence is the strongest safe boot-time signal. */
    wifi_config_t driver_config = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &driver_config) == ESP_OK &&
        driver_config.sta.ssid[0] != '\0') {
        marker_matches = wifi_verified_marker_matches(&driver_config);
    }

    const bool required = nuwax_provision_boot_profile_required(
        reprovision_err == ESP_OK, force_reprovision_requested(), cloud_ready,
        marker_present, marker_matches, app_wifi_present,
        recovery_err != ESP_OK || low_memory_recovery_requested);
    ESP_LOGI(TAG,
             "provisioning boot memory profile=%d recovery=%d cloud=%d marker=%d match=%d app_wifi=%d lowmem_recovery=%d",
             required, force_reprovision_requested(), cloud_ready, marker_present,
             marker_matches, app_wifi_present,
             recovery_err != ESP_OK || low_memory_recovery_requested);
    return required;
}

esp_err_t nuwax_provision_main_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }
    s_runtime_start = false;
    esp_err_t err = nuwax_device_uid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uid init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nuwax_cloud_config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cloud config init failed: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t sta_mac[6] = {0};
    err = esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read STA MAC failed: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(s_service_name, sizeof(s_service_name), "PROV_%02X%02X%02X",
             sta_mac[3], sta_mac[4], sta_mac[5]);
    err = build_qr_payload();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "build provisioning QR payload failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = register_provisioning_handlers();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register provisioning events failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    const char *caps[] = {"wifi_prov", "wifi_scan", "cloud_prov"};
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        /* Runtime reprovisioning reinitializes this manager after normal boot.
         * FREE_BLE calls esp_bt_mem_release(), which is irreversible until a
         * chip reset and was the hidden reason the old path had to reboot. */
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = {
            .event_cb = provisioning_manager_app_event_handler,
            .user_data = NULL,
        },
        .network_prov_wifi_conn_cfg = {.wifi_conn_attempts = 3},
    };
    err = network_prov_mgr_init(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "manager init failed: %s", esp_err_to_name(err));
        goto fail;
    }
    s_manager_initialized = true;
    err = network_prov_mgr_set_app_info("nuwax", "7.0.0", caps, 3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set app info failed: %s", esp_err_to_name(err));
        goto fail;
    }

    static uint8_t service_uuid[16] = {
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    };
    err = network_prov_scheme_ble_set_service_uuid(service_uuid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set service uuid failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = network_prov_mgr_endpoint_create(DEVICE_INFO_EP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create device-info endpoint failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }
    err = network_prov_mgr_endpoint_create(VOX_CONFIG_EP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create vox-config endpoint failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }

    bool provisioned = false;
    err = network_prov_mgr_is_wifi_provisioned(&provisioned);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read provisioned state failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = load_reprovision_record();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load reversible reprovision request failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }

    wifi_config_t driver_config = {0};
    err = esp_wifi_get_config(WIFI_IF_STA, &driver_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read provisioned Wi-Fi config failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }
    const bool has_driver_credentials = provisioned &&
                                        driver_config.sta.ssid[0] != '\0';
    bool verified = has_driver_credentials &&
                    wifi_verified_marker_matches(&driver_config);
    char driver_ssid[sizeof(driver_config.sta.ssid) + 1] = {0};
    char driver_password[sizeof(driver_config.sta.password) + 1] = {0};
    memcpy(driver_ssid, driver_config.sta.ssid,
           sizeof(driver_config.sta.ssid));
    memcpy(driver_password, driver_config.sta.password,
           sizeof(driver_config.sta.password));
    if (has_driver_credentials && force_reprovision_requested() &&
        wifi_commit_marker_matches(&driver_config) &&
        !nuwax_cloud_config_has_pending() &&
        nuwax_cloud_config_has_config() &&
        nuwax_wifi_saved_credentials_match(driver_ssid, driver_password)) {
        /* Power may fail after the new cloud/Wi-Fi pair commits but before the
         * ordinary success cleanup. A durable candidate marker plus an erased
         * cloud pending slot proves that rollback would now mix generations. */
        err = write_wifi_verified_marker(&driver_config);
        if (err == ESP_OK) {
            err = clear_reprovision_record();
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "recover committed provisioning transaction failed: %s",
                     esp_err_to_name(err));
            goto fail;
        }
        verified = true;
        ESP_LOGW(TAG, "completed interrupted provisioning transaction");
    }
    if (has_driver_credentials && !verified && !force_reprovision_requested() &&
        nuwax_cloud_config_has_config() &&
        nuwax_wifi_saved_credentials_match(driver_ssid, driver_password)) {
        /* One-time migration for units provisioned before the explicit marker
         * existed. Both legacy stores must agree that setup completed. */
        err = write_wifi_verified_marker(&driver_config);
        if (err == ESP_OK) {
            verified = true;
            ESP_LOGI(TAG, "migrated legacy provisioned Wi-Fi verification marker");
        } else {
            ESP_LOGE(TAG, "legacy Wi-Fi marker migration failed: %s",
                     esp_err_to_name(err));
            goto fail;
        }
    }
    if (has_driver_credentials && verified &&
        !nuwax_wifi_saved_credentials_match(driver_ssid, driver_password)) {
        err = nuwax_wifi_import_driver_credentials();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "synchronize verified Wi-Fi credentials failed: %s",
                     esp_err_to_name(err));
            goto fail;
        }
        ESP_LOGI(TAG, "synchronized verified driver Wi-Fi credentials to application storage");
    }

    const bool cloud_ready = nuwax_cloud_config_has_config();
    s_provisioned_pair_ready = nuwax_provision_pair_ready(
        has_driver_credentials, verified, cloud_ready);
    if (has_driver_credentials && !force_reprovision_requested() &&
        (!verified || !cloud_ready)) {
        ESP_LOGW(TAG,
                 "Wi-Fi configuration is incomplete verified=%d cloud=%d; candidate will be cleared before BLE provisioning",
                 verified, cloud_ready);
    }
    atomic_store_explicit(
        &s_cancel_state,
        force_reprovision_requested() &&
                reprovision_record_can_restore(&s_reprovision_record)
            ? PROVISION_CANCEL_AVAILABLE
            : PROVISION_CANCEL_DISABLED,
        memory_order_release);
    s_initialized = true;
    return ESP_OK;

fail:
    if (s_manager_initialized) {
        (void)network_prov_mgr_deinit();
        s_manager_initialized = false;
    }
    unregister_provisioning_handlers();
    return err;
}

bool nuwax_provision_main_should_auto_enter(void) {
    return s_initialized &&
           (!s_provisioned_pair_ready || force_reprovision_requested());
}

bool nuwax_provision_main_has_complete_pair(void) {
    return s_initialized && s_provisioned_pair_ready;
}

static esp_err_t retry_persist_operation(esp_err_t (*operation)(void),
                                         const char *name) {
    esp_err_t err = ESP_ERR_INVALID_ARG;
    for (int attempt = 1; attempt <= PERSIST_RETRY_COUNT; ++attempt) {
        err = operation();
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "%s attempt %d/%d failed: %s", name, attempt,
                 PERSIST_RETRY_COUNT, esp_err_to_name(err));
        if (attempt < PERSIST_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(PERSIST_RETRY_DELAY_MS));
        }
    }
    return err;
}

static esp_err_t retry_write_wifi_verified_marker(const wifi_config_t *config) {
    esp_err_t err = ESP_ERR_INVALID_ARG;
    for (int attempt = 1; attempt <= PERSIST_RETRY_COUNT; ++attempt) {
        err = write_wifi_verified_marker(config);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "write Wi-Fi verified marker attempt %d/%d failed: %s",
                 attempt, PERSIST_RETRY_COUNT, esp_err_to_name(err));
        if (attempt < PERSIST_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(PERSIST_RETRY_DELAY_MS));
        }
    }
    return err;
}

static esp_err_t restore_flash_credentials_after_candidate(void) {
    wifi_config_t restore_config = {0};
    const bool restore_old = force_reprovision_requested() &&
                             reprovision_record_can_restore(
                                 &s_reprovision_record);
    if (restore_old) {
        restore_config = s_reprovision_record.wifi_config;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG,
                        "select Flash while sanitizing Wi-Fi candidate failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &restore_config), TAG,
                        "restore verified Wi-Fi after candidate failed");
    ESP_LOGI(TAG, "%s Wi-Fi candidate in Flash",
             restore_old ? "restored verified rollback" : "cleared unverified");
    return ESP_OK;
}

static void clear_wifi_owner_locked(const char *reason) {
    s_state.wifi_owner = NUWAX_PROVISION_WIFI_OWNER_NONE;
    s_state.owner_generation++;
    s_state.app_claim_active = false;
    s_state.app_claim_pending = false;
    s_state.app_abort_pending = false;
    s_state.security_secured = false;
    s_state.ble_connected = false;
    s_state.ble_connected_us = 0;
    s_state.app_session_progress_us = 0;
    s_state.ble_conn_handle = UINT16_MAX;
    s_state.wifi_attempt_owner_generation = 0;
    s_state.wifi_candidate_hash_valid = false;
    s_state.wifi_candidate_hash = 0;
    s_state.failed_attempt_release_pending = false;
    s_state.failed_attempt_release_deadline_us = 0;
    s_state.got_ip = false;
    s_state.old_wifi_got_ip = false;
    s_state.old_wifi_attempt_failed = false;
    s_state.new_wifi_attempt_active = false;
    s_state.credentials_received_us = 0;
    s_state.wifi_config_admitted = false;
    s_state.wifi_credentials_started = false;
    s_state.wifi_config_locked = false;
    s_state.wifi_reset_pending = false;
    s_state.wifi_reset_kind = PROVISION_WIFI_RESET_NONE;
    s_state.wifi_marker_clear_pending = false;
    if (reason) {
        set_reason_locked(reason);
    }
}

static void set_owner_none(const char *reason) {
    portENTER_CRITICAL(&s_state_lock);
    clear_wifi_owner_locked(reason);
    portEXIT_CRITICAL(&s_state_lock);
}

static bool release_old_wifi_owner(uint32_t generation, const char *reason) {
    bool released = false;
    portENTER_CRITICAL(&s_state_lock);
    if (nuwax_provision_old_owner_matches(
            s_state.wifi_owner, s_state.owner_generation, generation)) {
        clear_wifi_owner_locked(reason);
        released = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return released;
}

static esp_err_t reset_manager_wifi_state(provision_wifi_reset_kind_t kind,
                                          const char *reason) {
    const bool failed_attempt = kind == PROVISION_WIFI_RESET_FAILED_ATTEMPT;
    esp_err_t reset_err = failed_attempt
                              ? network_prov_mgr_reset_wifi_sm_state_on_failure()
                              : network_prov_mgr_reset_wifi_sm_state_for_reprovision();
    if (reset_err == ESP_OK) {
        return ESP_OK;
    }
    /* Only a state mismatch may select the generic abort reset. A driver or
     * storage error must stay pending; the alternate STARTED fast path could
     * otherwise falsely acknowledge a partially completed reset. */
    if (!failed_attempt || reset_err != ESP_ERR_INVALID_STATE) {
        return reset_err;
    }

    ESP_LOGW(TAG,
             "%s: primary provisioning state reset failed kind=%s error=%s; trying alternate reset",
             reason, failed_attempt ? "failed_attempt" : "abort_attempt",
             esp_err_to_name(reset_err));
    return network_prov_mgr_reset_wifi_sm_state_for_reprovision();
}

static esp_err_t reset_manager_for_app(const char *reason,
                                       bool discard_pending,
                                       provision_wifi_reset_kind_t kind) {
    const esp_err_t reset_err = reset_manager_wifi_state(kind, reason);
    if (reset_err != ESP_OK) {
        ESP_LOGE(TAG, "%s: provisioning state reset failed: %s", reason,
                 esp_err_to_name(reset_err));
        return reset_err;
    }
    const esp_err_t flash_err = restore_flash_credentials_after_candidate();
    if (flash_err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Flash Wi-Fi candidate cleanup failed: %s", reason,
                 esp_err_to_name(flash_err));
        return flash_err;
    }
    if (discard_pending) {
        const esp_err_t discard_err = nuwax_cloud_config_discard_pending();
        if (discard_err != ESP_OK) {
            ESP_LOGE(TAG, "%s: pending cloud config discard failed: %s", reason,
                     esp_err_to_name(discard_err));
            return discard_err;
        }
        memset(&s_staged_vox_config, 0, sizeof(s_staged_vox_config));
        s_has_staged_vox_config = false;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_state.app_claim_pending = false;
    s_state.app_abort_pending = false;
    s_state.wifi_reset_pending = false;
    s_state.wifi_reset_kind = PROVISION_WIFI_RESET_NONE;
    s_state.got_ip = false;
    s_state.old_wifi_got_ip = false;
    s_state.old_wifi_attempt_failed = false;
    s_state.new_wifi_attempt_active = false;
    s_state.wifi_attempt_owner_generation = 0;
    s_state.wifi_candidate_hash_valid = false;
    s_state.wifi_candidate_hash = 0;
    s_state.credentials_received_us = 0;
    s_state.wifi_config_admitted = !discard_pending && s_has_staged_vox_config;
    s_state.wifi_credentials_started = false;
    s_state.wifi_marker_clear_pending = false;
    s_state.wifi_config_locked = false;
    if (discard_pending) {
        s_state.failed_attempt_release_pending = false;
        s_state.failed_attempt_release_deadline_us = 0;
    }
    if (s_state.ble_connected && s_state.security_secured && !discard_pending) {
        s_state.app_session_progress_us = esp_timer_get_time();
        s_state.runtime_activity_us = s_state.app_session_progress_us;
    } else {
        s_state.runtime_activity_us = esp_timer_get_time();
    }
    if (discard_pending || s_state.reason[0] == '\0') {
        set_reason_locked(reason);
    }
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static esp_err_t start_old_wifi_attempt(void) {
    if (!force_reprovision_requested() ||
        !reprovision_record_can_restore(&s_reprovision_record) ||
        !nuwax_cloud_config_has_config()) {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t generation;
    portENTER_CRITICAL(&s_state_lock);
    s_state.wifi_owner = NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY;
    s_state.owner_generation++;
    generation = s_state.owner_generation;
    s_state.wifi_attempt_owner_generation = generation;
    s_state.got_ip = false;
    s_state.old_wifi_got_ip = false;
    s_state.old_wifi_attempt_failed = false;
    set_reason_locked("old_wifi_connecting");
    portEXIT_CRITICAL(&s_state_lock);

    const esp_err_t storage_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (storage_err != ESP_OK) {
        (void)release_old_wifi_owner(generation, "old_wifi_storage_failed");
        ESP_LOGE(TAG, "select RAM storage for old Wi-Fi recovery failed: %s",
                 esp_err_to_name(storage_err));
        return storage_err;
    }
    esp_err_t config_err = esp_wifi_set_config(
        WIFI_IF_STA, &s_reprovision_record.wifi_config);
    if (config_err != ESP_OK) {
        (void)release_old_wifi_owner(generation, "old_wifi_config_failed");
        return config_err;
    }

    provision_state_t state = get_state_snapshot();
    if (state.wifi_owner != NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY ||
        state.owner_generation != generation) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK && connect_err != ESP_ERR_WIFI_CONN) {
        (void)release_old_wifi_owner(generation,
                                     "old_wifi_connect_start_failed");
        return connect_err;
    }
    ESP_LOGI(TAG, "saved Wi-Fi recovery attempt started SSID=%s generation=%lu",
             (const char *)s_reprovision_record.wifi_config.sta.ssid,
             (unsigned long)generation);
    return ESP_OK;
}

static esp_err_t finalize_old_wifi_recovery(void) {
    if (!reprovision_record_can_restore(&s_reprovision_record) ||
        !nuwax_cloud_config_has_config()) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_ap_record_t ap = {0};
    if (!nuwax_wifi_is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        strncmp((const char *)ap.ssid,
                (const char *)s_reprovision_record.wifi_config.sta.ssid,
                sizeof(ap.ssid)) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(nuwax_cloud_config_discard_pending(), TAG,
                        "discard interrupted cloud candidate failed");
    memset(&s_staged_vox_config, 0, sizeof(s_staged_vox_config));
    s_has_staged_vox_config = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG,
                        "select Flash for recovered Wi-Fi failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(
                            WIFI_IF_STA, &s_reprovision_record.wifi_config),
                        TAG, "persist recovered Wi-Fi failed");
    ESP_RETURN_ON_ERROR(retry_persist_operation(
                            nuwax_wifi_import_driver_credentials,
                            "import recovered Wi-Fi credentials"),
                        TAG, "import recovered Wi-Fi credentials failed");
    ESP_RETURN_ON_ERROR(retry_write_wifi_verified_marker(
                            &s_reprovision_record.wifi_config),
                        TAG, "verify recovered Wi-Fi failed");
    ESP_RETURN_ON_ERROR(retry_persist_operation(
                            clear_reprovision_record,
                            "clear recovered Wi-Fi rollback record"),
                        TAG, "clear recovered Wi-Fi rollback record failed");

    s_provisioned_pair_ready = true;
    portENTER_CRITICAL(&s_state_lock);
    s_state.outcome = NUWAX_PROVISION_OUTCOME_OLD_WIFI_RECOVERED;
    set_reason_locked("old_wifi_recovered");
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static esp_err_t finalize_new_provisioning(void) {
    if (!nuwax_wifi_is_connected() || !nuwax_cloud_config_has_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_t verified_config = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &verified_config), TAG,
                        "read provisioned Wi-Fi before commit failed");
    if (verified_config.sta.ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    const provision_state_t state = get_state_snapshot();
    if (!state.wifi_candidate_hash_valid ||
        state.wifi_candidate_hash != wifi_credentials_hash(&verified_config) ||
        !nuwax_provision_attempt_belongs_to_owner(
            state.wifi_attempt_owner_generation, state.owner_generation)) {
        ESP_LOGE(TAG, "connected Wi-Fi does not match the active APP candidate");
        return ESP_ERR_INVALID_STATE;
    }
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        strncmp((const char *)ap.ssid,
                (const char *)verified_config.sta.ssid,
                sizeof(ap.ssid)) != 0) {
        ESP_LOGE(TAG, "connected AP does not match the active APP SSID");
        return ESP_ERR_INVALID_STATE;
    }
    int cancel_state = PROVISION_CANCEL_AVAILABLE;
    if (!atomic_compare_exchange_strong_explicit(
            &s_cancel_state, &cancel_state, PROVISION_CANCEL_COMMITTING,
            memory_order_acq_rel, memory_order_acquire) &&
        cancel_state == PROVISION_CANCEL_REQUESTED) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(retry_persist_operation(
                            nuwax_wifi_import_driver_credentials,
                            "import provisioned Wi-Fi credentials"),
                        TAG, "import provisioned Wi-Fi credentials failed");
    if (force_reprovision_requested()) {
        ESP_RETURN_ON_ERROR(write_wifi_commit_marker(&verified_config), TAG,
                            "write Wi-Fi commit marker failed");
    }
    ESP_RETURN_ON_ERROR(retry_persist_operation(
                            nuwax_cloud_config_commit_pending,
                            "commit pending cloud config"),
                        TAG, "commit pending cloud config failed");
    const esp_err_t marker_err =
        retry_write_wifi_verified_marker(&verified_config);
    if (marker_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Wi-Fi verification marker deferred after committed pair: %s",
                 esp_err_to_name(marker_err));
    }
    if (force_reprovision_requested()) {
        const esp_err_t cleanup_err = retry_persist_operation(
            clear_reprovision_record, "clear committed Wi-Fi rollback record");
        if (cleanup_err != ESP_OK) {
            /* The Wi-Fi/cloud pair and commit marker are already durable. Do
             * not roll back to the old Wi-Fi and create a mixed generation. */
            ESP_LOGE(TAG,
                     "committed pair retained with deferred rollback cleanup: %s",
                     esp_err_to_name(cleanup_err));
        }
    }
    s_provisioned_pair_ready = true;
    memset(&s_staged_vox_config, 0, sizeof(s_staged_vox_config));
    s_has_staged_vox_config = false;
    portENTER_CRITICAL(&s_state_lock);
    s_state.outcome = NUWAX_PROVISION_OUTCOME_SUCCESS;
    s_state.new_wifi_attempt_active = false;
    s_state.wifi_attempt_owner_generation = 0;
    s_state.wifi_candidate_hash_valid = false;
    s_state.wifi_candidate_hash = 0;
    s_state.failed_attempt_release_pending = false;
    s_state.failed_attempt_release_deadline_us = 0;
    s_state.credentials_received_us = 0;
    set_reason_locked(marker_err == ESP_OK
                          ? "provisioning_committed"
                          : "success_marker_deferred");
    portEXIT_CRITICAL(&s_state_lock);
    atomic_store_explicit(&s_cancel_state, PROVISION_CANCEL_COMMITTED,
                          memory_order_release);
    return ESP_OK;
}

esp_err_t nuwax_provision_main_start(const char *reason) {
    if (!s_initialized || nuwax_provision_main_is_active() ||
        (s_provisioned_pair_ready && !force_reprovision_requested())) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Only the explicit in-process rebind path gets the one-quantum
     * tolerance.  Keep all other callers on the strict boot floor so a new
     * provisioning entry point cannot silently inherit the relaxed policy. */
    s_runtime_start = reason && strcmp(reason, "runtime_reprovision") == 0;
    ESP_RETURN_ON_ERROR(ensure_vox_config_queue(), TAG,
                        "create vox-config queue failed");
    ESP_RETURN_ON_ERROR(prepare_security2(), TAG, "prepare security2 failed");
    ESP_RETURN_ON_ERROR(nuwax_cloud_config_discard_pending(), TAG,
                        "discard stale provisioning candidate failed");
    ESP_RETURN_ON_ERROR(restore_flash_credentials_after_candidate(), TAG,
                        "sanitize persisted Wi-Fi candidate failed");
    memset(&s_staged_vox_config, 0, sizeof(s_staged_vox_config));
    s_has_staged_vox_config = false;
    static uint8_t service_uuid[16] = {
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    };
    ESP_RETURN_ON_ERROR(network_prov_scheme_ble_set_service_uuid(service_uuid), TAG,
                        "set service uuid failed");
    reset_provision_state("starting");
    if (!provisioning_cancel_requested()) {
        atomic_store_explicit(
            &s_cancel_state,
            force_reprovision_requested() &&
                    reprovision_record_can_restore(&s_reprovision_record)
                ? PROVISION_CANCEL_AVAILABLE
                : PROVISION_CANCEL_DISABLED,
            memory_order_release);
    }
    /* nuwax_provision_main_run() already keeps the status endpoint available
     * for 15 seconds after GOT_IP. The manager parameter is only the transport
     * cleanup delay after stop; keeping it at 30 seconds would unnecessarily
     * block normal UI/business startup after provisioning succeeds. */
    ESP_RETURN_ON_ERROR(network_prov_mgr_disable_auto_stop(1000), TAG,
                        "disable auto stop failed");
    /* NimBLE performs several asynchronous internal-heap allocations while
     * the provisioning transport starts. Hold one contiguous block across
     * that burst so the allocator cannot fragment every block large enough
     * for the Security 2 and endpoint phase. Runtime provisioning first moves
     * the LVGL draw buffer to PSRAM, so this reservation does not steal the
     * minimum total headroom from the BLE stack. */
    void *internal_largest_reserve = heap_caps_malloc(
        PROVISION_INTERNAL_STARTUP_RESERVE_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!internal_largest_reserve) {
        ESP_LOGE(TAG,
                 "BLE provisioning contiguous reserve allocation failed bytes=%u internal=%u largest=%u",
                 (unsigned)PROVISION_INTERNAL_STARTUP_RESERVE_BYTES,
                 (unsigned)heap_caps_get_free_size(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = network_prov_mgr_start_provisioning(
        NETWORK_PROV_SECURITY_2, &s_sec2_params, s_service_name, NULL);
    if (err != ESP_OK) {
        heap_caps_free(internal_largest_reserve);
        ESP_LOGE(TAG, "start provisioning failed: %s", esp_err_to_name(err));
        return err;
    }
    atomic_store_explicit(&s_started, true, memory_order_release);

    err = network_prov_mgr_endpoint_register(DEVICE_INFO_EP,
                                             device_info_handler, NULL);
    if (err == ESP_OK) {
        err = network_prov_mgr_endpoint_register(VOX_CONFIG_EP,
                                                 vox_config_handler, NULL);
    }
    if (err != ESP_OK) {
        heap_caps_free(internal_largest_reserve);
        ESP_LOGE(TAG, "register provisioning endpoint failed: %s",
                 esp_err_to_name(err));
        network_prov_mgr_stop_provisioning();
        atomic_store_explicit(&s_started, false, memory_order_release);
        return err;
    }
    /* Keep the protected block until all steady-state endpoints have taken
     * their allocations. Releasing it before endpoint registration allowed a
     * small endpoint object to split the only qualifying block, making the
     * post-start guard fail by one allocator quantum. */
    heap_caps_free(internal_largest_reserve);
    const provision_memory_snapshot_t memory = provision_memory_snapshot();
    ESP_LOGI(TAG,
             "BLE provisioning steady-state memory guard mode=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u internal_min=%u",
             s_runtime_start ? "runtime" : "boot",
             (unsigned)memory.internal_free, (unsigned)memory.internal_largest,
             (unsigned)memory.dma_free, (unsigned)memory.dma_largest,
             (unsigned)(s_runtime_start
                 ? PROVISION_RUNTIME_INTERNAL_FREE_MIN_BYTES
                 : PROVISION_INTERNAL_FREE_MIN_BYTES));
    if (provision_memory_is_low_for_start(&memory, s_runtime_start)) {
        portENTER_CRITICAL(&s_state_lock);
        s_state.outcome = NUWAX_PROVISION_OUTCOME_RESOURCE_EXHAUSTED;
        set_reason_locked("ble_start_low_memory");
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGE(TAG,
                 "BLE provisioning stopped: steady-state reserve is unsafe mode=%s internal_free=%u internal_largest=%u dma_free=%u internal_min=%u",
                 s_runtime_start ? "runtime" : "boot",
                 (unsigned)memory.internal_free,
                 (unsigned)memory.internal_largest,
                 (unsigned)memory.dma_free,
                 (unsigned)(s_runtime_start
                     ? PROVISION_RUNTIME_INTERNAL_FREE_MIN_BYTES
                     : PROVISION_INTERNAL_FREE_MIN_BYTES));
        network_prov_mgr_stop_provisioning();
        atomic_store_explicit(&s_started, false, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    set_vox_config_accepting(true);
    return ESP_OK;
}

void nuwax_provision_main_set_restart_on_unrecoverable(bool enabled) {
    s_restart_on_unrecoverable = enabled;
}

static nuwax_provision_outcome_t provision_main_run_internal(
    uint32_t focused_wait_ms,
    uint32_t overall_timeout_ms,
    bool allow_old_wifi_recovery) {
    if (!nuwax_provision_main_is_active()) {
        return get_state_snapshot().outcome;
    }
    const bool has_old_wifi = allow_old_wifi_recovery &&
        nuwax_provision_old_wifi_is_terminal_candidate(
            force_reprovision_requested(),
            reprovision_record_can_restore(&s_reprovision_record),
            nuwax_cloud_config_has_config());
    const int64_t started_us = esp_timer_get_time();
    const int64_t focused_wait_deadline =
        started_us + (int64_t)focused_wait_ms * 1000;
    const int64_t overall_deadline = overall_timeout_ms > 0U
        ? started_us + (int64_t)overall_timeout_ms * 1000
        : 0;
    int64_t old_attempt_deadline = 0;
    int64_t next_old_attempt_us = focused_wait_deadline;
    int64_t next_reset_retry_us = 0;
    int64_t next_finalize_retry_us = 0;
    unsigned reset_failures = 0;
    unsigned finalize_failures = 0;

    ESP_LOGI(TAG,
             "continuous provisioning supervisor started focused_wait_ms=%lu overall_timeout_ms=%lu old_wifi=%d runtime_rebind=%d",
             (unsigned long)focused_wait_ms,
             (unsigned long)overall_timeout_ms,
             has_old_wifi ? 1 : 0,
             allow_old_wifi_recovery ? 0 : 1);
    while (true) {
        note_supervisor_heartbeat();
        if (provisioning_cancel_requested()) {
            return finish_runtime_cancel();
        }
        process_vox_config_job(pdMS_TO_TICKS(PROVISION_SUPERVISOR_POLL_MS));
        note_supervisor_heartbeat();
        if (provisioning_cancel_requested()) {
            return finish_runtime_cancel();
        }
        provision_state_t snapshot = get_state_snapshot();
        if (snapshot.outcome == NUWAX_PROVISION_OUTCOME_RESOURCE_EXHAUSTED) {
            close_and_drain_vox_config_jobs();
            return snapshot.outcome;
        }
        const int64_t now = esp_timer_get_time();
        const bool runtime_idle_timeout =
            nuwax_provision_runtime_idle_timed_out(
                !allow_old_wifi_recovery, snapshot.new_wifi_attempt_active,
                snapshot.runtime_activity_us > 0
                    ? snapshot.runtime_activity_us
                    : started_us,
                now, CONFIG_NUWAX_PROVISION_RUNTIME_IDLE_TIMEOUT_MS);
        const bool finalize_due = snapshot.got_ip &&
            now >= next_finalize_retry_us &&
            ((allow_old_wifi_recovery &&
              snapshot.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY &&
              snapshot.old_wifi_got_ip) ||
             (snapshot.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_APP &&
              snapshot.new_wifi_attempt_active));
        if (overall_deadline != 0 && now >= overall_deadline && !finalize_due) {
            portENTER_CRITICAL(&s_state_lock);
            s_state.outcome = NUWAX_PROVISION_OUTCOME_TIMEOUT;
            set_reason_locked("runtime_total_timeout");
            portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGW(TAG,
                     "bounded provisioning supervisor timed out after %lu ms",
                     (unsigned long)overall_timeout_ms);
            close_and_drain_vox_config_jobs();
            return NUWAX_PROVISION_OUTCOME_TIMEOUT;
        }
        if (runtime_idle_timeout && !finalize_due) {
            portENTER_CRITICAL(&s_state_lock);
            s_state.outcome = NUWAX_PROVISION_OUTCOME_TIMEOUT;
            set_reason_locked("runtime_idle_timeout");
            portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGW(TAG,
                     "runtime reprovisioning inactive for %lu ms; returning to durable previous configuration",
                     (unsigned long)CONFIG_NUWAX_PROVISION_RUNTIME_IDLE_TIMEOUT_MS);
            close_and_drain_vox_config_jobs();
            return NUWAX_PROVISION_OUTCOME_TIMEOUT;
        }

        if (allow_old_wifi_recovery &&
            snapshot.got_ip && snapshot.old_wifi_got_ip &&
            snapshot.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY &&
            now >= next_finalize_retry_us) {
            const esp_err_t finalize_err = finalize_old_wifi_recovery();
            if (finalize_err == ESP_OK) {
                close_and_drain_vox_config_jobs();
                return NUWAX_PROVISION_OUTCOME_OLD_WIFI_RECOVERED;
            }
            finalize_failures++;
            const uint32_t retry_delay_ms =
                nuwax_provision_reset_retry_delay_ms(
                    finalize_failures, PROVISION_FINALIZE_RETRY_BASE_MS,
                    PROVISION_FINALIZE_RETRY_MAX_MS);
            next_finalize_retry_us =
                now + (int64_t)retry_delay_ms * 1000;
            ESP_LOGE(TAG,
                     "old Wi-Fi recovery finalization retry=%u delay_ms=%lu failed: %s",
                     finalize_failures, (unsigned long)retry_delay_ms,
                     esp_err_to_name(finalize_err));
            portENTER_CRITICAL(&s_state_lock);
            s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
            set_reason_locked("old_wifi_finalize_failed");
            portEXIT_CRITICAL(&s_state_lock);
            if (finalize_failures >= PROVISION_FINALIZE_RESTART_FAILURES) {
                ESP_LOGE(TAG,
                         "old Wi-Fi finalization is unrecoverable; durable rollback state retained");
                if (s_restart_on_unrecoverable) {
                    vTaskDelay(pdMS_TO_TICKS(250));
                    esp_restart();
                }
                close_and_drain_vox_config_jobs();
                return NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
            }
            if (overall_deadline != 0 && now >= overall_deadline) {
                portENTER_CRITICAL(&s_state_lock);
                s_state.outcome = NUWAX_PROVISION_OUTCOME_TIMEOUT;
                set_reason_locked("runtime_total_timeout");
                portEXIT_CRITICAL(&s_state_lock);
                close_and_drain_vox_config_jobs();
                return NUWAX_PROVISION_OUTCOME_TIMEOUT;
            }
            continue;
        }

        if (snapshot.got_ip && !snapshot.old_wifi_got_ip &&
            snapshot.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_APP &&
            snapshot.new_wifi_attempt_active &&
            now >= next_finalize_retry_us) {
            if (provisioning_cancel_requested()) {
                return finish_runtime_cancel();
            }
            const esp_err_t finalize_err = finalize_new_provisioning();
            if (provisioning_cancel_requested()) {
                return finish_runtime_cancel();
            }
            if (finalize_err != ESP_OK) {
                finalize_failures++;
                const uint32_t retry_delay_ms =
                    nuwax_provision_reset_retry_delay_ms(
                        finalize_failures, PROVISION_FINALIZE_RETRY_BASE_MS,
                        PROVISION_FINALIZE_RETRY_MAX_MS);
                next_finalize_retry_us =
                    now + (int64_t)retry_delay_ms * 1000;
                ESP_LOGE(TAG,
                         "new provisioning finalization retry=%u delay_ms=%lu failed: %s",
                         finalize_failures, (unsigned long)retry_delay_ms,
                         esp_err_to_name(finalize_err));
                portENTER_CRITICAL(&s_state_lock);
                s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
                set_reason_locked("transaction_finalize_failed");
                portEXIT_CRITICAL(&s_state_lock);
                if (finalize_failures >= PROVISION_FINALIZE_RESTART_FAILURES) {
                    ESP_LOGE(TAG,
                             "new provisioning finalization is unrecoverable; persistent transaction state retained");
                    if (s_restart_on_unrecoverable) {
                        vTaskDelay(pdMS_TO_TICKS(250));
                        esp_restart();
                    }
                    close_and_drain_vox_config_jobs();
                    return NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
                }
                if (overall_deadline != 0 && now >= overall_deadline) {
                    portENTER_CRITICAL(&s_state_lock);
                    s_state.outcome = NUWAX_PROVISION_OUTCOME_TIMEOUT;
                    set_reason_locked("runtime_total_timeout");
                    portEXIT_CRITICAL(&s_state_lock);
                    close_and_drain_vox_config_jobs();
                    return NUWAX_PROVISION_OUTCOME_TIMEOUT;
                }
                continue;
            }

            /* Commit is complete before the final query window begins. */
            const int64_t committed_us = esp_timer_get_time();
            int64_t hold_deadline = committed_us +
                (int64_t)CONFIG_NUWAX_PROVISION_SUCCESS_BLE_HOLD_MS * 1000;
            if (overall_deadline != 0 && hold_deadline > overall_deadline) {
                hold_deadline = overall_deadline;
            }
            while (esp_timer_get_time() < hold_deadline) {
                const provision_state_t hold = get_state_snapshot();
                if (hold.success_read_owner_generation == hold.owner_generation &&
                    nuwax_provision_success_read_grace_elapsed(
                        committed_us, hold.success_response_read_us,
                        esp_timer_get_time())) {
                    ESP_LOGI(TAG, "provision success response read; BLE drain grace complete hold_ms=%lld",
                             (long long)((esp_timer_get_time() - committed_us) / 1000));
                    break;
                }
                process_vox_config_job(
                    pdMS_TO_TICKS(PROVISION_SUPERVISOR_POLL_MS));
            }
            ESP_LOGI(TAG, "provision success query window complete hold_ms=%lld",
                     (long long)((esp_timer_get_time() - committed_us) / 1000));
            close_and_drain_vox_config_jobs();
            return NUWAX_PROVISION_OUTCOME_SUCCESS;
        }

        if (snapshot.app_claim_pending && snapshot.ble_connected) {
            esp_err_t disconnect_err = esp_wifi_disconnect();
            if (disconnect_err != ESP_OK &&
                disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
                ESP_LOGW(TAG, "APP Wi-Fi claim disconnect failed: %s",
                         esp_err_to_name(disconnect_err));
            }
            wifi_config_t empty = {0};
            esp_err_t claim_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
            if (claim_err == ESP_OK) {
                claim_err = esp_wifi_set_config(WIFI_IF_STA, &empty);
            }
            if (claim_err == ESP_OK) {
                claim_err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
            }
            portENTER_CRITICAL(&s_state_lock);
            s_state.app_claim_pending = false;
            s_state.old_wifi_attempt_failed = false;
            if (claim_err == ESP_OK) {
                set_reason_locked("app_wifi_owner_ready");
            } else {
                s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
                s_state.app_abort_pending = true;
                set_reason_locked("app_wifi_claim_failed");
            }
            portEXIT_CRITICAL(&s_state_lock);
            old_attempt_deadline = 0;
            finalize_failures = 0;
            next_finalize_retry_us = 0;
            ESP_LOGI(TAG, "APP owns provisioning Wi-Fi generation=%lu result=%s",
                     (unsigned long)snapshot.owner_generation,
                     esp_err_to_name(claim_err));
            continue;
        }

        const bool security_timed_out = snapshot.ble_connected &&
            !snapshot.security_secured && snapshot.ble_connected_us != 0 &&
            now >= snapshot.ble_connected_us +
                (int64_t)CONFIG_NUWAX_PROVISION_SECURITY_SESSION_TIMEOUT_MS * 1000;
        const uint32_t app_session_idle_timeout_ms =
            nuwax_provision_app_session_idle_timeout_ms(
                snapshot.wifi_config_admitted,
                CONFIG_NUWAX_PROVISION_APP_PRECONFIG_IDLE_TIMEOUT_MS,
                CONFIG_NUWAX_PROVISION_APP_SESSION_IDLE_TIMEOUT_MS);
        const bool app_session_idle_timed_out =
            nuwax_provision_app_session_idle_timed_out(
                snapshot.ble_connected, snapshot.security_secured,
                snapshot.wifi_config_admitted,
                snapshot.new_wifi_attempt_active,
                snapshot.app_session_progress_us, now,
                CONFIG_NUWAX_PROVISION_APP_PRECONFIG_IDLE_TIMEOUT_MS,
                CONFIG_NUWAX_PROVISION_APP_SESSION_IDLE_TIMEOUT_MS);
        if (snapshot.app_abort_pending || security_timed_out ||
            app_session_idle_timed_out) {
            if (now < next_reset_retry_us) {
                continue;
            }
            if (security_timed_out) {
                ESP_LOGW(TAG, "BLE Security 2 setup timed out after %u ms",
                         (unsigned)CONFIG_NUWAX_PROVISION_SECURITY_SESSION_TIMEOUT_MS);
            } else if (app_session_idle_timed_out) {
                ESP_LOGW(TAG,
                         "secured BLE provisioning session idle for %u ms stage=%s; disconnecting for APP retry",
                         (unsigned)app_session_idle_timeout_ms,
                         snapshot.wifi_config_admitted
                             ? "vox_config_staged"
                             : "awaiting_vox_config");
            }
            if (snapshot.ble_connected) {
                const int terminate_rc = ble_gap_terminate(
                    snapshot.ble_conn_handle, 0x13);
                if (terminate_rc != 0) {
                    ESP_LOGW(TAG, "BLE client terminate failed rc=%d",
                             terminate_rc);
                }
            }
            const char *reset_reason = security_timed_out
                ? "security_setup_timeout"
                : (app_session_idle_timed_out
                       ? "app_session_idle_timeout"
                       : "app_session_aborted");
            const provision_wifi_reset_kind_t reset_kind =
                snapshot.wifi_reset_kind == PROVISION_WIFI_RESET_FAILED_ATTEMPT
                    ? PROVISION_WIFI_RESET_FAILED_ATTEMPT
                    : PROVISION_WIFI_RESET_ABORT_ATTEMPT;
            const esp_err_t reset_err = reset_manager_for_app(
                reset_reason, true, reset_kind);
            if (reset_err != ESP_OK) {
                reset_failures++;
                const uint32_t retry_delay_ms =
                    nuwax_provision_reset_retry_delay_ms(
                        reset_failures, PROVISION_RESET_RETRY_BASE_MS,
                        PROVISION_RESET_RETRY_MAX_MS);
                next_reset_retry_us = now + (int64_t)retry_delay_ms * 1000;
                portENTER_CRITICAL(&s_state_lock);
                s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
                set_reason_locked("app_session_reset_failed");
                portEXIT_CRITICAL(&s_state_lock);
                ESP_LOGE(TAG,
                         "APP session reset retry=%u delay_ms=%lu error=%s",
                         reset_failures, (unsigned long)retry_delay_ms,
                         esp_err_to_name(reset_err));
                if (reset_failures >= PROVISION_RESET_RESTART_FAILURES) {
                    ESP_LOGE(TAG,
                             "provisioning state reset is unrecoverable; persistent state retained");
                    if (s_restart_on_unrecoverable) {
                        vTaskDelay(pdMS_TO_TICKS(250));
                        esp_restart();
                    }
                    close_and_drain_vox_config_jobs();
                    return NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
                }
                continue;
            }
            reset_failures = 0;
            next_reset_retry_us = 0;
            finalize_failures = 0;
            next_finalize_retry_us = 0;
            set_owner_none(app_session_idle_timed_out
                               ? "app_session_idle_timeout"
                               : (security_timed_out
                                      ? "security_setup_timeout"
                                      : "waiting_for_app_reconnect"));
            if (reset_err == ESP_OK) {
                ESP_LOGI(TAG,
                         "provisioning session reset complete; BLE advertising ready for APP reconnect");
            }
            next_old_attempt_us = now;
            old_attempt_deadline = 0;
            continue;
        }

        if (snapshot.wifi_reset_pending) {
            if (now < next_reset_retry_us) {
                continue;
            }
            const provision_wifi_reset_kind_t reset_kind =
                snapshot.wifi_reset_kind == PROVISION_WIFI_RESET_NONE
                    ? PROVISION_WIFI_RESET_ABORT_ATTEMPT
                    : snapshot.wifi_reset_kind;
            const esp_err_t reset_err = reset_manager_for_app(
                "wifi_attempt_reset", false, reset_kind);
            if (reset_err != ESP_OK) {
                reset_failures++;
                const uint32_t retry_delay_ms =
                    nuwax_provision_reset_retry_delay_ms(
                        reset_failures, PROVISION_RESET_RETRY_BASE_MS,
                        PROVISION_RESET_RETRY_MAX_MS);
                next_reset_retry_us = now + (int64_t)retry_delay_ms * 1000;
                portENTER_CRITICAL(&s_state_lock);
                s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
                set_reason_locked("wifi_attempt_reset_failed");
                portEXIT_CRITICAL(&s_state_lock);
                ESP_LOGE(TAG,
                         "Wi-Fi attempt reset retry=%u delay_ms=%lu error=%s",
                         reset_failures, (unsigned long)retry_delay_ms,
                         esp_err_to_name(reset_err));
                if (reset_failures >= PROVISION_RESET_RESTART_FAILURES) {
                    ESP_LOGE(TAG,
                             "Wi-Fi attempt reset is unrecoverable; persistent state retained");
                    if (s_restart_on_unrecoverable) {
                        vTaskDelay(pdMS_TO_TICKS(250));
                        esp_restart();
                    }
                    close_and_drain_vox_config_jobs();
                    return NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
                }
            } else {
                reset_failures = 0;
                next_reset_retry_us = 0;
                finalize_failures = 0;
                next_finalize_retry_us = 0;
                ESP_LOGI(TAG,
                         "Wi-Fi attempt reset complete; current BLE session can submit new credentials");
            }
            continue;
        }

        if (nuwax_provision_failed_session_release_due(
                snapshot.failed_attempt_release_pending,
                snapshot.new_wifi_attempt_active,
                snapshot.wifi_credentials_started,
                snapshot.failed_attempt_release_deadline_us, now)) {
            bool release_session = false;
            portENTER_CRITICAL(&s_state_lock);
            if (nuwax_provision_failed_session_release_due(
                    s_state.failed_attempt_release_pending,
                    s_state.new_wifi_attempt_active,
                    s_state.wifi_credentials_started,
                    s_state.failed_attempt_release_deadline_us, now) &&
                s_state.failed_attempt_release_deadline_us ==
                    snapshot.failed_attempt_release_deadline_us) {
                s_state.failed_attempt_release_pending = false;
                s_state.failed_attempt_release_deadline_us = 0;
                s_state.app_abort_pending = true;
                s_state.wifi_reset_kind = PROVISION_WIFI_RESET_ABORT_ATTEMPT;
                set_reason_locked("wifi_failure_return_to_parallel_mode");
                release_session = true;
            }
            portEXIT_CRITICAL(&s_state_lock);
            if (release_session) {
                ESP_LOGW(TAG,
                         "Wi-Fi failure status hold expired after %u ms; releasing APP owner for BLE advertising and saved Wi-Fi recovery",
                         (unsigned)CONFIG_NUWAX_PROVISION_FAILURE_STATUS_HOLD_MS);
                continue;
            }
        }

        if (snapshot.wifi_marker_clear_pending) {
            const esp_err_t marker_err = clear_wifi_verified_marker();
            portENTER_CRITICAL(&s_state_lock);
            if (marker_err == ESP_OK) {
                s_state.wifi_marker_clear_pending = false;
            } else {
                s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
                s_state.app_abort_pending = true;
                set_reason_locked("wifi_marker_clear_failed");
            }
            portEXIT_CRITICAL(&s_state_lock);
            if (marker_err != ESP_OK) {
                ESP_LOGE(TAG, "failed to invalidate Wi-Fi verification: %s",
                         esp_err_to_name(marker_err));
            }
            continue;
        }

        if (nuwax_provision_wifi_attempt_timed_out(
                snapshot.new_wifi_attempt_active, snapshot.got_ip,
                snapshot.credentials_received_us, now,
                CREDENTIAL_OBSERVATION_TIMEOUT_US)) {
            portENTER_CRITICAL(&s_state_lock);
            if (s_state.credentials_received_us == snapshot.credentials_received_us &&
                s_state.new_wifi_attempt_active) {
                s_state.outcome = NUWAX_PROVISION_OUTCOME_TIMEOUT;
                set_reason_locked("status_timeout");
                s_state.credentials_received_us = 0;
                s_state.new_wifi_attempt_active = false;
                s_state.wifi_credentials_started = false;
                s_state.wifi_reset_pending = true;
                s_state.wifi_reset_kind = PROVISION_WIFI_RESET_ABORT_ATTEMPT;
                s_state.failed_attempt_release_pending = true;
                s_state.failed_attempt_release_deadline_us =
                    now +
                    (int64_t)CONFIG_NUWAX_PROVISION_FAILURE_STATUS_HOLD_MS * 1000;
            }
            portEXIT_CRITICAL(&s_state_lock);
            continue;
        }

        if (allow_old_wifi_recovery &&
            snapshot.wifi_owner == NUWAX_PROVISION_WIFI_OWNER_OLD_RECOVERY &&
            (snapshot.old_wifi_attempt_failed ||
             (old_attempt_deadline != 0 && now >= old_attempt_deadline))) {
            esp_err_t disconnect_err = esp_wifi_disconnect();
            if (disconnect_err != ESP_OK &&
                disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
                ESP_LOGW(TAG, "old Wi-Fi retry cleanup failed: %s",
                         esp_err_to_name(disconnect_err));
            }
            const bool released = release_old_wifi_owner(
                snapshot.owner_generation,
                snapshot.old_wifi_attempt_failed
                    ? "old_wifi_attempt_failed"
                    : "old_wifi_attempt_timeout");
            if (!released) {
                ESP_LOGI(TAG,
                         "old Wi-Fi cleanup skipped; ownership changed generation=%lu",
                         (unsigned long)snapshot.owner_generation);
            }
            next_old_attempt_us = now +
                (int64_t)CONFIG_NUWAX_PROVISION_OLD_WIFI_RETRY_MS * 1000;
            old_attempt_deadline = 0;
            finalize_failures = 0;
            next_finalize_retry_us = 0;
            continue;
        }

        snapshot = get_state_snapshot();
        const bool focused_wait_elapsed = now >= focused_wait_deadline;
        if (now >= next_old_attempt_us &&
            nuwax_provision_should_try_old_wifi(
                focused_wait_elapsed, has_old_wifi,
                snapshot.ble_connected, snapshot.security_secured,
                snapshot.new_wifi_attempt_active, snapshot.wifi_owner)) {
            const esp_err_t start_err = start_old_wifi_attempt();
            if (start_err == ESP_OK) {
                old_attempt_deadline = now +
                    (int64_t)CONFIG_NUWAX_PROVISION_OLD_WIFI_ATTEMPT_MS * 1000;
            } else {
                ESP_LOGW(TAG, "saved Wi-Fi recovery attempt could not start: %s",
                         esp_err_to_name(start_err));
                next_old_attempt_us = now +
                    (int64_t)CONFIG_NUWAX_PROVISION_OLD_WIFI_RETRY_MS * 1000;
            }
        }
    }
}

nuwax_provision_outcome_t nuwax_provision_main_run(uint32_t focused_wait_ms) {
    return provision_main_run_internal(focused_wait_ms, 0U, true);
}

nuwax_provision_outcome_t nuwax_provision_main_run_runtime_rebind(
    uint32_t focused_wait_ms) {
    return provision_main_run_internal(focused_wait_ms, 0U, false);
}

nuwax_provision_outcome_t nuwax_provision_main_run_bounded(
    uint32_t focused_wait_ms,
    uint32_t overall_timeout_ms) {
    if (overall_timeout_ms == 0U) {
        portENTER_CRITICAL(&s_state_lock);
        s_state.outcome = NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
        set_reason_locked("invalid_overall_timeout");
        portEXIT_CRITICAL(&s_state_lock);
        return NUWAX_PROVISION_OUTCOME_INTERNAL_ERROR;
    }
    return provision_main_run_internal(focused_wait_ms, overall_timeout_ms,
                                       true);
}

esp_err_t nuwax_provision_main_stop(void) {
    if (!s_initialized && !s_manager_initialized &&
        !s_event_handlers_registered) {
        return ESP_OK;
    }
    close_and_drain_vox_config_jobs();
    atomic_store_explicit(&s_cancel_state, PROVISION_CANCEL_DISABLED,
                          memory_order_release);
    if (atomic_exchange_explicit(&s_started, false,
                                 memory_order_acq_rel)) {
        network_prov_mgr_stop_provisioning();
    }
    esp_err_t err = ESP_OK;
    if (s_manager_initialized) {
        err = network_prov_mgr_deinit();
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_manager_initialized = false;
            if (err == ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG,
                         "provisioning manager was already deinitialized during stop");
                err = ESP_OK;
            }
        }
    }
    unregister_provisioning_handlers();
    if (s_sec2_salt) {
        free(s_sec2_salt);
        s_sec2_salt = NULL;
    }
    if (s_sec2_verifier) {
        free(s_sec2_verifier);
        s_sec2_verifier = NULL;
    }
    /* Endpoint teardown can finish after stop is requested. Keep these small
     * singleton synchronization objects for the process lifetime so a late
     * endpoint call can fail closed without dereferencing deleted objects. */
    s_accept_vox_config_jobs = false;
    s_runtime_start = false;
    s_initialized = false;
    if (s_provisioned_pair_ready) {
        const esp_err_t clear_err = write_low_memory_boot_request(false);
        if (clear_err != ESP_OK) {
            ESP_LOGW(TAG, "clear low-memory boot request failed: %s",
                     esp_err_to_name(clear_err));
            if (err == ESP_OK) {
                err = clear_err;
            }
        }
    }
    return err;
}

esp_err_t nuwax_provision_main_reset_wifi_credentials(void) {
    ESP_LOGW(TAG, "local UI reprovision requested; preserving current Wi-Fi for rollback");
    if (force_reprovision_requested() && s_reprovision_record_durable) {
        return ESP_OK;
    }
    const esp_err_t guard_err =
        nuwax_wake_word_detector_begin_flash_operation(
            PROVISION_NVS_FLASH_GUARD_TIMEOUT_MS);
    if (guard_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Wi-Fi reprovision record deferred while wake detector is active: %s",
                 esp_err_to_name(guard_err));
        return guard_err;
    }
    const esp_err_t err = save_reprovision_record();
    nuwax_wake_word_detector_end_flash_operation();
    if (err == ESP_OK &&
        reprovision_record_can_restore(&s_reprovision_record)) {
        atomic_store_explicit(&s_cancel_state, PROVISION_CANCEL_AVAILABLE,
                              memory_order_release);
    }
    return err;
}

bool nuwax_provision_main_is_reprovisioning(void) {
    return force_reprovision_requested();
}

esp_err_t nuwax_provision_main_commit_wifi_credentials(void) {
    if (!force_reprovision_requested()) {
        return ESP_OK;
    }
    if (nuwax_cloud_config_has_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_t current = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &current), TAG,
                        "read Wi-Fi config before rollback commit failed");
    if (current.sta.ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    if (!wifi_verified_marker_matches(&current)) {
        ESP_RETURN_ON_ERROR(retry_write_wifi_verified_marker(&current), TAG,
                            "verify committed Wi-Fi config failed");
    }
    ESP_LOGI(TAG, "new Wi-Fi credentials reached GOT_IP; clearing rollback record");
    return retry_persist_operation(clear_reprovision_record,
                                   "clear Wi-Fi rollback record");
}

static bool wait_for_wifi_connection_to(const wifi_config_t *config,
                                        uint32_t timeout_ms) {
    if (!config || config->sta.ssid[0] == '\0') {
        return false;
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        wifi_ap_record_t ap = {0};
        if (nuwax_wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
            strncmp((const char *)ap.ssid, (const char *)config->sta.ssid,
                    sizeof(ap.ssid)) == 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

esp_err_t nuwax_provision_main_rollback_wifi_credentials(void) {
    if (!force_reprovision_requested()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "BLE reprovision did not complete; restoring previous Wi-Fi credentials");
    const esp_err_t discard_err = nuwax_cloud_config_discard_pending();
    if (discard_err != ESP_OK) {
        ESP_LOGE(TAG, "discard pending cloud config during rollback failed: %s",
                 esp_err_to_name(discard_err));
    }
    const esp_err_t marker_clear_err = clear_wifi_verified_marker();
    if (marker_clear_err != ESP_OK) {
        ESP_LOGE(TAG, "clear failed Wi-Fi verification during rollback failed: %s",
                 esp_err_to_name(marker_clear_err));
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG,
                        "set Wi-Fi storage for rollback failed");
    esp_err_t disconnect_err = esp_wifi_disconnect();
    if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "disconnect failed Wi-Fi before rollback failed: %s",
                 esp_err_to_name(disconnect_err));
    }
    const int64_t disconnect_deadline = esp_timer_get_time() + 2000000LL;
    while (nuwax_wifi_is_connected() && esp_timer_get_time() < disconnect_deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_err_t config_err = ESP_ERR_WIFI_STATE;
    for (int attempt = 1; attempt <= 10; ++attempt) {
        config_err = esp_wifi_set_config(WIFI_IF_STA,
                                         &s_reprovision_record.wifi_config);
        if (config_err == ESP_OK) {
            break;
        }
        if (config_err != ESP_ERR_WIFI_STATE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_RETURN_ON_ERROR(config_err, TAG, "restore previous Wi-Fi config failed");
    esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK && connect_err != ESP_ERR_WIFI_CONN) {
        return connect_err;
    }
    if (!wait_for_wifi_connection_to(&s_reprovision_record.wifi_config,
                                     ROLLBACK_CONNECT_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "previous Wi-Fi did not reconnect within %u ms; rollback record retained",
                 (unsigned)ROLLBACK_CONNECT_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    ESP_RETURN_ON_ERROR(retry_persist_operation(
                            nuwax_wifi_import_driver_credentials,
                            "restore application Wi-Fi credentials"),
                        TAG, "restore application Wi-Fi credentials failed");
    ESP_RETURN_ON_ERROR(retry_write_wifi_verified_marker(
                            &s_reprovision_record.wifi_config),
                        TAG, "verify restored Wi-Fi credentials failed");
    if (discard_err != ESP_OK || marker_clear_err != ESP_OK) {
        ESP_LOGE(TAG, "rollback data retained because prerequisite NVS cleanup failed");
        return discard_err != ESP_OK ? discard_err : marker_clear_err;
    }
    return clear_reprovision_record();
}

const char *nuwax_provision_main_service_name(void) { return s_service_name; }
const char *nuwax_provision_main_qr_payload(void) { return s_qr_payload; }
nuwax_provision_outcome_t nuwax_provision_main_last_outcome(void) {
    return get_state_snapshot().outcome;
}
const char *nuwax_provision_main_last_reason(void) {
    static char reason[48];
    const provision_state_t snapshot = get_state_snapshot();
    strlcpy(reason, snapshot.reason, sizeof(reason));
    return reason;
}
