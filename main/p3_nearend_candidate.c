#include "p3_nearend_candidate.h"

#include <math.h>

#include "esp_err.h"
#include "esp_log.h"

#ifndef CONFIG_NUWAX_P3_NEAREND_CANDIDATE_RECORD_ENABLE
#define CONFIG_NUWAX_P3_NEAREND_CANDIDATE_RECORD_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_REF_RATIO
#define CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_REF_RATIO 600
#endif

#ifndef CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_OUT_RMS
#define CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_OUT_RMS 140
#endif

#ifndef CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_MIC_RMS
#define CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_MIC_RMS 900
#endif

#ifndef CONFIG_NUWAX_P3_NEAREND_MIN_OUT_TO_REF_DB
#define CONFIG_NUWAX_P3_NEAREND_MIN_OUT_TO_REF_DB -1600
#endif

#ifndef CONFIG_NUWAX_P3_NEAREND_MAX_CORR_AFTER_ABS
#define CONFIG_NUWAX_P3_NEAREND_MAX_CORR_AFTER_ABS 100
#endif

#ifndef CONFIG_NUWAX_P3_NEAREND_STRONG_MIC_RMS
#define CONFIG_NUWAX_P3_NEAREND_STRONG_MIC_RMS 1800
#endif

#ifndef CONFIG_NUWAX_P3_NEAREND_STRONG_OUT_RMS
#define CONFIG_NUWAX_P3_NEAREND_STRONG_OUT_RMS 900
#endif

#ifndef CONFIG_NUWAX_P3_NEAREND_STRONG_OUT_TO_REF_DB
#define CONFIG_NUWAX_P3_NEAREND_STRONG_OUT_TO_REF_DB -600
#endif

#ifndef CONFIG_NUWAX_P6_RECORD_LOW_REF_RATIO_MAX
#define CONFIG_NUWAX_P6_RECORD_LOW_REF_RATIO_MAX CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_REF_RATIO
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_MIN_OUT_TO_REF_DB
#define CONFIG_NUWAX_P6_CANCEL_MIN_OUT_TO_REF_DB -400
#endif

#define NUWAX_CONFIG_RATIO_X1000(value) ((float)(value) / 1000.0f)
#define NUWAX_CONFIG_DB_X100(value) ((float)(value) / 100.0f)

