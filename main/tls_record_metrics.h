#pragma once
#include <stdbool.h>
#include "ws_send_metrics.h"

/* Synchronous, optional instrumentation. It never waits or allocates. Only
 * the task that acquired the scope may access its caller-owned metrics. */
bool nuwax_tls_metrics_begin(nuwax_ws_send_metrics_t *metrics);
void nuwax_tls_metrics_end(void);
/* Borrowed only by the synchronous PSA driver hook on the owning sender task. */
nuwax_ws_send_metrics_t *nuwax_tls_archive_metrics(void);
/* Process-lifetime preference list; every enabled default suite is retained.
 * NULL means use the SDK defaults. Certificate validation is unchanged. */
const int *nuwax_tls_software_suites(void);
