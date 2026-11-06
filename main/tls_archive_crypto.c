#include "tls_archive_crypto.h"
#include "tls_record_metrics.h"
#include "sdkconfig.h"
#include "psa/crypto.h"
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"

#if CONFIG_NUWAX_SD_TLS_SCOPED_AES
#include "aes/esp_aes_gcm.h"
#include "esp_crypto_shared_gdma.h"
#if defined(ESP_PLATFORM) || defined(NUWAX_ARCHIVE_GHASH_HOST_TEST)
#include "archive_ghash.h"
#include "esp_timer.h"
#define ARCHIVE_GHASH_OPTIMIZED 1
#else
#define ARCHIVE_GHASH_OPTIMIZED 0
#endif

/* Only the SD sender uses this context. In particular, SRP/BLE provisioning,
 * TLS handshakes, RX and ordinary messages keep the existing software driver.
 * The upstream PSA dispatcher still checks key permissions before this hook. */
#define ARCHIVE_PLAINTEXT_MAX (CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN + 1U)
static esp_gcm_context *s_context;
static uint8_t *s_output;
static size_t s_plaintext_capacity;
static uint8_t *s_staged_record;
static const uint8_t *s_ciphertext;
static bool s_disabled, s_verified, s_staged_verified;
/* The multiplication table belongs to the key, never to a TLS record. The
 * SDK starts/finish calls reset IV, counter, lengths and GHASH accumulator.
 * Keep an explicit key copy so key updates cannot reuse an old table. */
static uint8_t s_key[32];
static size_t s_key_bytes;
static atomic_flag s_busy = ATOMIC_FLAG_INIT;
#if ARCHIVE_GHASH_OPTIMIZED
static archive_ghash_table_t *s_ghash_table;
static bool s_ghash_calibrated, s_ghash_fast;

/* Same SDK state machine and AES-CTR backend. Only the ciphertext GHASH data
 * loop changes. This encrypt-only helper is private to bounded SD records. */
static int archive_gcm_update(const uint8_t *input, size_t bytes, uint8_t *output)
{
    nuwax_ws_send_metrics_t *metrics = nuwax_tls_archive_metrics();
    if (!s_ghash_fast) {
        size_t written = 0U;
        const int64_t start = esp_timer_get_time();
        const int err = esp_aes_gcm_update(s_context, input, bytes, output, 0U, &written);
        if (metrics) metrics->tls_gcm_sdk_update_us += (uint64_t)(esp_timer_get_time() - start);
        return err;
    }
    if (s_context->mode != ESP_AES_ENCRYPT || s_context->iv_len != 12U ||
        (s_context->gcm_state != ESP_AES_GCM_STATE_START && s_context->gcm_state != ESP_AES_GCM_STATE_UPDATE) ||
        s_context->data_len + bytes > ARCHIVE_PLAINTEXT_MAX) return -1;
    uint8_t counter[16], stream[16] = {0}; size_t offset = 0U;
    memcpy(counter, s_context->J0, sizeof(counter));
    if (s_context->gcm_state == ESP_AES_GCM_STATE_START) {
        /* GCM inc32, never carry into the IV. SDK starts() constructs J0. */
        for (unsigned i = 16U; i > 12U; --i) if (++counter[i - 1U]) break;
        s_context->gcm_state = ESP_AES_GCM_STATE_UPDATE;
    }
    int64_t start = esp_timer_get_time();
    const int err = esp_aes_crypt_ctr(&s_context->aes_ctx, bytes, &offset, counter, stream, input, output);
    if (metrics) metrics->tls_gcm_ctr_us += (uint64_t)(esp_timer_get_time() - start);
    if (err) return err;
    memcpy(s_context->J0, counter, sizeof(counter));
    s_context->data_len += bytes;
    start = esp_timer_get_time();
    archive_ghash_update(*s_ghash_table, s_context->ghash, output, bytes);
    if (metrics) {
        metrics->tls_gcm_hash_us += (uint64_t)(esp_timer_get_time() - start);
        metrics->tls_gcm_fast_updates++;
    }
    return 0;
}

