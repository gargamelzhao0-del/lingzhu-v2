/* Recovery-only repair of the pre-r191 FatFs stale-slot defect. The storage
 * owner is quiescent. Never rewrite ownership, audio, or a durable file ACK. */
static esp_err_t load_recovery_session(const char *path, const char *id, sd_audio_session_t *session)
{
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    const catalog_session_key_t key = {.id = id, .recovery = true};
    const esp_err_t err = read_pair_matching(file, 0U, session, sizeof(*session),
        offsetof(sd_audio_session_t, crc), CATALOG_MAGIC, session_record_matches, &key);
    const int closed = fclose(file);
    return err == ESP_OK && closed != 0 ? ESP_FAIL : err;
}

static esp_err_t archive_unconfirmed_evidence(const sd_audio_session_t *session, uint32_t index, bool *proven)
{
    *proven = false;
    sd_audio_segment_t part;
    const esp_err_t read = sd_audio_catalog_read(session->session_id, index, &part);
    if (read == ESP_FAIL) return read;
    if (read != ESP_OK || !part.sealed || part.acknowledged || part.missing || !part.path[0]) return ESP_OK;
    FILE *file = fopen(part.path, "rb");
    if (!file) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    uint8_t header[96], footer[32];
    bool valid = fread(header, 1U, sizeof(header), file) == sizeof(header) &&
        !memcmp(header, "NWXOFF1", 7U) && header[8] == 1U && !header[9] &&
        header[10] == 96U && !header[11] && header[12] == session->mode &&
        header[13] == 1U && u32(header + 16U) == 16000U &&
        u32(header + 24U) == index && memchr(header + 36U, 0, 48U) &&
        !strcmp((const char *)header + 36U, session->session_id);
    bool io_failed = false;
    if (valid && fseek(file, 0, SEEK_END) != 0) { valid = false; io_failed = true; }
    const long bytes = valid ? ftell(file) : -1;
    if (valid && bytes < 0) io_failed = true;
    valid = valid && bytes >= 96 + 666 + 32 &&
        (uint64_t)bytes == part.total_bytes;
    if (valid && fseek(file, -32L, SEEK_END) != 0) { valid = false; io_failed = true; }
    valid = valid && fread(footer, 1U, sizeof(footer), file) == sizeof(footer) &&
        !memcmp(footer, "NWXEND1", 7U) && u32(footer + 12U) > 0U &&
        (uint64_t)bytes == 96ULL + (uint64_t)u32(footer + 12U) * 666ULL + 32ULL;
    if (ferror(file)) io_failed = true;
    if (fclose(file) != 0) io_failed = true;
    if (io_failed) return ESP_FAIL;
    /* This proves a source exists, not that it can be deleted. The replay
     * reader still verifies all PCM records/CRC before the commit/ACK. */
    *proven = valid;
    return ESP_OK;
}

static esp_err_t first_existing_index(const sd_audio_session_t *session, uint32_t *index)
{
    bool paged = false;
    esp_err_t err = catalog_is_paged(session->session_id, &paged);
    if (err != ESP_OK) return err;
    *index = 0U;
    if (!paged) return ESP_OK;
    uint32_t candidate = session->segment_count;
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) return ESP_FAIL;
    char prefix[64]; snprintf(prefix, sizeof(prefix), "%s.index.", session->session_id);
    const size_t length = strlen(prefix);
    struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) err = ESP_FAIL; break; }
        if (strncmp(entry->d_name, prefix, length) || strlen(entry->d_name + length) != 8U) continue;
        char *end = NULL;
        const unsigned long page = strtoul(entry->d_name + length, &end, 16);
        if (!end || *end || page >= (session->segment_count + CATALOG_PAGE_RECORDS - 1U) / CATALOG_PAGE_RECORDS) continue;
        const uint32_t first = (uint32_t)page * CATALOG_PAGE_RECORDS;
        if (first < candidate) candidate = first;
    }
    if (closedir(dir) != 0) err = ESP_FAIL;
    if (err == ESP_OK) *index = candidate;
    return err;
}

