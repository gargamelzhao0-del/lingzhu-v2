#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_WAKE_WORD_MAX_VARIANTS 10U
#define NUWAX_WAKE_WORD_MAX_COMMAND_LEN 63U

typedef struct {
    uint8_t count;
    char variants[NUWAX_WAKE_WORD_MAX_VARIANTS]
                 [NUWAX_WAKE_WORD_MAX_COMMAND_LEN + 1U];
} nuwax_wake_word_config_t;

/* Array commands are normalized exactly as supplied. Duplicate normalized
 * variants are removed while preserving the first occurrence and order. */
bool nuwax_wake_word_config_normalize(
    const nuwax_wake_word_config_t *input,
    nuwax_wake_word_config_t *output);

/* Known factory string commands expand to all explicit factory aliases. */
bool nuwax_wake_word_config_from_legacy(
    const char *command,
    nuwax_wake_word_config_t *output);

bool nuwax_wake_word_configs_equal(
    const nuwax_wake_word_config_t *left,
    const nuwax_wake_word_config_t *right);
