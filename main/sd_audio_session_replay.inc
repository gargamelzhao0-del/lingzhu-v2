#if CONFIG_NUWAX_SD_SESSION_ENABLE
#include "sd_audio_monitor_fifo.h"
#include "sd_audio_file_fault.h"
static esp_err_t s_card_pass_error;
static uint32_t s_monitor_retry_until, s_monitor_retry_delay;
static uint32_t s_meeting_retry_until, s_meeting_retry_delay;
static const void *s_monitor_retry_transport;
static uint32_t s_monitor_retry_epoch;
static uint8_t s_monitor_retry_owner[32];

/* Storage lock held. A transient read is not loss: confirm across one ordered
 * remount, then discard only this sealed file without changing wire indexes. */
static esp_err_t card_confirm_missing_locked(const sd_audio_session_t *session,
    const sd_audio_segment_t *segment, bool damaged)
{
    if (!nuwax_sd_card_is_mounted()) return ESP_ERR_NOT_FINISHED;
    sd_audio_file_fault_t *fault = &s_card_file_fault[session->mode ? 1U : 0U];
    if (!sd_audio_file_fault_confirm(fault, session->session_id, segment->index,
            atomic_load_explicit(&s_media_generation, memory_order_acquire))) {
        atomic_store(&s_media_ready, false);
        nuwax_sd_card_mark_io_fault(ESP_FAIL);
        atomic_store_explicit(&s_replay_media_fault, true, memory_order_release);
        notify_writer_task();
        nuwax_offline_audio_supervisor_wake();
        ESP_LOGW(TAG, "SD file fault awaiting media recovery session=%s index=%lu; FIFO head retained",
            session->session_id, (unsigned long)segment->index);
        return ESP_ERR_NOT_FINISHED;
    }
    const bool current_capture = nuwax_sd_session_active() &&
        !strcmp(session->session_id, s_card_session.session_id) &&
        session->boot_id == s_card_session.boot_id && session->start_ms == s_card_session.start_ms &&
        !memcmp(session->owner, s_card_session.owner, 32U);
    /* Publish partial first. A reset after rename must not leave an apparently
     * complete scene. Do not replace any immutable file metadata here. */
    sd_audio_session_t partial;
    esp_err_t saved = sd_audio_catalog_load(session->session_id, &partial);
    if (saved != ESP_OK) goto failed;
    if (partial.done || partial.mode != session->mode || memcmp(partial.owner, session->owner, 32U) ||
        partial.boot_id != session->boot_id || partial.start_ms != session->start_ms)
        { saved = ESP_ERR_INVALID_STATE; goto failed; }
    sd_audio_segment_t bound;
    saved = sd_audio_catalog_read(session->session_id, segment->index, &bound);
    if (saved != ESP_OK) goto failed;
    if (bound.acknowledged) { fault->armed = false; return ESP_ERR_NOT_FINISHED; }
    if (!(bound.sealed & 1U) || bound.total_bytes != segment->total_bytes ||
        bound.payload_crc != segment->payload_crc || strcmp(bound.path, segment->path) ||
        bound.start_ms != segment->start_ms || bound.end_ms != segment->end_ms)
        { saved = ESP_ERR_INVALID_STATE; goto failed; }
    if (current_capture) {
        atomic_store(&s_card_partial, true);
        s_card_session.complete = 0U;
    }
    partial.complete = 0U;
    saved = sd_audio_catalog_save(&partial);
    if (saved != ESP_OK) goto failed;
    (void)damaged;
    saved = sd_audio_catalog_discard_expected(session, segment);
    if (saved != ESP_OK) goto failed;
    fault->armed = false;
    return ESP_OK;
failed:
    /* We cannot continue a current capture while its loss transaction has
     * no durable resolution. Historical faults never stop another scene. */
    if (current_capture) {
        atomic_store(&s_card_failed, true);
        atomic_store(&s.capture_complete, false);
        (void)nuwax_offline_audio_request_stop(false);
    }
    return saved;
}

/* Signature retained for the concurrent transport optimization. FIFO removes
 * realtime-file arbitration without changing its framing or speed governor. */
static bool card_live_pending(nuwax_ws_client_t *ws, uint32_t epoch)
{
    (void)ws; (void)epoch;
    return false;
}

