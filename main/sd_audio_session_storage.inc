/* Included by the spool after its storage primitives. All manifest mutations
 * take s.lock; the MIC path only queues frames. */
#if CONFIG_NUWAX_SD_SESSION_ENABLE
#include "sd_audio_time.h"
#include <strings.h>

static uint64_t card_online_since_ms(void)
{
    portENTER_CRITICAL(&s_card_network_lock);
    const uint64_t since = s_card_online_since;
    portEXIT_CRITICAL(&s_card_network_lock);
    return since;
}

static bool card_frame_realtime(uint32_t timestamp_ms)
{
    const uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
    portENTER_CRITICAL(&s_card_network_lock);
    const bool online = atomic_load(&s_card_online);
    const uint64_t since = s_card_online_since;
    portEXIT_CRITICAL(&s_card_network_lock);
    return online && sd_audio_frame_after_online(timestamp_ms, now, since);
}

static bool card_owner(uint8_t owner[32])
{
    nuwax_cloud_config_t config = {0};
    size_t size = 0;
    if (nuwax_cloud_config_get(&config) != ESP_OK) return false;
    /* Hash the device identity and binding key together; never log the key. */
    uint8_t input[NUWAX_CLOUD_DEVICE_ID_MAX + NUWAX_CLOUD_DEVICE_SECRET_MAX] = {0};
    memcpy(input, config.device_id, sizeof(config.device_id));
    memcpy(input + sizeof(config.device_id), config.device_secret, sizeof(config.device_secret));
    const bool ok = psa_hash_compute(PSA_ALG_SHA_256, input, sizeof(input),
                                      owner, 32U, &size) == PSA_SUCCESS && size == 32U;
    memset(&config, 0, sizeof(config)); memset(input, 0, sizeof(input));
    return ok;
}

/* Stop is sampled by the MIC owner before tail drain, not by SD finalization. */
static uint64_t s_card_stopped_ms;
static bool s_card_stopped;
static sd_audio_session_time_t s_card_time;
static void card_note_capture_stopped(void)
{
    if (!nuwax_sd_session_active()) return;
    const uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
    portENTER_CRITICAL(&s_card_network_lock);
    if (!s_card_stopped) { s_card_stopped = true; s_card_stopped_ms = now; }
    portEXIT_CRITICAL(&s_card_network_lock);
}
static bool card_stop_time(uint64_t *stopped_ms)
{
    portENTER_CRITICAL(&s_card_network_lock);
    const bool known = s_card_stopped;
    *stopped_ms = s_card_stopped_ms;
    portEXIT_CRITICAL(&s_card_network_lock);
    return known;
}
static void card_save_time_locked(void)
{
    nuwax_cloud_time_anchor_t anchor = {0};
    (void)nuwax_cloud_time_snapshot(&anchor);
    uint64_t stopped_ms = 0;
    const bool stopped = card_stop_time(&stopped_ms);
    sd_audio_session_time_t saved = {0};
    const esp_err_t err = sd_audio_catalog_session_time(&s_card_session, s_card_boot_id,
        &anchor, stopped, stopped_ms, &saved);
    if (err == ESP_OK) s_card_time = saved;
    if (err != ESP_OK) ESP_LOGW(TAG, "SD session time unavailable session=%s err=%s; audio retained",
        s_card_session.session_id, esp_err_to_name(err));
}
static void card_time_service(void)
{
    static uint64_t last_sample, last_saved_ms, next_attempt_ms;
    const uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
    if (now < next_attempt_ms || !atomic_load(&s_media_ready) ||
        atomic_load(&s.media_recovery_requested)) return;
    nuwax_cloud_time_anchor_t anchor = {0};
    const bool calibrated = nuwax_cloud_time_snapshot(&anchor);
    const bool clock_due = calibrated && anchor.uptime_ms != last_sample &&
        (!last_saved_ms || now - last_saved_ms >= 60000U);
    if (xSemaphoreTake(s.lock, 0) != pdTRUE) return;
    next_attempt_ms = now + 1000U;
    if (clock_due) {
        uint8_t owner[32];
        if (card_owner(owner) && sd_audio_catalog_clock_save(s_card_boot_id, owner, &anchor) == ESP_OK) {
            last_sample = anchor.uptime_ms; last_saved_ms = now;
        }
    }
    uint64_t stopped_ms = 0;
    const bool stopped = card_stop_time(&stopped_ms);
    if (nuwax_sd_session_active() && atomic_load(&s_card_created) &&
        ((calibrated && !(s_card_time.valid & SD_AUDIO_TIME_STARTED)) ||
         (stopped && !(s_card_time.valid & SD_AUDIO_TIME_DURATION)))) card_save_time_locked();
    xSemaphoreGive(s.lock);
}

