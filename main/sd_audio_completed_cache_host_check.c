/* Run the production catalog against files, including remount/allocation and
 * durability faults. Cache hits may avoid only previously completed scenes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <io.h>
static bool fail_allocation, fail_sync;
static void *cache_alloc(size_t size) { return fail_allocation ? NULL : malloc(size); }
static int cache_sync(int fd) { return fail_sync ? -1 : _commit(fd); }
#define malloc cache_alloc
#define _commit cache_sync
#include "sd_audio_catalog.c"
#undef malloc
#undef _commit

static sd_audio_session_t make(const char *id, bool done, unsigned owner) {
    sd_audio_session_t s = {0};
    strcpy(s.session_id, id); memset(s.owner, owner, sizeof(s.owner));
    s.boot_id = 7; s.complete = 1; s.start_ms = 100; s.ended_ms = 200;
    assert(sd_audio_catalog_create(&s) == ESP_OK);
    s.ended = 1; s.done = done;
    assert(sd_audio_catalog_save(&s) == ESP_OK);
    return s;
}

static unsigned pick(const sd_audio_session_t *expected, unsigned hits) {
    sd_audio_session_t s; sd_audio_segment_t p; bool found, file;
    sd_audio_catalog_stats_t a, b;
    sd_audio_catalog_replay_reset(); sd_audio_catalog_stats(&a);
    esp_err_t err;unsigned steps=0;
    do {
        err=sd_audio_catalog_next(expected->owner,7,1000,0,NULL,false,0,&s,&p,&found,&file);
        assert(++steps<1024U);
    } while(err==ESP_ERR_NOT_FINISHED);
    assert(err==ESP_OK);
    sd_audio_catalog_stats(&b);
    assert(found && !file && !strcmp(s.session_id, expected->session_id));
    assert(b.completed_hits - a.completed_hits == hits);
    return b.manifest_reads - a.manifest_reads;
}

int main(void) {
    assert(sd_audio_catalog_init() == ESP_OK && done_ids);
    for (unsigned i = 0; i < 40; ++i) {
        char id[48]; snprintf(id, sizeof(id), "history_%02u", i); make(id, true, 9);
    }
    sd_audio_session_t pending = make("z_pending", false, 9);
    assert(pick(&pending, 40) == 1);
    assert(pick(&pending, 40) == 1); // A connection reset retains only done hints.
    bool has_pending = false; sd_audio_catalog_stats_t a, b;
    sd_audio_catalog_stats(&a);
    assert(sd_audio_catalog_has_pending(&has_pending) == ESP_OK && has_pending);
    sd_audio_catalog_stats(&b); assert(b.manifest_reads - a.manifest_reads == 1);
    puts("PASS: exact completed IDs skip 40 historical manifests across new scene/reconnect/pending checks");

    assert(sd_audio_catalog_init() == ESP_OK && done_count == 0);
    assert(pick(&pending, 0) == 41); // Cold/remounted card is fully CRC checked.
    assert(sd_audio_catalog_recover(8) == ESP_OK);
    assert(pick(&pending, 40) == 1);
    puts("PASS: mount invalidates hints; recovery checks disk then warms completed scenes");

    // Another owner's pending scene must not be hidden or admitted as ours.
    sd_audio_session_t foreign = make("a_foreign", false, 8);
    assert(pick(&pending, 40) == 2);
    assert(pick(&foreign, 40) == 2);

    // Creation with a reused ID (after external maintenance) invalidates the
    // exact old hint before touching disk. No hashes or prefix matching.
    assert(!remove("catalog/history_00.session"));
    sd_audio_session_t reused = make("history_00", false, 9);
    assert(pick(&reused, 39) == 3);
    assert(!done_cache_has(reused.session_id));

    // A failed durable completion cannot install a completed hint. Even if
    // the failed write is visible to stdio, this operation grants no cache.
    pending.done = 1; fail_sync = true;
    assert(sd_audio_catalog_save(&pending) == ESP_FAIL);
    assert(!done_cache_has(pending.session_id)); fail_sync = false;
    assert(sd_audio_catalog_save(&pending) == ESP_OK && done_cache_has(pending.session_id));
    puts("PASS: owner isolation, reused IDs and failed completion writes cannot create a false completed hint");

    for (unsigned i = 0; i < CATALOG_DONE_CACHE_COUNT + 9; ++i) {
        char id[48]; snprintf(id, sizeof(id), "overflow_%03u", i); make(id, true, 9);
    }
    sd_audio_session_t s; sd_audio_segment_t p; bool found, file;
    sd_audio_catalog_replay_reset();
    esp_err_t err;unsigned steps=0;
    do {
        err=sd_audio_catalog_next(reused.owner,7,1000,0,NULL,false,0,&s,&p,&found,&file);
        assert(++steps<1024U);
    } while(err==ESP_ERR_NOT_FINISHED);
    assert(err==ESP_OK && found && !strcmp(s.session_id,reused.session_id));
    assert(done_count == CATALOG_DONE_CACHE_COUNT);

    free(done_ids); done_ids = NULL; fail_allocation = true;
    assert(sd_audio_catalog_init() == ESP_OK && !done_ids);
    assert(pick(&reused, 0) == 179); // 40 histories + pending + foreign + 137 overflow
    assert(sd_audio_catalog_has_pending(&has_pending) == ESP_OK && has_pending);
    fail_allocation = false;
    assert(sd_audio_catalog_init() == ESP_OK && done_ids);
    free(done_ids); done_ids = NULL; done_count = 0;
    puts("PASS: bounded eviction and PSRAM allocation failure fall back to disk without losing pending scenes");
    return 0;
}
