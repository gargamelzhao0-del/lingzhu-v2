#include <stdbool.h>
#include <stdio.h>

#include "nuwax_ota_new/nuwax_ota_ui_policy.h"

static int s_failures;

static void expect_active(const char *phase, bool expected)
{
    const bool actual = nuwax_ota_ui_progress_active(phase);
    if (actual != expected) {
        fprintf(stderr, "[FAIL] phase=%s active=%d expected=%d\n",
                phase ? phase : "-", actual ? 1 : 0, expected ? 1 : 0);
        s_failures++;
    } else {
        printf("[PASS] phase=%s active=%d\n",
               phase ? phase : "-", actual ? 1 : 0);
    }
}

int main(void)
{
    expect_active(NULL, false);
    expect_active("quiescing", false);
    expect_active("downloading", true);
    expect_active("writing", true);
    expect_active("verifying", true);
    expect_active("done", false);
    expect_active("failed", false);
    expect_active("rebooting", false);
    expect_active("idle", false);

    printf("OTA UI progress policy: %s (%d failures)\n",
           s_failures ? "FAIL" : "PASS", s_failures);
    return s_failures ? 1 : 0;
}
