/* Private catalog implementation. The spool storage lock serializes all calls.
 * Only newly enabled monitoring scenes use pages. Legacy .index files and the
 * on-wire archive format are unchanged. Retiring a whole page bounds metadata
 * by retained audio, instead of by the lifetime of an open monitoring scene. */
#define CATALOG_PAGE_RECORDS 128U
#define CATALOG_PAGE_TAG "NWXPAGE1:128\n"
static struct { char id[48]; bool paged; } page_cache[8];
static unsigned page_cache_next;

static esp_err_t catalog_is_paged(const char *id, bool *paged)
{
    if (!valid_id(id) || !paged) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < 8U; ++i) {
        if (page_cache[i].id[0] && !strcmp(page_cache[i].id, id)) {
            *paged = page_cache[i].paged; return ESP_OK;
        }
    }
    char path[128];
    if (!catalog_path(path, id, "paged")) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb");
    *paged = false;
    if (!file && errno != ENOENT) return ESP_FAIL;
    if (file) {
        char tag[sizeof(CATALOG_PAGE_TAG)] = {0};
        const size_t got = fread(tag, 1, sizeof(tag) - 1U, file);
        const bool valid = got == sizeof(tag) - 1U && !memcmp(tag, CATALOG_PAGE_TAG, got);
        const bool read_failed = ferror(file) != 0;
        const bool failed = fclose(file) != 0 || read_failed;
        if (failed || !valid) return ESP_FAIL; /* Never reinterpret a torn marker as legacy. */
        *paged = true;
    }
    const unsigned slot = page_cache_next++ % 8U;
    strcpy(page_cache[slot].id, id); page_cache[slot].paged = *paged;
    return ESP_OK;
}

static esp_err_t catalog_index_location(const char *id, uint32_t index,
                                         char path[128], uint64_t *offset)
{
    bool paged;
    esp_err_t err = catalog_is_paged(id, &paged);
    if (err != ESP_OK) return err;
    char suffix[24] = "index";
    if (paged) snprintf(suffix, sizeof(suffix), "index.%08lx", (unsigned long)(index / CATALOG_PAGE_RECORDS));
    if (!catalog_path(path, id, suffix)) return ESP_ERR_INVALID_ARG;
    *offset = (uint64_t)(paged ? index % CATALOG_PAGE_RECORDS : index) * 2U * sizeof(sd_audio_segment_t);
    return ESP_OK;
}

esp_err_t sd_audio_catalog_enable_retention(const char *id)
{
    sd_audio_session_t session;
    esp_err_t err = sd_audio_catalog_load(id, &session);
    if (err != ESP_OK) return err;
    if (session.mode != 1U || session.ended || session.segment_count) return ESP_ERR_INVALID_STATE;
    char path[128], temporary[128];
    if (!catalog_path(path, id, "paged") || !catalog_path(temporary, id, "paging")) return ESP_ERR_INVALID_ARG;
    struct stat st;
    if (stat(path, &st) == 0) { bool paged; return catalog_is_paged(id, &paged); }
    if (errno != ENOENT) return ESP_FAIL;
    FILE *file = fopen(temporary, "wb");
    if (!file) return ESP_FAIL;
    const size_t bytes = sizeof(CATALOG_PAGE_TAG) - 1U;
    bool ok = fwrite(CATALOG_PAGE_TAG, 1, bytes, file) == bytes && fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    if (!ok || rename(temporary, path) != 0) return ESP_FAIL;
    memset(page_cache, 0, sizeof(page_cache));
    return ESP_OK;
}

/* Cursor persistence precedes page removal. A reset before unlink merely
 * leaks metadata until recovery; it can never hide unconfirmed audio. */
static void catalog_prune_pages(const sd_audio_session_t *session, uint32_t previous, bool recovery)
{
    bool paged;
    if (catalog_is_paged(session->session_id, &paged) != ESP_OK || !paged) return;
    const uint32_t limit = session->first_pending / CATALOG_PAGE_RECORDS;
    if (!recovery) {
        for (uint32_t page = previous / CATALOG_PAGE_RECORDS; page < limit; ++page) {
            char path[128], suffix[24];
            snprintf(suffix, sizeof(suffix), "index.%08lx", (unsigned long)page);
            if (catalog_path(path, session->session_id, suffix)) (void)remove(path);
        }
        return;
    }
    /* Do not probe every historical page number after years of operation. */
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) return;
    char prefix[64]; snprintf(prefix, sizeof(prefix), "%s.index.", session->session_id);
    const size_t length = strlen(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, prefix, length) || strlen(entry->d_name + length) != 8U) continue;
        char *end; const unsigned long page = strtoul(entry->d_name + length, &end, 16);
        if (*end || page >= limit) continue;
        char path[128];
        if (catalog_path(path, session->session_id, entry->d_name + strlen(session->session_id) + 1U))
            (void)remove(path);
    }
    closedir(dir);
}