/* A late tail may finish while a new mode is being published. Stop and owner
 * publication share this short guard, so an old tail cannot cancel a new MIC.
 * request_stop performs bounded atomics only; never add I/O/waits here. */
static void card_note_mirror_result(const char *session_id, bool lost_pcm)
{
    portENTER_CRITICAL(&s_card_network_lock);
    if (nuwax_sd_session_active() &&
        !strncmp(session_id, s_card_mirror_session, sizeof(s_card_mirror_session))) {
        atomic_store(&s.capture_complete, false);
        if (lost_pcm) {
            atomic_store(&s_card_failed, true);
            (void)nuwax_offline_audio_request_stop(false);
        } else {
            /* MIC resynchronization retains monitoring intent, but never
             * upgrades that recording back to an uninterrupted result. */
            atomic_store(&s_card_partial, true);
        }
    }
    portEXIT_CRITICAL(&s_card_network_lock);
}

bool nuwax_sd_session_active(void) { return atomic_load(&s_card_active); }
bool nuwax_sd_session_complete(void)
{ return !atomic_load(&s_card_failed) && !atomic_load(&s_card_partial); }
bool nuwax_sd_session_stopped(void)
{
    return !nuwax_sd_session_active() && !atomic_load(&s_card_finishing) &&
        !atomic_load(&s_card_io_recovery.pending) && !nuwax_offline_audio_capture_running() &&
        !atomic_load(&s.active) && !atomic_load(&s.closing) &&
        !atomic_load(&s.writer_busy) && !atomic_load(&s_mirror_closers) &&
        (!s.queue || !uxQueueMessagesWaiting(s.queue));
}
bool nuwax_sd_session_monitoring(void)
{
    return nuwax_sd_session_active() && atomic_load(&s_card_mode) == 1;
}
bool nuwax_sd_session_available(void)
{
    return runtime_initialized() && atomic_load(&s_media_ready) &&
        !atomic_load(&s.media_recovery_requested) &&
        !atomic_load(&s_shutdown_requested);
}
bool nuwax_sd_session_prefers_local(void)
{
    return nuwax_sd_session_available() || nuwax_sd_card_media_detected();
}
esp_err_t nuwax_sd_session_start_fault(void)
{
    const esp_err_t err = nuwax_sd_card_media_error();
    /* Lock/resource contention can recover within the normal start budget.
     * A failed content check is different: do not stop the current foreground
     * or show a recording face while waiting for a scheduled slow remount. */
    return err == ESP_OK || err == ESP_ERR_NO_MEM || err == ESP_ERR_NOT_FINISHED ||
        err == ESP_ERR_TIMEOUT ? ESP_OK : err;
}
void nuwax_sd_session_network(bool online)
{
    const uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
    portENTER_CRITICAL(&s_card_network_lock);
    const bool old = atomic_load(&s_card_online);
    if (online && !old) s_card_online_since = now;
    atomic_store(&s_card_online, online);
    portEXIT_CRITICAL(&s_card_network_lock);
}

esp_err_t nuwax_sd_session_check_new_identity(const char *session_id)
{
    if (!sd_audio_identity_valid(session_id)) return ESP_ERR_INVALID_ARG;
    if (!nuwax_sd_session_available()) return ESP_ERR_NOT_FINISHED;
    if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100U)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    /* The active scene may not have reached its first durable manifest yet. */
    const esp_err_t err = !nuwax_sd_session_available() ? ESP_ERR_NOT_FINISHED :
        nuwax_sd_session_active() && !strcasecmp(session_id, s_card_session.session_id)
        ? ESP_ERR_INVALID_STATE : sd_audio_catalog_check_new(session_id);
    xSemaphoreGive(s.lock);
    return err;
}

