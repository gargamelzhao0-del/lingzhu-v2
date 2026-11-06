#include "board/sd_integrity_io.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE
#include "board/sd_write_verify.h"
#endif
#include "sdmmc_cmd.h"
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
#include "board/sd_command_history.h"
#endif
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
#include "esp_private/sdmmc_common.h"
#endif

enum { IO_SLOTS = 256, IO_PROBES = 8, IO_SECTOR = 512, RAW_BUDGET_MS = 200 };
typedef struct {
    uint64_t lba;
    uint32_t generation, crc, operation, valid;
} io_sector_t;
typedef struct {
    io_sector_t sectors[IO_SLOTS];
    nuwax_sd_integrity_io_report_t report;
    uint32_t generation, operation;
    uintptr_t owner;
    const sdmmc_card_t *card;
    bool armed, reading, exhausted;
    atomic_uintptr_t budget_owner;
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
    atomic_uintptr_t repair_owner;
    const sdmmc_card_t *repair_card;
    esp_err_t repair_error;
#endif
    int64_t deadline;
    esp_err_t (*transaction)(int, sdmmc_command_t *);
    sdmmc_card_t copy;
} io_state_t;
static io_state_t *s_io;
static atomic_bool s_visible;
static portMUX_TYPE s_io_lock = portMUX_INITIALIZER_UNLOCKED;
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *, void *, size_t, size_t);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *, const void *, size_t, size_t);

bool nuwax_sd_integrity_io_init(void)
{
    if (atomic_load_explicit(&s_visible, memory_order_acquire)) return true;
    /* Initialization owner is the existing serialized spool init owner. */
    io_state_t *state = heap_caps_calloc(1U, sizeof(*state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!state) return false;
    atomic_init(&state->budget_owner, 0U);
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
    atomic_init(&state->repair_owner, 0U);
#endif
    s_io = state;
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
    (void)nuwax_sd_command_history_init(); /* Optional evidence never blocks capture. */
#endif
    atomic_store_explicit(&s_visible, true, memory_order_release);
    return true;
}

void nuwax_sd_integrity_io_arm(void)
{
    if (!atomic_load_explicit(&s_visible, memory_order_acquire)) return;
    uint32_t generation = 0U;
    portENTER_CRITICAL(&s_io_lock);
    /* Generation tags avoid clearing a PSRAM table with interrupts disabled. */
    if (s_io->exhausted || ++s_io->generation == 0U) {
        s_io->armed = false; s_io->exhausted = true;
    } else {
        memset(&s_io->report, 0, sizeof(s_io->report));
        s_io->owner = (uintptr_t)xTaskGetCurrentTaskHandle();
        s_io->card = NULL; s_io->reading = false; s_io->armed = true;
        generation = s_io->generation;
    }
    portEXIT_CRITICAL(&s_io_lock);
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
    if (generation) nuwax_sd_command_history_arm(generation);
#else
    (void)generation;
#endif
}

void nuwax_sd_integrity_io_read_phase(bool reading)
{
    if (!atomic_load_explicit(&s_visible, memory_order_acquire)) return;
    portENTER_CRITICAL(&s_io_lock);
    if (s_io->owner == (uintptr_t)xTaskGetCurrentTaskHandle()) s_io->reading = reading;
    portEXIT_CRITICAL(&s_io_lock);
}

void nuwax_sd_integrity_io_freeze(nuwax_sd_integrity_io_report_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!atomic_load_explicit(&s_visible, memory_order_acquire)) return;
    portENTER_CRITICAL(&s_io_lock);
    if (s_io->owner == (uintptr_t)xTaskGetCurrentTaskHandle()) {
        s_io->armed = false; s_io->reading = false;
        if (out) *out = s_io->report;
    }
    portEXIT_CRITICAL(&s_io_lock);
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
    nuwax_sd_command_history_freeze();
#endif
}

