/* Tests the extracted production decoder/output functions with real Helix
 * decoding. Only HTTP/RTOS/I2S boundaries are simulated. */
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp3dec.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_INVALID_RESPONSE 5
#define ESP_ERR_TIMEOUT 6
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define MUSIC_INPUT_BYTES 8192U
#define CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM 4
#define CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM 128
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(v) (v)
typedef void *i2s_chan_handle_t;
typedef struct { unsigned token; } board_token;
typedef struct { uint32_t sample_rate_hz; } caps_t;
typedef struct { const uint8_t *data; size_t length, offset, limit; } stream_t;
typedef struct {
    bool pcm;
    stream_t *compressed;
    atomic_bool producer_done;
    board_token ref, lease;
    void *tx;
    uint32_t rate;
    uint64_t frames;
    int16_t last_sample;
    bool unmuted;
    int64_t first_audio_deadline_us;
} music_job_t;
static int64_t test_time_us;
static int64_t receive_delay_us, prepare_delay_us;
static int64_t esp_timer_get_time(void) { return test_time_us; }
static bool stop_requested;
static atomic_bool s_output_quiet;
static size_t frames_written, stop_after;
static unsigned board_refs, board_leases, configured_rate, restore_failures;
static bool muted;
static bool full_file;
static uint64_t last_decoded_frames;
static uint64_t pcm_hash;
static bool cancelled(void) { return stop_requested; }
static void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
static void heap_caps_free(void *p) { free(p); }
static size_t xStreamBufferBytesAvailable(stream_t *s) { return s->length - s->offset; }
static size_t xStreamBufferReceive(stream_t *s, void *out, size_t n, unsigned wait) {
    (void)wait;
    test_time_us += receive_delay_us;
    if (n > s->length - s->offset) n = s->length - s->offset;
    if (s->limit && n > s->limit) n = s->limit;
    memcpy(out, s->data + s->offset, n); s->offset += n; return n;
}
static esp_err_t nuwax_audio_board_init(board_token *ref) { ref->token = ++board_refs; return ESP_OK; }
static esp_err_t nuwax_audio_board_acquire_music(void **tx, board_token *lease) {
    assert(!board_leases); lease->token = ++board_leases; *tx = lease; return ESP_OK;
}
static esp_err_t nuwax_audio_board_music_rate(board_token *lease, uint32_t rate) {
    assert(lease->token);
    if (rate == 16000 && restore_failures) { --restore_failures; return ESP_ERR_INVALID_STATE; }
    configured_rate = rate; muted = true; return ESP_OK;
}
static uint8_t nuwax_speaker_playback_get_volume_percent(void) { return 70; }
static esp_err_t nuwax_audio_board_prepare_speaker_playback(void) { assert(board_leases); test_time_us += prepare_delay_us; muted = false; return ESP_OK; }
static esp_err_t nuwax_audio_board_force_speaker_mute(void) { muted = true; return ESP_OK; }
static esp_err_t nuwax_audio_board_release(board_token *lease) {
    assert(configured_rate == 16000 && muted); --board_leases; lease->token = 0; return ESP_OK;
}
static esp_err_t nuwax_audio_board_deinit(board_token *ref) { assert(!board_leases); --board_refs; ref->token = 0; return ESP_OK; }
static const caps_t *nuwax_audio_board_get_caps(void) { static caps_t caps = {16000}; return &caps; }
static esp_err_t i2s_channel_write(void *tx, const void *data, size_t n, size_t *written, unsigned timeout) {
    (void)timeout; assert(tx && n % 4 == 0);
    const int16_t *pcm = data;
    for (size_t i = 0; i < n / 2; i += 2) assert(pcm[i] == pcm[i + 1]);
    for (size_t i = 0; i < n / 2; ++i) pcm_hash = (pcm_hash ^ (uint16_t)pcm[i]) * UINT64_C(1099511628211);
    frames_written += n / 4; *written = n;
    if (stop_after && frames_written >= stop_after) stop_requested = true;
    return ESP_OK;
}
static void report(music_job_t *job, const char *state, const char *reason) {
    (void)reason; assert(job->rate && strcmp(state, "playing") == 0);
}
static void vTaskDelay(unsigned ms) { (void)ms; }
#include "music_core_under_test.inc"

