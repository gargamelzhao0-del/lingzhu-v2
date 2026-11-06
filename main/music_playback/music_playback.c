#include "music_playback.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "board/audio_board.h"
#include "conversation/speaker_playback.h"
#include "driver/i2s_std.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "session_protocol.h"
#include "mp3dec.h"
#include "lwip/sockets.h"

#define MUSIC_BUFFER_BYTES (96U * 1024U)
#define MUSIC_INPUT_BYTES 8192U
#define MUSIC_FIRST_AUDIO_TIMEOUT_US 10000000LL
static const char *TAG = "nuwax_music";
static atomic_bool s_active;
static atomic_bool s_audio_active;
static atomic_bool s_stop;
static atomic_bool s_output_quiet = true;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static nuwax_music_snapshot_t s_snapshot;

typedef struct {
    nuwax_ws_client_t *ws;
    uint32_t epoch;
    char id[80];
    char url[NUWAX_SESSION_MUSIC_URL_MAX_LEN];
    bool pcm, local;
    StreamBufferHandle_t compressed;
    StaticStreamBuffer_t stream_control;
    uint8_t *storage;
    atomic_bool producer_done;
    esp_err_t producer_result;
    atomic_uint received;
    nuwax_audio_board_ref_t ref;
    nuwax_audio_board_lease_t lease;
    void *tx;
    uint32_t rate;
    uint64_t frames;
    int16_t last_sample;
    bool unmuted;
    int64_t first_audio_deadline_us;
} music_job_t;

static bool cancelled(void) { return atomic_load_explicit(&s_stop, memory_order_acquire); }
static void phase(const char *state, uint32_t rate) {
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_snapshot.state, state, sizeof(s_snapshot.state));
    s_snapshot.sample_rate = rate;
    portEXIT_CRITICAL(&s_lock);
}
void nuwax_music_playback_snapshot(nuwax_music_snapshot_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}
static void report(music_job_t *job, const char *state, const char *reason) {
    portENTER_CRITICAL(&s_lock);
    if (strcmp(state, "playing") == 0 && cancelled()) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    strlcpy(s_snapshot.state, state, sizeof(s_snapshot.state));
    s_snapshot.sample_rate = job->rate;
    strlcpy(s_snapshot.reason, reason ? reason : "", sizeof(s_snapshot.reason));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "music status request=%s state=%s reason=%s rate=%lu frames=%llu bytes=%u",
             job->id, state, reason ? reason : "-", (unsigned long)job->rate,
             (unsigned long long)job->frames, atomic_load(&job->received));
    if (!job->local)
        nuwax_music_status_post(job->ws, job->epoch, job->id, state, reason);
}

/* Runs on the HTTP owner, including inside a read that is receiving a slow
 * trickle. Shutdown only the socket; leave parser/handle cleanup to that owner. */
static esp_err_t download_event(esp_http_client_event_t *event) {
    if (cancelled() && (event->event_id == HTTP_EVENT_ON_CONNECTED ||
                       event->event_id == HTTP_EVENT_ON_HEADER ||
                       event->event_id == HTTP_EVENT_ON_DATA)) {
        const int fd = esp_http_client_get_socket(event->client);
        if (fd >= 0) (void)shutdown(fd, SHUT_RDWR);
    }
    return ESP_OK;
}

/* Only this task touches the HTTP handle. A separate decoder/output task
 * can stop audio without waiting for a DNS/TLS/read operation to return. */
