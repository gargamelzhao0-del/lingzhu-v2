/* Same FILE/FatFs operations as the catalog. No audio file, binding secret,
 * scene identity or uploaded object is created by the scratch qualification. */
static esp_err_t catalog_verify_session_file(const char *path, const sd_audio_session_t *expected);
static esp_err_t catalog_seed_session(const char *path, sd_audio_session_t *session);

typedef struct {
    uint32_t generation, magic, stored_crc, computed_crc;
    bool full, valid;
} catalog_slot_sample_t;
typedef struct {
    long file_bytes;
    size_t read_bytes;
    int stat_errno, seek_errno, read_errno;
    catalog_slot_sample_t slot[2];
} catalog_pair_sample_t;

static inline const char *catalog_metadata_phase(const char *path)
{
    const char *suffix = strrchr(path, '.');
    if (!suffix) return "unknown";
    if (!strcmp(suffix, ".creating")) return "creating";
    if (!strcmp(suffix, ".session")) return "published";
    if (!strcmp(suffix, ".repairing")) return "repairing";
    return "scratch";
}

/* A second read is evidence only: it never changes the original verdict.
 * Keep it in a separate frame and off the normal metadata/upload path. */
static void catalog_decode_pair(const uint8_t *copies, size_t bytes,
    size_t crc_offset, uint32_t magic, catalog_pair_sample_t *sample)
{
    for (unsigned i = 0; i < 2U; ++i) {
        catalog_slot_sample_t *slot = &sample->slot[i];
        slot->full = sample->read_bytes >= (i + 1U) * bytes;
        if (!slot->full) continue;
        const uint8_t *record = copies + i * bytes;
        memcpy(&slot->generation, record, sizeof(uint32_t));
        memcpy(&slot->magic, record + sizeof(uint32_t), sizeof(uint32_t));
        memcpy(&slot->stored_crc, record + crc_offset, sizeof(uint32_t));
        slot->computed_crc = checksum(record, crc_offset);
        slot->valid = slot->generation && slot->magic == magic &&
            slot->stored_crc == slot->computed_crc;
    }
}

static __attribute__((noinline)) void catalog_sample_pair(FILE *file, uint64_t offset,
    size_t bytes, size_t crc_offset, uint32_t magic, catalog_pair_sample_t *sample)
{
    memset(sample, 0, sizeof(*sample)); sample->file_bytes = -1L;
    struct stat st;
    errno = 0;
    if (fstat(fileno(file), &st) == 0) sample->file_bytes = (long)st.st_size;
    else sample->stat_errno = errno;
    if (bytes > sizeof(sd_audio_segment_t) || crc_offset + sizeof(uint32_t) > bytes ||
        offset > 0x7fffffffU) return;
    _Alignas(sd_audio_segment_t) uint8_t copies[2U * sizeof(sd_audio_segment_t)] = {0};
    clearerr(file); errno = 0;
    if (fseek(file, (long)offset, SEEK_SET) != 0) { sample->seek_errno = errno; return; }
    errno = 0;
    sample->read_bytes = fread(copies, 1U, 2U * bytes, file);
    if (ferror(file)) sample->read_errno = errno ? errno : EIO;
    catalog_decode_pair(copies, bytes, crc_offset, magic, sample);
}

#ifdef ESP_PLATFORM
/* Failure-only independent descriptor: bypass the FILE seek/read buffer,
 * while retaining the VFS/FatFs and normal SD ownership barriers. Read-only;
 * never promotes a failed verdict or modifies any archive/manifest. */
static __attribute__((noinline)) void catalog_sample_descriptor(const char *path, uint64_t offset,
    size_t bytes, size_t crc_offset, uint32_t magic, catalog_pair_sample_t *sample)
{
    memset(sample, 0, sizeof(*sample)); sample->file_bytes = -1L;
    if (bytes > sizeof(sd_audio_segment_t) || crc_offset + sizeof(uint32_t) > bytes ||
        offset > 0x7fffffffU) return;
    errno = 0;
    int flags = O_RDONLY;
#ifdef _WIN32
    flags |= O_BINARY;
#endif
    const int fd = open(path, flags);
    if (fd < 0) { sample->read_errno = errno; return; }
    struct stat st;
    if (fstat(fd, &st) == 0) sample->file_bytes = (long)st.st_size;
    else sample->stat_errno = errno;
    _Alignas(sd_audio_segment_t) uint8_t copies[2U * sizeof(sd_audio_segment_t)] = {0};
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) sample->seek_errno = errno;
    else {
        const ssize_t got = read(fd, copies, 2U * bytes);
        if (got < 0) sample->read_errno = errno;
        else sample->read_bytes = (size_t)got;
    }
    if (close(fd) != 0 && !sample->read_errno) sample->read_errno = errno;
    catalog_decode_pair(copies, bytes, crc_offset, magic, sample);
}
#endif

