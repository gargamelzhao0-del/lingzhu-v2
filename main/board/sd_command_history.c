#include "board/sd_command_history.h"
#include "sdkconfig.h"
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"

enum { HISTORY_EVENTS = 256, HISTORY_DUMP = 12, HISTORY_SECTOR = 512 };
typedef struct {
    uint64_t lba;
    uint32_t sequence, operation, opcode, argument, bytes, response;
    uint32_t data_crc, source_crc, first_status, last_status, polls;
    int result, first_error, last_error;
    bool range_valid, source_valid, writer_owner;
} command_event_t;
typedef struct {
    command_event_t events[HISTORY_EVENTS];
    command_event_t erase[3];
    bool erase_valid[3];
    uint32_t generation, count, conflicts;
    uintptr_t owner;
    const void *tracked_card;
    uintptr_t pending_owner;
    atomic_bool frozen;
    bool dumped, pending;
    struct {
        const void *card, *alias;
        const uint8_t *source;
        uint64_t sector, count;
        uint32_t generation, operation;
        bool write, independent_read, writer_owner, block_addressed;
    } scope;
} command_history_t;
static command_history_t *s_history;
static atomic_uintptr_t s_scope_owner;
static portMUX_TYPE s_history_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "sd_commands";

bool nuwax_sd_command_history_init(void)
{
    if (s_history) return true; /* Existing serialized spool initialization. */
    const int saved_errno = errno;
    s_history = heap_caps_calloc(1U, sizeof(*s_history), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_history) atomic_init(&s_history->frozen, false);
    else ESP_LOGW(TAG, "SD command history unavailable; recording guards retained");
    errno = saved_errno;
    return s_history != NULL;
}

void nuwax_sd_command_history_arm(uint32_t generation)
{
    if (!s_history) return;
    portENTER_CRITICAL(&s_history_lock);
    if (!atomic_load_explicit(&s_scope_owner, memory_order_acquire)) {
        s_history->generation = generation; s_history->count = s_history->conflicts = 0U;
        s_history->owner = (uintptr_t)xTaskGetCurrentTaskHandle();
        s_history->tracked_card = NULL; s_history->pending_owner = 0U;
        memset(s_history->erase_valid, 0, sizeof(s_history->erase_valid));
        s_history->frozen = s_history->dumped = s_history->pending = false;
    }
    portEXIT_CRITICAL(&s_history_lock);
}

