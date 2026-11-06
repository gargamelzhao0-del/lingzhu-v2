#include "board/sd_probe_trace.h"
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"

/* Qualification owns the mounted card until finish. No pointer is retained
 * after finish, and no observer from a different task may touch this scope. */
enum { FIRST_EVENTS = 8, TAIL_EVENTS = 16, DIAG_BUDGET_US = 750000 };
typedef struct {
    uint32_t opcode, argument, response, bytes;
    int result;
    bool external;
} probe_event_t;
typedef struct {
    probe_event_t command;
    uint32_t first_status, last_status;
    int first_result, last_result;
    bool status_seen;
} probe_write_t;
typedef struct {
    const sdmmc_card_t *card;
    sdmmc_card_t *copy;
    esp_err_t (*transaction)(int, sdmmc_command_t *);
    uint32_t events;
    bool foreign_card, diagnostic;
    int64_t deadline;
    probe_event_t event[FIRST_EVENTS + TAIL_EVENTS];
    probe_write_t write[2U * FIRST_EVENTS]; /* First and last eight writes. */
    uint32_t writes;
    bool write_pending;
} probe_trace_t;
static const char *TAG = "sd_probe";
static atomic_uintptr_t s_owner;
static atomic_bool s_diagnosed;
static probe_trace_t *s_trace;

esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *, void *, size_t, size_t);

static probe_trace_t *owned_trace(void)
{
    const uintptr_t owner = atomic_load_explicit(&s_owner, memory_order_acquire);
    return owner && owner == (uintptr_t)xTaskGetCurrentTaskHandle() ? s_trace : NULL;
}

bool nuwax_sd_probe_trace_active(void)
{
    return owned_trace() != NULL;
}