#ifndef CONFIG_NUWAX_P6_CANCEL_MIN_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_MIN_MIC_RMS 1200.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_MIN_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_MIN_OUT_RMS 350.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_PRIMARY_MIN_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_PRIMARY_MIN_MIC_RMS 3600.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_RESIDUAL_MIN_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_RESIDUAL_MIN_MIC_RMS 3000.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_RESIDUAL_MIN_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_RESIDUAL_MIN_OUT_RMS 550.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_LOW_REF_MIN_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_LOW_REF_MIN_MIC_RMS 1800.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_LOW_REF_MAX_ERLE_DB
#define CONFIG_NUWAX_P6_CANCEL_LOW_REF_MAX_ERLE_DB 12.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MIN_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MIN_MIC_RMS 2600.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MIN_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MIN_OUT_RMS 550.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_OUT_RMS 800.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_ERLE_DB
#define CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_ERLE_DB 13.5f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_OUT_TO_REF_DB
#define CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_OUT_TO_REF_DB 0.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_RATIO_MAX
#define CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_RATIO_MAX 0.45f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MIN_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MIN_MIC_RMS 1400.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MIN_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MIN_OUT_RMS 300.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_OUT_RMS 500.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_OUT_TO_REF_DB
#define CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_OUT_TO_REF_DB -4.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_ERLE_DB
#define CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_ERLE_DB 14.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_CORR_AFTER_ABS
#define CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_CORR_AFTER_ABS 0.030f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_LOW_MIC_MAX_RMS
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_LOW_MIC_MAX_RMS 1700.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_LOW_MIC_MIN_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_LOW_MIC_MIN_OUT_RMS 430.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_LOW_MIC_MAX_OUT_TO_REF_DB
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_LOW_MIC_MAX_OUT_TO_REF_DB 0.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_MAX_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_MAX_OUT_RMS 950.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_MAX_OUT_TO_REF_DB
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_MAX_OUT_TO_REF_DB 2.5f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_MIC_MIN_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_HIGH_MIC_MIN_OUT_RMS 550.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_MIC_MIN_ERLE_DB
#define CONFIG_NUWAX_P6_CANCEL_HIGH_MIC_MIN_ERLE_DB 17.5f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_RATIO_MIN
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_RATIO_MIN 0.80f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_RATIO_MAX
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_RATIO_MAX 0.93f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_MIC_RMS 3800.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_OUT_RMS 700.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MAX_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MAX_OUT_RMS 950.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MAX_OUT_TO_REF_DB
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MAX_OUT_TO_REF_DB 4.8f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_ERLE_DB
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_ERLE_DB 12.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_RATIO_MIN
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_RATIO_MIN 0.885f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_RATIO_MAX
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_RATIO_MAX 0.930f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MIN_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MIN_MIC_RMS 3500.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MIN_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MIN_OUT_RMS 900.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_OUT_RMS 1500.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MIN_OUT_TO_REF_DB
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MIN_OUT_TO_REF_DB 2.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_OUT_TO_REF_DB
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_OUT_TO_REF_DB 6.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_OUT_TO_MIC_DB
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_OUT_TO_MIC_DB -8.5f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_ERLE_DB
#define CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_ERLE_DB 12.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_RATIO_MIN
#define CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_RATIO_MIN 0.62f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_RATIO_MAX
#define CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_RATIO_MAX 0.75f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_MIC_RMS 3800.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_OUT_RMS 900.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MAX_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MAX_OUT_RMS 1250.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MAX_OUT_TO_REF_DB
#define CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MAX_OUT_TO_REF_DB 2.8f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_ERLE_DB
#define CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_ERLE_DB 12.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MIN_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MIN_MIC_RMS 1600.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_MIC_RMS
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_MIC_RMS 2300.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MIN_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MIN_OUT_RMS 350.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_OUT_RMS
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_OUT_RMS 750.0f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_REF_RATIO_MAX
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_REF_RATIO_MAX 0.885f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_OUT_TO_REF_DB
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_OUT_TO_REF_DB 2.5f
#endif

#ifndef CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_ERLE_DB
#define CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_ERLE_DB 12.0f
#endif

static const char *TAG = "nuwax_p3_nearend";

static float db_ratio(float numerator, float denominator)
{
    if (numerator <= 0.0f) {
        return -120.0f;
    }
    if (denominator <= 0.0f) {
        return 120.0f;
    }
    return 20.0f * log10f(numerator / denominator);
}