static io_sector_t *sector_entry(uint64_t lba, bool create)
{
    const unsigned first = ((uint32_t)lba * 2654435761U ^ (uint32_t)(lba >> 32U)) & (IO_SLOTS - 1U);
    for (unsigned n = 0U; n < IO_PROBES; ++n) {
        io_sector_t *entry = &s_io->sectors[(first + n) & (IO_SLOTS - 1U)];
        if (entry->generation != s_io->generation) {
            if (!create) return NULL;
            *entry = (io_sector_t){.lba = lba, .generation = s_io->generation};
            return entry;
        }
        if (entry->lba == lba) return entry;
    }
    if (!create) return NULL;
    ++s_io->report.evictions;
    io_sector_t *entry = &s_io->sectors[first];
    *entry = (io_sector_t){.lba = lba, .generation = s_io->generation};
    return entry;
}

static esp_err_t bounded_transaction(int slot, sdmmc_command_t *command)
{
    if (!s_io || s_io->budget_owner != (uintptr_t)xTaskGetCurrentTaskHandle() || !s_io->transaction)
        return ESP_ERR_INVALID_STATE;
    const int64_t remaining = s_io->deadline - esp_timer_get_time();
    if (remaining < 1000) return ESP_ERR_TIMEOUT;
    const uint32_t ms = (uint32_t)(remaining / 1000);
    if (!command->timeout_ms || command->timeout_ms > ms) command->timeout_ms = ms;
    return s_io->transaction(slot, command);
}

bool nuwax_sd_integrity_io_budget_begin(uint32_t timeout_ms)
{
    if (!timeout_ms || !atomic_load_explicit(&s_visible, memory_order_acquire)) return false;
    bool available = false;
    portENTER_CRITICAL(&s_io_lock);
    if (!s_io->budget_owner
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
        && !s_io->repair_owner
#endif
    ) {
        s_io->budget_owner = (uintptr_t)xTaskGetCurrentTaskHandle();
        s_io->deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        available = true;
    }
    portEXIT_CRITICAL(&s_io_lock);
    return available;
}

void nuwax_sd_integrity_io_budget_end(void)
{
    if (!atomic_load_explicit(&s_visible, memory_order_acquire)) return;
    portENTER_CRITICAL(&s_io_lock);
    if (s_io->budget_owner == (uintptr_t)xTaskGetCurrentTaskHandle()
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
        && s_io->repair_owner != (uintptr_t)xTaskGetCurrentTaskHandle()
#endif
        ) {
        s_io->budget_owner = 0U; s_io->transaction = NULL;
    }
    portEXIT_CRITICAL(&s_io_lock);
}

