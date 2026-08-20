#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "nuwax_ui_v28/ui_bridge_revision_policy.h"

static int s_failures;

static void expect_true(const char *name, bool value)
{
    if (!value) {
        fprintf(stderr, "[FAIL] %s\n", name);
        s_failures++;
    } else {
        printf("[PASS] %s\n", name);
    }
}

int main(void)
{
    expect_true("first bridge transaction is accepted",
                nuwax_ui_bridge_revision_is_newer(1U, 0U));
    expect_true("zero bridge transaction is rejected",
                !nuwax_ui_bridge_revision_is_newer(0U, 0U));
    expect_true("newer bridge transaction is accepted",
                nuwax_ui_bridge_revision_is_newer(12U, 11U));
    expect_true("duplicate bridge transaction is rejected",
                !nuwax_ui_bridge_revision_is_newer(12U, 12U));
    expect_true("late bridge transaction is rejected",
                !nuwax_ui_bridge_revision_is_newer(11U, 12U));
    expect_true("wrapped bridge transaction is accepted",
                nuwax_ui_bridge_revision_is_newer(1U, UINT32_MAX));

    expect_true("owned revision equal to fence is rejected",
                nuwax_ui_bridge_owned_revision_is_rejected(20U, 20U));
    expect_true("owned revision older than fence is rejected",
                nuwax_ui_bridge_owned_revision_is_rejected(19U, 20U));
    expect_true("owned revision newer than fence is accepted",
                !nuwax_ui_bridge_owned_revision_is_rejected(21U, 20U));
    expect_true("wrapped owned revision newer than fence is accepted",
                !nuwax_ui_bridge_owned_revision_is_rejected(
                    1U, UINT32_MAX));
    expect_true("unowned semantic update bypasses queue fence",
                !nuwax_ui_bridge_owned_revision_is_rejected(0U, 20U));

    uint32_t field_revisions[4] = {0U};
    expect_true("first multi-field transaction is accepted",
                nuwax_ui_bridge_filter_newer_fields(
                    10U, 0x7U, field_revisions, 4U) == 0x7U);
    expect_true("new narrow transaction advances one field",
                nuwax_ui_bridge_filter_newer_fields(
                    12U, 0x4U, field_revisions, 4U) == 0x4U);
    expect_true("late transaction retains unrelated fields only",
                nuwax_ui_bridge_filter_newer_fields(
                    11U, 0xEU, field_revisions, 4U) == 0xAU);
    expect_true("late transaction cannot overwrite newer overlap",
                field_revisions[2] == 12U);

    printf("UI bridge revision policy host check: %s (%d failures)\n",
           s_failures ? "FAIL" : "PASS", s_failures);
    return s_failures ? 1 : 0;
}
