/* Exercise the production catalog, real archive bytes and both FIFO paths. */
#define main previous_repair_controls
#include "sd_audio_catalog_repair_host_check.c"
#undef main
#include "audio_frame.h"

static void physical_name(char path[128], const char *id, unsigned index)
{
    snprintf(path, 128U, "audio/card_%s_%08u.ready", id, index);
}

static uint32_t physical_crc(const char *id, unsigned index)
{
    char path[128]; physical_name(path, id, index);
    uint8_t bytes[794]; FILE *f = fopen(path, "rb"); assert(f);
    assert(fread(bytes, 1U, sizeof(bytes), f) == sizeof(bytes) && fclose(f) == 0);
    return checksum(bytes, sizeof(bytes));
}

static sd_audio_session_t prepare(unsigned mode, unsigned count)
{
    assert(mkdir("audio", 0775) == 0 && mkdir("audio2", 0775) == 0);
    sd_audio_session_t scene = make(mode ? "monitor_archive" : "meeting_archive", mode, count, mode != 0U);
    for (unsigned i = 0U; i < count; ++i) {
        sd_audio_segment_t part; assert(sd_audio_catalog_read(scene.session_id, i, &part) == ESP_OK);
        uint8_t bytes[794] = {0}; int16_t pcm[320];
        for (unsigned j = 0U; j < 320U; ++j) pcm[j] = (int16_t)(i * 31U + j);
        memcpy(bytes, "NWXOFF1", 7U); bytes[8] = 1U; bytes[10] = 96U;
        bytes[12] = (uint8_t)mode; bytes[13] = bytes[14] = 1U;
        put32(bytes + 16U, 16000U); bytes[20] = 20U; bytes[22] = 2U;
        put32(bytes + 24U, i); put64(bytes + 28U, part.start_ms);
        strcpy((char *)bytes + 36U, scene.session_id);
        put32(bytes + 96U, 662U); size_t frame_bytes = 0U;
        assert(nuwax_audio_frame_build_pcm16(bytes + 100U, 662U, pcm, 320U, i,
            (uint32_t)part.start_ms, 0U, 16000U, 1U, 20U, &frame_bytes) == ESP_OK && frame_bytes == 662U);
        uint8_t *footer = bytes + 762U; memcpy(footer, "NWXEND1", 7U);
        footer[8] = 1U; footer[10] = 32U; put32(footer + 12U, 1U);
        put64(footer + 16U, 662U); put32(footer + 24U, checksum(bytes + 100U, 662U)); footer[28] = 1U;
        char path[128]; physical_name(path, scene.session_id, i);
        assert(rename(part.path, path) == 0); strcpy(part.path, path);
        FILE *file = fopen(path, "wb"); assert(file);
        assert(fwrite(bytes, 1U, sizeof(bytes), file) == sizeof(bytes) && fclose(file) == 0);
        part.payload_crc = checksum(bytes + 100U, 662U); part.realtime = mode && (i & 1U);
        assert(sd_audio_catalog_write(scene.session_id, &part) == ESP_OK);
    }
    return scene;
}

static void damage(const sd_audio_session_t *scene, unsigned first, bool legacy)
{
    const uint8_t zero[2U * sizeof(sd_audio_segment_t)] = {0};
    for (unsigned i = first; i < scene->segment_count; ++i) {
        char path[128]; uint64_t offset;
        assert(catalog_index_location(scene->session_id, i, path, &offset) == ESP_OK);
        FILE *file = fopen(path, "r+b"); assert(file);
        assert(fseek(file, (long)offset, SEEK_SET) == 0 &&
            fwrite(zero, 1U, sizeof(zero), file) == sizeof(zero) && fclose(file) == 0);
        if (legacy) { physical_name(path, scene->session_id, i); assert(sd_audio_catalog_drop_backup(path) == ESP_OK); }
    }
}