static void archive_ghash_calibrate(void)
{
    s_ghash_calibrated = true; s_ghash_fast = false;
    if (!s_ghash_table || s_context->gcm_state != ESP_AES_GCM_STATE_START) {
        ESP_LOGI("sd_tls_crypto", "GHASH calibration backend=sdk4 table=absent revision=201");
        return;
    }
    archive_ghash_table_prepare(*s_ghash_table, s_context->H);
    uint8_t saved[16], reference[16], candidate[16];
    memcpy(saved, s_context->ghash, sizeof(saved));
    const uint8_t *aad = s_context->aad; const uint64_t aad_bytes = s_context->aad_len;
    uint64_t sdk_min = UINT64_MAX, fast_min = UINT64_MAX;
    bool matches = true;
    /* Identical internal input, fixed bounded work, no secret values logged.
     * Minimum-of-three reduces a single wake/UI preemption's timing bias. */
    const size_t probe_bytes = s_plaintext_capacity < 512U ? s_plaintext_capacity & ~(size_t)15U : 512U;
    if (!probe_bytes) return;
    for (unsigned i = 0U; i < probe_bytes; ++i) s_output[i] = (uint8_t)(i * 17U + 9U);
    for (unsigned trial = 0U; trial < 3U; ++trial) {
        memset(s_context->ghash, 0, 16U);
        int64_t start = esp_timer_get_time();
        const int err = esp_aes_gcm_update_ad(s_context, s_output, probe_bytes);
        uint64_t us = (uint64_t)(esp_timer_get_time() - start);
        if (us < sdk_min) sdk_min = us;
        memcpy(reference, s_context->ghash, 16U);
        memset(candidate, 0, 16U);
        start = esp_timer_get_time();
        archive_ghash_update(*s_ghash_table, candidate, s_output, probe_bytes);
        us = (uint64_t)(esp_timer_get_time() - start);
        if (us < fast_min) fast_min = us;
        if (err || memcmp(reference, candidate, 16U)) matches = false;
    }
    memcpy(s_context->ghash, saved, sizeof(saved));
    s_context->aad = aad; s_context->aad_len = aad_bytes;
    s_ghash_fast = matches && archive_ghash_worthwhile(sdk_min, fast_min);
#ifdef NUWAX_ARCHIVE_GHASH_FORCE_TEST
    s_ghash_fast = matches && NUWAX_ARCHIVE_GHASH_FORCE_TEST;
#endif
    ESP_LOGI("sd_tls_crypto", "GHASH calibration backend=%s table=psram sdk_min_us=%llu byte_min_us=%llu matched=%u revision=201",
        s_ghash_fast ? "byte8" : "sdk4", (unsigned long long)sdk_min,
        (unsigned long long)fast_min, matches ? 1U : 0U);
}

static int archive_gcm_start(const uint8_t *iv, size_t iv_bytes, const uint8_t *aad, size_t aad_bytes)
{
    int err = esp_aes_gcm_starts(s_context, ESP_AES_ENCRYPT, iv, iv_bytes);
    if (!err && !s_ghash_calibrated) archive_ghash_calibrate();
    if (!err) err = esp_aes_gcm_update_ad(s_context, aad, aad_bytes);
    return err;
}
#endif

static void forget_key(void)
{
    if (s_context) esp_aes_gcm_free(s_context);
    mbedtls_platform_zeroize(s_key, sizeof(s_key));
    s_key_bytes = 0U;
    s_verified = false;
    s_staged_verified = false;
#if ARCHIVE_GHASH_OPTIMIZED
    s_ghash_calibrated = s_ghash_fast = false;
    if (s_ghash_table) mbedtls_platform_zeroize(s_ghash_table, sizeof(*s_ghash_table));
#endif
}

