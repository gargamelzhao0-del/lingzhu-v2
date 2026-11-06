#include "mic_input.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "device_error_report.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
#include "board/audio_board.h"
#define NUWAX_VOCAT_MIC_SLOTS 4 /* ES7210 4-slot TDM: slot0=MIC1(primary), slot1=MIC2, slot2=AEC-ref candidate, slot3=NC */
#ifndef CONFIG_NUWAX_VOCAT_MIC_INPUT_GAIN_Q15
#define CONFIG_NUWAX_VOCAT_MIC_INPUT_GAIN_Q15 32768
#endif
#endif

static const char *TAG = "nuwax_mic";

#ifndef CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE
#define CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_MIC_AUTO_ATTENUATE
#define CONFIG_NUWAX_MIC_AUTO_ATTENUATE 0
#endif

static SemaphoreHandle_t s_mic_owner_lock;
static StaticSemaphore_t s_mic_owner_lock_storage;
static portMUX_TYPE s_mic_owner_init_mux = portMUX_INITIALIZER_UNLOCKED;
static nuwax_mic_owner_t s_active_owner = NUWAX_MIC_OWNER_NONE;

static const char *owner_name(nuwax_mic_owner_t owner) {
    switch (owner) {
        case NUWAX_MIC_OWNER_NONE:
            return "none";
        case NUWAX_MIC_OWNER_UNSPECIFIED:
            return "unspecified";
        case NUWAX_MIC_OWNER_WAKE_DETECTOR:
            return "wake_detector";
        case NUWAX_MIC_OWNER_CONVERSATION_CAPTURE:
            return "conversation_capture";
        case NUWAX_MIC_OWNER_MEETING_CAPTURE:
            return "meeting_capture";
        case NUWAX_MIC_OWNER_MONITORING_CAPTURE:
            return "monitoring_capture";
        default:
            return "unknown";
    }
}

static void report_mic_fault(const char *code, esp_err_t result,
                             nuwax_mic_owner_t owner) {
    char detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
    snprintf(detail,
             sizeof(detail),
             "%s;owner=%s",
             esp_err_to_name(result),
             owner_name(owner));
    (void)nuwax_device_error_report_enqueue_detail(
        "error", "microphone", code, result, detail, owner_name(owner), 0U,
        NULL);
}

static void report_mic_warning(const char *code, esp_err_t result,
                               nuwax_mic_owner_t owner) {
    char detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
    snprintf(detail,
             sizeof(detail),
             "%s;owner=%s",
             esp_err_to_name(result),
             owner_name(owner));
    (void)nuwax_device_warning_report_enqueue_detail(
        "microphone", code, result, detail, owner_name(owner), 0U, NULL);
}

static void report_mic_overflow(const char *code,
                                esp_err_t result,
                                nuwax_mic_owner_t owner,
                                const char *phase,
                                uint32_t count,
                                uint32_t delta,
                                uint32_t events) {
    char detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
    snprintf(detail,
             sizeof(detail),
             "%s;owner=%s;phase=%s;count=%lu;delta=%lu;events=%lu",
             esp_err_to_name(result),
             owner_name(owner),
             phase ? phase : "unknown",
             (unsigned long)count,
             (unsigned long)delta,
             (unsigned long)events);
    if (strcmp(code, "dma_overflow") == 0) {
        (void)nuwax_device_error_report_enqueue_detail(
            "error", "microphone", code, result, detail, owner_name(owner),
            0U, NULL);
    } else {
        (void)nuwax_device_warning_report_enqueue_detail(
            "microphone", code, result, detail, owner_name(owner), 0U, NULL);
    }
}

