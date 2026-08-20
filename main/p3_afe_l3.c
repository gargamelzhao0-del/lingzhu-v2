#include "p3_afe_l3.h"

#include <string.h>

#include "esp_afe_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "p3_software_ref.h"

static const char *TAG = "p3_l3";
static const char *NVS_NS = "nuwax_l3";
static const char *NVS_KEY = "en";

static bool s_l3_enabled;
static bool s_afe_inited;
static uint32_t s_last_log_frames;
static void *s_aec_handle;
static int s_aec_chunksize;
/* Before processing, the first chunk_samples entries hold pending mic PCM.
 * They are expanded backwards into M|R interleaved input for ESP AEC. */
static int16_t *s_mr_buf;
static int16_t *s_out_buf;
static int16_t *s_ref_buf;
static size_t s_pending_mic_samples;
static size_t s_output_read_index;
static size_t s_output_samples;
static size_t s_warmup_samples;
static nuwax_p3_l3_stats_t s_stats;

static void *alloc_l3_audio_buffer(size_t bytes) {
    void *ptr = heap_caps_aligned_alloc(
        16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void reset_stream_state(void) {
    s_pending_mic_samples = 0U;
    s_output_read_index = 0U;
    s_output_samples = 0U;
    s_warmup_samples = s_aec_chunksize > 0 ? (size_t)s_aec_chunksize : 0U;
}

static esp_err_t ensure_afe_inited(void) {
    if (s_afe_inited) {
        return ESP_OK;
    }
    s_aec_handle = afe_aec_create("MR", 4, AFE_TYPE_VC, AFE_MODE_LOW_COST);
    if (!s_aec_handle) {
        ESP_LOGE(TAG, "afe_aec_create failed; L3 enable rejected");
        return ESP_FAIL;
    }
    s_aec_chunksize = afe_aec_get_chunksize(s_aec_handle);
    if (s_aec_chunksize <= 0) {
        ESP_LOGE(TAG, "AFE returned invalid chunk size=%d", s_aec_chunksize);
        afe_aec_destroy(s_aec_handle);
        s_aec_handle = NULL;
        s_aec_chunksize = 0;
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t chunk_samples = (size_t)s_aec_chunksize;
    s_mr_buf = alloc_l3_audio_buffer(
        chunk_samples * 2U * sizeof(int16_t));
    s_out_buf = alloc_l3_audio_buffer(chunk_samples * sizeof(int16_t));
    s_ref_buf = alloc_l3_audio_buffer(chunk_samples * sizeof(int16_t));
    if (!s_mr_buf || !s_out_buf || !s_ref_buf) {
        ESP_LOGE(TAG,
                 "L3 buffer allocation failed mr=%p out=%p ref=%p",
                 s_mr_buf,
                 s_out_buf,
                 s_ref_buf);
        heap_caps_free(s_mr_buf);
        heap_caps_free(s_out_buf);
        heap_caps_free(s_ref_buf);
        s_mr_buf = NULL;
        s_out_buf = NULL;
        s_ref_buf = NULL;
        afe_aec_destroy(s_aec_handle);
        s_aec_handle = NULL;
        s_aec_chunksize = 0;
        return ESP_ERR_NO_MEM;
    }
    reset_stream_state();
    s_afe_inited = true;
    ESP_LOGI(TAG, "AFE MR initialized chunk_samples=%d", s_aec_chunksize);
    return ESP_OK;
}

static void teardown_afe(void) {
    if (s_aec_handle) {
        afe_aec_destroy(s_aec_handle);
    }
    heap_caps_free(s_mr_buf);
    heap_caps_free(s_out_buf);
    heap_caps_free(s_ref_buf);
    s_aec_handle = NULL;
    s_mr_buf = NULL;
    s_out_buf = NULL;
    s_ref_buf = NULL;
    s_aec_chunksize = 0;
    s_afe_inited = false;
    reset_stream_state();
}

esp_err_t nuwax_p3_l3_init(void) {
    nvs_handle_t handle;
    const esp_err_t open_err = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (open_err == ESP_OK) {
        uint8_t enabled = 0U;
        const esp_err_t get_err = nvs_get_u8(handle, NVS_KEY, &enabled);
        if (get_err == ESP_OK) {
            s_l3_enabled = enabled != 0U;
        } else if (get_err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "L3 NVS read failed: %s", esp_err_to_name(get_err));
        }
        nvs_close(handle);
    } else if (open_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "L3 NVS open failed: %s", esp_err_to_name(open_err));
    }

    if (s_l3_enabled) {
        const esp_err_t init_err = ensure_afe_inited();
        if (init_err != ESP_OK) {
            s_l3_enabled = false;
            ESP_LOGE(TAG,
                     "persisted L3 disabled for this boot because AFE init failed: %s",
                     esp_err_to_name(init_err));
        }
    }
    s_stats.en = s_l3_enabled;
    ESP_LOGI(TAG, "init enabled=%d", s_l3_enabled ? 1 : 0);
    return ESP_OK;
}

bool nuwax_p3_l3_is_enabled(void) {
    return s_l3_enabled;
}

esp_err_t nuwax_p3_l3_set_enabled(bool enabled, bool persist) {
    if (!persist && enabled == s_l3_enabled &&
        (!enabled || s_afe_inited)) {
        return ESP_OK;
    }
    if (enabled) {
        const esp_err_t init_err = ensure_afe_inited();
        if (init_err != ESP_OK) {
            s_l3_enabled = false;
            s_stats.en = false;
            ESP_LOGE(TAG,
                     "L3 enable rejected; AFE init failed: %s",
                     esp_err_to_name(init_err));
            return init_err;
        }
    } else {
        teardown_afe();
    }
    s_l3_enabled = enabled;
    s_stats.en = enabled;

    if (persist) {
        nvs_handle_t handle;
        esp_err_t persist_err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
        if (persist_err == ESP_OK) {
            persist_err = nvs_set_u8(handle, NVS_KEY, enabled ? 1U : 0U);
            if (persist_err == ESP_OK) {
                persist_err = nvs_commit(handle);
            }
            nvs_close(handle);
        }
        if (persist_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "L3 %s at runtime but NVS persistence failed: %s",
                     enabled ? "enabled" : "disabled",
                     esp_err_to_name(persist_err));
            return persist_err;
        }
        ESP_LOGI(TAG,
                 "L3 %s persisted to NVS",
                 enabled ? "enabled" : "disabled");
    } else {
        ESP_LOGW(TAG,
                 "L3 %s for this runtime only",
                 enabled ? "enabled" : "disabled");
    }
    return ESP_OK;
}

