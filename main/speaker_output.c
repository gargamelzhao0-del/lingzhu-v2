#include "speaker_output.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "device_error_report.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "p3_software_ref.h"

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
#include "board/audio_board.h"
#endif

#ifndef CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT
#define CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT 80
#endif

#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
#define CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS
#define CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS 2000
#endif

#ifndef CONFIG_NUWAX_LIVE_AEC_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_SPEAKER_WRITE_TIMEOUT_MS
#define CONFIG_NUWAX_SPEAKER_WRITE_TIMEOUT_MS 1000
#endif

#ifndef CONFIG_NUWAX_SPEAKER_DMA_FRAME_NUM
#define CONFIG_NUWAX_SPEAKER_DMA_FRAME_NUM 240
#endif
#ifndef CONFIG_NUWAX_SPEAKER_DMA_DESC_NUM
#define CONFIG_NUWAX_SPEAKER_DMA_DESC_NUM 6
#endif

#ifndef CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM
#define CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM 6
#endif
#ifndef CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM
#define CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM 240
#endif

#define NUWAX_SPEAKER_DISCRETE_STOP_MUTE_MS 20U
#define NUWAX_SPEAKER_DISCRETE_STOP_PRELOAD_MS 20U
#define NUWAX_SPEAKER_SHARED_STOP_SETTLE_MARGIN_MS 30U
#define NUWAX_SPEAKER_INIT_RETRY_DELAY_MS 20
#define NUWAX_SPEAKER_INIT_RETRIES 2
#define NUWAX_SPEAKER_OUTPUT_LOCK_TIMEOUT_MS 250U
#define NUWAX_SPEAKER_STARTUP_FADE_MS 15U
#define NUWAX_SPEAKER_STARTUP_FADE_FRAMES \
    ((uint32_t)CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE * NUWAX_SPEAKER_STARTUP_FADE_MS / 1000U)
#define NUWAX_SPEAKER_GAIN_ONE_Q15 32768U
#define NUWAX_SPEAKER_STOP_LOCK_TIMEOUT_MS \
    ((uint32_t)CONFIG_NUWAX_SPEAKER_WRITE_TIMEOUT_MS + 250U)

static const char *TAG = "nuwax_speaker";

static uint8_t clamp_volume_percent(uint8_t volume_percent) {
    return volume_percent > 100 ? 100 : volume_percent;
}

/* Smoothstep has zero slope at both ends. Fixed-point math is used only
 * during the initial 15 ms; normal playback keeps its original scaling. */
static uint32_t speaker_output_startup_gain_q15(uint32_t position) {
    if (NUWAX_SPEAKER_STARTUP_FADE_FRAMES < 2U ||
        position >= NUWAX_SPEAKER_STARTUP_FADE_FRAMES - 1U) {
        return NUWAX_SPEAKER_GAIN_ONE_Q15;
    }
    const uint32_t t = (uint32_t)(((uint64_t)position * NUWAX_SPEAKER_GAIN_ONE_Q15) /
                                (NUWAX_SPEAKER_STARTUP_FADE_FRAMES - 1U));
    return (uint32_t)(((uint64_t)t * t * (3U * NUWAX_SPEAKER_GAIN_ONE_Q15 - 2U * t)) /
                      ((uint64_t)NUWAX_SPEAKER_GAIN_ONE_Q15 * NUWAX_SPEAKER_GAIN_ONE_Q15));
}

static bool speaker_output_lock(SemaphoreHandle_t lock, uint32_t timeout_ms) {
    if (!lock) {
        return true;
    }
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0U && ticks == 0) {
        ticks = 1;
    }
    return xSemaphoreTake(lock, ticks) == pdTRUE;
}

static size_t speaker_output_dma_chunk_frames(
    const nuwax_speaker_output_t *speaker) {
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    if (speaker && speaker->vocat_shared_tx) {
        return (size_t)CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM;
    }
#else
    (void)speaker;
#endif
    return (size_t)CONFIG_NUWAX_SPEAKER_DMA_FRAME_NUM;
}

static size_t speaker_output_dma_capacity_frames(
    const nuwax_speaker_output_t *speaker) {
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    if (speaker && speaker->vocat_shared_tx) {
        return (size_t)CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM *
               (size_t)CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM;
    }
#else
    (void)speaker;
#endif
    return (size_t)CONFIG_NUWAX_SPEAKER_DMA_DESC_NUM *
           (size_t)CONFIG_NUWAX_SPEAKER_DMA_FRAME_NUM;
}

static size_t speaker_output_stop_silence_samples(
    const nuwax_speaker_output_t *speaker) {
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    if (speaker && speaker->vocat_shared_tx) {
        return (size_t)CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM *
               (size_t)CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM;
    }
#else
    (void)speaker;
#endif
    return ((size_t)CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE *
            NUWAX_SPEAKER_DISCRETE_STOP_PRELOAD_MS) /
           1000U;
}