#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
bool nuwax_sd_integrity_io_repair_begin(void)
{
    if (!atomic_load_explicit(&s_visible, memory_order_acquire)) return false;
    bool available = false;
    portENTER_CRITICAL(&s_io_lock);
    const uintptr_t task = (uintptr_t)xTaskGetCurrentTaskHandle();
    if (!s_io->repair_owner && !s_io->budget_owner && s_io->armed &&
        s_io->owner == task && s_io->card) {
        s_io->repair_card = s_io->card;
        s_io->repair_error = ESP_OK;
        s_io->deadline = esp_timer_get_time() + (int64_t)NUWAX_SD_REPAIR_BUDGET_MS * 1000LL;
        atomic_store_explicit(&s_io->budget_owner, (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_release);
        atomic_store_explicit(&s_io->repair_owner, task, memory_order_release);
        available = true;
    }
    portEXIT_CRITICAL(&s_io_lock);
    return available;
}

void nuwax_sd_integrity_io_repair_abort(void)
{
    if (s_io && atomic_load_explicit(&s_io->repair_owner, memory_order_acquire) ==
        (uintptr_t)xTaskGetCurrentTaskHandle()) s_io->repair_error = ESP_ERR_INVALID_STATE;
}

void nuwax_sd_integrity_io_repair_end(void)
{
    if (!atomic_load_explicit(&s_visible, memory_order_acquire)) return;
    portENTER_CRITICAL(&s_io_lock);
    if (s_io->repair_owner == (uintptr_t)xTaskGetCurrentTaskHandle()) {
        atomic_store_explicit(&s_io->budget_owner, 0U, memory_order_release);
        atomic_store_explicit(&s_io->repair_owner, 0U, memory_order_release);
        s_io->repair_card = NULL;
    }
    portEXIT_CRITICAL(&s_io_lock);
}
#endif

static esp_err_t original_transfer(sdmmc_card_t *card, void *buffer, size_t sector, size_t count, bool write)
{
    sdmmc_card_t *used = card;
    if (card && s_io && s_io->budget_owner == (uintptr_t)xTaskGetCurrentTaskHandle()) {
        if (esp_timer_get_time() + 1000 > s_io->deadline) return ESP_ERR_TIMEOUT;
        s_io->copy = *card;
        s_io->transaction = card->host.do_transaction;
        if (!s_io->transaction) return ESP_ERR_INVALID_STATE;
        s_io->copy.host.do_transaction = bounded_transaction;
        s_io->copy.host.command_timeout_ms = 100U;
        used = &s_io->copy;
    }
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
    const bool repair = s_io && write &&
        atomic_load_explicit(&s_io->repair_owner, memory_order_acquire) == (uintptr_t)xTaskGetCurrentTaskHandle();
    if (repair) {
        if (s_io->repair_error != ESP_OK) return s_io->repair_error;
        if (card != s_io->repair_card) return s_io->repair_error = ESP_ERR_INVALID_STATE;
        /* The mounted descriptor is a cached identity. A fresh native status
         * command also rejects an absent/uninitialized replacement before a
         * repair write; no SPI-only CID helper or remount under open FILEs. */
        uint32_t status = 0U;
        const esp_err_t ready = sdmmc_send_cmd_send_status(used, &status);
        if (ready != ESP_OK || !sdmmc_ready_for_data(status))
            return s_io->repair_error = ready != ESP_OK ? ready : ESP_ERR_INVALID_STATE;
    }
#endif
#if CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE
    const esp_err_t result = write ? nuwax_sd_write_verified(used, buffer, sector, count)
#else
    const esp_err_t result = write ? __real_sdmmc_write_sectors(used, buffer, sector, count)
#endif
                 : __real_sdmmc_read_sectors(used, buffer, sector, count);
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
    if (repair && result != ESP_OK) s_io->repair_error = result;
#endif
    return result;
}

/* Runs synchronously inside the caller's FatFs volume lock. A private internal
 * DMA buffer avoids reusing either the FILE cache or its PSRAM bounce target.
 * This evidence never changes the original result or caller's payload. */
static void raw_comparison(sdmmc_card_t *card, uint64_t lba, uint32_t generation)
{
    const int saved_errno = errno;
    esp_err_t result = ESP_ERR_NO_MEM;
    uint32_t crc = 0U;
    uint8_t *buffer = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 16384U + IO_SECTOR)
        buffer = heap_caps_malloc(IO_SECTOR, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buffer && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 16384U) {
        heap_caps_free(buffer); buffer = NULL;
    }
    if (buffer) {
        if (nuwax_sd_integrity_io_budget_begin(RAW_BUDGET_MS)) {
            memset(buffer, 0xa7, IO_SECTOR);
            result = original_transfer(card, buffer, (size_t)lba, 1U, false);
            if (result == ESP_OK) crc = esp_rom_crc32_le(0U, buffer, IO_SECTOR);
            nuwax_sd_integrity_io_budget_end();
        } else result = ESP_ERR_INVALID_STATE;
        heap_caps_free(buffer);
    }
    portENTER_CRITICAL(&s_io_lock);
    if (s_io->generation == generation) {
        s_io->report.raw_error = result; s_io->report.raw_crc = crc;
        s_io->report.raw_valid = result == ESP_OK;
    }
    portEXIT_CRITICAL(&s_io_lock);
    errno = saved_errno;
}