esp_err_t nuwax_p3_l3_set_enabled_runtime(bool enabled) {
    ESP_LOGI(TAG, "set_enabled_runtime=%d", enabled ? 1 : 0);
    return nuwax_p3_l3_set_enabled(enabled, false);
}

esp_err_t nuwax_p3_l3_process_chunk(int16_t *samples, size_t frames) {
    if (!samples && frames > 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    s_stats.f += frames;
    if (!s_l3_enabled) {
        s_stats.b += frames;
        return ESP_OK;
    }

    const esp_err_t init_err = ensure_afe_inited();
    if (init_err != ESP_OK) {
        /* Do not retry a failed allocator on every real-time capture frame. */
        s_l3_enabled = false;
        s_stats.en = false;
        s_stats.b += frames;
        s_stats.e++;
        return init_err;
    }

    const size_t chunk_samples = (size_t)s_aec_chunksize;
    uint32_t processed_chunks = 0U;
    for (size_t frame = 0U; frame < frames; ++frame) {
        const int16_t mic_sample = samples[frame];

        if (s_warmup_samples > 0U) {
            samples[frame] = 0;
            s_warmup_samples--;
        } else if (s_output_samples > 0U) {
            samples[frame] = s_out_buf[s_output_read_index++];
            s_output_samples--;
            if (s_output_samples == 0U) {
                s_output_read_index = 0U;
            }
        } else {
            samples[frame] = 0;
            s_stats.e++;
            ESP_LOGE(TAG, "L3 stream output underrun");
        }

        s_mr_buf[s_pending_mic_samples++] = mic_sample;
        if (s_pending_mic_samples != chunk_samples) {
            continue;
        }
        if (s_output_samples != 0U) {
            s_stats.e++;
            ESP_LOGE(TAG,
                     "L3 stream invariant recovered pending_output=%u",
                     (unsigned)s_output_samples);
            s_output_read_index = 0U;
            s_output_samples = 0U;
        }

        /* Reference delay is zero because the playback path pre-feeds the
         * software reference. Reading when the mic chunk is complete aligns
         * both chunks at their current end timestamp. */
        const size_t copied = nuwax_p3_software_ref_read_delayed(
            s_ref_buf, chunk_samples, 0U);
        if (copied < chunk_samples) {
            memset(s_ref_buf + copied,
                   0,
                   (chunk_samples - copied) * sizeof(int16_t));
        }
        for (size_t i = chunk_samples; i-- > 0U;) {
            const int16_t pending_mic = s_mr_buf[i];
            s_mr_buf[i * 2U] = pending_mic;
            s_mr_buf[i * 2U + 1U] = s_ref_buf[i];
        }
        afe_aec_process(s_aec_handle, s_mr_buf, s_out_buf);
        s_pending_mic_samples = 0U;
        s_output_read_index = 0U;
        s_output_samples = chunk_samples;
        processed_chunks++;
    }

    if (s_stats.f - s_last_log_frames >= 16000U) {
        ESP_LOGI(TAG,
                 "L3 stats frames=%u bypassed=%u processed_chunks=%u errors=%u free_internal=%u",
                 (unsigned)s_stats.f,
                 (unsigned)s_stats.b,
                 (unsigned)processed_chunks,
                 (unsigned)s_stats.e,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        s_last_log_frames = s_stats.f;
    }
    return ESP_OK;
}

void nuwax_p3_l3_get_stats(nuwax_p3_l3_stats_t *out) {
    if (out) {
        *out = s_stats;
        out->en = s_l3_enabled;
    }
}

void nuwax_p3_l3_reset_stats(void) {
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.en = s_l3_enabled;
}