static esp_err_t ensure_owner_lock(void) {
    if (s_mic_owner_lock) {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_mic_owner_init_mux);
    if (!s_mic_owner_lock) {
        s_mic_owner_lock = xSemaphoreCreateMutexStatic(&s_mic_owner_lock_storage);
    }
    taskEXIT_CRITICAL(&s_mic_owner_init_mux);

    return s_mic_owner_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t acquire_owner(nuwax_mic_input_t *mic, nuwax_mic_owner_t owner) {
    if (!mic) {
        return ESP_ERR_INVALID_ARG;
    }
    if (owner == NUWAX_MIC_OWNER_NONE) {
        owner = NUWAX_MIC_OWNER_UNSPECIFIED;
    }
    ESP_RETURN_ON_ERROR(ensure_owner_lock(), TAG, "create mic owner lock failed");
    if (xSemaphoreTake(s_mic_owner_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "I2S mic owner lock timeout requested=%s", owner_name(owner));
        report_mic_warning("owner_lock_timeout", ESP_ERR_TIMEOUT, owner);
        return ESP_ERR_TIMEOUT;
    }
    if (s_active_owner != NUWAX_MIC_OWNER_NONE) {
        ESP_LOGW(TAG,
                 "I2S mic owner busy requested=%s active=%s",
                 owner_name(owner),
                 owner_name(s_active_owner));
        report_mic_warning("owner_busy", ESP_ERR_INVALID_STATE, owner);
        xSemaphoreGive(s_mic_owner_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* Only initialize storage after exclusive ownership was admitted. A
     * rejected duplicate init must leave the live instance releasable. */
    memset(mic, 0, sizeof(*mic));
    s_active_owner = owner;
    mic->owner = owner;
    mic->owner_acquired = true;
    ESP_LOGI(TAG, "I2S mic owner acquired owner=%s", owner_name(owner));
    xSemaphoreGive(s_mic_owner_lock);
    return ESP_OK;
}

static void release_owner(nuwax_mic_input_t *mic) {
    if (!mic || !mic->owner_acquired || !s_mic_owner_lock) {
        return;
    }
    nuwax_mic_owner_t owner = mic->owner;
    if (xSemaphoreTake(s_mic_owner_lock, portMAX_DELAY) == pdTRUE) {
        if (s_active_owner == owner) {
            s_active_owner = NUWAX_MIC_OWNER_NONE;
            ESP_LOGI(TAG, "I2S mic owner released owner=%s", owner_name(owner));
        } else {
            ESP_LOGW(TAG,
                     "I2S mic owner release mismatch owner=%s active=%s",
                     owner_name(owner),
                     owner_name(s_active_owner));
            report_mic_warning("owner_release_mismatch", ESP_ERR_INVALID_STATE,
                               owner);
        }
        xSemaphoreGive(s_mic_owner_lock);
    }
    mic->owner = NUWAX_MIC_OWNER_NONE;
    mic->owner_acquired = false;
}

static void clear_raw_stats(nuwax_mic_input_t *mic) {
    if (!mic) {
        return;
    }
    memset(&mic->last_raw_stats, 0, sizeof(mic->last_raw_stats));
#if CONFIG_NUWAX_MIC_BITS_PER_SAMPLE_32
    mic->last_raw_stats.dc_block_enabled = CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE;
    mic->last_raw_stats.auto_attenuate_enabled = CONFIG_NUWAX_MIC_AUTO_ATTENUATE;
    mic->last_raw_stats.selected_shift = CONFIG_NUWAX_MIC_32BIT_SHIFT;
#endif
}

static bool is_i2s_read_error_usable(esp_err_t err, size_t bytes_read) {
    return err == ESP_OK || (err == ESP_ERR_TIMEOUT && bytes_read > 0);
}

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
#define NUWAX_MIC_RX_OVERFLOW_REPORT_INTERVAL_EVENTS 1000U
#define NUWAX_MIC_RX_STARTUP_DRAIN_READS 8U
#define NUWAX_MIC_RX_STARTUP_RESYNC_MAX 2U

#include "mic_dma_reader.inc"

static size_t mic_rx_drain_queued_audio(nuwax_mic_input_t *mic)
{
    if (!mic || !mic->rx_handle || !mic->raw_samples || mic->raw_capacity == 0U) {
        return 0U;
    }
    mic_dma_cache_reset(mic);
    size_t drained = 0U;
    size_t drain_read_bytes =
        (size_t)CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM *
        NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t);
    if (drain_read_bytes == 0U || drain_read_bytes > mic->raw_capacity) {
        drain_read_bytes = mic->raw_capacity;
    }
    for (uint32_t read = 0U; read < NUWAX_MIC_RX_STARTUP_DRAIN_READS; read++) {
        size_t bytes_read = 0U;
        const esp_err_t err = i2s_channel_read(
            (i2s_chan_handle_t)mic->rx_handle,
            mic->raw_samples,
            drain_read_bytes,
            &bytes_read,
            0U);
        drained += bytes_read;
        if (bytes_read == 0U || (err != ESP_OK && err != ESP_ERR_TIMEOUT)) {
            break;
        }
    }
    return drained;
}

static void mic_rx_overflow_note(nuwax_mic_input_t *mic,
                                 uint32_t current,
                                 const char *phase)
{
    mic_dma_cache_reset(mic);
    const uint32_t delta = current - mic->rx_overflow_count;
    mic->rx_overflow_events += delta;
    mic->rx_overflow_count = current;
    clear_raw_stats(mic);

    const bool report = !mic->rx_overflow_recovery_pending ||
                        (uint32_t)(mic->rx_overflow_events -
                                   mic->rx_overflow_last_report_event) >=
                            NUWAX_MIC_RX_OVERFLOW_REPORT_INTERVAL_EVENTS;
    if (report) {
        if (!mic->rx_audio_started &&
            mic->rx_startup_resyncs < NUWAX_MIC_RX_STARTUP_RESYNC_MAX) {
            ESP_LOGW(TAG,
                     "RX DMA startup overflow owner=%s phase=%s count=%lu delta=%lu events=%lu; bounded resync",
                     owner_name(mic->owner),
                     phase,
                     (unsigned long)current,
                     (unsigned long)delta,
                     (unsigned long)mic->rx_overflow_events);
            report_mic_overflow("dma_startup_overflow", ESP_ERR_TIMEOUT,
                                mic->owner, phase, current, delta,
                                mic->rx_overflow_events);
        } else {
            ESP_LOGE(TAG,
                     "RX DMA overflow owner=%s phase=%s count=%lu delta=%lu events=%lu; recovery read forced",
                     owner_name(mic->owner),
                     phase,
                     (unsigned long)current,
                     (unsigned long)delta,
                     (unsigned long)mic->rx_overflow_events);
            report_mic_overflow("dma_overflow", ESP_ERR_INVALID_STATE,
                                mic->owner, phase, current, delta,
                                mic->rx_overflow_events);
        }
        mic->rx_overflow_last_report_event = mic->rx_overflow_events;
    }
    mic->rx_overflow_recovery_pending = true;
}

static bool mic_rx_overflow_begin(nuwax_mic_input_t *mic,
                                  uint32_t *out_count)
{
    const uint32_t current = nuwax_audio_board_get_rx_overflow_count();
    const bool discontinuous = current != mic->rx_overflow_count;
    if (discontinuous) {
        mic_rx_overflow_note(mic, current, "before_read");
    }
    *out_count = current;
    return discontinuous;
}

static esp_err_t mic_rx_overflow_end(nuwax_mic_input_t *mic,
                                     size_t *samples_read,
                                     uint32_t before,
                                     bool discontinuous_before_read,
                                     bool read_usable)
{
    const uint32_t current = nuwax_audio_board_get_rx_overflow_count();
    const bool discontinuous_during_read = current != before;
    if (discontinuous_during_read) {
        mic_rx_overflow_note(mic, current, "during_read");
    }

    if (discontinuous_before_read || discontinuous_during_read) {
        mic_dma_cache_reset(mic);
        /* Always consume one DMA buffer before rejecting a discontinuity.
         * Rejecting before i2s_channel_read() caused a permanent overflow
         * loop because the consumer could never catch up with RX. */
        *samples_read = 0;
        clear_raw_stats(mic);
        if (!mic->rx_audio_started &&
            mic->rx_startup_resyncs < NUWAX_MIC_RX_STARTUP_RESYNC_MAX) {
            mic->rx_startup_resyncs++;
            const size_t drained = mic_rx_drain_queued_audio(mic);
            const uint32_t aligned =
                nuwax_audio_board_get_rx_overflow_count();
            if (aligned != mic->rx_overflow_count) {
                mic->rx_overflow_events += aligned - mic->rx_overflow_count;
                mic->rx_overflow_count = aligned;
            }
            ESP_LOGW(TAG,
                     "RX DMA startup overflow resynced owner=%s count=%lu events=%lu drained_bytes=%u resync=%lu/%u",
                     owner_name(mic->owner),
                     (unsigned long)mic->rx_overflow_count,
                     (unsigned long)mic->rx_overflow_events,
                     (unsigned)drained,
                     (unsigned long)mic->rx_startup_resyncs,
                     (unsigned)NUWAX_MIC_RX_STARTUP_RESYNC_MAX);
            report_mic_warning("dma_startup_resync", ESP_ERR_TIMEOUT,
                               mic->owner);
            return ESP_ERR_TIMEOUT;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (mic->rx_overflow_recovery_pending && read_usable) {
        mic->rx_overflow_recovery_pending = false;
        ESP_LOGI(TAG,
                 "RX DMA overflow recovery complete owner=%s count=%lu events=%lu",
                 owner_name(mic->owner),
                 (unsigned long)current,
                 (unsigned long)mic->rx_overflow_events);
    }
    return ESP_OK;
}
#endif

static int get_slot_mask(void) {
#if CONFIG_NUWAX_MIC_SLOT_LEFT
    return I2S_STD_SLOT_LEFT;
#elif CONFIG_NUWAX_MIC_SLOT_RIGHT
    return I2S_STD_SLOT_RIGHT;
#else
    return I2S_STD_SLOT_BOTH;
#endif
}

static const char *get_slot_name(void) {
#if CONFIG_NUWAX_MIC_SLOT_LEFT
    return "left";
#elif CONFIG_NUWAX_MIC_SLOT_RIGHT
    return "right";
#else
    return "both";
#endif
}

static i2s_data_bit_width_t get_data_bit_width(void) {
#if CONFIG_NUWAX_MIC_BITS_PER_SAMPLE_32
    return I2S_DATA_BIT_WIDTH_32BIT;
#else
    return I2S_DATA_BIT_WIDTH_16BIT;
#endif
}

static int16_t clamp_i32_to_i16(int32_t value) {
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
static int16_t apply_vocat_input_gain(int16_t sample) {
    const int32_t gain_q15 = CONFIG_NUWAX_VOCAT_MIC_INPUT_GAIN_Q15;
    if (gain_q15 == 32768) {
        return sample;
    }
    return clamp_i32_to_i16((int32_t)(((int64_t)sample * gain_q15) >> 15));
}
#endif

#if CONFIG_NUWAX_MIC_BITS_PER_SAMPLE_32
static uint32_t select_shift_for_peak(int64_t peak) {
    uint32_t shift = CONFIG_NUWAX_MIC_32BIT_SHIFT;
#if CONFIG_NUWAX_MIC_AUTO_ATTENUATE
    if (peak < 0) {
        peak = -peak;
    }
    while (shift < 24 && (peak >> shift) > INT16_MAX) {
        shift++;
    }
#else
    (void)peak;
#endif
    return shift;
}
#endif

esp_err_t nuwax_mic_input_init(nuwax_mic_input_t *mic) {
    return nuwax_mic_input_init_with_owner(mic, NUWAX_MIC_OWNER_UNSPECIFIED);
}

esp_err_t nuwax_mic_input_init_with_owner(nuwax_mic_input_t *mic, nuwax_mic_owner_t owner) {
    if (!mic) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t owner_err = acquire_owner(mic, owner);
    if (owner_err != ESP_OK) {
        return owner_err;
    }

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    /* P2-4: Miaopan V2.0 uses the shared single-I2S duplex bus. Do NOT create
     * an own I2S channel; acquire the ES7210 RX channel from audio_board and
     * read it as 4-slot TDM, downmixing to mono PCM16 for the existing ASR. */
    {
        esp_err_t board_err = nuwax_audio_board_init(&mic->board_ref);
        if (board_err != ESP_OK) {
            ESP_LOGE(TAG, "vocat audio board init failed: %s", esp_err_to_name(board_err));
            report_mic_fault("audio_board_init", board_err, owner);
            release_owner(mic);
            return board_err;
        }
        void *rx = NULL;
        esp_err_t acq = nuwax_audio_board_acquire_rx(&rx, &mic->board_lease);
        if (acq != ESP_OK || !rx) {
            ESP_LOGE(TAG, "vocat shared RX acquire failed: %s", esp_err_to_name(acq));
            report_mic_fault("rx_acquire", acq, owner);
            nuwax_audio_board_deinit(&mic->board_ref);
            release_owner(mic);
            return acq == ESP_OK ? ESP_ERR_INVALID_STATE : acq;
        }
        mic->raw_capacity = CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME *
                            NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t);
        mic->raw_samples = heap_caps_malloc(mic->raw_capacity + MIC_DMA_BLOCK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mic->raw_samples) {
            mic->raw_samples = heap_caps_malloc(mic->raw_capacity + MIC_DMA_BLOCK_BYTES, MALLOC_CAP_8BIT);
        }
        if (!mic->raw_samples) {
            (void)nuwax_audio_board_release(&mic->board_lease);
            nuwax_audio_board_deinit(&mic->board_ref);
            release_owner(mic);
            return ESP_ERR_NO_MEM;
        }
        mic->rx_handle = rx; /* shared: released, not deleted, in deinit */
        mic->vocat_shared_rx = true;
        const uint32_t overflow_before_drain =
            nuwax_audio_board_get_rx_overflow_count();
        const size_t startup_drain_bytes = mic_rx_drain_queued_audio(mic);
        mic->rx_overflow_count = nuwax_audio_board_get_rx_overflow_count();
        mic->initialized = true;
        ESP_LOGI(TAG,
                 "I2S mic (vocat shared RX) ready slots=%d downmix=mono owner=%s startup_drain_bytes=%u overflow=%lu->%lu",
                 NUWAX_VOCAT_MIC_SLOTS,
                 owner_name(owner),
                 (unsigned)startup_drain_bytes,
                 (unsigned long)overflow_before_drain,
                 (unsigned long)mic->rx_overflow_count);
        return ESP_OK;
    }
#else
    i2s_chan_handle_t rx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_NUWAX_MIC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CONFIG_NUWAX_MIC_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = CONFIG_NUWAX_MIC_DMA_FRAME_NUM;

    ESP_LOGI(TAG,
             "I2S mic DMA config port=%d desc=%d frame=%d dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u",
             CONFIG_NUWAX_MIC_I2S_PORT,
             CONFIG_NUWAX_MIC_DMA_DESC_NUM,
             CONFIG_NUWAX_MIC_DMA_FRAME_NUM,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (err != ESP_OK) {
        release_owner(mic);
        ESP_LOGE(TAG, "create I2S RX channel failed: %s", esp_err_to_name(err));
        report_mic_fault("rx_channel_create", err, owner);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_NUWAX_VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(get_data_bit_width(), I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_NUWAX_MIC_I2S_BCLK_GPIO,
            .ws = CONFIG_NUWAX_MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_NUWAX_MIC_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = get_slot_mask();

    err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "I2S mic std init failed err=%s dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        report_mic_fault("rx_std_init", err, owner);
        i2s_del_channel(rx_handle);
        release_owner(mic);
        return err;
    }

#if CONFIG_NUWAX_MIC_BITS_PER_SAMPLE_32
    mic->raw_capacity = CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME * sizeof(int32_t);
    mic->raw_samples = heap_caps_malloc(mic->raw_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mic->raw_samples) {
        mic->raw_samples = heap_caps_malloc(mic->raw_capacity, MALLOC_CAP_8BIT);
    }
    if (!mic->raw_samples) {
        i2s_del_channel(rx_handle);
        release_owner(mic);
        return ESP_ERR_NO_MEM;
    }
#endif

    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        heap_caps_free(mic->raw_samples);
        mic->raw_samples = NULL;
        i2s_del_channel(rx_handle);
        release_owner(mic);
        return err;
    }

    mic->rx_handle = rx_handle;
    mic->initialized = true;
    ESP_LOGI(TAG,
             "I2S mic initialized xiaozhi_compat=1 port=%d bclk=%d ws=%d din=%d sample_rate=%d slot=%s slot_mask=%d shift=%d dc_block=%d auto_attenuate=%d read_timeout_ms=%d",
             CONFIG_NUWAX_MIC_I2S_PORT,
             CONFIG_NUWAX_MIC_I2S_BCLK_GPIO,
             CONFIG_NUWAX_MIC_I2S_WS_GPIO,
             CONFIG_NUWAX_MIC_I2S_DIN_GPIO,
             CONFIG_NUWAX_VOICE_SAMPLE_RATE,
             get_slot_name(),
             (int)std_cfg.slot_cfg.slot_mask,
#if CONFIG_NUWAX_MIC_BITS_PER_SAMPLE_32
             CONFIG_NUWAX_MIC_32BIT_SHIFT,
#else
             0,
#endif
             CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE,
             CONFIG_NUWAX_MIC_AUTO_ATTENUATE,
             CONFIG_NUWAX_MIC_READ_TIMEOUT_MS);
    return ESP_OK;
#endif /* CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0 */
}

esp_err_t nuwax_mic_input_read_pcm16(nuwax_mic_input_t *mic,
                                     int16_t *samples,
                                     size_t sample_count,
                                     uint32_t timeout_ms,
                                     size_t *samples_read) {
    if (!mic || !mic->initialized || !mic->rx_handle || !samples || !samples_read) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    /* P2-4/P3-0: read interleaved 4-slot TDM, downmix slot0 (MIC1 primary) to mono PCM16.
     * Keeps the read_pcm16 mono contract so ASR / wake detector are unchanged. */
    if (mic->vocat_shared_rx) {
        if (sample_count == 0) {
            *samples_read = 0;
            return ESP_OK;
        }
        size_t want = sample_count;
        size_t cap_frames = mic->raw_capacity / (NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t));
        if (want > cap_frames) {
            want = cap_frames;
        }
        uint32_t overflow_before = 0;
        const bool discontinuous_before_read =
            mic_rx_overflow_begin(mic, &overflow_before);
        size_t bytes_read = 0;
        esp_err_t err = mic_read_dma_blocks(mic, want * NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t),
                                         timeout_ms, &bytes_read);
        esp_err_t continuity_err = mic_rx_overflow_end(mic,
                                                       samples_read,
                                                       overflow_before,
                                                       discontinuous_before_read,
                                                       is_i2s_read_error_usable(err,
                                                                                bytes_read));
        if (continuity_err != ESP_OK) {
            return continuity_err;
        }
        if (!is_i2s_read_error_usable(err, bytes_read)) {
            *samples_read = 0;
            clear_raw_stats(mic);
            return err;
        }
        const size_t frames = bytes_read / (NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t));
        const int16_t *tdm = (const int16_t *)mic->raw_samples;
        int32_t sum = 0, mn = 0, mx = 0;
        for (size_t f = 0; f < frames; f++) {
            int16_t v = apply_vocat_input_gain(tdm[f * NUWAX_VOCAT_MIC_SLOTS]); /* slot0 = MIC1 */
            samples[f] = v;
            if (f == 0) { mn = v; mx = v; }
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
        }
        const int32_t mean = frames > 0
                                 ? (int32_t)(sum / (int32_t)frames)
                                 : 0;
#if CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE
        mn = 0;
        mx = 0;
        for (size_t f = 0; f < frames; f++) {
            samples[f] = clamp_i32_to_i16((int32_t)samples[f] - mean);
            if (f == 0 || samples[f] < mn) {
                mn = samples[f];
            }
            if (f == 0 || samples[f] > mx) {
                mx = samples[f];
            }
        }
#endif
        nuwax_mic_raw_stats_t st = {0};
        st.sample_count = frames;
        st.dc_block_enabled = CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE;
        st.auto_attenuate_enabled = CONFIG_NUWAX_MIC_AUTO_ATTENUATE;
        if (frames > 0) {
            st.first = tdm[0];
            st.last = tdm[(frames - 1) * NUWAX_VOCAT_MIC_SLOTS];
            st.min = mn;
            st.max = mx;
            st.mean = mean;
            st.centered_peak = (mx > -mn) ? mx : -mn;
        }
        mic->last_raw_stats = st;
        *samples_read = frames;
        if (frames > 0U) {
            mic->rx_audio_started = true;
        }
        return ESP_OK;
    }
#endif

#if CONFIG_NUWAX_MIC_BITS_PER_SAMPLE_32
    if (sample_count * sizeof(int32_t) > mic->raw_capacity || !mic->raw_samples) {
        return ESP_ERR_NO_MEM;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read((i2s_chan_handle_t)mic->rx_handle,
                                     mic->raw_samples,
                                     sample_count * sizeof(int32_t),
                                     &bytes_read,
                                     timeout_ms);
    if (!is_i2s_read_error_usable(err, bytes_read)) {
        *samples_read = 0;
        clear_raw_stats(mic);
        return err;
    }

    const size_t raw_count = bytes_read / sizeof(int32_t);
    const int32_t *raw = (const int32_t *)mic->raw_samples;
    nuwax_mic_raw_stats_t stats = {0};
    stats.sample_count = raw_count;
    stats.dc_block_enabled = CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE;
    stats.auto_attenuate_enabled = CONFIG_NUWAX_MIC_AUTO_ATTENUATE;
    stats.selected_shift = CONFIG_NUWAX_MIC_32BIT_SHIFT;
    if (raw_count > 0) {
        int64_t sum = 0;
        stats.first = raw[0];
        stats.last = raw[raw_count - 1];
        stats.min = raw[0];
        stats.max = raw[0];
        for (size_t i = 0; i < raw_count; i++) {
            int32_t value = raw[i];
            if (value < stats.min) {
                stats.min = value;
            }
            if (value > stats.max) {
                stats.max = value;
            }
            if (value == 0) {
                stats.zero_count++;
            } else if (value > 0) {
                stats.positive_count++;
            } else {
                stats.negative_count++;
            }
            sum += value;
        }
        stats.mean = (int32_t)(sum / (int64_t)raw_count);
        int64_t centered_peak = 0;
        for (size_t i = 0; i < raw_count; i++) {
            int64_t centered = CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE ? ((int64_t)raw[i] - stats.mean) : raw[i];
            int64_t abs_centered = centered < 0 ? -centered : centered;
            if (abs_centered > centered_peak) {
                centered_peak = abs_centered;
            }
        }
        stats.centered_peak = centered_peak;
        stats.selected_shift = select_shift_for_peak(centered_peak);
    }
    mic->last_raw_stats = stats;

    for (size_t i = 0; i < raw_count; i++) {
        int64_t centered = CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE ? ((int64_t)raw[i] - stats.mean) : raw[i];
        int32_t converted = (int32_t)(centered >> stats.selected_shift);
        samples[i] = clamp_i32_to_i16(converted);
    }
    *samples_read = raw_count;
    return err == ESP_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_OK;
#else
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read((i2s_chan_handle_t)mic->rx_handle,
                                     samples,
                                     sample_count * sizeof(int16_t),
                                     &bytes_read,
                                     timeout_ms);
    if (!is_i2s_read_error_usable(err, bytes_read)) {
        *samples_read = 0;
        clear_raw_stats(mic);
        return err;
    }

    *samples_read = bytes_read / sizeof(int16_t);
    return err == ESP_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_OK;
#endif
}

esp_err_t nuwax_mic_input_read_vocat_mr_pcm16(nuwax_mic_input_t *mic,
                                              int16_t *mic_samples,
                                              int16_t *ref_samples,
                                              size_t sample_count,
                                              uint32_t timeout_ms,
                                              size_t *samples_read) {
    if (!mic || !mic->initialized || !mic->rx_handle || !mic_samples || !ref_samples || !samples_read) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    if (!mic->vocat_shared_rx) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sample_count == 0) {
        *samples_read = 0;
        return ESP_OK;
    }

    size_t want = sample_count;
    const size_t cap_frames = mic->raw_capacity / (NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t));
    if (want > cap_frames) {
        want = cap_frames;
    }
    uint32_t overflow_before = 0;
    const bool discontinuous_before_read =
        mic_rx_overflow_begin(mic, &overflow_before);
    size_t bytes_read = 0;
    esp_err_t err = mic_read_dma_blocks(mic, want * NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t),
                                         timeout_ms, &bytes_read);
    esp_err_t continuity_err = mic_rx_overflow_end(mic,
                                                   samples_read,
                                                   overflow_before,
                                                   discontinuous_before_read,
                                                   is_i2s_read_error_usable(err,
                                                                            bytes_read));
    if (continuity_err != ESP_OK) {
        return continuity_err;
    }
    if (!is_i2s_read_error_usable(err, bytes_read)) {
        *samples_read = 0;
        clear_raw_stats(mic);
        return err;
    }

    const size_t frames = bytes_read / (NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t));
    const int16_t *tdm = (const int16_t *)mic->raw_samples;
    int32_t sum = 0, mn = 0, mx = 0;
    for (size_t f = 0; f < frames; f++) {
        const size_t base = f * NUWAX_VOCAT_MIC_SLOTS;
        const int16_t mic_v = apply_vocat_input_gain(tdm[base]); /* slot0 = MIC1 primary */
        const int16_t ref_v = tdm[base + 2U]; /* slot2 = MIC3 hardware playback reference */
        mic_samples[f] = mic_v;
        ref_samples[f] = ref_v;
        if (f == 0) {
            mn = mic_v;
            mx = mic_v;
        }
        if (mic_v < mn) {
            mn = mic_v;
        }
        if (mic_v > mx) {
            mx = mic_v;
        }
        sum += mic_v;
    }

    nuwax_mic_raw_stats_t st = {0};
    st.sample_count = frames;
    if (frames > 0) {
        st.first = tdm[0];
        st.last = tdm[(frames - 1U) * NUWAX_VOCAT_MIC_SLOTS];
        st.min = mn;
        st.max = mx;
        st.mean = (int32_t)(sum / (int32_t)frames);
        st.centered_peak = (mx > -mn) ? mx : -mn;
    }
    mic->last_raw_stats = st;
    *samples_read = frames;
    if (frames > 0U) {
        mic->rx_audio_started = true;
    }
    return err == ESP_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_OK;
