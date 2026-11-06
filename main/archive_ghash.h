/* SPDX-License-Identifier: Apache-2.0
 * GHASH over GF(2^128), the same representation as ESP-IDF esp_aes_gcm.c
 * (Mbed TLS / Espressif). An optional byte table reduces the data loop from
 * 32 nibble steps with 64-bit shifts to 16 steps using native 32-bit words.
 * The key-dependent 4 KiB table is PSRAM-owned by the scoped SD sender.
 * This does not replace AES, IV/AAD processing, final length block or tag.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint32_t archive_ghash_table_t[256][4];

static inline uint32_t archive_ghash_be32(const uint8_t *p)
{
    uint32_t v; memcpy(&v, p, sizeof(v));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}
static inline void archive_ghash_store(uint8_t *p, uint32_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    memcpy(p, &v, sizeof(v));
}

/* Generated from eight repetitions of the standard GHASH right shift and
 * 0xe1000000 reduction. These are public polynomial constants, not key data. */
static const uint32_t archive_ghash_reduce8[256] = {
    0x00000000U,0x01c20000U,0x03840000U,0x02460000U,0x07080000U,0x06ca0000U,0x048c0000U,0x054e0000U,
    0x0e100000U,0x0fd20000U,0x0d940000U,0x0c560000U,0x09180000U,0x08da0000U,0x0a9c0000U,0x0b5e0000U,
    0x1c200000U,0x1de20000U,0x1fa40000U,0x1e660000U,0x1b280000U,0x1aea0000U,0x18ac0000U,0x196e0000U,
    0x12300000U,0x13f20000U,0x11b40000U,0x10760000U,0x15380000U,0x14fa0000U,0x16bc0000U,0x177e0000U,
    0x38400000U,0x39820000U,0x3bc40000U,0x3a060000U,0x3f480000U,0x3e8a0000U,0x3ccc0000U,0x3d0e0000U,
    0x36500000U,0x37920000U,0x35d40000U,0x34160000U,0x31580000U,0x309a0000U,0x32dc0000U,0x331e0000U,
    0x24600000U,0x25a20000U,0x27e40000U,0x26260000U,0x23680000U,0x22aa0000U,0x20ec0000U,0x212e0000U,
    0x2a700000U,0x2bb20000U,0x29f40000U,0x28360000U,0x2d780000U,0x2cba0000U,0x2efc0000U,0x2f3e0000U,
    0x70800000U,0x71420000U,0x73040000U,0x72c60000U,0x77880000U,0x764a0000U,0x740c0000U,0x75ce0000U,
    0x7e900000U,0x7f520000U,0x7d140000U,0x7cd60000U,0x79980000U,0x785a0000U,0x7a1c0000U,0x7bde0000U,
    0x6ca00000U,0x6d620000U,0x6f240000U,0x6ee60000U,0x6ba80000U,0x6a6a0000U,0x682c0000U,0x69ee0000U,
    0x62b00000U,0x63720000U,0x61340000U,0x60f60000U,0x65b80000U,0x647a0000U,0x663c0000U,0x67fe0000U,
    0x48c00000U,0x49020000U,0x4b440000U,0x4a860000U,0x4fc80000U,0x4e0a0000U,0x4c4c0000U,0x4d8e0000U,
    0x46d00000U,0x47120000U,0x45540000U,0x44960000U,0x41d80000U,0x401a0000U,0x425c0000U,0x439e0000U,
    0x54e00000U,0x55220000U,0x57640000U,0x56a60000U,0x53e80000U,0x522a0000U,0x506c0000U,0x51ae0000U,
    0x5af00000U,0x5b320000U,0x59740000U,0x58b60000U,0x5df80000U,0x5c3a0000U,0x5e7c0000U,0x5fbe0000U,
    0xe1000000U,0xe0c20000U,0xe2840000U,0xe3460000U,0xe6080000U,0xe7ca0000U,0xe58c0000U,0xe44e0000U,
    0xef100000U,0xeed20000U,0xec940000U,0xed560000U,0xe8180000U,0xe9da0000U,0xeb9c0000U,0xea5e0000U,
    0xfd200000U,0xfce20000U,0xfea40000U,0xff660000U,0xfa280000U,0xfbea0000U,0xf9ac0000U,0xf86e0000U,
    0xf3300000U,0xf2f20000U,0xf0b40000U,0xf1760000U,0xf4380000U,0xf5fa0000U,0xf7bc0000U,0xf67e0000U,
    0xd9400000U,0xd8820000U,0xdac40000U,0xdb060000U,0xde480000U,0xdf8a0000U,0xddcc0000U,0xdc0e0000U,
    0xd7500000U,0xd6920000U,0xd4d40000U,0xd5160000U,0xd0580000U,0xd19a0000U,0xd3dc0000U,0xd21e0000U,
    0xc5600000U,0xc4a20000U,0xc6e40000U,0xc7260000U,0xc2680000U,0xc3aa0000U,0xc1ec0000U,0xc02e0000U,
    0xcb700000U,0xcab20000U,0xc8f40000U,0xc9360000U,0xcc780000U,0xcdba0000U,0xcffc0000U,0xce3e0000U,
    0x91800000U,0x90420000U,0x92040000U,0x93c60000U,0x96880000U,0x974a0000U,0x950c0000U,0x94ce0000U,
    0x9f900000U,0x9e520000U,0x9c140000U,0x9dd60000U,0x98980000U,0x995a0000U,0x9b1c0000U,0x9ade0000U,
    0x8da00000U,0x8c620000U,0x8e240000U,0x8fe60000U,0x8aa80000U,0x8b6a0000U,0x892c0000U,0x88ee0000U,
    0x83b00000U,0x82720000U,0x80340000U,0x81f60000U,0x84b80000U,0x857a0000U,0x873c0000U,0x86fe0000U,
    0xa9c00000U,0xa8020000U,0xaa440000U,0xab860000U,0xaec80000U,0xaf0a0000U,0xad4c0000U,0xac8e0000U,
    0xa7d00000U,0xa6120000U,0xa4540000U,0xa5960000U,0xa0d80000U,0xa11a0000U,0xa35c0000U,0xa29e0000U,
    0xb5e00000U,0xb4220000U,0xb6640000U,0xb7a60000U,0xb2e80000U,0xb32a0000U,0xb16c0000U,0xb0ae0000U,
    0xbbf00000U,0xba320000U,0xb8740000U,0xb9b60000U,0xbcf80000U,0xbd3a0000U,0xbf7c0000U,0xbebe0000U,
};

