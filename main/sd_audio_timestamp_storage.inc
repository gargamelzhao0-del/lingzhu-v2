/* Optional time records use the catalog's two-slot CRC/fsync transaction.
 * Never reinterpret old session/index records or rewrite audio bytes. */
#define SD_CLOCK_MAGIC 0x31434c53U
#define SD_TIME_MAGIC 0x31544d53U
typedef struct {
    uint32_t generation, magic, boot_id, reserved;
    uint8_t owner[32];
    nuwax_cloud_time_anchor_t anchor;
    uint32_t crc;
} sd_clock_record_t;
typedef struct {
    uint32_t generation, magic, boot_id, valid;
    uint8_t owner[32];
    uint64_t start_ms, started_at_ms, duration_ms;
    uint32_t crc;
} sd_time_record_t;

static bool clock_path(char path[128], uint32_t boot_id, const uint8_t owner[32])
{
    char id[48];
    uint32_t tag;
    memcpy(&tag, owner, sizeof(tag));
    snprintf(id, sizeof(id), "clock_%08lx_%08lx", (unsigned long)boot_id, (unsigned long)tag);
    return catalog_path(path, id, "clock");
}
static esp_err_t read_time_record(const char *path, void *record, size_t size,
    size_t crc_offset, uint32_t magic)
{
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    const esp_err_t err = read_pair(file, 0, record, size, crc_offset, magic);
    fclose(file);
    return err;
}
static esp_err_t save_time_record(const char *path, void *record, size_t size, size_t crc_offset)
{
    FILE *file = fopen(path, "r+b");
    if (!file && errno == ENOENT) file = fopen(path, "w+b");
    if (!file) return ESP_FAIL;
    const esp_err_t err = write_pair(file, 0, record, size, crc_offset);
    const int closed = fclose(file);
    return err == ESP_OK && closed != 0 ? ESP_FAIL : err;
}
esp_err_t sd_audio_catalog_clock_save(uint32_t boot_id, const uint8_t owner[32],
    const nuwax_cloud_time_anchor_t *anchor)
{
    char path[128];
    if (!boot_id || !owner || !anchor || !nuwax_cloud_time_valid(anchor->utc_ms) ||
        !clock_path(path, boot_id, owner)) return ESP_ERR_INVALID_ARG;
    sd_clock_record_t record = {0};
    const esp_err_t loaded = read_time_record(path, &record, sizeof(record), offsetof(sd_clock_record_t, crc), SD_CLOCK_MAGIC);
    if (loaded != ESP_OK && loaded != ESP_ERR_NOT_FOUND) return loaded;
    if (loaded == ESP_OK) {
        if (record.boot_id != boot_id || memcmp(record.owner, owner, 32)) return ESP_ERR_INVALID_STATE;
        if (record.anchor.uptime_ms >= anchor->uptime_ms) return ESP_OK;
    }
    record.magic = SD_CLOCK_MAGIC; record.boot_id = boot_id; record.anchor = *anchor;
    memcpy(record.owner, owner, 32);
    return save_time_record(path, &record, sizeof(record), offsetof(sd_clock_record_t, crc));
}

esp_err_t sd_audio_catalog_session_time(const sd_audio_session_t *session,
    uint32_t current_boot_id, const nuwax_cloud_time_anchor_t *anchor,
    bool stopped, uint64_t stopped_ms, sd_audio_session_time_t *out)
{
    char path[128];
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!session || !catalog_path(path, session->session_id, "time")) return ESP_ERR_INVALID_ARG;
    sd_time_record_t record = {0};
    const esp_err_t loaded = read_time_record(path, &record, sizeof(record), offsetof(sd_time_record_t, crc), SD_TIME_MAGIC);
    if (loaded != ESP_OK && loaded != ESP_ERR_NOT_FOUND) return loaded;
    const bool matched = loaded == ESP_OK && record.boot_id == session->boot_id &&
        record.start_ms == session->start_ms && !memcmp(record.owner, session->owner, 32);
    if (matched) {
        if (record.valid & ~(SD_AUDIO_TIME_STARTED | SD_AUDIO_TIME_DURATION)) return ESP_ERR_INVALID_STATE;
        if ((record.valid & SD_AUDIO_TIME_STARTED) && !nuwax_cloud_time_valid(record.started_at_ms)) return ESP_ERR_INVALID_STATE;
        if ((record.valid & SD_AUDIO_TIME_DURATION) && record.duration_ms > UINT64_C(9007199254740991)) return ESP_ERR_INVALID_STATE;
        *out = (sd_audio_session_time_t){record.started_at_ms, record.duration_ms, record.valid};
    } else {
        const uint32_t generation = record.generation;
        memset(&record, 0, sizeof(record));
        record.generation = generation; /* Replacement must supersede both old slots. */
        record.magic = SD_TIME_MAGIC; record.boot_id = session->boot_id; record.start_ms = session->start_ms;
        memcpy(record.owner, session->owner, 32);
    }
    const uint32_t previous_valid = record.valid;
    if (!(record.valid & SD_AUDIO_TIME_STARTED)) {
        nuwax_cloud_time_anchor_t selected = {0};
        if (session->boot_id == current_boot_id && anchor && nuwax_cloud_time_valid(anchor->utc_ms)) selected = *anchor;
        else {
            char saved_path[128];
            sd_clock_record_t saved = {0};
            if (clock_path(saved_path, session->boot_id, session->owner) &&
                read_time_record(saved_path, &saved, sizeof(saved), offsetof(sd_clock_record_t, crc), SD_CLOCK_MAGIC) == ESP_OK &&
                saved.boot_id == session->boot_id && !memcmp(saved.owner, session->owner, 32)) selected = saved.anchor;
        }
        if (nuwax_cloud_time_project(&selected, session->start_ms, &record.started_at_ms)) record.valid |= SD_AUDIO_TIME_STARTED;
    }
    if (!(record.valid & SD_AUDIO_TIME_DURATION) && stopped && session->boot_id == current_boot_id &&
        stopped_ms >= session->start_ms && stopped_ms - session->start_ms <= UINT64_C(9007199254740991)) {
        record.duration_ms = stopped_ms - session->start_ms;
        record.valid |= SD_AUDIO_TIME_DURATION;
    }
    if (record.valid == previous_valid) return ESP_OK;
    const esp_err_t err = save_time_record(path, &record, sizeof(record), offsetof(sd_time_record_t, crc));
    /* Do not send a newly calculated value until it survives retry/restart. */
    if (err == ESP_OK) *out = (sd_audio_session_time_t){record.started_at_ms, record.duration_ms, record.valid};
    return err;
}
