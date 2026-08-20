#pragma once

#include <stdbool.h>
#include <string.h>

typedef enum {
    NUWAX_OTA_RETRY_NONE = 0,
    NUWAX_OTA_RETRY_AUTOMATIC,
    NUWAX_OTA_RETRY_CONDITION,
    NUWAX_OTA_RETRY_NEVER,
} nuwax_ota_retry_class_t;

static inline nuwax_ota_retry_class_t nuwax_ota_retry_classify(
    bool ok, bool rolled_back, const char *code)
{
    if (ok) {
        return NUWAX_OTA_RETRY_NONE;
    }
    if (rolled_back || code == NULL || code[0] == '\0') {
        return NUWAX_OTA_RETRY_NEVER;
    }
    if (strcmp(code, "download_failed") == 0 ||
        strcmp(code, "download_timeout") == 0 ||
        strcmp(code, "interrupted") == 0) {
        return NUWAX_OTA_RETRY_AUTOMATIC;
    }
    if (strcmp(code, "audio_busy") == 0 ||
        strcmp(code, "low_power") == 0 ||
        strcmp(code, "power_unknown") == 0 ||
        strcmp(code, "artifact_unavailable") == 0 ||
        strcmp(code, "ota_busy") == 0 ||
        strcmp(code, "cancelled") == 0) {
        return NUWAX_OTA_RETRY_CONDITION;
    }
    /* Unknown failures fail closed. The cloud must not automatically loop an
     * unclassified device, image, storage, or boot-validation defect. */
    return NUWAX_OTA_RETRY_NEVER;
}

static inline const char *nuwax_ota_retry_class_name(
    nuwax_ota_retry_class_t retry_class)
{
    switch (retry_class) {
    case NUWAX_OTA_RETRY_AUTOMATIC: return "automatic";
    case NUWAX_OTA_RETRY_CONDITION: return "condition";
    case NUWAX_OTA_RETRY_NEVER:     return "never";
    case NUWAX_OTA_RETRY_NONE:
    default:                        return "none";
    }
}

static inline bool nuwax_ota_retryable(nuwax_ota_retry_class_t retry_class)
{
    return retry_class == NUWAX_OTA_RETRY_AUTOMATIC ||
           retry_class == NUWAX_OTA_RETRY_CONDITION;
}