static esp_err_t repair_pending_session(sd_audio_session_t *session, bool quarantined, bool *repaired)
{
    *repaired = false;
    if (!session->segment_count || (!quarantined && !session->done && !session->first_pending)) return ESP_OK;
    uint32_t first = 0U;
    esp_err_t err = first_existing_index(session, &first);
    if (err != ESP_OK || first >= session->segment_count) return err;
    /* A cursor/done flag is exactly the metadata under investigation. ACKed
     * endpoints cannot prove that the middle of a scene was uploaded. Inspect
     * the entire skipped prefix (or all retained entries for done/quarantine)
     * before pruning any index pages. Existing file ACKs remain authoritative. */
    const uint32_t limit = quarantined || session->done ||
        session->first_pending > session->segment_count
        ? session->segment_count : session->first_pending;
    for (uint32_t i = first; i < limit; ++i) {
#ifdef ESP_PLATFORM
        if (i != first && ((i - first) % CATALOG_INDEX_SLICE) == 0U) vTaskDelay(1U);
#endif
        bool proven = false;
        err = archive_unconfirmed_evidence(session, i, &proven);
        /* A physical read failure cannot authorize pruning skipped pages or
         * remembering a scene as done. Retry recovery with the source intact. */
        if (err != ESP_OK) return err;
        if (!proven) continue;
        session->done = 0U;
        session->first_pending = first;
        /* Do not promote an uncertain recording to complete. Existing holes,
         * MIC failures and retention gaps must retain their honest meaning. */
        session->complete = 0U;
        *repaired = true;
        return ESP_OK;
    }
    return ESP_OK;
}

static void note_repaired_session(const sd_audio_session_t *session)
{
    ++catalog_stats.repaired_sessions;
    done_cache_forget(session->session_id);
    invalidate_replay_picks();
#ifdef ESP_PLATFORM
    ESP_LOGW("sd_audio_catalog", "restored unconfirmed scene=%s first_pending=%lu; owner and file ACKs retained",
        session->session_id, (unsigned long)session->first_pending);
#endif
}

static esp_err_t persist_repaired_session(const char *path, sd_audio_session_t *session)
{
    FILE *file = fopen(path, "r+b");
    if (!file) return ESP_FAIL;
    sd_audio_session_t latest;
    const catalog_session_key_t key = {.id = session->session_id, .expected = session, .recovery = true};
    esp_err_t err = read_pair_matching(file, 0U, &latest, sizeof(latest),
        offsetof(sd_audio_session_t, crc), CATALOG_MAGIC, session_record_matches, &key);
    if (err == ESP_OK) {
        session->generation = latest.generation;
        err = write_pair(file, 0U, session, sizeof(*session), offsetof(sd_audio_session_t, crc));
    }
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK) note_repaired_session(session);
    return err;
}

static esp_err_t restore_quarantined_sessions_filtered(const char *only_id)
{
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    esp_err_t result = ESP_OK;
    struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) result = ESP_FAIL; break; }
        const char *suffix = strstr(entry->d_name, ".session.bad.");
        if (!suffix || suffix == entry->d_name || (size_t)(suffix - entry->d_name) >= 48U) continue;
        char *end = NULL;
        const unsigned long number = strtoul(suffix + 13U, &end, 10);
        if (!end || end == suffix + 13U || *end || number >= 100U) continue;
        char id[48] = {0}; memcpy(id, entry->d_name, (size_t)(suffix - entry->d_name));
        if (only_id && strcmp(only_id, id)) continue;
        char target[128], source[128], temporary[128];
        if (!catalog_path(target, id, "session") || !catalog_path(source, id, suffix + 1U) ||
            !catalog_path(temporary, id, "repairing")) continue;
        struct stat st;
        if (stat(target, &st) == 0) continue;
        if (errno != ENOENT) { result = ESP_FAIL; break; }
        sd_audio_session_t session;
        esp_err_t err = load_recovery_session(source, id, &session);
        if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_SIZE) continue;
        if (err != ESP_OK) { result = err; break; }
        bool repaired = false;
        err = repair_pending_session(&session, true, &repaired);
        if (err != ESP_OK) { result = err; break; }
        if (!repaired) continue;
        FILE *file = fopen(temporary, "wb");
        if (!file) { result = ESP_FAIL; break; }
        session.generation = 0U;
        err = write_pair(file, 0U, &session, sizeof(session), offsetof(sd_audio_session_t, crc));
        if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
        if (err == ESP_OK && rename(temporary, target) != 0) err = ESP_FAIL;
        if (err != ESP_OK) { result = err; break; }
        note_repaired_session(&session);
        /* The quarantined source remains available as recovery evidence. */
    }
    if (closedir(dir) != 0 && result == ESP_OK) result = ESP_FAIL;
    return result;
}

static esp_err_t restore_quarantined_sessions(void)
{
    return restore_quarantined_sessions_filtered(NULL);
}
