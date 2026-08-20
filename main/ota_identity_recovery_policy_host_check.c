#include <assert.h>

#include "nuwax_ota_new/nuwax_ota_identity_recovery_policy.h"

int main(void)
{
    assert(nuwax_ota_identity_partition_fallback_allowed(
        false, NUWAX_OTA_IDENTITY_MISSING));
    assert(nuwax_ota_identity_partition_fallback_allowed(
        false, NUWAX_OTA_IDENTITY_STALE));

    assert(!nuwax_ota_identity_partition_fallback_allowed(
        true, NUWAX_OTA_IDENTITY_MISSING));
    assert(!nuwax_ota_identity_partition_fallback_allowed(
        true, NUWAX_OTA_IDENTITY_STALE));
    assert(!nuwax_ota_identity_partition_fallback_allowed(
        false, NUWAX_OTA_IDENTITY_AVAILABLE));
    assert(!nuwax_ota_identity_partition_fallback_allowed(
        false, NUWAX_OTA_IDENTITY_STORAGE_FAILURE));
    return 0;
}
