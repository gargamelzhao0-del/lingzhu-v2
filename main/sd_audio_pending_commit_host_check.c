#define SD_AUDIO_ARCHIVE_RECOVERY_HELPERS_ONLY 1
#include "sd_audio_archive_recovery_host_check.c"

#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define TAG "pending"
#define ESP_LOGI(tag,...) do{(void)(tag);if(0)printf(__VA_ARGS__);}while(0)
#define ESP_LOGW(tag,...) ESP_LOGI(tag,__VA_ARGS__)
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
typedef struct { uint8_t bytes[40]; } nuwax_sd_card_identity_t;
static struct { int lock; } s;
static sd_audio_session_t s_card_replay_session;
static sd_audio_segment_t s_card_replay_segment;
static bool busy, lock_available = true, allocation_fails;
static uint8_t current_owner[32];
static nuwax_sd_card_identity_t current_card;
static unsigned pending_allocs, pending_frees;
static bool nuwax_offline_audio_storage_busy(void) { return busy; }
static bool card_owner(uint8_t owner[32]) { memcpy(owner, current_owner, 32U); return true; }
static bool nuwax_sd_card_get_identity(nuwax_sd_card_identity_t *id) { *id = current_card; return true; }
static int xSemaphoreTake(int lock, unsigned ms) { (void)lock; (void)ms; return lock_available; }
static void xSemaphoreGive(int lock) { (void)lock; }
static void *heap_caps_calloc(size_t n, size_t bytes, unsigned caps)
{ (void)caps; if (allocation_fails) return NULL; ++pending_allocs; return calloc(n, bytes); }
static void heap_caps_free(void *p) { if (p) { ++pending_frees; free(p); } }
#include "sd_audio_pending_commit.inc"

static void stage(const sd_audio_session_t *scene, unsigned index)
{
    s_card_replay_session = *scene;
    assert(sd_audio_catalog_read(scene->session_id, index, &s_card_replay_segment) == ESP_OK);
    memcpy(current_owner, scene->owner, 32U);
    card_commit_stage(); assert(s_card_pending_commit && s_card_pending_commit->valid);
}

static void pending_tests(void)
{
    enter_case("pending"); sd_audio_session_t scene = prepare(0U, 4U);
    stage(&scene, 0U); const uint32_t crc = physical_crc(scene.session_id, 0U);
    busy = true; assert(card_commit_flush() == ESP_ERR_NOT_FINISHED);
    card_commit_release_idle(); assert(s_card_pending_commit && physical_crc(scene.session_id, 0U) == crc);
    busy = false; lock_available = false; assert(card_commit_flush() == ESP_ERR_NOT_FINISHED);
    lock_available = true;
    repair_open_fault = ".ready.meta"; assert(card_commit_flush() == ESP_FAIL && s_card_pending_commit->valid);
    sd_audio_segment_t part; assert(sd_audio_catalog_read(scene.session_id, 0U, &part) == ESP_OK && part.acknowledged);
    const uint32_t unsynced = part.generation;
    repair_open_fault = NULL; assert(card_commit_flush() == ESP_OK && !s_card_pending_commit->valid);
    assert(sd_audio_catalog_read(scene.session_id, 0U, &part) == ESP_OK && part.acknowledged && part.generation > unsynced);
    assert(physical_crc(scene.session_id, 0U) == crc); /* ACK alone never unlinks. */
    sd_audio_session_t selected; bool found = false, file = false;
    assert(sd_audio_catalog_next(scene.owner, 7U, 100000U, 0U, NULL, false, 0,
        &selected, &part, &found, &file) == ESP_OK && found && file && part.index == 1U);
    char path[128]; physical_name(path, scene.session_id, 0U); assert(access(path, F_OK) != 0);
    stage(&scene, 1U); current_card.bytes[0] ^= 1U;
    assert(card_commit_flush() == ESP_OK && !s_card_pending_commit->valid);
    assert(sd_audio_catalog_read(scene.session_id, 1U, &part) == ESP_OK && !part.acknowledged);
    stage(&scene, 1U); current_owner[0] ^= 1U;
    assert(card_commit_flush() == ESP_OK && !s_card_pending_commit->valid);
    assert(sd_audio_catalog_read(scene.session_id, 1U, &part) == ESP_OK && !part.acknowledged);
    stage(&scene, 1U); s_card_pending_commit->scene.start_ms++;
    assert(card_commit_flush() == ESP_OK && !s_card_pending_commit->valid);
    assert(sd_audio_catalog_read(scene.session_id, 1U, &part) == ESP_OK && !part.acknowledged);
    stage(&scene, 1U); card_commit_saved(); card_commit_release_idle(); assert(!s_card_pending_commit);
    allocation_fails = true; card_commit_stage(); assert(!s_card_pending_commit); allocation_fails = false;
    assert(pending_allocs == pending_frees && chdir("..") == 0);
    puts("PASS pending ACK: storage handoff, lock failure, index-visible/sidecar-failed retry, normal deletion, card/owner/capture fences and allocation failure");
}

static void index_tests(void)
{
    enter_case("index"); sd_audio_session_t scene = prepare(0U, 2U);
    sd_audio_segment_t part; assert(sd_audio_catalog_read(scene.session_id, 0U, &part) == ESP_OK);
    const uint32_t crc = physical_crc(scene.session_id, 0U);
    /* A valid-CRC wrong central entry, while immutable source and its owner-
     * bound sidecar agree. Bypass ordinary write (which rejects this conflict). */
    char path[128]; uint64_t offset; assert(catalog_index_location(scene.session_id, 0U, path, &offset) == ESP_OK);
    FILE *index = fopen(path, "r+b"); assert(index); part.payload_crc ^= 1U; part.total_bytes += 666U;
    assert(write_pair(index, offset, &part, sizeof(part), offsetof(sd_audio_segment_t, crc)) == ESP_OK && !fclose(index));
    assert(sd_audio_catalog_read(scene.session_id, 0U, &part) == ESP_OK);
    sd_audio_session_t foreign = scene; foreign.owner[0] ^= 1U; bool changed = false;
    assert(sd_audio_catalog_reconcile_file(&foreign, &part, &changed) == ESP_ERR_INVALID_STATE && !changed);
    assert(sd_audio_catalog_reconcile_file(&scene, &part, &changed) == ESP_OK && changed);
    assert(physical_crc(scene.session_id, 0U) == crc);
    assert(sd_audio_catalog_read(scene.session_id, 0U, &part) == ESP_OK && part.total_bytes == 794U && !part.acknowledged);
    sd_audio_session_t restored; assert(sd_audio_catalog_load(scene.session_id, &restored) == ESP_OK && !restored.done && !restored.complete);
    /* An ACK appearing after validation invalidates the stale repair snapshot. */
    assert(sd_audio_catalog_ack(scene.session_id, 0U) == ESP_OK); changed = false;
    assert(sd_audio_catalog_reconcile_file(&scene, &part, &changed) == ESP_ERR_INVALID_STATE && !changed);
    assert(physical_crc(scene.session_id, 0U) == crc);
    assert(chdir("..") == 0);
    puts("PASS targeted index repair: correct source/sidecar, wrong CRC-valid index; owner/capture/stale ACK protection, exact audio retained, completeness conservative");
}

int main(void) { pending_tests(); index_tests(); return 0; }
