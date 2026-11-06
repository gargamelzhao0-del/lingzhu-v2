#include "board/sd_write_verify.h"
#include "board/sd_io_memory_budget.h"
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "sdmmc_cmd.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define SD_VERIFY_DMA DMA_ATTR
#else
#define SD_VERIFY_DMA
#endif

enum { SECTOR_BYTES = 512U, READ_BUFFER_MAX = NUWAX_SD_VERIFY_MAX,
       VERIFY_BUDGET_US = 1500000U };
static atomic_uint s_writes, s_verified, s_rewrites, s_recovered, s_failures, s_memory_denied, s_max_us;
static atomic_uint s_log_ms;
static atomic_bool s_verify_single_reads;
static atomic_uint s_reserve_uses, s_single_recoveries;
/* A serialized FatFs operation must not fail merely because its verifier's
 * temporary allocation lost a race with TLS. This bounded internal reserve
 * also avoids the SDK's allocating PSRAM/unaligned-source bounce path. */
/* Native SDK writes complete synchronously before readback starts. The
 * caller still owns every original source byte, so the same sector can be
 * the write bounce and then the independent read destination. */
static SD_VERIFY_DMA _Alignas(64) uint8_t s_verify_reserve[512];
static atomic_flag s_verify_reserve_owner = ATOMIC_FLAG_INIT;
void nuwax_sd_write_verify_set_single_reads(bool enabled)
{
    atomic_store_explicit(&s_verify_single_reads, enabled, memory_order_release);
}
bool nuwax_sd_write_verify_single_reads(void)
{
    return atomic_load_explicit(&s_verify_single_reads, memory_order_acquire);
}
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *, void *, size_t, size_t);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *, const void *, size_t, size_t);

nuwax_sd_write_verify_stats_t nuwax_sd_write_verify_stats(void)
{
    return (nuwax_sd_write_verify_stats_t){atomic_load(&s_writes), atomic_load(&s_verified),
        atomic_load(&s_rewrites), atomic_load(&s_recovered), atomic_load(&s_failures),
        atomic_load(&s_memory_denied), atomic_load(&s_max_us),
        atomic_load(&s_reserve_uses), atomic_load(&s_single_recoveries)};
}

typedef struct { size_t sector; uint32_t expected, observed; bool valid; } verify_difference_t;

static esp_err_t verify_native_write(sdmmc_card_t *card, const uint8_t *source,
    size_t sector, size_t count, bool single, uint8_t *bounce, int64_t started)
{
    for (size_t at = 0U; at < count;) {
        if (esp_timer_get_time() - started >= VERIFY_BUDGET_US) { errno = ETIMEDOUT; return ESP_ERR_TIMEOUT; }
        const size_t blocks = single || bounce ? 1U : nuwax_sd_native_chunk(sector + at, count - at);
        const uint8_t *data = source + at * SECTOR_BYTES;
        if (bounce) { memcpy(bounce, data, SECTOR_BYTES); data = bounce; }
        const esp_err_t err = __real_sdmmc_write_sectors(card, data, sector + at, blocks);
        if (err != ESP_OK) return err;
        at += blocks;
    }
    return ESP_OK;
}

static esp_err_t verify_native_read(sdmmc_card_t *card, const uint8_t *source,
    uint8_t *readback, size_t sector, size_t count, size_t capacity, bool single,
    int64_t started, verify_difference_t *difference)
{
    bool mismatch = false;
    for (size_t at = 0U; at < count;) {
        if (esp_timer_get_time() - started >= VERIFY_BUDGET_US) { errno = ETIMEDOUT; return ESP_ERR_TIMEOUT; }
        size_t blocks = single ? 1U : nuwax_sd_native_chunk(sector + at, count - at);
        if (blocks > capacity / SECTOR_BYTES) blocks = capacity / SECTOR_BYTES;
        /* The complement cannot equal the expected bytes even for an all-a7
         * audio sector. A successful SDK return without filling DMA storage
         * must never certify a stale/poisoned buffer. */
        for (size_t i = 0U; i < blocks * SECTOR_BYTES; ++i)
            readback[i] = (uint8_t)~source[at * SECTOR_BYTES + i];
        const esp_err_t err = __real_sdmmc_read_sectors(card, readback, sector + at, blocks);
        if (err != ESP_OK) return err;
        for (size_t n = 0U; n < blocks; ++n) {
            const uint8_t *expected = source + (at + n) * SECTOR_BYTES;
            const uint8_t *observed = readback + n * SECTOR_BYTES;
            if (memcmp(expected, observed, SECTOR_BYTES)) {
                mismatch = true;
                if (!difference->valid) {
                    *difference = (verify_difference_t){sector + at + n,
                        esp_rom_crc32_le(0U, expected, SECTOR_BYTES),
                        esp_rom_crc32_le(0U, observed, SECTOR_BYTES), true};
                }
            }
        }
        at += blocks;
    }
    if (mismatch) { errno = EILSEQ; return ESP_ERR_INVALID_CRC; }
    return ESP_OK;
}

