#include "boot_logo.h"
#include "assets/ling_zhu_boot_logo.h"

bool nuwax_boot_logo_decode(const uint8_t *data, size_t size,
                           uint16_t *pixels, size_t count) {
    if (!data || !pixels || count == 0U || size % 3U != 0U) return false;
    size_t written = 0U;
    for (size_t offset = 0U; offset < size; offset += 3U) {
        const size_t run = data[offset];
        if (run == 0U || run > count - written) return false;
        const uint16_t color = (uint16_t)data[offset + 1U] |
                               ((uint16_t)data[offset + 2U] << 8);
        for (size_t i = 0U; i < run; ++i) pixels[written++] = color;
    }
    return written == count;
}

bool nuwax_boot_logo_decode_row(size_t row, uint16_t *pixels, size_t count) {
    if (row >= NUWAX_BOOT_LOGO_HEIGHT || count != NUWAX_BOOT_LOGO_WIDTH) {
        return false;
    }
    const size_t begin = ling_zhu_boot_logo_rows[row];
    const size_t end = ling_zhu_boot_logo_rows[row + 1U];
    if (end < begin || end > ling_zhu_boot_logo_size) return false;
    return nuwax_boot_logo_decode(ling_zhu_boot_logo_data + begin,
                                 end - begin, pixels, count);
}
