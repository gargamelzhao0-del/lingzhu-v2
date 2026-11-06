/* Included inside sd_audio_catalog.c, after its durable index primitives. */
#ifndef SD_AUDIO_ARCHIVE_ROOT
#define SD_AUDIO_ARCHIVE_ROOT "/sdcard/offline_audio"
#endif
#ifndef SD_AUDIO_ARCHIVE_NEW_ROOT
#define SD_AUDIO_ARCHIVE_NEW_ROOT "/sdcard/offline2"
#endif
static char pinned_id[48];
static uint32_t pinned_index;
static uint32_t pinned_scan_cursor;
static uint64_t lease_sequence;
static struct {
    sd_audio_lease_token_t token;
    sd_audio_lease_owner_t owner;
    uint32_t media_generation, capture_generation;
    sd_audio_session_t session;
    sd_audio_segment_t segment;
} read_lease;
static struct {
    DIR *dir;
    uint8_t owner[32];
    sd_audio_session_t oldest;
    sd_audio_segment_t candidate;
    uint32_t order;
    bool found;
} retention_scan;

static struct {
    DIR *dir;
    uint8_t owner[32];
    char id[48];
    uint32_t next, revision;
} confirmed_scan;
static bool retention_pinned(const char *id, uint32_t index);
static esp_err_t retention_validate(const char *id, const sd_audio_segment_t *segment, uint64_t *bytes);

static void confirmed_scan_reset(void)
{
    if (confirmed_scan.dir) closedir(confirmed_scan.dir);
    confirmed_scan.dir = NULL;
    confirmed_scan.id[0] = '\0';
}

/* Storage lock held. Cursor survives bounded passes; no missing/retirement
 * markers are created by this policy and no unconfirmed file is removed. */
esp_err_t sd_audio_catalog_reclaim_confirmed(const uint8_t owner[32], uint64_t *removed_bytes,
                                           char removed_path[128])
{
    if (!owner || !removed_bytes || !removed_path) return ESP_ERR_INVALID_ARG;
    *removed_bytes = 0U; removed_path[0] = '\0';
    if (confirmed_scan.dir && (memcmp(owner, confirmed_scan.owner, 32U) ||
        confirmed_scan.revision != sd_audio_catalog_live_revision())) confirmed_scan_reset();
    if (!confirmed_scan.dir) {
        confirmed_scan.dir = opendir(SD_AUDIO_CATALOG_ROOT);
        if (!confirmed_scan.dir) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        memcpy(confirmed_scan.owner, owner, 32U);
        confirmed_scan.revision = sd_audio_catalog_live_revision();
    }
    unsigned entries = 0U, manifests = 0U, records = 0U;
    for (;;) {
        if (!confirmed_scan.id[0]) {
            if (entries >= 32U || manifests >= 8U) return ESP_ERR_NOT_FINISHED;
            errno = 0;
            struct dirent *entry = readdir(confirmed_scan.dir);
            if (!entry) {
                const esp_err_t err = errno ? ESP_FAIL : ESP_ERR_NOT_FOUND;
                confirmed_scan_reset(); return err;
            }
            ++entries;
            const size_t n = strlen(entry->d_name);
            if (n <= 8U || n - 8U >= 48U || strcmp(entry->d_name + n - 8U, ".session")) continue;
            memcpy(confirmed_scan.id, entry->d_name, n - 8U);
            confirmed_scan.id[n - 8U] = '\0'; confirmed_scan.next = 0U;
        }
        if (manifests >= 8U) return ESP_ERR_NOT_FINISHED;
        ++manifests;
        sd_audio_session_t session;
        esp_err_t err = sd_audio_catalog_load(confirmed_scan.id, &session);
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_STATE) {
            confirmed_scan.id[0] = '\0'; continue;
        }
        if (err != ESP_OK) { confirmed_scan_reset(); return err; }
        if (session.mode != 1U || memcmp(owner, session.owner, 32U) || session_blocked(session.session_id)) {
            confirmed_scan.id[0] = '\0'; continue;
        }
        if (confirmed_scan.next < session.first_pending) confirmed_scan.next = session.first_pending;
        while (confirmed_scan.next < session.segment_count) {
            if (records >= CATALOG_INDEX_SLICE) return ESP_ERR_NOT_FINISHED;
            ++records;
            sd_audio_segment_t part;
            err = sd_audio_catalog_read(session.session_id, confirmed_scan.next++, &part);
            if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_STATE) continue;
            if (err != ESP_OK) { confirmed_scan_reset(); return err; }
            if (!part.acknowledged || part.missing || !(part.sealed & 1U) ||
                retention_pinned(session.session_id, part.index)) continue;
            uint64_t bytes = 0U;
            err = retention_validate(session.session_id, &part, &bytes);
            if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) continue;
            if (err != ESP_OK) { confirmed_scan_reset(); return err; }
            err = remove_acknowledged(session.session_id, &part);
            if (err != ESP_OK) { confirmed_scan_reset(); return err; }
            sd_audio_catalog_note_deleted(session.session_id, part.index);
            confirmed_scan.revision = sd_audio_catalog_live_revision();
            *removed_bytes = bytes;
            memcpy(removed_path, part.path, 128U);
            return ESP_OK;
        }
        confirmed_scan.id[0] = '\0';
    }
}

