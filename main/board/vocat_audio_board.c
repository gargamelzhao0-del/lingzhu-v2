#include "board/vocat_audio_board.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "board/audio_board.h"
#include "board/esp_vocat_v1_0_pins.h"
#include "board/i2c_bus_manager.h"
#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0

static const char *TAG = "vocat_audio";

#define VOCAT_AUDIO_I2S_PORT I2S_NUM_0
#define VOCAT_ES8311_DEFAULT_VOLUME_DB (-6.0f)
/* ES7210 runs 4-slot TDM. NOTE: the codec only enters TDM mode when >=3 mics
 * are selected (es7210 ENABLE_TDM_MAX_NUM=3), and P2-2's shared-clock 64-BCLK
 * alignment depends on 4 slots, so all four MICx stay selected and RX_SLOT_COUNT
 * stays 4 even though only 2 are real microphones. Physical slot roles on
 * the Miaopan 2.0 test board (runtime health-gated):
 *   slot0 = MIC1  primary mic (mono capture / ASR / AFE M channel)
 *   slot1 = MIC2  secondary mic (acoustic pickup, NOT electrical reference)
 *   slot2 = MIC3  candidate playback reference route (ES8311 OUTP/N -> RC -> MIC3)
 *                 Hardware AEC is not released unless ref_health and the
 *                 active-tone A/B check pass.
 *   slot3 = MIC4  not connected (NC) — unused. */
#define VOCAT_AUDIO_RX_SLOT_COUNT 4
#define VOCAT_AUDIO_FULL_SLOT_MASK ((1U << VOCAT_AUDIO_RX_SLOT_COUNT) - 1U)
#define VOCAT_ES7210_MIC_SELECTED (ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4)
#ifndef CONFIG_NUWAX_VOCAT_ES7210_MIC_GAIN_DB
#define CONFIG_NUWAX_VOCAT_ES7210_MIC_GAIN_DB 30
#endif
#define VOCAT_ES7210_MIC_GAIN_DB ((float)CONFIG_NUWAX_VOCAT_ES7210_MIC_GAIN_DB)
#define VOCAT_AUDIO_TX_FRAME_SAMPLES 320
#define VOCAT_AUDIO_RX_FRAME_SAMPLES 240
#define VOCAT_AUDIO_RX_RATE_TOLERANCE_HZ (VOCAT_AUDIO_SAMPLE_RATE_HZ / 100U)
#define VOCAT_AUDIO_RX_DRAIN_FRAMES 6
#define VOCAT_AEC_TEST_TONE_HZ 1000U
#define VOCAT_AEC_TEST_TONE_PERIOD_SAMPLES (VOCAT_AUDIO_SAMPLE_RATE_HZ / VOCAT_AEC_TEST_TONE_HZ)
#define VOCAT_AEC_REF_HEALTH_GOERTZEL_N (VOCAT_AUDIO_SAMPLE_RATE_HZ / 100U)
#define VOCAT_AEC_REF_HEALTH_XCORR_MAX_LAG 255
#define VOCAT_AEC_REF_HEALTH_TX_RING_SAMPLES 512
#define VOCAT_AEC_TEST_TONE_AMPLITUDE 4000.0f
#define VOCAT_MIC3_SILENT_PROBE_AMPLITUDE 28000.0f
#define VOCAT_DIAGNOSTIC_TX_STOP_TIMEOUT_MS 1500U
#define VOCAT_DIAGNOSTIC_TX_SUSPEND_TIMEOUT_MS 100U
#define VOCAT_CODEC_I2C_GUARD_TIMEOUT_MS 250U

#ifndef CONFIG_NUWAX_VOCAT_PA_ENABLE_SETTLE_MS
#define CONFIG_NUWAX_VOCAT_PA_ENABLE_SETTLE_MS 20
#endif
#ifndef CONFIG_NUWAX_VOCAT_PA_DISABLE_SETTLE_MS
#define CONFIG_NUWAX_VOCAT_PA_DISABLE_SETTLE_MS 5
#endif
#ifndef CONFIG_NUWAX_VOCAT_PA_TRANSITION_MAX_MS
#define CONFIG_NUWAX_VOCAT_PA_TRANSITION_MAX_MS 100
#endif

_Static_assert((VOCAT_AUDIO_SAMPLE_RATE_HZ % VOCAT_AEC_TEST_TONE_HZ) == 0,
               "AEC test tone requires an integer sample period");

#ifndef CONFIG_NUWAX_P3_AEC_REF_HEALTH_SCORE_THRESHOLD
#define CONFIG_NUWAX_P3_AEC_REF_HEALTH_SCORE_THRESHOLD 500
#endif

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static const audio_codec_ctrl_if_t *s_es7210_ctrl;
static const audio_codec_if_t *s_es7210_if;
static const audio_codec_ctrl_if_t *s_es8311_ctrl;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_es8311_if;
static bool s_initialized;
static bool s_pa_forced_off;
static DRAM_ATTR volatile uint32_t s_rx_overflow_count;
static float s_es8311_volume_db = VOCAT_ES8311_DEFAULT_VOLUME_DB;

typedef struct {
    audio_codec_ctrl_if_t base;
    const audio_codec_ctrl_if_t *delegate;
    atomic_uint io_failures;
} vocat_codec_ctrl_guard_t;

static vocat_codec_ctrl_guard_t s_es7210_ctrl_guard;
static vocat_codec_ctrl_guard_t s_es8311_ctrl_guard;

static int16_t s_tx_frame[VOCAT_AUDIO_TX_FRAME_SAMPLES * 2];
static int16_t s_rx_frame[VOCAT_AUDIO_RX_FRAME_SAMPLES * VOCAT_AUDIO_RX_SLOT_COUNT];

static esp_err_t set_external_pa_enabled(bool enabled, const char *label);

static int codec_ctrl_guard_open(const audio_codec_ctrl_if_t *ctrl,
                                 void *cfg,
                                 int cfg_size)
{
    (void)cfg;
    (void)cfg_size;
    const vocat_codec_ctrl_guard_t *guard =
        (const vocat_codec_ctrl_guard_t *)ctrl;
    if (!guard || !guard->delegate || !guard->delegate->is_open) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    return guard->delegate->is_open(guard->delegate)
               ? ESP_CODEC_DEV_OK
               : ESP_CODEC_DEV_WRONG_STATE;
}

static bool codec_ctrl_guard_is_open(const audio_codec_ctrl_if_t *ctrl)
{
    const vocat_codec_ctrl_guard_t *guard =
        (const vocat_codec_ctrl_guard_t *)ctrl;
    return guard && guard->delegate && guard->delegate->is_open &&
           guard->delegate->is_open(guard->delegate);
}

/* Some codec methods discard register-operation errors. Keep an independent
 * counter so a failed clock transaction cannot be reported as restored. */
static int codec_ctrl_guard_result(const audio_codec_ctrl_if_t *ctrl, int result)
{
    vocat_codec_ctrl_guard_t *guard = (vocat_codec_ctrl_guard_t *)ctrl;
    if (guard && result != ESP_CODEC_DEV_OK)
        atomic_fetch_add_explicit(&guard->io_failures, 1U, memory_order_relaxed);
    return result;
}

static int codec_ctrl_guard_read(const audio_codec_ctrl_if_t *ctrl,
                                 int reg,
                                 int reg_len,
                                 void *data,
                                 int data_len)
{
    const vocat_codec_ctrl_guard_t *guard =
        (const vocat_codec_ctrl_guard_t *)ctrl;
    if (!guard || !guard->delegate || !guard->delegate->read_reg) {
        return codec_ctrl_guard_result(ctrl, ESP_CODEC_DEV_INVALID_ARG);
    }
    if (nuwax_i2c_bus_transaction_begin(
            VOCAT_CODEC_I2C_GUARD_TIMEOUT_MS) != ESP_OK) {
        return codec_ctrl_guard_result(ctrl, ESP_CODEC_DEV_DRV_ERR);
    }
    const int result = guard->delegate->read_reg(
        guard->delegate, reg, reg_len, data, data_len);
    nuwax_i2c_bus_transaction_end();
    return codec_ctrl_guard_result(ctrl, result);
}

static int codec_ctrl_guard_write(const audio_codec_ctrl_if_t *ctrl,
                                  int reg,
                                  int reg_len,
                                  void *data,
                                  int data_len)
{
    const vocat_codec_ctrl_guard_t *guard =
        (const vocat_codec_ctrl_guard_t *)ctrl;
    if (!guard || !guard->delegate || !guard->delegate->write_reg) {
        return codec_ctrl_guard_result(ctrl, ESP_CODEC_DEV_INVALID_ARG);
    }
    if (nuwax_i2c_bus_transaction_begin(
            VOCAT_CODEC_I2C_GUARD_TIMEOUT_MS) != ESP_OK) {
        return codec_ctrl_guard_result(ctrl, ESP_CODEC_DEV_DRV_ERR);
    }
    const int result = guard->delegate->write_reg(
        guard->delegate, reg, reg_len, data, data_len);
    nuwax_i2c_bus_transaction_end();
    return codec_ctrl_guard_result(ctrl, result);
}

static int codec_ctrl_guard_close(const audio_codec_ctrl_if_t *ctrl)
{
    (void)ctrl;
    /* The raw ESP-Codec control object remains owned by the board and is
     * deleted after its codec object. */
    return ESP_CODEC_DEV_OK;
}

static const audio_codec_ctrl_if_t *codec_ctrl_guard_bind(
    vocat_codec_ctrl_guard_t *guard,
    const audio_codec_ctrl_if_t *delegate)
{
    if (!guard || !delegate) {
        return NULL;
    }
    memset(guard, 0, sizeof(*guard));
    atomic_init(&guard->io_failures, 0U);
    guard->base.open = codec_ctrl_guard_open;
    guard->base.is_open = codec_ctrl_guard_is_open;
    guard->base.read_reg = codec_ctrl_guard_read;
    guard->base.write_reg = codec_ctrl_guard_write;
    guard->base.close = codec_ctrl_guard_close;
    guard->delegate = delegate;
    return &guard->base;
}

static const audio_codec_ctrl_if_t *new_codec_i2c_ctrl_guarded(
    audio_codec_i2c_cfg_t *config)
{
    if (nuwax_i2c_bus_transaction_begin(
            VOCAT_CODEC_I2C_GUARD_TIMEOUT_MS) != ESP_OK) {
        return NULL;
    }
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(config);
    nuwax_i2c_bus_transaction_end();
    return ctrl;
}

static int delete_codec_i2c_ctrl_guarded(
    const audio_codec_ctrl_if_t *ctrl,
    bool *deleted)
{
    if (deleted) {
        *deleted = false;
    }
    if (nuwax_i2c_bus_transaction_begin(
            VOCAT_CODEC_I2C_GUARD_TIMEOUT_MS) != ESP_OK) {
        return ESP_CODEC_DEV_DRV_ERR;
    }
    const int result = audio_codec_delete_ctrl_if(ctrl);
    if (deleted) {
        /* ESP-Codec frees the object regardless of its close result. */
        *deleted = true;
    }
    nuwax_i2c_bus_transaction_end();
    return result;
}

static bool pa_output_latch_enabled(void)
{
#if NUWAX_VOCAT_PA_CTRL_GPIO < 32
    const uint32_t out0 = REG_READ(GPIO_OUT_REG);
    return ((out0 >> NUWAX_VOCAT_PA_CTRL_GPIO) & 1U) != 0U;
#else
    const uint32_t out1 = REG_READ(GPIO_OUT1_REG);
    return ((out1 >> (NUWAX_VOCAT_PA_CTRL_GPIO - 32U)) & 1U) != 0U;
#endif
}

typedef struct {
    atomic_bool stop;
    uint32_t errors;
    uint64_t frames;
    TaskHandle_t task;
    SemaphoreHandle_t done;
    StaticSemaphore_t done_storage;
} diagnostic_tx_control_t;

typedef struct {
    diagnostic_tx_control_t control;
} smoke_tx_task_ctx_t;

typedef struct {
    diagnostic_tx_control_t control;
    atomic_bool tone;
} health_tx_task_ctx_t;

struct nuwax_vocat_test_tone_handle {
    volatile bool stop;
    uint32_t errors;
    uint64_t frames;
    i2s_chan_handle_t tx_handle;
    nuwax_audio_board_lease_t board_lease;
    nuwax_audio_board_ref_t board_ref;
    SemaphoreHandle_t done;
    bool task_stopped;
};

static bool IRAM_ATTR rx_queue_overflow_callback(i2s_chan_handle_t handle,
                                                 i2s_event_data_t *event,
                                                 void *user_ctx)
{
    (void)handle;
    (void)event;
    (void)user_ctx;
    s_rx_overflow_count++;
    return false;
}

static int16_t tone_1khz_sample(uint32_t sample_index, float amplitude)
{
    float phase = (float)(sample_index % VOCAT_AEC_TEST_TONE_PERIOD_SAMPLES) /
                  (float)VOCAT_AEC_TEST_TONE_PERIOD_SAMPLES;
    return (int16_t)(sinf(phase * 2.0f * (float)M_PI) * amplitude);
}

static void fill_tx_frame_buffer(int16_t *frame, uint32_t frame_index, float amplitude)
{
    for (size_t i = 0; i < VOCAT_AUDIO_TX_FRAME_SAMPLES; i++) {
        int16_t v = tone_1khz_sample(frame_index * VOCAT_AUDIO_TX_FRAME_SAMPLES + (uint32_t)i, amplitude);
        frame[i * 2] = v;
        frame[i * 2 + 1] = v;
    }
}

static void fill_tx_frame(uint32_t frame_index)
{
    fill_tx_frame_buffer(s_tx_frame, frame_index, 3000.0f);
}

/* Keep the AEC diagnostic tone strong enough for the MIC3 reference path while
 * limiting its PCM amplitude to one seventh of the original 28000 test level. */
static void fill_tx_frame_loud(uint32_t frame_index)
{
    fill_tx_frame_buffer(s_tx_frame, frame_index, VOCAT_AEC_TEST_TONE_AMPLITUDE);
}

static void smoke_tx_task(void *arg)
{
    smoke_tx_task_ctx_t *ctx = (smoke_tx_task_ctx_t *)arg;
    uint32_t frame_index = 0;
    while (!atomic_load_explicit(&ctx->control.stop, memory_order_acquire)) {
        fill_tx_frame(frame_index++);
        size_t bytes_done = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_tx_frame, sizeof(s_tx_frame), &bytes_done, 1000);
        if (err == ESP_OK && bytes_done == sizeof(s_tx_frame)) {
            ctx->control.frames += bytes_done / (sizeof(int16_t) * 2U);
        } else {
            ESP_LOGW(TAG, "P2 TX write failed err=%s bytes=%u", esp_err_to_name(err), (unsigned)bytes_done);
            ctx->control.errors++;
        }
    }
    xSemaphoreGive(ctx->control.done);
    vTaskSuspend(NULL);
}