bool nuwax_sd_command_history_begin(const void *opaque, const void *alias, const void *source,
    size_t sector, size_t count, bool write, bool independent_read, uint32_t generation,
    uint32_t operation, bool writer_owner)
{
    const sdmmc_card_t *card = opaque;
    if (!s_history || !card || !source || !count ||
        count > SIZE_MAX / HISTORY_SECTOR || card->csd.sector_size != HISTORY_SECTOR ||
        sector >= card->csd.capacity || count > card->csd.capacity - sector) return false;
    bool started = false;
    portENTER_CRITICAL(&s_history_lock);
    if (!s_history->frozen && s_history->generation == generation) {
        if (atomic_load_explicit(&s_scope_owner, memory_order_acquire)) ++s_history->conflicts;
        else {
            s_history->scope.card = card; s_history->scope.alias = alias;
            s_history->tracked_card = card;
            s_history->scope.source = source; s_history->scope.sector = sector;
            s_history->scope.count = count; s_history->scope.generation = generation;
            s_history->scope.operation = operation; s_history->scope.write = write;
            s_history->scope.independent_read = !write && independent_read;
            s_history->scope.writer_owner = writer_owner;
            s_history->scope.block_addressed = (card->ocr & SD_OCR_SDHC_CAP) != 0U;
            s_history->pending = false;
            atomic_store_explicit(&s_scope_owner, (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_release);
            started = true;
        }
    }
    portEXIT_CRITICAL(&s_history_lock);
    return started;
}

bool nuwax_sd_command_history_active(void)
{
    const uintptr_t owner = atomic_load_explicit(&s_scope_owner, memory_order_acquire);
    return owner && owner == (uintptr_t)xTaskGetCurrentTaskHandle();
}

bool nuwax_sd_command_history_observing(void)
{
    return s_history && !atomic_load_explicit(&s_history->frozen, memory_order_acquire);
}

void nuwax_sd_command_history_end(void)
{
    if (!nuwax_sd_command_history_active()) return;
    portENTER_CRITICAL(&s_history_lock);
    memset(&s_history->scope, 0, sizeof(s_history->scope));
    s_history->pending = false;
    atomic_store_explicit(&s_scope_owner, 0U, memory_order_release);
    portEXIT_CRITICAL(&s_history_lock);
}

void nuwax_sd_command_history_command(const void *card, uint32_t opcode, uint32_t argument,
    uint32_t response, int result, const void *buffer, size_t bytes)
{
    if (!nuwax_sd_command_history_observing()) return;
    const uintptr_t actor = (uintptr_t)xTaskGetCurrentTaskHandle();
    const bool scoped = nuwax_sd_command_history_active();
    if (!scoped) {
        if (card != s_history->tracked_card) return;
        const bool erase = opcode == 32U || opcode == 33U || opcode == 35U || opcode == 36U || opcode == 38U;
        if (!erase && opcode != 13U) return;
        const int saved = errno;
        portENTER_CRITICAL(&s_history_lock);
        if (!s_history->frozen && erase && s_history->count != UINT32_MAX) {
            command_event_t event = {.sequence = s_history->count,
                .opcode = opcode, .argument = argument, .response = response,
                .result = result, .writer_owner = actor == s_history->owner};
            s_history->events[s_history->count++ % HISTORY_EVENTS] = event;
            const unsigned at = opcode == 38U ? 2U : (opcode == 32U || opcode == 35U ? 0U : 1U);
            s_history->erase[at] = event; s_history->erase_valid[at] = true;
            s_history->pending = true; s_history->pending_owner = actor;
        } else if (!s_history->frozen && opcode == 13U && s_history->pending &&
                   s_history->pending_owner == actor && s_history->count) {
            command_event_t *event = &s_history->events[(s_history->count - 1U) % HISTORY_EVENTS];
            if (!event->polls) { event->first_status = response; event->first_error = result; }
            event->last_status = response; event->last_error = result;
            if (event->polls < UINT32_MAX) ++event->polls;
            if (event->opcode == 38U) s_history->erase[2] = *event;
        }
        portEXIT_CRITICAL(&s_history_lock);
        errno = saved; return;
    }
    if (card != s_history->scope.card && card != s_history->scope.alias) return;
    const int saved_errno = errno;
    if (opcode == 13U) {
        portENTER_CRITICAL(&s_history_lock);
        if (!s_history->frozen && s_history->pending && s_history->count) {
            command_event_t *event = &s_history->events[(s_history->count - 1U) % HISTORY_EVENTS];
            if (!event->polls) { event->first_status = response; event->first_error = result; }
            event->last_status = response; event->last_error = result;
            if (event->polls < UINT32_MAX) ++event->polls;
        }
        portEXIT_CRITICAL(&s_history_lock);
    } else if (opcode == 17U || opcode == 18U || opcode == 24U || opcode == 25U) {
        const bool write = opcode == 24U || opcode == 25U;
        /* Exclude only the verifier's nested reads inside a write. Keep normal
         * FatFs reads too: a stale read during a header read/modify/write can
         * explain a later write whose source and persisted bytes both agree. */
        if (!write && s_history->scope.write) {
            s_history->pending = false; errno = saved_errno; return;
        }
        command_event_t event = {.operation = s_history->scope.operation, .opcode = opcode,
            .argument = argument, .bytes = (uint32_t)bytes, .response = response, .result = result,
            .writer_owner = s_history->scope.writer_owner};
        event.lba = s_history->scope.block_addressed ? argument : argument / HISTORY_SECTOR;
        event.range_valid = (s_history->scope.block_addressed || !(argument % HISTORY_SECTOR)) &&
            bytes && !(bytes % HISTORY_SECTOR) && bytes <= UINT32_MAX &&
            event.lba >= s_history->scope.sector &&
            event.lba - s_history->scope.sector < s_history->scope.count &&
            bytes / HISTORY_SECTOR <= s_history->scope.count - (event.lba - s_history->scope.sector);
        if (buffer && event.range_valid) {
            event.data_crc = esp_rom_crc32_le(0U, buffer, bytes);
            if (write && s_history->scope.write) {
                event.source_valid = true;
                event.source_crc = esp_rom_crc32_le(0U, s_history->scope.source +
                    (size_t)(event.lba - s_history->scope.sector) * HISTORY_SECTOR, bytes);
            }
        }
        portENTER_CRITICAL(&s_history_lock);
        if (!s_history->frozen && s_history->generation == s_history->scope.generation) {
            if (s_history->count == UINT32_MAX) s_history->frozen = true;
            else {
                event.sequence = s_history->count;
                s_history->events[s_history->count++ % HISTORY_EVENTS] = event;
                s_history->pending = true;
            }
        }
        portEXIT_CRITICAL(&s_history_lock);
    }
    errno = saved_errno;
}

void nuwax_sd_command_history_freeze(void)
{
    if (!s_history || s_history->owner != (uintptr_t)xTaskGetCurrentTaskHandle()) return;
    portENTER_CRITICAL(&s_history_lock);
    s_history->frozen = true;
    portEXIT_CRITICAL(&s_history_lock);
}

void nuwax_sd_command_history_dump(uint64_t bad_lba, bool lba_known)
{
    if (!s_history || !s_history->frozen || s_history->dumped ||
        s_history->owner != (uintptr_t)xTaskGetCurrentTaskHandle()) return;
    const int saved_errno = errno;
    s_history->dumped = true;
    const uint32_t total = s_history->count;
    const uint32_t first = total > HISTORY_EVENTS ? total - HISTORY_EVENTS : 0U;
    uint32_t write = UINT32_MAX, read = UINT32_MAX;
    bool selected[HISTORY_EVENTS] = {0};
    for (uint32_t n = first; n < total; ++n) {
        const command_event_t *event = &s_history->events[n % HISTORY_EVENTS];
        if (lba_known && event->range_valid && bad_lba >= event->lba &&
            bad_lba - event->lba < event->bytes / HISTORY_SECTOR) {
            if (event->opcode == 24U || event->opcode == 25U) write = n;
            else read = n;
        }
    }
    if (total) { selected[first % HISTORY_EVENTS] = true; selected[(total - 1U) % HISTORY_EVENTS] = true; }
    const uint32_t centers[2] = {write, read};
    for (unsigned c = 0U; c < 2U; ++c) if (centers[c] != UINT32_MAX)
        for (int delta = -2; delta <= 2; ++delta) {
            const int64_t n = (int64_t)centers[c] + delta;
            if (n >= first && n < total) selected[(uint32_t)n % HISTORY_EVENTS] = true;
        }
    if (write == UINT32_MAX && read == UINT32_MAX)
        for (uint32_t n = total > 8U ? total - 8U : first; n < total; ++n) selected[n % HISTORY_EVENTS] = true;
    ESP_LOGE(TAG, "SD command history revision=215 generation=%lu total=%lu retained=%lu conflicts=%lu lba_known=%u lba=%llu write_seq=%lu read_seq=%lu; SDK verdict unchanged",
        (unsigned long)s_history->generation, (unsigned long)total, (unsigned long)(total - first),
        (unsigned long)s_history->conflicts, lba_known, (unsigned long long)bad_lba,
        (unsigned long)write, (unsigned long)read);
    for (unsigned i = 0U; i < 3U; ++i) if (s_history->erase_valid[i]) {
        const command_event_t *event = &s_history->erase[i];
        ESP_LOGE(TAG, "SD erase history revision=217 seq=%lu op=%lu arg=%08lx owner=%u err=%s r1=%08lx polls=%lu last_status=%08lx last_err=%s",
            (unsigned long)event->sequence, (unsigned long)event->opcode,
            (unsigned long)event->argument, event->writer_owner, esp_err_to_name(event->result),
            (unsigned long)event->response, (unsigned long)event->polls,
            (unsigned long)event->last_status, esp_err_to_name(event->last_error));
    }
    unsigned emitted = 0U;
    for (uint32_t n = first; n < total && emitted < HISTORY_DUMP; ++n) if (selected[n % HISTORY_EVENTS]) {
        const command_event_t *event = &s_history->events[n % HISTORY_EVENTS];
        ESP_LOGE(TAG, "SD native seq=%lu io=%lu op=%lu arg=%08lx lba=%llu bytes=%lu range=%u owner=%u err=%s r1=%08lx data_crc=%08lx source_valid=%u source_crc=%08lx polls=%lu first_status=%08lx first_err=%s last_status=%08lx last_err=%s",
            (unsigned long)n, (unsigned long)event->operation, (unsigned long)event->opcode,
            (unsigned long)event->argument, (unsigned long long)event->lba, (unsigned long)event->bytes,
            event->range_valid, event->writer_owner, esp_err_to_name(event->result),
            (unsigned long)event->response, (unsigned long)event->data_crc, event->source_valid,
            (unsigned long)event->source_crc, (unsigned long)event->polls, (unsigned long)event->first_status,
            esp_err_to_name(event->first_error), (unsigned long)event->last_status, esp_err_to_name(event->last_error));
        ++emitted;
    }
    errno = saved_errno;
}
#endif