static void retention_scan_reset(void)
{
    if (retention_scan.dir) closedir(retention_scan.dir);
    retention_scan.dir = NULL; retention_scan.found = false;
}

static bool retention_pinned(const char *id, uint32_t index)
{
    return pinned_id[0] && pinned_index == index && !strcmp(pinned_id, id);
}
bool sd_audio_catalog_replay_pinned(void) { return pinned_id[0] != '\0'; }

esp_err_t sd_audio_catalog_pin(const char *id, uint32_t index)
{
    if (!valid_id(id) || pinned_id[0]) return ESP_ERR_INVALID_STATE;
    retention_scan_reset();
    strcpy(pinned_id, id); pinned_index = index; pinned_scan_cursor = 0;
    return ESP_OK;
}
void sd_audio_catalog_unpin(void)
{
    /* Old single-reader callers must not release a new reader's lease. */
    if (read_lease.token) return;
    if (pinned_id[0]) retention_scan_reset();
    pinned_id[0] = '\0'; pinned_scan_cursor = 0;
}

esp_err_t sd_audio_catalog_lease_acquire(const sd_audio_session_t *session,
    const sd_audio_segment_t *segment, sd_audio_lease_owner_t owner,
    uint32_t media_generation, uint32_t capture_generation, sd_audio_lease_token_t *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    *token = 0U;
    if (!session || !segment || !valid_id(session->session_id) ||
        !(segment->sealed & 1U) || segment->missing || segment->acknowledged ||
        (owner != SD_AUDIO_LEASE_REPLAY && owner != SD_AUDIO_LEASE_VERIFY))
        return ESP_ERR_INVALID_ARG;
    if (pinned_id[0]) return ESP_ERR_NOT_FINISHED;
    /* Exhaustion fails closed instead of making a stale token valid again. */
    if (lease_sequence == UINT64_MAX) return ESP_ERR_INVALID_STATE;
    retention_scan_reset();
    strcpy(pinned_id, session->session_id); pinned_index = segment->index;
    pinned_scan_cursor = 0U;
    read_lease.token = ++lease_sequence;
    read_lease.owner = owner;
    read_lease.media_generation = media_generation;
    read_lease.capture_generation = capture_generation;
    read_lease.session = *session; read_lease.segment = *segment;
    *token = read_lease.token;
    return ESP_OK;
}

bool sd_audio_catalog_lease_matches(sd_audio_lease_token_t token,
    sd_audio_lease_owner_t owner, uint32_t media_generation)
{
    return token && read_lease.token == token && read_lease.owner == owner &&
        read_lease.media_generation == media_generation && pinned_id[0];
}

esp_err_t sd_audio_catalog_lease_release(sd_audio_lease_token_t token,
                                       sd_audio_lease_owner_t owner)
{
    if (!token || read_lease.token != token || read_lease.owner != owner)
        return ESP_ERR_INVALID_STATE;
    memset(&read_lease, 0, sizeof(read_lease));
    sd_audio_catalog_unpin();
    return ESP_OK;
}

static bool retention_under_root(const char *path, const char *root)
{
    const size_t length = strlen(root);
    return !strncmp(path, root, length) && path[length] == '/';
}

/* Restrict deletion to a known archive root, an immutable ready file and its
 * CRC-protected catalog identity. A damaged/unknown record fails closed. */
static bool retention_path(const char *path, const char *id, uint32_t *order)
{
    if ((!retention_under_root(path, SD_AUDIO_ARCHIVE_ROOT) &&
         !retention_under_root(path, SD_AUDIO_ARCHIVE_NEW_ROOT)) ||
        strstr(path, "..") || strchr(path, '\\')) return false;
    const char *name = strrchr(path, '/');
    if (!name) return false;
    ++name;
    char prefix[64]; snprintf(prefix, sizeof(prefix), "card_%s_", id);
    const size_t length = strlen(prefix);
    if (strncmp(name, prefix, length)) return false;
    const char *number = name + length;
    if (*number < '0' || *number > '9') return false;
    char *end;
    errno = 0;
    const unsigned long value = strtoul(number, &end, 10);
    if (errno || value > UINT32_MAX || strcmp(end, ".ready")) return false;
    *order = (uint32_t)value;
    return true;
}