static void health_tx_task(void *arg)
{
    health_tx_task_ctx_t *ctx = (health_tx_task_ctx_t *)arg;
    int16_t tx_frame[VOCAT_AUDIO_TX_FRAME_SAMPLES * 2];
    uint32_t frame_index = 0;
    while (!atomic_load_explicit(&ctx->control.stop, memory_order_acquire)) {
        if (atomic_load_explicit(&ctx->tone, memory_order_acquire)) {
            fill_tx_frame_buffer(tx_frame, frame_index++, VOCAT_AEC_TEST_TONE_AMPLITUDE);
        } else {
            memset(tx_frame, 0, sizeof(tx_frame));
        }
        size_t bytes_done = 0;
        esp_err_t err = i2s_channel_write(s_tx, tx_frame, sizeof(tx_frame), &bytes_done, 1000);
        if (err == ESP_OK && bytes_done == sizeof(tx_frame)) {
            ctx->control.frames += bytes_done / (sizeof(int16_t) * 2U);
        } else {
            ESP_LOGW(TAG, "P3-0 health TX write failed err=%s bytes=%u/%u",
                     esp_err_to_name(err), (unsigned)bytes_done, (unsigned)sizeof(tx_frame));
            ctx->control.errors++;
        }
    }
    xSemaphoreGive(ctx->control.done);
    vTaskSuspend(NULL);
}

static void *diagnostic_tx_context_alloc(size_t bytes)
{
    diagnostic_tx_control_t *control = heap_caps_calloc(
        1U, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!control) {
        return NULL;
    }
    atomic_init(&control->stop, false);
    control->done = xSemaphoreCreateBinaryStatic(&control->done_storage);
    if (!control->done) {
        heap_caps_free(control);
        return NULL;
    }
    return control;
}

static void diagnostic_tx_context_free(void *context)
{
    diagnostic_tx_control_t *control = context;
    if (!control) {
        return;
    }
    if (control->task) {
        ESP_LOGE(TAG,
                 "refusing to release diagnostic TX context while task is still owned task=%p",
                 control->task);
        return;
    }
    if (control->done) {
        vSemaphoreDelete(control->done);
        control->done = NULL;
    }
    heap_caps_free(control);
}

static void diagnostic_tx_stop_and_reap(diagnostic_tx_control_t *control,
                                        const char *label)
{
    if (!control || !control->task) {
        return;
    }
    atomic_store_explicit(&control->stop, true, memory_order_release);
    if (xSemaphoreTake(control->done,
                       pdMS_TO_TICKS(VOCAT_DIAGNOSTIC_TX_STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG,
                 "%s TX task stop timed out after %u ms; controlled restart preserves diagnostic lease and I2S ownership",
                 label ? label : "diagnostic",
                 (unsigned)VOCAT_DIAGNOSTIC_TX_STOP_TIMEOUT_MS);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        vTaskDelay(portMAX_DELAY);
        return;
    }

    const int64_t suspend_deadline_us =
        esp_timer_get_time() +
        (int64_t)VOCAT_DIAGNOSTIC_TX_SUSPEND_TIMEOUT_MS * 1000LL;
    eTaskState task_state = eTaskGetState(control->task);
    while (task_state != eSuspended &&
           esp_timer_get_time() < suspend_deadline_us) {
        vTaskDelay(1);
        task_state = eTaskGetState(control->task);
    }
    if (task_state != eSuspended) {
        ESP_LOGE(TAG,
                 "%s TX task completion was signaled without suspension state=%d; controlled restart preserves task storage ownership",
                 label ? label : "diagnostic",
                 (int)task_state);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        vTaskDelay(portMAX_DELAY);
        return;
    }
    vTaskDelete(control->task);
    control->task = NULL;
}

static void test_tone_task(void *arg)
{
    nuwax_vocat_test_tone_handle_t *ctx = (nuwax_vocat_test_tone_handle_t *)arg;
    uint32_t frame_index = 0;
    while (!ctx->stop) {
        fill_tx_frame_loud(frame_index++);
        size_t bytes_done = 0;
        esp_err_t err = i2s_channel_write(ctx->tx_handle,
                                           s_tx_frame,
                                           sizeof(s_tx_frame),
                                           &bytes_done,
                                           1000);
        if (err == ESP_OK && bytes_done == sizeof(s_tx_frame)) {
            ctx->frames += bytes_done / (sizeof(int16_t) * 2U);
        } else {
            ctx->errors++;
            if (!ctx->stop) {
                ESP_LOGW(TAG, "P3 active-tone TX write failed err=%s bytes=%u/%u",
                         esp_err_to_name(err),
                         (unsigned)bytes_done,
                         (unsigned)sizeof(s_tx_frame));
            }
        }
    }
    if (ctx->done) {
        xSemaphoreGive(ctx->done);
    }
    vTaskDelete(NULL);
}

static void log_i2s_channel(const char *name, i2s_chan_handle_t handle)
{
    i2s_chan_info_t info = {0};
    esp_err_t err = i2s_channel_get_info(handle, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s get_info failed: %s", name, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG,
             "%s port=%d dir=%d mode=%d enabled=%d mclk=%u bclk=%u dma=%u pair=%p",
             name,
             info.id,
             info.dir,
             info.mode,
             info.is_enabled,
             (unsigned)info.mclk_hz,
             (unsigned)info.bclk_hz,
             (unsigned)info.total_dma_buf_size,
             info.pair_chan);
}

static uint32_t rate_delta_hz(uint32_t actual_hz, uint32_t expected_hz)
{
    return actual_hz > expected_hz ? actual_hz - expected_hz : expected_hz - actual_hz;
}

static esp_err_t enable_i2s_channel(const char *name, i2s_chan_handle_t handle)
{
    i2s_chan_info_t info = {0};
    esp_err_t info_err = i2s_channel_get_info(handle, &info);
    if (info_err == ESP_OK && info.is_enabled) {
        ESP_LOGI(TAG, "%s already enabled", name);
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_enable(handle);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "%s already enabled", name);
        return ESP_OK;
    }
    return err;
}

static void drain_rx_dma(void)
{
    for (int i = 0; i < VOCAT_AUDIO_RX_DRAIN_FRAMES; i++) {
        size_t bytes_done = 0;
        (void)i2s_channel_read(s_rx, s_rx_frame, sizeof(s_rx_frame), &bytes_done, 20);
    }
}

static esp_err_t codec_dev_ret_to_err(int rc, const char *label)
{
    if (rc == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s failed rc=%d", label, rc);
    return ESP_FAIL;
}

static esp_err_t set_es8311_sample_info(esp_codec_dev_sample_info_t *fs,
                                       const char *label)
{
    if (!s_es8311_if || !s_es8311_if->set_fs) return ESP_ERR_INVALID_STATE;
    const unsigned failures = atomic_load_explicit(&s_es8311_ctrl_guard.io_failures,
                                                   memory_order_relaxed);
    int rc = s_es8311_if->set_fs(s_es8311_if, fs);
    if (atomic_load_explicit(&s_es8311_ctrl_guard.io_failures, memory_order_relaxed) != failures)
        rc = ESP_CODEC_DEV_DRV_ERR;
    return codec_dev_ret_to_err(rc, label);
}

static esp_err_t set_es8311_volume_db(float volume_db, const char *label)
{
    ESP_RETURN_ON_FALSE(s_es8311_if && s_es8311_if->set_vol,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "ES8311 volume control unavailable");
    esp_err_t err = codec_dev_ret_to_err(s_es8311_if->set_vol(s_es8311_if, volume_db), label);
    if (err == ESP_OK) {
        s_es8311_volume_db = volume_db;
    }
    return err;
}

static esp_err_t restore_es8311_volume(float volume_db, const char *label)
{
    ESP_RETURN_ON_FALSE(s_es8311_if && s_es8311_if->set_vol,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "ES8311 volume restore unavailable");
    esp_err_t err = codec_dev_ret_to_err(
        s_es8311_if->set_vol(s_es8311_if, volume_db), label);
    if (err == ESP_OK) {
        s_es8311_volume_db = volume_db;
    }
    return err;
}

static int codec_get_reg_u8(const audio_codec_if_t *codec, int reg)
{
    int value = 0;
    if (!codec || !codec->get_reg) {
        return -1;
    }
    int rc = codec->get_reg(codec, reg, &value);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "codec read reg 0x%02x failed rc=%d", reg, rc);
        return -1;
    }
    return value & 0xff;
}

static void log_es7210_key_regs(const char *phase)
{
    static const uint8_t regs[] = {
        0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x11, 0x12, 0x14, 0x15, 0x40, 0x41, 0x42, 0x43,
        0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
        0x4c,
    };
    char line[256] = {0};
    size_t used = 0;
    for (size_t i = 0; i < sizeof(regs); i++) {
        int value = codec_get_reg_u8(s_es7210_if, regs[i]);
        int written = value < 0
                          ? snprintf(line + used, sizeof(line) - used, "%s%02x=??", i == 0 ? "" : " ", regs[i])
                          : snprintf(line + used,
                                     sizeof(line) - used,
                                     "%s%02x=%02x",
                                     i == 0 ? "" : " ",
                                     regs[i],
                                     value);
        if (written < 0 || (size_t)written >= sizeof(line) - used) {
            break;
        }
        used += (size_t)written;
    }
    ESP_LOGI(TAG, "ES7210 regs[%s] %s", phase ? phase : "snapshot", line);
}

static esp_err_t init_i2s_duplex(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(VOCAT_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM;
    chan_cfg.auto_clear_after_cb = true;

    ESP_LOGI(TAG,
             "P2 audio init I2S duplex port=%d dma_desc=%d dma_frame=%d dma_free=%u dma_largest=%u "
             "internal_free=%u internal_largest=%u",
             VOCAT_AUDIO_I2S_PORT,
             CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM,
             CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel duplex failed: %s", esp_err_to_name(err));
        goto fail;
    }

    i2s_std_config_t tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOCAT_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = NUWAX_VOCAT_I2S_MCLK_GPIO,
            .bclk = NUWAX_VOCAT_I2S_BCK_GPIO,
            .ws = NUWAX_VOCAT_I2S_WS_GPIO,
            .dout = NUWAX_VOCAT_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    tx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
#if CONFIG_NUWAX_P3_VENDOR_V2_AUDIO_PROFILE && !CONFIG_NUWAX_P3_VENDOR_V2_CLOCK_ALIGNED_TX
    tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    tx_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    tx_cfg.slot_cfg.left_align = true;
#else
    tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
#endif
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    err = i2s_channel_init_std_mode(s_tx, &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init TX STD failed: %s", esp_err_to_name(err));
        goto fail;
    }

    i2s_tdm_config_t rx_cfg = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(VOCAT_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = NUWAX_VOCAT_I2S_MCLK_GPIO,
            .bclk = NUWAX_VOCAT_I2S_BCK_GPIO,
            .ws = NUWAX_VOCAT_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = NUWAX_VOCAT_I2S_DIN_GPIO,
        },
    };
    rx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
#if CONFIG_NUWAX_P3_REF_TDM_REFERENCE_STYLE
    rx_cfg.clk_cfg.bclk_div = 8;
    rx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    rx_cfg.slot_cfg.total_slot = I2S_TDM_AUTO_SLOT_NUM;
    ESP_LOGW(TAG,
             "P3-0 TDM profile=reference-style rx_slot_bit_width=AUTO rx_total_slot=AUTO "
             "rx_bclk_div=8; compare cyberkit AECVERIFY");
#else
    rx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    rx_cfg.slot_cfg.total_slot = VOCAT_AUDIO_RX_SLOT_COUNT;
    ESP_LOGI(TAG,
             "P3-0 TDM profile=7.0-fixed rx_slot_bit_width=16 rx_total_slot=%d "
             "rx_bclk_div=default; reference profile is AUTO/AUTO/bclk_div=8",
             VOCAT_AUDIO_RX_SLOT_COUNT);
#endif
#if CONFIG_NUWAX_P3_VENDOR_V2_AUDIO_PROFILE
#if CONFIG_NUWAX_P3_VENDOR_V2_CLOCK_ALIGNED_TX
    ESP_LOGW(TAG,
             "P3-0 audio profile=vendor-v2-24k-clock-aligned sample_rate=%lu "
             "mclk_expected=%lu tx_bclk_expected=%lu rx_bclk_expected=%lu "
             "tx_data=16 tx_slot=32 rx_data=16 rx_slot=AUTO rx_total=AUTO "
             "rx_bclk_div=8 rx_mask=0x%02x; four-slot evidence preserved, "
             "16k ESP-SR A/B disabled",
             (unsigned long)VOCAT_AUDIO_SAMPLE_RATE_HZ,
             (unsigned long)(VOCAT_AUDIO_SAMPLE_RATE_HZ * 256U),
             (unsigned long)(VOCAT_AUDIO_SAMPLE_RATE_HZ * 64U),
             (unsigned long)(VOCAT_AUDIO_SAMPLE_RATE_HZ * 64U),
             VOCAT_AUDIO_FULL_SLOT_MASK);
#else
    ESP_LOGW(TAG,
             "P3-0 audio profile=vendor-v2-24k-exact sample_rate=%lu "
             "mclk_expected=%lu tx_bclk_expected=%lu rx_bclk_expected=%lu "
             "shared_bclk_mismatch=1 tx_data=16 tx_slot=AUTO rx_data=16 rx_slot=AUTO rx_total=AUTO "
             "rx_bclk_div=8 rx_mask=0x%02x; four-slot evidence preserved, "
             "16k ESP-SR hardref A/B disabled",
             (unsigned long)VOCAT_AUDIO_SAMPLE_RATE_HZ,
             (unsigned long)(VOCAT_AUDIO_SAMPLE_RATE_HZ * 256U),
             (unsigned long)(VOCAT_AUDIO_SAMPLE_RATE_HZ * 32U),
             (unsigned long)(VOCAT_AUDIO_SAMPLE_RATE_HZ * 64U),
             VOCAT_AUDIO_FULL_SLOT_MASK);
#endif
#else
    ESP_LOGI(TAG,
             "P3-0 audio profile=7.0-16k-baseline sample_rate=%lu tx_data=16 tx_slot=32 "
             "rx_mask=0x%02x",
             (unsigned long)VOCAT_AUDIO_SAMPLE_RATE_HZ,
             VOCAT_AUDIO_FULL_SLOT_MASK);
#endif
    ESP_LOGI(TAG,
             "P3-0 TDM route intent: full_probe_mask=0x%02x normal_candidate_mask=0x%02x "
             "(slot0=M, slot2=R candidate)",
             VOCAT_AUDIO_FULL_SLOT_MASK,
             (1U << VOCAT_AUDIO_SLOT_MIC_PRIMARY) | (1U << VOCAT_AUDIO_SLOT_AEC_REF));
    err = i2s_channel_init_tdm_mode(s_rx, &rx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init RX TDM failed: %s", esp_err_to_name(err));
        goto fail;
    }

    const i2s_event_callbacks_t rx_callbacks = {
        .on_recv_q_ovf = rx_queue_overflow_callback,
    };
    err = i2s_channel_register_event_callback(s_rx, &rx_callbacks, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register RX overflow callback failed: %s", esp_err_to_name(err));
        goto fail;
    }

    log_i2s_channel("tx", s_tx);
    log_i2s_channel("rx", s_rx);
    return ESP_OK;

fail: {
    esp_err_t cleanup_err = ESP_OK;
    if (s_tx) {
        const esp_err_t delete_err = i2s_del_channel(s_tx);
        if (delete_err == ESP_OK) {
            s_tx = NULL;
        } else {
            ESP_LOGE(TAG,
                     "TX delete failed during I2S init rollback; handle retained for retry: %s",
                     esp_err_to_name(delete_err));
            cleanup_err = delete_err;
        }
    }
    if (s_rx) {
        const esp_err_t delete_err = i2s_del_channel(s_rx);
        if (delete_err == ESP_OK) {
            s_rx = NULL;
        } else {
            ESP_LOGE(TAG,
                     "RX delete failed during I2S init rollback; handle retained for retry: %s",
                     esp_err_to_name(delete_err));
            if (cleanup_err == ESP_OK) {
                cleanup_err = delete_err;
            }
        }
    }
    return cleanup_err != ESP_OK ? cleanup_err : err;
}
}