/* Set before launching the MIC task; that task copies it into its own config. */
static int64_t s_card_start_deadline_us;

static esp_err_t card_start_checked(nuwax_offline_audio_mode_t mode, char session_id[48],
                                    int64_t deadline_us)
{
    if (!session_id || (mode != NUWAX_OFFLINE_AUDIO_MODE_MEETING &&
        mode != NUWAX_OFFLINE_AUDIO_MODE_MONITORING) ||
        (session_id[0] && !sd_audio_identity_valid(session_id))) return ESP_ERR_INVALID_ARG;
    if (deadline_us && esp_timer_get_time() >= deadline_us) return ESP_ERR_TIMEOUT;
    if (mode == NUWAX_OFFLINE_AUDIO_MODE_MEETING && atomic_load(&s_space_blocked))
        return ESP_ERR_NOT_FINISHED;
    if (!nuwax_sd_session_available() ||
        atomic_load(&s.active) || atomic_load(&s.closing) ||
        atomic_load(&s_mirror_closers) || nuwax_offline_audio_capture_running()) return ESP_ERR_NOT_FINISHED;
    if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100U)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    /* Reservation and finalization share this lock. Never publish a new owner
     * while the previous owner's finishing flag/manifest is still visible. */
    if (nuwax_sd_session_active()) {
        xSemaphoreGive(s.lock); return ESP_ERR_INVALID_STATE;
    }
    if (!nuwax_sd_session_available() || atomic_load(&s.active) || atomic_load(&s.closing) ||
        atomic_load(&s_mirror_closers) || nuwax_offline_audio_capture_running()) {
        xSemaphoreGive(s.lock); return ESP_ERR_NOT_FINISHED;
    }
    char selected[48] = {0};
    const bool supplied = session_id[0] != '\0';
    esp_err_t identity_err = ESP_ERR_INVALID_STATE;
    for (unsigned attempt = 0; attempt < 4U; ++attempt) {
        if (supplied) strlcpy(selected, session_id, sizeof(selected));
        else snprintf(selected, sizeof(selected), "%s_%08lx%08lx",
            mode == NUWAX_OFFLINE_AUDIO_MODE_MEETING ? "meeting" : "monitor",
            (unsigned long)esp_random(), (unsigned long)esp_random());
        identity_err = deadline_us ? nuwax_sd_session_poll_identity(mode, selected, deadline_us)
                                  : sd_audio_catalog_check_new(selected);
        if (identity_err == ESP_OK || supplied || identity_err != ESP_ERR_INVALID_STATE) break;
    }
    if (identity_err != ESP_OK) {
        xSemaphoreGive(s.lock);
        return identity_err;
    }
    if (deadline_us && esp_timer_get_time() >= deadline_us) {
        xSemaphoreGive(s.lock);
        return ESP_ERR_TIMEOUT;
    }
    memset(&s_card_session, 0, sizeof(s_card_session));
    memset(&s_card_time, 0, sizeof(s_card_time));
    portENTER_CRITICAL(&s_card_network_lock);
    s_card_stopped = false; s_card_stopped_ms = 0;
    portEXIT_CRITICAL(&s_card_network_lock);
    s_card_session.mode = (uint8_t)mode;
    s_card_session.complete = 1;
    s_card_session.start_ms = (uint64_t)esp_timer_get_time() / 1000U;
    s_card_session.boot_id = s_card_boot_id;
    strlcpy(s_card_session.session_id, selected, sizeof(s_card_session.session_id));
    const bool owner_ready = card_owner(s_card_session.owner);
    strlcpy(session_id, s_card_session.session_id, 48U);
    atomic_store(&s_card_mode, mode);
    atomic_store(&s_card_created, false);
    atomic_store(&s_card_finishing, false);
    atomic_store(&s_card_failed, false);
    atomic_store(&s_capture_recovery_until, (uint32_t)((uint64_t)esp_timer_get_time() / 1000U));
    atomic_store(&s_card_partial, false);
    s_card_damage = (sd_audio_damage_window_t){0};
    atomic_store(&s_card_io_recovery.pending, false);
    s_card_io_recovery.identity_valid = false;
    s_card_io_recovery.retry_probe = s_card_io_recovery.skip_retry = false;
    atomic_store(&s_monitor_space_blocked, false);
    s_card_start_deadline_us = deadline_us;
    portENTER_CRITICAL(&s_card_network_lock);
    strlcpy(s_card_mirror_session, session_id, sizeof(s_card_mirror_session));
    atomic_store(&s_card_active, owner_ready);
    portEXIT_CRITICAL(&s_card_network_lock);
    /* Task creation does not wait for capture. Its preparation takes s.lock;
     * keep launch failure cleanup inside the same ownership transaction. */
    const esp_err_t err = owner_ready ? nuwax_offline_audio_start_capture(mode, session_id,
        16000U, 1U, 20U) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) atomic_store(&s_card_active, false);
    xSemaphoreGive(s.lock);
    return err;
}

