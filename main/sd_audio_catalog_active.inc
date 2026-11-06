/* Active-scene recovery uses the storage owner's fenced RAM identity. It
 * never guesses identity from audio bytes or overwrites a valid foreign scene. */
static bool catalog_same_capture(const sd_audio_session_t *a, const sd_audio_session_t *b)
{
    return !strcmp(a->session_id, b->session_id) && a->mode == b->mode &&
        a->boot_id == b->boot_id && a->start_ms == b->start_ms &&
        !memcmp(a->owner, b->owner, sizeof(a->owner));
}

static bool catalog_metadata_missing(esp_err_t err)
{
    return err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_SIZE;
}

/* Complete an interrupted, CRC-verified .repairing -> .session publication.
 * Keeping this journal on disk makes a reset during active repair recoverable
 * without needing the previous boot's RAM checkpoint. */
static esp_err_t catalog_restore_pending_repairs(const char *only_id)
{
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    esp_err_t err = ESP_OK;
    for (;;) {
        errno = 0; struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) err = ESP_FAIL; break; }
        const size_t length = strlen(entry->d_name);
        if (length <= 10U || length - 10U >= 48U || strcmp(entry->d_name + length - 10U, ".repairing")) continue;
        char id[48] = {0}, pending[128], target[128];
        memcpy(id, entry->d_name, length - 10U);
        if (!valid_id(id) || (only_id && strcmp(id, only_id))) continue;
        char blocked[128]; struct stat blocked_stat;
        if (!catalog_path(blocked, id, "blocked")) { err = ESP_ERR_INVALID_ARG; break; }
        if (stat(blocked, &blocked_stat) == 0) continue;
        if (errno != ENOENT) { err = ESP_FAIL; break; }
        if (!catalog_path(pending, id, "repairing") || !catalog_path(target, id, "session")) { err = ESP_ERR_INVALID_ARG; break; }
        sd_audio_session_t prepared, current;
        err = load_recovery_session(pending, id, &prepared);
        if (catalog_metadata_missing(err)) { err = ESP_OK; continue; }
        if (err == ESP_ERR_INVALID_STATE) {
            err = catalog_block_reason(id, "pending_repair_record_conflict: source retained\n");
            if (err != ESP_OK) break;
            continue;
        }
        if (err != ESP_OK) break;
        if (prepared.first_pending > prepared.segment_count) { err = ESP_OK; continue; }
        const esp_err_t loaded = load_recovery_session(target, id, &current);
        if (loaded == ESP_OK) {
            if (!catalog_same_capture(&prepared, &current))
                err = catalog_block_reason(id, "pending_repair_identity_conflict: both manifests retained\n");
            /* A valid published record wins over an unpublished temporary. */
            if (err != ESP_OK) break;
            continue;
        }
        if (!catalog_metadata_missing(loaded)) { err = loaded; break; }
        if (loaded != ESP_ERR_NOT_FOUND) {
            err = catalog_quarantine_manifest(id);
            if (err != ESP_ERR_NOT_FOUND) break;
        }
        err = rename(pending, target) == 0 ? catalog_verify_session_file(target, &prepared) : ESP_FAIL;
        if (err != ESP_OK) break;
        note_repaired_session(&prepared);
    }
    if (closedir(dir) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

static esp_err_t catalog_active_quarantine_evidence(const sd_audio_session_t *checkpoint,
                                                   uint32_t *count)
{
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) return ESP_FAIL;
    char prefix[64]; snprintf(prefix, sizeof(prefix), "%s.session.bad.", checkpoint->session_id);
    const size_t prefix_bytes = strlen(prefix);
    esp_err_t err = ESP_OK;
    for (;;) {
        errno = 0; struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) err = ESP_FAIL; break; }
        if (strncmp(entry->d_name, prefix, prefix_bytes)) continue;
        char *end = NULL;
        const unsigned long number = strtoul(entry->d_name + prefix_bytes, &end, 10);
        if (!end || end == entry->d_name + prefix_bytes || *end || number >= 100U) continue;
        char path[160];
        const int n = snprintf(path, sizeof(path), "%s/%s", SD_AUDIO_CATALOG_ROOT, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) { err = ESP_ERR_INVALID_SIZE; break; }
        sd_audio_session_t saved;
        err = load_recovery_session(path, checkpoint->session_id, &saved);
        if (catalog_metadata_missing(err)) { err = ESP_OK; continue; }
        if (err != ESP_OK) break;
        if (!catalog_same_capture(checkpoint, &saved) || saved.done || saved.ended) { err = ESP_ERR_INVALID_STATE; break; }
        if (*count < saved.segment_count) *count = saved.segment_count;
    }
    if (closedir(dir) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

esp_err_t sd_audio_catalog_prepare_active(const sd_audio_session_t *checkpoint)
{
    if (!checkpoint || !valid_id(checkpoint->session_id) || checkpoint->mode > 1U ||
        checkpoint->ended || checkpoint->done || checkpoint->segment_count > 1000000U ||
        checkpoint->first_pending > checkpoint->segment_count) return ESP_ERR_INVALID_ARG;
    if (sd_audio_catalog_replay_pinned()) return ESP_ERR_NOT_FINISHED;
    esp_err_t err = ensure_catalog_directory();
    if (err != ESP_OK) return err;
    char path[128], temporary[128], blocked[128]; struct stat st;
    if (!catalog_path(path, checkpoint->session_id, "session") ||
        !catalog_path(temporary, checkpoint->session_id, "repairing") ||
        !catalog_path(blocked, checkpoint->session_id, "blocked")) return ESP_ERR_INVALID_ARG;
    if (stat(blocked, &st) == 0) return ESP_ERR_INVALID_STATE;
    if (errno != ENOENT) return ESP_FAIL;
    if (!metadata_io_qualified) {
        err = sd_audio_catalog_qualify_storage();
        if (err != ESP_OK) return err;
    }
    err = catalog_restore_pending_repairs(checkpoint->session_id);
    if (err != ESP_OK) return err;
    if (stat(blocked, &st) == 0) return ESP_ERR_INVALID_STATE;
    if (errno != ENOENT) return ESP_FAIL;
    sd_audio_session_t current;
    const esp_err_t loaded = load_recovery_session(path, checkpoint->session_id, &current);
    if (loaded == ESP_OK) return catalog_same_capture(checkpoint, &current) && !current.ended && !current.done
        ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (!catalog_metadata_missing(loaded)) return loaded;
    sd_audio_session_t replacement = *checkpoint;
    err = catalog_active_quarantine_evidence(checkpoint, &replacement.segment_count);
    if (err != ESP_OK) return err;
    /* No ACK is invented. The following physical archive/index scan rewinds
     * this cursor to the first provably unconfirmed file. Existing ACK slots
     * stay intact; a pruned prefix need not be recreated as millions of holes. */
    replacement.first_pending = replacement.segment_count;
    replacement.complete = replacement.done = replacement.ended = 0U;
    replacement.ended_ms = 0U;
    err = catalog_seed_session(temporary, &replacement);
    if (err != ESP_OK) return err;
    if (loaded != ESP_ERR_NOT_FOUND) {
        err = catalog_quarantine_manifest(checkpoint->session_id);
        if (err != ESP_ERR_NOT_FOUND) return err;
    }
    if (rename(temporary, path) != 0) return ESP_FAIL;
    err = catalog_verify_session_file(path, &replacement);
    if (err == ESP_OK) note_repaired_session(&replacement);
    return err;
}
