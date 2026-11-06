#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "offline_audio_voice.h"
#include "offline_audio_policy.h"

static uint32_t written, first_sequence, last_sequence, first_timestamp;
static unsigned opens, closes;
static unsigned incomplete_closes;
static uint32_t open_first_sequence[16];
static bool first_after_open;
static bool media = true;
static int16_t pcm[320];

esp_err_t nuwax_offline_audio_mirror_service(nuwax_offline_audio_mirror_t *m,
    nuwax_offline_audio_mode_t mode, const char *id, uint32_t rate,
    uint8_t channels, uint16_t duration, uint32_t now)
{
    (void)id; (void)now;
    assert(mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING);
    assert(rate == 16000 && channels == 1 && duration == 20);
    if (!media) return ESP_ERR_NOT_FINISHED;
    if (!m->started) { ++opens; first_after_open = true; }
    m->started = true;
    return ESP_OK;
}

esp_err_t nuwax_offline_audio_mirror_append(nuwax_offline_audio_mirror_t *m,
    const nuwax_offline_audio_frame_t *f, uint32_t now)
{
    (void)now;
    if (!media) { m->started = false; return ESP_FAIL; }
    assert(m->started && f->sample_count == 320);
    if (first_after_open) {
        assert(opens <= 16U);
        open_first_sequence[opens - 1U] = f->sequence;
        first_after_open = false;
    }
    if (!written) { first_sequence = f->sequence; first_timestamp = f->timestamp_ms; }
    else assert(f->sequence > last_sequence);
    assert(f->timestamp_ms == f->sequence * 20U);
    last_sequence = f->sequence;
    ++written;
    return ESP_OK;
}

esp_err_t nuwax_offline_audio_mirror_request_close(nuwax_offline_audio_mirror_t *m,
    bool complete, uint32_t now)
{
    (void)now;
    if (m->started && !complete) ++incomplete_closes;
    if (m->started) ++closes;
    m->started = false;
    return ESP_OK;
}

static void feed(nuwax_offline_voice_t *v, uint32_t from, uint32_t to, bool speech)
{
    for (uint32_t i = from; i < to; ++i)
        nuwax_offline_voice_process(v, pcm, 320, speech, i, i * 20U);
}

static void reset_counts(void)
{
    written = opens = closes = incomplete_closes = 0U;
    first_after_open = false;
    memset(open_first_sequence, 0, sizeof(open_first_sequence));
    media = true;
}

static void continuous_monitoring_checks(void)
{
    const nuwax_offline_voice_policy_t policy = {
        .silence_ms = 120000U, .resume_confirm_frames = 10U,
        .activity_confirm_frames = 10U, .start_recording = true,
    };
    nuwax_offline_voice_t v;
    reset_counts();
    assert(nuwax_offline_voice_init_with_policy(&v, "monitor_continuous", 16000, 20, &policy) == ESP_OK);
    assert(v.recording && v.silence_limit_frames == 6000U);
    feed(&v, 0, 5999, false);
    assert(v.recording && written == 5999 && first_sequence == 0 && !closes);
    feed(&v, 5999, 6000, false);
    assert(!v.recording && written == 6000 && last_sequence == 5999 && closes == 1);
    feed(&v, 6000, 6300, false);
    feed(&v, 6300, 6309, true);
    assert(!v.recording && written == 6000); /* Under 200ms cannot resume. */
    feed(&v, 6309, 6310, true);
    assert(v.recording && opens == 2 && written == 6025 && open_first_sequence[1] == 6285);
    assert(last_sequence == 6309 && v.quiet_frames == 0);
    feed(&v, 6310, 12309, false);
    assert(v.recording);
    feed(&v, 12309, 12310, false);
    assert(!v.recording && closes == 2 && written == 12025);
    /* Quick resume does not duplicate the last 500ms of the closed file. */
    feed(&v, 12310, 12320, true);
    assert(v.recording && written == 12035 && open_first_sequence[2] == 12310);
    nuwax_offline_voice_deinit(&v, true);
    assert(closes == 3 && !incomplete_closes);

    reset_counts();
    assert(nuwax_offline_voice_init_with_policy(&v, "monitor_noise", 16000, 20, &policy) == ESP_OK);
    /* Isolated short bursts must not keep an empty recording alive forever. */
    for (uint32_t i = 0; i < 6000U; ++i)
        nuwax_offline_voice_process(&v, pcm, 320, (i % 500U) < 3U, i, i * 20U);
    assert(!v.recording && closes == 1 && written == 6000);
    for (uint32_t i = 6000; i < 12000U; ++i)
        nuwax_offline_voice_process(&v, pcm, 320, (i % 500U) < 3U, i, i * 20U);
    assert(!v.recording && written == 6000 && closes == 1);
    nuwax_offline_voice_deinit(&v, true);

    reset_counts();
    assert(nuwax_offline_voice_init_with_policy(&v, "monitor_refresh", 16000, 20, &policy) == ESP_OK);
    feed(&v, 0, 5000, false); feed(&v, 5000, 5010, true);
    assert(v.quiet_frames == 0);
    feed(&v, 5010, 11009, false);
    assert(v.recording && closes == 0 && written == 11009);
    feed(&v, 11009, 11010, false);
    assert(!v.recording && closes == 1 && written == 11010);
    nuwax_offline_voice_reset(&v);
    feed(&v, 11010, 11020, false);
    assert(!v.recording && written == 11010); /* A reset cannot resume silence. */
    nuwax_offline_voice_deinit(&v, true);

    reset_counts();
    assert(nuwax_offline_voice_init_with_policy(&v, "monitor_gap", 16000, 20, &policy) == ESP_OK);
    feed(&v, 0, 100, false);
    nuwax_offline_voice_reset(&v);
    assert(v.recording && incomplete_closes == 1);
    feed(&v, 105, 110, false);
    assert(written == 105 && open_first_sequence[1] == 105);
    nuwax_offline_voice_deinit(&v, true);

    reset_counts();
    assert(nuwax_offline_voice_init_with_policy(&v, "monitor_wrap", 16000, 20, &policy) == ESP_OK);
    const uint32_t near_wrap = UINT32_MAX / 20U - 3000U;
    feed(&v, near_wrap, near_wrap + 6000U, false);
    assert(!v.recording && written == 6000 && closes == 1);
    nuwax_offline_voice_deinit(&v, true);

    reset_counts();
    assert(nuwax_offline_voice_init_with_policy(&v, "monitor_eight_hours", 16000, 20, &policy) == ESP_OK);
    const uint32_t frames = 8U * 3600U * 50U;
    feed(&v, 0, frames, true);
    assert(v.recording && written == frames && first_sequence == 0 && last_sequence == frames - 1U);
    nuwax_offline_voice_deinit(&v, true);
    assert(closes == 1 && !incomplete_closes);
    puts("SD monitoring continuous: first-frame capture, exact 120s pause, 200ms resume, 500ms preroll without duplicates, noise rejection, activity extension, repeated pause/resume, MIC gap, clock wrap and simulated 8h PASS");
}