static esp_err_t retention_validate(const char *id, const sd_audio_segment_t *segment, uint64_t *bytes)
{
    uint32_t order;
    if (!retention_path(segment->path, id, &order)) return ESP_ERR_INVALID_STATE;
    struct stat st;
    if (stat(segment->path, &st) != 0) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    FILE *file = fopen(segment->path, "rb");
    if (!file) return ESP_FAIL;
    uint8_t header[96];
    const size_t got = fread(header, 1, sizeof(header), file);
    const bool failed = ferror(file) != 0;
    const int closed = fclose(file);
    uint32_t index = 0;
    if (got == sizeof(header)) memcpy(&index, header + 24U, sizeof(index));
    if (failed || closed != 0) return ESP_FAIL;
    if (got != sizeof(header) || memcmp(header, "NWXOFF1\0", 8U) ||
        header[8] != 1U || header[9] || header[10] != 96U || header[11] ||
        header[12] != 1U || header[22] != 2U || header[23] || index != segment->index ||
        !memchr(header + 36U, 0, 48U) || strcmp((char *)header + 36U, id) ||
        st.st_size < 128 || (uint64_t)st.st_size != segment->total_bytes) return ESP_ERR_INVALID_STATE;
    *bytes = (uint64_t)st.st_size;
    return ESP_OK;
}

static esp_err_t finish_retirement(const char *id, sd_audio_segment_t *segment, uint64_t *removed_bytes)
{
    if (segment->missing != SD_AUDIO_SEGMENT_RETIRING || retention_pinned(id, segment->index))
        return ESP_ERR_INVALID_STATE;
    /* The newest manifest slot may be unreadable even though the segment's
     * durable retirement intent survived. Repair the old valid manifest
     * BEFORE unlink; every retry/recovery caller shares this ordering. */
    sd_audio_session_t session;
    esp_err_t err = sd_audio_catalog_load(id, &session);
    if (err != ESP_OK) return err;
    /* Rewrite even when the bit is already visible: a previous failed sync
     * can expose an unconfirmed value through the filesystem cache. */
    session.complete = 0;
    err = sd_audio_catalog_save(&session);
    if (err != ESP_OK) return err;
    uint64_t bytes = 0;
    err = retention_validate(id, segment, &bytes);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
    if (err == ESP_OK && remove(segment->path) != 0) return ESP_FAIL;
    if (removed_bytes) *removed_bytes = bytes;
    segment->missing = SD_AUDIO_SEGMENT_RETIRED;
    /* A failed post-unlink sync leaves RETIRING durable. Recovery retries the
     * idempotent unlink before permitting the historical cursor to advance. */
    return sd_audio_catalog_write(id, segment);
}

static esp_err_t retention_advance(sd_audio_session_t *session, uint32_t index)
{
    if (index != session->first_pending) return ESP_OK;
    ++session->first_pending;
    return sd_audio_catalog_save(session);
}