static esp_err_t init_es7210(void)
{
    ESP_RETURN_ON_ERROR(enable_i2s_channel("tx_mclk_pre_es7210", s_tx), TAG, "enable TX MCLK failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(nuwax_i2c_bus_get(&bus), TAG, "shared I2C get failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = NUWAX_VOCAT_CODEC_DEV_ADDR_ES7210,
        .bus_handle = bus,
    };
    s_es7210_ctrl = new_codec_i2c_ctrl_guarded(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_es7210_ctrl, ESP_FAIL, TAG, "ES7210 I2C ctrl create failed");
    const audio_codec_ctrl_if_t *guarded_ctrl =
        codec_ctrl_guard_bind(&s_es7210_ctrl_guard, s_es7210_ctrl);
    ESP_RETURN_ON_FALSE(guarded_ctrl, ESP_FAIL, TAG,
                        "ES7210 I2C guard create failed");

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = guarded_ctrl,
        /* All 4 selected so ES7210 stays in TDM mode (needs >=3); slots are
         * mic1/mic2/AEC-ref/NC per the slot-role macros above, NOT 4 mics. */
        .mic_selected = VOCAT_ES7210_MIC_SELECTED,
    };
    s_es7210_if = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(s_es7210_if, ESP_FAIL, TAG, "ES7210 codec create failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = VOCAT_AUDIO_RX_SLOT_COUNT,
        .channel_mask = VOCAT_AUDIO_FULL_SLOT_MASK,
        .sample_rate = VOCAT_AUDIO_SAMPLE_RATE_HZ,
    };
    ESP_LOGI(TAG,
             "ES7210 set_fs bits=%d channel=%d mask=0x%02x rate=%lu mic_selected=0x%02x "
             "gain=%.0fdB",
             fs.bits_per_sample,
             fs.channel,
             (unsigned)fs.channel_mask,
             (unsigned long)fs.sample_rate,
             VOCAT_ES7210_MIC_SELECTED,
             (double)VOCAT_ES7210_MIC_GAIN_DB);
    ESP_RETURN_ON_ERROR(codec_dev_ret_to_err(s_es7210_if->set_fs(s_es7210_if, &fs), "ES7210 set_fs"),
                        TAG,
                        "ES7210 set_fs failed");
    ESP_RETURN_ON_ERROR(codec_dev_ret_to_err(s_es7210_if->enable(s_es7210_if, true), "ES7210 enable"),
                        TAG,
                        "ES7210 enable failed");
    /* IMPORTANT: gain must be set AFTER enable. The es7210 driver rejects
     * set_gain while is_open==false, and enable()->mic_select rewrites the
     * gain nibble to codec->gain (default 0 = ~min). Real-hw symptom of the
     * old (gain-before-enable) order: avg_abs stuck at noise floor (~1-9),
     * VAD never triggers, no uplink. */
    if (s_es7210_if->set_mic_gain) {
        ESP_RETURN_ON_ERROR(codec_dev_ret_to_err(
                                s_es7210_if->set_mic_gain(s_es7210_if, VOCAT_ES7210_MIC_GAIN_DB), "ES7210 gain"),
                            TAG,
                            "ES7210 gain failed");
    }
    if (s_es7210_if->mute_mic) {
        ESP_RETURN_ON_ERROR(codec_dev_ret_to_err(s_es7210_if->mute_mic(s_es7210_if, false), "ES7210 unmute"),
                            TAG,
                            "ES7210 unmute failed");
    }
    log_es7210_key_regs("after_init");
    ESP_LOGI(TAG, "ES7210 init ok addr8=0x%02x gain=%.0fdB", NUWAX_VOCAT_CODEC_DEV_ADDR_ES7210,
             (double)VOCAT_ES7210_MIC_GAIN_DB);
    return ESP_OK;
}

static esp_err_t restart_es7210_after_i2s_ready(void)
{
    ESP_RETURN_ON_FALSE(s_es7210_if && s_es7210_if->enable, ESP_ERR_INVALID_STATE, TAG, "ES7210 iface missing");

    ESP_RETURN_ON_ERROR(codec_dev_ret_to_err(s_es7210_if->enable(s_es7210_if, false), "ES7210 clocked stop"),
                        TAG,
                        "ES7210 clocked stop failed");
    /* Preserve the board's existing 5 ms stop/start interval exactly.  The
     * ES7210 programming guide still needs to be frozen before treating this
     * conservative value as a device-mandated minimum. */
    esp_rom_delay_us(5000U);
    ESP_RETURN_ON_ERROR(codec_dev_ret_to_err(s_es7210_if->enable(s_es7210_if, true), "ES7210 clocked start"),
                        TAG,
                        "ES7210 clocked start failed");
    /* re-apply gain: the enable above ran mic_select which reset the gain
     * nibble back to codec->gain default. */
    if (s_es7210_if->set_mic_gain) {
        ESP_RETURN_ON_ERROR(codec_dev_ret_to_err(
                                s_es7210_if->set_mic_gain(s_es7210_if, VOCAT_ES7210_MIC_GAIN_DB), "ES7210 regain"),
                            TAG,
                            "ES7210 regain failed");
    }
    if (s_es7210_if->mute_mic) {
        ESP_RETURN_ON_ERROR(codec_dev_ret_to_err(s_es7210_if->mute_mic(s_es7210_if, false), "ES7210 clocked unmute"),
                            TAG,
                            "ES7210 clocked unmute failed");
    }
    log_es7210_key_regs("after_clocked_restart");
    return ESP_OK;
}

static esp_err_t init_es8311(void)
{
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(nuwax_i2c_bus_get(&bus), TAG, "shared I2C get failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = NUWAX_VOCAT_CODEC_DEV_ADDR_ES8311,
        .bus_handle = bus,
    };
    s_es8311_ctrl = new_codec_i2c_ctrl_guarded(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_es8311_ctrl, ESP_FAIL, TAG, "ES8311 I2C ctrl create failed");
    const audio_codec_ctrl_if_t *guarded_ctrl =
        codec_ctrl_guard_bind(&s_es8311_ctrl_guard, s_es8311_ctrl);
    ESP_RETURN_ON_FALSE(guarded_ctrl, ESP_FAIL, TAG,
                        "ES8311 I2C guard create failed");

    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if, ESP_FAIL, TAG, "codec gpio_if create failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = guarded_ctrl,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        /* The board owns PA sequencing. Codec enable() otherwise raises PA
         * and unmutes the DAC before the shared I2S clocks are running. */
        .pa_pin = -1,
        .use_mclk = true,
    };
    es8311_cfg.hw_gain.pa_voltage = 5.0f;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3f;
    s_es8311_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_es8311_if, ESP_FAIL, TAG, "ES8311 codec create failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = VOCAT_AUDIO_SAMPLE_RATE_HZ,
    };
    ESP_LOGI(TAG,
             "ES8311 set_fs bits=%d channel=%d mask=0x%02x rate=%lu volume_db=%.1f",
             fs.bits_per_sample,
             fs.channel,
             (unsigned)fs.channel_mask,
             (unsigned long)fs.sample_rate,
             (double)VOCAT_ES8311_DEFAULT_VOLUME_DB);
    ESP_RETURN_ON_ERROR(set_es8311_sample_info(&fs, "ES8311 set_fs"),
                        TAG,
                        "ES8311 set_fs failed");
    ESP_RETURN_ON_ERROR(set_es8311_volume_db(VOCAT_ES8311_DEFAULT_VOLUME_DB, "ES8311 volume"),
                        TAG,
                        "ES8311 volume failed");
    ESP_RETURN_ON_ERROR(codec_dev_ret_to_err(s_es8311_if->enable(s_es8311_if, true), "ES8311 enable"),
                        TAG,
                        "ES8311 enable failed");
    if (s_es8311_if->mute) {
        ESP_RETURN_ON_ERROR(codec_dev_ret_to_err(s_es8311_if->mute(s_es8311_if, false), "ES8311 unmute"),
                            TAG,
                            "ES8311 unmute failed");
    }
    ESP_LOGI(TAG, "ES8311 init ok addr8=0x%02x volume=55 pa=GPIO%d held_off=1",
             NUWAX_VOCAT_CODEC_DEV_ADDR_ES8311,
             NUWAX_VOCAT_PA_CTRL_GPIO);
    return ESP_OK;
}

esp_err_t nuwax_vocat_audio_board_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (s_es8311_if || s_es7210_if || s_es8311_ctrl || s_es7210_ctrl ||
        s_gpio_if || s_tx || s_rx) {
        ESP_LOGW(TAG, "retrying cleanup of a partial audio-board lifecycle");
        ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_deinit(),
                            TAG,
                            "partial audio-board cleanup retry failed");
    }

    /* Mute before codec power/register changes and clock startup, including
     * the first boot and the restore after BLE provisioning. */
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_force_speaker_mute(),
                        TAG, "audio init PA mute failed");

    ESP_LOGI(TAG,
             "P2 audio board init begin mclk=%d bclk=%d ws=%d dout=%d din=%d pa=%d",
             NUWAX_VOCAT_I2S_MCLK_GPIO,
             NUWAX_VOCAT_I2S_BCK_GPIO,
             NUWAX_VOCAT_I2S_WS_GPIO,
             NUWAX_VOCAT_I2S_DOUT_GPIO,
             NUWAX_VOCAT_I2S_DIN_GPIO,
             NUWAX_VOCAT_PA_CTRL_GPIO);

    /* Miaopan 2.0 supplies the codecs from the dedicated CODEC_3V3 DCDC.
     * There is no GPIO-controlled codec power switch in the schematic. Keep
     * the legacy GPIO path available only for profiles that explicitly define
     * a non-negative power pin. */
#if NUWAX_VOCAT_CODEC_PWR_GPIO >= 0
    gpio_config_t codec_pwr = {
        .pin_bit_mask = 1ULL << NUWAX_VOCAT_CODEC_PWR_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&codec_pwr),
                        TAG,
                        "codec analog power GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(NUWAX_VOCAT_CODEC_PWR_GPIO, 1),
                        TAG,
                        "codec analog power enable failed");
    ESP_LOGI(TAG, "codec analog power: GPIO%d=1", NUWAX_VOCAT_CODEC_PWR_GPIO);
#else
    ESP_LOGI(TAG, "codec analog power: CODEC_3V3 schematic rail (no GPIO switch)");
#endif

    esp_err_t err = init_i2s_duplex();
    if (err == ESP_OK) {
        err = init_es7210();
    }
    if (err == ESP_OK) {
        err = init_es8311();
    }
    if (err == ESP_OK) {
        err = enable_i2s_channel("rx", s_rx);
    }
    if (err == ESP_OK) {
        err = enable_i2s_channel("tx", s_tx);
    }
    if (err == ESP_OK) {
        err = restart_es7210_after_i2s_ready();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio board init failed with rollback: %s", esp_err_to_name(err));
        const esp_err_t cleanup_err = nuwax_vocat_audio_board_deinit();
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "audio board init rollback remains pending for next init/deinit retry: %s",
                     esp_err_to_name(cleanup_err));
            return cleanup_err;
        }
        return err;
    }
    log_i2s_channel("tx_after_codec", s_tx);
    log_i2s_channel("rx_after_codec", s_rx);

    s_initialized = true;
    ESP_LOGI(TAG, "P2 audio board init ok");
    return ESP_OK;
}

void *nuwax_vocat_audio_board_get_rx_handle(void)
{
    return s_initialized ? (void *)s_rx : NULL;
}

void *nuwax_vocat_audio_board_get_tx_handle(void)
{
    return s_initialized ? (void *)s_tx : NULL;
}

uint32_t nuwax_vocat_audio_board_get_rx_overflow_count(void)
{
    return s_rx_overflow_count;
}

esp_err_t nuwax_vocat_audio_board_set_music_rate(uint32_t rate)
{
    if (!s_initialized || !s_tx || !s_rx || !s_es8311_if ||
        (rate != 8000U && rate != 11025U && rate != 12000U && rate != 16000U &&
         rate != 22050U && rate != 24000U && rate != 32000U &&
         rate != 44100U && rate != 48000U)) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_force_speaker_mute(), TAG, "music mute");
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = i2s_channel_disable(s_rx);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    i2s_std_clk_config_t tx = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_tdm_clk_config_t rx = I2S_TDM_CLK_DEFAULT_CONFIG(rate);
    tx.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx.mclk_multiple = I2S_MCLK_MULTIPLE_256;
