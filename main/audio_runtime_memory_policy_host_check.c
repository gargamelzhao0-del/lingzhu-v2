#include <stdio.h>
#include <stdlib.h>

#include "audio_runtime_memory_policy.h"

static void expect_state(const char *name,
                         nuwax_audio_runtime_memory_t memory,
                         nuwax_audio_runtime_memory_state_t expected) {
    const nuwax_audio_runtime_memory_state_t actual =
        nuwax_audio_runtime_memory_classify(memory);
    if (actual != expected) {
        fprintf(stderr, "%s: expected=%d actual=%d\n", name,
                (int)expected, (int)actual);
        exit(1);
    }
}

int main(void) {
    expect_state("healthy",
                 (nuwax_audio_runtime_memory_t){16384U, 10240U, 8192U},
                 NUWAX_AUDIO_RUNTIME_MEMORY_READY);
    expect_state("fragmented defer",
                 (nuwax_audio_runtime_memory_t){20000U, 8192U, 4096U},
                 NUWAX_AUDIO_RUNTIME_MEMORY_DEFER);
    expect_state("low total critical",
                 (nuwax_audio_runtime_memory_t){3083U, 1920U, 1920U},
                 NUWAX_AUDIO_RUNTIME_MEMORY_CRITICAL);
    expect_state("log terminal state critical",
                 (nuwax_audio_runtime_memory_t){1387U, 640U, 640U},
                 NUWAX_AUDIO_RUNTIME_MEMORY_CRITICAL);

    puts("audio runtime memory policy: PASS");
    return 0;
}
