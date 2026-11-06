/* Formal scene files share the authenticated connection. Reuse the validated
 * SD reader, but retain shared-link admission, short complete-frame leases and
 * preemption without closing a healthy connection. No wire-format decisions. */
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE && CONFIG_NUWAX_SD_SESSION_ENABLE
static bool replay_shared_cancelled(void)
{
    return atomic_load_explicit(&s_formal_replay_cancel_requested, memory_order_acquire) ||
        !atomic_load(&s_card_online) || nuwax_offline_audio_storage_busy();
}

static esp_err_t replay_shared_check(nuwax_ws_client_t *ws, uint32_t epoch)
{
    if (!nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != epoch)
        return ESP_ERR_INVALID_STATE;
    if (atomic_load_explicit(&s_formal_replay_cancel_requested, memory_order_acquire) ||
        !atomic_load(&s_card_online) || nuwax_offline_audio_storage_busy())
        return ESP_ERR_NOT_FINISHED;
    return ESP_OK;
}

typedef struct {
    uint64_t yield_us, yield_max_us;
    uint32_t idle_wakes;
    uint64_t ui_wait_us;
    uint32_t ui_handoffs;
} replay_shared_schedule_metrics_t;

static void replay_shared_yield(replay_shared_schedule_metrics_t *metrics)
{
    const int64_t started = esp_timer_get_time();
#ifdef ESP_PLATFORM
    if (nuwax_replay_schedule_yield()) metrics->idle_wakes++;
#else
    vTaskDelay(1U);
#endif
    const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - started);
    metrics->yield_us += elapsed;
    if (elapsed > metrics->yield_max_us) metrics->yield_max_us = elapsed;
}

static size_t replay_shared_memory_reserve(void)
{
    return replay_shared_reserve_bytes(atomic_load_explicit(&s_mirror_staging_rings,
        memory_order_acquire), MIRROR_STAGING_FRAMES * sizeof(mirror_staged_frame_t));
}

static uint8_t *replay_shared_allocate(size_t wire_capacity, size_t *capacity)
{
    *capacity = 32768U; /* Two aligned four-frame slots; reserve RAM for TCP. */
    uint8_t *buffer = NULL;
    while (*capacity >= 4096U && !buffer) {
        const size_t bytes = 2U * *capacity + wire_capacity;
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= bytes + replay_shared_memory_reserve()) {
            buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            /* A recording may have closed during allocation. Recompute the
             * reserve after allocation; never reuse an expired ring credit. */
            if (buffer && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < replay_shared_memory_reserve()) {
                heap_caps_free(buffer); buffer = NULL;
            }
        }
        if (!buffer) *capacity /= 2U;
    }
    return buffer;
}