static int encrypt_record(const uint8_t *key, size_t key_bytes,
    const uint8_t *iv, size_t iv_bytes, const uint8_t *aad, size_t aad_bytes,
    const uint8_t *input, size_t bytes, bool allow_fast)
{
    if (!s_output || (bytes > s_plaintext_capacity && !s_staged_record)) return -1;
    /* SDK GCM supports exact in-place input/output. Keep the caller's input
     * unchanged, including after partial DMA failure. GHASH and AES operate
     * on this bounded internal buffer, not on external RAM. */
    int err = 0;
    if (s_key_bytes != key_bytes || memcmp(s_key, key, key_bytes)) {
        forget_key();
        esp_aes_gcm_init(s_context);
        /* ESP's AES GCM port ignores the cipher selector, accepting AES only. */
        err = esp_aes_gcm_setkey(s_context, 0, key, (unsigned)key_bytes * 8U);
        if (!err) {
            memcpy(s_key, key, key_bytes);
            s_key_bytes = key_bytes;
        }
    }
    if (!err && bytes <= s_plaintext_capacity) {
#if ARCHIVE_GHASH_OPTIMIZED
        if (allow_fast) {
            err = archive_gcm_start(iv, iv_bytes, aad, aad_bytes);
            memcpy(s_output, input, bytes); /* Calibration used private scratch. */
            if (!err) err = archive_gcm_update(s_output, bytes, s_output);
            size_t written = 0U;
            if (!err) err = esp_aes_gcm_finish(s_context, s_output, 0U, &written, s_output + bytes, 16U);
        } else
#else
        (void)allow_fast;
#endif
        {
        memcpy(s_output, input, bytes);
        err = esp_aes_gcm_crypt_and_tag(s_context, ESP_AES_ENCRYPT,
            bytes, iv, iv_bytes, aad, aad_bytes, s_output, s_output, 16U, s_output + bytes);
        }
        s_ciphertext = s_output;
    } else if (!err) {
        /* Low internal headroom does not require smaller TLS records. Keep
         * all GHASH/CTR work in the existing DMA window and only the completed
         * ciphertext in PSRAM. Until finish succeeds the caller's plaintext
         * remains untouched, so every partial failure can use software TLS.
         * Nonfinal chunks MUST be a multiple of 16: SDK update pads GHASH. */
#if ARCHIVE_GHASH_OPTIMIZED
        err = archive_gcm_start(iv, iv_bytes, aad, aad_bytes);
#else
        err = esp_aes_gcm_starts(s_context, ESP_AES_ENCRYPT, iv, iv_bytes);
        if (!err) err = esp_aes_gcm_update_ad(s_context, aad, aad_bytes);
#endif
        size_t offset = 0U, written = 0U;
        const size_t chunk_max = s_plaintext_capacity & ~(size_t)15U;
        while (!err && offset < bytes) {
            const size_t n = bytes - offset < chunk_max ? bytes - offset : chunk_max;
            memcpy(s_output, input + offset, n);
#if ARCHIVE_GHASH_OPTIMIZED
            err = archive_gcm_update(s_output, n, s_output);
#else
            err = esp_aes_gcm_update(s_context, s_output, n, s_output, 0U, &written);
#endif
            if (!err) memcpy(s_staged_record + offset, s_output, n);
            offset += n;
        }
        if (!err) err = esp_aes_gcm_finish(s_context, s_output, 0U, &written, s_output, 16U);
        if (!err) memcpy(s_staged_record + bytes, s_output, 16U);
        s_ciphertext = s_staged_record;
    }
    if (err) forget_key();
    return err;
}