bool nuwax_sd_probe_trace_begin(void)
{
    if (atomic_load_explicit(&s_diagnosed, memory_order_relaxed)) return false;
    uintptr_t empty = 0U;
    if (!atomic_compare_exchange_strong_explicit(&s_owner, &empty,
            (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_acq_rel, memory_order_acquire)) return false;
    const int saved_errno = errno;
    s_trace = heap_caps_calloc(1U, sizeof(*s_trace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_trace) atomic_store_explicit(&s_owner, 0U, memory_order_release);
    errno = saved_errno;
    return s_trace != NULL;
}

void nuwax_sd_probe_trace_command(const void *card, uint32_t opcode,
    uint32_t argument, uint32_t response, int result, const void *buffer, size_t bytes)
{
    probe_trace_t *trace = owned_trace();
    if (!trace || (opcode != 13U && opcode != 17U && opcode != 18U && opcode != 24U && opcode != 25U)) return;
    if (!trace->card) trace->card = card;
    if (card != trace->card && card != trace->copy) { trace->foreign_card = true; return; }
    const uint32_t n = trace->events++;
    const unsigned index = n < FIRST_EVENTS ? n : FIRST_EVENTS + (n - FIRST_EVENTS) % TAIL_EVENTS;
    trace->event[index] = (probe_event_t){opcode, argument, response, (uint32_t)bytes,
        result, buffer && esp_ptr_external_ram(buffer)};
    if (opcode == 24U || opcode == 25U) {
        const uint32_t write = trace->writes++;
        const unsigned at = write < FIRST_EVENTS ? write : FIRST_EVENTS + (write - FIRST_EVENTS) % FIRST_EVENTS;
        trace->write[at] = (probe_write_t){.command = trace->event[index]};
        trace->write_pending = true;
    } else if (opcode == 17U || opcode == 18U) trace->write_pending = false;
    else if (opcode == 13U && trace->write_pending) {
        const uint32_t nwrite = trace->writes - 1U;
        const unsigned at = nwrite < FIRST_EVENTS ? nwrite : FIRST_EVENTS + (nwrite - FIRST_EVENTS) % FIRST_EVENTS;
        probe_write_t *write = &trace->write[at];
        if (!write->status_seen) { write->first_status = response; write->first_result = result; }
        write->status_seen = true; write->last_status = response; write->last_result = result;
    }
}

static void dump_events(const probe_trace_t *trace, const char *phase)
{
    const uint32_t first = trace->events < FIRST_EVENTS ? trace->events : FIRST_EVENTS;
    for (uint32_t n = 0U; n < trace->events; ++n) {
        if (n >= first && n + TAIL_EVENTS < trace->events) { n = trace->events - TAIL_EVENTS; }
        const unsigned index = n < FIRST_EVENTS ? n : FIRST_EVENTS + (n - FIRST_EVENTS) % TAIL_EVENTS;
        const probe_event_t *event = &trace->event[index];
        ESP_LOGW(TAG, "SD probe command revision=206 phase=%s seq=%lu op=%lu arg=%08lx r1=%08lx err=%s bytes=%lu external=%u",
            phase, (unsigned long)n, (unsigned long)event->opcode, (unsigned long)event->argument,
            (unsigned long)event->response, esp_err_to_name(event->result),
            (unsigned long)event->bytes, event->external);
    }
    for (uint32_t n = 0U; n < trace->writes; ++n) {
        if (n >= FIRST_EVENTS && n + FIRST_EVENTS < trace->writes) n = trace->writes - FIRST_EVENTS;
        const unsigned at = n < FIRST_EVENTS ? n : FIRST_EVENTS + (n - FIRST_EVENTS) % FIRST_EVENTS;
        const probe_write_t *write = &trace->write[at];
        ESP_LOGW(TAG, "SD probe write revision=206 seq=%lu op=%lu arg=%08lx bytes=%lu r1=%08lx err=%s status_seen=%u first_status=%08lx first_err=%s last_status=%08lx last_err=%s",
            (unsigned long)n, (unsigned long)write->command.opcode, (unsigned long)write->command.argument,
            (unsigned long)write->command.bytes, (unsigned long)write->command.response, esp_err_to_name(write->command.result),
            write->status_seen, (unsigned long)write->first_status, esp_err_to_name(write->first_result),
            (unsigned long)write->last_status, esp_err_to_name(write->last_result));
    }
}

/* Only the local descriptor copy uses this callback. Its total deadline also
 * bounds CMD13 polling in the original SDK; the mounted card is never edited. */
static esp_err_t bounded_transaction(int slot, sdmmc_command_t *command)
{
    probe_trace_t *trace = owned_trace();
    if (!trace || !trace->diagnostic || !trace->transaction) return ESP_ERR_INVALID_STATE;
    const int64_t remaining = trace->deadline - esp_timer_get_time();
    if (remaining < 1000) return ESP_ERR_TIMEOUT;
    const uint32_t milliseconds = (uint32_t)(remaining / 1000);
    if (!command->timeout_ms || command->timeout_ms > milliseconds) command->timeout_ms = milliseconds;
    return trace->transaction(slot, command);
}

static bool budget_left(const probe_trace_t *trace)
{
    return esp_timer_get_time() + 1000 < trace->deadline;
}

static bool untouched(const uint8_t *buffer, uint8_t poison)
{
    for (unsigned i = 0U; i < 512U; ++i) if (buffer[i] != poison) return false;
    return true;
}

static void compare_read(probe_trace_t *trace, uint8_t *buffer, size_t lba, size_t blocks,
    const char *path, uint8_t poison, uint32_t expected, uint32_t observed)
{
    if (!buffer || !budget_left(trace)) return;
    memset(buffer, poison, 512U * blocks);
    const esp_err_t err = __real_sdmmc_read_sectors(trace->copy, buffer, lba, blocks);
    const uint32_t crc = esp_rom_crc32_le(0U, buffer, 512U);
    ESP_LOGW(TAG, "SD probe comparison revision=206 path=%s lba=%llu blocks=%u err=%s crc512=%08lx expected=%08lx previous=%08lx untouched=%u",
        path, (unsigned long long)lba, (unsigned)blocks, esp_err_to_name(err), (unsigned long)crc,
        (unsigned long)expected, (unsigned long)observed, untouched(buffer, poison));
}

static bool written_pair(const probe_trace_t *trace, uint64_t lba)
{
    for (uint32_t n = 0U; n < trace->writes; ++n) {
        if (n >= FIRST_EVENTS && n + FIRST_EVENTS < trace->writes) n = trace->writes - FIRST_EVENTS;
        const unsigned at = n < FIRST_EVENTS ? n : FIRST_EVENTS + (n - FIRST_EVENTS) % FIRST_EVENTS;
        const probe_event_t *write = &trace->write[at].command;
        if (write->result != ESP_OK || write->bytes < 1024U || write->bytes % 512U) continue;
        uint64_t first = write->argument;
        if (!(trace->card->ocr & SD_OCR_SDHC_CAP)) {
            if (first % 512U) continue;
            first /= 512U;
        }
        if (lba >= first && lba - first + 1U < write->bytes / 512U) return true;
    }
    return false;
}

static unsigned little16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8U); }
static uint32_t little32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static bool read_geometry_sector(probe_trace_t *trace, uint8_t *buffer, size_t lba)
{
    if (!budget_left(trace) || lba >= trace->copy->csd.capacity) return false;
    memset(buffer, 0xa7, 512U);
    return __real_sdmmc_read_sectors(trace->copy, buffer, lba, 1U) == ESP_OK;
}