static uint32_t speaker_output_stop_settle_ms(
    const nuwax_speaker_output_t *speaker,
    size_t silence_samples) {
    if (speaker && speaker->vocat_shared_tx && silence_samples > 0U) {
        const uint64_t dma_ms =
            ((uint64_t)silence_samples * 1000ULL +
             CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE - 1U) /
            CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE;
        const uint64_t settle_ms =
            dma_ms + NUWAX_SPEAKER_SHARED_STOP_SETTLE_MARGIN_MS;
        return settle_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)settle_ms;
    }
    return NUWAX_SPEAKER_DISCRETE_STOP_MUTE_MS;
}

/* Keep the software reference on the same timeline as the physical stop.
 * The DMA ring is preloaded before TX is re-enabled, and the settle margin
 * keeps the channel quiet after the last descriptor has elapsed.  Writing
 * zeros only for the preload span leaves the reference reader short during
 * that margin, which can make AEC report a false reference underrun exactly
 * when a barge-in handoff is completing. */
static void speaker_output_write_ref_silence(
    const nuwax_speaker_output_t *speaker,
    uint32_t settle_ms) {
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    if (!speaker || !speaker->software_ref_ready ||
        !speaker->stereo_buffer || speaker->stereo_capacity == 0U ||
        settle_ms == 0U) {
        return;
    }
    const uint64_t frames64 =
        ((uint64_t)CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE * settle_ms + 999ULL) /
        1000ULL;
    size_t remaining = frames64 > SIZE_MAX ? SIZE_MAX : (size_t)frames64;
    while (remaining > 0U) {
        const size_t chunk = remaining < speaker->stereo_capacity
                                 ? remaining
                                 : speaker->stereo_capacity;
        /* The staging buffer may contain the previous PCM packet. Clear each
         * chunk before mirroring it into the reference ring. */
        memset(speaker->stereo_buffer,
               0,
               chunk * 2U * sizeof(int16_t));
        nuwax_p3_software_ref_write_interleaved_lr(
            speaker->stereo_buffer,
            chunk);
        remaining -= chunk;
    }
#else
    (void)speaker;
    (void)settle_ms;
#endif
}

static esp_err_t init_p3_software_ref_if_enabled(
    nuwax_speaker_output_t *speaker) {
    if (!speaker) {
        return ESP_ERR_INVALID_ARG;
    }
    speaker->software_ref_required =
        CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE &&
        CONFIG_NUWAX_LIVE_AEC_ENABLE;
    speaker->software_ref_ready = false;
#if CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    esp_err_t err = nuwax_p3_software_ref_init(CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE,
                                               CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "P3-0S software ref init failed required=%d: %s",
                 speaker->software_ref_required ? 1 : 0,
                 esp_err_to_name(err));
        return err;
    }
    speaker->software_ref_ready = true;
#endif
    return ESP_OK;
}

esp_err_t nuwax_speaker_output_init(nuwax_speaker_output_t *speaker) {
    if (!speaker) {
        return ESP_ERR_INVALID_ARG;
    }
    if (speaker->initialized && speaker->tx_handle && speaker->lock &&
        (!speaker->vocat_shared_tx ||
         (speaker->board_ref.token && speaker->board_lease.token))) {
        return ESP_OK;
    }

    /* A failed stop/re-enable or an interrupted init may leave ownership
     * fields populated while initialized is false.  Never discard those
     * fields with memset: release the channel, mutex, and staging buffer first
     * so a retry cannot leak DMA resources. */
    if (speaker->tx_handle || speaker->lock || speaker->stereo_buffer ||
        speaker->stereo_capacity || speaker->initialized ||
        speaker->vocat_shared_tx) {
        const esp_err_t cleanup_err = nuwax_speaker_output_deinit(speaker);
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "cannot reinitialize speaker with stale resources: %s",
                     esp_err_to_name(cleanup_err));
            return cleanup_err;
        }
    }
    memset(speaker, 0, sizeof(*speaker));

    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (!lock) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    /* P2: Miaopan V2.0 plays through ES8311 on the shared single-I2S bus.
     * Do NOT create a second (port-1) I2S channel; that targets the wrong
     * hardware and fails with ESP_ERR_NO_MEM once the shared codec holds DMA.
     * Acquire shared TX with the codec running and PA held off. */
    {
        esp_err_t board_err = nuwax_audio_board_init(&speaker->board_ref);
        if (board_err != ESP_OK) {
            vSemaphoreDelete(lock);
            ESP_LOGE(TAG, "vocat audio board init failed: %s", esp_err_to_name(board_err));
            return board_err;
        }
        void *tx = NULL;
        esp_err_t acq = nuwax_audio_board_acquire_tx(&tx, &speaker->board_lease);
        if (acq != ESP_OK || !tx) {
            const esp_err_t board_cleanup_err = nuwax_audio_board_deinit(&speaker->board_ref);
            vSemaphoreDelete(lock);
            ESP_LOGE(TAG, "vocat shared TX acquire failed: %s", esp_err_to_name(acq));
            if (board_cleanup_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "vocat board cleanup after TX acquire failure failed: %s",
                         esp_err_to_name(board_cleanup_err));
            }
            return acq == ESP_OK ?
                       (board_cleanup_err != ESP_OK ? board_cleanup_err
                                                    : ESP_ERR_INVALID_STATE) :
                   acq;
        }
        speaker->tx_handle = tx;          /* shared: released, not deleted, in deinit */
        speaker->lock = lock;
        speaker->vocat_shared_tx = true;
        speaker->volume_percent = clamp_volume_percent(CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT);
        speaker->initialized = true;
        speaker->muted = false;
        speaker->stop_settled = false;
        const esp_err_t ref_err = init_p3_software_ref_if_enabled(speaker);
        if (ref_err != ESP_OK && speaker->software_ref_required) {
            const esp_err_t cleanup_err = nuwax_speaker_output_deinit(speaker);
            if (cleanup_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "speaker cleanup after required software-ref failure failed: %s",
                         esp_err_to_name(cleanup_err));
            }
            return ref_err;
        }
        /* Prove the complete DMA ring silent before any first PCM can open
         * PA. This also covers reacquiring TX while a microphone board ref
         * kept the codec alive after an earlier playback owner released it. */
        const esp_err_t quiet_err = nuwax_speaker_output_stop(speaker);
        if (quiet_err != ESP_OK) {
            const esp_err_t cleanup_err = nuwax_speaker_output_deinit(speaker);
            if (cleanup_err != ESP_OK) {
                ESP_LOGE(TAG, "speaker cleanup after quiet init failed: %s",
                         esp_err_to_name(cleanup_err));
            }
            return quiet_err;
        }
        ESP_LOGI(TAG,
                 "I2S speaker (vocat shared TX via ES8311) ready sample_rate=%d volume=%u%% software_ref=%s startup_fade_ms=%u revision=startup-fade-v1",
                 CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE,
                 (unsigned)speaker->volume_percent,
                 speaker->software_ref_ready ? "ready" : "degraded",
                 (unsigned)NUWAX_SPEAKER_STARTUP_FADE_MS);
        return ESP_OK;
    }