#if CONFIG_NUWAX_P3_REF_TDM_REFERENCE_STYLE
    rx.bclk_div = 8;
#endif
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &tx), TAG, "music TX clock");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_tdm_clock(s_rx, &rx), TAG, "music RX clock");
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16, .channel = 2, .sample_rate = rate,
    };
    ESP_RETURN_ON_ERROR(set_es8311_sample_info(&fs, "music DAC rate"), TAG, "music DAC rate");
    /* Clear the complete DMA ring before either music or voice can unmute. */
    int16_t silence[256 * 2] = {0};
    size_t total = 0;
    const size_t capacity = CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM *
                            CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM * 4U;
    while (total < capacity) {
        size_t written = 0;
        size_t bytes = capacity - total;
        if (bytes > sizeof(silence)) bytes = sizeof(silence);
        ESP_RETURN_ON_ERROR(i2s_channel_preload_data(s_tx, silence, bytes, &written),
                            TAG, "music silence preload");
        if (written == 0U) break;
        total += written;
    }
    ESP_RETURN_ON_ERROR(enable_i2s_channel("music RX", s_rx), TAG, "music RX enable");
    ESP_RETURN_ON_ERROR(enable_i2s_channel("music TX", s_tx), TAG, "music TX enable");
    ESP_LOGI(TAG, "exclusive music clock rate=%lu voice=%d DMA_silence=%u",
             (unsigned long)rate, rate == VOCAT_AUDIO_SAMPLE_RATE_HZ, (unsigned)total);
    return ESP_OK;
}

esp_err_t nuwax_vocat_audio_board_playback_stress(int cycles, nuwax_vocat_playback_stress_stats_t *out)
{
    ESP_RETURN_ON_FALSE(nuwax_audio_board_diagnostic_lease_is_held(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "playback stress requires exclusive diagnostic lease");
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_init(), TAG, "audio board init failed");
    ESP_RETURN_ON_FALSE(s_es8311_if && s_es8311_if->enable, ESP_ERR_INVALID_STATE, TAG, "ES8311 iface missing");
    if (cycles <= 0) {
        cycles = 100;
    }
    nuwax_vocat_playback_stress_stats_t st = {0};
    size_t min_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const float playback_volume_db = s_es8311_volume_db;

    for (int i = 0; i < cycles; i++) {
        /* Diagnostics explicitly opt into audible output; codec enable no
         * longer owns PA. Keep it off during each codec restart. */
        if (nuwax_vocat_audio_board_force_speaker_mute() != ESP_OK) {
            st.disable_errors++;
            break;
        }
        if (codec_dev_ret_to_err(s_es8311_if->enable(s_es8311_if, true), "stress enable") != ESP_OK) {
            st.enable_errors++;
            continue;
        }
        /* ES8311 suspend clears DAC volume register 0x32. Restore the intended
         * level after every start so this test proves audible playback instead
         * of only proving that enable() and I2S writes returned success. */
        if (set_es8311_volume_db(playback_volume_db, "stress volume restore") != ESP_OK) {
            st.enable_errors++;
            if (codec_dev_ret_to_err(s_es8311_if->enable(s_es8311_if, false),
                                     "stress disable after volume failure") != ESP_OK) {
                st.disable_errors++;
            }
            st.cycles_done++;
            continue;
        }
        if (nuwax_vocat_audio_board_prepare_speaker_playback() != ESP_OK) {
            st.enable_errors++;
            break;
        }
        fill_tx_frame((uint32_t)i); /* ~20ms 1kHz burst */
        size_t bytes_done = 0;
        esp_err_t werr = i2s_channel_write(s_tx, s_tx_frame, sizeof(s_tx_frame), &bytes_done, 200);
        if (werr != ESP_OK || bytes_done != sizeof(s_tx_frame)) {
            st.write_errors++;
        } else {
            st.frames += bytes_done / (sizeof(int16_t) * 2U);
        }
        if (nuwax_vocat_audio_board_force_speaker_mute() != ESP_OK) {
            st.disable_errors++;
            break;
        }
        if (codec_dev_ret_to_err(s_es8311_if->enable(s_es8311_if, false), "stress disable") != ESP_OK) {
            st.disable_errors++;
        }
        st.cycles_done++;

        size_t now_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (now_heap < min_heap) {
            min_heap = now_heap;
        }
        if ((i % 20) == 0) {
            ESP_LOGI(TAG, "P2-3 playback stress i=%d en_err=%lu wr_err=%lu dis_err=%lu internal_free=%u",
                     i, (unsigned long)st.enable_errors, (unsigned long)st.write_errors,
                     (unsigned long)st.disable_errors, (unsigned)now_heap);
        }
    }
    /* Leave the codec enabled at the same real DAC level for the smoke test. */
    if (nuwax_vocat_audio_board_force_speaker_mute() != ESP_OK ||
        codec_dev_ret_to_err(s_es8311_if->enable(s_es8311_if, true),
                             "stress final enable") != ESP_OK ||
        set_es8311_volume_db(playback_volume_db, "stress final volume restore") != ESP_OK) {
        st.enable_errors++;
    }
    st.min_internal_free = (uint32_t)min_heap;
    if (out) {
        *out = st;
    }
    bool pass = (st.cycles_done == cycles) && st.enable_errors == 0 &&
                st.write_errors == 0 && st.disable_errors == 0;
    ESP_LOGI(TAG, "P2-3 playback stress %s cycles=%d/%d en_err=%lu wr_err=%lu dis_err=%lu frames=%llu min_internal_free=%lu",
             pass ? "PASS" : "FAIL", st.cycles_done, cycles,
             (unsigned long)st.enable_errors, (unsigned long)st.write_errors,
             (unsigned long)st.disable_errors, (unsigned long long)st.frames,
             (unsigned long)st.min_internal_free);
    return pass ? ESP_OK : ESP_FAIL;
}

esp_err_t nuwax_vocat_audio_board_test_tone_start(nuwax_vocat_test_tone_handle_t **out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "active-tone out_handle is NULL");
    *out_handle = NULL;

    nuwax_audio_board_ref_t ref = {0};
    /* Keep TX alive while the concurrent hard-reference A/B run owns RX. */
    ESP_RETURN_ON_ERROR(nuwax_audio_board_init(&ref), TAG, "active-tone board init failed");
    void *tx_handle = NULL;
    nuwax_audio_board_lease_t lease = {0};
    esp_err_t err = nuwax_audio_board_acquire_tx(&tx_handle, &lease);
    if (err != ESP_OK) {
        (void)nuwax_audio_board_deinit(&ref);
        return err;
    }

    nuwax_vocat_test_tone_handle_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        (void)nuwax_audio_board_release(&lease);
        (void)nuwax_audio_board_deinit(&ref);
        return ESP_ERR_NO_MEM;
    }
    ctx->tx_handle = (i2s_chan_handle_t)tx_handle;
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        (void)nuwax_audio_board_release(&lease);
        (void)nuwax_audio_board_deinit(&ref);
        return ESP_ERR_NO_MEM;
    }

    err = nuwax_vocat_audio_board_prepare_speaker_playback();
    if (err != ESP_OK) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
        (void)nuwax_audio_board_release(&lease);
        (void)nuwax_audio_board_deinit(&ref);
        return err;
    }
    BaseType_t task_ok = xTaskCreate(test_tone_task,
                                     "p3_active_tone",
                                     4096,
                                     ctx,
                                     6,
                                     NULL);
    if (task_ok != pdPASS) {
        (void)nuwax_vocat_audio_board_force_speaker_mute();
        vSemaphoreDelete(ctx->done);
        free(ctx);
        (void)nuwax_audio_board_release(&lease);
        (void)nuwax_audio_board_deinit(&ref);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = ctx;
    ctx->board_lease = lease;
    ctx->board_ref = ref;
    ESP_LOGW(TAG, "P3 active-tone started amplitude=%.0f tone_hz=1000 board_tx_lease=held",
             (double)VOCAT_AEC_TEST_TONE_AMPLITUDE);
    return ESP_OK;
}

esp_err_t nuwax_vocat_audio_board_test_tone_stop(nuwax_vocat_test_tone_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "active-tone handle is NULL");
    handle->stop = true;
    if (!handle->task_stopped) {
        if (xSemaphoreTake(handle->done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE(TAG, "P3 active-tone task did not stop within timeout");
            return ESP_ERR_TIMEOUT;
        }
        handle->task_stopped = true;
    }
    ESP_LOGW(TAG, "P3 active-tone stopped tx_frames=%llu tx_errors=%lu",
             (unsigned long long)handle->frames,
             (unsigned long)handle->errors);
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_force_speaker_mute(),
                        TAG, "active-tone PA mute failed");
    if (handle->board_lease.token != 0) {
        ESP_RETURN_ON_ERROR(nuwax_audio_board_release(&handle->board_lease),
                            TAG,
                            "active-tone TX lease release failed");
    }
    ESP_RETURN_ON_ERROR(nuwax_audio_board_deinit(&handle->board_ref),
                        TAG,
                        "active-tone board deinit failed");
    vSemaphoreDelete(handle->done);
    free(handle);
    return ESP_OK;
}

static esp_err_t capture_slot_rms(uint32_t duration_ms,
                                  int out_rms[VOCAT_AUDIO_RX_SLOT_COUNT],
                                  uint32_t *out_timeouts)
{
    ESP_RETURN_ON_FALSE(out_rms, ESP_ERR_INVALID_ARG, TAG, "slot RMS output is NULL");
    memset(out_rms, 0, sizeof(int) * VOCAT_AUDIO_RX_SLOT_COUNT);
    if (out_timeouts) {
        *out_timeouts = 0;
    }

    double sumsq[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    uint64_t frames_total = 0;
    uint32_t read_timeouts = 0;
    const uint32_t overflow_before = s_rx_overflow_count;
    const uint64_t target_frames =
        ((uint64_t)duration_ms * VOCAT_AUDIO_SAMPLE_RATE_HZ + 999U) / 1000U;
    drain_rx_dma();
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms + 1000U);
    while (frames_total < target_frames &&
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx,
                                         s_rx_frame,
                                         sizeof(s_rx_frame),
                                         &bytes_read,
                                         250);
        if (err == ESP_ERR_TIMEOUT) {
            read_timeouts++;
            continue;
        }
        if (err != ESP_OK) {
            return err;
        }
        size_t frames = bytes_read / (sizeof(int16_t) * VOCAT_AUDIO_RX_SLOT_COUNT);
        for (size_t frame = 0; frame < frames; frame++) {
            for (size_t slot = 0; slot < VOCAT_AUDIO_RX_SLOT_COUNT; slot++) {
                double sample = s_rx_frame[frame * VOCAT_AUDIO_RX_SLOT_COUNT + slot];
                sumsq[slot] += sample * sample;
            }
        }
        frames_total += frames;
    }
    if (out_timeouts) {
        *out_timeouts = read_timeouts;
    }
    if (frames_total < target_frames) {
        ESP_LOGE(TAG,
                 "P3-0C slot RMS incomplete frames=%llu/%llu timeouts=%lu",
                 (unsigned long long)frames_total,
                 (unsigned long long)target_frames,
                 (unsigned long)read_timeouts);
        return frames_total == 0 ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
    }
    if (s_rx_overflow_count != overflow_before) {
        ESP_LOGE(TAG,
                 "slot RMS rejected after RX DMA overflow before=%lu after=%lu",
                 (unsigned long)overflow_before,
                 (unsigned long)s_rx_overflow_count);
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t slot = 0; slot < VOCAT_AUDIO_RX_SLOT_COUNT; slot++) {
        out_rms[slot] = (int)sqrt(sumsq[slot] / (double)frames_total);
    }
    if (read_timeouts > 0) {
        ESP_LOGW(TAG,
                 "P3-0C slot RMS recovered after %lu transient RX timeout(s)",
                 (unsigned long)read_timeouts);
    }
    return ESP_OK;
}

static esp_err_t set_es7210_channel_gain_db(int channel, float gain_db)
{
    ESP_RETURN_ON_FALSE(channel >= 0 && channel < VOCAT_AUDIO_RX_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "ES7210 channel out of range");
    ESP_RETURN_ON_FALSE(s_es7210_if && s_es7210_if->set_mic_channel_gain,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "ES7210 per-channel gain unavailable");
    uint16_t channel_mask = (uint16_t)(1U << channel);
    return codec_dev_ret_to_err(s_es7210_if->set_mic_channel_gain(s_es7210_if,
                                                                  channel_mask,
                                                                  gain_db),
                                "ES7210 channel gain");
}

static void wait_pa_settle_ms(uint32_t settle_ms)
{
    /* vTaskDelay(N) can return nearly one tick earlier than N tick periods
     * when called just before a tick. Measure elapsed time before declaring
     * the amplifier ready, especially for the 5 ms PA-off interval. */
    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)settle_ms * 1000LL;
    for (;;) {
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return;
        }
        const uint32_t remaining_ms = (uint32_t)((remaining_us + 999LL) / 1000LL);
        vTaskDelay((remaining_ms + portTICK_PERIOD_MS - 1U) / portTICK_PERIOD_MS);
    }
}

