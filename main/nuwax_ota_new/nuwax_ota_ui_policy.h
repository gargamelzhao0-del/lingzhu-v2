#pragma once

#include <stdbool.h>
#include <string.h>

static inline bool nuwax_ota_ui_progress_active(const char *phase)
{
    return phase != NULL &&
           (strcmp(phase, "downloading") == 0 ||
            strcmp(phase, "writing") == 0 ||
            strcmp(phase, "verifying") == 0);
}