#else
    (void)mic;
    (void)mic_samples;
    (void)ref_samples;
    (void)sample_count;
    (void)timeout_ms;
    *samples_read = 0;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t nuwax_mic_input_read_vocat_mic12_pcm16(nuwax_mic_input_t *mic,
                                                 int16_t *mic1_samples,
                                                 int16_t *mic2_samples,
                                                 size_t sample_count,
                                                 uint32_t timeout_ms,
                                                 size_t *samples_read) {
    if (!mic || !mic->initialized || !mic->rx_handle || !mic1_samples || !mic2_samples || !samples_read) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    if (!mic->vocat_shared_rx) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sample_count == 0) {
        *samples_read = 0;
        return ESP_OK;
    }

    size_t want = sample_count;
    const size_t cap_frames = mic->raw_capacity / (NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t));
    if (want > cap_frames) {
        want = cap_frames;
    }
    uint32_t overflow_before = 0;
    const bool discontinuous_before_read =
        mic_rx_overflow_begin(mic, &overflow_before);
    size_t bytes_read = 0;
    esp_err_t err = mic_read_dma_blocks(mic, want * NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t),
                                         timeout_ms, &bytes_read);
    esp_err_t continuity_err = mic_rx_overflow_end(mic,
                                                   samples_read,
                                                   overflow_before,
                                                   discontinuous_before_read,
                                                   is_i2s_read_error_usable(err,
                                                                            bytes_read));
    if (continuity_err != ESP_OK) {
        return continuity_err;
    }
    if (!is_i2s_read_error_usable(err, bytes_read)) {
        *samples_read = 0;
        clear_raw_stats(mic);
        return err;
    }

    const size_t frames = bytes_read / (NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t));
    const int16_t *tdm = (const int16_t *)mic->raw_samples;
    int32_t sum = 0, mn = 0, mx = 0;
    int32_t mic2_sum = 0;
    for (size_t f = 0; f < frames; f++) {
        const size_t base = f * NUWAX_VOCAT_MIC_SLOTS;
        const int16_t mic1_v = apply_vocat_input_gain(tdm[base]); /* slot0 = MIC1 primary */
        const int16_t mic2_v = tdm[base + 1U]; /* slot1 = MIC2 secondary */
        mic1_samples[f] = mic1_v;
        mic2_samples[f] = mic2_v;
        if (f == 0) {
            mn = mic1_v;
            mx = mic1_v;
        }
        if (mic1_v < mn) {
            mn = mic1_v;
        }
        if (mic1_v > mx) {
            mx = mic1_v;
        }
        sum += mic1_v;
        mic2_sum += mic2_v;
    }

    const int32_t mic1_mean = frames > 0 ? sum / (int32_t)frames : 0;
    const int32_t mic2_mean = frames > 0 ? mic2_sum / (int32_t)frames : 0;