static void download_task(void *arg) {
    music_job_t *job = arg;
    esp_http_client_config_t config = {
        .url = job->url, .method = HTTP_METHOD_GET, .timeout_ms = 1500,
        .disable_auto_redirect = true, .buffer_size = 4096, .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = download_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    uint8_t *buffer = heap_caps_malloc(4096U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t result = ESP_ERR_NO_MEM;
    if (!client || !buffer) goto done;
    if (cancelled()) { result = ESP_ERR_INVALID_STATE; goto done; }
    result = esp_http_client_open(client, 0);
    if (result != ESP_OK) goto done;
    const int64_t length = esp_http_client_fetch_headers(client);
    const bool chunked = esp_http_client_is_chunked_response(client);
    if (length < 0 || esp_http_client_get_status_code(client) != 200 || (length == 0 && !chunked) ||
        length > CONFIG_NUWAX_MUSIC_MAX_BYTES) {
        result = ESP_ERR_INVALID_RESPONSE; goto done;
    }
    esp_http_client_set_timeout_ms(client, 200);
    int64_t last_data = esp_timer_get_time();
    while (!cancelled()) {
        const int n = esp_http_client_read(client, (char *)buffer, 4096);
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            if (esp_timer_get_time() - last_data >
                (int64_t)CONFIG_NUWAX_MUSIC_HTTP_TIMEOUT_MS * 1000LL) {
                result = ESP_ERR_TIMEOUT; break;
            }
            continue;
        }
        if (n < 0) { result = ESP_FAIL; break; }
        if (n == 0) {
            result = esp_http_client_is_complete_data_received(client) &&
                     (chunked || atomic_load(&job->received) == (size_t)length)
                         ? ESP_OK : ESP_ERR_INVALID_SIZE;
            break;
        }
        last_data = esp_timer_get_time();
        const unsigned received = atomic_fetch_add(&job->received, (unsigned)n) + (unsigned)n;
        if (received > CONFIG_NUWAX_MUSIC_MAX_BYTES) { result = ESP_ERR_INVALID_SIZE; break; }
        size_t offset = 0;
        while (offset < (size_t)n && !cancelled())
            offset += xStreamBufferSend(job->compressed, buffer + offset,
                                         (size_t)n - offset, pdMS_TO_TICKS(20));
    }
done:
    if (client) esp_http_client_cleanup(client);
    heap_caps_free(buffer);
    job->producer_result = cancelled() ? ESP_ERR_INVALID_STATE : result;
    atomic_store_explicit(&job->producer_done, true, memory_order_release);
    vTaskDeleteWithCaps(NULL);
}
static bool input_done(music_job_t *job) {
    return atomic_load_explicit(&job->producer_done, memory_order_acquire) &&
           xStreamBufferBytesAvailable(job->compressed) == 0;
}
static bool first_audio_expired(const music_job_t *job) {
    return !job->frames && job->first_audio_deadline_us > 0 &&
           esp_timer_get_time() >= job->first_audio_deadline_us;
}
static esp_err_t output_open(music_job_t *job, uint32_t rate) {
    if (cancelled()) return ESP_ERR_INVALID_STATE;
    if (first_audio_expired(job)) return ESP_ERR_TIMEOUT;
    if (job->tx) return job->rate == rate ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    esp_err_t err = nuwax_audio_board_init(&job->ref);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "music board init failed err=%s", esp_err_to_name(err));
        return err;
    }
    err = nuwax_audio_board_acquire_music(&job->tx, &job->lease);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "music exclusive RX/TX acquire failed err=%s", esp_err_to_name(err));
        return err;
    }
    job->rate = rate;
    return nuwax_audio_board_music_rate(&job->lease, rate);
}
static esp_err_t write_stereo(music_job_t *job, const int16_t *pcm, size_t frames) {
    size_t written = 0;
    const esp_err_t err = i2s_channel_write((i2s_chan_handle_t)job->tx, pcm,
                                           frames * 4U, &written, 60U);
    return err == ESP_OK && written != frames * 4U ? ESP_ERR_INVALID_SIZE : err;
}
static esp_err_t output_pcm(music_job_t *job, const int16_t *pcm,
                            size_t frames, unsigned channels, uint32_t rate) {
    esp_err_t err = output_open(job, rate);
    if (err != ESP_OK) return err;
    int16_t stereo[256U * 2U];
    for (size_t offset = 0; offset < frames; ) {
        if (cancelled()) return ESP_ERR_INVALID_STATE;
        if (first_audio_expired(job)) return ESP_ERR_TIMEOUT;
        size_t count = frames - offset;
        if (count > 256U) count = 256U;
        const int32_t volume = nuwax_speaker_playback_get_volume_percent();
        const uint32_t fade_frames = rate * 40U / 1000U;
        for (size_t i = 0; i < count; ++i) {
            int32_t sample = pcm[(offset + i) * channels];
            if (channels == 2) sample = (sample + pcm[(offset + i) * 2U + 1U]) / 2;
            sample = sample * volume / 100;
            if (job->frames + i < fade_frames)
                sample = (int32_t)((int64_t)sample * (job->frames + i) / fade_frames);
            stereo[i * 2U] = stereo[i * 2U + 1U] = (int16_t)sample;
        }
        if (!job->unmuted) {
            if (cancelled()) return ESP_ERR_INVALID_STATE;
            err = nuwax_audio_board_prepare_speaker_playback();
            if (err != ESP_OK) return err;
            job->unmuted = true;
        }
        if (cancelled()) return ESP_ERR_INVALID_STATE;
        if (first_audio_expired(job)) return ESP_ERR_TIMEOUT;
        err = write_stereo(job, stereo, count);
        if (err != ESP_OK) return err;
        job->last_sample = stereo[(count - 1U) * 2U];
        if (job->frames == 0U && !cancelled()) report(job, "playing", NULL);
        job->frames += count;
        offset += count;
    }
    return ESP_OK;
}
static esp_err_t play_stream(music_job_t *job) {
    uint8_t *input = heap_caps_malloc(MUSIC_INPUT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(2304U * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    HMP3Decoder decoder = job->pcm ? NULL : MP3InitDecoder();
    esp_err_t result = ESP_ERR_NO_MEM;
    if (!input || !pcm || (!job->pcm && !decoder)) goto done;
    size_t available = 0, skip_id3 = 0, corrupt = 0;
    bool header_seen = job->pcm, need_input = true;
    result = ESP_OK;
    while (!cancelled()) {
        if (first_audio_expired(job)) { result = ESP_ERR_TIMEOUT; break; }
        if (need_input && available < MUSIC_INPUT_BYTES && !input_done(job))
            available += xStreamBufferReceive(job->compressed, input + available,
                                               MUSIC_INPUT_BYTES - available, pdMS_TO_TICKS(20));
        const bool eof = input_done(job);
        need_input = false;
        if (!available && eof) break;
        if (!header_seen) {
            if (available < 10U && !eof) { need_input = true; continue; }
            header_seen = true;
            if (available >= 10U && memcmp(input, "ID3", 3) == 0) {
                if ((input[6] | input[7] | input[8] | input[9]) & 0x80U) {
                    result = ESP_ERR_INVALID_RESPONSE; break;
                }
                skip_id3 = 10U + ((size_t)input[6] << 21) + ((size_t)input[7] << 14) +
                           ((size_t)input[8] << 7) + input[9] +
                           ((input[3] == 4U && (input[5] & 0x10U)) ? 10U : 0U);
                if (skip_id3 > 1024U * 1024U) { result = ESP_ERR_INVALID_SIZE; break; }
            }
        }
        if (skip_id3) {
            size_t skip = skip_id3 < available ? skip_id3 : available;
            available -= skip; skip_id3 -= skip;
            memmove(input, input + skip, available);
            if (skip_id3 && eof) { result = ESP_ERR_INVALID_SIZE; break; }
            need_input = true; continue;
        }
        if (job->pcm) {
            const size_t samples = available / 2U > 1152U ? 1152U : available / 2U;
            if (!samples) {
                if (eof) { result = ESP_ERR_INVALID_SIZE; break; }
                need_input = true; continue;
            }
            memcpy(pcm, input, samples * 2U);
            result = output_pcm(job, pcm, samples, 1U, 16000U);
            available -= samples * 2U;
            memmove(input, input + samples * 2U, available);
            need_input = available < 4096U;
            if (result != ESP_OK) break;
            continue;
        }
        const int sync = MP3FindSyncWord(input, (int)available);
        if (sync < 0) {
            if (eof) {
                if (!job->frames) result = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            const size_t keep = available < 3U ? available : 3U;
            corrupt += available - keep;
            memmove(input, input + available - keep, keep);
            available = keep; need_input = true;
        } else {
            if (sync) { available -= (size_t)sync; memmove(input, input + sync, available); }
            unsigned char *cursor = input;
            int remaining = (int)available;
            const int decoded = MP3Decode(decoder, &cursor, &remaining, pcm, 0);
            if (decoded == ERR_MP3_INDATA_UNDERFLOW && !eof && available < MUSIC_INPUT_BYTES) {
                /* Refill even above 4096 bytes: incomplete input must always
                 * make progress or yield, never spin on the same frame. */
                need_input = true; continue;
            }
            if (decoded == ERR_MP3_INDATA_UNDERFLOW) {
                result = ESP_ERR_INVALID_SIZE; break;
            }
            size_t consumed = (size_t)(cursor - input);
            if (consumed > available) { result = ESP_ERR_INVALID_RESPONSE; break; }
            if (decoded != 0) {
                if (!consumed) consumed = available ? 1U : 0U;
                corrupt += consumed;
            }
            available -= consumed;
            memmove(input, input + consumed, available);
            need_input = available < 4096U;
            if (decoded == 0) {
                MP3FrameInfo frame;
                MP3GetLastFrameInfo(decoder, &frame);
                if (frame.nChans < 1 || frame.nChans > 2 || frame.outputSamps <= 0 ||
                    frame.outputSamps > 2304 || frame.samprate <= 0 || !consumed) {
                    result = ESP_ERR_INVALID_RESPONSE; break;
                }
                corrupt = 0;
                result = output_pcm(job, pcm, (size_t)frame.outputSamps / frame.nChans,
                                     (unsigned)frame.nChans, (uint32_t)frame.samprate);
                if (result != ESP_OK) break;
            }
        }
        if (corrupt > 65536U) { result = ESP_ERR_INVALID_RESPONSE; break; }
    }
    if (cancelled()) result = ESP_ERR_INVALID_STATE;
    if (result == ESP_OK && !job->frames) result = ESP_ERR_INVALID_RESPONSE;
done:
    if (decoder) MP3FreeDecoder(decoder);
    heap_caps_free(input); heap_caps_free(pcm);
    return result;
}
static void close_output(music_job_t *job, bool natural) {
    if (job->tx) {
        int16_t silence[256U * 2U] = {0};
        if (job->unmuted) {
            if (!natural) {
                for (size_t i = 0; i < 256U; ++i)
                    silence[i * 2U] = silence[i * 2U + 1U] =
                        (int16_t)((int32_t)job->last_sample * (255 - (int)i) / 256);
                (void)write_stereo(job, silence, 256U);
                memset(silence, 0, sizeof(silence));
            }
            size_t remaining = CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM *
                                CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM;
            while (remaining) {
                const size_t frames = remaining > 256U ? 256U : remaining;
                if (write_stereo(job, silence, frames) != ESP_OK) break;
                remaining -= frames;
            }
        }
        if (nuwax_audio_board_force_speaker_mute() == ESP_OK)
            atomic_store_explicit(&s_output_quiet, true, memory_order_release);
    } else {
        atomic_store_explicit(&s_output_quiet, true, memory_order_release);
    }
    if (job->lease.token) {
        unsigned attempt = 0;
        while (nuwax_audio_board_music_rate(&job->lease,
                   nuwax_audio_board_get_caps()->sample_rate_hz) != ESP_OK) {
            if ((attempt++ % 10U) == 0U)
                ESP_LOGE(TAG, "music voice clock restore pending; microphone remains fenced");
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        (void)nuwax_audio_board_release(&job->lease);
    }
    if (job->ref.token) (void)nuwax_audio_board_deinit(&job->ref);
}
static void music_task(void *arg) {
    music_job_t *job = arg;
    TaskHandle_t downloader = NULL;
    esp_err_t result = ESP_ERR_NO_MEM;
    if (xTaskCreateWithCaps(download_task, "music_http", 6144, job, 3,
                            &downloader, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        const int64_t deadline = esp_timer_get_time() + 2500000LL;
        while (!cancelled() && !atomic_load_explicit(&job->producer_done, memory_order_acquire) &&
               xStreamBufferBytesAvailable(job->compressed) < 24U * 1024U &&
               esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
        result = play_stream(job);
    }
    const bool stopped = cancelled();
    const bool natural = result == ESP_OK && !stopped;
    if (natural && atomic_load_explicit(&job->producer_done, memory_order_acquire))
        result = job->producer_result;
    atomic_store_explicit(&s_stop, true, memory_order_release);
    phase("stopping", job->rate);
    close_output(job, natural);
    /* Meeting/OTA need the restored clock, not completion of a slow HTTP
     * close. Keep the worker alive only to join its producer safely. */
    atomic_store_explicit(&s_audio_active, false, memory_order_release);
    nuwax_music_snapshot_t snapshot;
    nuwax_music_playback_snapshot(&snapshot);
    if (stopped) report(job, "cancelled", snapshot.reason[0] ? snapshot.reason : "stopped");
    else if (result == ESP_OK) report(job, "completed", NULL);
    else report(job, "failed", !job->frames && result == ESP_ERR_TIMEOUT
                                ? "first_audio_timeout" : esp_err_to_name(result));
    /* Publish the terminal state after safe audio release, without waiting
     * for HTTP. Keep its storage alive until the producer has actually exited. */
    if (downloader)
        while (!atomic_load_explicit(&job->producer_done, memory_order_acquire))
            vTaskDelay(pdMS_TO_TICKS(20));
    vStreamBufferDelete(job->compressed);
    heap_caps_free(job->storage); heap_caps_free(job);
    atomic_store_explicit(&s_active, false, memory_order_release);
    vTaskDeleteWithCaps(NULL);
}
esp_err_t nuwax_music_playback_init(void) {
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    strlcpy(s_snapshot.state, "idle", sizeof(s_snapshot.state));
    return nuwax_music_status_init();
}
static bool start(nuwax_ws_client_t *ws, const char *id, const char *url,
                   const char *format, bool local) {
    if (!id || !id[0] || strlen(id) >= sizeof(s_snapshot.request_id) ||
        !url || strlen(url) >= NUWAX_SESSION_MUSIC_URL_MAX_LEN || !format ||
        (!local && (!ws || strncmp(url, "https://", 8) != 0)) ||
        (local && strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) ||
        (strcmp(format, "mp3") != 0 && (!local || strcmp(format, "pcm") != 0))) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= ' ' || *p == 127 || *p == '\\' || *p == '#') return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_active, &expected, true)) return false;
    music_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) goto failed;
    job->storage = heap_caps_malloc(MUSIC_BUFFER_BYTES + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job->storage) { heap_caps_free(job); goto failed; }
    job->compressed = xStreamBufferCreateStatic(MUSIC_BUFFER_BYTES + 1U, 1U,
                                                 job->storage, &job->stream_control);
    if (!job->compressed) { heap_caps_free(job->storage); heap_caps_free(job); goto failed; }
    job->ws = ws;
    job->epoch = ws ? nuwax_ws_connection_epoch(ws) : 0U;
    job->pcm = strcmp(format, "pcm") == 0; job->local = local;
    job->first_audio_deadline_us = esp_timer_get_time() + MUSIC_FIRST_AUDIO_TIMEOUT_US;
    strlcpy(job->id, id, sizeof(job->id)); strlcpy(job->url, url, sizeof(job->url));
    atomic_init(&job->producer_done, false); atomic_init(&job->received, 0);
    atomic_store_explicit(&s_stop, false, memory_order_release);
    atomic_store_explicit(&s_output_quiet, false, memory_order_release);
    atomic_store_explicit(&s_audio_active, true, memory_order_release);
    portENTER_CRITICAL(&s_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    strlcpy(s_snapshot.request_id, id, sizeof(s_snapshot.request_id));
    strlcpy(s_snapshot.state, "buffering", sizeof(s_snapshot.state));
    portEXIT_CRITICAL(&s_lock);
    TaskHandle_t task = NULL;
    if (xTaskCreateWithCaps(music_task, "nuwax_music", 12288, job, 4, &task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) return true;
    vStreamBufferDelete(job->compressed);
    heap_caps_free(job->storage); heap_caps_free(job);
failed:
    portENTER_CRITICAL(&s_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    strlcpy(s_snapshot.request_id, id, sizeof(s_snapshot.request_id));
    strlcpy(s_snapshot.state, "failed", sizeof(s_snapshot.state));
    strlcpy(s_snapshot.reason, "music_start_failed", sizeof(s_snapshot.reason));
    portEXIT_CRITICAL(&s_lock);
    atomic_store_explicit(&s_output_quiet, true, memory_order_release);
    atomic_store_explicit(&s_audio_active, false, memory_order_release);
    atomic_store_explicit(&s_active, false, memory_order_release);
    return false;
}
bool nuwax_music_playback_start(nuwax_ws_client_t *ws, const char *id,
                                const char *url, const char *format) {
    return start(ws, id, url, format, false);
}
bool nuwax_music_playback_start_test(const char *id, const char *url, const char *format) {
#if CONFIG_NUWAX_MUSIC_CONTROL_TEST_ENABLE
    return start(NULL, id, url, format, true);
#else
    (void)id; (void)url; (void)format; return false;
#endif
}
void nuwax_music_playback_cancel(const char *reason) {
    if (!atomic_load_explicit(&s_active, memory_order_acquire)) return;
    portENTER_CRITICAL(&s_lock);
    if (!atomic_load_explicit(&s_stop, memory_order_relaxed)) {
        strlcpy(s_snapshot.reason, reason ? reason : "stopped", sizeof(s_snapshot.reason));
        atomic_store_explicit(&s_stop, true, memory_order_release);
    }
    portEXIT_CRITICAL(&s_lock);
}
bool nuwax_music_playback_stop(const char *id) {
    nuwax_music_snapshot_t snapshot;
    nuwax_music_playback_snapshot(&snapshot);
    if (id && id[0] && nuwax_music_playback_active() && strcmp(id, snapshot.request_id)) return false;
    nuwax_music_playback_cancel("cloud_stop");
    return true;
}
bool nuwax_music_playback_active(void) {
    return atomic_load_explicit(&s_active, memory_order_acquire);
}
bool nuwax_music_playback_audio_active(void) {
    return atomic_load_explicit(&s_audio_active, memory_order_acquire);
}
bool nuwax_music_playback_output_quiesced(void) {
    return !nuwax_music_playback_active() ||
           atomic_load_explicit(&s_output_quiet, memory_order_acquire);
}
