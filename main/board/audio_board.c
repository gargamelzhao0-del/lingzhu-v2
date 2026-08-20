/* P2-1 audio_board abstraction — board-agnostic facade. See audio_board.h. */
#include "board/audio_board.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
#include "board/vocat_audio_board.h"
#endif

static const char *TAG = "nuwax_audio_board";

static _Atomic(SemaphoreHandle_t) s_lock = ATOMIC_VAR_INIT(NULL);
static int s_init_refcount;
static bool s_teardown_pending;
static uint32_t s_generation;
static uint32_t s_next_token;
static nuwax_audio_board_lease_t s_rx_lease;
static nuwax_audio_board_lease_t s_tx_lease;
static TaskHandle_t s_diagnostic_owner;
static portMUX_TYPE s_ref_health_lock = portMUX_INITIALIZER_UNLOCKED;
static nuwax_hardware_ref_health_t s_ref_health = NUWAX_HARDWARE_REF_HEALTH_UNTESTED;

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
static const nuwax_audio_board_caps_t s_caps = {
    .codec = NUWAX_AUDIO_CODEC_ES7210_ES8311,
    .hardware_profile = "miaopan_v2_0_n32r8",
    .mic_channels = 4,
    .has_speaker = true,
    .full_duplex = true,
    .shared_i2s = true,
    .sample_rate_hz = VOCAT_AUDIO_SAMPLE_RATE_HZ,
    /* P3-0: frozen slot semantics. This is a route declaration; hardware AEC
     * remains disabled unless the runtime ref_health gate passes. */
    .has_hardware_ref_candidate = true,
    .aec_reference      = NUWAX_AEC_REF_ES7210_SLOT2,
    .ref_slot           = 2,   /* ES7210 slot2 = MIC3 candidate reference route */
    .mic_primary_slot   = 0,   /* ES7210 slot0 = MIC1 = primary mic */
    .input_format       = NUWAX_AFE_INPUT_MR,  /* target path: slot0(M) + slot2(R) */
};
#else
static const nuwax_audio_board_caps_t s_caps = {
    .codec = NUWAX_AUDIO_CODEC_LEGACY_DIGITAL,
    .hardware_profile = "legacy",
    .mic_channels = 1,
    .has_speaker = true,
    .full_duplex = false,
    .shared_i2s = false,
    .sample_rate_hz = CONFIG_NUWAX_VOICE_SAMPLE_RATE,
    .has_hardware_ref_candidate = false,
    .aec_reference      = NUWAX_AEC_REF_NONE,
    .ref_slot           = 0,
    .mic_primary_slot   = 0,
    .input_format       = NUWAX_AFE_INPUT_MM,
};
#endif

static esp_err_t ensure_lock(SemaphoreHandle_t *out_lock)
{
    SemaphoreHandle_t lock = atomic_load_explicit(&s_lock, memory_order_acquire);
    if (!lock) {
        SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
        if (!candidate) {
            return ESP_ERR_NO_MEM;
        }

        SemaphoreHandle_t expected = NULL;
        if (atomic_compare_exchange_strong_explicit(&s_lock,
                                                    &expected,
                                                    candidate,
                                                    memory_order_release,
                                                    memory_order_acquire)) {
            lock = candidate;
        } else {
            /* Another task published the process-wide lock first. */
            vSemaphoreDelete(candidate);
            lock = expected;
        }
    }
    *out_lock = lock;
    return ESP_OK;
}

const nuwax_audio_board_caps_t *nuwax_audio_board_get_caps(void)
{
    return &s_caps;
}

void nuwax_audio_board_set_hardware_ref_health(nuwax_hardware_ref_health_t state)
{
    switch (state) {
    case NUWAX_HARDWARE_REF_HEALTH_UNTESTED:
    case NUWAX_HARDWARE_REF_HEALTH_PASSED:
    case NUWAX_HARDWARE_REF_HEALTH_FAILED:
    case NUWAX_HARDWARE_REF_HEALTH_BLOCKED_UNPROVEN:
        break;
    default:
        state = NUWAX_HARDWARE_REF_HEALTH_FAILED;
        break;
    }
    portENTER_CRITICAL(&s_ref_health_lock);
    s_ref_health = state;
    portEXIT_CRITICAL(&s_ref_health_lock);
}

