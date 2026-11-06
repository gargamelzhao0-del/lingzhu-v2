/* Verification progress is an optional local hint. A new boot/remount always
 * rechecks audio, and the uploader always validates the original CRC again. */
#define VERIFY_HINT_MAGIC UINT32_C(0x56584631)
typedef struct {
    uint32_t magic, media_generation, boot_id, next;
    uint64_t nonce, start_ms;
    uint8_t owner[32];
    char id[48];
    uint32_t crc;
} catalog_verify_hint_t;
static struct { DIR *dir; char id[48]; uint32_t next; uint8_t owner[32];
    uint64_t nonce; uint32_t media_generation; esp_err_t error; } verify_scan;

void sd_audio_catalog_verify_reset(void)
{
    if (verify_scan.dir) closedir(verify_scan.dir);
    memset(&verify_scan, 0, sizeof(verify_scan));
}

static uint32_t catalog_verify_position(const sd_audio_session_t *session,
                                      uint64_t nonce, uint32_t media_generation)
{
    char path[128];
    if (!catalog_path(path, session->session_id, "verify")) return 0U;
    FILE *file = fopen(path, "rb");
    if (!file) return 0U;
    catalog_verify_hint_t hint;
    const bool read_ok = fread(&hint, 1U, sizeof(hint), file) == sizeof(hint);
    const bool closed = fclose(file) == 0;
    if (!read_ok || !closed || hint.magic != VERIFY_HINT_MAGIC || hint.nonce != nonce ||
        hint.media_generation != media_generation || hint.boot_id != session->boot_id ||
        hint.start_ms != session->start_ms || hint.next > session->segment_count ||
        memcmp(hint.owner, session->owner, 32U) ||
        memcmp(hint.id, session->session_id, sizeof(hint.id)) ||
        hint.crc != checksum(&hint, offsetof(catalog_verify_hint_t, crc))) return 0U;
    return hint.next;
}

esp_err_t sd_audio_catalog_verify_checkpoint(const sd_audio_session_t *session,
    uint64_t nonce, uint32_t media_generation, uint32_t next_index, sd_audio_lease_token_t token)
{
    if (!session || !nonce || next_index > session->segment_count) return ESP_ERR_INVALID_ARG;
    if (!sd_audio_catalog_lease_matches(token, SD_AUDIO_LEASE_VERIFY, media_generation) ||
        strcmp(read_lease.session.session_id, session->session_id) ||
        memcmp(read_lease.session.owner, session->owner, 32U) ||
        next_index != read_lease.segment.index + 1U) return ESP_ERR_INVALID_STATE;
    catalog_verify_hint_t hint = {.magic = VERIFY_HINT_MAGIC,
        .media_generation = media_generation, .boot_id = session->boot_id,
        .next = next_index, .nonce = nonce, .start_ms = session->start_ms};
    memcpy(hint.id, session->session_id, sizeof(hint.id));
    memcpy(hint.owner, session->owner, sizeof(hint.owner));
    hint.crc = checksum(&hint, offsetof(catalog_verify_hint_t, crc));
    char path[128], temporary[128];
    if (!catalog_path(path, session->session_id, "verify") ||
        !catalog_path(temporary, session->session_id, "verify.tmp")) return ESP_ERR_INVALID_ARG;
    /* This optional hint is never evidence of audio or an upload ACK. A torn
     * temp may be replaced only after all readers are quiescent/serialized. */
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return errno == ENOMEM ? ESP_ERR_NO_MEM :
        (errno == EMFILE || errno == ENFILE) ? ESP_ERR_NOT_FINISHED : ESP_FAIL;
    const bool wrote = write(fd, &hint, sizeof(hint)) == (ssize_t)sizeof(hint);
    const bool synced = wrote && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    if (!synced || !closed) return ESP_FAIL;
    /* FatFs rename does not replace an existing destination. Losing this
     * optional hint at either boundary only schedules a harmless recheck. */
    if (unlink(path) != 0 && errno != ENOENT) return ESP_FAIL;
    if (rename(temporary, path) != 0) return ESP_FAIL;
    if (!strcmp(verify_scan.id, session->session_id)) verify_scan.next = next_index;
    return ESP_OK;
}

esp_err_t sd_audio_catalog_verify_next(const uint8_t owner[32], uint64_t nonce,
    uint32_t media_generation, sd_audio_session_t *session, sd_audio_segment_t *segment)
{
    if (!owner || !nonce || !session || !segment) return ESP_ERR_INVALID_ARG;
    if (verify_scan.nonce && (verify_scan.nonce != nonce ||
        verify_scan.media_generation != media_generation || memcmp(verify_scan.owner, owner, 32U)))
        sd_audio_catalog_verify_reset();
    if (!verify_scan.dir) {
        verify_scan.dir = opendir(SD_AUDIO_CATALOG_ROOT);
        if (!verify_scan.dir) return errno == ENOENT ? ESP_ERR_NOT_FOUND :
            errno == ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
        verify_scan.nonce = nonce; verify_scan.media_generation = media_generation;
        memcpy(verify_scan.owner, owner, 32U);
    }
    unsigned entries = 0U, records = 0U;
    for (;;) {
        const bool new_scene = !verify_scan.id[0];
        if (new_scene) {
            if (++entries > 16U) return ESP_ERR_NOT_FINISHED;
            errno = 0;
            struct dirent *entry = readdir(verify_scan.dir);
            if (!entry) {
                const esp_err_t err = errno ? ESP_FAIL :
                    verify_scan.error != ESP_OK ? verify_scan.error : ESP_ERR_NOT_FOUND;
                sd_audio_catalog_verify_reset(); return err;
            }
            const size_t len = strlen(entry->d_name);
            if (len <= 8U || len - 8U >= sizeof(verify_scan.id) ||
                strcmp(entry->d_name + len - 8U, ".session")) continue;
            memcpy(verify_scan.id, entry->d_name, len - 8U);
            verify_scan.id[len - 8U] = 0;
        }
        esp_err_t err = sd_audio_catalog_load(verify_scan.id, session);
        if (err != ESP_OK) {
            verify_scan.id[0] = 0;
            /* A vanished scene is not the end of the directory. An unreadable
             * manifest must not starve healthy scenes or turn into a successful
             * inventory result; retain the first metadata error until EOF. */
            if (err == ESP_ERR_NOT_FOUND) continue;
            if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_STATE ||
                err == ESP_ERR_INVALID_SIZE) {
                if (verify_scan.error == ESP_OK) verify_scan.error = err;
                continue;
            }
            return err;
        }
        if (session->done || memcmp(session->owner, owner, 32U) || session_blocked(session->session_id)) {
            verify_scan.id[0] = 0; continue;
        }
        if (new_scene) verify_scan.next = catalog_verify_position(session, nonce, media_generation);
        if (verify_scan.next < session->first_pending) verify_scan.next = session->first_pending;
        while (verify_scan.next < session->segment_count) {
            if (++records > CATALOG_INDEX_SLICE) return ESP_ERR_NOT_FINISHED;
            err = sd_audio_catalog_read(session->session_id, verify_scan.next, segment);
            if (err != ESP_OK) return err == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_STATE : err;
            if (segment->acknowledged || segment->missing) { ++verify_scan.next; continue; }
            /* Do not advance a durable verification hint across an OPEN
             * reservation: it may seal later at exactly this index. */
            if (!(segment->sealed & 1U)) { verify_scan.id[0] = 0; break; }
            const int closed = closedir(verify_scan.dir); verify_scan.dir = NULL;
            if (closed != 0) return ESP_FAIL;
            return ESP_OK;
        }
        verify_scan.id[0] = 0;
    }
}
