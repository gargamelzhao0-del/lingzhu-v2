#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sd_audio_catalog.c"

int main(void)
{
    assert(sd_audio_catalog_init() == ESP_OK);
    sd_audio_session_t scene = {.complete = 1U, .segment_count = 1U,
        .boot_id = 7U, .start_ms = 1000U};
    strcpy(scene.session_id, "lease_review");
    memset(scene.owner, 0x31, sizeof(scene.owner));
    assert(sd_audio_catalog_create(&scene) == ESP_OK);
    sd_audio_segment_t part = {.sealed = 1U, .total_bytes = 794U,
        .payload_crc = 0x12345678U, .start_ms = 1000U, .end_ms = 1020U};
    strcpy(part.path, "./card_lease_review_1.ready");
    assert(sd_audio_catalog_write(scene.session_id, &part) == ESP_OK);
    sd_audio_lease_token_t first, failed = 99U, next;
    assert(sd_audio_catalog_lease_acquire(&scene, &part, SD_AUDIO_LEASE_VERIFY,
        3U, 8U, &first) == ESP_OK && first);
    assert(sd_audio_catalog_lease_acquire(&scene, &part, SD_AUDIO_LEASE_REPLAY,
        3U, 8U, &failed) == ESP_ERR_NOT_FINISHED && !failed);
    sd_audio_catalog_unpin();
    assert(sd_audio_catalog_replay_pinned());
    assert(sd_audio_catalog_prepare_remount() == ESP_ERR_NOT_FINISHED);
    assert(!sd_audio_catalog_lease_matches(first, SD_AUDIO_LEASE_REPLAY, 3U));
    assert(!sd_audio_catalog_lease_matches(first, SD_AUDIO_LEASE_VERIFY, 4U));
    assert(sd_audio_catalog_lease_release(first, SD_AUDIO_LEASE_REPLAY) == ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_lease_matches(first, SD_AUDIO_LEASE_VERIFY, 3U));
    assert(sd_audio_catalog_lease_release(first, SD_AUDIO_LEASE_VERIFY) == ESP_OK);
    assert(sd_audio_catalog_lease_acquire(&scene, &part, SD_AUDIO_LEASE_REPLAY,
        3U, 8U, &next) == ESP_OK && next != first);
    assert(sd_audio_catalog_lease_release(first, SD_AUDIO_LEASE_VERIFY) == ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_lease_matches(next, SD_AUDIO_LEASE_REPLAY, 3U));
    assert(sd_audio_catalog_lease_release(next, SD_AUDIO_LEASE_REPLAY) == ESP_OK);
    puts("PASS lease: failed acquire/legacy release/wrong owner/stale token cannot release a reader; remount fenced");

    sd_audio_session_t foreign = scene;
    foreign.owner[0] ^= 1U;
    assert(sd_audio_catalog_mark_missing_expected(&foreign, &part) == ESP_ERR_INVALID_STATE);
    sd_audio_segment_t wrong = part, actual;
    wrong.payload_crc ^= 1U;
    assert(sd_audio_catalog_mark_missing_expected(&scene, &wrong) == ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_read(scene.session_id, 0U, &actual) == ESP_OK && !actual.missing);
    char path[128]; uint64_t offset;
    assert(catalog_index_location(scene.session_id, 0U, path, &offset) == ESP_OK);
    FILE *file = fopen(path, "r+b"); assert(file);
    assert(fseek(file, (long)offset, SEEK_SET) == 0);
    sd_audio_segment_t damaged[2]; memset(damaged, 0xa5, sizeof(damaged));
    assert(fwrite(damaged, sizeof(damaged), 1U, file) == 1U && !fclose(file));
    assert(sd_audio_catalog_mark_missing_expected(&scene, &part) != ESP_OK);
    file = fopen(path, "rb"); assert(file);
    sd_audio_segment_t retained[2];
    assert(fread(retained, sizeof(retained), 1U, file) == 1U && !fclose(file));
    assert(!memcmp(retained, damaged, sizeof(damaged)));
    assert(sd_audio_catalog_write(scene.session_id, &part) == ESP_OK);
    assert(sd_audio_catalog_mark_missing_expected(&scene, &part) == ESP_OK);
    assert(sd_audio_catalog_mark_missing_expected(&scene, &part) == ESP_OK);
    assert(sd_audio_catalog_read(scene.session_id, 0U, &actual) == ESP_OK);
    assert(actual.missing && !actual.acknowledged && actual.total_bytes == part.total_bytes &&
        actual.payload_crc == part.payload_crc && !strcmp(actual.path, part.path));
    assert(sd_audio_catalog_load(scene.session_id, &scene) == ESP_OK && !scene.complete);
    puts("PASS known-file loss: foreign/changed/unreadable record rejected; no zero replacement; original tuple and partial state retained on retry");
    scene = (sd_audio_session_t){.complete=1U, .segment_count=2U, .boot_id=7U, .start_ms=2000U};
    strcpy(scene.session_id, "verify_cursor"); memset(scene.owner, 0x31, 32U);
    assert(sd_audio_catalog_create(&scene) == ESP_OK);
    part = (sd_audio_segment_t){.sealed=1U, .total_bytes=794U, .payload_crc=0xabcdefU};
    strcpy(part.path, "./card_verify_cursor_1.ready");
    assert(sd_audio_catalog_write(scene.session_id, &part) == ESP_OK);
    sd_audio_segment_t open = {.index=1U};
    strcpy(open.path, "./card_verify_cursor_2.part");
    assert(sd_audio_catalog_write(scene.session_id, &open) == ESP_OK);
    sd_audio_session_t picked;
    assert(sd_audio_catalog_verify_next(scene.owner, 99U, 3U, &picked, &actual) == ESP_OK && actual.index == 0U);
    assert(sd_audio_catalog_verify_checkpoint(&picked, 99U, 3U, 1U, first) == ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_lease_acquire(&picked, &actual, SD_AUDIO_LEASE_VERIFY, 3U, 8U, &first) == ESP_OK);
    assert(sd_audio_catalog_verify_checkpoint(&picked, 99U, 3U, 1U, first) == ESP_OK);
    assert(sd_audio_catalog_lease_release(first, SD_AUDIO_LEASE_VERIFY) == ESP_OK);
    sd_audio_catalog_verify_reset();
    assert(sd_audio_catalog_verify_next(scene.owner, 99U, 3U, &picked, &actual) == ESP_ERR_NOT_FOUND);
    open.sealed = 1U; open.total_bytes=794U; open.payload_crc=0x98765U;
    assert(sd_audio_catalog_write(scene.session_id, &open) == ESP_OK);
    assert(sd_audio_catalog_verify_next(scene.owner, 99U, 3U, &picked, &actual) == ESP_OK && actual.index == 1U);
    sd_audio_catalog_verify_reset();
    assert(sd_audio_catalog_verify_next(scene.owner, 100U, 3U, &picked, &actual) == ESP_OK && actual.index == 0U);
    sd_audio_catalog_verify_reset();
    assert(sd_audio_catalog_verify_next(scene.owner, 99U, 4U, &picked, &actual) == ESP_OK && actual.index == 0U);
    sd_audio_catalog_verify_reset();
    assert(sd_audio_catalog_read(scene.session_id, 0U, &actual) == ESP_OK && !actual.acknowledged && !actual.missing);
    puts("PASS optional verification cursor: token required, OPEN reservation not skipped, restart/remount nonce invalidates hint, no invented ACK");
    assert(sd_audio_catalog_ack(scene.session_id,0U)==ESP_OK);
    assert(sd_audio_catalog_ack(scene.session_id,1U)==ESP_OK);
    scene.ended=scene.done=1U;scene.first_pending=scene.segment_count;
    assert(sd_audio_catalog_save(&scene)==ESP_OK);
    assert(sd_audio_catalog_mark_missing_expected(&scene,&part)==ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_load(scene.session_id,&scene)==ESP_OK&&scene.done&&scene.complete);
    puts("PASS completed scene: late damage result cannot downgrade a durably confirmed recording");
    return 0;
}