void nuwax_tls_archive_crypto_prepare(void)
{
    if (atomic_flag_test_and_set_explicit(&s_busy, memory_order_acquire)) return;
    if (s_disabled || s_context) goto done;
    /* Leave the existing internal reserve available to MIC, Wi-Fi and UI. */
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 24576U ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 4096U)
        goto done;
    s_context = heap_caps_calloc(1U, sizeof(*s_context), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t capacity = ARCHIVE_PLAINTEXT_MAX;
    while (s_context && !s_output) {
        const size_t allocation = (capacity + 79U) & ~(size_t)63U;
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= allocation + 24576U &&
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= allocation) {
            s_output = heap_caps_aligned_alloc(64U, allocation,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
            if (s_output && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 24576U) {
                heap_caps_free(s_output); s_output = NULL;
            }
        }
        if (s_output) { s_plaintext_capacity = capacity; break; }
        if (capacity <= 4097U) break;
        capacity = 4097U;
    }
    if (!s_context || !s_output) goto failed;
    if (s_plaintext_capacity < ARCHIVE_PLAINTEXT_MAX &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= ARCHIVE_PLAINTEXT_MAX + 16U + 65536U) {
        s_staged_record = heap_caps_malloc(ARCHIVE_PLAINTEXT_MAX + 16U,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_staged_record && heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < 65536U) {
            heap_caps_free(s_staged_record); s_staged_record = NULL;
        }
    }
    /* AES-128 GCM known-answer check: all-zero 16-byte key/plaintext, 12-byte IV. */
    static const uint8_t zero[16];
    static const uint8_t expected[32] = {
        0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78,
        0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf
    };
    if (encrypt_record(zero, 16U, zero, 12U, zero, 0U, zero, 16U, false) ||
        memcmp(s_output, expected, sizeof(expected))) {
        s_disabled = true;
        ESP_LOGW("sd_tls_crypto", "scoped AES self-check unavailable; retaining software TLS");
        goto failed;
    }
    /* The KAT key must not authorize real traffic. Each new traffic key is
     * independently compared against PSA software on its first record. */
    forget_key();
#if ARCHIVE_GHASH_OPTIMIZED
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= sizeof(*s_ghash_table) + 65536U) {
        s_ghash_table = heap_caps_malloc(sizeof(*s_ghash_table), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_ghash_table && heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < 65536U) {
            heap_caps_free(s_ghash_table); s_ghash_table = NULL;
        }
    }
#endif
    ESP_LOGI("sd_tls_crypto", "scoped AES ready capacity=%u dma_window=%u staged=%u reserve=24576 key_cache=1",
             (unsigned)(s_staged_record ? ARCHIVE_PLAINTEXT_MAX : s_plaintext_capacity),
             (unsigned)s_plaintext_capacity, s_staged_record ? 1U : 0U);
    goto done;
failed:
    forget_key();
    if (s_context) mbedtls_platform_zeroize(s_context, sizeof(*s_context));
    heap_caps_free(s_context); s_context = NULL;
    heap_caps_free(s_output); s_output = NULL;
    if (s_staged_record) mbedtls_platform_zeroize(s_staged_record, ARCHIVE_PLAINTEXT_MAX + 16U);
    heap_caps_free(s_staged_record); s_staged_record = NULL;
    s_ciphertext = NULL;
#if ARCHIVE_GHASH_OPTIMIZED
    heap_caps_free(s_ghash_table); s_ghash_table = NULL;
#endif
    s_plaintext_capacity = 0U;
    /* This profile leaves all other hardware AES/SHA paths disabled. */
    esp_crypto_shared_gdma_free();
done:
    atomic_flag_clear_explicit(&s_busy, memory_order_release);
}

void nuwax_tls_archive_crypto_release(void)
{
    if (atomic_flag_test_and_set_explicit(&s_busy, memory_order_acquire)) return;
    if (s_context) {
        forget_key();
        mbedtls_platform_zeroize(s_context, sizeof(*s_context));
        heap_caps_free(s_context); s_context = NULL;
        mbedtls_platform_zeroize(s_output, s_plaintext_capacity + 16U);
        heap_caps_free(s_output); s_output = NULL;
        s_plaintext_capacity = 0U;
        esp_crypto_shared_gdma_free();
    }
    if (s_staged_record) mbedtls_platform_zeroize(s_staged_record, ARCHIVE_PLAINTEXT_MAX + 16U);
    heap_caps_free(s_staged_record); s_staged_record = NULL;
    s_ciphertext = NULL;
#if ARCHIVE_GHASH_OPTIMIZED
    heap_caps_free(s_ghash_table); s_ghash_table = NULL;
#endif
    mbedtls_platform_zeroize(s_key, sizeof(s_key));
    s_key_bytes = 0U;
    s_verified = false;
    s_staged_verified = false;
    atomic_flag_clear_explicit(&s_busy, memory_order_release);
}
size_t nuwax_tls_archive_crypto_capacity(void)
{
    return !s_disabled && s_context && s_output ?
        (s_staged_record ? ARCHIVE_PLAINTEXT_MAX : s_plaintext_capacity) : 0U;
}
#else
void nuwax_tls_archive_crypto_prepare(void) {}
void nuwax_tls_archive_crypto_release(void) {}
size_t nuwax_tls_archive_crypto_capacity(void) { return 0U; }
#endif