static void archive_ghash_table_prepare(archive_ghash_table_t table, const uint8_t h[16])
{
    memset(table, 0, sizeof(archive_ghash_table_t));
    uint32_t v[4];
    for (unsigned j = 0; j < 4U; ++j) v[j] = archive_ghash_be32(h + 4U * j);
    for (unsigned bit = 128U; bit; bit >>= 1U) {
        memcpy(table[bit], v, sizeof(v));
        const uint32_t carry = 0U - (v[3] & 1U);
        v[3] = (v[3] >> 1U) | (v[2] << 31U);
        v[2] = (v[2] >> 1U) | (v[1] << 31U);
        v[1] = (v[1] >> 1U) | (v[0] << 31U);
        v[0] = (v[0] >> 1U) ^ (0xe1000000U & carry);
    }
    for (unsigned i = 1U; i < 256U; ++i) {
        if (!(i & (i - 1U))) continue;
        const unsigned low = i & (0U - i);
        for (unsigned j = 0; j < 4U; ++j) table[i][j] = table[low][j] ^ table[i ^ low][j];
    }
}

static inline __attribute__((always_inline)) void archive_ghash_step(
    uint32_t z[restrict 4], const uint32_t table[restrict 256][4], unsigned byte)
{
    const uint32_t reduction = archive_ghash_reduce8[z[3] & 255U];
    z[3] = (z[3] >> 8U) | (z[2] << 24U);
    z[2] = (z[2] >> 8U) | (z[1] << 24U);
    z[1] = (z[1] >> 8U) | (z[0] << 24U);
    z[0] = (z[0] >> 8U) ^ reduction;
    const uint32_t *row = table[byte];
    for (unsigned i = 0U; i < 4U; ++i) z[i] ^= row[i];
}

static void archive_ghash_block(uint32_t value[4], const uint8_t data[16],
                               const archive_ghash_table_t table)
{
    uint32_t x[4], z[4] = {0};
    for (unsigned i = 0U; i < 4U; ++i) x[i] = value[i] ^ archive_ghash_be32(data + 4U * i);
    for (int i = 3; i >= 0; --i) {
        const uint32_t w = x[i];
        archive_ghash_step(z, table, w & 255U);
        archive_ghash_step(z, table, (w >> 8U) & 255U);
        archive_ghash_step(z, table, (w >> 16U) & 255U);
        archive_ghash_step(z, table, w >> 24U);
    }
    memcpy(value, z, sizeof(z));
}

/* Nonfinal update calls must be a multiple of 16, as for the SDK's GHASH.
 * The scoped sender enforces this with its aligned DMA window. */
static void archive_ghash_update(const archive_ghash_table_t table, uint8_t hash[16],
                                 const uint8_t *data, size_t bytes)
{
    uint32_t value[4];
    for (unsigned i = 0U; i < 4U; ++i) value[i] = archive_ghash_be32(hash + 4U * i);
    while (bytes >= 16U) {
        archive_ghash_block(value, data, table);
        data += 16U; bytes -= 16U;
    }
    if (bytes) {
        uint8_t tail[16] = {0}; memcpy(tail, data, bytes);
        archive_ghash_block(value, tail, table);
    }
    for (unsigned i = 0U; i < 4U; ++i) archive_ghash_store(hash + 4U * i, value[i]);
}

static inline int archive_ghash_worthwhile(uint64_t sdk_us, uint64_t fast_us)
{
    return sdk_us > 0U && fast_us > 0U && fast_us * 100U <= sdk_us * 85U;
}