int main(void)
{
    nuwax_offline_voice_t v;
    assert(nuwax_offline_voice_init(&v, "test", 16000, 20) == ESP_OK);
    feed(&v, 0, 100, false);
    assert(written == 0 && opens == 0);
    feed(&v, 100, 102, true); /* Isolated clicks do not create a file. */
    assert(written == 0);
    feed(&v, 102, 110, false);
    feed(&v, 110, 130, true);
    assert(opens == 1 && first_sequence == 88 && first_timestamp == 1760);
    feed(&v, 130, 180, false);
    assert(closes == 1 && last_sequence == 179 && !v.recording);
    const uint32_t speech_total = written;
    feed(&v, 180, 300, false);
    assert(written == speech_total);
    /* Cloud ACKs are deliberately absent: a second utterance still records. */
    feed(&v, 300, 320, true);
    assert(opens == 2 && last_sequence == 319);
    nuwax_offline_voice_reset(&v);
    feed(&v, 320, 323, true);
    assert(last_sequence == 322); /* No stale preroll after a discontinuity. */
    media = false;
    feed(&v, 323, 400, true);
    media = true;
    feed(&v, 400, 410, true);
    assert(last_sequence == 409); /* Recovers while the same session is active. */
    nuwax_offline_voice_deinit(&v, true);

    written = opens = closes = 0;
    assert(nuwax_offline_voice_init(&v, "eight_hours", 16000, 20) == ESP_OK);
    const uint32_t frames = 8U * 3600U * 50U;
    feed(&v, 0, frames, true);
    assert(written == frames && first_sequence == 0 && last_sequence == frames - 1);
    const uint64_t record_bytes = (uint64_t)frames * (640U + 22U + 4U);
    const uint64_t segments = record_bytes / (1048576U - 128U - 666U) + 1U;
    assert(record_bytes + segments * 128U + NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES
           < nuwax_offline_audio_quota(268435456U));
    assert(!nuwax_offline_limit_elapsed(28800000000ULL - 1U, 0, 8));
    assert(nuwax_offline_limit_elapsed(28800000000ULL, 0, 8));
    uint64_t used = nuwax_offline_budget_used(0U, 1000000U, 61000000U);
    assert(used == 60000000ULL);
    /* Eight online hours do not consume budget; the next offline minute does. */
    used = nuwax_offline_budget_used(used, 30000000000ULL, 30060000000ULL);
    assert(used == 120000000ULL && !nuwax_offline_limit_elapsed(used, 0U, 8U));
    used = nuwax_offline_budget_used(used, 40000000000ULL, 68680000000ULL);
    assert(used == 28800000000ULL && nuwax_offline_limit_elapsed(used, 0U, 8U));
    nuwax_offline_voice_deinit(&v, true);
    assert(closes == 1);
    puts("offline voice: silence, preroll, cloud-independent capture, recovery and simulated 8h PCM budget PASS");
    continuous_monitoring_checks();
    return 0;
}
