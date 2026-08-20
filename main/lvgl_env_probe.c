#include "lvgl_env_probe.h"

#include <stdio.h>

#include "lvgl.h"

bool nuwax_lvgl_env_is_available(void) {
    if (lv_version_major() > 9) {
        return true;
    }
    return lv_version_major() == 9 && lv_version_minor() >= 5;
}

const char *nuwax_lvgl_env_version(void) {
    static char version[24];
    snprintf(version,
             sizeof(version),
             "%d.%d.%d",
             (int)lv_version_major(),
             (int)lv_version_minor(),
             (int)lv_version_patch());
    return version;
}