esp_err_t nuwax_sd_write_verified(void *opaque, const void *source, size_t sector, size_t count)
{
    sdmmc_card_t *card = opaque;
    /* Leave SDK validation for unsupported/malformed requests unchanged. */
    if (!card || !source || !count || card->csd.sector_size != SECTOR_BYTES ||
        count > UINT32_MAX / SECTOR_BYTES || sector > card->csd.capacity || count > card->csd.capacity - sector)
        return __real_sdmmc_write_sectors(card, source, sector, count);
    const int64_t started = esp_timer_get_time();
    const int caller_errno = errno;
    size_t capacity = atomic_load_explicit(&s_verify_single_reads, memory_order_acquire)
        ? SECTOR_BYTES : READ_BUFFER_MAX;
    uint8_t *readback = NULL;
    while (capacity >= SECTOR_BYTES) {
        if (capacity <= count * SECTOR_BYTES &&
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= nuwax_sd_write_internal_floor() + capacity) {
            readback = heap_caps_malloc(capacity, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (readback && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < nuwax_sd_write_internal_floor()) {
                heap_caps_free(readback); readback = NULL;
            }
            if (readback) break;
        }
        capacity /= 2U;
    }
    bool reserve = false;
    if (!readback) {
        atomic_fetch_add(&s_memory_denied, 1U);
        if (atomic_flag_test_and_set_explicit(&s_verify_reserve_owner, memory_order_acquire)) {
            errno = ENOMEM; return ESP_ERR_NO_MEM;
        }
        reserve = true; capacity = SECTOR_BYTES; readback = s_verify_reserve;
        atomic_fetch_add(&s_reserve_uses, 1U);
    }
    atomic_fetch_add(&s_writes, 1U);
    const size_t bytes = count * SECTOR_BYTES;
    const uint32_t source_crc = esp_rom_crc32_le(0U, source, (uint32_t)bytes);
    esp_err_t result = ESP_OK;
    int result_errno = errno;
    verify_difference_t difference = {0};
    bool independent_recovered = false;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        if (esp_timer_get_time() - started >= VERIFY_BUDGET_US) { result = ESP_ERR_TIMEOUT; result_errno = ETIMEDOUT; break; }
        if (esp_rom_crc32_le(0U, source, (uint32_t)bytes) != source_crc) { result = ESP_ERR_INVALID_CRC; result_errno = EILSEQ; break; }
        if (attempt) atomic_fetch_add(&s_rewrites, 1U);
        errno = caller_errno;
        result = verify_native_write(card, source, sector, count, attempt != 0U,
            reserve ? s_verify_reserve : NULL, started);
        if (result == ESP_ERR_NO_MEM && !reserve) {
            /* SDK bounce allocation also happens before its first data write.
             * Retrying the exact source range is idempotent, including any
             * already completed boundary slice. Preserve its original CRC. */
            heap_caps_free(readback); readback = NULL;
            atomic_fetch_add(&s_memory_denied, 1U);
            if (!atomic_flag_test_and_set_explicit(&s_verify_reserve_owner, memory_order_acquire)) {
                reserve = true; capacity = SECTOR_BYTES; readback = s_verify_reserve;
                atomic_fetch_add(&s_reserve_uses, 1U);
                if (esp_rom_crc32_le(0U, source, (uint32_t)bytes) != source_crc) {
                    result = ESP_ERR_INVALID_CRC; errno = EILSEQ;
                } else result = verify_native_write(card, source, sector, count, true, s_verify_reserve, started);
            }
        }
        result_errno = errno;
        if (result != ESP_OK) break; /* Preserve native failures; never certify them. */
        result = verify_native_read(card, source, readback, sector, count, capacity,
            attempt != 0U || nuwax_sd_write_verify_single_reads(), started, &difference);
        if (result == ESP_ERR_INVALID_CRC && attempt == 0U && !nuwax_sd_write_verify_single_reads()) {
            /* A distinct read must match every original byte before a read
             * anomaly can enable compatibility mode. Never rewrite first. */
            result = verify_native_read(card, source, readback, sector, count, capacity,
                true, started, &difference);
            independent_recovered = result == ESP_OK;
        }
        if (result != ESP_OK) result_errno = errno;
        if (esp_rom_crc32_le(0U, source, (uint32_t)bytes) != source_crc) { result = ESP_ERR_INVALID_CRC; result_errno = EILSEQ; break; }
        if (result == ESP_OK) {
            atomic_fetch_add(&s_verified, (unsigned)count);
            if (attempt || independent_recovered) atomic_fetch_add(&s_recovered, 1U);
            if (independent_recovered) {
                nuwax_sd_write_verify_set_single_reads(true);
                atomic_fetch_add(&s_single_recoveries, 1U);
            }
            break;
        }
        if (result != ESP_ERR_INVALID_CRC) break;
    }
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    unsigned maximum = atomic_load(&s_max_us);
    while (maximum < elapsed && !atomic_compare_exchange_weak(&s_max_us, &maximum, elapsed)) {}
    if (result != ESP_OK) atomic_fetch_add(&s_failures, 1U);
    if (reserve) atomic_flag_clear_explicit(&s_verify_reserve_owner, memory_order_release);
    else heap_caps_free(readback);
    if (difference.valid || result == ESP_ERR_INVALID_CRC) {
        const unsigned now = (unsigned)(esp_timer_get_time() / 1000);
        unsigned last = atomic_load(&s_log_ms);
        if ((!last || (uint32_t)(now-last) >= 5000U) && atomic_compare_exchange_strong(&s_log_ms, &last, now ? now : 1U)) {
            ESP_LOGW("sd_write_verify", "SD verified write revision=218 sector=%lu count=%lu first_bad=%lu source_crc=%08lx expected_sector_crc=%08lx read_sector_crc=%08lx recovered=%u independent=%u reserve=%u boundary_sectors=64 elapsed_us=%lu err=%s",
                (unsigned long)sector, (unsigned long)count, (unsigned long)difference.sector,
                (unsigned long)source_crc, (unsigned long)difference.expected, (unsigned long)difference.observed,
                difference.valid && result == ESP_OK, independent_recovered, reserve, (unsigned long)elapsed, esp_err_to_name(result));
        }
    }
    errno = result_errno;
    return result;
}