static void roundtrip(unsigned mode, unsigned count, unsigned first, bool legacy, bool pruned)
{
    char directory[48]; snprintf(directory, sizeof(directory), "round_%u_%u_%u_%u_%u", mode, count, first, legacy, pruned);
    enter_case(directory); sd_audio_session_t scene = prepare(mode, count);
    uint32_t original[129]; assert(count <= 129U);
    for (unsigned i = 0U; i < count; ++i) original[i] = physical_crc(scene.session_id, i);
    if (pruned) {
        for (unsigned i = 0U; i < first; ++i) {
            char path[128]; physical_name(path, scene.session_id, i);
            assert(sd_audio_catalog_ack(scene.session_id, i) == ESP_OK && remove(path) == 0);
            assert(sd_audio_catalog_drop_backup(path) == ESP_OK);
        }
    }
    damage(&scene, first, legacy);
    if (first < count) poison(&scene, count, true);
    if (pruned) assert(remove("catalog/monitor_archive.index.00000000") == 0);
    assert(sd_audio_catalog_recover(8U) == ESP_OK);
    assert(sd_audio_catalog_load(scene.session_id, &scene) == ESP_OK && !scene.done);
    assert(scene.complete == (first == count));
    const unsigned expected = pruned ? count - first : count;
    unsigned uploaded = 0U; bool finished = false;
    for (unsigned step = 0U; step < count * 8U + 50U; ++step) {
        sd_audio_session_t selected; sd_audio_segment_t part; bool found = false, file = false, waiting = false;
        esp_err_t err = mode ? sd_monitor_fifo_next(scene.owner, &selected, &part, &found, &file, &waiting) :
            sd_audio_catalog_next(scene.owner, 8U, 9999999U, 0U, NULL, false, 0, &selected, &part, &found, &file);
        if (err == ESP_ERR_NOT_FINISHED) continue;
        assert(err == ESP_OK && found);
        if (file) {
            const unsigned i = part.index; assert(i < count && physical_crc(scene.session_id, i) == original[i]);
            const bool origin = mode && (i & 1U) && !(legacy && i >= first);
            assert(part.realtime == origin && !part.acknowledged && !part.missing);
            assert(sd_audio_catalog_ack(scene.session_id, i) == ESP_OK && remove(part.path) == 0);
            assert(sd_audio_catalog_drop_backup(part.path) == ESP_OK);
            sd_audio_catalog_note_deleted(scene.session_id, i); ++uploaded; continue;
        }
        assert(uploaded == expected);
        if (mode) {
            bool ready = false;
            do { err = sd_monitor_fifo_end_ready(&selected, &ready); } while (err == ESP_ERR_NOT_FINISHED);
            assert(err == ESP_OK && ready && sd_monitor_fifo_finish(&selected) == ESP_OK);
        } else { selected.done = 1U; assert(sd_audio_catalog_save(&selected) == ESP_OK); }
        finished = true; break;
    }
    assert(finished && uploaded == expected && sd_audio_catalog_recover(9U) == ESP_OK);
    bool pending = true; assert(sd_audio_catalog_has_pending(&pending) == ESP_OK && !pending);
    assert(sd_audio_catalog_load(scene.session_id, &scene) == ESP_OK && scene.done);
    assert(scene.complete == (first == count));
    printf("PASS archive recovery mode=%u count=%u damaged_from=%u legacy=%u pruned=%u uploaded=%u; exact PCM bytes, origin/ACK isolation and restart\n",
        mode, count, first, legacy, pruned, uploaded);
    assert(chdir("..") == 0);
}

