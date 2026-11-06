#if CONFIG_NUWAX_SD_SESSION_ENABLE
/* One replay owner, one file in flight. Preserve a received cloud commit ACK
 * when a new recording takes storage ownership before the local fsync. This
 * is a volatile proof only; reboot still uses the ordinary idempotent query. */
typedef struct {
    sd_audio_session_t scene;
    sd_audio_segment_t part;
    nuwax_sd_card_identity_t card;
    bool valid;
} card_pending_commit_t;
static card_pending_commit_t *s_card_pending_commit;

static void card_commit_stage(void)
{
    if (!s_card_pending_commit)
        s_card_pending_commit = heap_caps_calloc(1U, sizeof(*s_card_pending_commit),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_card_pending_commit || s_card_pending_commit->valid) return;
    if (!nuwax_sd_card_get_identity(&s_card_pending_commit->card)) return;
    s_card_pending_commit->scene = s_card_replay_session;
    s_card_pending_commit->part = s_card_replay_segment;
    s_card_pending_commit->valid = true;
}

static void card_commit_saved(void)
{
    if (s_card_pending_commit) s_card_pending_commit->valid = false;
}

static void card_commit_release_idle(void)
{
    if (s_card_pending_commit && !s_card_pending_commit->valid) {
        heap_caps_free(s_card_pending_commit);
        s_card_pending_commit = NULL;
    }
}

static esp_err_t card_commit_flush(void)
{
    card_pending_commit_t *pending = s_card_pending_commit;
    if (!pending || !pending->valid) return ESP_OK;
    if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
    uint8_t owner[32]; nuwax_sd_card_identity_t card;
    if (!card_owner(owner) || memcmp(owner, pending->scene.owner, 32U) ||
        !nuwax_sd_card_get_identity(&card) || memcmp(&card, &pending->card, sizeof(card))) {
        card_commit_saved(); /* No ACK or deletion may cross card/binding changes. */
        ESP_LOGW(TAG, "SD pending commit identity changed; source retained for normal reconciliation");
        return ESP_OK;
    }
    if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    sd_audio_session_t scene;
    sd_audio_segment_t part;
    esp_err_t err = sd_audio_catalog_load(pending->scene.session_id, &scene);
    if (err == ESP_OK && (scene.boot_id != pending->scene.boot_id ||
        scene.mode != pending->scene.mode || scene.start_ms != pending->scene.start_ms ||
        memcmp(scene.owner, pending->scene.owner, 32U))) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = sd_audio_catalog_read(scene.session_id, pending->part.index, &part);
    if (err == ESP_OK && (part.missing || !(part.sealed & 1U) ||
        part.total_bytes != pending->part.total_bytes || part.payload_crc != pending->part.payload_crc ||
        part.start_ms != pending->part.start_ms || part.end_ms != pending->part.end_ms ||
        part.realtime != pending->part.realtime || strcmp(part.path, pending->part.path)))
        err = ESP_ERR_INVALID_STATE;
    /* Rewrite even a visible ACK: an earlier fsync/close may have failed,
     * and a fresh untouched FatFs handle cannot flush that prior write. */
    if (err == ESP_OK)
        err = sd_audio_catalog_ack(scene.session_id, part.index);
    xSemaphoreGive(s.lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD pending commit persisted session=%s index=%lu; payload replay avoided",
            pending->scene.session_id, (unsigned long)pending->part.index);
        /* Normal ACKed-file selection owns unlink, quota and cursor progress. */
        card_commit_saved();
    } else if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_CRC ||
               err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGW(TAG, "SD pending commit metadata changed err=%s; source retained for normal reconciliation",
            esp_err_to_name(err));
        card_commit_saved();
        return ESP_OK;
    }
    return err;
}
#endif
