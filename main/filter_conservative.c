/*
 * filter_conservative.c —— M2 第一层保守清洗
 *
 * 5 类门禁(高容错,产品要求"容错高一点,过滤大部分杂志即可"):
 *   1. 能量门禁 (energy):< min 或 > max 视为非语音
 *   2. AEC 残余 (aec_erle):< 阈值视为设备自己声音,丢弃
 *   3. 持续时间 (duration):短促声(< 200ms) 视为杂音
 *   4. 频谱粗筛 (spectrum):0-200Hz 风扇、> 8kHz 键盘尖峰
 *   5. 过零率 (zcr):过零率极端(0.05 或 0.5) 视为杂音
 *
 * 失败模式:5 类门禁 **任一** 不过,drop frame
 * (容错优先:多丢一些不要让杂音上云)
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"

#define TAG "filter_conservative"
#define FRAME_SAMPLES 480  /* 16kHz 30ms */

typedef struct {
    int16_t pcm[FRAME_SAMPLES];
    uint32_t ts_ms;
    float mic_rms;
    float aec_erle_db;       /* 来自 P3-1 */
    bool has_aec;
} filter_frame_in_t;

typedef enum {
    FILTER_KEEP = 0,
    FILTER_DROP_LOW_ENERGY,
    FILTER_DROP_HIGH_ENERGY,
    FILTER_DROP_AEC_RESIDUAL,
    FILTER_DROP_SHORT,
    FILTER_DROP_FAN,
    FILTER_DROP_KEYBOARD,
    FILTER_DROP_ZCR
} filter_result_t;

/* 阈值表(Kconfig 可改) */
static const float TH_MIN_RMS   = 600.0f;   /* min mic_rms */
static const float TH_MAX_RMS   = 14000.0f; /* max mic_rms */
static const float TH_AEC_ERLE  = 4.0f;     /* AEC 残余 < 4dB 视为自己声音 */
static const float TH_FAN_HZ    = 200.0f;   /* 风扇频率上限 */
static const float TH_KEYBD_HZ  = 6000.0f;  /* 键盘尖峰下限 */
static const float TH_ZCR_LOW   = 0.05f;
static const float TH_ZCR_HI    = 0.50f;

/* 计算 0-200Hz / > 6kHz 能量占比 */
static void spectrum_ratio(const int16_t *pcm, int n, float *fan_ratio, float *keybd_ratio) {
    /* 简化:用平均过零间隔粗略估计主频 */
    int zc = 0;
    for (int i = 1; i < n; i++) if ((pcm[i-1] >= 0) != (pcm[i] >= 0)) zc++;
    float zcr = (float)zc / (float)n;
    /* 主频 ≈ 8000 / (2*zcr) Hz (Nyquist 8kHz) */
    float dom_hz = zcr > 0.01f ? 8000.0f / (2.0f * zcr) : 0.0f;
    *fan_ratio   = (dom_hz < TH_FAN_HZ) ? 1.0f : 0.0f;
    *keybd_ratio = (dom_hz > TH_KEYBD_HZ) ? 1.0f : 0.0f;
}

/* 5 类门禁评估 */
filter_result_t filter_conservative_eval(const filter_frame_in_t *f) {
    /* 1. 能量门禁 */
    if (f->mic_rms < TH_MIN_RMS) return FILTER_DROP_LOW_ENERGY;
    if (f->mic_rms > TH_MAX_RMS) return FILTER_DROP_HIGH_ENERGY;
    /* 2. AEC 残余:自己声音丢弃(优先) */
    if (f->has_aec && f->aec_erle_db < TH_AEC_ERLE) return FILTER_DROP_AEC_RESIDUAL;
    /* 4. 频谱粗筛 */
    float fan_r, key_r;
    spectrum_ratio(f->pcm, FRAME_SAMPLES, &fan_r, &key_r);
    if (fan_r > 0.7f) return FILTER_DROP_FAN;
    if (key_r > 0.7f) return FILTER_DROP_KEYBOARD;
    /* 3. 持续时间门禁:不在这儿判(要看 history) */
    /* 5. 过零率 */
    int zc = 0;
    for (int i = 1; i < FRAME_SAMPLES; i++) if ((f->pcm[i-1] >= 0) != (f->pcm[i] >= 0)) zc++;
    float zcr = (float)zc / (float)FRAME_SAMPLES;
    if (zcr < TH_ZCR_LOW || zcr > TH_ZCR_HI) return FILTER_DROP_ZCR;
    return FILTER_KEEP;
}

const char *filter_result_str(filter_result_t r) {
    switch (r) {
        case FILTER_KEEP: return "KEEP";
        case FILTER_DROP_LOW_ENERGY: return "DROP_LOW_ENERGY";
        case FILTER_DROP_HIGH_ENERGY: return "DROP_HIGH_ENERGY";
        case FILTER_DROP_AEC_RESIDUAL: return "DROP_AEC_RESIDUAL";
        case FILTER_DROP_SHORT: return "DROP_SHORT";
        case FILTER_DROP_FAN: return "DROP_FAN";
        case FILTER_DROP_KEYBOARD: return "DROP_KEYBOARD";
        case FILTER_DROP_ZCR: return "DROP_ZCR";
        default: return "UNKNOWN";
    }
}