static uint64_t run(const uint8_t *data, size_t length, bool pcm, size_t chunk,
                uint32_t rate, bool stop, bool fail, bool restore_retry) {
    stop_requested = false; frames_written = 0; stop_after = stop ? 2048 : 0;
    pcm_hash = UINT64_C(14695981039346656037);
    restore_failures = restore_retry ? 2 : 0;
    configured_rate = 16000; muted = true;
    stream_t stream = { data, length, 0, chunk };
    music_job_t job = { .pcm = pcm, .compressed = &stream };
    atomic_init(&job.producer_done, true);
    const esp_err_t result = play_stream(&job);
    if (stop) assert(result == ESP_ERR_INVALID_STATE);
    else if (fail) assert(result != ESP_OK);
    else {
        assert(result == ESP_OK && job.rate == rate && job.frames > 0);
        if (!pcm && !full_file) assert(job.frames >= rate && job.frames < rate + 4096U);
    }
    const uint64_t decoded_hash = pcm_hash;
    last_decoded_frames = job.frames;
    close_output(&job, result == ESP_OK);
    assert(!board_refs && !board_leases && configured_rate == 16000 && muted);
    return decoded_hash;
}
int main(int argc, char **argv) {
    assert(argc == 3 || (argc == 4 && strcmp(argv[3], "--full-file") == 0));
    full_file = argc == 4;
    FILE *f = fopen(argv[1], "rb"); assert(f);
    fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
    uint8_t *mp3 = malloc((size_t)size); assert(mp3);
    assert(fread(mp3, 1, (size_t)size, f) == (size_t)size); fclose(f);
    const unsigned rate = (unsigned)atoi(argv[2]);
    const uint64_t reference = run(mp3, (size_t)size, false, 8192, rate, false, false, true);
    assert(reference == run(mp3, (size_t)size, false, 137, rate, false, false, false));
    if (full_file) {
        printf("PASS: complete source decoded; bytes=%ld rate=%u frames=%llu seconds=%.3f PCM identical across 8192/137-byte input fragments\n",
               size, rate, (unsigned long long)last_decoded_frames, (double)last_decoded_frames / rate);
        free(mp3);
        return 0;
    }
    run(mp3, (size_t)size, false, 8111, rate, true, false, false);
    run(mp3, (size_t)size - 50U, false, 137, rate, false, true, false);
    uint8_t *tagged = calloc(1, (size_t)size + 65546); assert(tagged);
    memcpy(tagged, "ID3\4\0\0\0\4\0\0", 10); // 65536-byte synchsafe ID3 body
    memcpy(tagged + 65546, mp3, (size_t)size);
    run(tagged, (size_t)size + 65546, false, 5001, rate, false, false, false);
    free(tagged);
    /* A server sending one byte per 200 ms must not keep music ownership
     * until Helix finally assembles a frame (over two minutes previously). */
    stop_requested = false; stop_after = 0; frames_written = 0;
    test_time_us = 0; receive_delay_us = 200000;
    stream_t slow = {mp3, (size_t)size, 0, 1};
    music_job_t stalled = {.compressed = &slow, .first_audio_deadline_us = 10000000};
    atomic_init(&stalled.producer_done, true);
    assert(play_stream(&stalled) == ESP_ERR_TIMEOUT);
    assert(!stalled.frames && !frames_written && test_time_us <= 10200000);
    close_output(&stalled, false);
    assert(!board_refs && !board_leases && muted);
    receive_delay_us = 0; test_time_us = 0;
    /* The PA sequence may cross the deadline after output_open succeeds. */
    prepare_delay_us = 11000000;
    slow.offset = 0; slow.limit = 8192;
    stalled = (music_job_t){.compressed = &slow, .first_audio_deadline_us = 10000000};
    atomic_init(&stalled.producer_done, true);
    assert(play_stream(&stalled) == ESP_ERR_TIMEOUT);
    assert(!stalled.frames && !frames_written);
    close_output(&stalled, false);
    assert(!board_refs && !board_leases && muted && configured_rate == 16000);
    prepare_delay_us = 0; test_time_us = 0;
    puts("PASS: slow trickle and delayed PA cannot output late first audio; voice clock restored");
    free(mp3);
    uint8_t pcm[12000] = {0};
    run(pcm, sizeof(pcm), true, 137, 16000, false, false, false);
    run(pcm, sizeof(pcm) - 1, true, 137, 16000, false, true, false);
    uint8_t corrupt[6000]; memset(corrupt, 255, sizeof(corrupt));
    run(corrupt, sizeof(corrupt), false, 6000, 0, false, true, false);
    puts("PASS: native MP3, fragmented input, ID3, cancellation, malformed/odd PCM and voice-clock restoration");
    return 0;
}
