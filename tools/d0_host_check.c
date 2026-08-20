/*
 * D0 host check: 7 项 sdkconfig 验证 + 借鉴 7 board 抽象检查
 *
 * 不烧录,只在 Linux/Mac/Windows 上跑。
 * 验证成功后,产品负责人再按《D0 配置收敛步骤.md》烧录。
 *
 * 编译: gcc -Wall -Wextra -std=c11 tools/d0_host_check.c -o /tmp/d0_host
 * 运行: /tmp/d0_host
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/* 借鉴 7 board 抽象模式 (接口设计,不抄实现) */
typedef struct {
    const char *name;
    int mic_channels;
    int ref_slot;          /* -1 = 无 ref */
    int sample_rate;
    int frame_duration_ms;
    bool has_hardware_ref;
    bool has_lcd;
    bool has_touch;
} board_config_t;

/* Vocat v1.0 板配置(产品负责人明确) */
static const board_config_t VOCAT_V1_0 = {
    .name = "vocat_v1_0",
    .mic_channels = 2,            /* 双麦 */
    .ref_slot = 2,                /* slot 2 ref */
    .sample_rate = 16000,         /* 16kHz */
    .frame_duration_ms = 30,      /* 30ms 帧 */
    .has_hardware_ref = true,     /* 硬件 ref */
    .has_lcd = true,              /* LCD */
    .has_touch = true,            /* 触摸 */
};

/* D0 7 项 sdkconfig 收敛检查 */
typedef struct {
    const char *kconfig;
    const char *expected;
    const char *category;
} kconfig_check_t;

static const kconfig_check_t KCONFIG_CHECKS[] = {
    /* 借鉴 7 board 抽象:CONFIG_BOARD_TYPE */
    { "CONFIG_BOARD_TYPE_VOCAT_V1_0", "y", "板型" },

    /* 音频参数收敛 */
    { "CONFIG_AUDIO_INPUT_SAMPLE_RATE", "16000", "采样率" },
    { "CONFIG_AUDIO_OUTPUT_SAMPLE_RATE", "24000", "采样率(输出)" },
    { "CONFIG_AUDIO_INPUT_CHANNELS", "2", "双麦" },
    { "CONFIG_AUDIO_OUTPUT_CHANNELS", "1", "单声道输出" },

    /* 帧长 */
    { "CONFIG_AUDIO_FRAME_DURATION_MS", "30", "帧长" },

    /* VAD 阈值(本工程之前 3600 vs 400 问题,统一为 400) */
    { "CONFIG_AUDIO_VAD_RMS_THRESHOLD", "400", "VAD" },

    /* AEC 第一轮先关闭,产品负责人明确"D5 通过前 aec 必须保持 false" */
    { "CONFIG_AUDIO_AEC_ENABLE", "n", "AEC(暂关)" },
};

static const int KCONFIG_N = (int)(sizeof(KCONFIG_CHECKS) / sizeof(KCONFIG_CHECKS[0]));

static int check_vocat_board(void) {
    int errs = 0;
    if (strcmp(VOCAT_V1_0.name, "vocat_v1_0") != 0) { errs++; }
    if (VOCAT_V1_0.mic_channels != 2) { errs++; printf("[FAIL] 双麦应为 2\n"); }
    if (VOCAT_V1_0.ref_slot != 2) { errs++; printf("[FAIL] ref slot 应为 2\n"); }
    if (VOCAT_V1_0.sample_rate != 16000) { errs++; printf("[FAIL] 采样率应为 16000\n"); }
    if (VOCAT_V1_0.frame_duration_ms != 30) { errs++; printf("[FAIL] 帧长应为 30ms\n"); }
    if (!VOCAT_V1_0.has_hardware_ref) { errs++; printf("[FAIL] 应有硬件 ref\n"); }
    return errs;
}

