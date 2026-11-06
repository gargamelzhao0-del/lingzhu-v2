#pragma once
#include <stddef.h>

/* Called by the single formal replay owner, outside WebSocket locks. Failure
 * only disables the optional accelerator; the normal TLS stack remains usable. */
void nuwax_tls_archive_crypto_prepare(void);
void nuwax_tls_archive_crypto_release(void);
/* Single replay owner only. Zero means the optional hardware path is absent;
 * the regular TLS implementation remains available. Includes a TLS 1.3 byte. */
size_t nuwax_tls_archive_crypto_capacity(void);