static esp_err_t card_session_end(nuwax_ws_client_t *ws, uint32_t epoch,
    bool independent, sd_audio_session_t *session)
{
    if (!session->ended) return ESP_ERR_NOT_FINISHED;
    {
        if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100U)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
        if (sd_audio_catalog_replay_pinned()) {
            xSemaphoreGive(s.lock); return ESP_ERR_NOT_FINISHED;
        }
        bool ready = false;
        const esp_err_t gate = session->mode == 1U ? sd_monitor_fifo_end_ready(session, &ready) :
            sd_meeting_fifo_end_ready(session, &ready);
        xSemaphoreGive(s.lock);
        if (gate != ESP_OK) return gate;
        if (!ready) return ESP_ERR_NOT_FINISHED;
    }
    const int64_t started = esp_timer_get_time();
    esp_err_t err = card_scene_prepare(ws, epoch, independent, session, NULL, NULL);
    if (err != ESP_OK) return err;
    const char *ack = session->mode ? "monitoring_audio_session_end_ack" : "meeting_audio_session_end_ack";
    char request[48];
    snprintf(request, sizeof(request), "sd_end_%08lx", (unsigned long)esp_random());
    sd_audio_session_time_t time;
    card_scene_time(session, &time);
    cJSON *json = sd_audio_end_json(session->mode != 0, request, session->session_id,
                                   session->segment_count, session->complete != 0, &time);
    if (!json) return ESP_ERR_NO_MEM;
    err = replay_arm_ack(epoch, request, ack, session->session_id);
    if (err == ESP_OK) err = replay_send_json(ws, epoch, json, independent);
    cJSON_Delete(json);
    if (err == ESP_OK) err = replay_wait_ack(ws, epoch, request, false, false, NULL);
    else replay_disarm_ack();
    if (err != ESP_OK) return err;
    const int64_t confirmed = esp_timer_get_time();
    if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    if (session->mode == 1U) err = sd_monitor_fifo_finish(session);
    else err = sd_meeting_fifo_finish(session);
    xSemaphoreGive(s.lock);
    if (err == ESP_OK) card_scene_forget(session->session_id);
    if (err == ESP_OK) ESP_LOGI(TAG, "SD whole session acknowledged session=%s segments=%lu complete=%d confirm_ms=%lu persist_ms=%lu",
        session->session_id, (unsigned long)session->segment_count, session->complete,
        (unsigned long)((confirmed - started) / 1000U),
        (unsigned long)((esp_timer_get_time() - confirmed) / 1000U));
    return err;
}

static esp_err_t card_replay_next_impl(nuwax_ws_client_t *ws, uint32_t epoch,
    bool independent, bool *worked, sd_audio_lease_token_t *lease)
{
    *worked = false;
    if (!atomic_load(&s_card_pending_upload) && !nuwax_sd_session_monitoring()) return ESP_OK;
    if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
    esp_err_t err = replay_wait_capture_headroom(ws, epoch, NULL, NULL);
    if (err != ESP_OK) return err;
    uint8_t owner[32];
    if (!card_owner(owner)) return ESP_ERR_INVALID_STATE;
    if (s_monitor_retry_transport != ws || s_monitor_retry_epoch != epoch ||
        memcmp(s_monitor_retry_owner, owner, 32U)) {
        s_monitor_retry_until = s_monitor_retry_delay = 0U;
        s_meeting_retry_until = s_meeting_retry_delay = 0U;
        s_monitor_retry_transport = ws; s_monitor_retry_epoch = epoch;
        memcpy(s_monitor_retry_owner, owner, 32U);
    }
    const int64_t select_started = esp_timer_get_time();
    sd_audio_session_t session;
    sd_audio_segment_t segment = {0};
    bool found = false, file = false;
    if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        /* The writer holding its lock is backpressure, not a broken link. */
        *worked = true;
        vTaskDelay(pdMS_TO_TICKS(10));
        return nuwax_ws_is_connected(ws) && nuwax_ws_connection_epoch(ws) == epoch ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    sd_audio_catalog_stats_t select_before, select_after;
    sd_audio_catalog_stats(&select_before);
#ifdef ESP_PLATFORM
    uint32_t repair_budget_ms = 1500U;
    if (nuwax_offline_audio_capture_running()) {
        const int32_t remaining = (int32_t)(atomic_load_explicit(
            &s_capture_recovery_until, memory_order_acquire) - offline_now_ms());
        if (remaining <= 0) repair_budget_ms = 0U;
        else if ((uint32_t)remaining < repair_budget_ms) repair_budget_ms = (uint32_t)remaining;
    }
    sd_audio_catalog_repair_set_deadline(esp_timer_get_time() + (int64_t)repair_budget_ms * 1000);
#endif
    static int background_mode;
    bool monitor_waiting = false;
    for (unsigned pass = 0U; pass < 2U; ++pass) {
        const int mode = (background_mode + (int)pass) & 1;
        if (mode == 0) {
            if (s_meeting_retry_delay &&
                !offline_deadline_elapsed(offline_now_ms(), s_meeting_retry_until)) {
                monitor_waiting = true; err = ESP_OK;
            } else {
                bool waiting = false;
                err = sd_meeting_fifo_next(owner, &session, &segment, &found, &file, &waiting);
                monitor_waiting |= waiting;
            }
        } else if (s_monitor_retry_delay &&
                   !offline_deadline_elapsed(offline_now_ms(), s_monitor_retry_until)) {
            /* A wrapped zero deadline is valid. The delay marks an armed retry. */
            monitor_waiting = true; err = ESP_OK;
        } else {
            bool waiting = false;
            err = sd_monitor_fifo_next(owner, &session, &segment, &found, &file, &waiting);
            monitor_waiting |= waiting;
        }
        if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
            /* Selection faults used to end the entire replay pass before
             * the other lane could run. Keep this lane's ordering and retry
             * deadline, while independent valid scenes remain serviceable. */
            uint32_t *delay = mode ? &s_monitor_retry_delay : &s_meeting_retry_delay;
            uint32_t *until = mode ? &s_monitor_retry_until : &s_meeting_retry_until;
            *delay = !*delay ? 1000U : *delay < 16000U ? *delay * 2U : 30000U;
            *until = offline_now_ms() + *delay;
            s_card_pass_error = err;
            monitor_waiting = true;
            ESP_LOGW(TAG, "SD %s metadata retained retry_ms=%lu err=%s",
                mode ? "monitor" : "meeting", (unsigned long)*delay, esp_err_to_name(err));
            err = ESP_OK;
            continue;
        }
        if (err != ESP_OK || found) break;
    }
    background_mode = 1 - background_mode;
    /* Waiting for an unsealed tail is level-triggered by its later seal (or
     * session stop), not a reason to start a new worker every idle tick. */
    const bool retry_pending = s_monitor_retry_delay || s_meeting_retry_delay;
    if (err == ESP_OK && !found && !retry_pending && s_card_pass_error == ESP_OK)
        atomic_store(&s_card_pending_upload, false);
    if (err == ESP_OK && !found && monitor_waiting && retry_pending)
        err = ESP_ERR_TIMEOUT; /* Let the existing supervisor enforce backoff. */
    /* Pin while selection is still serialized with retention. The lease also
     * covers fopen, begin/commit waits, CRC checks and post-ACK local deletion. */
    if (err == ESP_OK && found && file)
        err = sd_audio_catalog_lease_acquire(&session, &segment, SD_AUDIO_LEASE_REPLAY,
            atomic_load_explicit(&s_media_generation, memory_order_acquire), 0U, lease);
    sd_audio_catalog_stats(&select_after);
