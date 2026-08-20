#include <assert.h>
#include <stdio.h>

#include "monitoring_speech_policy.h"

int main(void) {
    /* Upper end of the quiet-room evidence supplied from the real device. */
    assert(!nuwax_monitoring_vad_energy_qualified(39U, 67U, 200U,
                                                   160U, 600U, 80U));
    assert(!nuwax_monitoring_vad_energy_qualified(39U, 100U, 261U,
                                                   160U, 600U, 80U));
    /* Weak real speech remains below the standalone 160/600 detector while
     * still qualifying an ESP-VAD speech decision. */
    assert(nuwax_monitoring_vad_energy_qualified(39U, 100U, 350U,
                                                  160U, 600U, 80U));
    assert(!nuwax_monitoring_stream_activity_confirmed(9U, 3U, 10U));
    assert(nuwax_monitoring_stream_activity_confirmed(10U, 3U, 10U));
    assert(nuwax_monitoring_stream_activity_confirmed(3U, 3U, 2U));
    puts("monitoring_speech_policy_host_check: PASS");
    return 0;
}
