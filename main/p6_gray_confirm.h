#pragma once

/*
 * P6-GRAYCONFIRM-003-1: 灰度确认状态机骨架(纯函数,无副作用)
 *
 * 三段式:
 *   p6_record_candidate=1 + 未确认     -> GRAY_CANDIDATE
 *   p6_record_candidate=1 + confirm ok -> CONFIRMED_CANCEL
 *   p6_record_candidate=1 + reject/timeout -> REJECTED_GRAY
 *   p6_record_candidate=0 / data 异常 -> NONE
 *
 * 阶段 1 约束(Codex 20:20 拍板):
 * - 仅纯状态机,不在本模块中操作 speaker / WebSocket / session dispatch
 * - 不发 cancel_turn
 * - 不改旧 gray_cancel 行为
 *
 * 阶段 2(003-2+) 才接 endpoint / session / speaker。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 状态机结果类型 */
typedef enum {
    NUWAX_P6_GRAY_CONFIRM_NONE = 0,
    NUWAX_P6_GRAY_CONFIRM_GRAY_CANDIDATE,
    NUWAX_P6_GRAY_CONFIRM_CONFIRMED_CANCEL,
    NUWAX_P6_GRAY_CONFIRM_REJECTED_GRAY,
} nuwax_p6_gray_confirm_kind_t;

/* 状态机输入(纯数据,无任何硬件/全局状态依赖) */
typedef struct {
    /* 来自 P3-1/P5-1 live detector 的近端候选判定 */
    bool p6_record_candidate;
    /* 数据健康门禁(短路用,与 p3_nearend_candidate 现有 data_ok 字段对齐) */
    bool data_ok;
    /* 资源水位(heap / dma / queue 深度) */
    bool resource_ok;
    /* 实时 mic 读是否超时(>5 次连续 timeout 视为不可用) */
    bool read_timeouts_ok;
    /* 云端 confirm/reject 决策 */
    bool confirmation_received;
    bool confirmation_success;
    /* 本地等待确认是否超时 */
    bool timeout;
} nuwax_p6_gray_confirm_input_t;

/* 状态机输出(reason 指向静态字符串,生命周期 = 程序运行期) */
typedef struct {
    nuwax_p6_gray_confirm_kind_t kind;
    bool gray_candidate;
    bool confirmed_cancel;
    bool rejected_gray;
    const char *reason;
} nuwax_p6_gray_confirm_result_t;

/* 纯函数:输入 -> 输出。不持任何全局状态、不调用任何硬件 API、不分配/释放内存。 */
nuwax_p6_gray_confirm_result_t
nuwax_p6_gray_confirm_evaluate(const nuwax_p6_gray_confirm_input_t *input);

/* 调试用:enum -> 字符串 */
const char *nuwax_p6_gray_confirm_kind_name(nuwax_p6_gray_confirm_kind_t kind);

#ifdef __cplusplus
}
#endif