static __attribute__((noinline)) void catalog_report_readback(FILE *file, const char *path,
    const char *id, uint64_t offset, const void *expected, const void *actual, bool selected,
    size_t bytes, size_t crc_offset, uint32_t magic, esp_err_t original_error)
{
    const int saved_errno = errno;
    catalog_pair_sample_t sample;
    catalog_sample_pair(file, offset, bytes, crc_offset, magic, &sample);
    uint32_t expected_generation, selected_generation = 0U, expected_crc, selected_crc = 0U;
    memcpy(&expected_generation, expected, sizeof(expected_generation));
    memcpy(&expected_crc, (const uint8_t *)expected + crc_offset, sizeof(expected_crc));
    uint32_t difference = UINT32_MAX;
    if (selected) {
        memcpy(&selected_generation, actual, sizeof(selected_generation));
        memcpy(&selected_crc, (const uint8_t *)actual + crc_offset, sizeof(selected_crc));
        for (size_t i = 0; i < crc_offset + sizeof(uint32_t); ++i) {
            if (((const uint8_t *)expected)[i] != ((const uint8_t *)actual)[i]) {
                difference = (uint32_t)i; break;
            }
        }
    }
#ifdef ESP_PLATFORM
    ESP_LOGE("sd_audio_catalog", "SD metadata readback revision=199 stage=%s session=%s offset=%lu record_bytes=%u first_selected=%u expected_gen=%lu selected_gen=%lu diff_offset=%lu expected_crc=%08lx selected_crc=%08lx first_err=%s sample=retry file_bytes=%ld read_bytes=%u stat_errno=%d seek_errno=%d read_errno=%d",
        catalog_metadata_phase(path), id, (unsigned long)offset, (unsigned)bytes,
        selected ? 1U : 0U, (unsigned long)expected_generation, (unsigned long)selected_generation,
        (unsigned long)difference, (unsigned long)expected_crc, (unsigned long)selected_crc, esp_err_to_name(original_error), sample.file_bytes,
        (unsigned)sample.read_bytes, sample.stat_errno, sample.seek_errno, sample.read_errno);
    for (unsigned i = 0; i < 2U; ++i) {
        const catalog_slot_sample_t *slot = &sample.slot[i];
        ESP_LOGE("sd_audio_catalog", "SD metadata slot=%u full=%u valid=%u gen=%lu magic=%08lx stored_crc=%08lx computed_crc=%08lx sample=retry",
            i, slot->full ? 1U : 0U, slot->valid ? 1U : 0U,
            (unsigned long)slot->generation, (unsigned long)slot->magic,
            (unsigned long)slot->stored_crc, (unsigned long)slot->computed_crc);
    }
    /* Reuse the snapshot to avoid an additional task-stack-sized sample. */
    catalog_sample_descriptor(path, offset, bytes, crc_offset, magic, &sample);
    ESP_LOGE("sd_audio_catalog", "SD metadata descriptor revision=205 stage=%s read_bytes=%u file_bytes=%ld stat_errno=%d seek_errno=%d io_errno=%d slot0_valid=%u slot0_gen=%lu slot0_crc=%08lx slot1_valid=%u slot1_gen=%lu slot1_crc=%08lx",
        catalog_metadata_phase(path), (unsigned)sample.read_bytes, sample.file_bytes,
        sample.stat_errno, sample.seek_errno, sample.read_errno,
        sample.slot[0].valid, (unsigned long)sample.slot[0].generation, (unsigned long)sample.slot[0].stored_crc,
        sample.slot[1].valid, (unsigned long)sample.slot[1].generation, (unsigned long)sample.slot[1].stored_crc);
#else
    (void)path; (void)id; (void)expected_generation; (void)selected_generation;
    (void)difference; (void)original_error; (void)expected_crc; (void)selected_crc;
#endif
    errno = saved_errno;
}

