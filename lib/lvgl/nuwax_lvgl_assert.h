#ifndef NUWAX_LVGL_ASSERT_H
#define NUWAX_LVGL_ASSERT_H

#include <stdlib.h>

/* LVGL's default handler spins forever in the UI task. An assertion means
 * internal state can no longer be trusted, so use ESP-IDF's panic/reboot
 * policy instead of leaving audio and networking alive with a frozen UI. */
#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER abort();

#endif
