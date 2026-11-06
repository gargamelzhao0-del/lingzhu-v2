#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "board/sd_dma_bulk.h"
#include "board/sd_io_memory_budget.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE
#include "board/sd_write_verify.h"
#endif
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
#include "board/sd_integrity_io.h"
#endif
#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#include "esp_attr.h"
#define SD_READ_DMA DMA_ATTR
#else
#define SD_READ_DMA
#endif

/* Project-local linker adapters. The SDK's PSRAM fallback issues one command
 * per sector. Borrow a bounded internal buffer only for this operation; never
 * reserve scarce internal heap across idle/recording/OTA transitions. All card
 * validation, command ordering, error handling and mount ownership stay in IDF. */
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *, void *, size_t, size_t);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *, const void *, size_t, size_t);

static atomic_uint_fast32_t s_bulk_commands, s_bulk_sectors, s_fallback_sectors;
static atomic_uint_fast32_t s_command_us, s_copy_us, s_allocation_us;
static atomic_uint_fast32_t s_native_calls, s_native_sectors;
static atomic_uint_fast32_t s_read_calls, s_read_sectors, s_read_us, s_read_errors;
static atomic_uint_fast32_t s_read_le2ms, s_read_le5ms, s_read_le10ms, s_read_le20ms, s_read_gt20ms;
static atomic_uintptr_t s_recovery_reader;
static atomic_bool s_single_reads;
static int64_t s_recovery_deadline;
#ifndef SD_READ_DMA
#define SD_READ_DMA
#endif
static SD_READ_DMA _Alignas(64) uint8_t s_read_reserve[512];
static atomic_flag s_read_reserve_owner = ATOMIC_FLAG_INIT;

bool nuwax_sd_bulk_read_is_degraded(void)
{
    return atomic_load_explicit(&s_single_reads, memory_order_acquire)
#if CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE
        || nuwax_sd_write_verify_single_reads()
#endif
        ;
}

