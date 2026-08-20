#include <assert.h>
#include <string.h>

#include "nuwax_provision_qr_payload.h"

int main(void) {
    char payload[256] = {0};
    assert(nuwax_provision_qr_payload_build(
        payload,
        sizeof(payload),
        "PROV_A1B2C3",
        "NX-A1B2-C3D4-E5F6",
        "Nuwax7-Dev-PoP"));
    assert(strcmp(payload,
                  "https://nuwax.com/lz.html?n=PROV_A1B2C3&u="
                  "NX-A1B2-C3D4-E5F6&p=Nuwax7-Dev-PoP") == 0);

    assert(nuwax_provision_qr_payload_build(
        payload, sizeof(payload), "PROV_A1B2C3", "NX-A1B2", "a b&c"));
    assert(strcmp(payload,
                  "https://nuwax.com/lz.html?n=PROV_A1B2C3&u=NX-A1B2&"
                  "p=a%20b%26c") == 0);

    assert(nuwax_provision_qr_payload_build(
        payload, sizeof(payload), "PROV_A1B2C3", "NX-A1B2", NULL));
    assert(strcmp(payload,
                  "https://nuwax.com/lz.html?n=PROV_A1B2C3&u=NX-A1B2") == 0);

    char too_small[32] = "stale";
    assert(!nuwax_provision_qr_payload_build(
        too_small, sizeof(too_small), "PROV_A1B2C3", "NX-A1B2", "pop"));
    assert(too_small[0] == '\0');
    return 0;
}
