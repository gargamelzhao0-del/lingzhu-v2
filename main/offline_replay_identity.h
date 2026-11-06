#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool replay_path_quarantined(const char *path)
{
    const size_t n = strlen(path);
    return n >= 10U && strcmp(path + n - 10U, ".ready.bad") == 0;
}

/* Quarantine is a local disposition, never a new cloud idempotency key. */
static inline size_t replay_basename_length(const char *base)
{
    const size_t n = strlen(base);
    if (replay_path_quarantined(base)) return n - 10U;
    const char *dot = strrchr(base, '.');
    return dot && dot > base ? (size_t)(dot - base) : n;
}
