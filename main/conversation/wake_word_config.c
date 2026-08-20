#include "wake_word_config.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "wake_word_alias_policy.h"

static bool normalize_command(
    const char *input,
    char output[NUWAX_WAKE_WORD_MAX_COMMAND_LEN + 1U])
{
    if (!input || !output) {
        return false;
    }
    size_t out_len = 0U;
    bool previous_space = true;
    for (const unsigned char *p = (const unsigned char *)input; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            if (!previous_space && out_len < NUWAX_WAKE_WORD_MAX_COMMAND_LEN) {
                output[out_len++] = ' ';
                previous_space = true;
            }
            continue;
        }
        if (*p >= 0x80U || !isalnum(*p) ||
            out_len >= NUWAX_WAKE_WORD_MAX_COMMAND_LEN) {
            return false;
        }
        output[out_len++] = (char)tolower(*p);
        previous_space = false;
    }
    if (previous_space && out_len > 0U) {
        out_len--;
    }
    if (out_len < 2U) {
        return false;
    }
    output[out_len] = '\0';
    return true;
}

bool nuwax_wake_word_config_normalize(
    const nuwax_wake_word_config_t *input,
    nuwax_wake_word_config_t *output)
{
    if (!input || !output || input->count == 0U ||
        input->count > NUWAX_WAKE_WORD_MAX_VARIANTS) {
        return false;
    }

    nuwax_wake_word_config_t normalized = {0};
    for (uint8_t i = 0U; i < input->count; ++i) {
        if (!memchr(input->variants[i], '\0',
                    NUWAX_WAKE_WORD_MAX_COMMAND_LEN + 1U)) {
            return false;
        }
        char variant[NUWAX_WAKE_WORD_MAX_COMMAND_LEN + 1U] = {0};
        if (!normalize_command(input->variants[i], variant)) {
            return false;
        }
        bool duplicate = false;
        for (uint8_t j = 0U; j < normalized.count; ++j) {
            if (strcmp(normalized.variants[j], variant) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        memcpy(normalized.variants[normalized.count],
               variant,
               strlen(variant) + 1U);
        normalized.count++;
    }
    if (normalized.count == 0U) {
        return false;
    }
    *output = normalized;
    return true;
}

bool nuwax_wake_word_config_from_legacy(
    const char *command,
    nuwax_wake_word_config_t *output)
{
    if (!output) {
        return false;
    }
    char normalized[NUWAX_WAKE_WORD_MAX_COMMAND_LEN + 1U] = {0};
    if (!normalize_command(command, normalized)) {
        return false;
    }

    nuwax_wake_word_config_t config = {0};
    const size_t variant_count = nuwax_wake_command_variant_count(normalized);
    if (variant_count == 0U ||
        variant_count > NUWAX_WAKE_WORD_MAX_VARIANTS) {
        return false;
    }
    config.count = (uint8_t)variant_count;
    for (size_t i = 0U; i < variant_count; ++i) {
        const char *variant = nuwax_wake_command_variant_at(normalized, i);
        if (!variant) {
            return false;
        }
        memcpy(config.variants[i], variant, strlen(variant) + 1U);
    }
    *output = config;
    return true;
}

bool nuwax_wake_word_configs_equal(
    const nuwax_wake_word_config_t *left,
    const nuwax_wake_word_config_t *right)
{
    if (!left || !right || left->count != right->count) {
        return false;
    }
    for (uint8_t i = 0U; i < left->count; ++i) {
        if (strcmp(left->variants[i], right->variants[i]) != 0) {
            return false;
        }
    }
    return true;
}