static esp_err_t catalog_probe_update(const char *path, sd_audio_session_t *expected)
{
    FILE *file = fopen(path, "r+b");
    if (!file) return ESP_FAIL;
    sd_audio_session_t actual = {0};
    const catalog_session_key_t key = {.id = expected->session_id, .expected = expected, .diagnostics = true};
    esp_err_t err = read_pair_matching(file, 0U, &actual, sizeof(actual),
        offsetof(sd_audio_session_t, crc), CATALOG_MAGIC, session_record_matches, &key);
    const bool selected = err == ESP_OK;
    if (selected && memcmp(&actual, expected, offsetof(sd_audio_session_t, crc) + sizeof(actual.crc)))
        err = ESP_ERR_INVALID_CRC;
    if (err != ESP_OK) catalog_report_readback(file, path, expected->session_id, 0U,
        expected, &actual, selected, sizeof(actual), offsetof(sd_audio_session_t, crc), CATALOG_MAGIC, err);
    if (err == ESP_OK) {
        ++expected->segment_count;
        err = write_pair(file, 0U, expected, sizeof(*expected), offsetof(sd_audio_session_t, crc));
    }
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err == ESP_OK ? catalog_verify_session_file(path, expected) : err;
}

static __attribute__((noinline)) esp_err_t catalog_probe_index(const char *path)
{
    sd_audio_segment_t expected = {.magic = SEGMENT_MAGIC, .index = 1U, .realtime = 1U};
    strcpy(expected.path, "/sdcard/metadata_probe_only");
    expected.start_ms = 0x0102030405060708ULL ^ metadata_probe_ticket;
    expected.end_ms = expected.start_ms + 20U;
    const uint64_t offset = 2U * sizeof(expected);
    FILE *file = fopen(path, "w+b");
    if (!file) return ESP_FAIL;
    esp_err_t err = write_pair(file, offset, &expected, sizeof(expected), offsetof(sd_audio_segment_t, crc));
    if (err == ESP_OK) err = write_pair(file, offset, &expected, sizeof(expected), offsetof(sd_audio_segment_t, crc));
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err != ESP_OK) return err;
    file = fopen(path, "rb");
    if (!file) return ESP_FAIL;
    uint8_t gap[64];
    for (size_t at = 0U; at < offset && err == ESP_OK;) {
        const size_t want = offset - at < sizeof(gap) ? (size_t)(offset - at) : sizeof(gap);
        if (fread(gap, 1U, want, file) != want) { err = ESP_FAIL; break; }
        for (size_t i = 0U; i < want; ++i) if (gap[i]) { err = ESP_ERR_INVALID_CRC; break; }
        at += want;
    }
    sd_audio_segment_t actual = {0}; bool selected = false;
    if (err == ESP_OK) {
        err = read_pair(file, offset, &actual, sizeof(actual), offsetof(sd_audio_segment_t, crc), SEGMENT_MAGIC);
        selected = err == ESP_OK;
        if (selected && memcmp(&actual, &expected, offsetof(sd_audio_segment_t, crc) + sizeof(actual.crc)))
            err = ESP_ERR_INVALID_CRC;
    }
    if (err != ESP_OK) catalog_report_readback(file, path, "metadata_index_probe", offset,
        &expected, &actual, selected, sizeof(actual), offsetof(sd_audio_segment_t, crc), SEGMENT_MAGIC, err);
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

