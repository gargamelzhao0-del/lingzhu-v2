/*
 * D1 播放主链路核心代码(借鉴 xiaozhi audio_service 架构)
 *
 * 来源:xiaozhi-esp32/main/audio/audio_service.cc 架构
 * 实现:C 重写,适配 Vocat v1.0 板 + 现有 speaker_playback.c
 *
 * 流程:
 *   1. 接收服务端下行 NA frame
 *   2. 解析 22B header 拿到 codec/payload
 *   3. 按 codec 分流:PCM16/Opus 解码
 *   4. 推送到 speaker_playback 队列(已存在)
 *   5. 状态机:idle → playing → finishing → idle
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "...    d1_state_idle,             /* 等待播放 */
    d1_state_playing,            /* 正在播放 NA frame */
    d1_state_finishing,          /* 播放完末尾,等停止 */
    d1_state_max
} d1_playback_state_t;

extern int d1_playback_state;
extern int d1_playback_state_set(int new_state);

/* 借鉴 xiaozhi audio_service:状态变化时回调 */
int d1_playback_state_set(int new_state) {
    if (new_state >= d1_state_max) return -1;
    int old = d1_playback_state;
    d1_playback_state = new_state;
    /* D7 命令通道:每 100ms 探测一次状态变化 */
    return old;
}
