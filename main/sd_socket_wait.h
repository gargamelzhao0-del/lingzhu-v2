#pragma once
#include <stdbool.h>
#include "ws_send_metrics.h"

/* Optional lifetime timer, prepared outside transport locks. Failure retains
 * the SDK's semaphore timeout path. No global tick or SDK source changes. */
void nuwax_sd_socket_wait_prepare(void);
bool nuwax_sd_socket_wait_begin(nuwax_ws_send_metrics_t *metrics);
void nuwax_sd_socket_wait_end(void);
