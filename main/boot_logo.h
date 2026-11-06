#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NUWAX_BOOT_LOGO_WIDTH 360U
#define NUWAX_BOOT_LOGO_HEIGHT 360U

/* RGB565 RLE packets: nonzero run length, low byte, high byte. Each row is
 * independently bounded. The destination is owned by the boot display path. */
bool nuwax_boot_logo_decode(const uint8_t *data, size_t size,
                           uint16_t *pixels, size_t count);
bool nuwax_boot_logo_decode_row(size_t row, uint16_t *pixels, size_t count);
