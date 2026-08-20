#include "cloud_connection_epoch_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint32_t pending_epoch = 0U;

    assert(!nuwax_cloud_epoch_record_provision_required(
        12U, 11U, &pending_epoch));
    assert(pending_epoch == 0U);
    assert(nuwax_cloud_epoch_accepts_positive(12U, 12U, pending_epoch));

    assert(nuwax_cloud_epoch_record_provision_required(
        12U, 12U, &pending_epoch));
    assert(pending_epoch == 12U);
    assert(!nuwax_cloud_epoch_accepts_positive(12U, 12U, pending_epoch));
    assert(!nuwax_cloud_epoch_consume_provision_required(
        11U, &pending_epoch));
    assert(pending_epoch == 12U);

    assert(nuwax_cloud_epoch_consume_provision_required(
        12U, &pending_epoch));
    assert(pending_epoch == 0U);
    assert(nuwax_cloud_epoch_accepts_positive(12U, 12U, pending_epoch));

    assert(!nuwax_cloud_epoch_record_provision_required(
        13U, 12U, &pending_epoch));
    assert(!nuwax_cloud_epoch_accepts_positive(13U, 12U, pending_epoch));
    assert(!nuwax_cloud_epoch_is_current(0U, 0U));

    puts("cloud_connection_epoch_policy_host_check: PASS");
    return 0;
}