esp_err_t nuwax_sd_session_start(nuwax_offline_audio_mode_t mode, char session_id[48])
{
    return card_start_checked(mode, session_id, 0);
}

esp_err_t nuwax_sd_session_start_until(nuwax_offline_audio_mode_t mode,
    char session_id[48], int64_t deadline_us)
{
    if (deadline_us <= 0) return ESP_ERR_INVALID_ARG;
    return card_start_checked(mode, session_id, deadline_us);
}

void nuwax_sd_session_stop(bool complete)
{
    if (!nuwax_sd_session_active()) return;
    atomic_store(&s_offline_verify_cancel, true);
    if (!complete) atomic_store(&s_card_failed, true);
    (void)nuwax_offline_audio_request_stop(complete);
}

typedef struct {
    char session_id[48];
    int64_t deadline_us;
    uint32_t generation;
    bool pending;
    bool done;
    esp_err_t result;
} card_identity_query_t;
static card_identity_query_t s_card_identity_queries[2];
static portMUX_TYPE s_card_identity_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t nuwax_sd_session_poll_identity(nuwax_offline_audio_mode_t mode,
    const char *session_id, int64_t deadline_us)
{
    if ((mode != NUWAX_OFFLINE_AUDIO_MODE_MEETING && mode != NUWAX_OFFLINE_AUDIO_MODE_MONITORING) ||
        !sd_audio_identity_valid(session_id) || deadline_us <= 0) return ESP_ERR_INVALID_ARG;
    if (esp_timer_get_time() >= deadline_us) return ESP_ERR_TIMEOUT;
    if (mode == NUWAX_OFFLINE_AUDIO_MODE_MEETING && atomic_load(&s_space_blocked))
        return ESP_ERR_NOT_FINISHED;
    if (!nuwax_sd_session_available()) return ESP_ERR_NOT_FINISHED;
    const unsigned index = mode == NUWAX_OFFLINE_AUDIO_MODE_MEETING ? 0U : 1U;
    bool wake = false;
    portENTER_CRITICAL(&s_card_identity_lock);
    card_identity_query_t *q = &s_card_identity_queries[index];
    if (q->deadline_us != deadline_us || strcmp(q->session_id, session_id)) {
        ++q->generation;
        strlcpy(q->session_id, session_id, sizeof(q->session_id));
        q->deadline_us = deadline_us;
        q->done = false;
        q->pending = true;
        wake = true;
    }
    if (!q->done) wake = true;
    const esp_err_t result = q->done ? q->result : ESP_ERR_NOT_FINISHED;
    portEXIT_CRITICAL(&s_card_identity_lock);
    if (wake) nuwax_offline_audio_supervisor_wake();
    return result;
}

