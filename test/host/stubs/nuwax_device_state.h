#pragma once

/*
 * Minimal nuwax_device_state.h stub for host unit tests.
 *
 * The pure-logic focus-menu modules under test (data, status, wheel,
 * surface_style) include nuwax_focus_menu.h, which in turn includes this
 * header — but none of them read any device-state field. So an opaque type
 * is enough to satisfy the include without pulling in the real (hardware /
 * RTOS coupled) definition.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct nuwax_device_state nuwax_device_state_t;
