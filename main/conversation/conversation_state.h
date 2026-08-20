#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"

#define NUWAX_CONVERSATION_STATE_INITIALIZER(initial_state) \
    { .current = (initial_state) }

typedef struct {
    bool initialized;
    volatile nuwax_app_state_t current;
    volatile uint32_t generation;
    uint32_t idle_since_ms;
} nuwax_conversation_state_t;

bool nuwax_conversation_state_set(nuwax_conversation_state_t *state,
                                  nuwax_app_state_t next,
                                  uint32_t now_ms,
                                  uint32_t *out_generation);
nuwax_app_state_t nuwax_conversation_state_current(const nuwax_conversation_state_t *state);
uint32_t nuwax_conversation_state_generation(const nuwax_conversation_state_t *state);
uint32_t nuwax_conversation_state_idle_since_ms(const nuwax_conversation_state_t *state);
bool nuwax_conversation_state_idle_sleep_due(const nuwax_conversation_state_t *state,
                                             uint32_t now_ms,
                                             uint32_t timeout_ms);
