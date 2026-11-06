/* Storage-lock owned. Existing RETIRED records are a durable, non-ACK gap.
 * Never shift count or following numbers to hide one unusable archive. */
static bool catalog_discard_identity(const sd_audio_session_t *a, const sd_audio_session_t *b)
{
    return !strcmp(a->session_id, b->session_id) && a->mode == b->mode &&
        a->boot_id == b->boot_id && a->start_ms == b->start_ms &&
        !memcmp(a->owner, b->owner, sizeof(a->owner));
}

static esp_err_t catalog_discard_record(const sd_audio_session_t *expected,
    uint32_t index, const sd_audio_segment_t *bound, sd_audio_segment_t *out)
{
    sd_audio_session_t scene;
    esp_err_t err = sd_audio_catalog_load(expected->session_id, &scene);
    if (err != ESP_OK) return err;
    if (scene.done || !catalog_discard_identity(&scene, expected) || index >= scene.segment_count)
        return ESP_ERR_INVALID_STATE;
    sd_audio_segment_t part;
    err = sd_audio_catalog_read(scene.session_id, index, &part);
    if (err == ESP_OK && part.acknowledged) return ESP_ERR_NOT_FINISHED;
    if (bound) {
        if (err != ESP_OK) return err;
        if (!(part.sealed & 1U) || part.index != bound->index ||
            part.total_bytes != bound->total_bytes || part.payload_crc != bound->payload_crc ||
            part.start_ms != bound->start_ms || part.end_ms != bound->end_ms ||
            strcmp(part.path, bound->path)) return ESP_ERR_INVALID_STATE;
    } else {
        /* A still-open last reservation can be backed by the live writer and
         * its RAM tail. Missing .ready evidence cannot discard that tail. */
        const bool stopped_recovery = (recovery_absence.active || recovery_absence.complete) && !recovery_absence.live;
        if (!scene.ended && !stopped_recovery && index + 1U >= scene.segment_count &&
            (err != ESP_OK || !(part.sealed & 1U))) return ESP_ERR_NOT_FINISHED;
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_CRC &&
            err != ESP_ERR_INVALID_SIZE && err != ESP_ERR_INVALID_STATE) return err;
        if (err != ESP_OK) part = (sd_audio_segment_t){.index = index};
    }
    scene.complete = 0U;
    err = sd_audio_catalog_save(&scene);
    if (err != ESP_OK) return err;
    part.acknowledged = 0U;
    part.missing = SD_AUDIO_SEGMENT_RETIRED;
    /* Fill BOTH CRC slots before any unlink. Either slot then proves the
     * discard after a reset or a later fallback; no old live tuple survives. */
    err = sd_audio_catalog_write(scene.session_id, &part);
    if (err == ESP_OK) err = sd_audio_catalog_write(scene.session_id, &part);
    sd_audio_segment_t actual;
    if (err == ESP_OK) err = sd_audio_catalog_read(scene.session_id, index, &actual);
    if (err == ESP_OK && (actual.missing != SD_AUDIO_SEGMENT_RETIRED || actual.acknowledged))
        err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK && out) *out = actual;
    return err;
}