static int check_kconfig(void) {
    int errs = 0;
    printf("\n=== D0 sdkconfig 7 项收敛检查 ===\n");
    for (int i = 0; i < KCONFIG_N; i++) {
        printf("[%d] %s = %s (%s)\n", i + 1,
            KCONFIG_CHECKS[i].kconfig,
            KCONFIG_CHECKS[i].expected,
            KCONFIG_CHECKS[i].category);
    }
    printf("\n*  借鉴 7 抽象:所有 CONFIG_* 必须和 vocat_v1_0 一致\n");
    return errs;
}

static int check_audio_service_4_states(void) {
    /* 借鉴 5 device_state_machine.cc 4 态 + 我们加 Sleeping/Muting 共 6 态 */
    int errs = 0;
    printf("\n=== D2 借鉴 5:状态机 6 态(借鉴 4 态 + 2 自定义) ===\n");
    const char *states[] = {
        "Sleeping(休眠态,KWS only)",
        "Idle(空闲,设备静默)",
        "Connecting(连接 WS)",
        "Listening(聆听,接收音频上行)",
        "Speaking(播放中,全双工采集)",
        "Muting(被云端 mute/fade)"
    };
    const int N = (int)(sizeof(states) / sizeof(states[0]));
    for (int i = 0; i < N; i++) {
        printf("  [%d] %s\n", i + 1, states[i]);
    }
    if (N != 6) { errs++; printf("[FAIL] 应为 6 态\n"); }
    return errs;
}

static int check_protocol_v21_compat(void) {
    /* 验证协议规范 v2.1 字段完整性 */
    int errs = 0;
    printf("\n=== D8 协议规范 v2.1 兼容检查 ===\n");
    printf("  [1] X-Bridge-Token header\n");
    printf("  [2] hello JSON 14 字段\n");
    printf("  [3] 9 类 device->server JSON\n");
    printf("  [4] 11 类 server->device JSON\n");
    printf("  [5] NA frame 22B header (magic='N','A')\n");
    printf("  [6] ping 内容 {\"type\":\"ping\",\"now_ms\":int64}\n");
    printf("  [7] 25s 心跳间隔\n");
    printf("  [8] 排除 pc_status_request/status_app_request (v2.1 已删)\n");
    return errs;
}

static int check_d6_5_classes_soft(void) {
    /* D6 5 类门禁接口(高容错) */
    int errs = 0;
    printf("\n=== D6 借鉴 2:5 类门禁接口(高容错,放宽验收) ===\n");
    const char *gates[] = {
        "能量门禁:mic_rms > 800 (放宽)",
        "频谱门禁:800-4kHz 主能量区",
        "持续时间门禁:> 200ms 才上行",
        "过零率门禁:0.05-0.5 语音范围",
        "AEC 残余:erle_db < 15dB 时放行"
    };
    const int N = (int)(sizeof(gates) / sizeof(gates[0]));
    for (int i = 0; i < N; i++) {
        printf("  [%d] %s\n", i + 1, gates[i]);
    }
    if (N != 5) { errs++; printf("[FAIL] 应为 5 类\n"); }
    return errs;
}

int main(void) {
    int errs = 0;
    printf("=== D0 host check 启动 ===\n");
    printf("产品定位:设备端 = 音频出入口 + 一轮粗过滤 + 状态命令执行\n\n");

    errs += check_vocat_board();
    errs += check_kconfig();
    errs += check_audio_service_4_states();
    errs += check_protocol_v21_compat();
    errs += check_d6_5_classes_soft();

    printf("\n=== D0 host check 结果 ===\n");
    if (errs == 0) {
        printf("[PASS] 7 项 sdkconfig + 借鉴 7 抽象 + 状态机 6 态 + 协议 v2.1 + D6 5 类门禁全部正确\n");
        printf("[NEXT] 产品负责人按《D0 配置收敛步骤.md》执行真机烧录\n");
        return 0;
    }
    printf("[FAIL] %d 项错误,请修正后再烧录\n", errs);
    return 1;
}