esp_err_t sd_audio_catalog_reclaim(const uint8_t owner[32], uint64_t *removed_bytes,
                                 char removed_path[128])
{
    if (!owner || !removed_bytes || !removed_path) return ESP_ERR_INVALID_ARG;
    *removed_bytes = 0; removed_path[0] = '\0';
    if (retention_scan.dir && memcmp(owner, retention_scan.owner, 32U)) retention_scan_reset();
    if (!retention_scan.dir) {
        retention_scan.dir = opendir(SD_AUDIO_CATALOG_ROOT);
        if (!retention_scan.dir) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        memcpy(retention_scan.owner, owner, 32U);
        retention_scan.found = false;
        retention_scan.order = UINT32_MAX;
    }
    esp_err_t result = ESP_OK;
    struct dirent *entry;
    unsigned entries = 0, manifests = 0;
    for (;;) {
        /* Keep the directory cursor across supervisor slices; reopening at
         * entry zero would starve forever on a card with many old scenes. */
        if (entries >= 32U || manifests >= 8U) return ESP_ERR_NOT_FINISHED;
        errno = 0;
        entry = readdir(retention_scan.dir);
        if (!entry) { if (errno) result = ESP_FAIL; break; }
        ++entries;
        const size_t n = strlen(entry->d_name);
        if (n <= 8U || n - 8U >= 48U || strcmp(entry->d_name + n - 8U, ".session")) continue;
        char id[48] = {0}; memcpy(id, entry->d_name, n - 8U);
        if (done_cache_skip(id) || session_blocked(id)) continue;
        ++manifests;
        sd_audio_session_t session;
        result = sd_audio_catalog_load(id, &session);
        /* A corrupt manifest owns unknown data. Never delete its files. */
        if (result == ESP_ERR_INVALID_CRC || result == ESP_ERR_INVALID_STATE) { result = ESP_OK; continue; }
        if (result != ESP_OK) break;
        if (session.mode != 1U || session.done || memcmp(owner, session.owner, 32U)) continue;
        unsigned budget = CATALOG_INDEX_SLICE;
        const uint32_t previous = session.first_pending;
        const bool was_complete = session.complete != 0;
        uint32_t first = session.first_pending;
        const bool pinned_scene = pinned_id[0] && !strcmp(pinned_id, id);
        if (pinned_scene && pinned_scan_cursor > first && pinned_scan_cursor <= session.segment_count)
            first = pinned_scan_cursor;
        for (uint32_t i = first; i < session.segment_count; ++i) {
            if (!budget) { result = ESP_ERR_NOT_FINISHED; break; }
            --budget;
            sd_audio_segment_t segment;
            result = sd_audio_catalog_read(id, i, &segment);
            if (result != ESP_OK) break; /* Do not guess at damaged metadata. */
            if (retention_pinned(id, i)) { pinned_scan_cursor = i + 1U; continue; }
            if (segment.missing == SD_AUDIO_SEGMENT_RETIRING) {
                strcpy(removed_path, segment.path);
                result = finish_retirement(id, &segment, removed_bytes);
                if (result == ESP_OK) {
                    strcpy(removed_path, segment.path);
                    result = retention_advance(&session, i);
                }
                retention_scan_reset(); return result;
            }
            if (segment.acknowledged || segment.missing) {
                /* Preserve evidence from an ordinary missing/retired record
                 * before advancing/pruning its index page. This also covers
                 * fallback to a manifest predating that missing record. */
                if (segment.missing) session.complete = 0;
                /* ACK deletion is normally performed by replay. It is safe to
                 * retry here, but never delete unknown/corrupt missing audio. */
                if (segment.acknowledged) {
                    result = remove_acknowledged(id, &segment);
                    if (result != ESP_OK) break;
                }
                if (i == session.first_pending) ++session.first_pending;
                if (pinned_scene) pinned_scan_cursor = i + 1U;
                continue;
            }
            if (!(segment.sealed & 1U)) continue;
            uint32_t order;
            if (!retention_path(segment.path, id, &order)) { result = ESP_ERR_INVALID_STATE; break; }
            if (!retention_scan.found || order < retention_scan.order) {
                retention_scan.oldest = session; retention_scan.candidate = segment;
                retention_scan.order = order; retention_scan.found = true;
            }
            break; /* Monotonic file numbering: this scene's oldest eligible file. */
        }
        if (session.first_pending != previous || (was_complete && !session.complete)) {
            const esp_err_t saved = sd_audio_catalog_save(&session);
            if (saved != ESP_OK) result = saved;
        }
        if (result != ESP_OK) break;
    }
    sd_audio_session_t oldest = retention_scan.oldest;
    sd_audio_segment_t candidate = retention_scan.candidate;
    const bool found = retention_scan.found;
    retention_scan_reset();
    if (result != ESP_OK) return result;
    if (!found) return ESP_ERR_NOT_FOUND;
    /* Replay or the writer may have changed metadata between scan slices. */
    result = sd_audio_catalog_load(oldest.session_id, &oldest);
    if (result == ESP_OK) result = sd_audio_catalog_read(oldest.session_id, candidate.index, &candidate);
    if (result != ESP_OK) return result;
    if (oldest.mode != 1U || oldest.done || memcmp(owner, oldest.owner, 32U) ||
        session_blocked(oldest.session_id) || retention_pinned(oldest.session_id, candidate.index) ||
        !(candidate.sealed & 1U) || candidate.acknowledged || candidate.missing) return ESP_ERR_NOT_FINISHED;
    uint64_t bytes;
    result = retention_validate(oldest.session_id, &candidate, &bytes);
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) return result;
    /* Both writes must succeed BEFORE removing any unconfirmed audio. */
    oldest.complete = 0;
    result = sd_audio_catalog_save(&oldest);
    if (result != ESP_OK) return result;
    candidate.missing = SD_AUDIO_SEGMENT_RETIRING;
    result = sd_audio_catalog_write(oldest.session_id, &candidate);
    if (result != ESP_OK) return result;
    strcpy(removed_path, candidate.path);
    result = finish_retirement(oldest.session_id, &candidate, removed_bytes);
    if (result == ESP_OK) {
        strcpy(removed_path, candidate.path);
        result = retention_advance(&oldest, candidate.index);
    }
    return result;
}
