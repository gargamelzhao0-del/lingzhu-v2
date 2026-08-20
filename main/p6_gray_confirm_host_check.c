/* P6-GRAYCONFIRM-003-1 host syntax check (不在最终构建中链接) */
#include <stdio.h>
#include <string.h>

/* 屏蔽 IDF 依赖,只测纯状态机 */
#define _ESP_ERR_H_ 1
typedef int esp_err_t;
#define ESP_OK 0

#include "p6_gray_confirm.h"

static int run_case(const char *label, nuwax_p6_gray_confirm_input_t in, int expected_kind) {
    nuwax_p6_gray_confirm_result_t r = nuwax_p6_gray_confirm_evaluate(&in);
    int got = (int)r.kind;
    int ok = (got == expected_kind);
    printf("[%s] %s kind=%s reason=%s (got=%d expected=%d)\n",
           ok ? "PASS" : "FAIL",
           label,
           nuwax_p6_gray_confirm_kind_name(r.kind),
           r.reason ? r.reason : "(null)",
           got, expected_kind);
    return ok ? 0 : 1;
}

int main(void) {
    int fail = 0;
    nuwax_p6_gray_confirm_input_t in;

    /* Case 1: NULL input (must call evaluate with NULL literally) */
    {
        nuwax_p6_gray_confirm_result_t r = nuwax_p6_gray_confirm_evaluate(NULL);
        int ok = ((int)r.kind == NUWAX_P6_GRAY_CONFIRM_NONE) &&
                 (r.reason != NULL) && (strcmp(r.reason, "null_input") == 0);
        printf("[%s] null_input kind=%s reason=%s (got kind=%d reason=%s)\n",
               ok ? "PASS" : "FAIL",
               nuwax_p6_gray_confirm_kind_name(r.kind),
               r.reason ? r.reason : "(null)",
               (int)r.kind,
               r.reason ? r.reason : "(null)");
        if (!ok) fail++;
    }

    /* Case 2: no record candidate (valid struct, all false) */
    {
        nuwax_p6_gray_confirm_input_t in = (nuwax_p6_gray_confirm_input_t){0};
        fail += run_case("no_record_candidate", in, NUWAX_P6_GRAY_CONFIRM_NONE);
    }

    /* Case 3: record + data unhealthy */
    in = (nuwax_p6_gray_confirm_input_t){
        .p6_record_candidate = true, .data_ok = false,
        .resource_ok = true, .read_timeouts_ok = true,
    };
    fail += run_case("data_unhealthy", in, NUWAX_P6_GRAY_CONFIRM_NONE);

    /* Case 4: record + data ok + resource unhealthy */
    in = (nuwax_p6_gray_confirm_input_t){
        .p6_record_candidate = true, .data_ok = true,
        .resource_ok = false, .read_timeouts_ok = true,
    };
    fail += run_case("resource_unhealthy", in, NUWAX_P6_GRAY_CONFIRM_NONE);

    /* Case 5: record + data ok + resource ok + read timeouts bad */
    in = (nuwax_p6_gray_confirm_input_t){
        .p6_record_candidate = true, .data_ok = true,
        .resource_ok = true, .read_timeouts_ok = false,
    };
    fail += run_case("read_timeouts_exceeded", in, NUWAX_P6_GRAY_CONFIRM_NONE);

    /* Case 6: record + healthy + awaiting */
    in = (nuwax_p6_gray_confirm_input_t){
        .p6_record_candidate = true, .data_ok = true,
        .resource_ok = true, .read_timeouts_ok = true,
        .confirmation_received = false, .confirmation_success = false,
        .timeout = false,
    };
    fail += run_case("awaiting_confirm", in, NUWAX_P6_GRAY_CONFIRM_GRAY_CANDIDATE);

    /* Case 7: confirm success */
    in = (nuwax_p6_gray_confirm_input_t){
        .p6_record_candidate = true, .data_ok = true,
        .resource_ok = true, .read_timeouts_ok = true,
        .confirmation_received = true, .confirmation_success = true,
    };
    fail += run_case("cloud_confirm_success", in, NUWAX_P6_GRAY_CONFIRM_CONFIRMED_CANCEL);

    /* Case 8: confirm failure */
    in = (nuwax_p6_gray_confirm_input_t){
        .p6_record_candidate = true, .data_ok = true,
        .resource_ok = true, .read_timeouts_ok = true,
        .confirmation_received = true, .confirmation_success = false,
        .timeout = false,
    };
    fail += run_case("cloud_confirm_failed", in, NUWAX_P6_GRAY_CONFIRM_REJECTED_GRAY);

    /* Case 9: timeout (no confirmation) */
    in = (nuwax_p6_gray_confirm_input_t){
        .p6_record_candidate = true, .data_ok = true,
        .resource_ok = true, .read_timeouts_ok = true,
        .confirmation_received = false, .confirmation_success = false,
        .timeout = true,
    };
    fail += run_case("confirmation_timeout", in, NUWAX_P6_GRAY_CONFIRM_REJECTED_GRAY);

    /* Case 10: confirm success priority over timeout (Codex 任务规则) */
    in = (nuwax_p6_gray_confirm_input_t){
        .p6_record_candidate = true, .data_ok = true,
        .resource_ok = true, .read_timeouts_ok = true,
        .confirmation_received = true, .confirmation_success = true,
        .timeout = true, /* 矛盾输入 */
    };
    fail += run_case("confirm_priority_over_timeout", in, NUWAX_P6_GRAY_CONFIRM_CONFIRMED_CANCEL);

    if (fail) {
        printf("\nFAIL: %d case(s) failed\n", fail);
        return 1;
    }
    printf("\nALL PASS (10/10)\n");
    return 0;
}