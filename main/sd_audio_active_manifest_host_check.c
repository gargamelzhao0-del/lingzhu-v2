#include <stdio.h>
#include <stdbool.h>
#include <string.h>
static bool corrupt_readback;
static size_t readback_fread(void *buffer, size_t size, size_t count, FILE *file)
{
    const size_t got = fread(buffer, size, count, file);
    if (corrupt_readback && size == 1U && count >= 256U && got >= 256U) memset(buffer, 0, count);
    return got;
}
#define fread readback_fread
#define SD_AUDIO_ARCHIVE_RECOVERY_HELPERS_ONLY 1
#include "sd_audio_archive_recovery_host_check.c"
#undef fread

static sd_audio_session_t active_scene(unsigned mode, unsigned count)
{
    sd_audio_session_t scene = prepare(mode, count);
    /* Fixture archives are the bytes captured BEFORE an ordinary stop. */
    scene.ended = scene.done = 0U; scene.ended_ms = 0U;
    char path[128]; assert(catalog_path(path, scene.session_id, "session"));
    FILE *file = fopen(path, "r+b"); assert(file);
    assert(write_pair(file, 0U, &scene, sizeof(scene), offsetof(sd_audio_session_t, crc)) == ESP_OK && !fclose(file));
    return scene;
}
static void corrupt_manifest(const sd_audio_session_t *scene)
{
    char path[128]; assert(catalog_path(path, scene->session_id, "session"));
    FILE *file = fopen(path, "r+b"); assert(file);
    uint8_t zero[2U * sizeof(*scene)] = {0};
    assert(fwrite(zero, 1U, sizeof(zero), file) == sizeof(zero) && !fclose(file));
}
static void repaired_capture(unsigned mode, unsigned count, bool pruned)
{
    char directory[48]; snprintf(directory, sizeof(directory), "active_%u_%u_%u", mode, count, pruned);
    enter_case(directory); sd_audio_session_t checkpoint = active_scene(mode, count);
    const unsigned prefix = pruned ? CATALOG_PAGE_RECORDS : 1U;
    assert(count > prefix);
    uint32_t original[256]; assert(count <= 256U);
    for (unsigned i = 0; i < count; ++i) original[i] = physical_crc(checkpoint.session_id, i);
    for (unsigned i = 0; i < prefix; ++i) {
        char path[128]; physical_name(path, checkpoint.session_id, i);
        assert(sd_audio_catalog_ack(checkpoint.session_id, i) == ESP_OK && !remove(path));
        assert(sd_audio_catalog_drop_backup(path) == ESP_OK);
    }
    if (pruned) {
        char page[128]; snprintf(page, sizeof(page), "catalog/%s.index.00000000", checkpoint.session_id);
        assert(!remove(page));
    }
    if (mode && !pruned) damage(&checkpoint, prefix, false);
    corrupt_manifest(&checkpoint);
    assert(sd_audio_catalog_recover_active(7U, checkpoint.session_id) == ESP_ERR_INVALID_CRC);
    assert(sd_audio_catalog_prepare_active(&checkpoint) == ESP_OK);
    assert(sd_audio_catalog_recover_active(7U, checkpoint.session_id) == ESP_OK);
    sd_audio_session_t recovered;
    assert(sd_audio_catalog_load(checkpoint.session_id, &recovered) == ESP_OK);
    assert(catalog_same_capture(&checkpoint, &recovered) && !recovered.ended && !recovered.done && !recovered.complete);
    assert(recovered.segment_count == count);
    for (unsigned i = prefix; i < count; ++i) {
        sd_audio_segment_t part;
        assert(sd_audio_catalog_read(checkpoint.session_id, i, &part) == ESP_OK && !part.acknowledged && !part.missing);
        assert(part.realtime == (mode && (i & 1U)));
        assert(physical_crc(checkpoint.session_id, i) == original[i]);
    }
    if (!pruned) {
        sd_audio_segment_t ack;
        assert(sd_audio_catalog_read(checkpoint.session_id, 0U, &ack) == ESP_OK && ack.acknowledged);
    } else {
        char page[128]; snprintf(page, sizeof(page), "catalog/%s.index.00000000", checkpoint.session_id);
        assert(access(page, F_OK) != 0); /* No invented ACKs or recreated old pages. */
    }
    recovered.ended = 1U; recovered.ended_ms = 1000U + count * 20U;
    assert(sd_audio_catalog_save(&recovered) == ESP_OK);
    unsigned sent = 0U; bool ended = false;
    for (unsigned step = 0; step < count * 8U + 50U; ++step) {
        sd_audio_session_t selected; sd_audio_segment_t part;
        bool found = false, file = false, waiting = false;
        esp_err_t err = mode ? sd_monitor_fifo_next(checkpoint.owner, &selected, &part, &found, &file, &waiting) :
            sd_audio_catalog_next(checkpoint.owner, 7U, 999999U, 0U, NULL, false, 0, &selected, &part, &found, &file);
        if (err == ESP_ERR_NOT_FINISHED) continue;
        assert(err == ESP_OK && found);
        if (file) {
            assert(part.index == prefix + sent && physical_crc(checkpoint.session_id, part.index) == original[part.index]);
            assert(sd_audio_catalog_ack(selected.session_id, part.index) == ESP_OK && !remove(part.path));
            assert(sd_audio_catalog_drop_backup(part.path) == ESP_OK);
            sd_audio_catalog_note_deleted(selected.session_id, part.index); ++sent;
        } else {
            assert(sent == count - prefix && !selected.complete);
            if (mode) {
                bool ready = false;
                do { err = sd_monitor_fifo_end_ready(&selected, &ready); } while (err == ESP_ERR_NOT_FINISHED);
                assert(err == ESP_OK && ready && sd_monitor_fifo_finish(&selected) == ESP_OK);
            } else { selected.done = 1U; assert(sd_audio_catalog_save(&selected) == ESP_OK); }
            ended = true; break;
        }
    }
    assert(ended);
    printf("PASS active manifest: mode=%u files=%u pruned=%u; exact audio/ACK/origin/order retained, resumed same scene, end remains incomplete\n", mode, count, pruned);
    assert(!chdir(".."));
}
int main(void)
{
    enter_case("readback");
    sd_audio_session_t scene = {.start_ms=1000U,.boot_id=7U,.complete=1U};
    strcpy(scene.session_id,"meeting_readback");memset(scene.owner,0x31,32U);
    corrupt_readback = true;
    assert(sd_audio_catalog_create(&scene) == ESP_ERR_INVALID_CRC);
    corrupt_readback = false;
    assert(access("catalog/meeting_readback.session",F_OK) != 0 && access("catalog/meeting_readback.creating",F_OK)==0);
    puts("PASS readback: successful writes with corrupted reads cannot publish a new recording");
    assert(!chdir(".."));
    enter_case("first_reserve");scene=active_scene(0U,0U);corrupt_manifest(&scene);
    sd_audio_segment_t part={0};assert(sd_audio_catalog_reserve(&scene,&part)==ESP_ERR_INVALID_CRC && scene.segment_count==1U);
    assert(sd_audio_catalog_prepare_active(&scene)==ESP_OK && sd_audio_catalog_recover_active(7U,scene.session_id)==ESP_OK);
    sd_audio_session_t loaded;assert(sd_audio_catalog_load(scene.session_id,&loaded)==ESP_OK && !loaded.ended && !loaded.complete);
    assert(sd_audio_catalog_reserve(&loaded,&part)==ESP_OK && part.index==1U);
    puts("PASS first reserve: INVALID_CRC -> fenced checkpoint -> active reload OK, reserved hole is never reused");
    assert(!chdir(".."));
    repaired_capture(0U,4U,false);repaired_capture(1U,4U,false);
    repaired_capture(1U,CATALOG_PAGE_RECORDS+3U,true);
    enter_case("pending_repair");scene=active_scene(0U,3U);corrupt_manifest(&scene);
    assert(sd_audio_catalog_prepare_active(&scene)==ESP_OK);
    char published[128],pending[128];assert(catalog_path(published,scene.session_id,"session")&&catalog_path(pending,scene.session_id,"repairing"));
    assert(!rename(published,pending)); /* reset between quarantine and publication */
    assert(sd_audio_catalog_check_new(scene.session_id)==ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_recover(8U)==ESP_OK && sd_audio_catalog_load(scene.session_id,&loaded)==ESP_OK);
    assert(loaded.ended&&!loaded.complete&&loaded.segment_count==3U && access(pending,F_OK)!=0);
    puts("PASS pending repair: reset after quarantine restores verified journal before boot recovery, no metadata/audio loss");
    assert(!chdir(".."));
    enter_case("foreign");scene=active_scene(0U,2U);sd_audio_session_t foreign=scene;foreign.owner[0]^=1U;
    assert(catalog_path(published,scene.session_id,"session"));FILE *file=fopen(published,"r+b");assert(file);
    assert(write_pair(file,0U,&foreign,sizeof(foreign),offsetof(sd_audio_session_t,crc))==ESP_OK&&!fclose(file));
    assert(sd_audio_catalog_prepare_active(&scene)==ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_load(scene.session_id,&loaded)==ESP_OK && loaded.owner[0]==foreign.owner[0]);
    puts("PASS identity: valid foreign owner cannot be overwritten by a stale active checkpoint");
    assert(!chdir(".."));
    enter_case("io_failure");scene=active_scene(0U,2U);repair_open_fault="meeting_archive.session";
    assert(sd_audio_catalog_prepare_active(&scene)==ESP_FAIL);repair_open_fault=NULL;
    assert(sd_audio_catalog_load(scene.session_id,&loaded)==ESP_OK && catalog_same_capture(&scene,&loaded));
    assert(sd_audio_catalog_pin(scene.session_id,0U)==ESP_OK);
    assert(sd_audio_catalog_prepare_active(&scene)==ESP_ERR_NOT_FINISHED);sd_audio_catalog_unpin();
    puts("PASS I/O/lease: unreadable files and pinned replay cannot authorize manifest replacement");
    return 0;
}