nuwax_hardware_ref_health_t nuwax_audio_board_get_hardware_ref_health(void)
{
    portENTER_CRITICAL(&s_ref_health_lock);
    const nuwax_hardware_ref_health_t state = s_ref_health;
    portEXIT_CRITICAL(&s_ref_health_lock);
    return state;
}

static void clear_teardown_state_locked(void)
{
    s_init_refcount = 0;
    s_teardown_pending = false;
    memset(&s_rx_lease, 0, sizeof(s_rx_lease));
    memset(&s_tx_lease, 0, sizeof(s_tx_lease));
    s_diagnostic_owner = NULL;
}

esp_err_t nuwax_audio_board_init(void)
{
    SemaphoreHandle_t lock = NULL;
    ESP_RETURN_ON_ERROR(ensure_lock(&lock), TAG, "lock alloc failed");
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_teardown_pending) {
        if (s_init_refcount != 1 || s_rx_lease.token != 0 ||
            s_tx_lease.token != 0) {
            ESP_LOGE(TAG,
                     "audio board teardown retry state invalid refcount=%d rx_token=%lu tx_token=%lu",
                     s_init_refcount,
                     (unsigned long)s_rx_lease.token,
                     (unsigned long)s_tx_lease.token);
            err = ESP_ERR_INVALID_STATE;
        } else {
            ESP_LOGW(TAG,
                     "audio board init retrying pending teardown before reinitialization");
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
            err = nuwax_vocat_audio_board_deinit();
#endif
            if (err == ESP_OK) {
                clear_teardown_state_locked();
                ESP_LOGW(TAG, "audio board pending teardown recovered");
            } else {
                ESP_LOGE(TAG,
                         "audio board pending teardown retry failed: %s",
                         esp_err_to_name(err));
            }
        }
    }
    if (err == ESP_OK && s_init_refcount == 0) {
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
        err = nuwax_vocat_audio_board_init();
#endif
        if (err == ESP_OK) {
            s_generation++;
            if (s_generation == 0) {
                s_generation = 1;
            }
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0 && \
    !CONFIG_NUWAX_VOCAT_MIC3_REFERENCE_PRODUCTION_PROVEN
            nuwax_audio_board_set_hardware_ref_health(
                NUWAX_HARDWARE_REF_HEALTH_BLOCKED_UNPROVEN);
#endif
            ESP_LOGI(TAG, "audio board up codec=%d mic_ch=%u duplex=%d shared_i2s=%d ref_candidate=%d aec_ref=%d ref_slot=%u mic_primary_slot=%u input_format=%s",
                     s_caps.codec, s_caps.mic_channels, s_caps.full_duplex, s_caps.shared_i2s,
                     s_caps.has_hardware_ref_candidate, s_caps.aec_reference, s_caps.ref_slot, s_caps.mic_primary_slot,
                     s_caps.input_format == NUWAX_AFE_INPUT_MR ? "MR" :
                     s_caps.input_format == NUWAX_AFE_INPUT_MMR ? "MMR" : "MM");
        }
    }
    if (err == ESP_OK) {
        s_init_refcount++;
    }
    xSemaphoreGive(lock);
    return err;
}

esp_err_t nuwax_audio_board_deinit(void)
{
    SemaphoreHandle_t lock = NULL;
    ESP_RETURN_ON_ERROR(ensure_lock(&lock), TAG, "lock alloc failed");
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_init_refcount > 0) {
        if (s_init_refcount == 1 &&
            (s_rx_lease.token != 0 || s_tx_lease.token != 0)) {
            ESP_LOGE(TAG,
                     "audio board final deinit rejected while channel leased rx_token=%lu tx_token=%lu",
                     (unsigned long)s_rx_lease.token,
                     (unsigned long)s_tx_lease.token);
            err = ESP_ERR_INVALID_STATE;
        } else if (s_init_refcount > 1) {
            s_init_refcount--;
        } else {
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
            err = nuwax_vocat_audio_board_deinit();
#endif
            if (err == ESP_OK) {
                clear_teardown_state_locked();
                ESP_LOGI(TAG, "audio board down");
            } else {
                s_teardown_pending = true;
                ESP_LOGE(TAG,
                         "audio board teardown pending retry; ownership retained: %s",
                         esp_err_to_name(err));
            }
        }
    }
    xSemaphoreGive(lock);
    return err;
}