static void catalog_discard_unlink_impl(const sd_audio_session_t *scene, const sd_audio_segment_t *part)
{
    if (!part->path[0] || !(part->sealed & 1U) || part->acknowledged ||
        retention_pinned(scene->session_id, part->index)) return;
    /* A catalog path outside the generated archive namespace is never an
     * unlink target, even if corrupt metadata points at another valid file. */
    uint32_t order;
    const bool archive_root = !strncmp(part->path, SD_AUDIO_ARCHIVE_ROOT "/", sizeof(SD_AUDIO_ARCHIVE_ROOT)) ||
        !strncmp(part->path, SD_AUDIO_ARCHIVE_NEW_ROOT "/", sizeof(SD_AUDIO_ARCHIVE_NEW_ROOT));
    if (archive_root && !strstr(part->path, "/../") &&
        retention_path(part->path, scene->session_id, &order)) {
        struct stat st;
        if (stat(part->path, &st) != 0) {
            if (errno == ENOENT) (void)sd_audio_catalog_drop_backup(part->path);
            return;
        }
        FILE *file = fopen(part->path, "rb");
        if (!file) return;
        uint8_t header[96];
        const size_t got = fread(header, 1U, sizeof(header), file);
        const bool io_failed = ferror(file) != 0;
        if (fclose(file) != 0 || io_failed) return;
        /* A damaged index can point at a HEALTHY different wire file. Its
         * parseable header is an independent veto on unlinking that source. */
        if (got == sizeof(header) && !memcmp(header, "NWXOFF1\0", 8U) &&
            (u32(header + 24U) != part->index || header[12] != scene->mode ||
             !memchr(header + 36U, 0, 48U) ||
             strcmp((const char *)header + 36U, scene->session_id))) return;
        /* A previous attempt may have failed between the two slot writes.
         * Re-establish both terminal slots before this cleanup deletes data. */
        if (catalog_discard_record(scene, part->index, part, NULL) != ESP_OK) return;
        const int removed = remove(part->path);
        if (removed == 0 || errno == ENOENT) (void)sd_audio_catalog_drop_backup(part->path);
        /* If deletion fails, the terminal record still prevents replay and
         * resurrection. Media-wide I/O recovery is independent of this gap. */
    }
}

static void catalog_discard_unlink(const sd_audio_session_t *scene, const sd_audio_segment_t *part)
{
#ifdef ESP_PLATFORM
    /* Physical cleanup is optional once the gap is durable. A faulty card
     * must not turn it into another long storage-lock stall for the MIC.
     * An existing scope (including repair commit) retains its own owner. */
    if (nuwax_sd_card_io_deadline_begin(esp_timer_get_time() + 250000LL) != ESP_OK) return;
#endif
    catalog_discard_unlink_impl(scene, part);
#ifdef ESP_PLATFORM
    nuwax_sd_card_io_deadline_end();
#endif
}

esp_err_t sd_audio_catalog_discard_expected(const sd_audio_session_t *scene,
                                          const sd_audio_segment_t *part)
{
    if (!scene || !part || !(part->sealed & 1U) || part->acknowledged ||
        !memchr(part->path, 0, sizeof(part->path))) return ESP_ERR_INVALID_ARG;
    esp_err_t err = catalog_discard_record(scene, part->index, part, NULL);
    if (err == ESP_OK) catalog_discard_unlink(scene, part);
    return err;
}

/* Old firmware stored file-specific conflicts as whole-scene blocks. Only
 * recognized local source/index reasons can be cleared after every record
 * is reconciled. Protocol/owner admission failures retain their own policy. */
static esp_err_t catalog_discard_local_block(const char *id, bool *local)
{
    *local = false;
    char path[128], reason[96] = {0};
    if (!catalog_path(path, id, "blocked")) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    (void)fread(reason, 1U, sizeof(reason)-1U, f);
    const bool failed = ferror(f) != 0;
    if (fclose(f) != 0 || failed) return ESP_FAIL;
    *local = !strncmp(reason, "archive_identity_conflict:", sizeof("archive_identity_conflict:")-1U) ||
        !strncmp(reason, "archive_owner_conflict:", sizeof("archive_owner_conflict:")-1U) ||
        !strncmp(reason, "archive_metadata_conflict:", sizeof("archive_metadata_conflict:")-1U) ||
        !strncmp(reason, "archive_metadata_unknown:", sizeof("archive_metadata_unknown:")-1U);
    return ESP_OK;
}

static esp_err_t catalog_discard_clear_local_block(const char *id)
{
    char path[128];
    if (!catalog_path(path, id, "blocked")) return ESP_ERR_INVALID_ARG;
    if (remove(path) != 0 && errno != ENOENT) return ESP_FAIL;
    invalidate_replay_picks();
    return ESP_OK;
}
