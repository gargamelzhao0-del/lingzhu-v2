#include "board/sd_dma_bulk.h"
#include <errno.h>
/* Windowed replay switches from metadata stdio to the descriptor exactly once.
 * Flush/discard stdio read-ahead before seeking the shared descriptor. The
 * reader remains its sole owner until join; no stdio reads run concurrently. */
static esp_err_t replay_read_rewind(FILE *file)
{
    if (fflush(file) != 0 || lseek(fileno(file), 0, SEEK_SET) != 0) return ESP_FAIL;
    clearerr(file);
    return ESP_OK;
}

typedef struct {
    uint64_t copy_us, allocation_us;
    uint32_t internal_bytes, fallback_bytes;
    uint32_t fallback_unaligned, fallback_reserve, fallback_alloc, fallback_post_reserve;
    uint32_t downsized_reads, min_capacity, max_capacity;
    uint32_t allocation_rejects, post_reserve_rejects;
    uint32_t rejected_free_before, rejected_free_after;
    uint64_t largest_scan_us;
    uint32_t largest_scans, hinted_reads;
    uint32_t fallback_fragment_hint;
    size_t fragment_capacity;
    uint32_t fragment_hint_remaining;
    size_t reserve_margin;
    uint32_t reserve_hint_remaining, reserve_hinted_reads;
    replay_stream_t *hot_validator;
    bool hot_validator_allocated;
    bool single_reads;
    uint32_t independent_recoveries;
    /* Optional two-phase reader admission. Raw bytes remain unpublished in
     * the owned PSRAM slot until CRC succeeds. Retry must reacquire storage. */
    void *storage_context;
    esp_err_t (*enter_storage)(void *context);
    void (*leave_storage)(void);
    bool storage_released;
    uint32_t unlocked_validations;
} replay_read_stats_t;

/* Keep the small hot state separate: appending it to an 8 KiB DMA buffer
 * crosses an allocator size class. Only the state lives across reads/ACKs;
 * DMA storage is still released after every slice. Failure is a safe fallback
 * to the caller's validator, with the same CRC and sequence validation. */
