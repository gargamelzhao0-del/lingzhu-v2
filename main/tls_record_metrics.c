#include "tls_record_metrics.h"
#include "tls_cipher_policy.h"
#include <stdatomic.h>
#include <stdint.h>
#include <errno.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "mbedtls/net_sockets.h"

/* The formal replay owner is serialized. An unrelated caller never reads
 * the borrowed pointer: task identity is checked first. A competing scope
 * simply skips profiling, and cannot hold up networking or audio. */
static atomic_uintptr_t s_metrics_owner;
static nuwax_ws_send_metrics_t *s_metrics;

static nuwax_ws_send_metrics_t *tls_current_metrics(void)
{
    return atomic_load_explicit(&s_metrics_owner, memory_order_acquire) ==
        (uintptr_t)xTaskGetCurrentTaskHandle() ? s_metrics : NULL;
}

bool nuwax_tls_metrics_begin(nuwax_ws_send_metrics_t *metrics)
{
    if (!metrics) return false;
    uintptr_t vacant = 0U;
    if (!atomic_compare_exchange_strong_explicit(&s_metrics_owner, &vacant,
            (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_acq_rel, memory_order_acquire)) {
        metrics->tls_profile_skipped++;
        return false;
    }
    s_metrics = metrics;
    metrics->tls_profiled_calls++;
    return true;
}

nuwax_ws_send_metrics_t *nuwax_tls_archive_metrics(void)
{
    nuwax_ws_send_metrics_t *metrics = tls_current_metrics();
    return metrics && metrics->shared_tls_archive ? metrics : NULL;
}

void nuwax_tls_metrics_end(void)
{
    if (atomic_load_explicit(&s_metrics_owner, memory_order_acquire) !=
        (uintptr_t)xTaskGetCurrentTaskHandle()) return;
    s_metrics = NULL;
    atomic_store_explicit(&s_metrics_owner, 0U, memory_order_release);
}

psa_status_t __real_psa_aead_encrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg,
    const uint8_t *nonce, size_t nonce_length, const uint8_t *additional_data,
    size_t additional_data_length, const uint8_t *plaintext, size_t plaintext_length,
    uint8_t *ciphertext, size_t ciphertext_size, size_t *ciphertext_length);

psa_status_t __wrap_psa_aead_encrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg,
    const uint8_t *nonce, size_t nonce_length, const uint8_t *additional_data,
    size_t additional_data_length, const uint8_t *plaintext, size_t plaintext_length,
    uint8_t *ciphertext, size_t ciphertext_size, size_t *ciphertext_length)
{
    nuwax_ws_send_metrics_t *metrics = tls_current_metrics();
    const int64_t started = metrics ? esp_timer_get_time() : 0;
    const psa_status_t result = __real_psa_aead_encrypt(key, alg, nonce, nonce_length,
        additional_data, additional_data_length, plaintext, plaintext_length,
        ciphertext, ciphertext_size, ciphertext_length);
    const int saved_errno = errno;
    if (metrics) {
        const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - started);
        metrics->tls_aead_us += elapsed;
        if (elapsed > metrics->tls_aead_max_us) metrics->tls_aead_max_us = elapsed;
        metrics->tls_aead_calls++;
        metrics->tls_aead_bytes += (uint32_t)plaintext_length;
    }
    errno = saved_errno;
    return result;
}

int __real_mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len);
int __wrap_mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    nuwax_ws_send_metrics_t *metrics = tls_current_metrics();
    const int64_t started = metrics ? esp_timer_get_time() : 0;
    const int result = __real_mbedtls_net_send(ctx, buf, len);
    const int saved_errno = errno;
    if (metrics) {
        const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - started);
        metrics->tls_bio_us += elapsed;
        if (elapsed > metrics->tls_bio_max_us) metrics->tls_bio_max_us = elapsed;
        metrics->tls_bio_calls++;
        if (result > 0) metrics->tls_bio_bytes += (uint32_t)result;
    }
    errno = saved_errno;
    return result;
}

const int *nuwax_tls_software_suites(void)
{
#if !CONFIG_MBEDTLS_HARDWARE_AES && CONFIG_MBEDTLS_CHACHAPOLY_C
    static atomic_uint state;
    static int suites[128];
    unsigned vacant = 0U;
    if (atomic_compare_exchange_strong_explicit(&state, &vacant, 1U,
            memory_order_acq_rel, memory_order_acquire)) {
        const bool ok = nuwax_tls_order_suites(esp_tls_get_ciphersuites_list(), suites,
                                             sizeof(suites) / sizeof(suites[0]));
        atomic_store_explicit(&state, ok ? 2U : 3U, memory_order_release);
    }
    /* An overlapping handshake uses defaults, never spins on initialization. */
    if (atomic_load_explicit(&state, memory_order_acquire) == 2U) return suites;
#endif
    return NULL;
}
