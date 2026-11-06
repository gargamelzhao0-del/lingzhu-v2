#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

/* Lexical resource admission, not a replacement JSON grammar/parser. Count
 * keys as tokens too: this deliberately overestimates cJSON node allocation.
 * No global hooks, recursion, heap allocation or mutable shared state. */
#define NUWAX_JSON_MAX_WIRE_BYTES 16384U
#define NUWAX_JSON_MAX_DEPTH 32U
#define NUWAX_JSON_MAX_TOKENS 512U
#define NUWAX_JSON_MAX_CONTAINER_ITEMS 256U
#define NUWAX_JSON_MAX_STRING_BYTES 16384U
#define NUWAX_JSON_MAX_PRIMITIVE_BYTES 128U
#define NUWAX_JSON_MAX_ESTIMATED_BYTES 32768U

static inline bool nuwax_json_resource_admit(const char *data, size_t length)
{
    if (!data || !length || length > NUWAX_JSON_MAX_WIRE_BYTES + 1U) return false;
    /* Legacy callers may include their terminating NUL in the buffer length. */
    if (data[length - 1U] == '\0') --length;
    if (!length || length > NUWAX_JSON_MAX_WIRE_BYTES) return false;
    uint16_t commas[NUWAX_JSON_MAX_DEPTH] = {0};
    char containers[NUWAX_JSON_MAX_DEPTH];
    size_t depth = 0U, tokens = 0U, strings = 0U, primitive_max = 0U, i = 0U;
    if (length >= 3U && (unsigned char)data[0] == 0xefU &&
        (unsigned char)data[1] == 0xbbU && (unsigned char)data[2] == 0xbfU) i = 3U;
    while (i < length) {
        const unsigned char ch = (unsigned char)data[i];
        if (ch == 0U) return false;
        if (ch <= 0x20U) { ++i; continue; }
        if (ch == '{' || ch == '[') {
            if (depth == NUWAX_JSON_MAX_DEPTH) return false;
            containers[depth] = (char)ch;
            commas[depth++] = 0U;
            ++tokens;
            ++i;
        } else if (ch == '}' || ch == ']') {
            if (!depth || containers[depth - 1U] != (ch == '}' ? '{' : '[')) return false;
            --depth;
            ++i;
        } else if (ch == ',') {
            if (!depth || ++commas[depth - 1U] >= NUWAX_JSON_MAX_CONTAINER_ITEMS) return false;
            ++i;
        } else if (ch == ':') {
            ++i;
        } else if (ch == '"') {
            const size_t start = ++i;
            while (i < length && data[i] != '"') {
                if ((unsigned char)data[i] < 0x20U) return false;
                if (data[i] == '\\') {
                    if (++i >= length || (unsigned char)data[i] < 0x20U) return false;
                    /* cJSON exposes decoded strings as NUL-terminated text.
                     * Reject embedded U+0000 before decoding so neither keys
                     * nor identities can silently alias a shorter string.
                     * Escaped backslashes ("\\\\u0000") remain ordinary text. */
                    if (data[i] == 'u' && length - i > 4U &&
                        data[i + 1U] == '0' && data[i + 2U] == '0' &&
                        data[i + 3U] == '0' && data[i + 4U] == '0') return false;
                }
                ++i;
            }
            if (i == length) return false;
            /* Current cJSON allocates raw length + 2, less escape savings. */
            strings += i - start + 2U;
            ++tokens;
            ++i;
        } else {
            /* A primitive stops at any structural boundary. Malformed
             * numbers/literals remain the existing parser's responsibility. */
            ++tokens;
            const size_t start = i;
            do {
                ++i;
                if (i == length) break;
                const unsigned char next = (unsigned char)data[i];
                if (next <= 0x20U || next == '{' || next == '[' || next == '}' ||
                    next == ']' || next == ',' || next == ':' || next == '"') break;
            } while (true);
            const size_t primitive_bytes = i - start;
            if (primitive_bytes > NUWAX_JSON_MAX_PRIMITIVE_BYTES) return false;
            if (primitive_bytes > primitive_max) primitive_max = primitive_bytes;
        }
        if (tokens > NUWAX_JSON_MAX_TOKENS || strings > NUWAX_JSON_MAX_STRING_BYTES ||
            /* One spare failed node plus cJSON's temporary numeric buffer. */
            (tokens + 1U) * sizeof(cJSON) + strings + primitive_max + 1U >
                NUWAX_JSON_MAX_ESTIMATED_BYTES) return false;
    }
    return depth == 0U && tokens != 0U;
}

static inline cJSON *nuwax_json_parse_with_length_opts(
    const char *data, size_t length, const char **end, cJSON_bool require_null)
{
    if (!nuwax_json_resource_admit(data, length)) {
        if (end) *end = data;
        return NULL;
    }
    return cJSON_ParseWithLengthOpts(data, length, end, require_null);
}

static inline cJSON *nuwax_json_parse_with_length(const char *data, size_t length)
{
    return nuwax_json_parse_with_length_opts(data, length, NULL, false);
}