#if CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE
    mn = 0;
    mx = 0;
    for (size_t f = 0; f < frames; f++) {
        mic1_samples[f] = clamp_i32_to_i16((int32_t)mic1_samples[f] - mic1_mean);
        mic2_samples[f] = clamp_i32_to_i16((int32_t)mic2_samples[f] - mic2_mean);
        if (f == 0 || mic1_samples[f] < mn) {
            mn = mic1_samples[f];
        }
        if (f == 0 || mic1_samples[f] > mx) {
            mx = mic1_samples[f];
        }
    }
#endif

    nuwax_mic_raw_stats_t st = {0};
    st.sample_count = frames;
    st.dc_block_enabled = CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE;
    st.auto_attenuate_enabled = CONFIG_NUWAX_MIC_AUTO_ATTENUATE;
    if (frames > 0) {
        st.first = tdm[0];
        st.last = tdm[(frames - 1U) * NUWAX_VOCAT_MIC_SLOTS];
        st.min = mn;
        st.max = mx;
        st.mean = mic1_mean;
        st.centered_peak = (mx > -mn) ? mx : -mn;
    }
    mic->last_raw_stats = st;
    *samples_read = frames;
    if (frames > 0U) {
        mic->rx_audio_started = true;
    }
    return err == ESP_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_OK;
