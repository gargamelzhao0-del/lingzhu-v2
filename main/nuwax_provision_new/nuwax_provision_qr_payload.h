#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NUWAX_PROVISION_QR_BASE_URL "https://nuwax.com/lz.html"

static inline bool nuwax_provision_qr_append(
    char *output,
    size_t output_size,
    size_t *used,
    const char *text) {
    if (!output || output_size == 0U || !used || !text) {
        return false;
    }
    const size_t length = strlen(text);
    if (*used >= output_size || length >= output_size - *used) {
        return false;
    }
    memcpy(output + *used, text, length);
    *used += length;
    output[*used] = '\0';
    return true;
}

static inline bool nuwax_provision_qr_append_component(
    char *output,
    size_t output_size,
    size_t *used,
    const char *value) {
    static const char hex[] = "0123456789ABCDEF";
    if (!output || output_size == 0U || !used || !value) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor;
         ++cursor) {
        const unsigned char byte = *cursor;
        const bool unreserved =
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
            byte == '_' || byte == '~';
        const size_t encoded_length = unreserved ? 1U : 3U;
        if (*used >= output_size || encoded_length >= output_size - *used) {
            return false;
        }
        if (unreserved) {
            output[(*used)++] = (char)byte;
        } else {
            output[(*used)++] = '%';
            output[(*used)++] = hex[byte >> 4U];
            output[(*used)++] = hex[byte & 0x0FU];
        }
        output[*used] = '\0';
    }
    return true;
}

static inline bool nuwax_provision_qr_payload_build(
    char *output,
    size_t output_size,
    const char *service_name,
    const char *username,
    const char *pop) {
    if (!output || output_size == 0U) {
        return false;
    }
    output[0] = '\0';
    if (!service_name || !service_name[0] || !username || !username[0]) {
        return false;
    }

    size_t used = 0U;
    bool ok =
        nuwax_provision_qr_append(output, output_size, &used,
                                  NUWAX_PROVISION_QR_BASE_URL "?n=") &&
        nuwax_provision_qr_append_component(output, output_size, &used,
                                             service_name) &&
        nuwax_provision_qr_append(output, output_size, &used, "&u=") &&
        nuwax_provision_qr_append_component(output, output_size, &used,
                                             username);
    if (ok && pop && pop[0]) {
        ok = nuwax_provision_qr_append(output, output_size, &used, "&p=") &&
             nuwax_provision_qr_append_component(output, output_size, &used,
                                                  pop);
    }
    if (!ok) {
        output[0] = '\0';
    }
    return ok;
}
