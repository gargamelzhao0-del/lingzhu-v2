#include "sd_audio_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

static sd_audio_session_t create_owned(const char *id, unsigned mode, uint8_t owner)
{
    sd_audio_session_t session = {0};
    strcpy(session.session_id, id);
    memset(session.owner, owner, 32);
    session.mode = mode; session.boot_id = 7; session.start_ms = 1000; session.complete = 1;
    assert(sd_audio_catalog_create(&session) == ESP_OK);
    return session;
}
static sd_audio_session_t create(const char *id, unsigned mode)
{
    return create_owned(id, mode, 0x31);
}
static sd_audio_segment_t append(sd_audio_session_t *session, uint64_t start)
{
    sd_audio_segment_t segment = {0};
    strcpy(segment.path, "unused.ready");
    segment.start_ms = start; segment.end_ms = start + 960;
    segment.realtime = 1;
    assert(sd_audio_catalog_reserve(session, &segment) == ESP_OK);
    segment.sealed = 1; segment.total_bytes = 32096; segment.payload_crc = 10;
    assert(sd_audio_catalog_write(session->session_id, &segment) == ESP_OK);
    return segment;
}
int main(void)
{
    assert(sd_audio_catalog_init() == ESP_OK);
    bool pending = true;
    assert(sd_audio_catalog_has_pending(&pending) == ESP_OK && !pending);
    sd_audio_session_t meeting = create("meeting_a", 0);
    sd_audio_segment_t part = append(&meeting, 1000);
    assert(sd_audio_catalog_has_pending(&pending) == ESP_OK && pending);
    sd_audio_session_t found; sd_audio_segment_t file;
    bool have_session, have_file;
    assert(sd_audio_catalog_next(meeting.owner, 7, 2000, 0, NULL, false, -1,
        &found, &file, &have_session, &have_file) == ESP_OK && !have_session);
    meeting.ended = 1; meeting.ended_ms = 2100;
    assert(sd_audio_catalog_save(&meeting) == ESP_OK);
    assert(sd_audio_catalog_has_pending(&pending) == ESP_OK && pending);
    assert(sd_audio_catalog_next(meeting.owner, 7, 2200, 0, NULL, false, -1,
        &found, &file, &have_session, &have_file) == ESP_OK && have_file);
    assert(file.index == 0 && !strcmp(found.session_id, "meeting_a"));
    assert(sd_audio_catalog_ack("meeting_a", 0) == ESP_OK);
    assert(sd_audio_catalog_ack("meeting_a", 0) == ESP_OK);
    assert(sd_audio_catalog_next(meeting.owner, 7, 2300, 0, NULL, false, -1,
        &found, &file, &have_session, &have_file) == ESP_OK && have_session && !have_file);
    found.done = 1; assert(sd_audio_catalog_save(&found) == ESP_OK);
    assert(sd_audio_catalog_has_pending(&pending) == ESP_OK && !pending);
    sd_audio_session_t monitoring = create("monitor_b", 1);
    part = append(&monitoring, 3000);
    assert(sd_audio_catalog_has_pending(&pending) == ESP_OK && pending);
    assert(sd_audio_segment_realtime(&monitoring, &part, 7, 4100, 0));
    assert(sd_audio_segment_realtime(&monitoring, &part, 8, 4100, 0));
    assert(sd_audio_segment_realtime(&monitoring, &part, 7, 4100, 3100));
    assert(sd_audio_segment_realtime(&monitoring, &part, 7, 9000, 0));
    part.realtime=0;assert(!sd_audio_segment_realtime(&monitoring,&part,7,4100,0));part.realtime=1;
    assert(sd_audio_catalog_next(monitoring.owner, 7, 4100, 0, "monitor_b", true, -1,
        &found, &file, &have_session, &have_file) == ESP_OK && have_file);
    uint8_t other[32] = {0};
    assert(sd_audio_catalog_next(other, 7, 4100, 0, NULL, false, -1,
        &found, &file, &have_session, &have_file) == ESP_OK && !have_session);
    assert(sd_audio_catalog_create(&monitoring) == ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_load("../escape", &found) == ESP_ERR_INVALID_ARG);
    /* The latest manifest slot is torn. Its predecessor is still readable. */
    assert(sd_audio_catalog_load("monitor_b", &found) == ESP_OK);
    found.ended = 1; assert(sd_audio_catalog_save(&found) == ESP_OK);
    FILE *manifest = fopen("catalog/monitor_b.session", "r+b"); assert(manifest);
    assert(fseek(manifest, (found.generation & 1U) * sizeof(found) + offsetof(sd_audio_session_t, crc), SEEK_SET) == 0);
    int byte = fgetc(manifest); assert(byte != EOF);
    assert(fseek(manifest, -1, SEEK_CUR) == 0); fputc(byte ^ 0x80, manifest); fclose(manifest);
    assert(sd_audio_catalog_load("monitor_b", &found) == ESP_OK && !found.ended);
    /* Recovery identifies interrupted sessions and missing files, without
     * acknowledging bytes which were never uploaded. */
    assert(sd_audio_catalog_recover(8) == ESP_OK);
    assert(sd_audio_catalog_load("monitor_b", &found) == ESP_OK && found.ended && !found.complete);
    assert(sd_audio_catalog_read("monitor_b", 0, &file) == ESP_OK && file.missing && !file.acknowledged);
    /* Cached mode misses must not hide a newly ended/sealed scene. ACK
     * progress, new earlier scenes, identity changes and deferred retries
     * must give exactly the same selection as a cold directory scan. */
    assert(sd_audio_catalog_next(meeting.owner,7,10000,0,NULL,false,0,
        &found,&file,&have_session,&have_file)==ESP_OK && !have_session);
    sd_audio_session_t later=create("meeting_cache_later",0);
    append(&later,4000);append(&later,5000);later.ended=1;later.ended_ms=9000;
    assert(sd_audio_catalog_save(&later)==ESP_OK);
    for(unsigned i=0;i<2;i++) {
        assert(sd_audio_catalog_next(later.owner,7,10000,0,NULL,false,0,
            &found,&file,&have_session,&have_file)==ESP_OK && have_file);
        assert(!strcmp(found.session_id,later.session_id) && file.index==i);
        assert(sd_audio_catalog_ack(later.session_id,i)==ESP_OK);
    }
    sd_audio_session_t earlier=create("meeting_cache_earlier",0);
    append(&earlier,3000);earlier.ended=1;earlier.ended_ms=8000;
    assert(sd_audio_catalog_save(&earlier)==ESP_OK);
    assert(sd_audio_catalog_next(earlier.owner,7,10000,0,NULL,false,0,
        &found,&file,&have_session,&have_file)==ESP_OK && have_file);
    assert(!strcmp(found.session_id,earlier.session_id));
    sd_audio_catalog_defer(&found);
    assert(sd_audio_catalog_next(later.owner,7,10000,0,NULL,false,0,
        &found,&file,&have_session,&have_file)==ESP_OK && have_session && !have_file);
    assert(!strcmp(found.session_id,later.session_id));
    sd_audio_catalog_replay_reset();
    assert(sd_audio_catalog_next(other,7,10000,0,NULL,false,0,
        &found,&file,&have_session,&have_file)==ESP_OK && !have_session);
    assert(sd_audio_catalog_next(earlier.owner,7,10000,0,NULL,false,0,
        &found,&file,&have_session,&have_file)==ESP_OK && have_file);
    assert(!strcmp(found.session_id,earlier.session_id));
    /* A pending, unsealed index can be invisible when a miss is cached. */
    assert(sd_audio_catalog_load("monitor_b",&found)==ESP_OK);
    found.done=1;assert(sd_audio_catalog_save(&found)==ESP_OK);
    sd_audio_session_t unsealed=create("monitor_cache_seal",1);
    sd_audio_segment_t pending_part={0};strcpy(pending_part.path,"unused.ready");
    assert(sd_audio_catalog_reserve(&unsealed,&pending_part)==ESP_OK);
    assert(sd_audio_catalog_next(unsealed.owner,7,10000,0,NULL,false,1,
        &found,&file,&have_session,&have_file)==ESP_OK);
    pending_part.sealed=1;pending_part.total_bytes=32096;pending_part.payload_crc=10;
    assert(sd_audio_catalog_write(unsealed.session_id,&pending_part)==ESP_OK);
    assert(sd_audio_catalog_next(unsealed.owner,7,10000,0,NULL,false,1,
        &found,&file,&have_session,&have_file)==ESP_OK && have_file);
    assert(!strcmp(found.session_id,unsealed.session_id));
    /* Sequential large scenes avoid rewriting .session on every ACK. Reset
     * the volatile cursor mid-slice: only durable ACKs may be skipped. */
    sd_audio_session_t bulk=create_owned("meeting_cursor_batch",0,0x42);
    for(unsigned i=0;i<40;i++) append(&bulk,1000U+i*1000U);
    bulk.ended=1;bulk.ended_ms=42000;
    assert(sd_audio_catalog_save(&bulk)==ESP_OK);
    unsigned cursor_writes=0, generation=bulk.generation;
    for(unsigned i=0;i<40;i++) {
        if(i==9) sd_audio_catalog_replay_reset();
        esp_err_t e=sd_audio_catalog_next(bulk.owner,7,50000,0,NULL,false,0,
            &found,&file,&have_session,&have_file);
        assert(e==ESP_OK && have_file && file.index==i && !file.acknowledged);
        assert(sd_audio_catalog_load(bulk.session_id,&bulk)==ESP_OK);
        if(bulk.generation!=generation) {cursor_writes++;generation=bulk.generation;}
        assert(bulk.first_pending<=i && i-bulk.first_pending<16);
        assert(sd_audio_catalog_ack(bulk.session_id,i)==ESP_OK);
    }
    assert(sd_audio_catalog_next(bulk.owner,7,50000,0,NULL,false,0,
        &found,&file,&have_session,&have_file)==ESP_OK && have_session && !have_file);
    assert(found.first_pending==40 && !found.done && cursor_writes==2);
    assert(sd_audio_catalog_load(bulk.session_id,&bulk)==ESP_OK && bulk.first_pending==32 && !bulk.done);
    sd_audio_catalog_replay_reset();
    assert(sd_audio_catalog_next(bulk.owner,7,50000,0,NULL,false,0,
        &found,&file,&have_session,&have_file)==ESP_OK && have_session && !have_file);
    assert(found.first_pending==40 && !found.done); // Lost end ACK retries without publishing done.
    found.done=1;assert(sd_audio_catalog_save(&found)==ESP_OK);
    assert(sd_audio_catalog_load(bulk.session_id,&bulk)==ESP_OK && bulk.first_pending==40 && bulk.done);
    /* Combined adjacent reads preserve independent-slot crash recovery even
     * when EOF bisects the newer slot; reject when neither copy is complete. */
    sd_audio_session_t torn=create("meeting_truncated_pair",0);
    if (torn.generation & 1U) assert(sd_audio_catalog_save(&torn)==ESP_OK);
    const uint32_t fallback_generation=torn.generation; // older complete slot 0
    torn.ended=1;assert(sd_audio_catalog_save(&torn)==ESP_OK); // newer slot 1
    uint8_t bytes[2*sizeof(torn)];
    FILE *raw=fopen("catalog/meeting_truncated_pair.session","rb");assert(raw);
    assert(fread(bytes,1,sizeof(bytes),raw)==sizeof(bytes));fclose(raw);
    raw=fopen("catalog/meeting_truncated_pair.session","wb");assert(raw);
    assert(fwrite(bytes,1,sizeof(torn)+sizeof(torn)/2,raw)==sizeof(torn)+sizeof(torn)/2);fclose(raw);
    assert(sd_audio_catalog_load(torn.session_id,&found)==ESP_OK && found.generation==fallback_generation && !found.ended);
    raw=fopen("catalog/meeting_truncated_pair.session","wb");assert(raw);
    assert(fwrite(bytes,1,sizeof(torn)-1,raw)==sizeof(torn)-1);fclose(raw);
    assert(sd_audio_catalog_load(torn.session_id,&found)==ESP_ERR_INVALID_CRC);
    puts("PASS: 40-file bounded cursor, mid-slice reset/ACK recovery, 2 cursor writes instead of 39, adjacent-slot EOF fallback");
    puts("PASS: cached scene selection, empty-cache invalidation, new earlier scene, durable cursor, deferred retry, owner isolation and seal visibility");
    puts("PASS: meeting barrier, live priority/age, identity isolation, durable ACK, torn-slot and interrupted-session recovery");
    return 0;
}