#else
    (void)mic;
    (void)mic1_samples;
    (void)mic2_samples;
    (void)sample_count;
    (void)timeout_ms;
    *samples_read = 0;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t nuwax_mic_input_read_vocat_mic12_ref_pcm16(nuwax_mic_input_t *mic,
                                                     int16_t *mic1_samples,
                                                     int16_t *mic2_samples,
                                                     int16_t *ref_samples,
                                                     size_t sample_count,
                                                     uint32_t timeout_ms,
                                                     size_t *samples_read) {
    if (!mic || !mic->initialized || !mic->rx_handle || !mic1_samples || !mic2_samples ||
        !ref_samples || !samples_read) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    if (!mic->vocat_shared_rx) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sample_count == 0) {
        *samples_read = 0;
        return ESP_OK;
    }

    size_t want = sample_count;
    const size_t cap_frames = mic->raw_capacity / (NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t));
    if (want > cap_frames) {
        want = cap_frames;
    }
    uint32_t overflow_before = 0;
    const bool discontinuous_before_read =
        mic_rx_overflow_begin(mic, &overflow_before);
    size_t bytes_read = 0;
    esp_err_t err = mic_read_dma_blocks(mic, want * NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t),
                                         timeout_ms, &bytes_read);
    esp_err_t continuity_err = mic_rx_overflow_end(mic,
                                                   samples_read,
                                                   overflow_before,
                                                   discontinuous_before_read,
                                                   is_i2s_read_error_usable(err,
                                                                            bytes_read));
    if (continuity_err != ESP_OK) {
        return continuity_err;
    }
    if (!is_i2s_read_error_usable(err, bytes_read)) {
        *samples_read = 0;
        clear_raw_stats(mic);
        return err;
    }

    const size_t frames = bytes_read / (NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t));
    const int16_t *tdm = (const int16_t *)mic->raw_samples;
    int32_t sum = 0, mn = 0, mx = 0;
    int32_t mic2_sum = 0;
    for (size_t f = 0; f < frames; f++) {
        const size_t base = f * NUWAX_VOCAT_MIC_SLOTS;
        const int16_t mic1_v = apply_vocat_input_gain(tdm[base]); /* slot0 = MIC1 primary */
        const int16_t mic2_v = tdm[base + 1U];                   /* slot1 = MIC2 secondary */
        const int16_t ref_v = tdm[base + 2U];                    /* slot2 = hardware ref candidate */
        mic1_samples[f] = mic1_v;
        mic2_samples[f] = mic2_v;
        ref_samples[f] = ref_v;
        if (f == 0) {
            mn = mic1_v;
            mx = mic1_v;
        }
        if (mic1_v < mn) {
            mn = mic1_v;
        }
        if (mic1_v > mx) {
            mx = mic1_v;
        }
        sum += mic1_v;
        mic2_sum += mic2_v;
    }

    const int32_t mic1_mean = frames > 0 ? sum / (int32_t)frames : 0;
    const int32_t mic2_mean = frames > 0 ? mic2_sum / (int32_t)frames : 0;