static void card_identity_service(void)
{
    for (unsigned index = 0; index < 2; ++index) {
        portENTER_CRITICAL(&s_card_identity_lock);
        const card_identity_query_t query = s_card_identity_queries[index];
        portEXIT_CRITICAL(&s_card_identity_lock);
        if (!query.pending || query.done) continue;
        esp_err_t result = esp_timer_get_time() >= query.deadline_us ? ESP_ERR_TIMEOUT
            : nuwax_sd_session_check_new_identity(query.session_id);
        /* A slow filesystem result must not revive an expired/replaced request. */
        if (esp_timer_get_time() >= query.deadline_us) result = ESP_ERR_TIMEOUT;
        portENTER_CRITICAL(&s_card_identity_lock);
        card_identity_query_t *q = &s_card_identity_queries[index];
        if (q->generation == query.generation && result != ESP_ERR_NOT_FINISHED) {
            q->result = result;
            q->done = true;
            q->pending = false;
        }
        portEXIT_CRITICAL(&s_card_identity_lock);
    }
}

static bool card_prepare_capture(void)
{
    if (!nuwax_sd_session_active()) return true;
    if (card_monitor_prepare_space() != ESP_OK) {
        atomic_store(&s_card_failed, true);
        atomic_store(&s.capture_complete, false);
        return false;
    }
    if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(1500U)) != pdTRUE) return false;
    esp_err_t err = sd_audio_catalog_create(&s_card_session);
    if (err == ESP_OK) {
        s_card_io_recovery.identity_valid = nuwax_sd_card_get_identity(&s_card_io_recovery.identity);
        if (!s_card_io_recovery.identity_valid) err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK && s_card_session.mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING)
        err = sd_audio_catalog_enable_retention(s_card_session.session_id);
    if (err == ESP_OK) {
        atomic_store(&s_card_created, true);
        card_save_time_locked();
    }
    else {
        atomic_store(&s_card_failed, true);
        atomic_store(&s.capture_complete, false);
        if (err == ESP_FAIL || err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_NOT_FOUND) {
            writer_note_fault("scene_create_readback", err == ESP_FAIL ? errno : 0);
            atomic_store(&s_media_ready, false);
            nuwax_sd_card_mark_io_fault(err);
        }
        ESP_LOGW(TAG, "SD scene preparation failed session=%s err=%s; recording not admitted",
            s_card_session.session_id, esp_err_to_name(err));
    }
    xSemaphoreGive(s.lock);
    return err == ESP_OK;
}

