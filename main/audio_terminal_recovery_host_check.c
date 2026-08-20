#include <assert.h>

#include "conversation/audio_terminal_recovery_policy.h"

int main(void) {
    assert(nuwax_audio_terminal_recovery_action(
               false, true, true, true, false) ==
           NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_IDLE);
    assert(nuwax_audio_terminal_recovery_action(
               true, true, true, false, false) ==
           NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_IDLE);
    assert(nuwax_audio_terminal_recovery_action(
               true, true, true, true, false) ==
           NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_RETRY_END);
    assert(nuwax_audio_terminal_recovery_action(
               true, true, true, true, true) ==
           NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_RETRY_STOP);
    assert(nuwax_audio_terminal_recovery_action(
               true, false, true, true, false) ==
           NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_COMPLETE);
    assert(nuwax_audio_terminal_recovery_action(
               true, true, false, true, false) ==
           NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_COMPLETE);
    assert(nuwax_audio_terminal_recovery_backoff_ms(0U) == 100U);
    assert(nuwax_audio_terminal_recovery_backoff_ms(4U) == 1600U);
    assert(nuwax_audio_terminal_recovery_backoff_ms(5U) == 2000U);
    assert(!nuwax_audio_terminal_recovery_should_quarantine(7U, 29999U));
    assert(nuwax_audio_terminal_recovery_should_quarantine(8U, 0U));
    assert(nuwax_audio_terminal_recovery_should_quarantine(0U, 30000U));
    assert(nuwax_audio_terminal_recovery_wait_ms(1500000, 30000000, 1000000,
                                                 100U) == 100U);
    assert(nuwax_audio_terminal_recovery_wait_ms(1050000, 30000000, 1000000,
                                                 100U) == 50U);
    assert(nuwax_audio_terminal_recovery_wait_ms(1000000, 30000000, 1000000,
                                                 100U) == 0U);
    assert(nuwax_audio_terminal_recovery_wait_ms(30000000, 1000500, 1000000,
                                                 100U) == 1U);
    return 0;
}