#if CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE
    mn = 0;
    mx = 0;
    for (size_t f = 0; f < frames; f++) {
        mic1_samples[f] = clamp_i32_to_i16((int32_t)mic1_samples[f] - mic1_mean);
        mic2_samples[f] = clamp_i32_to_i16((int32_t)mic2_samples[f] - mic2_mean);
        if (f == 0 || mic1_samples[f] < mn) {
            mn = mic1_samples[f];
        }
        if (f == 0 || mic1_samples[f] > mx) {
            mx = mic1_samples[f];
        }
    }
#endif

    nuwax_mic_raw_stats_t st = {0};
    st.sample_count = frames;
    st.dc_block_enabled = CONFIG_NUWAX_MIC_DC_BLOCK_ENABLE;
    st.auto_attenuate_enabled = CONFIG_NUWAX_MIC_AUTO_ATTENUATE;
    if (frames > 0) {
        st.first = tdm[0];
        st.last = tdm[(frames - 1U) * NUWAX_VOCAT_MIC_SLOTS];
        st.min = mn;
        st.max = mx;
        st.mean = mic1_mean;
        st.centered_peak = (mx > -mn) ? mx : -mn;
    }
    mic->last_raw_stats = st;
    *samples_read = frames;
    if (frames > 0U) {
        mic->rx_audio_started = true;
    }
    return err == ESP_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_OK;