/* Read-only MBR/VBR inspection on failure; no FAT scan or cluster allocation.
 * Unrecognized/GPT/invalid BPB is reported as unknown, never used for writes. */
static void report_geometry(probe_trace_t *trace, uint8_t *buffer)
{
    if (!read_geometry_sector(trace, buffer, 0U)) return;
    uint32_t volume = 0U;
    if (buffer[510] != 0x55U || buffer[511] != 0xaaU) return;
    if (little16(buffer + 11U) != 512U || (buffer[0] != 0xebU && buffer[0] != 0xe9U)) {
        for (unsigned i = 0U; i < 4U; ++i) {
            const uint8_t *entry = buffer + 446U + 16U * i;
            if (entry[4] == 0x0bU || entry[4] == 0x0cU || entry[4] == 0x06U || entry[4] == 0x0eU) {
                volume = little32(entry + 8U); break;
            }
        }
        if (!volume || !read_geometry_sector(trace, buffer, volume)) return;
    }
    const unsigned bytes = little16(buffer + 11U), cluster = buffer[13], reserved = little16(buffer + 14U);
    const unsigned fats = buffer[16];
    const uint32_t fat16 = little16(buffer + 22U), fat = fat16 ? fat16 : little32(buffer + 36U);
    const uint32_t total16 = little16(buffer + 19U), total = total16 ? total16 : little32(buffer + 32U);
    const uint64_t metadata = (uint64_t)reserved + (uint64_t)fats * fat;
    const bool valid = buffer[510] == 0x55U && buffer[511] == 0xaaU && bytes == 512U &&
        cluster && cluster <= 128U && !(cluster & (cluster - 1U)) && reserved &&
        (fats == 1U || fats == 2U) && fat && metadata < total && (uint64_t)volume + total <= trace->copy->csd.capacity;
    if (!valid) { ESP_LOGW(TAG, "SD probe BPB revision=206 valid=0"); return; }
    const bool fat32 = !fat16 && !little16(buffer + 17U);
    const unsigned info_sector = fat32 ? little16(buffer + 48U) : 0U;
    ESP_LOGW(TAG, "SD probe BPB revision=206 valid=1 fat32=%u volume_lba=%lu sector_bytes=%u cluster_sectors=%u reserved=%u fats=%u fat_sectors=%lu total_sectors=%lu",
        fat32, (unsigned long)volume, bytes, cluster, reserved, fats, (unsigned long)fat, (unsigned long)total);
    if (info_sector && info_sector < reserved && read_geometry_sector(trace, buffer, (size_t)volume + info_sector)) {
        const bool info_valid = little32(buffer) == 0x41615252U && little32(buffer + 484U) == 0x61417272U &&
            little32(buffer + 508U) == 0xaa550000U;
        ESP_LOGW(TAG, "SD probe FSInfo revision=206 valid=%u free_hint=%lu last_hint=%lu",
            info_valid, (unsigned long)little32(buffer + 488U), (unsigned long)little32(buffer + 492U));
    }
}

