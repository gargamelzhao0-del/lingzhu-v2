#if CONFIG_NUWAX_SD_SESSION_ENABLE
/* Sender-owned PSRAM. RX only touches replay_ack under s_replay_lock. A
 * connection epoch fences all volatile offsets; the SD ACK is authoritative. */
#define SD_SCENE_CONTEXTS 32U
#define SD_SCENE_FILES 32U
typedef struct { bool used; sd_scene_resume_t file; } card_scene_file_t;
typedef struct {
    char id[48];
    bool begun;
    uint64_t last_used;
    card_scene_file_t files[SD_SCENE_FILES];
} card_scene_context_t;
static card_scene_context_t *s_scene_contexts;
static nuwax_ws_client_t *s_scene_ws;
static uint32_t s_scene_epoch;
static uint64_t s_scene_clock;
static card_scene_file_t *s_scene_current;

static void card_scene_reset(void)
{
    if (s_scene_contexts) memset(s_scene_contexts, 0, sizeof(*s_scene_contexts) * SD_SCENE_CONTEXTS);
    s_scene_current = NULL; s_scene_ws = NULL; s_scene_epoch = 0; s_scene_clock = 0;
}
static card_scene_file_t *card_scene_file(card_scene_context_t *ctx, uint32_t index, bool create)
{
    card_scene_file_t *empty = NULL;
    for (unsigned i = 0; i < SD_SCENE_FILES; ++i) {
        if (ctx->files[i].used && ctx->files[i].file.index == index) return &ctx->files[i];
        if (!ctx->files[i].used && !empty) empty = &ctx->files[i];
    }
    if (create && empty) {
        memset(empty, 0, sizeof(*empty)); empty->used = true; empty->file.index = index;
        return empty;
    }
    return NULL;
}
static card_scene_context_t *card_scene_context(nuwax_ws_client_t *ws, uint32_t epoch, const char *id)
{
    if (!s_scene_contexts) s_scene_contexts = heap_caps_calloc(SD_SCENE_CONTEXTS,
        sizeof(*s_scene_contexts), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scene_contexts) return NULL;
    if (s_scene_ws != ws || s_scene_epoch != epoch) { card_scene_reset(); s_scene_ws = ws; s_scene_epoch = epoch; }
    card_scene_context_t *empty = NULL, *oldest = NULL;
    for (unsigned i = 0; i < SD_SCENE_CONTEXTS; ++i) {
        card_scene_context_t *ctx = &s_scene_contexts[i];
        if (!strcmp(ctx->id, id)) { ctx->last_used = ++s_scene_clock; return ctx; }
        if (!ctx->id[0] && !empty) empty = ctx;
        if (!oldest || ctx->last_used < oldest->last_used) oldest = ctx;
    }
    /* This is a bounded sender-owned cache, not a limit on SD scene count.
     * prepare cleared s_scene_current before entering; no reader/RX task owns
     * these slots. An evicted attempted file must query the peer's durable
     * offset through the existing begin/resume path before sending again. */
    if (!empty) empty = oldest;
    memset(empty, 0, sizeof(*empty));
    snprintf(empty->id, sizeof(empty->id), "%s", id);
    empty->last_used = ++s_scene_clock;
    return empty;
}
static void card_scene_forget(const char *id)
{
    if (!s_scene_contexts) return;
    for (unsigned i = 0; i < SD_SCENE_CONTEXTS; ++i)
        if (!strcmp(s_scene_contexts[i].id, id)) memset(&s_scene_contexts[i], 0, sizeof(*s_scene_contexts));
    s_scene_current = NULL;
}
static void card_scene_sent(uint32_t offset)
{
    if (s_scene_current) s_scene_current->file.offset = offset;
}
static void card_scene_file_done(void)
{
    if (s_scene_current) s_scene_current->used = false;
    s_scene_current = NULL;
}
static void card_scene_time(const sd_audio_session_t *session, sd_audio_session_time_t *time)
{
    memset(time, 0, sizeof(*time));
    nuwax_cloud_time_anchor_t anchor = {0};
    (void)nuwax_cloud_time_snapshot(&anchor);
    if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    (void)sd_audio_catalog_session_time(session, s_card_boot_id, &anchor, false, 0, time);
    xSemaphoreGive(s.lock);
}
static esp_err_t card_scene_prepare(nuwax_ws_client_t *ws, uint32_t epoch, bool independent,
    const sd_audio_session_t *session, const sd_audio_segment_t *segment, uint32_t *offset)
{
    s_scene_current = NULL;
    card_scene_context_t *ctx = card_scene_context(ws, epoch, session->session_id);
    if (!ctx) return ESP_ERR_NO_MEM;
    card_scene_file_t *cached = segment ? card_scene_file(ctx, segment->index, false) : NULL;
    const bool recover = segment && !cached && (session->boot_id != s_card_boot_id ||
        (segment->sealed & SD_AUDIO_SEGMENT_ATTEMPTED));
    if (!ctx->begun || recover) {
        sd_scene_ack_t query = {0};
        if (recover) {
            /* Bound catalog reads per exchange. Subsequent batches are needed
             * only for previously attempted files or after device restart. */
            if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
            for (uint32_t i = segment->index; i < session->segment_count &&
                 i - segment->index < SD_SCENE_RESUME_MAX; ++i) {
                sd_audio_segment_t part;
                if (sd_audio_catalog_read(session->session_id, i, &part) != ESP_OK) break;
                if (!part.sealed || part.acknowledged || part.missing || card_scene_file(ctx, i, false)) continue;
                if (session->boot_id == s_card_boot_id && !(part.sealed & SD_AUDIO_SEGMENT_ATTEMPTED)) continue;
                query.items[query.count++] = (sd_scene_resume_t){i, part.total_bytes, part.payload_crc, 0};
            }
            xSemaphoreGive(s.lock);
            if (!query.count || query.items[0].index != segment->index) return ESP_ERR_INVALID_STATE;
        }
        sd_audio_session_time_t time;
        card_scene_time(session, &time);
        char request[48]; snprintf(request, sizeof(request), "sd_begin_%08lx", (unsigned long)esp_random());
        cJSON *json = cJSON_CreateObject();
        if (!json) return ESP_ERR_NO_MEM;
        const unsigned route = session->mode ? SD_UPLOAD_LIVE : SD_UPLOAD_MEETING;
        bool built = cJSON_AddStringToObject(json, "type", sd_audio_type(route, SD_BEGIN)) &&
            cJSON_AddStringToObject(json, "request_id", request) &&
            cJSON_AddStringToObject(json, "audio_session_id", session->session_id) &&
            sd_audio_add_session_time(json, &time);
        if (query.count) {
            cJSON *array = cJSON_AddArrayToObject(json, "resume");
            built = built && array;
            for (unsigned i = 0; built && i < query.count; ++i) {
                cJSON *item = cJSON_CreateObject();
                if (!item || !cJSON_AddItemToArray(array, item)) { cJSON_Delete(item); built = false; break; }
                built = cJSON_AddNumberToObject(item, "segment_index", query.items[i].index) &&
                    cJSON_AddNumberToObject(item, "total_bytes", query.items[i].total) &&
                    cJSON_AddNumberToObject(item, "crc32", query.items[i].crc);
            }
        }
        if (!built) { cJSON_Delete(json); return ESP_ERR_NO_MEM; }
        esp_err_t err = replay_arm_ack(epoch, request, sd_audio_type(route, SD_BEGIN_ACK), session->session_id);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_replay_lock); s.replay_ack.scene = query; portEXIT_CRITICAL(&s_replay_lock);
            err = replay_send_json(ws, epoch, json, independent);
        }
        cJSON_Delete(json);
        if (err == ESP_OK) err = replay_wait_ack(ws, epoch, request, false, false, NULL);
        else replay_disarm_ack();
        if (err != ESP_OK) return err;
        portENTER_CRITICAL(&s_replay_lock); query = s.replay_ack.scene; portEXIT_CRITICAL(&s_replay_lock);
        ctx->begun = true;
        for (unsigned i = 0; i < query.count; ++i) {
            card_scene_file_t *entry = card_scene_file(ctx, query.items[i].index, true);
            if (!entry) return ESP_ERR_NO_MEM;
            entry->file = query.items[i];
        }
        ESP_LOGI(TAG, "SD scene registered session=%s resume_files=%u", session->session_id, query.count);
    }
    if (!segment) return ESP_OK;
    cached = card_scene_file(ctx, segment->index, true);
    if (!cached || cached->file.offset > segment->total_bytes) return ESP_ERR_NO_MEM;
    if (!(segment->sealed & SD_AUDIO_SEGMENT_ATTEMPTED)) {
        if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
        const esp_err_t err = sd_audio_catalog_mark_attempted(session->session_id, segment->index);
        xSemaphoreGive(s.lock);
        if (err != ESP_OK) { cached->used = false; return err; }
    }
    cached->file.total = segment->total_bytes; cached->file.crc = segment->payload_crc;
    s_scene_current = cached; *offset = cached->file.offset;
    return ESP_OK;
}
#endif
