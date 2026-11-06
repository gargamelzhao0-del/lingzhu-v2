#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define SD_DISCOVERY_QUERY_BYTES 72U
#define SD_DISCOVERY_REPLY_BYTES 76U
static inline bool sd_discovery_hex(const char *text, uint8_t *out, size_t count) {
    if (!text || strlen(text) != count * 2U) return false;
    for (size_t i = 0; i < count * 2U; ++i) {
        unsigned n = text[i] >= '0' && text[i] <= '9' ? (unsigned)(text[i] - '0') :
                     text[i] >= 'a' && text[i] <= 'f' ? (unsigned)(text[i] - 'a' + 10) : 256U;
        if (n > 15U) return false;
        if (!(i & 1U)) out[i / 2U] = (uint8_t)(n << 4U);
        else out[i / 2U] |= (uint8_t)n;
    }
    return true;
}
static inline void sd_discovery_query(uint8_t out[72], const uint8_t nonce[16], const uint8_t id[16]) {
    memset(out, 0, 72); memcpy(out, "NWXSDQ1", 8); memcpy(out + 8, nonce, 16); memcpy(out + 24, id, 16);
}
/* Layout/nonce checks precede cryptographic verification. Never trust a host
 * address from the payload: the UDP source supplies the connected endpoint. */
static inline uint16_t sd_discovery_reply_port(const uint8_t *reply, size_t length, const uint8_t query[72]) {
    if (length != SD_DISCOVERY_REPLY_BYTES || memcmp(reply, "NWXSDR1", 8) ||
        memcmp(reply + 8, query + 8, 32) || reply[42] || reply[43]) return 0;
    return (uint16_t)(((unsigned)reply[40] << 8U) | reply[41]);
}