#ifdef ESP_PLATFORM
    sd_audio_catalog_repair_set_deadline(0);
#endif
    xSemaphoreGive(s.lock);
    if (select_after.owner_skips != select_before.owner_skips) {
        ESP_LOGW(TAG, "SD replay retained old-binding scene=%s owner_skips=%lu; not an upload acknowledgment",
            select_after.last_owner_blocked,
            (unsigned long)(select_after.owner_skips - select_before.owner_skips));
    }
    const uint32_t select_ms = (uint32_t)((esp_timer_get_time() - select_started) / 1000U);
    if (err == ESP_ERR_NOT_FINISHED) {
        /* Cursor work is deliberately sliced; let capture/writer use the SD
         * lock before continuing on the same connection. */
        *worked = true;
        vTaskDelay(pdMS_TO_TICKS(10));
        return nuwax_ws_is_connected(ws) && nuwax_ws_connection_epoch(ws) == epoch ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK || !found) return err;
    ESP_LOGI(TAG, "SD replay selection ms=%lu session=%s index=%lu file=%u manifest_reads=%lu completed_hits=%lu",
        (unsigned long)select_ms, session.session_id, (unsigned long)segment.index, file ? 1U : 0U,
        (unsigned long)(select_after.manifest_reads - select_before.manifest_reads),
        (unsigned long)(select_after.completed_hits - select_before.completed_hits));
    *worked = true;
    s_card_preempted = false;
    portENTER_CRITICAL(&s_replay_lock);
    s.replay_ack.scene.error = SD_SCENE_RETRY;
    portEXIT_CRITICAL(&s_replay_lock);
    err = file ? replay_one_ready(ws, epoch, segment.path, independent)
               : card_session_end(ws, epoch, independent, &session);
    if (s_card_preempted) return ESP_OK;
    if (err != ESP_OK && file) card_scene_file_done(); /* Query durable offset on retry. */
    unsigned peer_error;
    portENTER_CRITICAL(&s_replay_lock); peer_error = s.replay_ack.scene.error; portEXIT_CRITICAL(&s_replay_lock);
    if (err == ESP_FAIL && peer_error == SD_SCENE_PERMANENT) {
        if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
        const esp_err_t blocked = sd_audio_catalog_block(session.session_id);
        xSemaphoreGive(s.lock);
        if (blocked != ESP_OK) return blocked;
        card_scene_forget(session.session_id);
        ESP_LOGE(TAG, "SD protocol conflict; scene retained for recovery session=%s index=%lu",
            session.session_id, (unsigned long)segment.index);
        return ESP_OK;
    }
    if (!file && err == ESP_ERR_NOT_FINISHED &&
        nuwax_ws_is_connected(ws) && nuwax_ws_connection_epoch(ws) == epoch &&
        !nuwax_offline_audio_storage_busy()) {
        vTaskDelay(1U); return ESP_OK; /* Continue bounded end verification. */
    }
    if (err == ESP_OK) {
        s_card_file_fault[session.mode ? 1U : 0U].armed = false;
        if (file && xSemaphoreTake(s.lock, pdMS_TO_TICKS(100U)) == pdTRUE) {
            if (nuwax_sd_session_active() && !strcmp(session.session_id, s_card_session.session_id) &&
                !memcmp(session.owner, s_card_session.owner, 32U))
                sd_audio_damage_healthy(&s_card_damage, segment.index);
            xSemaphoreGive(s.lock);
        }
        if (session.mode == 1U) s_monitor_retry_until = s_monitor_retry_delay = 0U;
        else s_meeting_retry_until = s_meeting_retry_delay = 0U;
    }
    if (file && err != ESP_OK && !nuwax_sd_card_is_mounted()) {
        /* A driver-detected fault may already have requested the slow remount
         * below replay_one_ready(). Keep its file identity for that recovery. */
        if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100U)) == pdTRUE) {
            (void)sd_audio_file_fault_confirm(&s_card_file_fault[session.mode ? 1U : 0U],
                session.session_id, segment.index, atomic_load_explicit(&s_media_generation, memory_order_acquire));
            xSemaphoreGive(s.lock);
        }
    }
    if (err == ESP_OK || err == ESP_ERR_NOT_FINISHED ||
        !nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != epoch ||
        !nuwax_sd_card_is_mounted()) return err;
    bool missing_file = false;
    if (file && err == ESP_FAIL && !nuwax_offline_audio_storage_busy()) {
        struct stat st;
        missing_file = stat(segment.path, &st) != 0 && errno == ENOENT;
    }
    if (missing_file) {
        if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
        err = card_confirm_missing_locked(&session, &segment, false);
        xSemaphoreGive(s.lock);
        if (err == ESP_OK) {
            ESP_LOGE(TAG, "SD absent segment recorded as missing session=%s index=%lu",
                session.session_id, (unsigned long)segment.index);
            return ESP_OK;
        }
    }
    if (file && (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_SIZE)) {
        const esp_err_t checked = recheck_archive_expected(&segment);
        if (checked == ESP_OK) {
            /* A healthy independent full read breaks the fault sequence.
             * An old pre-remount witness must not classify a later transient
             * error as persistent corruption. This is no upload ACK. */
            s_card_file_fault[session.mode ? 1U : 0U].armed = false;
        }
        if (checked == ESP_ERR_INVALID_CRC || checked == ESP_ERR_INVALID_SIZE) {
            if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
            err = card_confirm_missing_locked(&session, &segment, true);
            xSemaphoreGive(s.lock);
            if (err == ESP_OK) {
                ESP_LOGE(TAG, "SD damaged segment discarded; following indexes unchanged session=%s index=%lu path=%s",
                    session.session_id, (unsigned long)segment.index, segment.path);
                return ESP_OK;
            }
        }
    }
    {
        /* A failed file remains the head of its own mode lane. The other
         * lane and foreground capture still run; retries never busy-loop. */
        uint32_t *delay = session.mode ? &s_monitor_retry_delay : &s_meeting_retry_delay;
        uint32_t *until = session.mode ? &s_monitor_retry_until : &s_meeting_retry_until;
        *delay = !*delay ? 1000U : *delay < 16000U ? *delay * 2U : 30000U;
        *until = offline_now_ms() + *delay;
        s_card_pass_error = err;
        card_scene_forget(session.session_id);
        ESP_LOGW(TAG, "SD %s FIFO head retained session=%s index=%lu retry_ms=%lu err=%s",
            session.mode ? "monitor" : "meeting", session.session_id, (unsigned long)segment.index,
            (unsigned long)*delay, esp_err_to_name(err));
        return ESP_OK;
    }
}

static esp_err_t card_replay_next(nuwax_ws_client_t *ws, uint32_t epoch,
    bool independent, bool *worked)
{
    sd_audio_lease_token_t lease = 0U;
    const esp_err_t err = card_replay_next_impl(ws, epoch, independent, worked, &lease);
    /* No network wait while holding this lock. Every return path releases the
     * lease, including preemption, disconnect and local verification failure. */
    if (lease && xSemaphoreTake(s.lock, portMAX_DELAY) == pdTRUE) {
        (void)sd_audio_catalog_lease_release(lease, SD_AUDIO_LEASE_REPLAY);
        xSemaphoreGive(s.lock);
    }
    return err;
}
#endif