static esp_err_t replay_send_shared_file(nuwax_ws_client_t *ws, uint32_t epoch,
    FILE *file, const char *segment_id, uint32_t total, uint32_t resume,
    const archive_header_t *header, const archive_footer_t *footer, bool *allocation_deferred)
{
    *allocation_deferred = false;
    if (!file || !ws || resume > total) return ESP_ERR_INVALID_ARG;
    esp_err_t err = replay_shared_check(ws, epoch);
    if (err != ESP_OK) return err;
#ifdef ESP_PLATFORM
    nuwax_replay_schedule_init();
#endif
    const size_t wire_capacity = REPLAY_CHUNK_MAX_PAYLOAD_BYTES + 64U + 14U;
#ifdef ESP_PLATFORM
    /* Admit the optional ciphertext workspace FIRST. The following slot
     * allocation must account for it before preserving the capture/UI reserve. */
    nuwax_tls_archive_crypto_prepare();
    nuwax_sd_socket_wait_prepare();
    const size_t crypto_capacity = nuwax_tls_archive_crypto_capacity();
#else
    const size_t crypto_capacity = CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN + 1U;
#endif
    size_t capacity = 0U;
    uint8_t *buffer = replay_shared_allocate(wire_capacity, &capacity);
    if (!buffer) { *allocation_deferred = true; return ESP_ERR_NO_MEM; }
    uint8_t *wire = buffer + 2U * capacity;
    /* Wire remains beyond both allocated slots. The reader can use a smaller
     * aligned stride without borrowing scratch or changing resume offsets. */
    replay_shared_governor_t governor = {0};
    const size_t first_limit = replay_shared_payload_admit(&governor, nuwax_sd_session_active(),
        replay_capture_queue_depth(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        nuwax_wake_word_capture_backlog_ms(), crypto_capacity);
    capacity = replay_shared_read_capacity(capacity, first_limit != 8064U);
    const int64_t started = esp_timer_get_time();
    err = replay_read_rewind(file);
    replay_prefetch_t *reader = err == ESP_OK ? replay_prefetch_start_context(file,
        ws, epoch, total, resume, capacity, buffer, header, footer, false,
        segment_id, &s_formal_replay_cancel_requested) : NULL;
    replay_stream_t validator = {0};
    replay_read_stats_t read_stats = {0};
    if (!reader && err == ESP_OK) {
        err = replay_read_rewind(file);
        if (err == ESP_OK) replay_read_prepare(&read_stats);
    }
    uint32_t position = reader ? resume : 0U;
    uint32_t sent_end = resume;
    unsigned slot_index = 0U;
    bool sent_any = false;
    uint64_t read_us = 0U, validate_us = 0U, wait_us = 0U, send_us = 0U, pressure_us = 0U;
    uint64_t live_check_us = 0U, foreground_wait_us = 0U, join_us = 0U;
    uint32_t live_checks = 0U;
    uint32_t large_frames = 0U, small_frames = 0U;
    int64_t first_payload_us = -1;
    offline_replay_budget_t budget = {0};
    replay_shared_schedule_metrics_t schedule = {0};
    replay_storage_stats_t admission = {0};
#ifdef ESP_PLATFORM
    uint32_t served_ui_generation = 0U;
#endif
    nuwax_ws_send_metrics_t metrics = {.shared_tls_archive = true, .cancelled = replay_shared_cancelled,
        .tls_plaintext_limit = crypto_capacity > 1U ? crypto_capacity - 1U : 0U,
        .tcp_queue_ceiling_bytes = CONFIG_LWIP_TCP_SND_BUF_DEFAULT};
    while (err == ESP_OK && position < total) {
        err = replay_shared_check(ws, epoch);
        if (err != ESP_OK) break;
        replay_read_slot_t *slot = reader ? &reader->slots[slot_index] : NULL;
        uint8_t *data = reader ? slot->data : buffer;
        size_t count = total - position;
        if (reader) {
            while (!atomic_load_explicit(&slot->ready, memory_order_acquire)) {
                err = replay_shared_check(ws, epoch);
                if (err == ESP_OK) err = atomic_load_explicit(&reader->error, memory_order_acquire);
                if (err != ESP_OK) break;
                const int64_t before = esp_timer_get_time();
                if (replay_prefetch_wait_signal(reader->data_signal)) offline_replay_budget_relax(&budget);
                wait_us += esp_timer_get_time() - before;
            }
            if (err != ESP_OK) break;
            if (slot->position != position) { err = ESP_ERR_INVALID_STATE; break; }
            count = slot->count;
        } else {
            if (position < resume && count > resume - position) count = resume - position;
            count = replay_read_slice_size(position, count);
            err = replay_read_storage_enter_profiled(ws, epoch, &s_formal_replay_cancel_requested,
                &pressure_us, &admission);
            uint64_t work_us = 0U;
            if (err == ESP_OK) {
                err = replay_read_validate(file, data, &count, &validator,
                    total, header, footer, &read_us, &validate_us, &work_us, &read_stats);
                replay_read_storage_leave();
            }
            if (err != ESP_OK) break;
            if (offline_replay_budget_due(&budget, count, work_us)) replay_shared_yield(&schedule);
        }
        if (position < resume) { position += count; continue; }
        for (size_t at = 0U; at < count;) {
            err = replay_shared_check(ws, epoch);
            if (err != ESP_OK) break;
            if (s_replay_route != SD_UPLOAD_LIVE) {
                const int64_t checked = esp_timer_get_time();
                const bool pending = card_live_pending(ws, epoch);
                live_check_us += esp_timer_get_time() - checked;
                ++live_checks;
                if (pending) { s_card_preempted = true; err = ESP_ERR_NOT_FINISHED; break; }
            }
            const int64_t foreground_started = esp_timer_get_time();
            while (!nuwax_audio_uplink_background_ready()) {
                err = replay_shared_check(ws, epoch);
                if (err != ESP_OK) break;
                vTaskDelay(pdMS_TO_TICKS(20U));
                offline_replay_budget_relax(&budget);
            }
            foreground_wait_us += esp_timer_get_time() - foreground_started;
            if (err != ESP_OK) break;
            err = replay_wait_capture_headroom(ws, epoch, &s_formal_replay_cancel_requested, &pressure_us);
            if (err != ESP_OK) break;
            size_t n = count - at;
            const size_t limit = replay_shared_payload_admit(&governor, nuwax_sd_session_active(),
                replay_capture_queue_depth(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                nuwax_wake_word_capture_backlog_ms(), crypto_capacity);
            if (!limit) { err = ESP_ERR_NO_MEM; break; }
            if (n > limit) n = limit;
            if (reader) {
                bool blocked = false;
                err = replay_prefetch_wait_bytes(reader, slot, at + n, &wait_us, &blocked);
                if (blocked) offline_replay_budget_relax(&budget);
                if (err != ESP_OK) break;
            }
#ifdef ESP_PLATFORM
            /* Core0 idle replay shares the face decoder's CPU. Honor its
             * existing bounded lease only BETWEEN complete frames, without
             * holding transport/storage locks or forgiving the byte budget. */
            if (xPortGetCoreID() == 0) {
                const int64_t ui_started = esp_timer_get_time();
                if (nuwax_replay_ui_handoff(&served_ui_generation)) schedule.ui_handoffs++;
                schedule.ui_wait_us += (uint64_t)(esp_timer_get_time() - ui_started);
            }
#endif
            const int64_t packet_started = esp_timer_get_time();
            uint8_t packet[64];
            sd_scene_packet(packet, header->session_id, header->segment_index,
                position + (uint32_t)at, (uint32_t)n, position + at + n == total);
            const int64_t unsent_retry_until = esp_timer_get_time() +
                (int64_t)REPLAY_SHARED_ADMISSION_RETRY_MS * 1000;
            for (;;) {
                err = replay_shared_check(ws, epoch);
                if (err != ESP_OK) break;
                if (!nuwax_audio_uplink_background_ready()) { err = ESP_ERR_NOT_FINISHED; break; }
                /* Re-evaluate recording-ring credit at each safe admission.
                 * TCP's extra allocation is charged as it grows by transport. */
                metrics.tcp_psram_reserve_bytes = replay_shared_memory_reserve() - REPLAY_TCP_HEADROOM_BYTES;
                /* Only commit has a realtime/history type. Decide as close to
                 * first Binary admission as possible, after begin/reader waits. */
                if (!sent_any && header->mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING)
                    s_replay_route = sd_audio_segment_realtime(&s_card_replay_session,
                        &s_card_replay_segment, s_card_boot_id,
                        (uint64_t)esp_timer_get_time() / 1000U, card_online_since_ms())
                            ? SD_UPLOAD_LIVE : SD_UPLOAD_HISTORY;
                const int64_t send_started = esp_timer_get_time();
                metrics.operation_wait_max_us = 0U;
                err = nuwax_ws_send_archive_parts_if_epoch(ws, packet, data + at, n,
                    250U, epoch, wire, wire_capacity, &metrics);
                send_us += esp_timer_get_time() - send_started;
                if (err != ESP_ERR_TIMEOUT || !nuwax_ws_is_connected(ws) ||
                    nuwax_ws_connection_epoch(ws) != epoch) break;
                if (esp_timer_get_time() >= unsent_retry_until) break;
                /* Only no-progress admission timeouts retain this connection.
                 * A partial TLS/WS write has already retired its epoch. Keep
                 * the same file/offset while TCP releases bounded capacity. */
                vTaskDelay(1U);
                offline_replay_budget_relax(&budget);
            }
            if (err != ESP_OK) break;
            if (!sent_any) first_payload_us = esp_timer_get_time() - started;
            sent_any = true;
            if (n > 3896U) ++large_frames; else ++small_frames;
            at += n; sent_end = position + (uint32_t)at;
            card_scene_sent(sent_end);
            if (offline_replay_wait_relaxes(metrics.operation_wait_max_us,
                                            (uint64_t)portTICK_PERIOD_MS * 1000U))
                offline_replay_budget_relax(&budget);
            else if (offline_replay_budget_due(&budget, n,
                         (uint64_t)(esp_timer_get_time() - packet_started))) replay_shared_yield(&schedule);
        }
        if (err != ESP_OK) break;
        position += count;
        if (reader) {
            atomic_store_explicit(&slot->ready, false, memory_order_release);
            xSemaphoreGive(reader->space_signal);
            slot_index ^= 1U;
        }
    }
    const bool prefetched = reader != NULL;
    if (reader) {
        const int64_t joining = esp_timer_get_time();
        const esp_err_t read_err = replay_prefetch_join_profiled(reader, err != ESP_OK,
            &read_us, &validate_us, &admission);
        join_us = esp_timer_get_time() - joining;
        if (err == ESP_OK) err = read_err;
    } else replay_read_release(&read_stats);
    if (err == ESP_OK) err = replay_shared_check(ws, epoch);
    if (err == ESP_OK && position != total) err = ESP_ERR_INVALID_SIZE;
    ESP_LOGI(TAG, "SD formal pipeline revision=195 reader=%s capacity=%u sent=%lu sd_read_ms=%llu validate_ms=%llu send_ms=%llu data_wait_ms=%llu pressure_ms=%llu first_payload_ms=%lld yields=%lu write_calls=%lu elapsed_ms=%llu err=%s",
        prefetched ? "prefetch" : "serial", (unsigned)capacity, (unsigned long)(sent_end - resume),
        (unsigned long long)(read_us / 1000U), (unsigned long long)(validate_us / 1000U),
        (unsigned long long)(send_us / 1000U), (unsigned long long)(wait_us / 1000U),
        (unsigned long long)(pressure_us / 1000U), (long long)(first_payload_us < 0 ? -1 : first_payload_us / 1000U),
        (unsigned long)budget.yields, (unsigned long)metrics.write_calls,
        (unsigned long long)((esp_timer_get_time() - started) / 1000U), esp_err_to_name(err));
    ESP_LOGI(TAG, "SD formal arbitration live_check_ms=%llu live_checks=%lu foreground_wait_ms=%llu join_ms=%llu",
        (unsigned long long)(live_check_us / 1000U), (unsigned long)live_checks,
        (unsigned long long)(foreground_wait_us / 1000U), (unsigned long long)(join_us / 1000U));
    ESP_LOGI(TAG, "SD formal reader admission revision=202 total_ms=%llu writer_ms=%llu queue_ms=%llu wake_ms=%llu mutex_ms=%llu signals=%lu timeouts=%lu mutex_waits=%lu retries=%lu",
        (unsigned long long)(admission.total_us / 1000U),
        (unsigned long long)(admission.writer_us / 1000U),
        (unsigned long long)(admission.queue_us / 1000U),
        (unsigned long long)(admission.wake_us / 1000U),
        (unsigned long long)(admission.mutex_us / 1000U),
        (unsigned long)admission.signals, (unsigned long)admission.timeouts,
        (unsigned long)admission.mutex_waits, (unsigned long)admission.retries);
    ESP_LOGI(TAG, "SD formal geometry crypto_capacity=%u large_frames=%lu small_frames=%lu tls_limit=%u record_splits=%lu",
        (unsigned)crypto_capacity, (unsigned long)large_frames, (unsigned long)small_frames, (unsigned)metrics.tls_plaintext_limit, (unsigned long)metrics.tls_record_splits);
    ESP_LOGI(TAG, "SD formal transport admission_ms=%llu io_lock_ms=%llu write_ms=%llu socket_wait_ms=%llu encode_ms=%llu retries=%lu tls_write_waits=%lu tls_read_waits=%lu retry_yields=%lu recovered_frames=%lu",
        (unsigned long long)(metrics.admission_us / 1000U),
        (unsigned long long)(metrics.io_lock_us / 1000U),
        (unsigned long long)(metrics.write_us / 1000U),
        (unsigned long long)(metrics.writable_wait_us / 1000U),
        (unsigned long long)(metrics.encode_us / 1000U), (unsigned long)metrics.retries,
        (unsigned long)metrics.tls_write_waits, (unsigned long)metrics.tls_read_waits,
        (unsigned long)metrics.tls_retry_yields, (unsigned long)metrics.progress_recoveries);
    ESP_LOGI(TAG, "SD formal writes bytes=%lu max_us=%llu over_20ms=%lu reserve=%u staging_rings=%u tls_record=%u",
        (unsigned long)metrics.write_bytes, (unsigned long long)metrics.write_max_us,
        (unsigned long)metrics.write_over_20ms, (unsigned)replay_shared_memory_reserve(),
        atomic_load(&s_mirror_staging_rings), (unsigned)CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN);
    ESP_LOGI(TAG, "SD formal fairness idle_wakes=%lu idle_credits=%lu ui_credits=%lu yield_ms=%llu yield_max_us=%llu",
        (unsigned long)schedule.idle_wakes, (unsigned long)budget.idle_credits,
        (unsigned long)budget.ui_credits, (unsigned long long)(schedule.yield_us / 1000U),
        (unsigned long long)schedule.yield_max_us);
    ESP_LOGI(TAG, "SD formal UI handoff revision=202 handoffs=%lu wait_ms=%llu",
        (unsigned long)schedule.ui_handoffs, (unsigned long long)(schedule.ui_wait_us / 1000U));
    ESP_LOGI(TAG, "SD formal flow revision=210 admission_waits=%lu event_waits=%lu event_wakes=%lu event_timeouts=%lu tcp_ceiling=%lu no_progress_max_ms=%llu",
        (unsigned long)metrics.tcp_admission_waits, (unsigned long)metrics.tcp_event_waits,
        (unsigned long)metrics.tcp_event_wakes, (unsigned long)metrics.tcp_event_timeouts,
        (unsigned long)metrics.tcp_queue_ceiling_bytes, (unsigned long long)(metrics.no_progress_max_us / 1000U));
    ESP_LOGI(TAG, "SD formal writable revision=204 precise_calls=%lu precise_timeouts=%lu fallbacks=%lu timeout_next_progress=%lu timeout_next_blocked=%lu timeout_next_bytes=%llu bio_progress_after_timeout=%lu timeout_overrun_ms=%llu wait_max_us=%llu",
        (unsigned long)metrics.precise_wait_calls, (unsigned long)metrics.precise_wait_timeouts,
        (unsigned long)metrics.precise_wait_fallbacks, (unsigned long)metrics.timeout_next_progress,
        (unsigned long)metrics.timeout_next_blocked, (unsigned long long)metrics.timeout_next_bytes,
        (unsigned long)metrics.tls_timeout_bio_progress, (unsigned long long)(metrics.writable_timeout_overrun_us / 1000U),
        (unsigned long long)metrics.writable_wait_max_us);
    ESP_LOGI(TAG, "SD formal TLS aead_ms=%llu bio_ms=%llu aead_max_us=%llu bio_max_us=%llu aead_calls=%lu bio_calls=%lu aead_bytes=%lu bio_bytes=%lu profiled=%lu skipped=%lu",
        (unsigned long long)(metrics.tls_aead_us / 1000U), (unsigned long long)(metrics.tls_bio_us / 1000U),
        (unsigned long long)metrics.tls_aead_max_us, (unsigned long long)metrics.tls_bio_max_us,
        (unsigned long)metrics.tls_aead_calls, (unsigned long)metrics.tls_bio_calls,
        (unsigned long)metrics.tls_aead_bytes, (unsigned long)metrics.tls_bio_bytes,
        (unsigned long)metrics.tls_profiled_calls, (unsigned long)metrics.tls_profile_skipped);
    ESP_LOGI(TAG, "SD formal AES hw_calls=%lu hw_bytes=%lu verified=%lu fallbacks=%lu failures=%lu",
        (unsigned long)metrics.tls_hw_calls, (unsigned long)metrics.tls_hw_bytes,
        (unsigned long)metrics.tls_hw_verified, (unsigned long)metrics.tls_hw_fallbacks,
        (unsigned long)metrics.tls_hw_failures);
    ESP_LOGI(TAG, "SD formal AES key_cache setups=%lu reuses=%lu staged_calls=%lu staged_bytes=%lu revision=200",
        (unsigned long)metrics.tls_hw_key_setups, (unsigned long)metrics.tls_hw_key_reuses,
        (unsigned long)metrics.tls_hw_staged_calls, (unsigned long)metrics.tls_hw_staged_bytes);
    ESP_LOGI(TAG, "SD formal GCM ctr_ms=%llu hash_ms=%llu sdk_update_ms=%llu fast_updates=%lu revision=201",
        (unsigned long long)(metrics.tls_gcm_ctr_us / 1000U),
        (unsigned long long)(metrics.tls_gcm_hash_us / 1000U),
        (unsigned long long)(metrics.tls_gcm_sdk_update_us / 1000U),
        (unsigned long)metrics.tls_gcm_fast_updates);
    /* The replay owner releases crypto once the batch ends. Keeping it across
     * files avoids repeated allocation/KAT and traffic-key table generation. */
    /* join above owns the only FILE user; no healthy shared connection is
     * closed for preemption/cancellation/CRC. The source stays until commit ACK. */
    heap_caps_free(buffer);
    return err;
}

/* A single small buffer is also a complete, validating streaming path. In
 * particular, low PSRAM must not turn every file into a full prescan followed
 * by a second read. Validate resumed prefixes too, and never commit on CRC,
 * identity, media, epoch or cancellation failure. No reader task is created. */
static esp_err_t replay_send_shared_bounded_file(nuwax_ws_client_t *ws, uint32_t epoch,
    FILE *file, const char *segment_id, uint32_t total, uint32_t resume,
    const archive_header_t *header, const archive_footer_t *footer)
{
    if (!ws || !epoch || !file || !header || !footer || resume > total ||
        total < HEADER_BYTES + FOOTER_BYTES) return ESP_ERR_INVALID_ARG;
    const int64_t started = esp_timer_get_time();
    uint8_t *chunk = heap_caps_malloc(64U + REPLAY_FORMAL_CHUNK_PAYLOAD_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) return ESP_ERR_NO_MEM;
    esp_err_t err = replay_shared_check(ws, epoch);
    if (err == ESP_OK) err = replay_read_rewind(file);
    replay_stream_t validator = {0};
    replay_read_stats_t reader_stats = {0};
    replay_read_prepare(&reader_stats);
    nuwax_ws_send_metrics_t metrics = {.shared_tls_archive = true, .cancelled = replay_shared_cancelled,
        .tcp_queue_ceiling_bytes = CONFIG_LWIP_TCP_SND_BUF_DEFAULT};
    replay_storage_stats_t admission = {0};
    offline_replay_budget_t budget = {0};
    replay_shared_schedule_metrics_t schedule = {0};
    uint64_t read_us = 0U, validate_us = 0U, send_us = 0U, pressure_us = 0U;
    uint32_t position = 0U, transferred = 0U, chunks = 0U;
#ifdef ESP_PLATFORM
    uint32_t served_ui_generation = 0U;
#endif
    while (err == ESP_OK && position < total) {
        err = replay_shared_check(ws, epoch);
        if (err != ESP_OK) break;
        size_t count = total - position;
        if (count > REPLAY_FORMAL_CHUNK_PAYLOAD_BYTES) count = REPLAY_FORMAL_CHUNK_PAYLOAD_BYTES;
        if (position < resume && count > resume - position) count = resume - position;
        err = replay_read_storage_enter_profiled(ws, epoch, &s_formal_replay_cancel_requested,
            &pressure_us, &admission);
        if (err == ESP_OK) {
            uint64_t work_us = 0U;
            err = replay_read_validate(file, chunk + 64U, &count, &validator,
                total, header, footer, &read_us, &validate_us, &work_us, &reader_stats);
            replay_read_storage_leave();
        }
        if (err == ESP_OK) err = replay_shared_check(ws, epoch);
        if (err != ESP_OK) break;
        if (position < resume) {
            position += (uint32_t)count;
            if (offline_replay_budget_due(&budget, count, 0U)) replay_shared_yield(&schedule);
            continue;
        }
        while (!nuwax_audio_uplink_background_ready() && err == ESP_OK) {
            err = replay_shared_check(ws, epoch);
            if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(20U));
            offline_replay_budget_relax(&budget);
        }
        if (err == ESP_OK) err = replay_wait_capture_headroom(ws, epoch,
            &s_formal_replay_cancel_requested, &pressure_us);
        if (err != ESP_OK) break;
        if (s_replay_route != SD_UPLOAD_LIVE && card_live_pending(ws, epoch)) {
            s_card_preempted = true; err = ESP_ERR_NOT_FINISHED; break;
        }
#ifdef ESP_PLATFORM
        if (xPortGetCoreID() == 0) {
            const int64_t ui_started = esp_timer_get_time();
            if (nuwax_replay_ui_handoff(&served_ui_generation)) schedule.ui_handoffs++;
            schedule.ui_wait_us += (uint64_t)(esp_timer_get_time() - ui_started);
        }
#endif
        if (!transferred && header->mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING)
            s_replay_route = sd_audio_segment_realtime(&s_card_replay_session,
                &s_card_replay_segment, s_card_boot_id,
                (uint64_t)esp_timer_get_time() / 1000U, card_online_since_ms())
                    ? SD_UPLOAD_LIVE : SD_UPLOAD_HISTORY;
        sd_scene_packet(chunk, header->session_id, header->segment_index,
            position, (uint32_t)count, position + count == total);
        metrics.tcp_psram_reserve_bytes = replay_shared_memory_reserve() - REPLAY_TCP_HEADROOM_BYTES;
        metrics.operation_wait_max_us = 0U;
        const int64_t send_started = esp_timer_get_time();
        err = replay_send_frame_buffered(ws, epoch, chunk, count + 64U,
            true, false, NULL, 0U, &metrics);
        send_us += (uint64_t)(esp_timer_get_time() - send_started);
        if (err != ESP_OK) break;
        position += (uint32_t)count; transferred += (uint32_t)count; ++chunks;
        card_scene_sent(position);
        if (offline_replay_wait_relaxes(metrics.operation_wait_max_us,
                (uint64_t)portTICK_PERIOD_MS * 1000U)) offline_replay_budget_relax(&budget);
        if (offline_replay_budget_due(&budget, count, 0U)) replay_shared_yield(&schedule);
    }
    if (err == ESP_OK && (position != total || validator.position != total)) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) err = replay_shared_check(ws, epoch);
    ESP_LOGI(TAG, "SD bounded stream revision=216 segment=%s resume=%lu validated=%lu transferred=%lu chunks=%lu read_ms=%llu validate_ms=%llu send_ms=%llu pressure_ms=%llu tls_calls=%lu elapsed_ms=%llu err=%s",
        segment_id, (unsigned long)resume, (unsigned long)validator.position,
        (unsigned long)transferred, (unsigned long)chunks, (unsigned long long)(read_us/1000U),
        (unsigned long long)(validate_us/1000U), (unsigned long long)(send_us/1000U),
        (unsigned long long)(pressure_us/1000U), (unsigned long)metrics.write_calls,
        (unsigned long long)((esp_timer_get_time()-started)/1000U), esp_err_to_name(err));
    replay_read_release(&reader_stats);
    heap_caps_free(chunk);
    return err;
}
#endif
