#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Host-only link stub. The real implementation is part of the ESP-IDF app;
 * meeting controller checks only need to verify that reporting is non-fatal. */
bool nuwax_device_error_report_enqueue(const char *severity,
                                       const char *component,
                                       const char *code,
                                       esp_err_t result,
                                       const char *state,
                                       uint32_t generation,
                                       const char *turn_id)
{
    (void)severity;
    (void)component;
    (void)code;
    (void)result;
    (void)state;
    (void)generation;
    (void)turn_id;
    return true;
}