static uint32_t next_token(void)
{
    s_next_token++;
    if (s_next_token == 0) {
        s_next_token = 1;
    }
    return s_next_token;
}

static bool lease_matches(const nuwax_audio_board_lease_t *current,
                          const nuwax_audio_board_lease_t *candidate)
{
    return current && candidate &&
           current->generation == candidate->generation &&
           current->token == candidate->token &&
           current->channels == candidate->channels;
}

static esp_err_t acquire_impl(uint8_t channels,
                              void **out_rx_handle,
                              void **out_tx_handle,
                              nuwax_audio_board_lease_t *out_lease,
                              bool log_expected_rejection)
{
    ESP_RETURN_ON_FALSE(out_lease, ESP_ERR_INVALID_ARG, TAG, "lease output is NULL");
    memset(out_lease, 0, sizeof(*out_lease));
    if (out_rx_handle) {
        *out_rx_handle = NULL;
    }
    if (out_tx_handle) {
        *out_tx_handle = NULL;
    }
    if (!s_caps.shared_i2s) {
        return ESP_ERR_NOT_SUPPORTED; /* legacy: caller creates its own channel */
    }
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    SemaphoreHandle_t lock = NULL;
    ESP_RETURN_ON_ERROR(ensure_lock(&lock), TAG, "lock alloc failed");
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    void *rx = NULL;
    void *tx = NULL;
    if (s_init_refcount <= 0 || s_teardown_pending) {
        ESP_LOGE(TAG, "board not initialized");
        err = ESP_ERR_INVALID_STATE;
    } else if (((channels & NUWAX_AUDIO_BOARD_LEASE_RX) && s_rx_lease.token != 0) ||
               ((channels & NUWAX_AUDIO_BOARD_LEASE_TX) && s_tx_lease.token != 0)) {
        if (log_expected_rejection) {
            ESP_LOGE(TAG,
                     "audio channel already acquired requested=0x%02x rx_token=%lu tx_token=%lu",
                     channels,
                     (unsigned long)s_rx_lease.token,
                     (unsigned long)s_tx_lease.token);
        }
        err = ESP_ERR_INVALID_STATE;
    } else {
        if (channels & NUWAX_AUDIO_BOARD_LEASE_RX) {
            rx = nuwax_vocat_audio_board_get_rx_handle();
        }
        if (channels & NUWAX_AUDIO_BOARD_LEASE_TX) {
            tx = nuwax_vocat_audio_board_get_tx_handle();
        }
        if (((channels & NUWAX_AUDIO_BOARD_LEASE_RX) && !rx) ||
            ((channels & NUWAX_AUDIO_BOARD_LEASE_TX) && !tx)) {
            ESP_LOGE(TAG, "shared audio handle unavailable requested=0x%02x", channels);
            err = ESP_ERR_INVALID_STATE;
        } else {
            const nuwax_audio_board_lease_t lease = {
                .generation = s_generation,
                .token = next_token(),
                .channels = channels,
            };
            if (channels & NUWAX_AUDIO_BOARD_LEASE_RX) {
                s_rx_lease = lease;
                *out_rx_handle = rx;
            }
            if (channels & NUWAX_AUDIO_BOARD_LEASE_TX) {
                s_tx_lease = lease;
                *out_tx_handle = tx;
            }
            if (channels & NUWAX_AUDIO_BOARD_LEASE_DIAGNOSTIC) {
                s_diagnostic_owner = xTaskGetCurrentTaskHandle();
            }
            *out_lease = lease;
        }
    }
    xSemaphoreGive(lock);
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t nuwax_audio_board_acquire_rx(void **out_rx_handle,
                                       nuwax_audio_board_lease_t *out_lease)
{
    ESP_RETURN_ON_FALSE(out_rx_handle, ESP_ERR_INVALID_ARG, TAG, "RX output is NULL");
    return acquire_impl(NUWAX_AUDIO_BOARD_LEASE_RX,
                        out_rx_handle,
                        NULL,
                        out_lease,
                        true);
}

esp_err_t nuwax_audio_board_acquire_tx(void **out_tx_handle,
                                       nuwax_audio_board_lease_t *out_lease)
{
    ESP_RETURN_ON_FALSE(out_tx_handle, ESP_ERR_INVALID_ARG, TAG, "TX output is NULL");
    return acquire_impl(NUWAX_AUDIO_BOARD_LEASE_TX,
                        NULL,
                        out_tx_handle,
                        out_lease,
                        true);
}

esp_err_t nuwax_audio_board_acquire_diagnostic(void **out_rx_handle,
                                               void **out_tx_handle,
                                               nuwax_audio_board_lease_t *out_lease)
{
    ESP_RETURN_ON_FALSE(out_rx_handle && out_tx_handle,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "diagnostic channel output is NULL");
    return acquire_impl(NUWAX_AUDIO_BOARD_LEASE_RX |
                            NUWAX_AUDIO_BOARD_LEASE_TX |
                            NUWAX_AUDIO_BOARD_LEASE_DIAGNOSTIC,
                        out_rx_handle,
                        out_tx_handle,
                        out_lease,
                        true);
}

esp_err_t nuwax_audio_board_release(nuwax_audio_board_lease_t *lease)
{
    ESP_RETURN_ON_FALSE(lease && lease->token != 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid audio lease");
    SemaphoreHandle_t lock = NULL;
    ESP_RETURN_ON_ERROR(ensure_lock(&lock), TAG, "release lock alloc failed");
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    const uint8_t known_channels = NUWAX_AUDIO_BOARD_LEASE_RX |
                                   NUWAX_AUDIO_BOARD_LEASE_TX |
                                   NUWAX_AUDIO_BOARD_LEASE_DIAGNOSTIC;
    const bool has_rx = (lease->channels & NUWAX_AUDIO_BOARD_LEASE_RX) != 0;
    const bool has_tx = (lease->channels & NUWAX_AUDIO_BOARD_LEASE_TX) != 0;
    const bool has_diagnostic =
        (lease->channels & NUWAX_AUDIO_BOARD_LEASE_DIAGNOSTIC) != 0;
    const bool shape_valid =
        (lease->channels & (uint8_t)~known_channels) == 0 &&
                             (has_rx || has_tx) &&
                             (!has_diagnostic || (has_rx && has_tx));
    const bool rx_match = !has_rx || lease_matches(&s_rx_lease, lease);
    const bool tx_match = !has_tx || lease_matches(&s_tx_lease, lease);
    if (!shape_valid || !rx_match || !tx_match) {
        ESP_LOGE(TAG,
                 "stale or malformed audio lease release rejected generation=%lu token=%lu channels=0x%02x current_generation=%lu rx_token=%lu rx_channels=0x%02x tx_token=%lu tx_channels=0x%02x",
                 (unsigned long)lease->generation,
                 (unsigned long)lease->token,
                 (unsigned)lease->channels,
                 (unsigned long)s_generation,
                 (unsigned long)s_rx_lease.token,
                 (unsigned)s_rx_lease.channels,
                 (unsigned long)s_tx_lease.token,
                 (unsigned)s_tx_lease.channels);
        err = ESP_ERR_INVALID_STATE;
    } else {
        if (has_rx) {
            memset(&s_rx_lease, 0, sizeof(s_rx_lease));
        }
        if (has_tx) {
            memset(&s_tx_lease, 0, sizeof(s_tx_lease));
        }
        if (has_diagnostic) {
            s_diagnostic_owner = NULL;
        }
        memset(lease, 0, sizeof(*lease));
    }
    xSemaphoreGive(lock);
    return err;
}

bool nuwax_audio_board_diagnostic_lease_is_held(void)
{
    SemaphoreHandle_t lock = atomic_load_explicit(&s_lock, memory_order_acquire);
    if (!lock) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool held = s_rx_lease.token != 0 &&
                      s_rx_lease.token == s_tx_lease.token &&
                      s_rx_lease.generation == s_tx_lease.generation &&
                      (s_rx_lease.channels & NUWAX_AUDIO_BOARD_LEASE_DIAGNOSTIC) != 0 &&
                      s_diagnostic_owner == xTaskGetCurrentTaskHandle();
    xSemaphoreGive(lock);
    return held;
}

uint32_t nuwax_audio_board_get_rx_overflow_count(void)
{
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    return nuwax_vocat_audio_board_get_rx_overflow_count();
#else
    return 0;
#endif
}

esp_err_t nuwax_audio_board_force_speaker_mute(void)
{
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    return nuwax_vocat_audio_board_force_speaker_mute();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t nuwax_audio_board_prepare_speaker_playback(void)
{
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    return nuwax_vocat_audio_board_prepare_speaker_playback();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t nuwax_audio_board_lifecycle_selftest(int cycles)
{
    if (cycles <= 0) {
        cycles = 1;
    }
    const bool shared = s_caps.shared_i2s;
    for (int i = 0; i < cycles; i++) {
        esp_err_t err = nuwax_audio_board_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "selftest cycle %d: init failed %s", i, esp_err_to_name(err));
            return err;
        }

        void *rx = NULL;
        void *tx = NULL;
        nuwax_audio_board_lease_t rx_lease = {0};
        nuwax_audio_board_lease_t tx_lease = {0};
        esp_err_t rx_err = nuwax_audio_board_acquire_rx(&rx, &rx_lease);
        esp_err_t tx_err = nuwax_audio_board_acquire_tx(&tx, &tx_lease);

        if (shared) {
            if (rx_err != ESP_OK || tx_err != ESP_OK || !rx || !tx) {
                ESP_LOGE(TAG, "selftest cycle %d: acquire failed rx=%s tx=%s", i,
                         esp_err_to_name(rx_err), esp_err_to_name(tx_err));
                if (rx_err == ESP_OK) {
                    (void)nuwax_audio_board_release(&rx_lease);
                }
                if (tx_err == ESP_OK) {
                    (void)nuwax_audio_board_release(&tx_lease);
                }
                (void)nuwax_audio_board_deinit();
                return ESP_FAIL;
            }
            /* double-acquire must be rejected, not hand out the channel twice */
            void *dup = NULL;
            nuwax_audio_board_lease_t dup_lease = {0};
            if (acquire_impl(NUWAX_AUDIO_BOARD_LEASE_RX,
                             &dup,
                             NULL,
                             &dup_lease,
                             false) == ESP_OK) {
                ESP_LOGE(TAG, "selftest cycle %d: double-acquire RX not rejected", i);
                (void)nuwax_audio_board_release(&dup_lease);
                (void)nuwax_audio_board_release(&tx_lease);
                (void)nuwax_audio_board_deinit();
                return ESP_FAIL;
            }
            (void)nuwax_audio_board_release(&rx_lease);
            (void)nuwax_audio_board_release(&tx_lease);
        } else if (rx_err != ESP_ERR_NOT_SUPPORTED || tx_err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(TAG, "selftest cycle %d: legacy acquire should be NOT_SUPPORTED rx=%s tx=%s",
                     i, esp_err_to_name(rx_err), esp_err_to_name(tx_err));
            nuwax_audio_board_deinit();
            return ESP_FAIL;
        }

        nuwax_audio_board_deinit();
    }

    SemaphoreHandle_t lock = NULL;
    ESP_RETURN_ON_ERROR(ensure_lock(&lock), TAG, "selftest lock alloc failed");
    xSemaphoreTake(lock, portMAX_DELAY);
    const int final_refcount = s_init_refcount;
    xSemaphoreGive(lock);
    if (final_refcount != 0) {
        ESP_LOGE(TAG, "selftest: refcount leaked = %d", final_refcount);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "P2-1 audio_board lifecycle selftest PASS cycles=%d shared_i2s=%d", cycles, shared);
    return ESP_OK;
}
