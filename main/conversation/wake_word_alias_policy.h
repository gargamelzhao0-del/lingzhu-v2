#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Explicit MultiNet phrases keep all accepted pronunciations bounded. */
#define NUWAX_WAKE_XIAOZHU_ALIAS_COUNT 4U
#define NUWAX_WAKE_LEGACY_LINGZHU_ALIAS_COUNT 4U
#define NUWAX_WAKE_LINGZHU_ALIAS_COUNT 6U
#define NUWAX_WAKE_FACTORY_ALIAS_COUNT 10U

static inline const char *nuwax_wake_factory_alias_at(size_t index)
{
    static const char *const aliases[NUWAX_WAKE_FACTORY_ALIAS_COUNT] = {
        "xiao zhu xiao zhu",
        "xiao zu xiao zu",
        "xiao zhu xiao zu",
        "xiao zu xiao zhu",
        "ni hao ling zhu",
        "ni hao lin zhu",
        "ni hao ling zu",
        "ni hao lin zu",
        "ni hao xiao zu",
        "ni hao xiao zhu",
    };

    return index < NUWAX_WAKE_FACTORY_ALIAS_COUNT ? aliases[index] : NULL;
}

static inline bool nuwax_wake_command_is_xiaozhu_family(const char *command)
{
    if (!command || !command[0]) {
        return false;
    }
    for (size_t i = 0; i < NUWAX_WAKE_XIAOZHU_ALIAS_COUNT; ++i) {
        if (strcmp(command, nuwax_wake_factory_alias_at(i)) == 0) {
            return true;
        }
    }
    return false;
}

/* Retained for protocol compatibility and factory-default migration. */
static inline bool nuwax_wake_command_is_lingzhu_family(const char *command)
{
    if (!command || !command[0]) {
        return false;
    }
    for (size_t i = 0; i < NUWAX_WAKE_LINGZHU_ALIAS_COUNT; ++i) {
        if (strcmp(command,
                   nuwax_wake_factory_alias_at(
                       NUWAX_WAKE_XIAOZHU_ALIAS_COUNT + i)) == 0) {
            return true;
        }
    }
    return false;
}

static inline bool nuwax_wake_command_is_legacy_lingzhu_family(
    const char *command)
{
    if (!command || !command[0]) {
        return false;
    }
    for (size_t i = 0; i < NUWAX_WAKE_LEGACY_LINGZHU_ALIAS_COUNT; ++i) {
        if (strcmp(command,
                   nuwax_wake_factory_alias_at(
                       NUWAX_WAKE_XIAOZHU_ALIAS_COUNT + i)) == 0) {
            return true;
        }
    }
    return false;
}

static inline bool nuwax_wake_command_is_factory_family(const char *command)
{
    return nuwax_wake_command_is_xiaozhu_family(command) ||
           nuwax_wake_command_is_lingzhu_family(command);
}

static inline size_t nuwax_wake_command_variant_count(const char *command)
{
    if (!command || !command[0]) {
        return 0U;
    }
    return nuwax_wake_command_is_factory_family(command)
               ? NUWAX_WAKE_FACTORY_ALIAS_COUNT
               : 1U;
}

static inline const char *nuwax_wake_command_variant_at(const char *command,
                                                        size_t index)
{
    if (!command || !command[0]) {
        return NULL;
    }
    if (nuwax_wake_command_is_factory_family(command)) {
        return nuwax_wake_factory_alias_at(index);
    }
    return index == 0U ? command : NULL;
}
