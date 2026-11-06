#pragma once
#include <stdbool.h>
#include <stddef.h>

static inline bool nuwax_tls_software_suite(int suite)
{
    /* IANA: TLS 1.3 ChaCha20, TLS 1.2 ECDHE ECDSA/RSA ChaCha20. */
    return suite == 0x1303 || suite == 0xcca9 || suite == 0xcca8;
}

static inline unsigned nuwax_tls_suite_priority(int suite)
{
    if (nuwax_tls_software_suite(suite)) return 0U;
    /* TLS 1.3 AES-128-GCM and TLS 1.2 ECDHE ECDSA/RSA AES-128-GCM.
     * The deployed TLS 1.2 endpoint does not select RSA ChaCha20. Preserve
     * authentication and all fallbacks, but avoid software AES-256 when
     * the peer also accepts AES-128-GCM. Never prefer a legacy CBC suite. */
    if (suite == 0x1301 || suite == 0xc02b || suite == 0xc02f) return 1U;
    return 2U;
}

/* No filtering, forced cipher or extra handshake: servers can select any of
 * the original enabled suites. Reject an oversized list instead of truncating. */
static inline bool nuwax_tls_order_suites(const int *supported, int *ordered, size_t capacity)
{
    if (!supported || !ordered || capacity == 0U) return false;
    size_t count = 0U, out = 0U;
    while (count < capacity && supported[count] != 0) count++;
    if (count == 0U || count == capacity) return false;
    for (unsigned pass = 0U; pass < 3U; ++pass)
        for (size_t i = 0U; i < count; ++i)
            if (nuwax_tls_suite_priority(supported[i]) == pass) ordered[out++] = supported[i];
    ordered[out] = 0;
    return true;
}