static esp_err_t set_external_pa_enabled(bool enabled, const char *label)
{
    ESP_RETURN_ON_FALSE(s_gpio_if && s_gpio_if->set,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "PA GPIO control unavailable");
    const int64_t started_us = esp_timer_get_time();
    esp_err_t err = codec_dev_ret_to_err(
        s_gpio_if->set(NUWAX_VOCAT_PA_CTRL_GPIO, enabled), label);
    if (err != ESP_OK) {
        return err;
    }
    const bool actual = pa_output_latch_enabled();
    if (actual != enabled) {
        ESP_LOGE(TAG, "%s output latch mismatch requested=%d actual=%d",
                 label, enabled ? 1 : 0, actual ? 1 : 0);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint32_t settle_ms = enabled ? CONFIG_NUWAX_VOCAT_PA_ENABLE_SETTLE_MS
                                       : CONFIG_NUWAX_VOCAT_PA_DISABLE_SETTLE_MS;
    wait_pa_settle_ms(settle_ms);
    const uint32_t elapsed_ms =
        (uint32_t)((esp_timer_get_time() - started_us + 999LL) / 1000LL);
    if (elapsed_ms > CONFIG_NUWAX_VOCAT_PA_TRANSITION_MAX_MS) {
        ESP_LOGE(TAG,
                 "%s exceeded PA transition deadline elapsed_ms=%lu max_ms=%d",
                 label,
                 (unsigned long)elapsed_ms,
                 CONFIG_NUWAX_VOCAT_PA_TRANSITION_MAX_MS);
        return ESP_ERR_TIMEOUT;
    }
    s_pa_forced_off = !enabled;
    ESP_LOGI(TAG,
             "%s GPIO%d=%d elapsed_ms=%lu feedback=output_latch",
             label,
             NUWAX_VOCAT_PA_CTRL_GPIO,
             actual ? 1 : 0,
             (unsigned long)elapsed_ms);
    return ESP_OK;
}

esp_err_t nuwax_vocat_audio_board_force_speaker_mute(void)
{
    /* Use the SoC GPIO driver directly so this last-resort path remains
     * available even when the codec GPIO wrapper is damaged or half-torn-down. */
    const int64_t started_us = esp_timer_get_time();
    /* Load the low latch before enabling output, so a stale high latch cannot
     * briefly turn PA on when recovering a pin previously configured as input. */
    ESP_RETURN_ON_ERROR(gpio_set_level(NUWAX_VOCAT_PA_CTRL_GPIO, 0),
                        TAG,
                        "force PA-off level failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(NUWAX_VOCAT_PA_CTRL_GPIO, GPIO_MODE_OUTPUT),
                        TAG,
                        "force PA-off direction failed");
    if (pa_output_latch_enabled()) {
        ESP_LOGE(TAG, "force PA-off GPIO latch mismatch");
        return ESP_ERR_INVALID_RESPONSE;
    }
    wait_pa_settle_ms(CONFIG_NUWAX_VOCAT_PA_DISABLE_SETTLE_MS);
    const uint32_t elapsed_ms =
        (uint32_t)((esp_timer_get_time() - started_us + 999LL) / 1000LL);
    if (elapsed_ms > CONFIG_NUWAX_VOCAT_PA_TRANSITION_MAX_MS) {
        ESP_LOGE(TAG,
                 "force PA-off exceeded deadline elapsed_ms=%lu max_ms=%d",
                 (unsigned long)elapsed_ms,
                 CONFIG_NUWAX_VOCAT_PA_TRANSITION_MAX_MS);
        return ESP_ERR_TIMEOUT;
    }
    s_pa_forced_off = true;
    ESP_LOGI(TAG,
             "PA forced off GPIO%d=0 elapsed_ms=%lu feedback=output_latch",
             NUWAX_VOCAT_PA_CTRL_GPIO,
             (unsigned long)elapsed_ms);
    return ESP_OK;
}

esp_err_t nuwax_vocat_audio_board_prepare_speaker_playback(void)
{
    ESP_RETURN_ON_FALSE(s_initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "prepare playback requires initialized board");
    if (!s_pa_forced_off && pa_output_latch_enabled()) {
        return ESP_OK;
    }
    const esp_err_t err =
        set_external_pa_enabled(true, "speaker playback PA prepare");
    if (err != ESP_OK) {
        /* An enable may have reached GPIO before verification/settling fails. */
        const esp_err_t mute_err = nuwax_vocat_audio_board_force_speaker_mute();
        if (mute_err != ESP_OK) {
            ESP_LOGE(TAG, "PA prepare rollback failed: %s", esp_err_to_name(mute_err));
        }
    }
    return err;
}

static esp_err_t capture_slots_with_sync_tone(
    float amplitude,
    uint32_t duration_ms,
    int out_rms[VOCAT_AUDIO_RX_SLOT_COUNT],
    int out_peak[VOCAT_AUDIO_RX_SLOT_COUNT],
    float out_clip_ratio[VOCAT_AUDIO_RX_SLOT_COUNT],
    uint32_t *tx_errors,
    uint32_t *rx_errors)
{
    ESP_RETURN_ON_FALSE(out_rms && out_peak && out_clip_ratio && tx_errors && rx_errors,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "silent probe capture output is NULL");
    memset(out_rms, 0, sizeof(int) * VOCAT_AUDIO_RX_SLOT_COUNT);
    memset(out_peak, 0, sizeof(int) * VOCAT_AUDIO_RX_SLOT_COUNT);
    memset(out_clip_ratio, 0, sizeof(float) * VOCAT_AUDIO_RX_SLOT_COUNT);

    double sumsq[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    uint64_t clipped[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    uint64_t frames_total = 0;
    uint32_t sample_index = 0;
    const uint32_t overflow_before = s_rx_overflow_count;
    int16_t tx_frame[VOCAT_AUDIO_RX_FRAME_SAMPLES * 2];

    /* Prime the DAC and discard the corresponding RX transition. The helper
     * stays synchronous so every exit has a bounded cleanup path. */
    for (int i = 0; i < 6; i++) {
        for (size_t sample = 0; sample < VOCAT_AUDIO_RX_FRAME_SAMPLES; sample++) {
            int16_t value = tone_1khz_sample(sample_index++, amplitude);
            tx_frame[sample * 2] = value;
            tx_frame[sample * 2 + 1] = value;
        }
        size_t bytes_written = 0;
        esp_err_t write_err = i2s_channel_write(s_tx,
                                                tx_frame,
                                                sizeof(tx_frame),
                                                &bytes_written,
                                                1000);
        if (write_err != ESP_OK || bytes_written != sizeof(tx_frame)) {
            (*tx_errors)++;
            return write_err == ESP_OK ? ESP_ERR_INVALID_SIZE : write_err;
        }
        size_t bytes_read = 0;
        esp_err_t read_err = i2s_channel_read(s_rx,
                                              s_rx_frame,
                                              sizeof(s_rx_frame),
                                              &bytes_read,
                                              250);
        if (read_err != ESP_OK) {
            (*rx_errors)++;
            if (read_err != ESP_ERR_TIMEOUT) {
                return read_err;
            }
        }
    }

    const uint64_t target_frames =
        ((uint64_t)duration_ms * VOCAT_AUDIO_SAMPLE_RATE_HZ + 999U) / 1000U;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms + 1500U);
    while (frames_total < target_frames &&
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        for (size_t sample = 0; sample < VOCAT_AUDIO_RX_FRAME_SAMPLES; sample++) {
            int16_t value = tone_1khz_sample(sample_index++, amplitude);
            tx_frame[sample * 2] = value;
            tx_frame[sample * 2 + 1] = value;
        }
        size_t bytes_written = 0;
        esp_err_t write_err = i2s_channel_write(s_tx,
                                                tx_frame,
                                                sizeof(tx_frame),
                                                &bytes_written,
                                                1000);
        if (write_err != ESP_OK || bytes_written != sizeof(tx_frame)) {
            (*tx_errors)++;
            return write_err == ESP_OK ? ESP_ERR_INVALID_SIZE : write_err;
        }

        size_t bytes_read = 0;
        esp_err_t read_err = i2s_channel_read(s_rx,
                                              s_rx_frame,
                                              sizeof(s_rx_frame),
                                              &bytes_read,
                                              250);
        if (read_err == ESP_ERR_TIMEOUT) {
            (*rx_errors)++;
            continue;
        }
        if (read_err != ESP_OK) {
            (*rx_errors)++;
            return read_err;
        }

        size_t frames = bytes_read / (sizeof(int16_t) * VOCAT_AUDIO_RX_SLOT_COUNT);
        for (size_t frame = 0; frame < frames; frame++) {
            for (size_t slot = 0; slot < VOCAT_AUDIO_RX_SLOT_COUNT; slot++) {
                int sample = s_rx_frame[frame * VOCAT_AUDIO_RX_SLOT_COUNT + slot];
                int magnitude = sample < 0 ? -sample : sample;
                sumsq[slot] += (double)sample * (double)sample;
                if (magnitude > out_peak[slot]) {
                    out_peak[slot] = magnitude;
                }
                if (magnitude >= 32760) {
                    clipped[slot]++;
                }
            }
        }
        frames_total += frames;
    }

    if (frames_total < target_frames) {
        ESP_LOGE(TAG,
                 "P3-0D silent probe capture incomplete frames=%llu/%llu tx_err=%lu rx_err=%lu",
                 (unsigned long long)frames_total,
                 (unsigned long long)target_frames,
                 (unsigned long)*tx_errors,
                 (unsigned long)*rx_errors);
        return frames_total == 0 ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
    }
    if (s_rx_overflow_count != overflow_before) {
        ESP_LOGE(TAG,
                 "synchronous slot capture rejected after RX DMA overflow before=%lu after=%lu",
                 (unsigned long)overflow_before,
                 (unsigned long)s_rx_overflow_count);
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t slot = 0; slot < VOCAT_AUDIO_RX_SLOT_COUNT; slot++) {
        out_rms[slot] = (int)sqrt(sumsq[slot] / (double)frames_total);
        out_clip_ratio[slot] = (float)clipped[slot] / (float)frames_total;
    }
    return ESP_OK;
}

static esp_err_t flush_tx_with_silence(void)
{
    memset(s_tx_frame, 0, sizeof(s_tx_frame));
    esp_err_t first_error = ESP_OK;
    for (int i = 0; i < 6; i++) {
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx,
                                          s_tx_frame,
                                          sizeof(s_tx_frame),
                                          &bytes_written,
                                          1000);
        if ((err != ESP_OK || bytes_written != sizeof(s_tx_frame)) && first_error == ESP_OK) {
            first_error = err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
        }
    }
    /* Six 320-sample zero frames exceed the 6x240-sample TX DMA capacity.
     * Keep PA disabled until the last possible strong sample has elapsed. */
    vTaskDelay(pdMS_TO_TICKS(120));
    return first_error;
}

esp_err_t nuwax_vocat_audio_board_mic3_silent_probe(
    nuwax_vocat_mic3_silent_probe_stats_t *out)
{
    ESP_RETURN_ON_FALSE(nuwax_audio_board_diagnostic_lease_is_held(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "MIC3 silent probe requires exclusive diagnostic lease");
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "silent MIC3 probe output is NULL");
    memset(out, 0, sizeof(*out));
    out->inferred_slot = -1;

    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_init(), TAG, "audio board init failed");
    ESP_RETURN_ON_FALSE(s_gpio_if && s_gpio_if->set &&
                            s_es7210_if && s_es7210_if->set_reg,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "silent MIC3 probe controls unavailable");

    const bool original_pa_high = pa_output_latch_enabled();
    const float previous_volume_db = s_es8311_volume_db;
    const int original_mic3_gain_reg = codec_get_reg_u8(s_es7210_if, 0x45);
    esp_err_t result = ESP_OK;
    bool pa_forced_off = false;
    bool volume_changed = false;
    bool gain_changed = false;
    bool strong_tone_may_be_queued = false;

    if (original_mic3_gain_reg < 0) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    result = set_external_pa_enabled(false, "P3-0D force PA off");
    if (result != ESP_OK) {
        goto cleanup;
    }
    pa_forced_off = true;
    vTaskDelay(pdMS_TO_TICKS(120));

    volume_changed = true; /* A failed I2C transaction may still write the register. */
    result = set_es8311_volume_db(0.0f, "P3-0D DAC volume");
    if (result != ESP_OK) {
        goto cleanup;
    }

    gain_changed = true; /* Restore the raw register even after a partial write. */
    result = set_es7210_channel_gain_db(2, VOCAT_ES7210_MIC_GAIN_DB);
    if (result != ESP_OK) {
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
    strong_tone_may_be_queued = true;
    result = capture_slots_with_sync_tone(VOCAT_MIC3_SILENT_PROBE_AMPLITUDE,
                                          500,
                                          out->high_gain_rms,
                                          out->high_gain_peak,
                                          out->high_gain_clip_ratio,
                                          &out->tx_errors,
                                          &out->rx_errors);
    if (result != ESP_OK) {
        goto cleanup;
    }

    result = set_es7210_channel_gain_db(2, 0.0f);
    if (result != ESP_OK) {
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
    result = capture_slots_with_sync_tone(VOCAT_MIC3_SILENT_PROBE_AMPLITUDE,
                                          500,
                                          out->low_gain_rms,
                                          out->low_gain_peak,
                                          out->low_gain_clip_ratio,
                                          &out->tx_errors,
                                          &out->rx_errors);
    if (result != ESP_OK) {
        goto cleanup;
    }

    float best_drop_db = 0.0f;
    float second_drop_db = 0.0f;
    for (int slot = 0; slot < VOCAT_AUDIO_RX_SLOT_COUNT; slot++) {
        out->drop_db[slot] =
            20.0f * log10f(((float)out->high_gain_rms[slot] + 1.0f) /
                           ((float)out->low_gain_rms[slot] + 1.0f));
        if (out->drop_db[slot] > best_drop_db) {
            second_drop_db = best_drop_db;
            best_drop_db = out->drop_db[slot];
            out->inferred_slot = slot;
        } else if (out->drop_db[slot] > second_drop_db) {
            second_drop_db = out->drop_db[slot];
        }
    }
    const float winner_margin_db = best_drop_db - second_drop_db;
    out->passed = out->inferred_slot == VOCAT_AUDIO_SLOT_AEC_REF &&
                  out->low_gain_rms[VOCAT_AUDIO_SLOT_AEC_REF] >= 20 &&
                  best_drop_db >= 6.0f && winner_margin_db >= 3.0f &&
                  out->tx_errors == 0 && out->rx_errors == 0;
    if (!out->passed) {
        result = ESP_ERR_INVALID_RESPONSE;
    }

cleanup:
    out->tx_flush_ok = !strong_tone_may_be_queued;
    if (strong_tone_may_be_queued) {
        out->tx_flush_ok = flush_tx_with_silence() == ESP_OK;
    }

    out->gain_restored = !gain_changed;
    if (gain_changed) {
        esp_err_t gain_err = codec_dev_ret_to_err(
            s_es7210_if->set_reg(s_es7210_if, 0x45, original_mic3_gain_reg),
            "P3-0D MIC3 gain restore");
        out->gain_restored = gain_err == ESP_OK &&
                             codec_get_reg_u8(s_es7210_if, 0x45) == original_mic3_gain_reg;
    }

    out->volume_restored = !volume_changed;
    if (volume_changed) {
        out->volume_restored =
            set_es8311_volume_db(previous_volume_db, "P3-0D volume restore") == ESP_OK;
    }

    const bool restoration_ready = pa_forced_off && out->tx_flush_ok &&
                                   out->gain_restored && out->volume_restored;
    if (restoration_ready) {
        out->pa_restored =
            set_external_pa_enabled(original_pa_high, "P3-0D PA restore") == ESP_OK;
    }
    out->cleanup_safe = restoration_ready && out->pa_restored;
    if (!out->cleanup_safe) {
        const esp_err_t force_mute_err =
            nuwax_vocat_audio_board_force_speaker_mute();
        ESP_LOGE(TAG,
                 "P3-0D cleanup unsafe flush=%d gain=%d volume=%d pa=%d force_mute=%s; PA intentionally left disabled",
                 out->tx_flush_ok ? 1 : 0,
                 out->gain_restored ? 1 : 0,
                 out->volume_restored ? 1 : 0,
                 out->pa_restored ? 1 : 0,
                 esp_err_to_name(force_mute_err));
        result = ESP_FAIL;
    }

    ESP_LOGW(TAG,
             "P3-0D MIC3 silent electrical probe result=%s amplitude=%.0f "
             "high_rms=[%d,%d,%d,%d] low_rms=[%d,%d,%d,%d] "
             "drop_db=[%.2f,%.2f,%.2f,%.2f] inferred_slot=%d passed=%d "
             "tx_err=%lu rx_err=%lu cleanup_safe=%d",
             esp_err_to_name(result),
             (double)VOCAT_MIC3_SILENT_PROBE_AMPLITUDE,
             out->high_gain_rms[0], out->high_gain_rms[1],
             out->high_gain_rms[2], out->high_gain_rms[3],
             out->low_gain_rms[0], out->low_gain_rms[1],
             out->low_gain_rms[2], out->low_gain_rms[3],
             (double)out->drop_db[0], (double)out->drop_db[1],
             (double)out->drop_db[2], (double)out->drop_db[3],
             out->inferred_slot,
             out->passed ? 1 : 0,
             (unsigned long)out->tx_errors,
             (unsigned long)out->rx_errors,
             out->cleanup_safe ? 1 : 0);
    return result;
}

esp_err_t nuwax_vocat_audio_board_channel_map_probe(void)
{
    ESP_RETURN_ON_FALSE(nuwax_audio_board_diagnostic_lease_is_held(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "channel-map probe requires exclusive diagnostic lease");
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_init(), TAG, "audio board init failed");
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_prepare_speaker_playback(),
                        TAG, "diagnostic PA prepare failed");

    const float previous_volume_db = s_es8311_volume_db;
    esp_err_t result = set_es8311_volume_db(0.0f, "channel-map probe volume");
    if (result != ESP_OK) {
        return result;
    }

    health_tx_task_ctx_t *tx_ctx = diagnostic_tx_context_alloc(sizeof(*tx_ctx));
    bool tx_started = false;
    uint64_t tx_frames = 0U;
    uint32_t tx_errors = 0U;
    uint32_t expected_mask = 0;
    uint32_t observable_mask = 0;
    uint32_t weak_mask = 0;
    bool mic3_ref_confirmed = false;
    bool mic2_confirmed = false;
    /* Figure 2e's CH1,CH3,CH2,CH4 wire labels cannot be equated directly with
     * physical MIC/PGA numbers. The 2026-07-24 per-PGA probe proved MIC1->slot0
     * and MIC2->slot1; keep the board's sequential slot contract while MIC3/4
     * remain explicitly unobservable. */
    static const int expected_slot_by_channel[VOCAT_AUDIO_RX_SLOT_COUNT] = {0, 1, 2, 3};
    if (!tx_ctx) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    atomic_init(&tx_ctx->tone, true);
    BaseType_t task_ok = xTaskCreate(health_tx_task,
                                    "p3_chmap_tx",
                                    4096,
                                    tx_ctx,
                                    5,
                                    &tx_ctx->control.task);
    if (task_ok != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    tx_started = true;

    for (int channel = 0; channel < VOCAT_AUDIO_RX_SLOT_COUNT; channel++) {
        result = set_es7210_channel_gain_db(channel, VOCAT_ES7210_MIC_GAIN_DB);
        if (result != ESP_OK) {
            goto cleanup;
        }
    }
    /* Keep the RX consumer active before its DMA ring can overflow. The RMS
     * helper tolerates a transient timeout and then requires a full frame
     * target, so a scheduling edge cannot abort the whole mapping probe. */
    vTaskDelay(pdMS_TO_TICKS(20));

    int baseline[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    uint32_t baseline_timeouts = 0;
    result = capture_slot_rms(500, baseline, &baseline_timeouts);
    if (result != ESP_OK) {
        goto cleanup;
    }
    ESP_LOGW(TAG,
             "P3-0C channel-map baseline gain=%.0fdB rms=[%d,%d,%d,%d] rx_timeouts=%lu",
             (double)VOCAT_ES7210_MIC_GAIN_DB,
             baseline[0], baseline[1], baseline[2], baseline[3],
             (unsigned long)baseline_timeouts);

    for (int channel = 0; channel < VOCAT_AUDIO_RX_SLOT_COUNT; channel++) {
        int low_gain_rms[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
        result = set_es7210_channel_gain_db(channel, 0.0f);
        if (result != ESP_OK) {
            goto cleanup;
        }
        int low_gain_reg = codec_get_reg_u8(s_es7210_if, 0x43 + channel);
        if (low_gain_reg < 0) {
            result = ESP_ERR_INVALID_RESPONSE;
        }
        vTaskDelay(pdMS_TO_TICKS(80));
        uint32_t rx_timeouts = 0;
        if (result == ESP_OK) {
            result = capture_slot_rms(350, low_gain_rms, &rx_timeouts);
        }
        esp_err_t restore_err = set_es7210_channel_gain_db(channel, VOCAT_ES7210_MIC_GAIN_DB);
        int restored_gain_reg = codec_get_reg_u8(s_es7210_if, 0x43 + channel);
        if (result == ESP_OK && restore_err != ESP_OK) {
            result = restore_err;
        }
        if (result == ESP_OK && restored_gain_reg < 0) {
            result = ESP_ERR_INVALID_RESPONSE;
        }
        if (result != ESP_OK) {
            goto cleanup;
        }
        vTaskDelay(pdMS_TO_TICKS(80));

        int inferred_slot = -1;
        float best_drop_db = 0.0f;
        float second_drop_db = 0.0f;
        float drop_db[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
        for (int slot = 0; slot < VOCAT_AUDIO_RX_SLOT_COUNT; slot++) {
            if (baseline[slot] < 20) {
                continue;
            }
            drop_db[slot] = 20.0f * log10f(((float)baseline[slot] + 1.0f) /
                                           ((float)low_gain_rms[slot] + 1.0f));
            if (drop_db[slot] > best_drop_db) {
                second_drop_db = best_drop_db;
                best_drop_db = drop_db[slot];
                inferred_slot = slot;
            } else if (drop_db[slot] > second_drop_db) {
                second_drop_db = drop_db[slot];
            }
        }
        const float winner_margin_db = best_drop_db - second_drop_db;
        const bool mapped = inferred_slot >= 0 && best_drop_db >= 6.0f && winner_margin_db >= 3.0f;
        const int expected_slot = expected_slot_by_channel[channel];
        /* A channel is observable only when changing its own PGA produces a
         * unique, material drop in a DMA slot. A loud expected slot alone does
         * not prove that the physical input is present on that slot. */
        const bool observable = mapped;
        const bool expected = mapped && inferred_slot == expected_slot;
        if (observable) {
            observable_mask |= 1U << channel;
        } else {
            weak_mask |= 1U << channel;
        }
        if (expected) {
            expected_mask |= 1U << channel;
        }
        if (channel == 1) {
            mic2_confirmed = expected;
        } else if (channel == 2) {
            mic3_ref_confirmed = expected;
        }
        ESP_LOGW(TAG,
                 "P3-0C channel-map mic=%d expected_slot=%d low_gain_reg=0x%02x restored_reg=0x%02x "
                 "low_gain_rms=[%d,%d,%d,%d] drop_db=[%.2f,%.2f,%.2f,%.2f] "
                 "inferred_slot=%d winner_drop_db=%.2f margin_db=%.2f mapped=%d observable=%d "
                 "expected=%d expected_slot_baseline=%d rx_timeouts=%lu",
                 channel + 1,
                 expected_slot,
                 low_gain_reg,
                 restored_gain_reg,
                 low_gain_rms[0], low_gain_rms[1], low_gain_rms[2], low_gain_rms[3],
                 (double)drop_db[0], (double)drop_db[1],
                 (double)drop_db[2], (double)drop_db[3],
                 inferred_slot,
                 (double)best_drop_db,
                 (double)winner_margin_db,
                 mapped ? 1 : 0,
                 observable ? 1 : 0,
                 expected ? 1 : 0,
                 baseline[expected_slot],
                 (unsigned long)rx_timeouts);
    }

    /* MIC4 is NC on this board. The hardware reference must be proven as
     * physical MIC3 on slot2 before hardware-reference AEC can be enabled. */
    if (!mic3_ref_confirmed) {
        result = ESP_ERR_INVALID_RESPONSE;
    }

cleanup:
    for (int channel = 0; channel < VOCAT_AUDIO_RX_SLOT_COUNT; channel++) {
        esp_err_t restore_err = set_es7210_channel_gain_db(channel, VOCAT_ES7210_MIC_GAIN_DB);
        if (result == ESP_OK && restore_err != ESP_OK) {
            result = restore_err;
        }
    }
    if (tx_started) {
        diagnostic_tx_stop_and_reap(&tx_ctx->control, "P3-0C channel-map");
    }
    if (tx_ctx) {
        tx_frames = tx_ctx->control.frames;
        tx_errors = tx_ctx->control.errors;
        diagnostic_tx_context_free(tx_ctx);
        tx_ctx = NULL;
    }
    esp_err_t volume_restore_err =
        restore_es8311_volume(previous_volume_db, "channel-map probe volume restore");
    if (result == ESP_OK && volume_restore_err != ESP_OK) {
        result = volume_restore_err;
    }
    ESP_LOGW(TAG,
             "P3-0C channel-map done result=%s expected_mask=0x%02lx observable_mask=0x%02lx "
             "weak_mask=0x%02lx mic3_ref_confirmed=%d mic2_confirmed=%d tx_frames=%llu tx_errors=%lu",
             esp_err_to_name(result),
             (unsigned long)expected_mask,
             (unsigned long)observable_mask,
             (unsigned long)weak_mask,
             mic3_ref_confirmed ? 1 : 0,
             mic2_confirmed ? 1 : 0,
             (unsigned long long)tx_frames,
             (unsigned long)tx_errors);
    return result;
}

esp_err_t nuwax_vocat_audio_board_ref_probe(nuwax_vocat_ref_probe_stats_t *out)
{
    ESP_RETURN_ON_FALSE(nuwax_audio_board_diagnostic_lease_is_held(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "REF probe requires exclusive diagnostic lease");
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_init(), TAG, "audio board init failed");
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_prepare_speaker_playback(),
                        TAG, "diagnostic PA prepare failed");
    nuwax_vocat_ref_probe_stats_t st = {0};

    /* P2-6 / P3-0 REF identification and health. HW model: slot0/1=mics,
     * slot2=MIC3(candidate AEC ref: ES8311 OUTP/N -> RC divider -> ES7210 MIC3),
     * slot3=MIC4(unused/NC). Slot2 is only a route candidate until runtime
     * ref_health and active-tone A/B both pass.
     * The RC network heavily attenuates the played signal, so probe LOUD:
     * full-scale tone + ES8311 volume to 0dB, else MIC3 stays near noise floor.
    * REF = slot with largest (tone-quiet) delta (expected slot2). */
    const float previous_volume_db = s_es8311_volume_db;
    const uint32_t overflow_before = s_rx_overflow_count;
    esp_err_t result = set_es8311_volume_db(0.0f, "REF probe volume");
    if (result != ESP_OK) {
        return result;
    }
    const int phase_ms = 1500;
    for (int phase = 0; phase < 2; phase++) {
        const bool tone = (phase == 0);
        double sumsq[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
        uint64_t n = 0;
        uint32_t fidx = 0;
        TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(phase_ms);
        for (int i = 0; i < 4; i++) {
            size_t b = 0;
            esp_err_t drain_err =
                i2s_channel_read(s_rx, s_rx_frame, sizeof(s_rx_frame), &b, 20);
            if (drain_err != ESP_OK && drain_err != ESP_ERR_TIMEOUT) {
                result = drain_err;
                goto ref_probe_cleanup;
            }
        }
        while ((int32_t)(end - xTaskGetTickCount()) > 0) {
            if (tone) {
                fill_tx_frame_loud(fidx++);
            } else {
                memset(s_tx_frame, 0, sizeof(s_tx_frame));
            }
            size_t wb = 0;
            esp_err_t write_err =
                i2s_channel_write(s_tx, s_tx_frame, sizeof(s_tx_frame), &wb, 100);
            if (write_err != ESP_OK || wb != sizeof(s_tx_frame)) {
                result = write_err == ESP_OK ? ESP_ERR_INVALID_SIZE : write_err;
                goto ref_probe_cleanup;
            }
            size_t rb = 0;
            esp_err_t read_err =
                i2s_channel_read(s_rx, s_rx_frame, sizeof(s_rx_frame), &rb, 100);
            if (read_err == ESP_OK) {
                size_t frames = rb / (sizeof(int16_t) * VOCAT_AUDIO_RX_SLOT_COUNT);
                for (size_t f = 0; f < frames; f++) {
                    for (size_t s = 0; s < VOCAT_AUDIO_RX_SLOT_COUNT; s++) {
                        int16_t v = s_rx_frame[f * VOCAT_AUDIO_RX_SLOT_COUNT + s];
                        sumsq[s] += (double)v * (double)v;
                    }
                }
                n += frames;
            } else if (read_err != ESP_ERR_TIMEOUT) {
                result = read_err;
                goto ref_probe_cleanup;
            }
        }
        if (n == 0) {
            result = ESP_ERR_TIMEOUT;
            goto ref_probe_cleanup;
        }
        for (int s = 0; s < VOCAT_AUDIO_RX_SLOT_COUNT; s++) {
            int rms = n ? (int)sqrt(sumsq[s] / (double)n) : 0;
            if (tone) st.tone_rms[s] = rms; else st.quiet_rms[s] = rms;
        }
        /* Dump raw interleaved slots of the last frame to see exactly what each
         * physical TDM slot carries (slot2 should be MIC3 = AEC reference). */
        if (tone) {
            ESP_LOGW(TAG, "P2-6 raw TDM (tone) [mic1 mic2 aecref nc] x4:");
            for (int f = 0; f < 4; f++) {
                ESP_LOGW(TAG, "  [%6d %6d %6d %6d]",
                         s_rx_frame[f * 4 + VOCAT_AUDIO_SLOT_MIC_PRIMARY],
                         s_rx_frame[f * 4 + VOCAT_AUDIO_SLOT_MIC_SECONDARY],
                         s_rx_frame[f * 4 + VOCAT_AUDIO_SLOT_AEC_REF],
                         s_rx_frame[f * 4 + VOCAT_AUDIO_SLOT_UNUSED]);
            }
        }
    }

    int best = -1, best_delta = 0;
    for (int s = 0; s < VOCAT_AUDIO_RX_SLOT_COUNT; s++) {
        int d = st.tone_rms[s] - st.quiet_rms[s];
        if (d > best_delta) { best_delta = d; best = s; }
    }
    st.ref_slot = best;
    st.ref_delta = best_delta;
ref_probe_cleanup:
    {
        esp_err_t restore_err =
            restore_es8311_volume(previous_volume_db, "REF probe volume restore");
        if (result == ESP_OK && restore_err != ESP_OK) {
            result = restore_err;
        }
    }
    if (result == ESP_OK && s_rx_overflow_count != overflow_before) {
        ESP_LOGE(TAG,
                 "REF probe rejected after RX DMA overflow before=%lu after=%lu",
                 (unsigned long)overflow_before,
                 (unsigned long)s_rx_overflow_count);
        result = ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGW(TAG, "P2-6/P3-0 REF probe(loud): tone_rms[m1=%d,m2=%d,REF=%d,NC=%d]"
             " quiet_rms[%d,%d,%d,%d] -> REF slot=%d delta=%d",
             st.tone_rms[0], st.tone_rms[1], st.tone_rms[2], st.tone_rms[3],
             st.quiet_rms[0], st.quiet_rms[1], st.quiet_rms[2], st.quiet_rms[3],
             st.ref_slot, st.ref_delta);
    if (out) {
        *out = st;
    }
    return result;
}

/*
 * P3-0: AEC reference health check.
 *
 * Plays a 1kHz calibration tone at the requested volume, simultaneously
 * captures slot0 (primary mic) + slot2 (AEC reference), and computes a
 * ref_health_score along with a discrete status.
 *
 * Implementation: two-phase (tone + quiet), with Goertzel-based 1kHz
 * amplitude extraction per slot, cross-correlation with the playback
 * waveform, clip-ratio counting, and a simple score heuristic.
 */
esp_err_t nuwax_vocat_aec_ref_health_check(float volume_db, uint32_t duration_ms,
                                           nuwax_aec_ref_health_t *out)
{
    ESP_RETURN_ON_FALSE(nuwax_audio_board_diagnostic_lease_is_held(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "AEC ref health requires exclusive diagnostic lease");
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    memset(out, 0, sizeof(*out));
    out->status = NUWAX_AEC_REF_HEALTH_NO_SIGNAL;

    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_init(), TAG, "audio board init failed");
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_prepare_speaker_playback(),
                        TAG, "diagnostic PA prepare failed");

    if (duration_ms < 500) duration_ms = 500;
    if (duration_ms > 10000) duration_ms = 10000;

    const float previous_volume_db = s_es8311_volume_db;
    const uint32_t overflow_before = s_rx_overflow_count;
    ESP_RETURN_ON_ERROR(set_es8311_volume_db(volume_db, "AEC ref health volume"),
                        TAG,
                        "set AEC ref health volume failed");

    /* One 10 ms Goertzel window at the active board sample rate. */
    const float goertzel_coeff =
        2.0f * cosf(2.0f * (float)M_PI * (float)VOCAT_AEC_TEST_TONE_HZ /
                    (float)VOCAT_AUDIO_SAMPLE_RATE_HZ);

    double slot_tone_amp_sum[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    uint64_t slot_tone_windows[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    double go_q0[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    double go_q1[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    uint32_t go_count[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    double slot_tone_sumsq[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    double slot_quiet_sumsq[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    uint64_t slot_tone_frames = 0;
    uint64_t slot_quiet_frames = 0;
    uint64_t slot2_clipped = 0;
    uint32_t io_errors = 0;
    esp_err_t last_io_err = ESP_OK;

    /* Cross-correlation accumulator. This scans a short lag window so the
     * score does not depend on the exact DMA/read scheduling phase. */
    int16_t tx_ring[VOCAT_AEC_REF_HEALTH_TX_RING_SAMPLES];
    memset(tx_ring, 0, sizeof(tx_ring));
    uint32_t tx_ring_pos = 0;
    double xcorr_num[VOCAT_AEC_REF_HEALTH_XCORR_MAX_LAG + 1] = {0};
    double xcorr_energy_tx[VOCAT_AEC_REF_HEALTH_XCORR_MAX_LAG + 1] = {0};
    double xcorr_energy_rx = 0.0;
    uint64_t xcorr_frames = 0;
    uint32_t tx_ref_sample_index = 0;

    health_tx_task_ctx_t *tx_ctx = diagnostic_tx_context_alloc(sizeof(*tx_ctx));
    if (!tx_ctx) {
        esp_err_t restore_err =
            restore_es8311_volume(previous_volume_db, "AEC ref health volume restore");
        return restore_err == ESP_OK ? ESP_ERR_NO_MEM : restore_err;
    }
    atomic_init(&tx_ctx->tone, false);
    BaseType_t task_ok = xTaskCreate(health_tx_task,
                                    "p3_health_tx",
                                    4096,
                                    tx_ctx,
                                    5,
                                    &tx_ctx->control.task);
    if (task_ok != pdPASS) {
        diagnostic_tx_context_free(tx_ctx);
        esp_err_t restore_err =
            restore_es8311_volume(previous_volume_db, "AEC ref health volume restore");
        return restore_err == ESP_OK ? ESP_ERR_NO_MEM : restore_err;
    }

    for (int phase = 0; phase < 2; phase++) {
        const bool tone = (phase == 0);
        atomic_store_explicit(&tx_ctx->tone, tone, memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(80));

        /* Drain stale RX */
        for (int i = 0; i < 4; i++) {
            size_t b = 0;
            esp_err_t drain_err =
                i2s_channel_read(s_rx, s_rx_frame, sizeof(s_rx_frame), &b, 20);
            if (drain_err != ESP_OK && drain_err != ESP_ERR_TIMEOUT) {
                io_errors++;
                last_io_err = drain_err;
            }
        }

        TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
        while ((int32_t)(end - xTaskGetTickCount()) > 0) {
            size_t rb = 0;
            esp_err_t rerr = i2s_channel_read(s_rx, s_rx_frame, sizeof(s_rx_frame), &rb, 500);
            if (rerr != ESP_OK || rb < sizeof(int16_t) * VOCAT_AUDIO_RX_SLOT_COUNT) {
                io_errors++;
                last_io_err = (rerr != ESP_OK) ? rerr : ESP_ERR_INVALID_SIZE;
                if (io_errors <= 5) {
                    ESP_LOGW(TAG, "AEC ref health RX read failed err=%s bytes=%u",
                             esp_err_to_name(rerr), (unsigned)rb);
                }
                continue;
            }

            size_t frames = rb / (sizeof(int16_t) * VOCAT_AUDIO_RX_SLOT_COUNT);
            for (size_t f = 0; f < frames; f++) {
                if (tone) {
                    int16_t v2 = s_rx_frame[f * VOCAT_AUDIO_RX_SLOT_COUNT + VOCAT_AUDIO_SLOT_AEC_REF];
                    const double dv2 = (double)v2;
                    slot_tone_frames++;

                    if (v2 <= -32767 || v2 >= 32767) {
                        slot2_clipped++;
                    }

                    for (size_t s = 0; s < VOCAT_AUDIO_RX_SLOT_COUNT; s++) {
                        int16_t v = s_rx_frame[f * VOCAT_AUDIO_RX_SLOT_COUNT + s];
                        const double dv = (double)v;
                        slot_tone_sumsq[s] += dv * dv;

                        double q2 = go_q1[s];
                        go_q1[s] = go_q0[s];
                        go_q0[s] = goertzel_coeff * go_q1[s] - q2 + dv;
                        go_count[s]++;
                        if (go_count[s] == VOCAT_AEC_REF_HEALTH_GOERTZEL_N) {
                            double power = go_q0[s] * go_q0[s] + go_q1[s] * go_q1[s] -
                                           goertzel_coeff * go_q0[s] * go_q1[s];
                            if (power < 0.0) {
                                power = 0.0;
                            }
                            slot_tone_amp_sum[s] += sqrt(power) / (double)VOCAT_AEC_REF_HEALTH_GOERTZEL_N;
                            slot_tone_windows[s]++;
                            go_q0[s] = 0.0;
                            go_q1[s] = 0.0;
                            go_count[s] = 0;
                        }
                    }

                    tx_ring[tx_ring_pos] =
                        tone_1khz_sample(tx_ref_sample_index++, VOCAT_AEC_TEST_TONE_AMPLITUDE);
                    tx_ring_pos = (tx_ring_pos + 1) % VOCAT_AEC_REF_HEALTH_TX_RING_SAMPLES;
                    xcorr_energy_rx += dv2 * dv2;
                    xcorr_frames++;
                    for (uint32_t lag = 0; lag <= VOCAT_AEC_REF_HEALTH_XCORR_MAX_LAG; lag++) {
                        uint32_t idx = (tx_ring_pos + VOCAT_AEC_REF_HEALTH_TX_RING_SAMPLES - 1U - lag) %
                                       VOCAT_AEC_REF_HEALTH_TX_RING_SAMPLES;
                        double tx_val = (double)tx_ring[idx];
                        xcorr_num[lag] += dv2 * tx_val;
                        xcorr_energy_tx[lag] += tx_val * tx_val;
                    }
                } else {
                    slot_quiet_frames++;
                    for (size_t s = 0; s < VOCAT_AUDIO_RX_SLOT_COUNT; s++) {
                        int16_t v = s_rx_frame[f * VOCAT_AUDIO_RX_SLOT_COUNT + s];
                        slot_quiet_sumsq[s] += (double)v * (double)v;
                    }
                }
            }
        }
    }

    diagnostic_tx_stop_and_reap(&tx_ctx->control, "P3-0 health");
    const uint64_t tx_frames = tx_ctx->control.frames;
    const uint32_t tx_errors = tx_ctx->control.errors;
    diagnostic_tx_context_free(tx_ctx);
    tx_ctx = NULL;

    esp_err_t restore_err =
        restore_es8311_volume(previous_volume_db, "AEC ref health volume restore");
    if (restore_err != ESP_OK) {
        io_errors++;
        last_io_err = restore_err;
    }
    if (s_rx_overflow_count != overflow_before) {
        ESP_LOGE(TAG,
                 "AEC ref health rejected after RX DMA overflow before=%lu after=%lu",
                 (unsigned long)overflow_before,
                 (unsigned long)s_rx_overflow_count);
        io_errors++;
        last_io_err = ESP_ERR_INVALID_RESPONSE;
    }

    if (tx_errors > 0) {
        io_errors += tx_errors;
        last_io_err = ESP_ERR_TIMEOUT;
    }

    if (slot_tone_frames == 0 || slot_quiet_frames == 0) {
        ESP_LOGE(TAG, "AEC ref health has no usable samples tone=%llu quiet=%llu io_errors=%lu tx_errors=%lu",
                 (unsigned long long)slot_tone_frames,
                 (unsigned long long)slot_quiet_frames,
                 (unsigned long)io_errors,
                 (unsigned long)tx_errors);
        return last_io_err != ESP_OK ? last_io_err : ESP_ERR_INVALID_RESPONSE;
    }

    /* Compute output metrics */
    int tone_rms[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    int quiet_rms[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    float tone_amp[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    for (size_t s = 0; s < VOCAT_AUDIO_RX_SLOT_COUNT; s++) {
        tone_rms[s] = (int)sqrt(slot_tone_sumsq[s] / (double)slot_tone_frames);
        quiet_rms[s] = (int)sqrt(slot_quiet_sumsq[s] / (double)slot_quiet_frames);
        tone_amp[s] = slot_tone_windows[s] > 0
            ? (float)(slot_tone_amp_sum[s] / (double)slot_tone_windows[s]) : 0.0f;
    }
    out->slot2_tone_amplitude = tone_amp[VOCAT_AUDIO_SLOT_AEC_REF];
    out->slot2_quiet_rms = (float)sqrt(slot_quiet_sumsq[VOCAT_AUDIO_SLOT_AEC_REF] /
                                       (double)slot_quiet_frames);
    out->slot2_clip_ratio = (float)((double)slot2_clipped / (double)slot_tone_frames);
    out->slot0_rms = tone_rms[VOCAT_AUDIO_SLOT_MIC_PRIMARY];

    double best_corr = 0.0;
    int best_lag = 0;
    if (xcorr_frames > 0 && xcorr_energy_rx > 0.0) {
        for (uint32_t lag = 0; lag <= VOCAT_AEC_REF_HEALTH_XCORR_MAX_LAG; lag++) {
            double denom = sqrt(xcorr_energy_tx[lag] * xcorr_energy_rx);
            if (denom <= 0.0) {
                continue;
            }
            double corr = fabs(xcorr_num[lag] / denom);
            if (corr > best_corr) {
                best_corr = corr;
                best_lag = (int)lag;
            }
        }
    }
    out->slot2_correlation = (float)best_corr;
    out->estimated_delay_samples = best_lag;

    /* Score heuristic (0.0 .. 1.0):
     *   - amplitude component: map tone_amplitude to [0,1] with saturation at 500
     *   - correlation component: xcorr_peak directly (should be high for valid ref)
     *   - penalty for clipping
     *   - penalty for high quiet noise
     */
    float amp_score = out->slot2_tone_amplitude / 500.0f;
    if (amp_score > 1.0f) amp_score = 1.0f;
    float corr_score = out->slot2_correlation > 0.0f ? out->slot2_correlation : 0.0f;
    if (corr_score > 1.0f) corr_score = 1.0f;
    float clip_penalty = out->slot2_clip_ratio > 0.02f ? 0.5f : 0.0f;
    float noise_penalty = out->slot2_quiet_rms > 50.0f ? 0.3f : 0.0f;
    out->score = amp_score * 0.4f + corr_score * 0.4f - clip_penalty - noise_penalty;
    if (out->score < 0.0f) out->score = 0.0f;
    if (out->score > 1.0f) out->score = 1.0f;

    /* Discrete status classification */
    if (out->slot2_tone_amplitude < 10.0f && out->slot0_rms < 10) {
        out->status = NUWAX_AEC_REF_HEALTH_PA_MUTED;
    } else if (out->slot2_tone_amplitude < 10.0f) {
        out->status = NUWAX_AEC_REF_HEALTH_NO_SIGNAL;
    } else if (out->slot2_clip_ratio > 0.02f) {
        out->status = NUWAX_AEC_REF_HEALTH_CLIPPED;
    } else if (out->slot2_quiet_rms > 50.0f) {
        out->status = NUWAX_AEC_REF_HEALTH_NOISY;
    } else {
        out->status = NUWAX_AEC_REF_HEALTH_OK;
    }

    float threshold = (float)CONFIG_NUWAX_P3_AEC_REF_HEALTH_SCORE_THRESHOLD / 1000.0f;
    if (threshold < 0.0f) threshold = 0.0f;
    if (threshold > 1.0f) threshold = 1.0f;
    out->passed = (out->status == NUWAX_AEC_REF_HEALTH_OK && out->score >= threshold);

    ESP_LOGW(TAG, "P3-0 AEC ref health slot sweep: tone_rms[m1=%d,m2=%d,ref=%d,nc=%d] "
             "quiet_rms[%d,%d,%d,%d] amp1k[%.1f,%.1f,%.1f,%.1f] tx_frames=%llu rx_tone=%llu rx_quiet=%llu",
             tone_rms[0], tone_rms[1], tone_rms[2], tone_rms[3],
             quiet_rms[0], quiet_rms[1], quiet_rms[2], quiet_rms[3],
             (double)tone_amp[0], (double)tone_amp[1], (double)tone_amp[2], (double)tone_amp[3],
             (unsigned long long)tx_frames,
             (unsigned long long)slot_tone_frames,
             (unsigned long long)slot_quiet_frames);

    if (io_errors > 0) {
        ESP_LOGW(TAG, "AEC ref health completed with io_errors=%lu last=%s",
                 (unsigned long)io_errors, esp_err_to_name(last_io_err));
    }

    ESP_LOGI(TAG, "P3-0 AEC ref health: status=%d score=%.3f threshold=%.3f tone_amp=%.1f quiet_rms=%.1f "
             "corr=%.3f delay=%d clip=%.3f slot0_rms=%d passed=%d",
             out->status, (double)out->score, (double)threshold, (double)out->slot2_tone_amplitude,
             (double)out->slot2_quiet_rms, (double)out->slot2_correlation,
             out->estimated_delay_samples, (double)out->slot2_clip_ratio, out->slot0_rms, out->passed);

    return io_errors == 0 ? ESP_OK :
           (last_io_err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : last_io_err);
}

esp_err_t nuwax_vocat_audio_board_smoke_test(uint32_t duration_ms, nuwax_vocat_audio_smoke_stats_t *out_stats)
{
    ESP_RETURN_ON_FALSE(nuwax_audio_board_diagnostic_lease_is_held(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "audio smoke requires exclusive diagnostic lease");
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_init(), TAG, "audio board init failed");
    ESP_RETURN_ON_ERROR(nuwax_vocat_audio_board_prepare_speaker_playback(),
                        TAG, "diagnostic PA prepare failed");
    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
    }

    double slot_sumsq[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    int slot_peak[VOCAT_AUDIO_RX_SLOT_COUNT] = {0};
    uint64_t slot_n = 0;
    uint32_t rx_errors = 0;
    uint64_t rx_frames = 0;
    smoke_tx_task_ctx_t *tx_ctx = diagnostic_tx_context_alloc(sizeof(*tx_ctx));
    ESP_RETURN_ON_FALSE(tx_ctx, ESP_ERR_NO_MEM, TAG, "allocate P2 smoke TX context failed");
    BaseType_t task_ok = xTaskCreate(smoke_tx_task,
                                    "p2_smoke_tx",
                                    4096,
                                    tx_ctx,
                                    5,
                                    &tx_ctx->control.task);
    if (task_ok != pdPASS) {
        diagnostic_tx_context_free(tx_ctx);
        return ESP_ERR_NO_MEM;
    }
    drain_rx_dma();

    TickType_t start = xTaskGetTickCount();
    TickType_t end = start + pdMS_TO_TICKS(duration_ms);
    if (end == start) {
        end = start + 1;
    }

    /* P2-2 drift monitor: sample windowed RX rate to prove no drift over time. */
    const uint32_t window_ms = 2000;
    TickType_t window_start = start;
    uint64_t window_frames = 0;
    uint32_t window_idx = 0;
    uint32_t worst_window_permille = 0;

    while ((int32_t)(end - xTaskGetTickCount()) > 0) {
        memset(s_rx_frame, 0, sizeof(s_rx_frame));
        size_t bytes_done = 0;
        esp_err_t rrc = i2s_channel_read(s_rx, s_rx_frame, sizeof(s_rx_frame), &bytes_done, 1000);
        size_t frames_read = bytes_done / (sizeof(int16_t) * VOCAT_AUDIO_RX_SLOT_COUNT);
        if (rrc == ESP_OK && frames_read > 0) {
            rx_frames += frames_read;
            window_frames += frames_read;
            for (size_t f = 0; f < frames_read; f++) {
                for (size_t s = 0; s < VOCAT_AUDIO_RX_SLOT_COUNT; s++) {
                    int16_t v = s_rx_frame[f * VOCAT_AUDIO_RX_SLOT_COUNT + s];
                    slot_sumsq[s] += (double)v * (double)v;
                    int a = v < 0 ? -v : v;
                    if (a > slot_peak[s]) {
                        slot_peak[s] = a;
                    }
                }
            }
            slot_n += frames_read;
        } else {
            ESP_LOGW(TAG, "P2 RX read failed err=%s bytes=%u", esp_err_to_name(rrc), (unsigned)bytes_done);
            rx_errors++;
        }

        uint32_t win_elapsed = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - window_start);
        if (win_elapsed >= window_ms) {
            uint32_t win_rate = (uint32_t)((window_frames * 1000ULL + win_elapsed / 2U) / win_elapsed);
            uint32_t win_delta = rate_delta_hz(win_rate, VOCAT_AUDIO_SAMPLE_RATE_HZ);
            uint32_t win_permille = (win_delta * 1000U + VOCAT_AUDIO_SAMPLE_RATE_HZ / 2U) / VOCAT_AUDIO_SAMPLE_RATE_HZ;
            if (win_permille > worst_window_permille) {
                worst_window_permille = win_permille;
            }
            ESP_LOGI(TAG, "P2-2 drift window=%lu rx_rate=%luHz err=%lupermille worst=%lupermille",
                     (unsigned long)window_idx, (unsigned long)win_rate,
                     (unsigned long)win_permille, (unsigned long)worst_window_permille);
            window_idx++;
            window_frames = 0;
            window_start = xTaskGetTickCount();
        }
    }
    diagnostic_tx_stop_and_reap(&tx_ctx->control, "P2 smoke");
    const uint64_t tx_frames = tx_ctx->control.frames;
    const uint32_t tx_errors = tx_ctx->control.errors;
    diagnostic_tx_context_free(tx_ctx);
    tx_ctx = NULL;

    TickType_t finish = xTaskGetTickCount();
    uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(finish - start);
    if (elapsed_ms == 0) {
        elapsed_ms = 1;
    }

    nuwax_vocat_audio_smoke_stats_t stats = {
        .elapsed_ms = elapsed_ms,
        .tx_errors = tx_errors,
        .rx_errors = rx_errors,
        .tx_frames = tx_frames,
        .rx_frames = rx_frames,
    };
    for (size_t s = 0; s < VOCAT_AUDIO_RX_SLOT_COUNT; s++) {
        stats.slot_rms[s] = slot_n ? (int)sqrt(slot_sumsq[s] / (double)slot_n) : 0;
        stats.slot_peak[s] = slot_peak[s];
        if (slot_peak[s] > 0) {
            stats.signal_present = true;
        }
    }
    stats.rx_rate_hz = (uint32_t)((stats.rx_frames * 1000ULL + (elapsed_ms / 2U)) / elapsed_ms);
    uint32_t rx_rate_delta = rate_delta_hz(stats.rx_rate_hz, VOCAT_AUDIO_SAMPLE_RATE_HZ);
    stats.rx_rate_error_permille =
        (rx_rate_delta * 1000U + (VOCAT_AUDIO_SAMPLE_RATE_HZ / 2U)) / VOCAT_AUDIO_SAMPLE_RATE_HZ;
    stats.rx_rate_ok = rx_rate_delta <= VOCAT_AUDIO_RX_RATE_TOLERANCE_HZ;

    ESP_LOGI(TAG,
             "P2 audio smoke done requested_ms=%lu elapsed_ms=%lu tx_frames=%llu rx_frames=%llu rx_rate=%luHz rate_err=%lupermille rate_ok=%d signal=%d tx_err=%lu rx_err=%lu rms=%d/%d/%d/%d peak=%d/%d/%d/%d dma_free=%u dma_largest=%u",
             (unsigned long)duration_ms,
             (unsigned long)stats.elapsed_ms,
             (unsigned long long)stats.tx_frames,
             (unsigned long long)stats.rx_frames,
             (unsigned long)stats.rx_rate_hz,
             (unsigned long)stats.rx_rate_error_permille,
             stats.rx_rate_ok,
             stats.signal_present,
             (unsigned long)stats.tx_errors,
             (unsigned long)stats.rx_errors,
             stats.slot_rms[0],
             stats.slot_rms[1],
             stats.slot_rms[2],
             stats.slot_rms[3],
             stats.slot_peak[0],
             stats.slot_peak[1],
             stats.slot_peak[2],
             stats.slot_peak[3],
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (out_stats) {
        *out_stats = stats;
    }
    return (stats.tx_errors == 0 && stats.rx_errors == 0 && stats.rx_rate_ok && stats.signal_present)
               ? ESP_OK
               : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t nuwax_vocat_audio_board_deinit(void)
{
    if (!s_es8311_if && !s_es7210_if && !s_es8311_ctrl && !s_es7210_ctrl &&
        !s_gpio_if && !s_tx && !s_rx) {
        s_initialized = false;
        return ESP_OK;
    }

    /* Fail closed before teardown. ESP-Codec delete APIs free their object
     * even when close reports an error, so ownership pointers are cleared
     * immediately after each delete call and must never be retried. */
    esp_err_t err = nuwax_vocat_audio_board_force_speaker_mute();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PA force-off blocked audio teardown: %s", esp_err_to_name(err));
        return err;
    }
    if (s_es8311_if && s_es8311_if->enable) {
        err = codec_dev_ret_to_err(s_es8311_if->enable(s_es8311_if, false),
                                   "ES8311 disable during teardown");
        if (err != ESP_OK) {
            return err;
        }
        err = nuwax_vocat_audio_board_force_speaker_mute();
        if (err != ESP_OK) {
            return err;
        }
    }

    /* Codec objects depend on control/GPIO objects and active I2S clocks. */
    if (s_es8311_if) {
        int rc = audio_codec_delete_codec_if(s_es8311_if);
        s_es8311_if = NULL;
        if (rc != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES8311 codec interface delete failed rc=%d", rc);
            return ESP_FAIL;
        }
    }
    if (s_es7210_if) {
        int rc = audio_codec_delete_codec_if(s_es7210_if);
        s_es7210_if = NULL;
        if (rc != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES7210 codec interface delete failed rc=%d", rc);
            return ESP_FAIL;
        }
    }
    if (s_es8311_ctrl) {
        bool deleted = false;
        int rc = delete_codec_i2c_ctrl_guarded(s_es8311_ctrl, &deleted);
        if (deleted) {
            s_es8311_ctrl = NULL;
            memset(&s_es8311_ctrl_guard, 0, sizeof(s_es8311_ctrl_guard));
        }
        if (rc != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES8311 control interface delete failed rc=%d", rc);
            return ESP_FAIL;
        }
    }
    if (s_es7210_ctrl) {
        bool deleted = false;
        int rc = delete_codec_i2c_ctrl_guarded(s_es7210_ctrl, &deleted);
        if (deleted) {
            s_es7210_ctrl = NULL;
            memset(&s_es7210_ctrl_guard, 0, sizeof(s_es7210_ctrl_guard));
        }
        if (rc != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES7210 control interface delete failed rc=%d", rc);
            return ESP_FAIL;
        }
    }
    if (s_gpio_if) {
        int rc = audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
        if (rc != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec GPIO interface delete failed rc=%d", rc);
            return ESP_FAIL;
        }
    }
    if (s_tx) {
        err = i2s_channel_disable(s_tx);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "TX disable failed during teardown: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (s_rx) {
        err = i2s_channel_disable(s_rx);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "RX disable failed during teardown: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (s_tx) {
        err = i2s_del_channel(s_tx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TX delete failed during teardown: %s", esp_err_to_name(err));
            return err;
        }
        s_tx = NULL;
    }
    if (s_rx) {
        err = i2s_del_channel(s_rx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RX delete failed during teardown: %s", esp_err_to_name(err));
            return err;
        }
        s_rx = NULL;
    }
    s_initialized = false;
    s_pa_forced_off = true;
    s_es8311_volume_db = VOCAT_ES8311_DEFAULT_VOLUME_DB;
    return ESP_OK;
}

#else

esp_err_t nuwax_vocat_audio_board_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_vocat_audio_board_smoke_test(uint32_t duration_ms, nuwax_vocat_audio_smoke_stats_t *out_stats)
{
    (void)duration_ms;
    (void)out_stats;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_vocat_audio_board_deinit(void)
{
    return ESP_OK;
}

void *nuwax_vocat_audio_board_get_rx_handle(void) { return NULL; }
void *nuwax_vocat_audio_board_get_tx_handle(void) { return NULL; }
uint32_t nuwax_vocat_audio_board_get_rx_overflow_count(void) { return 0; }
esp_err_t nuwax_vocat_audio_board_force_speaker_mute(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t nuwax_vocat_audio_board_prepare_speaker_playback(void) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t nuwax_vocat_audio_board_playback_stress(int cycles, nuwax_vocat_playback_stress_stats_t *out)
{
    (void)cycles;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_vocat_audio_board_test_tone_start(nuwax_vocat_test_tone_handle_t **out_handle)
{
    (void)out_handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_vocat_audio_board_test_tone_stop(nuwax_vocat_test_tone_handle_t *handle)
{
    (void)handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_vocat_audio_board_ref_probe(nuwax_vocat_ref_probe_stats_t *out)
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_vocat_aec_ref_health_check(float volume_db, uint32_t duration_ms,
                                           nuwax_aec_ref_health_t *out)
{
    (void)volume_db;
    (void)duration_ms;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
