#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#elif defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

/* Test UI and effects share these gates. Never gate an effect on visibility. */
#ifndef CONFIG_NUWAX_TEST_PANEL_ENABLE
#define CONFIG_NUWAX_TEST_PANEL_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_TEST_NETWORK_ENABLE
#define CONFIG_NUWAX_TEST_NETWORK_ENABLE 0
#endif
#define NUWAX_TEST_PANEL_ENABLED CONFIG_NUWAX_TEST_PANEL_ENABLE
#define NUWAX_TEST_NETWORK_ENABLED \
    (NUWAX_TEST_PANEL_ENABLED && CONFIG_NUWAX_TEST_NETWORK_ENABLE)
