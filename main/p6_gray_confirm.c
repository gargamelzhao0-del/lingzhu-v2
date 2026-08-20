#include "p6_gray_confirm.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * P6-GRAYCONFIRM-003-1: 灰度确认状态机(纯函数实现)
 *
 * 决策表(Codex 任务 P6-GRAYCONFIRM-003-1):
 * | p6_record_candidate | data_ok | resource_ok | read_timeouts_ok | confirmation_received | confirmation_success | timeout | kind                |
 * | ---                  | ---     | ---         | ---              | ---                    | ---                   | ---     | ---                 |
 * | 0                    | *       | *           | *                | *                      | *                     | *       | NONE                |
 * | 1                    | 0       | *           | *                | *                      | *                     | *       | NONE (data unhealthy) |
 * | 1                    | 1       | 0           | *                | *                      | *                     | *       | NONE (resource)     |
 * | 1                    | 1       | 1           | 0                | *                      | *                     | *       | NONE (read timeouts) |
 * | 1                    | 1       | 1           | 1                | 1                      | 1                     | *       | CONFIRMED_CANCEL    |
 * | 1                    | 1       | 1           | 1                | 1                      | 0                     | 0       | REJECTED_GRAY       |
 * | 1                    | 1       | 1           | 1                | 0                      | 0                     | 1       | REJECTED_GRAY       |
 * | 1                    | 1       | 1           | 1                | 0                      | 0                     | 0       | GRAY_CANDIDATE      |
 *
 * 冲突优先级:confirmation_success=true 优先于 confirmation_received=false/timeout=true。
 */

const char *nuwax_p6_gray_confirm_kind_name(nuwax_p6_gray_confirm_kind_t kind) {
    switch (kind) {
        case NUWAX_P6_GRAY_CONFIRM_NONE:             return "none";
        case NUWAX_P6_GRAY_CONFIRM_GRAY_CANDIDATE:   return "gray_candidate";
        case NUWAX_P6_GRAY_CONFIRM_CONFIRMED_CANCEL: return "confirmed_cancel";
        case NUWAX_P6_GRAY_CONFIRM_REJECTED_GRAY:    return "rejected_gray";
        default:                                     return "unknown";
    }
}

nuwax_p6_gray_confirm_result_t
nuwax_p6_gray_confirm_evaluate(const nuwax_p6_gray_confirm_input_t *input) {
    nuwax_p6_gray_confirm_result_t out = {0};
    out.kind = NUWAX_P6_GRAY_CONFIRM_NONE;
    out.reason = "no_record_candidate";

    if (input == NULL) {
        out.reason = "null_input";
        return out;
    }

    /* 短路 1:无候选 */
    if (!input->p6_record_candidate) {
        out.reason = "no_record_candidate";
        return out;
    }

    /* 短路 2:数据不健康(对齐 p3_nearend_candidate 现有 data_ok 字段语义) */
    if (!input->data_ok) {
        out.kind = NUWAX_P6_GRAY_CONFIRM_NONE;
        out.reason = "data_unhealthy";
        return out;
    }

    /* 短路 3:资源水位不足 */
    if (!input->resource_ok) {
        out.kind = NUWAX_P6_GRAY_CONFIRM_NONE;
        out.reason = "resource_unhealthy";
        return out;
    }

    /* 短路 4:连续 mic 读超时 */
    if (!input->read_timeouts_ok) {
        out.kind = NUWAX_P6_GRAY_CONFIRM_NONE;
        out.reason = "read_timeouts_exceeded";
        return out;
    }

    /* 已收到云端确认且成功 -> 最高优先级 */
    if (input->confirmation_received && input->confirmation_success) {
        out.kind = NUWAX_P6_GRAY_CONFIRM_CONFIRMED_CANCEL;
        out.confirmed_cancel = true;
        out.reason = "cloud_confirm_success";
        return out;
    }

    /* 已收到云端确认但失败 -> 拒绝 */
    if (input->confirmation_received && !input->confirmation_success) {
        out.kind = NUWAX_P6_GRAY_CONFIRM_REJECTED_GRAY;
        out.rejected_gray = true;
        out.reason = "cloud_confirm_failed";
        return out;
    }

    /* 超时且未确认 -> 拒绝 */
    if (input->timeout) {
        out.kind = NUWAX_P6_GRAY_CONFIRM_REJECTED_GRAY;
        out.rejected_gray = true;
        out.reason = "confirmation_timeout";
        return out;
    }

    /* 默认:候选成立,等待确认 */
    out.kind = NUWAX_P6_GRAY_CONFIRM_GRAY_CANDIDATE;
    out.gray_candidate = true;
    out.reason = "awaiting_cloud_confirm";
    return out;
}