static void card_finalize(void)
{
    if (!nuwax_sd_session_active() || !atomic_load(&s_card_finishing) ||
        atomic_load(&s_offline_verify_running) ||
        atomic_load(&s_card_io_recovery.pending) ||
        nuwax_offline_audio_capture_running() || atomic_load(&s.active) ||
        atomic_load(&s.closing) || atomic_load(&s_mirror_closers) ||
        atomic_load(&s.writer_busy) || uxQueueMessagesWaiting(s.queue)) return;
    if (xSemaphoreTake(s.lock, 0) != pdTRUE) return;
    /* A start or writer may have run after the optimistic checks above. */
    if (!nuwax_sd_session_active() || !atomic_load(&s_card_finishing) ||
        s.file || atomic_load(&s_offline_verify_running) ||
        atomic_load(&s_card_io_recovery.pending) ||
        nuwax_offline_audio_capture_running() || atomic_load(&s.active) ||
        atomic_load(&s.closing) || atomic_load(&s_mirror_closers) ||
        atomic_load(&s.writer_busy) || uxQueueMessagesWaiting(s.queue)) {
        xSemaphoreGive(s.lock); return;
    }
    s_card_session.ended = 1;
    uint64_t stopped_ms = 0;
    s_card_session.ended_ms = card_stop_time(&stopped_ms) ? stopped_ms : (uint64_t)esp_timer_get_time() / 1000U;
    if (atomic_load(&s_card_created)) card_save_time_locked();
    const bool capture_complete = atomic_load(&s.capture_complete);
    const bool capture_degraded = atomic_load(&s.degraded);
    const bool capture_failed = atomic_load(&s_card_failed);
    const bool capture_fault = capture_failed || !capture_complete || capture_degraded;
    if (capture_fault && sd_audio_catalog_replay_pinned()) {
        /* Make the worker leave at its next read/send/ACK checkpoint, then
         * recover on a later supervisor pass without invalidating its lease. */
        atomic_store(&s_card_failed, true);
        xSemaphoreGive(s.lock);
        return;
    }
    s_card_session.complete = s_card_session.complete && !capture_fault && !atomic_load(&s_card_partial);
    const bool complete_before_save = s_card_session.complete;
    esp_err_t err = atomic_load(&s_card_created) ? sd_audio_catalog_save(&s_card_session) : ESP_OK;
    /* Healthy files were sealed and indexed by the writer. Do not reread and
     * fsync an entire eight-hour session on an ordinary stop. */
    /* Intentional retention gaps do not indicate a damaged writer. Do not run
     * a whole-card recovery (or reset a live replay lease) on a normal stop. */
    if (err == ESP_OK && capture_fault && atomic_load(&s_card_created) && atomic_load(&s_media_ready)) {
        err = recover_open_segments();
        if (err == ESP_OK) err = sd_audio_catalog_recover(s_card_boot_id);
        if (err == ESP_OK) err = sd_audio_catalog_load(s_card_session.session_id, &s_card_session);
    }
    if (err != ESP_OK) {
        atomic_store(&s_card_failed, true);
        atomic_store(&s_media_ready, false);
        nuwax_sd_card_mark_io_fault(err);
    }
    if (!s_card_session.complete) atomic_store(&s_card_partial, true);
    if (!s_card_session.complete || err != ESP_OK)
        ESP_LOGW(TAG, "SD session finalization incomplete session=%s capture_complete=%d degraded=%d failed=%d before_save=%d after_save=%d err=%s",
            s_card_session.session_id, capture_complete, capture_degraded, capture_failed,
            complete_before_save, s_card_session.complete, esp_err_to_name(err));
    if (err == ESP_OK || !atomic_load(&s_media_ready)) {
        ESP_LOGI(TAG, "SD session ended session=%s segments=%lu complete=%d err=%s",
            s_card_session.session_id, (unsigned long)s_card_session.segment_count,
            s_card_session.complete, esp_err_to_name(err));
        ESP_LOGI(TAG, "SD session time session=%s started_at_ms=%llu duration_ms=%llu known=%lu",
            s_card_session.session_id, (unsigned long long)s_card_time.started_at_ms,
            (unsigned long long)s_card_time.duration_ms, (unsigned long)s_card_time.valid);
        portENTER_CRITICAL(&s_card_network_lock);
        atomic_store(&s_card_finishing, false);
        atomic_store(&s_card_active, false);
        s_card_mirror_session[0] = 0;
        portEXIT_CRITICAL(&s_card_network_lock);
        atomic_store(&s_card_pending_upload, true);
    }
    xSemaphoreGive(s.lock);
}

static bool card_recording_locked(void)
{
    return nuwax_sd_session_active() && atomic_load(&s_card_created) &&
           !strcmp(s.session_id, s_card_session.session_id);
}

static uint64_t card_file_started_at(uint64_t start_ms)
{
    /* Persist the scene clock before deriving file dates. All files then use
     * one fixed mapping even if a later heartbeat adjusts the UTC estimate. */
    if (!(s_card_time.valid & SD_AUDIO_TIME_STARTED)) card_save_time_locked();
    return sd_audio_file_started_at(&s_card_time, s_card_session.start_ms, start_ms);
}

static esp_err_t card_reserve_segment(uint64_t start, bool realtime)
{
    memset(&s_card_segment, 0, sizeof(s_card_segment));
    strlcpy(s_card_segment.path, s.ready_path, sizeof(s_card_segment.path));
    s_card_segment.start_ms = start;
    s_card_segment.end_ms = start;
    s_card_segment.realtime = realtime;
    return sd_audio_catalog_reserve(&s_card_session, &s_card_segment);
}

static esp_err_t card_seal_segment(void)
{
    if (!card_recording_locked()) return ESP_OK;
    s_card_segment.total_bytes = (uint32_t)(HEADER_BYTES + FOOTER_BYTES +
        s.payload_bytes + (uint64_t)s.frame_count * sizeof(uint32_t));
    s_card_segment.payload_crc = s.payload_crc32;
    s_card_segment.sealed = 1;
    esp_err_t err = sd_audio_catalog_write(s.session_id, &s_card_segment);
    if (err == ESP_OK) atomic_store(&s_card_pending_upload, true);
    return err;
}
#endif
