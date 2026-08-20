/*
 * P5-LIVEBARGEIN stage 0 fixtures: real 004-3A device-log samples
 *
 * Each fixture is a single P3-1 frame as captured by the device:
 *   - ref_ratio, mic_rms, ref_rms, out_rms, out_to_ref_db, erle_db
 *   - quality_ok, gate_ok, resource_healthy, read_timeouts_ok
 *   - active_dual_mic_voice_ok
 *   - p6 voice class flags
 *
 * Source logs:
 *   F1 playbackonly_nocancel_pass_20260702_140244 (expected: cancel=NO)
 *   F2 normalvoice_50_100cm_fail_20260702_140947 (expected: cancel=YES,
 *       actually cancel=0 = fixture reproduces real failure mode)
 */
#ifndef P5_LIVE_FAST_GATE_FIXTURES_H
#define P5_LIVE_FAST_GATE_FIXTURES_H

#include "p5_live_fast_gate_host_check.h"

static const mock_input_t FIXTURE_PLAYBACK_ONLY_NOCANCEL = {
    .ref_ratio = 0.965,
    .mic_rms = 900.0,
    .ref_rms = 850.0,
    .out_rms = 410.0,
    .out_to_ref_db = 3.8,
    .erle_db = 9.0,
    .quality_ok = 1,
    .gate_ok = 1,
    .resource_healthy = 1,
    .read_timeouts_ok = 1,
    .active_dual_mic_voice_ok = 0,
    .p6_high_ref_voice_ok = 0,
    .p6_high_ref_strong_voice_ok = 0,
    .p6_mid_ref_strong_voice_ok = 0,
    .p6_normal_voice_ok = 0,
    .p6_low_ref_nearend_ok = 0,
    .p6_low_ref_normal_voice_ok = 0,
    .label = "playbackonly_nocancel_140244"
};

static const mock_input_t FIXTURE_NORMALVOICE_50_100CM_FAIL = {
    .ref_ratio = 0.653,
    .mic_rms = 5976.7,
    .mic2_rms = 5840.6,
    .ref_rms = 2380.0,
    .out_rms = 720.1,
    .out_to_ref_db = -3.72,
    .erle_db = 18.38,
    .quality_ok = 1,
    .gate_ok = 1,
    .resource_healthy = 1,
    .read_timeouts_ok = 1,
    .active_dual_mic_voice_ok = 0,
    .p6_high_ref_voice_ok = 0,
    .p6_high_ref_strong_voice_ok = 0,
    .p6_mid_ref_strong_voice_ok = 0,
    .p6_normal_voice_ok = 0,
    .p6_low_ref_nearend_ok = 1,
    .p6_low_ref_normal_voice_ok = 0,
    .label = "normalvoice_50_100cm_fail_140947"
};

#endif