static void diagnose(probe_trace_t *trace, const char *scope, int result,
    uint64_t lba, uint32_t expected, uint32_t observed)
{
    ESP_LOGW(TAG, "SD probe failure revision=206 scope=%s err=%s events=%lu foreign_card=%u lba=%llu budget_ms=750 verdict_retained=1",
        scope, esp_err_to_name(result), (unsigned long)trace->events, trace->foreign_card, (unsigned long long)lba);
    dump_events(trace, "qualification");
    if (!trace->card || trace->foreign_card || trace->card->csd.sector_size != 512U) return;
    ESP_LOGW(TAG, "SD probe card revision=206 mmc=%u mfg=%lu oem=%lu serial=%lu sectors=%lu sector_bytes=%lu freq_khz=%lu ocr=%08lx",
        trace->card->is_mmc, (unsigned long)trace->card->cid.mfg_id, (unsigned long)trace->card->cid.oem_id,
        (unsigned long)trace->card->cid.serial, (unsigned long)trace->card->csd.capacity,
        (unsigned long)trace->card->csd.sector_size, (unsigned long)trace->card->max_freq_khz, (unsigned long)trace->card->ocr);
    const bool pair = written_pair(trace, lba);
    trace->copy = heap_caps_malloc(sizeof(*trace->copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Keep the same 16 KiB internal reserve as the normal SD adapter, plus
     * room for the SDK's 512-byte PSRAM fallback. Skip optional reads first. */
    uint8_t *internal = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 16384U + 1536U) {
        internal = heap_caps_malloc(1024U, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (internal && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 16384U + 512U) {
            heap_caps_free(internal); internal = NULL;
        }
    }
    uint8_t *external = heap_caps_malloc(1024U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (trace->copy && internal && trace->card->host.do_transaction) {
        memcpy(trace->copy, trace->card, sizeof(*trace->copy));
        trace->transaction = trace->card->host.do_transaction;
        trace->copy->host.do_transaction = bounded_transaction;
        trace->copy->host.command_timeout_ms = 200U;
        trace->deadline = esp_timer_get_time() + DIAG_BUDGET_US;
        trace->diagnostic = true; trace->events = trace->writes = 0U; trace->write_pending = false;
        if (lba && lba < trace->copy->csd.capacity) {
            compare_read(trace, internal, (size_t)lba, 1U, "native_internal_1", 0xa5U, expected, observed);
            compare_read(trace, internal, (size_t)lba, 1U, "native_internal_2", 0x5aU, expected, observed);
            compare_read(trace, external, (size_t)lba, 1U, "native_psram", 0x3cU, expected, observed);
            if (pair && lba + 1U < trace->copy->csd.capacity)
                compare_read(trace, internal, (size_t)lba, 2U, "native_internal_multi", 0x96U, expected, observed);
            else ESP_LOGW(TAG, "SD probe multi read skipped: no recorded two-sector write covering this LBA");
        }
        report_geometry(trace, internal);
        trace->diagnostic = false;
        dump_events(trace, "diagnostic_read_only");
    } else ESP_LOGW(TAG, "SD probe read-only comparison unavailable; verdict retained");
    heap_caps_free(external); heap_caps_free(internal); heap_caps_free(trace->copy); trace->copy = NULL;
}

void nuwax_sd_probe_trace_finish(const char *scope, int result,
    uint64_t bad_lba, uint32_t expected_crc, uint32_t observed_crc)
{
    probe_trace_t *trace = owned_trace();
    if (!trace) return;
    const int saved_errno = errno;
    /* A partial-sector witness is evidence, not a replacement for the logical
     * qualification verdict. Still retain its native read comparison when the
     * logical check succeeds; previously that contradiction was discarded. */
    if ((result != ESP_OK || expected_crc != observed_crc) &&
        !atomic_exchange_explicit(&s_diagnosed, true, memory_order_relaxed))
        diagnose(trace, scope, result, bad_lba, expected_crc, observed_crc);
    s_trace = NULL;
    atomic_store_explicit(&s_owner, 0U, memory_order_release);
    heap_caps_free(trace);
    errno = saved_errno;
}
