#include <assert.h>
#include <stdio.h>

#include "conversation/aec_reference_provenance.h"

int main(void) {
    const nuwax_aec_reference_provenance_t hardware =
        nuwax_aec_reference_select(true, true, 41U, false, 0U);
    assert(hardware.snapshot);
    assert(hardware.kind == NUWAX_AEC_PROVENANCE_HARDWARE);
    assert(hardware.generation == 41U);

    const nuwax_aec_reference_provenance_t software =
        nuwax_aec_reference_select(false, true, 41U, true, 17U);
    assert(software.snapshot);
    assert(software.kind == NUWAX_AEC_PROVENANCE_SOFTWARE);
    assert(software.generation == 17U);

    const nuwax_aec_reference_provenance_t unbound_hardware =
        nuwax_aec_reference_select(true, false, 41U, true, 17U);
    assert(!unbound_hardware.snapshot);
    assert(unbound_hardware.kind == NUWAX_AEC_PROVENANCE_NONE);

    assert(nuwax_aec_reference_epoch_contiguous(hardware, hardware, true));
    assert(!nuwax_aec_reference_epoch_contiguous(hardware, software, true));
    assert(!nuwax_aec_reference_epoch_contiguous(hardware, hardware, false));
    assert(nuwax_aec_reference_epoch_contiguous(
        software, (nuwax_aec_reference_provenance_t){0}, true));

    puts("AEC reference provenance host check: PASS");
    return 0;
}