static void replay_read_prepare(replay_read_stats_t *stats)
{
    const int64_t started = esp_timer_get_time();
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
        16384U + sizeof(replay_stream_t) + 128U) {
        stats->hot_validator = heap_caps_malloc(sizeof(replay_stream_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (stats->hot_validator &&
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 16384U) {
            heap_caps_free(stats->hot_validator);
            stats->hot_validator = NULL;
        }
    }
    stats->hot_validator_allocated = stats->hot_validator != NULL;
    stats->allocation_us += esp_timer_get_time() - started;
}

static void replay_read_release(replay_read_stats_t *stats)
{
    heap_caps_free(stats->hot_validator);
    stats->hot_validator = NULL;
}

static size_t replay_read_slice_size(uint32_t offset, size_t remaining)
{
    /* After an arbitrary resume boundary, end the first slice on a sector
     * boundary so subsequent DMA destinations stay aligned in FatFS. */
    const size_t limit = 8192U - (offset % 512U);
    return remaining < limit ? remaining : limit;
}

static esp_err_t replay_read_validate(FILE *file, uint8_t *destination, size_t *bytes,
    replay_stream_t *validator, uint32_t total, const archive_header_t *header,
    const archive_footer_t *footer, uint64_t *read_us, uint64_t *validate_us,
    uint64_t *work_us, replay_read_stats_t *stats)
{
    /* Return the actual validated prefix, so a smaller DMA read is published
     * immediately. Never make callers advance past data that was not read. */
    size_t n = *bytes;
    stats->storage_released = false;
    *bytes = 0U;
    *work_us = 0U;
    if (!n || n > 8192U) return ESP_ERR_INVALID_SIZE;
    size_t capacity = (n + 511U) & ~(size_t)511U;
    uint8_t *scratch = NULL;
    int64_t phase = esp_timer_get_time();
    /* An odd resume can make FatFS advance the output pointer by a partial
     * sector. Let the PSRAM adapter handle that first slice; otherwise an
     * unaligned internal pointer makes IDF fall back to single-sector DMA. */
    if (validator->position % 512U != 0U) {
        stats->fallback_unaligned++;
    } else {
        /* Largest-block queries walk each matching heap under its lock. The
         * allocator is the contiguity authority: inspect fragmentation only
         * after failure, not before every healthy read. Hints never replace
         * fresh free-space or post-allocation reserve checks. */
        const bool fragment_blocked = stats->fragment_hint_remaining &&
                                      stats->fragment_capacity < 512U;
        if (stats->fragment_hint_remaining) {
            --stats->fragment_hint_remaining;
            if (capacity > stats->fragment_capacity) {
                capacity = stats->fragment_capacity;
                stats->hinted_reads++;
            }
        }
        size_t margin = 128U;
        if (stats->reserve_hint_remaining) {
            --stats->reserve_hint_remaining;
            margin = stats->reserve_margin;
            stats->reserve_hinted_reads++;
        }
        bool scanned = false;
        enum { RESERVE, ALLOCATION, POST_RESERVE, FRAGMENT_HINT } fallback =
            fragment_blocked ? FRAGMENT_HINT : RESERVE;
        for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
            const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            const size_t overhead = margin;
            size_t available = before > 16384U ? before - 16384U : 0U;
            size_t fit = available > overhead ? (available - overhead) & ~(size_t)511U : 0U;
            if (capacity > fit) capacity = fit;
            if (capacity < 512U) break;
            const size_t allocation_bytes = capacity;
            scratch = heap_caps_malloc(allocation_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!scratch) {
                stats->allocation_rejects++;
                fallback = ALLOCATION;
                capacity -= 512U;
                if (!scanned && capacity >= 512U && attempt < 2U) {
                    const int64_t scan_started = esp_timer_get_time();
                    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
                    stats->largest_scan_us += esp_timer_get_time() - scan_started;
                    stats->largest_scans++;
                    scanned = true;
                    const size_t contiguous = largest > overhead
                        ? (largest - overhead) & ~(size_t)511U : 0U;
                    if (capacity > contiguous) capacity = contiguous;
                    /* Retry the unrestricted size after 32 reads, including
                     * when no sector fits. Never cache a failed allocation or
                     * a heap pointer across reads, ACKs or capture handoffs. */
                    stats->fragment_capacity = contiguous;
                    stats->fragment_hint_remaining = 32U;
                }
                continue;
            }
            const size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (after >= 16384U) break;
            stats->post_reserve_rejects++;
            stats->rejected_free_before = (uint32_t)before;
            stats->rejected_free_after = (uint32_t)after;
            /* Measured cost includes allocator rounding and concurrent users.
             * Reuse it briefly to avoid repeating the same rejected size.
             * Expiration probes recovery; every read still checks free space
             * and the actual post-allocation reserve. */
            if (before > after && before - after > allocation_bytes) {
                const size_t measured_margin = before - after - allocation_bytes + 128U;
                if (margin < measured_margin) margin = measured_margin;
            }
            stats->reserve_margin = margin;
            stats->reserve_hint_remaining = 32U;
            heap_caps_free(scratch);
            scratch = NULL;
            fallback = POST_RESERVE;
            capacity -= 512U;
        }
        if (!scratch) {
            if (fallback == RESERVE) stats->fallback_reserve++;
            else if (fallback == ALLOCATION) stats->fallback_alloc++;
            else if (fallback == POST_RESERVE) stats->fallback_post_reserve++;
            else stats->fallback_fragment_hint++;
        } else {
            if (n > capacity) { n = capacity; stats->downsized_reads++; }
            if (!stats->min_capacity || capacity < stats->min_capacity) stats->min_capacity = (uint32_t)capacity;
            if (capacity > stats->max_capacity) stats->max_capacity = (uint32_t)capacity;
        }
    }
    *work_us = esp_timer_get_time() - phase;
    stats->allocation_us += *work_us;
    uint8_t *input = scratch ? scratch : destination;
    const replay_stream_t checkpoint = *validator;
    bool independent = false;
    if (stats->single_reads) {
        independent = nuwax_sd_bulk_read_recovery_begin(1500U);
        if (!independent) { heap_caps_free(scratch); return ESP_ERR_NOT_FINISHED; }
    }
#ifdef ESP_PLATFORM
    if (scratch) nuwax_replay_dma_loan_begin((uint32_t)capacity, (uint32_t)esp_timer_get_time());
#endif
    phase = esp_timer_get_time();
    /* SD completion wakes the reader at its fixed task priority. Fairness is
     * handled by the bounded work budget, not priority changes per read. */
    errno = 0;
    ssize_t got = read(fileno(file), input, n);
    *read_us += esp_timer_get_time() - phase;
    esp_err_t err = got == (ssize_t)n ? ESP_OK : ESP_FAIL;
    bool copied_early = false;
    if (err == ESP_OK && !independent && stats->leave_storage && stats->enter_storage) {
        /* Free the complete DMA loan before permitting a concurrent writer.
         * Slot ownership, not the storage mutex, protects these bytes while
         * CPU validation runs; the sender cannot see them until publication. */
        if (scratch) {
            phase = esp_timer_get_time();
            memcpy(destination, scratch, n);
            const uint64_t copy_us = esp_timer_get_time() - phase;
            stats->copy_us += copy_us; *work_us += copy_us;
            stats->internal_bytes += (uint32_t)n;
#ifdef ESP_PLATFORM
            nuwax_replay_dma_loan_end();
#endif
            heap_caps_free(scratch); scratch = NULL;
        } else stats->fallback_bytes += (uint32_t)n;
        input = destination;
        copied_early = true;
        stats->leave_storage(); stats->storage_released = true;
        ++stats->unlocked_validations;
    }
    if (err == ESP_OK) {
        phase = esp_timer_get_time();
        replay_stream_t *hot = stats->hot_validator ? stats->hot_validator : validator;
        if (hot != validator) memcpy(hot, validator, sizeof(*hot));
        err = replay_stream_feed(hot, input, n, total, header, footer);
        if (hot != validator) memcpy(validator, hot, sizeof(*hot));
        const uint64_t validation_us = esp_timer_get_time() - phase;
        *work_us += validation_us;
        *validate_us += validation_us;
        if (scratch) {
            stats->internal_bytes += (uint32_t)n;
            if (err == ESP_OK) {
                phase = esp_timer_get_time();
                memcpy(destination, scratch, n);
                const uint64_t copy_us = esp_timer_get_time() - phase;
                stats->copy_us += copy_us;
                *work_us += copy_us;
            }
        } else if (!copied_early) {
            stats->fallback_bytes += (uint32_t)n;
        }
    }
    if ((err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_SIZE) && !independent) {
        if (stats->storage_released) {
            const esp_err_t admitted = stats->enter_storage(stats->storage_context);
            if (admitted != ESP_OK) { err = admitted; goto cleanup; }
            stats->storage_released = false;
        }
        /* Retry the SAME unpublished slice from its original stream state.
         * Reading one byte in another sector evicts FatFs' partial-sector
         * cache; seeking back alone can otherwise reuse its bad bytes. */
        independent = nuwax_sd_bulk_read_recovery_begin(1500U);
        if (!independent) err = ESP_ERR_NOT_FINISHED;
        else {
            *validator = checkpoint;
            uint8_t evicted = 0U;
            const off_t other = checkpoint.position >= 512U ? 0 : (off_t)total - 1;
            phase = esp_timer_get_time();
            errno = 0;
            if (total <= 512U || lseek(fileno(file), other, SEEK_SET) != other ||
                read(fileno(file), &evicted, 1U) != 1 ||
                lseek(fileno(file), checkpoint.position, SEEK_SET) != (off_t)checkpoint.position)
                err = ESP_FAIL;
            else {
                got = read(fileno(file), input, n);
                err = got == (ssize_t)n ? ESP_OK : ESP_FAIL;
            }
            *read_us += esp_timer_get_time() - phase;
            if (err == ESP_OK) {
                phase = esp_timer_get_time();
                replay_stream_t *hot = stats->hot_validator ? stats->hot_validator : validator;
                if (hot != validator) memcpy(hot, validator, sizeof(*hot));
                err = replay_stream_feed(hot, input, n, total, header, footer);
                if (hot != validator) memcpy(validator, hot, sizeof(*hot));
                const uint64_t validation_us = esp_timer_get_time() - phase;
                *work_us += validation_us; *validate_us += validation_us;
                if (scratch) {
                    stats->internal_bytes += (uint32_t)n;
                    if (err == ESP_OK) {
                        phase = esp_timer_get_time();
                        memcpy(destination, scratch, n);
                        const uint64_t copy_us = esp_timer_get_time() - phase;
                        stats->copy_us += copy_us; *work_us += copy_us;
                    }
                } else stats->fallback_bytes += (uint32_t)n;
                if (err == ESP_OK) {
                    stats->single_reads = true; ++stats->independent_recoveries;
                    ESP_LOGW(TAG, "SD replay independent read recovered revision=218 offset=%lu bytes=%u; original stream checkpoint retained, commit still requires original file CRC",
                        (unsigned long)checkpoint.position, (unsigned)n);
                }
            }
        }
    }
cleanup:
    if (independent) nuwax_sd_bulk_read_recovery_end(err == ESP_OK && validator->position == total);
    if (err == ESP_FAIL && errno == ENOMEM) err = ESP_ERR_NO_MEM;
    if (err == ESP_FAIL) nuwax_sd_card_mark_io_fault(err);
#ifdef ESP_PLATFORM
    if (scratch) nuwax_replay_dma_loan_end();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD archive read/validation failed session=%.*s index=%lu position=%lu requested=%u got=%ld length=%lu filled=%lu frames=%lu last_sequence=%lu crc=%08lx expected_crc=%08lx err=%s",
            (int)sizeof(header->session_id), header->session_id,
            (unsigned long)header->segment_index, (unsigned long)validator->position,
            (unsigned)n, (long)got, (unsigned long)validator->length,
            (unsigned long)validator->filled, (unsigned long)validator->frames,
            (unsigned long)validator->last_sequence, (unsigned long)validator->crc,
            (unsigned long)footer->crc32, esp_err_to_name(err));
    }