#else
    esp_err_t err = ESP_OK;
#if CONFIG_NUWAX_SPEAKER_SD_GPIO >= 0
    gpio_config_t sd_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_NUWAX_SPEAKER_SD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&sd_cfg);
    if (err != ESP_OK) {
        vSemaphoreDelete(lock);
        ESP_LOGE(TAG, "configure speaker SD/EN GPIO%d failed: %s",
                 CONFIG_NUWAX_SPEAKER_SD_GPIO,
                 esp_err_to_name(err));
        return err;
    }
    /* Keep the PA muted until the first successful write. This also prevents
     * an init failure after GPIO setup from leaving the amplifier enabled. */
    err = gpio_set_level(CONFIG_NUWAX_SPEAKER_SD_GPIO, 0);
    if (err != ESP_OK) {
        vSemaphoreDelete(lock);
        ESP_LOGE(TAG,
                 "mute speaker SD/EN GPIO%d during init failed: %s",
                 CONFIG_NUWAX_SPEAKER_SD_GPIO,
                 esp_err_to_name(err));
        return err;
    }
#endif

    i2s_chan_handle_t tx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_NUWAX_SPEAKER_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CONFIG_NUWAX_SPEAKER_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = CONFIG_NUWAX_SPEAKER_DMA_FRAME_NUM;

    ESP_LOGI(TAG,
             "I2S speaker DMA config port=%d desc=%d frame=%d dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u",
             CONFIG_NUWAX_SPEAKER_I2S_PORT,
             CONFIG_NUWAX_SPEAKER_DMA_DESC_NUM,
             CONFIG_NUWAX_SPEAKER_DMA_FRAME_NUM,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    err = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (err != ESP_OK) {
        vSemaphoreDelete(lock);
        ESP_LOGE(TAG, "create I2S TX channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_NUWAX_SPEAKER_I2S_BCLK_GPIO,
            .ws = CONFIG_NUWAX_SPEAKER_I2S_WS_GPIO,
            .dout = CONFIG_NUWAX_SPEAKER_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    for (int attempt = 0; attempt <= NUWAX_SPEAKER_INIT_RETRIES; attempt++) {
        err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG,
                 "I2S speaker std init failed attempt=%d err=%s dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u",
                 attempt + 1,
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        char result_detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
        snprintf(result_detail,
                 sizeof(result_detail),
                 "%s;attempt=%d;dma=%u/%u;internal=%u/%u",
                 esp_err_to_name(err),
                 attempt + 1,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        (void)nuwax_device_warning_report_enqueue_detail(
            "speaker",
            "i2s_init_retry",
            err,
            result_detail,
            "speaker_init",
            0U,
            NULL);
        vTaskDelay(pdMS_TO_TICKS(NUWAX_SPEAKER_INIT_RETRY_DELAY_MS));
    }
    if (err != ESP_OK) {
        i2s_del_channel(tx_handle);
        vSemaphoreDelete(lock);
        return err;
    }

    err = i2s_channel_enable(tx_handle);
    if (err != ESP_OK) {
        i2s_del_channel(tx_handle);
        vSemaphoreDelete(lock);
        return err;
    }

    speaker->tx_handle = tx_handle;
    speaker->lock = lock;
    speaker->volume_percent = clamp_volume_percent(CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT);
    speaker->initialized = true;
    speaker->stop_settled = false;
#if CONFIG_NUWAX_SPEAKER_SD_GPIO >= 0
    speaker->muted = true;
#else
    speaker->muted = false;
#endif
    const esp_err_t ref_err = init_p3_software_ref_if_enabled(speaker);
    if (ref_err != ESP_OK && speaker->software_ref_required) {
        const esp_err_t cleanup_err = nuwax_speaker_output_deinit(speaker);
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "speaker cleanup after required software-ref failure failed: %s",
                     esp_err_to_name(cleanup_err));
        }
        return ref_err;
    }
    ESP_LOGI(TAG,
             "I2S speaker initialized port=%d bclk=%d ws=%d dout=%d sd=%d sample_rate=%d slot=stereo_both volume=%u%% software_ref=%s",
             CONFIG_NUWAX_SPEAKER_I2S_PORT,
             CONFIG_NUWAX_SPEAKER_I2S_BCLK_GPIO,
             CONFIG_NUWAX_SPEAKER_I2S_WS_GPIO,
             CONFIG_NUWAX_SPEAKER_I2S_DOUT_GPIO,
             CONFIG_NUWAX_SPEAKER_SD_GPIO,
             CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE,
             (unsigned)speaker->volume_percent,
             speaker->software_ref_ready ? "ready" : "degraded");
    return ESP_OK;
#endif /* CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0 */
}

esp_err_t nuwax_speaker_output_set_volume_percent(nuwax_speaker_output_t *speaker,
                                                  uint8_t volume_percent) {
    if (!speaker) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t next = clamp_volume_percent(volume_percent);
    SemaphoreHandle_t lock = (SemaphoreHandle_t)speaker->lock;
    if (!speaker_output_lock(lock, NUWAX_SPEAKER_OUTPUT_LOCK_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "speaker volume update skipped: output lock timeout");
        (void)nuwax_device_warning_report_enqueue(
            "speaker", "volume_lock_timeout", ESP_ERR_TIMEOUT,
            "speaking", 0U, NULL);
        return ESP_ERR_TIMEOUT;
    }
    speaker->volume_percent = next;
    if (lock) {
        xSemaphoreGive(lock);
    }
    return ESP_OK;
}

uint8_t nuwax_speaker_output_get_volume_percent(const nuwax_speaker_output_t *speaker) {
    if (!speaker) {
        return clamp_volume_percent(CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT);
    }
    return clamp_volume_percent(speaker->volume_percent);
}

bool nuwax_speaker_output_software_ref_ready(
    const nuwax_speaker_output_t *speaker) {
    return speaker && speaker->software_ref_ready;
}

esp_err_t nuwax_speaker_output_write_pcm16(nuwax_speaker_output_t *speaker,
                                           const int16_t *samples,
                                           size_t sample_count,
                                           uint32_t timeout_ms) {
    if (!speaker || !speaker->initialized || !speaker->tx_handle ||
        !speaker->lock || !samples) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_count == 0U) {
        return ESP_OK;
    }
    if (sample_count > SIZE_MAX / (2U * sizeof(int16_t))) {
        return ESP_ERR_INVALID_SIZE;
    }
    SemaphoreHandle_t lock = (SemaphoreHandle_t)speaker->lock;
    if (!speaker_output_lock(lock, timeout_ms)) {
        ESP_LOGE(TAG,
                 "I2S speaker write lock timeout sample_count=%u timeout_ms=%u",
                 (unsigned)sample_count,
                 (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    if (speaker->vocat_shared_tx && speaker->shared_tx_recovery_required) {
        ESP_LOGE(TAG,
                 "shared speaker write rejected until stop/DMA-silence recovery succeeds");
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_count > speaker->stereo_capacity) {
        int16_t *next = (int16_t *)heap_caps_realloc(speaker->stereo_buffer,
                                                     sample_count * 2 * sizeof(int16_t),
                                                     MALLOC_CAP_8BIT);
        if (!next) {
            if (lock) {
                xSemaphoreGive(lock);
            }
            return ESP_ERR_NO_MEM;
        }
        speaker->stereo_buffer = next;
        speaker->stereo_capacity = sample_count;
    }
    const uint8_t volume_percent = clamp_volume_percent(speaker->volume_percent);
    const uint32_t fade_start = speaker->muted ? 0U :
        (speaker->startup_fade_position < NUWAX_SPEAKER_STARTUP_FADE_FRAMES
             ? speaker->startup_fade_position : NUWAX_SPEAKER_STARTUP_FADE_FRAMES);
    uint32_t fade_position = fade_start;
    size_t leading_silence = 0U;
    for (size_t i = 0; i < sample_count; i++) {
        int16_t scaled = samples[i];
        if (volume_percent != 100) {
            scaled = (int16_t)(((int32_t)samples[i] * (int32_t)volume_percent) / 100);
        }
        if (fade_position < NUWAX_SPEAKER_STARTUP_FADE_FRAMES) {
            if (fade_position == 0U && scaled == 0) {
                ++leading_silence;
            } else {
                scaled = (int16_t)(((int64_t)scaled * speaker_output_startup_gain_q15(fade_position)) /
                                   NUWAX_SPEAKER_GAIN_ONE_Q15);
                ++fade_position;
            }
        }
        speaker->stereo_buffer[i * 2] = scaled;
        speaker->stereo_buffer[i * 2 + 1] = scaled;
    }
    /* Allocate/prepare PCM while still muted. With the DMA ring already
     * silent, let PA settle BEFORE submitting the first speech sample; doing
     * this after the write would play the first syllable into a muted PA. */
    speaker->stop_settled = false;
    if (speaker->vocat_shared_tx && speaker->muted) {
        speaker->shared_tx_recovery_required = true;
        const esp_err_t prepare_err =
            nuwax_audio_board_prepare_speaker_playback();
        if (prepare_err != ESP_OK) {
            xSemaphoreGive(lock);
            return prepare_err;
        }
        speaker->muted = false;
        speaker->shared_tx_recovery_required = false;
    }
#if CONFIG_NUWAX_SPEAKER_SD_GPIO >= 0
    if (!speaker->vocat_shared_tx && speaker->muted) {
        const esp_err_t unmute_err =
            gpio_set_level(CONFIG_NUWAX_SPEAKER_SD_GPIO, 1);
        if (unmute_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "speaker amplifier unmute failed: %s",
                     esp_err_to_name(unmute_err));
            if (lock) {
                xSemaphoreGive(lock);
            }
            return unmute_err;
        }
        speaker->muted = false;
    }
#endif
    const size_t stereo_frame_bytes = 2U * sizeof(int16_t);
    const size_t requested_bytes = sample_count * stereo_frame_bytes;
    const size_t dma_chunk_frames = speaker_output_dma_chunk_frames(speaker);
    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    size_t frames_written_total = 0U;
    size_t bytes_written = 0U;
    esp_err_t err = ESP_OK;
    if (dma_chunk_frames == 0U) {
        err = ESP_ERR_INVALID_SIZE;
    }
    while (err == ESP_OK && frames_written_total < sample_count) {
        size_t chunk_frames = sample_count - frames_written_total;
        if (chunk_frames > dma_chunk_frames) {
            chunk_frames = dma_chunk_frames;
        }
        uint32_t chunk_timeout_ms = 0U;
        if (timeout_ms > 0U) {
            const int64_t remaining_us = deadline_us - esp_timer_get_time();
            if (remaining_us <= 0) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            const uint64_t rounded_ms =
                ((uint64_t)remaining_us + 999ULL) / 1000ULL;
            chunk_timeout_ms = rounded_ms > UINT32_MAX
                                   ? UINT32_MAX
                                   : (uint32_t)rounded_ms;
        }

        const size_t chunk_bytes = chunk_frames * stereo_frame_bytes;
        size_t chunk_bytes_written = 0U;
        esp_err_t chunk_err = i2s_channel_write(
            (i2s_chan_handle_t)speaker->tx_handle,
            &speaker->stereo_buffer[frames_written_total * 2U],
            chunk_bytes,
            &chunk_bytes_written,
            chunk_timeout_ms);
        if (chunk_bytes_written > chunk_bytes) {
            chunk_bytes_written = chunk_bytes;
            if (chunk_err == ESP_OK) {
                chunk_err = ESP_ERR_INVALID_SIZE;
            }
        }
        const size_t complete_frames =
            chunk_bytes_written / stereo_frame_bytes;
        if (complete_frames > 0U && speaker->software_ref_ready) {
            /* A timed-out chunk may still have reached the codec partially.
             * Mirror every complete physical frame, and no uncommitted tail. */
            nuwax_p3_software_ref_write_interleaved_lr(
                &speaker->stereo_buffer[frames_written_total * 2U],
                complete_frames);
        }
        frames_written_total += complete_frames;
        bytes_written += chunk_bytes_written;
        if ((chunk_bytes_written % stereo_frame_bytes) != 0U) {
            ESP_LOGE(TAG,
                     "I2S speaker write ended on partial stereo frame bytes=%u frame_bytes=%u",
                     (unsigned)chunk_bytes_written,
                     (unsigned)stereo_frame_bytes);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (chunk_err != ESP_OK) {
            err = chunk_err;
            break;
        }
        if (chunk_bytes_written != chunk_bytes) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
    }
    if (bytes_written != requested_bytes) {
        ESP_LOGE(TAG,
                 "I2S speaker short write bytes=%u expected=%u",
                 (unsigned)bytes_written,
                 (unsigned)requested_bytes);
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    /* Preparation/PA failures return before here. A partial I2S write may
     * consume only a prefix; never advance the envelope for unsent samples. */
    const size_t fade_written = frames_written_total > leading_silence
                                   ? frames_written_total - leading_silence : 0U;
    speaker->startup_fade_position =
        fade_written >= NUWAX_SPEAKER_STARTUP_FADE_FRAMES - fade_start
            ? NUWAX_SPEAKER_STARTUP_FADE_FRAMES : fade_start + (uint32_t)fade_written;
#if CONFIG_NUWAX_SPEAKER_SD_GPIO >= 0
    if (err != ESP_OK && !speaker->vocat_shared_tx) {
        /* A failed/short write can leave stale samples in the DMA ring. Mute
         * the discrete PA before returning so the caller can retry or tear
         * down the channel without exposing that stale audio. */
        const esp_err_t mute_err =
            gpio_set_level(CONFIG_NUWAX_SPEAKER_SD_GPIO, 0);
        if (mute_err == ESP_OK) {
            speaker->muted = true;
        } else {
            ESP_LOGE(TAG,
                     "speaker amplifier mute after write failure failed: %s",
                     esp_err_to_name(mute_err));
        }
    }
#endif
    if (err != ESP_OK && speaker->vocat_shared_tx) {
        speaker->shared_tx_recovery_required = true;
        const esp_err_t mute_err = nuwax_audio_board_force_speaker_mute();
        if (mute_err == ESP_OK) {
            speaker->muted = true;
        } else {
            ESP_LOGE(TAG,
                     "shared speaker physical mute after write failure failed: %s",
                     esp_err_to_name(mute_err));
        }
    }
    if (lock) {
        xSemaphoreGive(lock);
    }
    return err;
}

esp_err_t nuwax_speaker_output_drain_and_stop(
    nuwax_speaker_output_t *speaker,
    uint32_t timeout_ms) {
    if (!speaker || !speaker->initialized || !speaker->tx_handle ||
        !speaker->lock) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t chunk_capacity = speaker_output_dma_chunk_frames(speaker);
    const size_t drain_frames = speaker_output_dma_capacity_frames(speaker);
    if (chunk_capacity == 0U || drain_frames == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    int16_t silence[(CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM >
                     CONFIG_NUWAX_SPEAKER_DMA_FRAME_NUM)
                        ? CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM
                        : CONFIG_NUWAX_SPEAKER_DMA_FRAME_NUM] = {0};
    const int64_t started_us = esp_timer_get_time();
    const int64_t deadline_us =
        started_us + (int64_t)timeout_ms * 1000LL;
    size_t drained_frames = 0U;
    esp_err_t drain_err = ESP_OK;
    while (drained_frames < drain_frames) {
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (timeout_ms == 0U || remaining_us <= 0) {
            drain_err = ESP_ERR_TIMEOUT;
            break;
        }
        size_t chunk_frames = drain_frames - drained_frames;
        if (chunk_frames > chunk_capacity) {
            chunk_frames = chunk_capacity;
        }
        const uint64_t rounded_ms =
            ((uint64_t)remaining_us + 999ULL) / 1000ULL;
        const uint32_t chunk_timeout_ms = rounded_ms > UINT32_MAX
                                              ? UINT32_MAX
                                              : (uint32_t)rounded_ms;
        drain_err = nuwax_speaker_output_write_pcm16(speaker,
                                                     silence,
                                                     chunk_frames,
                                                     chunk_timeout_ms);
        if (drain_err != ESP_OK) {
            break;
        }
        drained_frames += chunk_frames;
    }

    const esp_err_t stop_err = nuwax_speaker_output_stop(speaker);
    const uint32_t elapsed =
        (uint32_t)((esp_timer_get_time() - started_us) / 1000LL);
    if (drain_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "speaker natural drain failed frames=%u/%u timeout_ms=%u elapsed_ms=%u result=%s fallback_stop=%s",
                 (unsigned)drained_frames,
                 (unsigned)drain_frames,
                 (unsigned)timeout_ms,
                 (unsigned)elapsed,
                 esp_err_to_name(drain_err),
                 esp_err_to_name(stop_err));
        return stop_err != ESP_OK ? stop_err : drain_err;
    }
    if (stop_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "speaker natural drain stop failed frames=%u elapsed_ms=%u result=%s",
                 (unsigned)drained_frames,
                 (unsigned)elapsed,
                 esp_err_to_name(stop_err));
        return stop_err;
    }
    ESP_LOGI(TAG,
             "speaker natural drain complete frames=%u audio_ms=%u elapsed_ms=%u",
             (unsigned)drained_frames,
             (unsigned)(((uint64_t)drained_frames * 1000ULL +
                         CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE - 1U) /
                        CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE),
             (unsigned)elapsed);
    return ESP_OK;
}

esp_err_t nuwax_speaker_output_stop(nuwax_speaker_output_t *speaker) {
    if (!speaker || !speaker->initialized || !speaker->tx_handle ||
        !speaker->lock) {
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t lock = (SemaphoreHandle_t)speaker->lock;
    if (!speaker_output_lock(lock, NUWAX_SPEAKER_STOP_LOCK_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "I2S speaker stop lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (speaker->stop_settled &&
        !speaker->shared_tx_recovery_required) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }
    speaker->stop_settled = false;
    i2s_chan_handle_t tx_handle = (i2s_chan_handle_t)speaker->tx_handle;

    if (speaker->vocat_shared_tx) {
        /* PA must be physically quiet before resetting shared TX. RX stays
         * enabled; only the existing TX DMA-silence sequence follows. */
        speaker->shared_tx_recovery_required = true;
        const esp_err_t mute_err = nuwax_audio_board_force_speaker_mute();
        if (mute_err != ESP_OK) {
            xSemaphoreGive(lock);
            return mute_err;
        }
        speaker->muted = true;
    }

#if CONFIG_NUWAX_SPEAKER_SD_GPIO >= 0
    if (!speaker->vocat_shared_tx) {
        const esp_err_t mute_err =
            gpio_set_level(CONFIG_NUWAX_SPEAKER_SD_GPIO, 0);
        if (mute_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "speaker amplifier mute failed: %s",
                     esp_err_to_name(mute_err));
            if (lock) {
                xSemaphoreGive(lock);
            }
            return mute_err;
        }
        speaker->muted = true;
    }
#endif

    esp_err_t err = i2s_channel_disable(tx_handle);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* The amplifier was muted before touching I2S. Keep it muted on
         * failure; re-enabling PA here would expose stale DMA audio while the
         * software state still reports muted=true. */
        if (speaker->vocat_shared_tx &&
            nuwax_audio_board_force_speaker_mute() == ESP_OK) {
            speaker->muted = true;
            speaker->shared_tx_recovery_required = true;
        }
        if (lock) {
            xSemaphoreGive(lock);
        }
        return err;
    }

    size_t preload_bytes = 0;
    esp_err_t preload_err = ESP_OK;
    const size_t silence_samples =
        speaker_output_stop_silence_samples(speaker);
    const uint32_t settle_ms =
        speaker_output_stop_settle_ms(speaker, silence_samples);
    if (silence_samples > 0) {
        if (silence_samples > SIZE_MAX / (2U * sizeof(int16_t))) {
            preload_err = ESP_ERR_INVALID_SIZE;
        } else if (silence_samples > speaker->stereo_capacity) {
            int16_t *next = (int16_t *)heap_caps_realloc(speaker->stereo_buffer,
                                                         silence_samples * 2 * sizeof(int16_t),
                                                         MALLOC_CAP_8BIT);
            if (next) {
                speaker->stereo_buffer = next;
                speaker->stereo_capacity = silence_samples;
            } else {
                preload_err = ESP_ERR_NO_MEM;
                ESP_LOGE(TAG, "speaker stop silence buffer allocation failed samples=%u",
                         (unsigned)silence_samples);
            }
        }
        if (preload_err == ESP_OK &&
            speaker->stereo_buffer && silence_samples <= speaker->stereo_capacity) {
            memset(speaker->stereo_buffer, 0, silence_samples * 2 * sizeof(int16_t));
            preload_err = i2s_channel_preload_data(tx_handle,
                                                   speaker->stereo_buffer,
                                                   silence_samples * 2 * sizeof(int16_t),
                                                   &preload_bytes);
            if (preload_err != ESP_OK) {
                ESP_LOGE(TAG, "speaker stop silence preload failed: %s", esp_err_to_name(preload_err));
            }
            const size_t stereo_frame_bytes = 2U * sizeof(int16_t);
            const size_t requested_preload_bytes =
                silence_samples * stereo_frame_bytes;
            if ((preload_bytes % stereo_frame_bytes) != 0U) {
                ESP_LOGE(TAG,
                         "speaker stop silence preload partial stereo frame bytes=%u frame_bytes=%u",
                         (unsigned)preload_bytes,
                         (unsigned)stereo_frame_bytes);
                if (preload_err == ESP_OK) {
                    preload_err = ESP_ERR_INVALID_SIZE;
                }
            }
            if (preload_bytes != requested_preload_bytes) {
                ESP_LOGE(TAG,
                         "speaker stop silence preload incomplete bytes=%u expected=%u",
                         (unsigned)preload_bytes,
                         (unsigned)requested_preload_bytes);
                if (preload_err == ESP_OK) {
                    preload_err = ESP_ERR_INVALID_SIZE;
                }
            }
        }
    }

    if (preload_err != ESP_OK && speaker->vocat_shared_tx) {
        const esp_err_t mute_err = nuwax_audio_board_force_speaker_mute();
        if (mute_err == ESP_OK) {
            speaker->muted = true;
            speaker->shared_tx_recovery_required = true;
        }
        if (lock) {
            xSemaphoreGive(lock);
        }
        ESP_LOGE(TAG,
                 "speaker shared-TX stop left channel disabled; PA force-off=%s silence_error=%s",
                 esp_err_to_name(mute_err),
                 esp_err_to_name(preload_err));
        return mute_err != ESP_OK ? mute_err : preload_err;
    }

    err = i2s_channel_enable(tx_handle);
    if (err != ESP_OK) {
        /* Keep the channel, buffer, and mutex owned by this object.  The
         * caller records the stop fault and may retry or deinitialize it;
         * marking initialized=false here would make the next init memset the
         * live handles and leak the I2S channel/mutex. The PA also remains
         * muted so hardware and speaker->muted cannot diverge. */
        if (speaker->vocat_shared_tx &&
            nuwax_audio_board_force_speaker_mute() == ESP_OK) {
            speaker->muted = true;
            speaker->shared_tx_recovery_required = true;
        }
        if (lock) {
            xSemaphoreGive(lock);
        }
        return err;
    }

    if (settle_ms > 0U) {
        /* `preload_err == ESP_OK` means the complete zero preload was accepted
         * by the driver. For a discrete PA, mute was asserted before the I2S
         * operation; for shared TX, the full descriptor ring was verified
         * above. Only this proven-silent path may advance the AEC reference. */
        if (preload_err == ESP_OK) {
            speaker_output_write_ref_silence(speaker, settle_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
    }

    if (speaker->vocat_shared_tx) {
        speaker->shared_tx_recovery_required = false;
    }
    if (preload_err == ESP_OK) {
        speaker->stop_settled = true;
        speaker->startup_fade_position = 0U;
    }

    if (lock) {
        xSemaphoreGive(lock);
    }
    ESP_LOGI(TAG,
             "I2S speaker stopped, DMA reset, quieting=%s silence_preload_bytes=%u settle_ms=%u preload_result=%s ref_timeline=%s",
             speaker->vocat_shared_tx ? "pa_muted_dma_silence" : "pa_muted",
             (unsigned)preload_bytes,
             (unsigned)settle_ms,
             esp_err_to_name(preload_err),
             preload_err == ESP_OK ? "advanced" : "held");
    return preload_err;
}

esp_err_t nuwax_speaker_output_deinit(nuwax_speaker_output_t *speaker) {
    if (!speaker) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!speaker->tx_handle && !speaker->lock && !speaker->stereo_buffer &&
        speaker->stereo_capacity == 0U && !speaker->vocat_shared_tx) {
        return ESP_OK;
    }
    SemaphoreHandle_t lock = (SemaphoreHandle_t)speaker->lock;
    if (!speaker_output_lock(lock, NUWAX_SPEAKER_OUTPUT_LOCK_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "I2S speaker deinit lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    int16_t *stereo_buffer = speaker->stereo_buffer;
    i2s_chan_handle_t tx_handle = (i2s_chan_handle_t)speaker->tx_handle;
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    if (speaker->vocat_shared_tx) {
        /* shared TX owned by audio_board: release + drop refcount, never delete/disable
         * (the codec/board owns the channel lifecycle). */
        speaker->initialized = false;
        speaker->software_ref_ready = false;
        if (speaker->board_lease.token != 0) {
            const esp_err_t release_err =
                nuwax_audio_board_release(&speaker->board_lease);
            if (release_err != ESP_OK) {
                if (lock) {
                    xSemaphoreGive(lock);
                }
                return release_err;
            }
        }
        const esp_err_t board_err = nuwax_audio_board_deinit(&speaker->board_ref);
        if (speaker->board_ref.token != 0) {
            /* Logical release was rejected: retain the cleanup handle, but
             * never present partially released output as ready for playback. */
            if (lock) {
                xSemaphoreGive(lock);
            }
            return board_err;
        }
        if (lock) {
            xSemaphoreGive(lock);
            vSemaphoreDelete(lock);
        }
        if (stereo_buffer) {
            heap_caps_free(stereo_buffer);
        }
        memset(speaker, 0, sizeof(*speaker));
        return board_err;
    }
#endif
    esp_err_t first_err = ESP_OK;
#if CONFIG_NUWAX_SPEAKER_SD_GPIO >= 0
    const esp_err_t mute_err =
        gpio_set_level(CONFIG_NUWAX_SPEAKER_SD_GPIO, 0);
    if (mute_err == ESP_OK) {
        speaker->muted = true;
    } else {
        first_err = mute_err;
        ESP_LOGE(TAG,
                 "speaker amplifier mute during deinit failed: %s",
                 esp_err_to_name(mute_err));
    }
#endif
    if (speaker->initialized && tx_handle) {
        const esp_err_t disable_err = i2s_channel_disable(tx_handle);
        if (disable_err != ESP_OK && disable_err != ESP_ERR_INVALID_STATE) {
            if (first_err == ESP_OK) {
                first_err = disable_err;
            }
            ESP_LOGE(TAG, "I2S speaker disable during deinit failed: %s",
                     esp_err_to_name(disable_err));
        }
    }
    if (tx_handle) {
        const esp_err_t delete_err = i2s_del_channel(tx_handle);
        if (delete_err != ESP_OK) {
            if (first_err == ESP_OK) {
                first_err = delete_err;
            }
            ESP_LOGE(TAG, "I2S speaker channel delete failed: %s",
                     esp_err_to_name(delete_err));
        }
        if (delete_err != ESP_OK) {
            /* Keep the ownership fields intact so a later deinit can retry;
             * deleting them here would leak an undeleted channel. */
            if (lock) {
                xSemaphoreGive(lock);
            }
            return first_err;
        }
    }
    if (lock) {
        xSemaphoreGive(lock);
        vSemaphoreDelete(lock);
    }
    if (stereo_buffer) {
        heap_caps_free(stereo_buffer);
    }
    memset(speaker, 0, sizeof(*speaker));
    return first_err;
}