esp_err_t nuwax_sd_integrity_io_transfer(void *opaque, void *buffer, size_t sector, size_t count, bool write)
{
    sdmmc_card_t *card = opaque;
    if (!atomic_load_explicit(&s_visible, memory_order_acquire))
        return write ? __real_sdmmc_write_sectors(card, buffer, sector, count)
                     : __real_sdmmc_read_sectors(card, buffer, sector, count);
    uint32_t generation = 0U, operation = 0U;
    bool observe = false, read_phase = false;
    const uintptr_t task = (uintptr_t)xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_io_lock);
    if (s_io->armed && card && buffer && count && card->csd.sector_size == IO_SECTOR &&
        count <= SIZE_MAX / IO_SECTOR && sector <= card->csd.capacity && count <= card->csd.capacity - sector) {
        if (!s_io->card && task == s_io->owner) s_io->card = card;
        if (s_io->card == card) {
            observe = true; generation = s_io->generation; operation = ++s_io->operation;
            read_phase = task == s_io->owner && s_io->reading;
            if (write) {
                ++s_io->report.write_calls;
                if (task != s_io->owner) ++s_io->report.foreign_writes;
            } else if (read_phase) ++s_io->report.read_calls;
        }
    }
    portEXIT_CRITICAL(&s_io_lock);
    if (observe && write) for (size_t n = 0U; n < count; ++n) {
        const uint32_t crc = esp_rom_crc32_le(0U, (uint8_t *)buffer + n * IO_SECTOR, IO_SECTOR);
        portENTER_CRITICAL(&s_io_lock);
        if (s_io->armed && s_io->generation == generation) {
            io_sector_t *entry = sector_entry(sector + n, true);
            entry->crc = crc; entry->operation = operation; entry->valid = 0U;
        }
        portEXIT_CRITICAL(&s_io_lock);
    }
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
    const bool history = observe && nuwax_sd_command_history_begin(card, &s_io->copy, buffer,
        sector, count, write, read_phase, generation, operation, task == s_io->owner);
#endif
    const esp_err_t result = original_transfer(card, buffer, sector, count, write);
    const int original_errno = errno;
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
    if (history) nuwax_sd_command_history_end();
#endif
    if (observe && result != ESP_OK) {
        portENTER_CRITICAL(&s_io_lock);
        if (s_io->armed && s_io->generation == generation) ++s_io->report.sdk_errors;
        portEXIT_CRITICAL(&s_io_lock);
    }
    if (observe && write) for (size_t n = 0U; n < count; ++n) {
        const uint32_t after = result == ESP_OK ? esp_rom_crc32_le(0U, (uint8_t *)buffer + n * IO_SECTOR, IO_SECTOR) : 0U;
        portENTER_CRITICAL(&s_io_lock);
        if (s_io->armed && s_io->generation == generation) {
            io_sector_t *entry = sector_entry(sector + n, false);
            if (entry && entry->operation == operation) {
                entry->valid = result == ESP_OK;
                if (entry->valid && entry->crc != after) ++s_io->report.source_mutations;
            }
        }
        portEXIT_CRITICAL(&s_io_lock);
    }
    if (observe && read_phase && result == ESP_OK) for (size_t n = 0U; n < count; ++n) {
        const uint32_t crc = esp_rom_crc32_le(0U, (uint8_t *)buffer + n * IO_SECTOR, IO_SECTOR);
        bool raw = false;
        portENTER_CRITICAL(&s_io_lock);
        if (s_io->armed && s_io->generation == generation) {
            io_sector_t *entry = sector_entry(sector + n, false);
            if (!entry || !entry->valid) ++s_io->report.unknown_sectors;
            else {
                ++s_io->report.covered_sectors;
                if (entry->crc == crc) ++s_io->report.matched_sectors;
                else {
                    ++s_io->report.mismatched_sectors;
                    if (!s_io->report.mismatch_seen) {
                        s_io->report.mismatch_seen = true; s_io->report.bad_lba = sector + n;
                        s_io->report.expected_crc = entry->crc; s_io->report.observed_crc = crc;
                        if (!s_io->report.raw_attempts) { ++s_io->report.raw_attempts; raw = true; }
                    }
                }
            }
        }
        portEXIT_CRITICAL(&s_io_lock);
        if (raw) {
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
            /* Keep the original write/read commands; repair and the bounded
             * raw comparison must not evict the first physical contradiction. */
            nuwax_sd_command_history_freeze();
#endif
            raw_comparison(card, sector + n, generation);
        }
    }
    errno = original_errno;
    return result;
}
