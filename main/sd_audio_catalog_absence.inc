/* Quiescent full recovery only. A completed archive/backup walk restores
 * every trustworthy source. Only then can a still-unreadable index denote
 * an absent legacy reservation. Unparseable sources prohibit that inference. */
static struct {
    bool active, complete, uncertain_all, live;
    unsigned count;
    char (*uncertain)[48];
} recovery_absence;

static void catalog_absence_clear(void)
{
#ifdef ESP_PLATFORM
    heap_caps_free(recovery_absence.uncertain);
#else
    free(recovery_absence.uncertain);
#endif
    memset(&recovery_absence, 0, sizeof(recovery_absence));
}

static void catalog_absence_note(const char *id)
{
    if (!recovery_absence.active || recovery_absence.uncertain_all) return;
    for (unsigned i = 0U; i < recovery_absence.count; ++i)
        if (!strcmp(recovery_absence.uncertain[i], id)) return;
    if (recovery_absence.count == CATALOG_DEFERRED_MAX) {
        recovery_absence.uncertain_all = true; return;
    }
    if (!recovery_absence.uncertain) {
#ifdef ESP_PLATFORM
        recovery_absence.uncertain = heap_caps_malloc(CATALOG_DEFERRED_MAX * 48U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        recovery_absence.uncertain = malloc(CATALOG_DEFERRED_MAX * 48U);
#endif
        if (!recovery_absence.uncertain) { recovery_absence.uncertain_all = true; return; }
    }
    snprintf(recovery_absence.uncertain[recovery_absence.count++], 48U, "%s", id);
}

static void catalog_absence_other_source(const char *path)
{
    if (!recovery_absence.active) return;
    char normalized[160], id[48];
    const char *name = strrchr(path, '/');
    if (!name || strncasecmp(name + 1U, "card_", 5U)) return;
    if (strlen(path) >= sizeof(normalized)) { recovery_absence.uncertain_all = true; return; }
    strcpy(normalized, path);
    char *suffix = strstr(normalized, ".ready");
    if (suffix) suffix[6] = 0;
    else {
        suffix = strstr(normalized, ".part");
        if (!suffix || (size_t)(suffix - normalized) + 7U > sizeof(normalized)) {
            recovery_absence.uncertain_all = true; return;
        }
        strcpy(suffix, ".ready");
    }
    if (catalog_audio_name(normalized, id)) catalog_absence_note(id);
    else recovery_absence.uncertain_all = true;
}

static esp_err_t catalog_mark_proven_absent(const char *id, uint32_t index)
{
    if (!recovery_absence.complete || recovery_absence.active ||
        recovery_absence.uncertain_all || session_blocked(id)) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0U; i < recovery_absence.count; ++i)
        if (!strcmp(recovery_absence.uncertain[i], id)) return ESP_ERR_INVALID_STATE;
    sd_audio_session_t scene;
    esp_err_t err = sd_audio_catalog_load(id, &scene);
    if (err != ESP_OK) return err;
    if (scene.done || index >= scene.segment_count) return ESP_ERR_INVALID_STATE;
    sd_audio_segment_t old;
    err = sd_audio_catalog_read(id, index, &old);
    if (err == ESP_OK) return ESP_ERR_NOT_FINISHED; /* Re-evaluate a now-readable record. */
    if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_CRC &&
        err != ESP_ERR_INVALID_STATE && err != ESP_ERR_INVALID_SIZE) return err;
    scene.complete = 0U;
    err = sd_audio_catalog_save(&scene);
    if (err != ESP_OK) return err;
    sd_audio_segment_t hole = {.index = index, .missing = 1U};
    return sd_audio_catalog_write(id, &hole);
}