bool nuwax_sd_bulk_read_recovery_begin(uint32_t timeout_ms)
{
    if (!timeout_ms) return false;
    uintptr_t vacant = 0U;
    if (!atomic_compare_exchange_strong_explicit(&s_recovery_reader, &vacant,
            (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_acq_rel, memory_order_acquire)) return false;
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    if (!nuwax_sd_integrity_io_budget_begin(timeout_ms)) {
        atomic_store_explicit(&s_recovery_reader, 0U, memory_order_release);
        return false;
    }
#endif
    s_recovery_deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    return true;
}

void nuwax_sd_bulk_read_recovery_end(bool validated_original)
{
    if (atomic_load_explicit(&s_recovery_reader, memory_order_acquire) !=
            (uintptr_t)xTaskGetCurrentTaskHandle()) return;
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    nuwax_sd_integrity_io_budget_end();
#endif
    if (validated_original) {
#if CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE
        nuwax_sd_write_verify_set_single_reads(true);
#endif
        atomic_store_explicit(&s_single_reads, true, memory_order_release);
    }
    atomic_store_explicit(&s_recovery_reader, 0U, memory_order_release);
}
nuwax_sd_bulk_stats_t nuwax_sd_bulk_stats(void)
{
    return (nuwax_sd_bulk_stats_t){
        atomic_load_explicit(&s_bulk_commands, memory_order_relaxed),
        atomic_load_explicit(&s_bulk_sectors, memory_order_relaxed),
        atomic_load_explicit(&s_fallback_sectors, memory_order_relaxed),
        atomic_load_explicit(&s_command_us, memory_order_relaxed),
        atomic_load_explicit(&s_copy_us, memory_order_relaxed),
        atomic_load_explicit(&s_allocation_us, memory_order_relaxed),
        atomic_load_explicit(&s_native_calls, memory_order_relaxed),
        atomic_load_explicit(&s_native_sectors, memory_order_relaxed),
        atomic_load_explicit(&s_read_calls, memory_order_relaxed),
        atomic_load_explicit(&s_read_sectors, memory_order_relaxed),
        atomic_load_explicit(&s_read_us, memory_order_relaxed),
        atomic_load_explicit(&s_read_errors, memory_order_relaxed),
        atomic_load_explicit(&s_read_le2ms, memory_order_relaxed),
        atomic_load_explicit(&s_read_le5ms, memory_order_relaxed),
        atomic_load_explicit(&s_read_le10ms, memory_order_relaxed),
        atomic_load_explicit(&s_read_le20ms, memory_order_relaxed),
        atomic_load_explicit(&s_read_gt20ms, memory_order_relaxed)};
}

static atomic_uintptr_t s_probe_owner;
static nuwax_sd_io_probe_t *s_probe;

bool nuwax_sd_io_probe_begin(nuwax_sd_io_probe_t *probe)
{
    if (!probe) return false;
    uintptr_t vacant = 0U;
    if (!atomic_compare_exchange_strong_explicit(&s_probe_owner, &vacant,
            (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_acq_rel, memory_order_acquire)) return false;
    memset(probe, 0, sizeof(*probe));
    s_probe = probe;
    return true;
}

void nuwax_sd_io_probe_end(void)
{
    if (atomic_load_explicit(&s_probe_owner, memory_order_acquire) !=
            (uintptr_t)xTaskGetCurrentTaskHandle()) return;
    s_probe = NULL;
    atomic_store_explicit(&s_probe_owner, 0U, memory_order_release);
}

static uint32_t probe_crc(const void *data, size_t bytes)
{
#ifdef ESP_PLATFORM
    return esp_rom_crc32_le(0U, data, (uint32_t)bytes);
#else
    uint32_t crc = ~0U; const uint8_t *p = data;
    while (bytes--) {
        crc ^= *p++;
        for (unsigned b = 0U; b < 8U; ++b) crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return ~crc;
#endif
}

static void probe_before_write(nuwax_sd_io_probe_t *probe, const uint8_t *data,
    size_t sector, size_t count, size_t block)
{
    ++probe->writes;
    /* Keep initial sectors instead of a rolling tail: the qualification
     * stops at its first bad block, so a tail-only witness could miss it. */
    for (size_t at = 0U; at < count; ++at) {
        nuwax_sd_io_sector_t *entry = NULL;
        for (unsigned i = 0U; i < 8U; ++i) {
            if (probe->sectors[i].valid && probe->sectors[i].sector == sector + at) {
                entry = &probe->sectors[i]; break;
            }
            if (!probe->sectors[i].valid && !entry) entry = &probe->sectors[i];
        }
        if (!entry) continue;
        *entry = (nuwax_sd_io_sector_t){.sector = sector + at,
            .crc = probe_crc(data + at * block, block), .valid = true};
        ++probe->write_witnesses;
    }
}

static void probe_after_read(nuwax_sd_io_probe_t *probe, const uint8_t *data,
    size_t sector, size_t count, size_t block)
{
    ++probe->reads;
    for (unsigned i = 0U; i < 8U; ++i) {
        const nuwax_sd_io_sector_t *entry = &probe->sectors[i];
        if (!entry->valid || entry->sector < sector || entry->sector - sector >= count) continue;
        const uint32_t crc = probe_crc(data + (size_t)(entry->sector - sector) * block, block);
        if (crc == entry->crc) ++probe->read_matches;
        else {
            if (!probe->read_mismatches) {
                probe->first_bad_sector = entry->sector;
                probe->first_expected_crc = entry->crc;
                probe->first_read_crc = crc;
            }
            ++probe->read_mismatches;
        }
    }
}

static esp_err_t sd_bulk_command(sdmmc_card_t *card, void *buffer,
                                 size_t sector, size_t count, bool write)
{
    const uintptr_t owner = atomic_load_explicit(&s_probe_owner, memory_order_acquire);
    nuwax_sd_io_probe_t *probe = owner && owner ==
        (uintptr_t)xTaskGetCurrentTaskHandle() ? s_probe : NULL;
    const size_t block = card ? card->csd.sector_size : 0U;
    if (probe && (!block || !buffer || count > SIZE_MAX / block ||
        sector > card->csd.capacity || count > card->csd.capacity - sector)) probe = NULL;
    if (probe && write) probe_before_write(probe, buffer, sector, count, block);
    const int64_t started = esp_timer_get_time();
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    const esp_err_t err = nuwax_sd_integrity_io_transfer(card, buffer, sector, count, write);
#elif CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE
    const esp_err_t err = write ? nuwax_sd_write_verified(card, buffer, sector, count)
                               : __real_sdmmc_read_sectors(card, buffer, sector, count);
#else
    const esp_err_t err = write ? __real_sdmmc_write_sectors(card, buffer, sector, count)
                               : __real_sdmmc_read_sectors(card, buffer, sector, count);
#endif
    if (probe && err != ESP_OK) {
        ++probe->sdk_errors;
        if (write) for (unsigned i = 0U; i < 8U; ++i) {
            nuwax_sd_io_sector_t *entry = &probe->sectors[i];
            if (entry->valid && entry->sector >= sector && entry->sector - sector < count)
                entry->valid = false; /* An unsuccessful write may have changed a prefix. */
        }
    } else if (probe && !write) probe_after_read(probe, buffer, sector, count, block);
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    atomic_fetch_add_explicit(&s_command_us, elapsed, memory_order_relaxed);
    if (!write) {
        atomic_fetch_add_explicit(&s_read_calls, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_read_sectors, count, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_read_us, elapsed, memory_order_relaxed);
        if (err != ESP_OK) atomic_fetch_add_explicit(&s_read_errors, 1U, memory_order_relaxed);
        atomic_uint_fast32_t *bucket = elapsed <= 2000U ? &s_read_le2ms :
            elapsed <= 5000U ? &s_read_le5ms : elapsed <= 10000U ? &s_read_le10ms :
            elapsed <= 20000U ? &s_read_le20ms : &s_read_gt20ms;
        atomic_fetch_add_explicit(bucket, 1U, memory_order_relaxed);
    }
    return err;
}

static esp_err_t sd_bulk_single_read(sdmmc_card_t *card, void *buffer, size_t sector,
    size_t count, bool recovery_owner)
{
    bool reserved = false;
    uint8_t *scratch = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
            NUWAX_SD_INTERNAL_RESERVE + 512U + NUWAX_SD_ALLOC_ALLOWANCE) {
        scratch = heap_caps_malloc(512U, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (scratch && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < NUWAX_SD_INTERNAL_RESERVE) {
            heap_caps_free(scratch); scratch = NULL;
        }
    }
    if (!scratch) {
        if (atomic_flag_test_and_set_explicit(&s_read_reserve_owner, memory_order_acquire)) return ESP_ERR_NO_MEM;
        scratch = s_read_reserve; reserved = true;
    }
    esp_err_t err = ESP_OK;
    for (size_t at = 0U; at < count && err == ESP_OK; ++at) {
        if (recovery_owner && esp_timer_get_time() >= s_recovery_deadline) { err = ESP_ERR_TIMEOUT; break; }
        memset(scratch, 0xa7, 512U);
        err = sd_bulk_command(card, scratch, sector + at, 1U, false);
        if (err == ESP_OK) memcpy((uint8_t *)buffer + at * 512U, scratch, 512U);
    }
    if (reserved) atomic_flag_clear_explicit(&s_read_reserve_owner, memory_order_release);
    else heap_caps_free(scratch);
    return err;
}

static esp_err_t sd_bulk_native_range(sdmmc_card_t *card, void *buffer,
    size_t sector, size_t count, bool write)
{
    if (!card || !buffer || !count || card->csd.sector_size != 512U ||
        count > SIZE_MAX / 512U || sector > card->csd.capacity || count > card->csd.capacity - sector)
        return sd_bulk_command(card, buffer, sector, count, write);
    uint8_t *cursor = buffer;
    while (count) {
        const size_t blocks = nuwax_sd_native_chunk(sector, count);
        const esp_err_t err = sd_bulk_command(card, cursor, sector, blocks, write);
        if (err == ESP_ERR_NO_MEM && !write)
            return sd_bulk_single_read(card, cursor, sector, count, false);
        if (err != ESP_OK) return err;
        sector += blocks; count -= blocks; cursor += blocks * 512U;
    }
    return ESP_OK;
}

static esp_err_t sd_bulk_transfer(sdmmc_card_t *card, void *buffer,
                                  size_t sector, size_t count, bool write)
{
    const bool recovery_owner = atomic_load_explicit(&s_recovery_reader, memory_order_acquire) ==
        (uintptr_t)xTaskGetCurrentTaskHandle();
    if (!write && card && buffer && count && card->csd.sector_size == 512U &&
        (recovery_owner || nuwax_sd_bulk_read_is_degraded())) {
        if (count > SIZE_MAX / 512U || sector > card->csd.capacity || count > card->csd.capacity - sector)
            return ESP_ERR_INVALID_SIZE;
        return sd_bulk_single_read(card, buffer, sector, count, recovery_owner);
    }
    if (!card || !buffer || !count || !esp_ptr_external_ram(buffer)) {
        if (card && buffer && count) {
            atomic_fetch_add_explicit(&s_native_calls, 1U, memory_order_relaxed);
            atomic_fetch_add_explicit(&s_native_sectors, count, memory_order_relaxed);
        }
        return sd_bulk_native_range(card, buffer, sector, count, write);
    }
    const size_t block = card->csd.sector_size;
    if (!block || count > SIZE_MAX / block) return ESP_ERR_INVALID_SIZE;
    if (sector > card->csd.capacity || count > card->csd.capacity - sector)
        return ESP_ERR_INVALID_SIZE;
    size_t bytes = 16384U;
#if CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE
    const bool verified_write = write;
#else
    const bool verified_write = false;
#endif
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    while (bytes >= block * 2U && (bytes > count * block ||
           free_internal < bytes + nuwax_sd_bulk_internal_floor(bytes, verified_write))) bytes /= 2U;
    void *dma = NULL;
    const int64_t allocation_started = esp_timer_get_time();
    while (bytes >= block * 2U && !dma) {
        dma = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (dma && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
                nuwax_sd_bulk_internal_floor(bytes, verified_write)) {
            heap_caps_free(dma);
            dma = NULL;
        }
        if (!dma) bytes /= 2U;
    }
    atomic_fetch_add_explicit(&s_allocation_us, (uint32_t)(esp_timer_get_time() - allocation_started), memory_order_relaxed);
    if (!dma) {
        atomic_fetch_add_explicit(&s_fallback_sectors, count, memory_order_relaxed);
        return sd_bulk_native_range(card, buffer, sector, count, write);
    }
    const size_t capacity = bytes / block;
    esp_err_t err = ESP_OK;
    uint8_t *cursor = buffer;
    while (count && err == ESP_OK) {
        const size_t remaining = count < capacity ? count : capacity;
        const size_t blocks = nuwax_sd_native_chunk(sector, remaining);
        atomic_fetch_add_explicit(&s_bulk_commands, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_bulk_sectors, blocks, memory_order_relaxed);
        int64_t copy_started = esp_timer_get_time();
        if (write) {
            memcpy(dma, cursor, blocks * block);
            atomic_fetch_add_explicit(&s_copy_us, (uint32_t)(esp_timer_get_time() - copy_started), memory_order_relaxed);
        }
        err = sd_bulk_command(card, dma, sector, blocks, write);
        if (err == ESP_ERR_NO_MEM && !write)
            err = sd_bulk_single_read(card, dma, sector, blocks, false);
        if (err != ESP_OK) break;
        if (!write) {
            copy_started = esp_timer_get_time();
            memcpy(cursor, dma, blocks * block);
            atomic_fetch_add_explicit(&s_copy_us, (uint32_t)(esp_timer_get_time() - copy_started), memory_order_relaxed);
        }
        cursor += blocks * block; sector += blocks; count -= blocks;
    }
    heap_caps_free(dma);
    return err;
}

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t sector, size_t count)
{
    return sd_bulk_transfer(card, dst, sector, count, false);
}

esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t sector, size_t count)
{
    return sd_bulk_transfer(card, (void *)src, sector, count, true);
}