#else
    (void)mic;
    (void)mic1_samples;
    (void)mic2_samples;
    (void)ref_samples;
    (void)sample_count;
    (void)timeout_ms;
    *samples_read = 0;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

const nuwax_mic_raw_stats_t *nuwax_mic_input_get_last_raw_stats(const nuwax_mic_input_t *mic) {
    if (!mic) {
        return NULL;
    }
    return &mic->last_raw_stats;
}

esp_err_t nuwax_mic_input_deinit(nuwax_mic_input_t *mic) {
    if (!mic) {
        return ESP_OK;
    }
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    if (mic->vocat_shared_rx) {
        /* Consume this instance's board reference once. Physical teardown
         * failure belongs to audio_board; no old MIC object may retry release
         * against a new generation. Release the process-wide capture owner
         * after this task stops using RX so the next init can recover. */
        esp_err_t result = ESP_OK;
        if (mic->board_lease.token != 0) {
            esp_err_t release_err = nuwax_audio_board_release(&mic->board_lease);
            if (release_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "shared RX lease release failed owner=%s err=%s; controlled restart preserves generation isolation",
                         owner_name(mic->owner),
                         esp_err_to_name(release_err));
                heap_caps_free(mic->raw_samples);
                mic->raw_samples = NULL;
                mic->raw_capacity = 0U;
                mic_dma_cache_reset(mic);
                mic->rx_handle = NULL;
                mic->initialized = false;
                mic->rx_audio_started = false;
                release_owner(mic);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
                vTaskDelay(portMAX_DELAY);
                return release_err;
            }
        }
        if (mic->board_lease.token == 0) {
            esp_err_t board_err = nuwax_audio_board_deinit(&mic->board_ref);
            if (board_err != ESP_OK) {
                if (result == ESP_OK) {
                    result = board_err;
                }
                ESP_LOGE(TAG,
                         "shared audio board deinit failed owner=%s err=%s; teardown remains retryable",
                         owner_name(mic->owner),
                         esp_err_to_name(board_err));
            }
        }
        heap_caps_free(mic->raw_samples);
        mic->raw_samples = NULL;
        mic->raw_capacity = 0U;
        mic_dma_cache_reset(mic);
        mic->rx_handle = NULL;
        mic->initialized = false;
        mic->rx_audio_started = false;
        release_owner(mic);
        if (mic->board_ref.token == 0) {
            memset(mic, 0, sizeof(*mic));
        }
        return result;
    }
