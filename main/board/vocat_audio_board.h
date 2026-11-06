#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_NUWAX_P3_VENDOR_V2_AUDIO_PROFILE
#define VOCAT_AUDIO_SAMPLE_RATE_HZ 24000U
#else
#define VOCAT_AUDIO_SAMPLE_RATE_HZ 16000U
#endif

/*
 * Miaopan 2.0 product profile: frozen audio hardware slot semantics.
 *
 * ES7210 4-slot TDM physical mapping (candidate route; health-gated by
 * P3-0 diagnostics):
 *
 *   slot0 = MIC1  primary microphone (mono capture / ASR / AFE M channel)
 *   slot1 = MIC2  secondary microphone (acoustic pickup, NOT electrical ref)
 *   slot2 = MIC3  candidate playback reference route
 *                 (ES8311 OUTP/N -> RC -> ES7210 MIC3)
 *                 Product firmware measures this route independently and
 *                 keeps it disabled unless the runtime health gate passes.
 *                 Do not enable hardware AEC unless ref_health passes.
 *   slot3 = MIC4  not connected (NC, unused).
 *
 * Target AFE input, if health passes: slot0 + slot2 -> MR.
 * slot1 must NOT be used as electrical reference; slot3 is ignored.
 *
 * These macros are the single source of truth. All other code that references
 * TDM slot indices MUST use these names, not bare integers.
 */
#define VOCAT_AUDIO_SLOT_MIC_PRIMARY   0  /* MIC1: primary mic, AFE M channel */
#define VOCAT_AUDIO_SLOT_MIC_SECONDARY 1  /* MIC2: secondary mic, NOT reference */
#define VOCAT_AUDIO_SLOT_AEC_REF       2  /* MIC3: candidate playback reference route */
#define VOCAT_AUDIO_SLOT_UNUSED        3  /* MIC4: NC, ignore */

#define VOCAT_AUDIO_AEC_REF_SLOT VOCAT_AUDIO_SLOT_AEC_REF  /* canonical alias */
#define VOCAT_AUDIO_MIC_PRIMARY_SLOT VOCAT_AUDIO_SLOT_MIC_PRIMARY

typedef struct {
    uint32_t elapsed_ms;
    uint32_t tx_errors;
    uint32_t rx_errors;
    uint64_t tx_frames;
    uint64_t rx_frames;
    uint32_t rx_rate_hz;
    uint32_t rx_rate_error_permille;
    bool rx_rate_ok;
    bool signal_present;
    int slot_rms[4];
    int slot_peak[4];
} nuwax_vocat_audio_smoke_stats_t;

esp_err_t nuwax_vocat_audio_board_init(void);
/* Called only through audio_board while both channel leases are held. */
esp_err_t nuwax_vocat_audio_board_set_music_rate(uint32_t sample_rate);
esp_err_t nuwax_vocat_audio_board_smoke_test(uint32_t duration_ms, nuwax_vocat_audio_smoke_stats_t *out_stats);
esp_err_t nuwax_vocat_audio_board_deinit(void);

/* P2-1: expose the shared duplex I2S channels (cast to i2s_chan_handle_t).
 * Valid only after a successful init; NULL otherwise. */
void *nuwax_vocat_audio_board_get_rx_handle(void);
void *nuwax_vocat_audio_board_get_tx_handle(void);
uint32_t nuwax_vocat_audio_board_get_rx_overflow_count(void);

/* PA safety control. Init leaves PA off; the TX owner must establish silent
 * DMA output before prepare, and submit speech only after prepare returns.
 * Validation reads the SoC output latch, not the input pad. */
esp_err_t nuwax_vocat_audio_board_force_speaker_mute(void);
esp_err_t nuwax_vocat_audio_board_prepare_speaker_playback(void);

/* P2-3 ES8311 playback start/stop stress. Each cycle toggles the codec,
 * explicitly controls the board-owned PA, and writes a 1kHz burst. */
typedef struct {
    int cycles_done;
    uint32_t enable_errors;
    uint32_t disable_errors;
    uint32_t write_errors;
    uint64_t frames;
    uint32_t min_internal_free;
} nuwax_vocat_playback_stress_stats_t;

esp_err_t nuwax_vocat_audio_board_playback_stress(int cycles, nuwax_vocat_playback_stress_stats_t *out);

/* P3 validation helper: continuously write a deterministic
 * 1 kHz tone to the ES8311 TX path while an external diagnostic consumes the
 * simultaneous ES7210 TDM RX stream. The returned handle owns the shared TX
 * lease and an audio-board reference until a successful stop. */
typedef struct nuwax_vocat_test_tone_handle nuwax_vocat_test_tone_handle_t;
esp_err_t nuwax_vocat_audio_board_test_tone_start(nuwax_vocat_test_tone_handle_t **out_handle);
esp_err_t nuwax_vocat_audio_board_test_tone_stop(nuwax_vocat_test_tone_handle_t *handle);

