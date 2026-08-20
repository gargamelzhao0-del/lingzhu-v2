#include <stdio.h>

#include "nuwax_ota_new/nuwax_ota_retry_policy.h"

static int expect_class(const char *name, nuwax_ota_retry_class_t actual,
                        nuwax_ota_retry_class_t expected)
{
    if (actual == expected) {
        printf("[PASS] %s=%s\n", name,
               nuwax_ota_retry_class_name(actual));
        return 0;
    }
    printf("[FAIL] %s actual=%s expected=%s\n", name,
           nuwax_ota_retry_class_name(actual),
           nuwax_ota_retry_class_name(expected));
    return 1;
}

int main(void)
{
    int failures = 0;
    failures += expect_class("success", nuwax_ota_retry_classify(
                                 true, false, "ok"), NUWAX_OTA_RETRY_NONE);
    failures += expect_class("network", nuwax_ota_retry_classify(
                                 false, false, "download_failed"),
                             NUWAX_OTA_RETRY_AUTOMATIC);
    failures += expect_class("timeout", nuwax_ota_retry_classify(
                                 false, false, "download_timeout"),
                             NUWAX_OTA_RETRY_AUTOMATIC);
    failures += expect_class("low power", nuwax_ota_retry_classify(
                                 false, false, "low_power"),
                             NUWAX_OTA_RETRY_CONDITION);
    failures += expect_class("audio busy", nuwax_ota_retry_classify(
                                 false, false, "audio_busy"),
                             NUWAX_OTA_RETRY_CONDITION);
    failures += expect_class("artifact unavailable", nuwax_ota_retry_classify(
                                 false, false, "artifact_unavailable"),
                             NUWAX_OTA_RETRY_CONDITION);
    failures += expect_class("bad sha", nuwax_ota_retry_classify(
                                 false, false, "sha256_mismatch"),
                             NUWAX_OTA_RETRY_NEVER);
    failures += expect_class("bad image", nuwax_ota_retry_classify(
                                 false, false, "image_invalid"),
                             NUWAX_OTA_RETRY_NEVER);
    failures += expect_class("rollback", nuwax_ota_retry_classify(
                                 false, true, "boot_validation_failed"),
                             NUWAX_OTA_RETRY_NEVER);
    failures += expect_class("unknown fails closed", nuwax_ota_retry_classify(
                                 false, false, "ota_failed"),
                             NUWAX_OTA_RETRY_NEVER);
    if (failures != 0) {
        printf("OTA retry policy failures=%d\n", failures);
        return 1;
    }
    printf("OTA retry policy: PASS\n");
    return 0;
}