esp_err_t sd_audio_catalog_qualify_storage(void)
{
    if (sd_audio_catalog_replay_pinned()) return ESP_ERR_NOT_FINISHED;
    metadata_io_qualified = false;
    esp_err_t err = ensure_catalog_directory();
    if (err != ESP_OK) return err;
    char path[128], moved[128]; bool owned = false, moved_owned = false;
    const char *stage = "reserve";
    for (unsigned attempt = 0U; attempt < 4U; ++attempt) {
        if (!++metadata_probe_ticket) ++metadata_probe_ticket;
        const int a = snprintf(path, sizeof(path), "%s/.nuwax_meta_%08lx.creating",
            SD_AUDIO_CATALOG_ROOT, (unsigned long)metadata_probe_ticket);
        const int b = snprintf(moved, sizeof(moved), "%s/.nuwax_meta_%08lx.checked",
            SD_AUDIO_CATALOG_ROOT, (unsigned long)metadata_probe_ticket);
        if (a < 0 || b < 0 || (size_t)a >= sizeof(path) || (size_t)b >= sizeof(moved)) return ESP_ERR_INVALID_SIZE;
        errno = 0;
        const int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) { if (errno == EEXIST) continue; return ESP_FAIL; }
        owned = true;
        if (close(fd) != 0) { err = ESP_FAIL; break; }
        struct stat st; errno = 0;
        const int target = stat(moved, &st), target_errno = errno;
        if (target != 0 && target_errno == ENOENT) break;
        if (unlink(path) != 0) { err = ESP_FAIL; break; }
        owned = false;
        if (target != 0) { errno = target_errno; return ESP_FAIL; }
    }
    if (!owned) return ESP_ERR_NOT_FINISHED;
    uint64_t challenge = metadata_probe_ticket;
#ifdef ESP_PLATFORM
    challenge ^= ((uint64_t)esp_random() << 32U) | esp_random();
    nuwax_sd_io_probe_t witness = {0};
    const bool observed = nuwax_sd_io_probe_begin(&witness);
    const bool trace_observed = observed && nuwax_sd_probe_trace_begin();
#endif
    sd_audio_session_t expected = {.complete = 1U, .boot_id = (uint32_t)challenge,
        .start_ms = 0x0102030405060708ULL ^ challenge};
    strcpy(expected.session_id, "sd_metadata_selftest");
    memset(expected.owner, 0x5a, sizeof(expected.owner)); /* Test bytes, never a binding. */
    if (err == ESP_OK) { stage = "seed"; err = catalog_seed_session(path, &expected); }
    for (unsigned i = 0U; i < 3U && err == ESP_OK; ++i) {
        stage = "read_update"; err = catalog_probe_update(path, &expected);
    }
    if (err == ESP_OK) {
        stage = "rename";
        if (rename(path, moved) != 0) err = ESP_FAIL;
        else { owned = false; moved_owned = true; }
    }
    if (err == ESP_OK) { stage = "renamed_readback"; err = catalog_verify_session_file(moved, &expected); }
    if (err == ESP_OK) { stage = "sparse_index"; err = catalog_probe_index(moved); }
    int fault_errno = err == ESP_FAIL ? errno : 0, cleanup_errno = 0;
#ifdef ESP_PLATFORM
    if (observed) nuwax_sd_io_probe_end();
    if (trace_observed) nuwax_sd_probe_trace_finish("metadata", err,
        witness.first_bad_sector, witness.first_expected_crc, witness.first_read_crc);
    ESP_LOGI("sd_audio_catalog", "SD IO witness revision=205 scope=metadata observed=%u reads=%lu writes=%lu sdk_errors=%lu witnessed=%lu matches=%lu mismatches=%lu first_sector=%llu expected_crc=%08lx read_crc=%08lx",
        observed, (unsigned long)witness.reads, (unsigned long)witness.writes, (unsigned long)witness.sdk_errors,
        (unsigned long)witness.write_witnesses, (unsigned long)witness.read_matches, (unsigned long)witness.read_mismatches,
        (unsigned long long)witness.first_bad_sector, (unsigned long)witness.first_expected_crc, (unsigned long)witness.first_read_crc);
#endif
    /* Only our O_EXCL-reserved scratch inode(s) may be deleted. */
    if (owned && unlink(path) != 0) cleanup_errno = errno;
    if (moved_owned && unlink(moved) != 0) cleanup_errno = errno;
    if (cleanup_errno && err == ESP_OK) { stage = "cleanup"; err = ESP_FAIL; fault_errno = cleanup_errno; }
    metadata_io_qualified = err == ESP_OK;
#ifdef ESP_PLATFORM
    ESP_LOGI("sd_audio_catalog", "SD metadata qualification revision=199 stage=%s session_bytes=%u index_bytes=%u err=%s errno=%d cleanup_errno=%d challenge=%016llx; exclusive scratch only",
        stage, (unsigned)sizeof(sd_audio_session_t), (unsigned)sizeof(sd_audio_segment_t), esp_err_to_name(err), fault_errno, cleanup_errno,
        (unsigned long long)challenge);
#else
    (void)stage; (void)fault_errno;
#endif
    return err;
}