/* P2-5 REF TDM slot identification: two-phase (tone vs silence) per-slot RMS;
 * REF = slot whose RMS tracks playback (max tone-quiet delta). */
typedef struct {
    int tone_rms[4];
    int quiet_rms[4];
    int ref_slot;   /* -1 if none clearly tracks playback */
    int ref_delta;  /* tone-quiet RMS of the identified REF slot */
} nuwax_vocat_ref_probe_stats_t;

esp_err_t nuwax_vocat_audio_board_ref_probe(nuwax_vocat_ref_probe_stats_t *out);

/* P3-0C ES7210 channel-to-slot probe. While a deterministic playback tone is
 * active, lower one physical MICx PGA at a time and observe which TDM slot
 * follows. This distinguishes a TDM slot-map error from a silent MIC3 analog
 * reference path without changing the production capture route. */
esp_err_t nuwax_vocat_audio_board_channel_map_probe(void);

/* P3-0D MIC3 electrical-reference probe. The external PA is forced off while
 * a strong DAC-only tone excites OUTP/N, so the MIC3 path can be tested without
 * an audible high-level speaker tone. cleanup_safe must be true before any
 * later playback diagnostic is allowed to run. */
typedef struct {
    int high_gain_rms[4];
    int low_gain_rms[4];
    int high_gain_peak[4];
    int low_gain_peak[4];
    float high_gain_clip_ratio[4];
    float low_gain_clip_ratio[4];
    float drop_db[4];
    int inferred_slot;
    uint32_t tx_errors;
    uint32_t rx_errors;
    bool passed;
    bool tx_flush_ok;
    bool gain_restored;
    bool volume_restored;
    bool pa_restored;
    bool cleanup_safe;
} nuwax_vocat_mic3_silent_probe_stats_t;

esp_err_t nuwax_vocat_audio_board_mic3_silent_probe(
    nuwax_vocat_mic3_silent_probe_stats_t *out);

/*
 * P3-0: AEC reference health check.
 *
 * Plays a short 1kHz calibration tone (via ES8311), simultaneously captures
 * slot0 (primary mic) + slot2 (candidate AEC reference), and computes:
 *
 *   - slot2 1kHz amplitude (band-pass / Goertzel)
 *   - slot2 baseline noise (quiet-period RMS)
 *   - correlation between playback signal and slot2 signal
 *   - clipping ratio (samples at int16 extremes)
 *   - estimated delay between TX write and RX slot2 appearance
 *
 * Output is a ref_health_score (0.0 = dead, 1.0 = textbook) and a discrete
 * status: ok / no_signal / clipped / noisy / pa_muted.
 *
 * When status != ok, the caller MUST disable hardware AEC and fall back to
 * software reference or disable L3 interrupt level.
 */
typedef enum {
    NUWAX_AEC_REF_HEALTH_OK = 0,
    NUWAX_AEC_REF_HEALTH_NO_SIGNAL,   /* slot2 RMS near zero during tone */
    NUWAX_AEC_REF_HEALTH_CLIPPED,     /* >2% samples at int16 min/max */
    NUWAX_AEC_REF_HEALTH_NOISY,       /* quiet-period RMS too high */
    NUWAX_AEC_REF_HEALTH_PA_MUTED,    /* no tone detected on any slot */
} nuwax_aec_ref_health_status_t;

typedef struct {
    nuwax_aec_ref_health_status_t status;
    float score;               /* 0.0 (dead) .. 1.0 (textbook) */
    float slot2_tone_amplitude;     /* RMS of 1kHz component in slot2 */
    float slot2_quiet_rms;          /* baseline noise RMS during silence */
    float slot2_correlation;        /* cross-correlation peak with playback */
    float slot2_clip_ratio;         /* fraction of samples at int16 min/max */
    int   estimated_delay_samples;  /* TX->slot2 lag in samples at the active board rate */
    int   slot0_rms;                /* primary mic RMS during tone (sanity) */
    bool  passed;                   /* true when status == OK and score >= configurable threshold */
} nuwax_aec_ref_health_t;

/*
 * Run the AEC reference health check.
 *
 * @param volume_db   ES8311 volume during the test tone, e.g. -6.0f.
 *                    Pass -30.0f or lower for quiet environments; use
 *                    -6.0f..0.0f when the RC divider attenuates heavily.
 * @param duration_ms  Tone duration per phase, typically 1500-3000 ms.
 * @param out          Output struct (must not be NULL).
 * @return ESP_OK on success (even if health is NO_SIGNAL; check out->status).
 */
esp_err_t nuwax_vocat_aec_ref_health_check(float volume_db, uint32_t duration_ms,
                                           nuwax_aec_ref_health_t *out);
