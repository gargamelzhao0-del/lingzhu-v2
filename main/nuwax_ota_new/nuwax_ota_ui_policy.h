#pragma once

#include <stdbool.h>
#include <string.h>

static inline bool nuwax_ota_ui_progress_active(const char *phase)
{
    return phase != NULL &&
           (strcmp(phase, "downloading") == 0 ||
            strcmp(phase, "writing") == 0 ||
            strcmp(phase, "verifying") == 0 ||
            /* The staged image still owns the device until reboot. Avoid
             * revealing the normal screen/resuming its decoder in this gap. */
            strcmp(phase, "done") == 0 ||
            strcmp(phase, "rebooting") == 0);
}