static void evaluate_candidate(nuwax_p3_nearend_candidate_result_t *r)
{
    if (!r) {
        return;
    }
    r->ready = r->aec.diagnostic_ready;
    r->active_out_to_ref_db = db_ratio(r->aec.active_out_rms, r->aec.active_ref_rms);
    r->active_out_to_mic_db = db_ratio(r->aec.active_out_rms, r->aec.active_mic_rms);
    r->mic_ok = r->aec.active_mic_rms >= (float)CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_MIC_RMS;
    r->strong_nearend_ok =
        r->aec.active_mic_rms >= (float)CONFIG_NUWAX_P3_NEAREND_STRONG_MIC_RMS &&
        r->aec.active_out_rms >= (float)CONFIG_NUWAX_P3_NEAREND_STRONG_OUT_RMS &&
        r->active_out_to_ref_db >=
            NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P3_NEAREND_STRONG_OUT_TO_REF_DB);
    r->quality_gate_ok = r->aec.quality_ok || r->strong_nearend_ok;
    /*
     * Near-end speech intentionally raises the AEC residual, so the playback-only
     * ERLE quality gate can be bypassed only for strong near-end evidence.
     */
    r->gate_ok = r->aec.data_plane_ok &&
                 r->aec.resource_ok &&
                 r->quality_gate_ok &&
                 r->aec.active_samples > 0 &&
                 r->aec.active_ref_ratio >=
                     NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_REF_RATIO) &&
                 r->aec.active_corr_abs_after <=
                     NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_NEAREND_MAX_CORR_AFTER_ABS);
    r->energy_ok = r->mic_ok &&
                   r->aec.active_out_rms >= (float)CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_OUT_RMS &&
                   r->active_out_to_ref_db >=
                       NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P3_NEAREND_MIN_OUT_TO_REF_DB);
    r->candidate = r->gate_ok && r->energy_ok;
    r->nearend_observed = r->mic_ok && r->energy_ok;
    r->p6_low_ref_nearend_ok = r->nearend_observed &&
                               r->aec.data_plane_ok &&
                               r->aec.resource_ok &&
                                r->aec.active_ref_ratio <=
                                    NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P6_RECORD_LOW_REF_RATIO_MAX);
    const bool p6_low_ref_normal_voice_ok =
        r->aec.active_ref_ratio <=
            NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P6_RECORD_LOW_REF_RATIO_MAX) &&
        r->aec.active_mic_rms >= (float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MIN_MIC_RMS &&
        r->aec.active_out_rms >= (float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MIN_OUT_RMS &&
        r->aec.active_out_rms <= (float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_OUT_RMS &&
        r->active_out_to_ref_db >=
            NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P6_CANCEL_MIN_OUT_TO_REF_DB) &&
        r->active_out_to_ref_db <= (float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_OUT_TO_REF_DB &&
        r->aec.active_erle_db <= (float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_ERLE_DB;
    const bool p6_very_low_ref_soft_voice_ok =
        r->aec.active_ref_ratio <= (float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_RATIO_MAX &&
        r->aec.active_mic_rms >= (float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MIN_MIC_RMS &&
        r->aec.active_out_rms >= (float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MIN_OUT_RMS &&
        r->aec.active_out_rms <= (float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_OUT_RMS &&
        r->active_out_to_ref_db <= (float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_OUT_TO_REF_DB &&
        r->aec.active_erle_db <= (float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_ERLE_DB &&
        r->aec.active_corr_abs_after <= (float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_CORR_AFTER_ABS;
    r->p6_low_ref_normal_voice_ok = p6_low_ref_normal_voice_ok || p6_very_low_ref_soft_voice_ok;
    /*
     * Bare low ERLE and strong residual can also appear in playback-only echo
     * residue. Low-ref cancel therefore requires the narrower normal voice
     * shape; broad low-ref/strong evidence remains record/diagnostic only.
     */
    const bool p6_low_ref_cancel_energy_ok =
        r->aec.active_ref_ratio <=
            NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P6_RECORD_LOW_REF_RATIO_MAX) &&
        r->aec.active_mic_rms >= (float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_MIN_MIC_RMS &&
        r->aec.active_out_rms >= (float)CONFIG_NUWAX_P6_CANCEL_MIN_OUT_RMS &&
        r->active_out_to_ref_db >=
            NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P6_CANCEL_MIN_OUT_TO_REF_DB) &&
        r->p6_low_ref_normal_voice_ok;
    const bool p6_normal_ref =
        r->aec.active_ref_ratio >
            NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P6_RECORD_LOW_REF_RATIO_MAX);
    const bool p6_high_ref_voice_ok =
        p6_normal_ref &&
        r->aec.quality_ok &&
        r->aec.active_ref_ratio >= (float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_RATIO_MIN &&
        r->aec.active_ref_ratio <= (float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_RATIO_MAX &&
        r->aec.active_mic_rms >= (float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_MIC_RMS &&
        r->aec.active_out_rms >= (float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_OUT_RMS &&
        r->aec.active_out_rms <= (float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MAX_OUT_RMS &&
        r->active_out_to_ref_db <= (float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MAX_OUT_TO_REF_DB &&
        r->aec.active_erle_db >= (float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_ERLE_DB;
    /*
     * The older high-ref strong shortcut overlaps with playback-only echo
     * residue on the current V1.2/P3-0S path. Keep the field for log/schema
     * stability, but do not let it authorize gray-cancel until we have a
     * stronger discriminator than RMS shape alone.
     */
    r->p6_high_ref_strong_voice_ok = false;
    r->p6_high_ref_voice_ok = p6_high_ref_voice_ok;
    r->p6_mid_ref_strong_voice_ok =
        p6_normal_ref &&
        r->candidate &&
        r->strong_nearend_ok &&
        r->aec.quality_ok &&
        r->aec.active_ref_ratio >= (float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_RATIO_MIN &&
        r->aec.active_ref_ratio <= (float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_RATIO_MAX &&
        r->aec.active_mic_rms >= (float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_MIC_RMS &&
        r->aec.active_out_rms >= (float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_OUT_RMS &&
        r->aec.active_out_rms <= (float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MAX_OUT_RMS &&
        r->active_out_to_ref_db >=
            NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P6_CANCEL_MIN_OUT_TO_REF_DB) &&
        r->active_out_to_ref_db <= (float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MAX_OUT_TO_REF_DB &&
        r->aec.active_erle_db >= (float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_ERLE_DB &&
        r->aec.active_corr_abs_after <=
            NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_NEAREND_MAX_CORR_AFTER_ABS);
    r->p6_normal_voice_ok =
        p6_normal_ref &&
        r->candidate &&
        r->aec.active_ref_ratio <= (float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_REF_RATIO_MAX &&
        r->aec.active_mic_rms >= (float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MIN_MIC_RMS &&
        r->aec.active_mic_rms <= (float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_MIC_RMS &&
        r->aec.active_out_rms >= (float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MIN_OUT_RMS &&
        r->aec.active_out_rms <= (float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_OUT_RMS &&
        r->active_out_to_ref_db <= (float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_OUT_TO_REF_DB &&
        r->aec.active_erle_db <= (float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_ERLE_DB;
    /*
     * P6-LIVEBARGEIN-004-4C: narrow normalvoice cancel class.
     * Targets ref_ratio 0.60..0.75 (mid-ref with residual playback) and a tight
     * acoustic shape that excludes both low-mic echo residue and high-mic yell.
     */
    r->p6_mid_ref_high_erle_voice_ok =
#ifndef CONFIG_NUWAX_P6_NARROW_NORMALVOICE_REF_RATIO_LO
        r->aec.active_ref_ratio >= 0.60f &&
        r->aec.active_ref_ratio <= 0.75f &&
        r->aec.active_mic_rms >= 3800.0f &&
        r->aec.active_out_rms >= 550.0f &&
        r->aec.active_out_rms <= 900.0f &&
        r->active_out_to_ref_db >= -4.0f &&
        r->active_out_to_ref_db <= 0.0f &&
        r->aec.active_erle_db >= 17.5f
#else
        r->aec.active_ref_ratio >=
            NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P6_NARROW_NORMALVOICE_REF_RATIO_LO) &&
        r->aec.active_ref_ratio <=
            NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P6_NARROW_NORMALVOICE_REF_RATIO_HI) &&
        r->aec.active_mic_rms >= (float)CONFIG_NUWAX_P6_NARROW_NORMALVOICE_MIC_RMS_LO &&
        r->aec.active_out_rms >= (float)CONFIG_NUWAX_P6_NARROW_NORMALVOICE_OUT_RMS_LO &&
        r->aec.active_out_rms <= (float)CONFIG_NUWAX_P6_NARROW_NORMALVOICE_OUT_RMS_HI &&
        r->active_out_to_ref_db >=
            NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P6_NARROW_NORMALVOICE_OUT_TO_REF_DB_LO) &&
        r->active_out_to_ref_db <=
            NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P6_NARROW_NORMALVOICE_OUT_TO_REF_DB_HI) &&
        r->aec.active_erle_db >=
            NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P6_NARROW_NORMALVOICE_ERLE_DB_LO)
#endif
        &&
        r->aec.data_plane_ok &&
        r->aec.quality_ok &&
        r->gate_ok &&
        r->aec.resource_ok;
    /*
     * Do not let the broad normal-ref RMS shape authorize cancel by itself.
     * Playback-only echo residue can hit the same mic/out RMS band, so normal
     * ref cancel now requires one of the explicit P6 voice classes above.
     */
    const bool p6_normal_ref_cancel_energy_ok =
        p6_normal_ref &&
        (r->p6_high_ref_voice_ok ||
         r->p6_mid_ref_strong_voice_ok ||
         r->p6_normal_voice_ok ||
         r->p6_mid_ref_high_erle_voice_ok);
    const bool p6_cancel_energy_ok = p6_low_ref_cancel_energy_ok || p6_normal_ref_cancel_energy_ok;
    /* P6-LIVEBARGEIN-004-1B: single-mic voice shape strict OR, plus dual-mic */
    r->p6_single_mic_voice_shape_ok =
        r->p6_low_ref_normal_voice_ok ||
        r->p6_high_ref_voice_ok ||
        r->p6_mid_ref_strong_voice_ok ||
        r->p6_normal_voice_ok ||
        r->p6_mid_ref_high_erle_voice_ok;
    r->p6_cancel_discriminator_ok =
        r->aec.active_dual_mic_voice_ok || r->p6_single_mic_voice_shape_ok;
    const bool p6_cancel_signal_ok =
        r->nearend_observed &&
        r->aec.data_plane_ok &&
        r->aec.read_timeouts == 0 &&
        r->aec.ref_underrun_ratio <= 0.05f /* placeholder; P3 AB max underrun */ &&
        r->aec.active_corr_abs_after <=
            NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_NEAREND_MAX_CORR_AFTER_ABS) &&
        r->p6_cancel_discriminator_ok &&
        p6_cancel_energy_ok;
    /*
     * P6 gray cancel keeps the data/correlation/energy safety gates. Dual-mic evidence
     * is no longer the only hard gate on the current Vocat layout; a narrow P6 single-mic
     * voice shape may also authorize the discriminator, while broad low-ref nearend
     * evidence remains record/diagnostic only.
     */
    r->p6_cancel_candidate = p6_cancel_signal_ok;
    /*
     * Once gray cancel is safe enough to stop the old TTS, the same window must
     * be treated as recordable barge-in audio. Otherwise normal voice can mute
     * playback but leave the follow-up turn with only post-cancel silence.
     */
    r->p6_record_candidate = r->candidate || r->p6_low_ref_nearend_ok || r->p6_cancel_candidate;
}

static void log_candidate_result(esp_err_t err, const nuwax_p3_nearend_candidate_result_t *result)
{
    const nuwax_p3_nearend_candidate_result_t zero = {0};
    const nuwax_p3_nearend_candidate_result_t *r = result ? result : &zero;
    ESP_LOGW(TAG,
             "P3-1-NEAREND-CANDIDATE err=%s record_only=1 candidate=%d ready=%d "
             "gate_ok=%d quality_gate_ok=%d strong_nearend_ok=%d mic_ok=%d energy_ok=%d "
             "nearend_observed=%d p6_low_ref_nearend_ok=%d p6_low_ref_normal_voice_ok=%d "
             "p6_high_ref_voice_ok=%d "
             "p6_high_ref_strong_voice_ok=%d p6_mid_ref_strong_voice_ok=%d p6_normal_voice_ok=%d p6_mid_ref_high_erle_voice_ok=%d "
             "p6_single_mic_voice_shape_ok=%d p6_cancel_discriminator_ok=%d "
             "p6_record_candidate=%d p6_cancel_candidate=%d "
             "duration_ms=%lu delay_samples=%lu "
             "active_ref_ratio=%.3f active_mic_rms=%.1f active_ref_rms=%.1f active_out_rms=%.1f "
             "active_out_to_ref_db=%.2f active_out_to_mic_db=%.2f active_erle_db=%.2f "
             "active_mic2_rms=%.1f active_dual_mic_voice_ok=%d "
             "active_dual_mic_corr=%.3f active_dual_mic_abs_corr=%.3f "
             "active_dual_mic_level_delta_db=%.2f active_dual_mic_diff_rms=%.1f "
             "active_dual_mic_diff_ratio=%.3f "
             "active_corr_abs_after=%.3f data_ok=%d quality_ok=%d resource_ok=%d "
             "ref_underrun_ratio=%.3f read_timeouts=%lu heap_internal=%u heap_internal_largest=%u "
             "min_active_ref_ratio=%.3f min_active_mic_rms=%.1f min_active_out_rms=%.1f min_out_to_ref_db=%.1f "
             "strong_mic_rms=%.1f strong_out_rms=%.1f strong_out_to_ref_db=%.1f "
             "p6_low_ref_ratio_max=%.3f p6_cancel_min_mic_rms=%.1f "
             "p6_cancel_min_out_rms=%.1f p6_cancel_min_out_to_ref_db=%.1f "
             "p6_cancel_primary_min_mic_rms=%.1f p6_cancel_residual_min_mic_rms=%.1f "
             "p6_cancel_residual_min_out_rms=%.1f "
             "p6_cancel_low_ref_min_mic_rms=%.1f p6_cancel_low_ref_max_erle_db=%.1f "
             "p6_cancel_low_ref_normal_min_mic_rms=%.1f p6_cancel_low_ref_normal_min_out_rms=%.1f "
             "p6_cancel_low_ref_normal_max_out_rms=%.1f "
             "p6_cancel_low_ref_normal_max_erle_db=%.1f "
             "p6_cancel_low_ref_normal_max_out_to_ref_db=%.1f "
             "p6_cancel_very_low_ref_ratio_max=%.3f "
             "p6_cancel_very_low_ref_soft_min_mic_rms=%.1f "
             "p6_cancel_very_low_ref_soft_min_out_rms=%.1f "
             "p6_cancel_very_low_ref_soft_max_out_rms=%.1f "
             "p6_cancel_very_low_ref_soft_max_out_to_ref_db=%.1f "
             "p6_cancel_very_low_ref_soft_max_erle_db=%.1f "
             "p6_cancel_very_low_ref_soft_max_corr_after_abs=%.3f "
             "p6_cancel_normal_low_mic_max_rms=%.1f "
             "p6_cancel_normal_low_mic_min_out_rms=%.1f p6_cancel_normal_max_out_rms=%.1f "
             "p6_cancel_normal_max_out_to_ref_db=%.1f p6_cancel_high_mic_min_out_rms=%.1f "
             "p6_cancel_high_mic_min_erle_db=%.1f "
             "p6_cancel_high_ref_voice_ratio_min=%.2f p6_cancel_high_ref_voice_ratio_max=%.2f "
             "p6_cancel_high_ref_voice_min_mic_rms=%.1f p6_cancel_high_ref_voice_min_out_rms=%.1f "
             "p6_cancel_high_ref_voice_max_out_rms=%.1f p6_cancel_high_ref_voice_max_out_to_ref_db=%.1f "
             "p6_cancel_high_ref_voice_min_erle_db=%.1f "
             "p6_cancel_high_ref_strong_ratio_min=%.3f p6_cancel_high_ref_strong_ratio_max=%.3f "
             "p6_cancel_high_ref_strong_min_mic_rms=%.1f "
             "p6_cancel_high_ref_strong_min_out_rms=%.1f p6_cancel_high_ref_strong_max_out_rms=%.1f "
             "p6_cancel_high_ref_strong_min_out_to_ref_db=%.1f "
             "p6_cancel_high_ref_strong_max_out_to_ref_db=%.1f "
             "p6_cancel_high_ref_strong_max_out_to_mic_db=%.1f "
             "p6_cancel_high_ref_strong_max_erle_db=%.1f "
             "p6_cancel_mid_ref_strong_ratio_min=%.3f p6_cancel_mid_ref_strong_ratio_max=%.3f "
             "p6_cancel_mid_ref_strong_min_mic_rms=%.1f "
             "p6_cancel_mid_ref_strong_min_out_rms=%.1f p6_cancel_mid_ref_strong_max_out_rms=%.1f "
             "p6_cancel_mid_ref_strong_max_out_to_ref_db=%.1f "
             "p6_cancel_mid_ref_strong_min_erle_db=%.1f "
             "p6_cancel_normal_voice_min_mic_rms=%.1f p6_cancel_normal_voice_max_mic_rms=%.1f "
             "p6_cancel_normal_voice_min_out_rms=%.1f p6_cancel_normal_voice_max_out_rms=%.1f "
             "p6_cancel_normal_voice_ref_ratio_max=%.3f p6_cancel_normal_voice_max_out_to_ref_db=%.1f "
             "p6_cancel_normal_voice_max_erle_db=%.1f "
             "max_corr_after_abs=%.3f action=record_only",
             esp_err_to_name(err),
             r->candidate ? 1 : 0,
             r->ready ? 1 : 0,
             r->gate_ok ? 1 : 0,
             r->quality_gate_ok ? 1 : 0,
             r->strong_nearend_ok ? 1 : 0,
             r->mic_ok ? 1 : 0,
             r->energy_ok ? 1 : 0,
             r->nearend_observed ? 1 : 0,
             r->p6_low_ref_nearend_ok ? 1 : 0,
             r->p6_low_ref_normal_voice_ok ? 1 : 0,
             r->p6_high_ref_voice_ok ? 1 : 0,
             r->p6_high_ref_strong_voice_ok ? 1 : 0,
             r->p6_mid_ref_strong_voice_ok ? 1 : 0,
             r->p6_normal_voice_ok ? 1 : 0,
             r->p6_mid_ref_high_erle_voice_ok ? 1 : 0,
             r->p6_single_mic_voice_shape_ok ? 1 : 0,
             r->p6_cancel_discriminator_ok ? 1 : 0,
             r->p6_record_candidate ? 1 : 0,
             r->p6_cancel_candidate ? 1 : 0,
             (unsigned long)r->duration_ms,
             (unsigned long)r->delay_samples,
             (double)r->aec.active_ref_ratio,
             (double)r->aec.active_mic_rms,
             (double)r->aec.active_ref_rms,
             (double)r->aec.active_out_rms,
             (double)r->active_out_to_ref_db,
             (double)r->active_out_to_mic_db,
             (double)r->aec.active_erle_db,
             (double)r->aec.active_mic2_rms,
             r->aec.active_dual_mic_voice_ok ? 1 : 0,
             (double)r->aec.active_dual_mic_corr,
             (double)r->aec.active_dual_mic_abs_corr,
             (double)r->aec.active_dual_mic_level_delta_db,
             (double)r->aec.active_dual_mic_diff_rms,
             (double)r->aec.active_dual_mic_diff_ratio,
             (double)r->aec.active_corr_abs_after,
             r->aec.data_plane_ok ? 1 : 0,
             r->aec.quality_ok ? 1 : 0,
             r->aec.resource_ok ? 1 : 0,
             (double)r->aec.ref_underrun_ratio,
             (unsigned long)r->aec.read_timeouts,
             (unsigned)r->aec.heap_internal_free,
             (unsigned)r->aec.heap_internal_largest,
             (double)NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_REF_RATIO),
             (double)((float)CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P3_NEAREND_MIN_ACTIVE_OUT_RMS),
             (double)NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P3_NEAREND_MIN_OUT_TO_REF_DB),
             (double)((float)CONFIG_NUWAX_P3_NEAREND_STRONG_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P3_NEAREND_STRONG_OUT_RMS),
             (double)NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P3_NEAREND_STRONG_OUT_TO_REF_DB),
             (double)NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P6_RECORD_LOW_REF_RATIO_MAX),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_MIN_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_MIN_OUT_RMS),
             (double)NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P6_CANCEL_MIN_OUT_TO_REF_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_PRIMARY_MIN_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_RESIDUAL_MIN_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_RESIDUAL_MIN_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_MIN_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_MAX_ERLE_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MIN_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MIN_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_ERLE_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_LOW_REF_NORMAL_MAX_OUT_TO_REF_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_RATIO_MAX),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MIN_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MIN_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_OUT_TO_REF_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_ERLE_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_VERY_LOW_REF_SOFT_MAX_CORR_AFTER_ABS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_LOW_MIC_MAX_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_LOW_MIC_MIN_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_MAX_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_MAX_OUT_TO_REF_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_MIC_MIN_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_MIC_MIN_ERLE_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_RATIO_MIN),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_RATIO_MAX),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MAX_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MAX_OUT_TO_REF_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_VOICE_MIN_ERLE_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_RATIO_MIN),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_RATIO_MAX),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MIN_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MIN_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MIN_OUT_TO_REF_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_OUT_TO_REF_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_OUT_TO_MIC_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_HIGH_REF_STRONG_MAX_ERLE_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_RATIO_MIN),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_RATIO_MAX),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MAX_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MAX_OUT_TO_REF_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_MID_REF_STRONG_MIN_ERLE_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MIN_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_MIC_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MIN_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_OUT_RMS),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_REF_RATIO_MAX),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_OUT_TO_REF_DB),
             (double)((float)CONFIG_NUWAX_P6_CANCEL_NORMAL_VOICE_MAX_ERLE_DB),
             (double)NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_NEAREND_MAX_CORR_AFTER_ABS));
}

esp_err_t nuwax_p3_nearend_candidate_run(uint32_t duration_ms,
                                         uint32_t delay_samples,
                                         nuwax_p3_nearend_candidate_result_t *out_result)
{
    nuwax_p3_nearend_candidate_result_t result = {
        .duration_ms = duration_ms,
        .delay_samples = delay_samples,
    };

#if !CONFIG_NUWAX_P3_NEAREND_CANDIDATE_RECORD_ENABLE
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    log_candidate_result(err, &result);
    if (out_result) {
        *out_result = result;
    }
    return err;
#else
    esp_err_t err = nuwax_p3_softref_aec_ab_run(duration_ms, delay_samples, &result.aec);
    evaluate_candidate(&result);
    log_candidate_result(err, &result);
    if (out_result) {
        *out_result = result;
    }
    return err;
#endif
}

esp_err_t nuwax_p3_nearend_candidate_evaluate(uint32_t duration_ms,
                                              uint32_t delay_samples,
                                              const nuwax_p3_softref_aec_ab_result_t *aec,
                                              nuwax_p3_nearend_candidate_result_t *out_result)
{
    if (!aec || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    nuwax_p3_nearend_candidate_result_t result = {
        .duration_ms = duration_ms,
        .delay_samples = delay_samples,
        .aec = *aec,
    };
    evaluate_candidate(&result);
    *out_result = result;
    return ESP_OK;
}
