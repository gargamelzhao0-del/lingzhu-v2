#pragma once

/*
 * P2-1 audio_board abstraction, extended P3-0.
 *
 * Board-agnostic facade over the concrete codec/I2S backend. On Miaopan V2.0
 * a single I2S peripheral is shared in duplex by the ES7210 capture path and
 * the ES8311 playback path, so mic/speaker must NOT each call i2s_new_channel.
 * They acquire the shared channels from here instead. Legacy (pre-7.0 discrete
 * digital mic/speaker) boards report shared_i2s=false and keep their own path.
 *
 * P3-0: caps now carries the frozen slot semantics and AEC reference
 * capability so session_protocol hello and diagnostic logs have a single
 * source of truth.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    NUWAX_AUDIO_CODEC_NONE = 0,
    NUWAX_AUDIO_CODEC_LEGACY_DIGITAL, /* raw digital I2S mic/speaker (pre-7.0) */
    NUWAX_AUDIO_CODEC_ES7210_ES8311,  /* Miaopan V2.0 */
} nuwax_audio_codec_kind_t;

/*
 * P3-0: AEC reference source enumeration.  Used by caps and hello to tell
 * the cloud gateway how the device plans to feed the AEC reference channel.
 */
typedef enum {
    NUWAX_AEC_REF_NONE = 0,           /* no reference available */
    NUWAX_AEC_REF_ES7210_SLOT2,       /* hardware playback ref on ES7210 MIC3/slot2 */
    NUWAX_AEC_REF_SOFTWARE,           /* software reference: playback PCM side-copy */
    NUWAX_AEC_REF_ES7210_SLOT2_SOFTWARE_FALLBACK, /* hardware primary, software fallback */
} nuwax_aec_reference_kind_t;

/* Electrical routing is only a candidate until the runtime signal test passes. */
typedef enum {
    NUWAX_HARDWARE_REF_HEALTH_UNTESTED = 0,
    NUWAX_HARDWARE_REF_HEALTH_PASSED,
    NUWAX_HARDWARE_REF_HEALTH_FAILED,
    NUWAX_HARDWARE_REF_HEALTH_BLOCKED_UNPROVEN,
} nuwax_hardware_ref_health_t;

/*
 * P3-0: AFE input format.  Matches the esp-sr afe_config_t.input_format
 * string convention so it can be passed through without translation.
 */
typedef enum {
    NUWAX_AFE_INPUT_MM  = 0,  /* 2 mics, no reference (half-duplex / no-AEC path) */
    NUWAX_AFE_INPUT_MR  = 1,  /* 1 mic + 1 reference (hardware or software ref) */
    NUWAX_AFE_INPUT_MMR = 2,  /* 2 mics + 1 reference */
} nuwax_afe_input_format_t;

typedef struct {
    nuwax_audio_codec_kind_t codec;
    const char *hardware_profile;
    uint8_t mic_channels;   /* capture slots (TDM slots for ES7210) */
    bool has_speaker;
    bool full_duplex;       /* one I2S shared by capture + playback */
    bool shared_i2s;        /* true => acquire shared channels, do not new_channel */
    uint32_t sample_rate_hz;

    /* ---- P3-0: frozen slot semantics & AEC capability ---- */
    bool     has_hardware_ref_candidate; /* board routes a possible hardware reference */
    nuwax_aec_reference_kind_t aec_reference;  /* which reference source */
    uint8_t  ref_slot;                 /* TDM slot index carrying the reference (2 for V1.2) */
    uint8_t  mic_primary_slot;         /* TDM slot index for primary mic (0 for V1.2) */
    nuwax_afe_input_format_t input_format;  /* AFE input format: MM, MR, or MMR */
} nuwax_audio_board_caps_t;

typedef enum {
    NUWAX_AUDIO_BOARD_LEASE_NONE = 0,
    NUWAX_AUDIO_BOARD_LEASE_RX = 1U << 0,
    NUWAX_AUDIO_BOARD_LEASE_TX = 1U << 1,
    NUWAX_AUDIO_BOARD_LEASE_DIAGNOSTIC = 1U << 2,
} nuwax_audio_board_lease_channel_t;

/*
 * An audio channel lease is valid only for one initialized board generation.
 * The token prevents a stale owner from releasing a channel reacquired by a
 * newer owner after recovery or reinitialization.
 */
typedef struct {
    uint32_t generation;
    uint32_t token;
    uint8_t channels;
} nuwax_audio_board_lease_t;

/* Refcounted: first call brings the backend up, last deinit tears it down. */
esp_err_t nuwax_audio_board_init(void);
esp_err_t nuwax_audio_board_deinit(void);

const nuwax_audio_board_caps_t *nuwax_audio_board_get_caps(void);

/* Runtime health is process state, not a static board capability. */
void nuwax_audio_board_set_hardware_ref_health(nuwax_hardware_ref_health_t state);
nuwax_hardware_ref_health_t nuwax_audio_board_get_hardware_ref_health(void);

/*
 * Acquire the shared capture/playback I2S channel. Handle is returned as void*
 * (cast to i2s_chan_handle_t by the caller) to keep this header driver-free.
 * For shared_i2s=false boards these return ESP_ERR_NOT_SUPPORTED and *out=NULL,
 * signalling the caller to create its own legacy channel.
 */
esp_err_t nuwax_audio_board_acquire_rx(void **out_rx_handle,
                                       nuwax_audio_board_lease_t *out_lease);
esp_err_t nuwax_audio_board_acquire_tx(void **out_tx_handle,
                                       nuwax_audio_board_lease_t *out_lease);
esp_err_t nuwax_audio_board_acquire_diagnostic(void **out_rx_handle,
                                               void **out_tx_handle,
                                               nuwax_audio_board_lease_t *out_lease);
esp_err_t nuwax_audio_board_release(nuwax_audio_board_lease_t *lease);
bool nuwax_audio_board_diagnostic_lease_is_held(void);

/* Monotonic modulo-2^32 count of RX DMA queue overflows. */
uint32_t nuwax_audio_board_get_rx_overflow_count(void);

/* Shared-TX physical safety controls. Feedback is the GPIO output latch only. */
esp_err_t nuwax_audio_board_force_speaker_mute(void);
esp_err_t nuwax_audio_board_prepare_speaker_playback(void);

/*
 * P2-1 acceptance self-test: run `cycles` rounds of
 * init -> acquire rx+tx -> (verify double-acquire rejected) -> release -> deinit
 * and confirm the refcount returns to zero with no resource-busy / OOM.
 * Returns ESP_OK only if every cycle is clean. Safe to call on any board:
 * on legacy (shared_i2s=false) it validates the init/deinit refcount path and
 * that acquire reports ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t nuwax_audio_board_lifecycle_selftest(int cycles);