#endif
    if (mic->rx_handle) {
        i2s_chan_handle_t rx_handle = (i2s_chan_handle_t)mic->rx_handle;
        if (mic->initialized) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(rx_handle));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_del_channel(rx_handle));
    }
    heap_caps_free(mic->raw_samples);
    release_owner(mic);
    memset(mic, 0, sizeof(*mic));
    return ESP_OK;
}

esp_err_t nuwax_mic_input_capture_selftest(uint32_t frames, int *out_rms, int *out_peak) {
    if (frames == 0) {
        frames = 1600; /* ~100ms @16k */
    }
    nuwax_mic_input_t mic = {0};
    esp_err_t err = nuwax_mic_input_init_with_owner(&mic, NUWAX_MIC_OWNER_CONVERSATION_CAPTURE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "P2-4 mic selftest init failed: %s", esp_err_to_name(err));
        return err;
    }

    const size_t chunk = 320; /* 20ms @16k mono */
    int16_t buf[320];
    uint64_t sumsq = 0;
    uint64_t n = 0;
    int peak = 0;
    uint32_t read_errors = 0;
    uint32_t got = 0;
    while (got < frames) {
        size_t want = frames - got;
        if (want > chunk) {
            want = chunk;
        }
        size_t rd = 0;
        esp_err_t rerr = nuwax_mic_input_read_pcm16(&mic, buf, want, 200, &rd);
        if (rerr != ESP_OK) {
            read_errors++;
            if (read_errors > 20) {
                break;
            }
            continue;
        }
        for (size_t i = 0; i < rd; i++) {
            int v = buf[i];
            sumsq += (uint64_t)((int64_t)v * v);
            int a = v < 0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
        }
        n += rd;
        got += rd;
    }

    int rms = n ? (int)sqrt((double)sumsq / (double)n) : 0;
    if (out_rms) {
        *out_rms = rms;
    }
    if (out_peak) {
        *out_peak = peak;
    }
    ESP_LOGI(TAG, "P2-4 mic capture selftest %s frames=%llu rms=%d peak=%d read_err=%lu",
             (n > 0 && read_errors == 0) ? "PASS" : "WARN",
             (unsigned long long)n, rms, peak, (unsigned long)read_errors);
    nuwax_mic_input_deinit(&mic);
    return (n > 0) ? ESP_OK : ESP_FAIL;
}