#endif
    heap_caps_free(scratch);
    if (err == ESP_OK) *bytes = n;
    else *validator = checkpoint;
    return err;
}

static void replay_read_log_fallback(const replay_read_stats_t *stats)
{
    ESP_LOGI(TAG, "offline replay fallback slices unaligned=%lu reserve=%lu allocation=%lu post_reserve=%lu",
        (unsigned long)stats->fallback_unaligned, (unsigned long)stats->fallback_reserve,
        (unsigned long)stats->fallback_alloc, (unsigned long)stats->fallback_post_reserve);
    ESP_LOGI(TAG, "offline replay adaptive read downsized=%lu capacity_min=%lu capacity_max=%lu allocation_rejects=%lu post_reserve_rejects=%lu rejected_free_before=%lu rejected_free_after=%lu",
        (unsigned long)stats->downsized_reads, (unsigned long)stats->min_capacity,
        (unsigned long)stats->max_capacity, (unsigned long)stats->allocation_rejects,
        (unsigned long)stats->post_reserve_rejects, (unsigned long)stats->rejected_free_before,
        (unsigned long)stats->rejected_free_after);
    ESP_LOGI(TAG, "offline replay heap policy=on_failure largest_scans=%lu largest_scan_ms=%llu hinted_reads=%lu fragment_fallback=%lu",
        (unsigned long)stats->largest_scans, (unsigned long long)(stats->largest_scan_us / 1000U),
        (unsigned long)stats->hinted_reads, (unsigned long)stats->fallback_fragment_hint);
    ESP_LOGI(TAG, "offline replay heap state=split hot_internal=%u reserve_hinted_reads=%lu reserve_margin=%u unlocked_slices=%lu",
        stats->hot_validator_allocated ? 1U : 0U,
        (unsigned long)stats->reserve_hinted_reads, (unsigned)stats->reserve_margin,
        (unsigned long)stats->unlocked_validations);
}