psa_status_t __real_mbedtls_psa_aead_encrypt(const psa_key_attributes_t *attributes,
    const uint8_t *key, size_t key_bytes, psa_algorithm_t alg,
    const uint8_t *iv, size_t iv_bytes, const uint8_t *aad, size_t aad_bytes,
    const uint8_t *input, size_t bytes, uint8_t *output, size_t capacity, size_t *written);

psa_status_t __wrap_mbedtls_psa_aead_encrypt(const psa_key_attributes_t *attributes,
    const uint8_t *key, size_t key_bytes, psa_algorithm_t alg,
    const uint8_t *iv, size_t iv_bytes, const uint8_t *aad, size_t aad_bytes,
    const uint8_t *input, size_t bytes, uint8_t *output, size_t capacity, size_t *written)
{
#if CONFIG_NUWAX_SD_TLS_SCOPED_AES
    nuwax_ws_send_metrics_t *m = nuwax_tls_archive_metrics();
    if (m && alg == PSA_ALG_GCM && attributes && key && iv && input && output && written &&
        psa_get_key_type(attributes) == PSA_KEY_TYPE_AES &&
        (key_bytes == 16U || key_bytes == 32U) && psa_get_key_bits(attributes) == key_bytes * 8U &&
        iv_bytes == 12U && (aad || !aad_bytes) && bytes <= ARCHIVE_PLAINTEXT_MAX &&
        capacity >= bytes + 16U &&
        !atomic_flag_test_and_set_explicit(&s_busy, memory_order_acquire)) {
        if (!s_disabled && s_context && s_output && bytes <= nuwax_tls_archive_crypto_capacity() &&
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 12288U) {
            const bool key_reused = s_key_bytes == key_bytes && !memcmp(s_key, key, key_bytes);
            const int hw_err = encrypt_record(key, key_bytes, iv, iv_bytes, aad, aad_bytes, input, bytes, true);
            if (!hw_err) {
                if (bytes > s_plaintext_capacity) {
                    m->tls_hw_staged_calls++;
                    m->tls_hw_staged_bytes += (uint32_t)bytes;
                }
                if (key_reused) m->tls_hw_key_reuses++;
                else m->tls_hw_key_setups++;
                /* A short first file can verify only the direct path. Verify
                 * the first chunked record separately before trusting it. */
                bool *verified = bytes > s_plaintext_capacity ? &s_staged_verified : &s_verified;
                if (!*verified) {
                    /* Do not alter an in-place plaintext before the software
                     * fallback has run. The software result is authoritative. */
                    const psa_status_t result = __real_mbedtls_psa_aead_encrypt(attributes,
                        key, key_bytes, alg, iv, iv_bytes, aad, aad_bytes, input, bytes,
                        output, capacity, written);
                    *verified = result == PSA_SUCCESS && *written == bytes + 16U &&
                        !memcmp(output, s_ciphertext, *written);
                    if (!*verified) { forget_key(); s_disabled = true; m->tls_hw_failures++; }
                    else m->tls_hw_verified++;
                    atomic_flag_clear_explicit(&s_busy, memory_order_release);
                    return result;
                }
                memcpy(output, s_ciphertext, bytes + 16U);
                *written = bytes + 16U;
                m->tls_hw_calls++; m->tls_hw_bytes += (uint32_t)bytes;
                atomic_flag_clear_explicit(&s_busy, memory_order_release);
                return PSA_SUCCESS;
            }
            s_disabled = true; m->tls_hw_failures++;
        }
        m->tls_hw_fallbacks++;
        atomic_flag_clear_explicit(&s_busy, memory_order_release);
    }
#endif
    return __real_mbedtls_psa_aead_encrypt(attributes, key, key_bytes, alg, iv, iv_bytes,
                                          aad, aad_bytes, input, bytes, output, capacity, written);
}