static void ack_backup(void)
{
    enter_case("backup_ack"); sd_audio_session_t scene = prepare(0U, 1U);
    assert(sd_audio_catalog_ack(scene.session_id, 0U) == ESP_OK);
    damage(&scene, 0U, false);
    assert(sd_audio_catalog_recover(8U) == ESP_OK);
    sd_audio_segment_t part; assert(sd_audio_catalog_read(scene.session_id, 0U, &part) == ESP_OK && part.acknowledged);
    sd_audio_session_t selected; bool have = false, file = false;
    assert(sd_audio_catalog_next(scene.owner, 8U, 99999U, 0U, NULL, false, 0, &selected, &part, &have, &file) == ESP_OK && have && !file);
    puts("PASS archive recovery: backup ACK remains authoritative; already committed audio is not retransmitted");
    assert(chdir("..") == 0);
    enter_case("backup_ack_audio_absent"); scene = prepare(0U, 1U);
    assert(sd_audio_catalog_ack(scene.session_id, 0U) == ESP_OK);
    char path[128]; physical_name(path, scene.session_id, 0U); assert(remove(path) == 0);
    damage(&scene, 0U, false);
    assert(sd_audio_catalog_recover(8U) == ESP_OK);
    assert(sd_audio_catalog_read(scene.session_id, 0U, &part) == ESP_OK && part.acknowledged && !part.missing);
    puts("PASS archive recovery: crash after audio unlink restores index ACK from owner-bound sidecar before cleanup");
    assert(chdir("..") == 0);
}

static void owner_and_conflict(void)
{
    enter_case("foreign_backup"); sd_audio_session_t scene = prepare(1U, 1U);
    char path[128], backup[160]; physical_name(path, scene.session_id, 0U);
    assert(catalog_backup_path(path, backup)); FILE *f = fopen(backup, "r+b"); assert(f);
    uint8_t header[112]; assert(fread(header, 1U, sizeof(header), f) == sizeof(header));
    header[8] ^= 0x55U; put32(header + 108U, checksum(header, 108U));
    assert(fseek(f, 0, SEEK_SET) == 0 && fwrite(header, 1U, sizeof(header), f) == sizeof(header) && fclose(f) == 0);
    damage(&scene, 0U, false); assert(sd_audio_catalog_recover(8U) == ESP_OK);
    sd_audio_segment_t discarded;
    assert(!session_blocked(scene.session_id) && physical_crc(scene.session_id, 0U));
    assert(sd_audio_catalog_read(scene.session_id,0U,&discarded)==ESP_OK&&discarded.missing&&!discarded.acknowledged);
    puts("PASS archive recovery: foreign-owner backup cannot authorize ACK or upload; one ambiguous slot discarded, scene unblocked");
    assert(chdir("..") == 0);
    enter_case("duplicate_key"); scene = prepare(0U, 1U);
    physical_name(path, scene.session_id, 0U); f = fopen(path, "rb"); assert(f);
    uint8_t bytes[794]; assert(fread(bytes, 1U, sizeof(bytes), f) == sizeof(bytes) && fclose(f) == 0);
    const char *duplicate = "audio/card_meeting_archive_00000999.ready";
    f = fopen(duplicate, "wb"); assert(f); assert(fwrite(bytes, 1U, sizeof(bytes), f) == sizeof(bytes) && fclose(f) == 0);
    assert(sd_audio_catalog_recover(8U) == ESP_OK && !session_blocked(scene.session_id));
    assert(sd_audio_catalog_read(scene.session_id,0U,&discarded)==ESP_OK&&discarded.missing==SD_AUDIO_SEGMENT_RETIRED&&!discarded.acknowledged);
    puts("PASS archive recovery: duplicate physical sources discard exactly their ambiguous wire index, never block later files");
    assert(chdir("..") == 0);
}

#ifndef SD_AUDIO_ARCHIVE_RECOVERY_HELPERS_ONLY
int main(void)
{
    roundtrip(0U, 129U, 129U, false, false);
    roundtrip(1U, 129U, 129U, false, false);
    roundtrip(0U, 129U, 54U, false, false);
    roundtrip(1U, 129U, 54U, false, false);
    roundtrip(0U, 129U, 54U, true, false);
    roundtrip(1U, 129U, 54U, true, false);
    roundtrip(1U, 129U, 54U, false, true);
    ack_backup(); owner_and_conflict();
    return 0;
}
#endif
