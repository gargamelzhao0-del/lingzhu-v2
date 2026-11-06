#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "sd_audio_catalog.h"
#include "board/sd_card_driver.h"
#include "audio_frame.h"
#include "sd_audio_file_fault.h"
static sd_audio_file_fault_t s_card_file_fault[2];
#define HEADER_BYTES 96U
#define FOOTER_BYTES 32U
#define MAX_FRAME_BYTES 2048U
typedef struct { uint8_t type, complete; bool realtime; uint32_t generation; size_t bytes; uint8_t data[MAX_FRAME_BYTES]; } spool_item_t;
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t bytes) {
    crc = ~crc;
    for (size_t i=0; i<bytes; ++i) { crc ^= data[i]; for (unsigned b=0;b<8U;++b) crc=(crc>>1U)^((crc&1U)?0xedb88320U:0U); }
    return ~crc;
}
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define TAG "active-recovery-test"
#define pdTRUE 1
#define ESP_LOGW(...) log_ignore(__VA_ARGS__)
static void log_ignore(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
static struct {
    int admission_lock, writer_gate, lock;
    atomic_bool writer_busy, writer_error, degraded, capture_complete, media_recovery_requested, archive_ready;
    FILE *file; uint32_t next_index, ready_count, frame_count, payload_crc32;
    uint32_t writer_retry_deadline_ms, writer_retry_backoff_ms, capture_generation;
    uint64_t used_bytes, payload_bytes; long last_good_offset;
    bool writer_fault_latched, writer_frame_written, writer_frame_uncertain;
    char open_path[128], ready_path[128], session_id[48];
} s;
static struct {
    atomic_bool pending;
    uint32_t started_ms, deadline_ms, next_ms, attempts, capture_generation;
    bool identity_valid;
    nuwax_sd_card_identity_t identity;
    bool retry_probe, skip_retry;
    uint32_t retry_index, retry_sequence, retry_crc, retry_generation, retry_bytes;
    bool prefix_probe;
    uint32_t prefix_index, prefix_frames, prefix_crc;
    uint64_t prefix_payload_bytes;
} s_card_io_recovery;
static atomic_bool s_card_created, s_card_failed, s_shutdown_requested, s_media_ready, s_replay_media_fault;
static atomic_bool s_card_pending_upload, s_directory_dirty, s_formal_replay_cancel_requested;
static atomic_uint s_capture_recovery_until;
static atomic_bool s_offline_verify_running, s_offline_verify_cancel, s_card_partial;
static sd_audio_damage_window_t s_card_damage;
static sd_audio_session_t s_card_session, restored;
static sd_audio_segment_t s_card_segment, recovered_tail;
esp_err_t sd_audio_catalog_read(const char *id, uint32_t index, sd_audio_segment_t *out) {
    assert(!strcmp(id,s_card_session.session_id) && index == s_card_segment.index); *out = recovered_tail; return ESP_OK;
}
static const char *s_writer_fault_stage;
static int s_writer_fault_errno;
static uint32_t s_card_boot_id = 7U, now_ms;
static bool active, healthy, replay, shutdown, stop_requested, capture_running;
static bool foreign_card, fail_mount, stop_during_mount, late_mount, blocked_prepare, shutdown_during_mount;
static bool foreign_binding, binding_during_scan, fail_filename_scan;
static unsigned preparations;
static bool card_owner(uint8_t owner[32]) { memcpy(owner,s_card_session.owner,32U);if(foreign_binding)owner[0]^=1U;return true; }
static int blocked_lock;
static unsigned locks, mounts, scans, notifications, closes, marks;
static uint32_t queued_frames, mic_generation;
static bool nuwax_sd_session_active(void) { return active; }
static bool nuwax_power_ctrl_shutdown_preparing(void) { return shutdown; }
bool nuwax_sd_card_is_mounted(void) { return healthy; }
static uint32_t offline_now_ms(void) { return now_ms; }
static int64_t esp_timer_get_time(void) { return (int64_t)now_ms * 1000; }
static bool deadline_owned;
esp_err_t nuwax_sd_card_io_deadline_begin(int64_t until) {
    assert(!deadline_owned && until > esp_timer_get_time()); deadline_owned = true; return ESP_OK;
}
void nuwax_sd_card_io_deadline_end(void) { assert(deadline_owned); deadline_owned = false; }
static bool independent_prefix;
static bool nuwax_sd_bulk_read_recovery_begin(uint32_t ms)
{ assert(ms && deadline_owned && !independent_prefix); independent_prefix = true; return true; }
static void nuwax_sd_bulk_read_recovery_end(bool whole)
{ assert(independent_prefix && !whole); independent_prefix = false; }
static bool offline_deadline_elapsed(uint32_t now, uint32_t deadline) { return (int32_t)(now-deadline) >= 0; }
static bool nuwax_offline_audio_replay_running(void) { return replay; }
static void notify_writer_task(void) { ++notifications; }
static esp_err_t nuwax_offline_audio_request_stop(bool complete) { assert(!complete); stop_requested = true; return ESP_OK; }
static esp_err_t commit_segment_locked(bool complete) { assert(!complete && s.file); s.file = NULL; ++closes; return ESP_OK; }
static void writer_schedule_retry_locked(esp_err_t error) { assert(error != ESP_OK); healthy = false; s.writer_error = true; }
static int xSemaphoreTake(int lock, unsigned wait) { assert(wait == 0U); if (lock == blocked_lock) return 0; assert(!(locks & (1U<<lock))); locks |= 1U<<lock; return pdTRUE; }
static void xSemaphoreGive(int lock) {
    assert(locks & (1U<<lock));
    if (late_mount && lock == 3 && !s_card_io_recovery.pending) assert(stop_requested);
    locks &= ~(1U<<lock);
}
esp_err_t sd_audio_catalog_prepare_remount(void) { assert(locks == 14U && !s.file && !s.writer_busy && !replay); return blocked_prepare ? ESP_ERR_NOT_FINISHED : ESP_OK; }
esp_err_t nuwax_sd_card_recover(uint32_t timeout) {
    assert(timeout > 0U && timeout <= 10000U && locks == 14U && s.media_recovery_requested && !s.file && !replay);
    ++mounts;
    if (stop_during_mount) { stop_requested = true; capture_running = false; }
    if (late_mount) now_ms += 10001U;
    if (shutdown_during_mount) shutdown = true;
    if (fail_mount) return ESP_FAIL;
    healthy = true; return ESP_OK;
}
bool nuwax_sd_card_get_identity(nuwax_sd_card_identity_t *out) {
    *out = s_card_io_recovery.identity; if (foreign_card) out->bytes[0] ^= 1U; return true;
}
void nuwax_sd_card_mark_io_fault(esp_err_t error) { assert(error != ESP_OK); healthy = false; ++marks; }
static esp_err_t ensure_directory(void) { assert(locks == 14U); return ESP_OK; }
static esp_err_t prepare_media_space(void) { return ESP_OK; }
esp_err_t sd_audio_catalog_prepare_active(const sd_audio_session_t *checkpoint) {
    assert(locks==14U && preparations==scans && !strcmp(checkpoint->session_id,s_card_session.session_id));++preparations;return ESP_OK;
}
static esp_err_t recover_open_segments(void) { assert(locks == 14U && healthy && preparations); ++scans;if(binding_during_scan)foreign_binding=true;return ESP_OK; }
esp_err_t sd_audio_catalog_recover_active(uint32_t boot, const char *id) { assert(locks == 14U && boot == 7U && !strcmp(id, s_card_session.session_id)); return ESP_OK; }
esp_err_t sd_audio_catalog_load(const char *id, sd_audio_session_t *out) { assert(!strcmp(id,s_card_session.session_id)); *out = restored; return ESP_OK; }
static esp_err_t refresh_next_segment_index(void) { if(fail_filename_scan)return ESP_FAIL;s.next_index=130U;return ESP_OK; }
static uint64_t scan_used_bytes(void) { return 999999U; }
static uint32_t scan_ready_count(void) { return 0U; }
static esp_err_t refresh_capture_capacity_locked(void) { return ESP_OK; }
static void reset_media_recovery_locked(bool changed) { assert(changed && locks == 14U); }
#include "sd_audio_active_recovery.inc"

static void reset(void)
{
    assert(locks == 0U); memset(&s,0,sizeof(s)); memset(&s_card_io_recovery,0,sizeof(s_card_io_recovery));
    memset(&s_card_session,0,sizeof(s_card_session));
    memset(s_card_file_fault,0,sizeof(s_card_file_fault));
    memset(&s_card_segment,0,sizeof(s_card_segment)); memset(&recovered_tail,0,sizeof(recovered_tail));
    strcpy(s_card_session.session_id,"meeting_active"); s_card_session.boot_id = 7U;
    strcpy(s.session_id,s_card_session.session_id);s.archive_ready=true;
    s_card_session.start_ms = 100U;s_card_session.complete=1U; memset(s_card_session.owner,0x31,32U); restored = s_card_session;
    s.admission_lock=1;s.writer_gate=2;s.lock=3;s.capture_generation=33U;
    s.capture_complete=true;s_card_created=true;s_card_failed=false;s_shutdown_requested=false;
    s_media_ready=true;s_card_pending_upload=false;s_directory_dirty=false;s_formal_replay_cancel_requested=false;
    s_replay_media_fault=false;
    s_card_io_recovery.identity_valid=true;memset(s_card_io_recovery.identity.bytes,0x56,40U);
    now_ms=1000U;s_capture_recovery_until=now_ms+10000U;active=healthy=capture_running=true;replay=shutdown=stop_requested=false;
    s_card_partial=false;s_card_damage=(sd_audio_damage_window_t){0};
    s_offline_verify_running=false;s_offline_verify_cancel=false;
    foreign_card=fail_mount=stop_during_mount=late_mount=blocked_prepare=shutdown_during_mount=false;blocked_lock=0;
    foreign_binding=binding_during_scan=fail_filename_scan=false;preparations=0U;
    mounts=scans=notifications=closes=marks=0U;queued_frames=128U;mic_generation=91U;
    s_writer_fault_stage="periodic_sync";s_writer_fault_errno=5;
}
static void request(void) { s.file=(FILE*)1; assert(card_io_recovery_request_locked(ESP_FAIL, NULL)); assert(s_card_io_recovery.pending && !healthy && !s.capture_complete && closes==1U); }
static void preserved(void) { assert(!locks && queued_frames==128U && mic_generation==91U && s.capture_generation==33U && !strcmp(s_card_session.session_id,"meeting_active")); }
int main(void)
{
    reset();s.file=(FILE*)1;s_replay_media_fault=true;replay=true;
    card_io_recovery_observe_reader_fault();assert(!s_card_io_recovery.pending&&s_replay_media_fault&&!mounts);
    replay=false;s.writer_busy=true;card_io_recovery_observe_reader_fault();assert(!s_card_io_recovery.pending&&s_replay_media_fault);
    s.writer_busy=false;card_io_recovery_observe_reader_fault();
    assert(s_card_io_recovery.pending&&!s_replay_media_fault&&!s.file&&closes==1U&&!locks);
    assert(card_io_recovery_service()==ESP_OK&&!s_card_io_recovery.pending&&healthy&&capture_running);
    preserved();puts("PASS reader fault: immediate ordered writer FILE close after replay/writer quiescence, no wait for file rollover");
    reset();active=false;assert(!card_io_recovery_request_locked(ESP_FAIL,NULL));
    reset();assert(!card_io_recovery_request_locked(ESP_ERR_NO_MEM,NULL));assert(!card_io_recovery_request_locked(ESP_ERR_INVALID_STATE,NULL));
    reset();s_writer_fault_stage="record_readback_crc";s.file=(FILE*)1;
    assert(!card_io_recovery_request_locked(ESP_FAIL,NULL)&&s_card_failed&&stop_requested&&!s.capture_complete&&!s_card_io_recovery.pending&&!mounts);
    puts("PASS persistent PCM mismatch: stops capture truthfully; no scratch-pass restart or false complete recording");
    reset();request();replay=true;assert(card_io_recovery_service()==ESP_ERR_NOT_FINISHED&&!mounts&&s_formal_replay_cancel_requested);replay=false;
    s.writer_busy=true;assert(card_io_recovery_service()==ESP_ERR_NOT_FINISHED&&!mounts);s.writer_busy=false;
    for(int lock=1;lock<=3;lock++){blocked_lock=lock;assert(card_io_recovery_service()==ESP_ERR_NOT_FINISHED&&!mounts);preserved();}blocked_lock=0;
    assert(card_io_recovery_service()==ESP_OK && mounts==1U && scans==1U && healthy && s_media_ready && !s.writer_error && !s_card_io_recovery.pending && capture_running && !stop_requested);preserved();
    puts("PASS active recovery: same scene/MIC/generation/queued frames; ordered locks; replay and busy writer block remount; success releases original writer");
    reset();request();fail_mount=true;assert(card_io_recovery_service()==ESP_ERR_NOT_FINISHED&&mounts==1U);preserved();
    assert(card_io_recovery_service()==ESP_ERR_NOT_FINISHED&&mounts==1U);now_ms+=250U;fail_mount=false;
    assert(card_io_recovery_service()==ESP_OK&&mounts==2U&&!s.writer_error);preserved();
    puts("PASS active recovery: bounded backoff retries transient mount failure without losing retained frames");
    reset();request();foreign_card=true;assert(card_io_recovery_service()==ESP_ERR_INVALID_STATE&&stop_requested&&s_card_failed&&!s_card_io_recovery.pending&&!scans);preserved();
    reset();request();now_ms+=10000U;assert(card_io_recovery_service()==ESP_ERR_TIMEOUT&&!mounts&&stop_requested&&s_card_failed);preserved();
    reset();request();shutdown=true;assert(card_io_recovery_service()==ESP_ERR_INVALID_STATE&&!mounts&&stop_requested);preserved();
    puts("PASS active recovery: changed card, deadline and shutdown stop safely; foreign media never receives this scene");
    reset();request();stop_during_mount=true;assert(card_io_recovery_service()==ESP_OK&&stop_requested&&!capture_running&&!s_card_io_recovery.pending);preserved();
    reset();request();late_mount=true;assert(card_io_recovery_service()==ESP_ERR_TIMEOUT&&stop_requested&&s_card_failed&&!s_card_io_recovery.pending);preserved();
    reset();request();shutdown_during_mount=true;assert(card_io_recovery_service()==ESP_ERR_INVALID_STATE&&stop_requested&&s_card_failed&&!s_card_io_recovery.pending);preserved();
    puts("PASS active recovery: user stop and late completion cannot restart capture; timeout published before writer release");
    reset();request();blocked_prepare=true;assert(card_io_recovery_service()==ESP_ERR_NOT_FINISHED&&!mounts);preserved();
    reset();request();restored.owner[0]^=1U;assert(card_io_recovery_service()==ESP_ERR_NOT_FINISHED&&!s_media_ready&&s_card_io_recovery.pending);preserved();
    puts("PASS active recovery: retained file leases and mismatched restored owner fail closed without cross-scene mutation");
    reset();request();foreign_binding=true;assert(card_io_recovery_service()==ESP_ERR_INVALID_STATE&&stop_requested&&s_card_failed&&!preparations);
    reset();request();binding_during_scan=true;assert(card_io_recovery_service()==ESP_ERR_INVALID_STATE&&stop_requested&&s_card_failed&&!s_card_io_recovery.pending);
    reset();request();++s.capture_generation;assert(card_io_recovery_service()==ESP_ERR_INVALID_STATE&&stop_requested&&!mounts);
    puts("PASS active recovery: binding before/during recovery and stale capture generation cannot resume storage");
    reset();request();fail_filename_scan=true;
    assert(card_io_recovery_service()==ESP_ERR_NOT_FINISHED&&s_card_io_recovery.pending&&!s_media_ready);
    fail_filename_scan=false;now_ms+=250U;
    assert(card_io_recovery_service()==ESP_OK&&s.next_index==130U&&!s_card_io_recovery.pending);preserved();
    reset();request();s_card_file_fault[0].armed=true;
    strcpy(s_card_file_fault[0].session_id,s_card_session.session_id);
    s_card_file_fault[0].index=s_card_segment.index;recovered_tail.missing=1U;
    assert(card_io_recovery_service()==ESP_ERR_INVALID_CRC&&stop_requested&&s_card_failed&&!s.capture_complete&&!s_card_io_recovery.pending);
    puts("PASS recovery classification: failed filename scan cannot resume; a skipped active-scene file stops before writer release");
    reset();request();s_card_file_fault[0].armed=true;
    strcpy(s_card_file_fault[0].session_id,s_card_session.session_id);
    s_card_file_fault[0].index=s_card_segment.index;
    recovered_tail.missing=1U;recovered_tail.sealed=1U;recovered_tail.total_bytes=794U;
    strcpy(recovered_tail.path,"sealed_original.ready");
    assert(card_io_recovery_service()==ESP_OK&&s_card_partial&&!s_card_failed&&!stop_requested&&capture_running);
    assert(!s_card_damage.armed);preserved();
    reset();request();s_card_damage.armed=true;s_card_damage.index=s_card_segment.index+1U;
    s_card_file_fault[0].armed=true;strcpy(s_card_file_fault[0].session_id,s_card_session.session_id);
    s_card_file_fault[0].index=s_card_segment.index;
    recovered_tail.missing=1U;recovered_tail.sealed=1U;recovered_tail.total_bytes=794U;
    strcpy(recovered_tail.path,"sealed_original.ready");
    assert(card_io_recovery_service()==ESP_OK&&s_card_partial&&!s_card_failed&&!stop_requested&&capture_running);
    reset();s_capture_recovery_until=now_ms+1000U;request();now_ms+=1000U;
    assert(card_io_recovery_service()==ESP_ERR_TIMEOUT&&!mounts&&stop_requested);
    reset();request();s_offline_verify_running=true;
    assert(card_io_recovery_service()==ESP_ERR_NOT_FINISHED&&!mounts);
    s_offline_verify_running=false;
    assert(card_io_recovery_service()==ESP_OK&&!s_card_io_recovery.pending);
    puts("PASS active policy: isolated and repeated sealed-file losses keep recording as partial; damaged active prefix still stops; grace cannot renew; verifier quiesces before remount");
    for (unsigned corrupt_prefix = 0U; corrupt_prefix < 2U; ++corrupt_prefix) {
        reset();uint8_t accepted[662];memset(accepted,0x71,sizeof(accepted));
        s.frame_count=1U;s.payload_bytes=sizeof(accepted);
        s.payload_crc32=crc32_update(0U,accepted,sizeof(accepted));
        request();recovered_tail.sealed=1U;recovered_tail.total_bytes=794U;
        strcpy(recovered_tail.path,"prefix.ready");
        if(corrupt_prefix)accepted[100]^=0x80U;
        FILE *file=fopen(recovered_tail.path,"wb");assert(file);
        uint8_t header[96]={0},footer[32]={0};uint32_t bytes=sizeof(accepted);
        assert(fwrite(header,1,sizeof(header),file)==sizeof(header)&&fwrite(&bytes,1,4,file)==4);
        assert(fwrite(accepted,1,sizeof(accepted),file)==sizeof(accepted)&&fwrite(footer,1,sizeof(footer),file)==sizeof(footer)&&!fclose(file));
        const esp_err_t result=card_io_recovery_service();
        assert(!independent_prefix&&!deadline_owned);
        if(corrupt_prefix)assert(result==ESP_ERR_INVALID_CRC&&stop_requested&&s_card_failed);
        else assert(result==ESP_OK&&!stop_requested&&!s_card_failed);
        assert(!remove(recovered_tail.path));
    }
    puts("PASS accepted-prefix proof: recovered metadata cannot certify changed PCM; original pre-fault RAM CRC required, independent retry bounded, damaged open-prefix stops before writer release");
    spool_item_t item = {.generation=33U, .bytes=662U};
    uint32_t sequence=99U; memcpy(item.data+10U,&sequence,4U);
    for (unsigned scenario=0U;scenario<4U;++scenario) {
        reset(); s.writer_frame_uncertain=true;
        assert(card_io_recovery_request_locked(ESP_FAIL,&item));
        recovered_tail.sealed=1U; recovered_tail.total_bytes=96U+4U+662U+32U;
        strcpy(recovered_tail.path,"retry.ready");
        FILE *file=fopen(recovered_tail.path,"wb"); assert(file);
        uint8_t header[96]={0}, data[662], footer[32]={0}; memcpy(data,item.data,sizeof(data));
        if(scenario==1U) { uint32_t old=98U;memcpy(data+10U,&old,4U); }
        if(scenario==2U) data[80]^=1U;
        memcpy(footer,"NWXEND1",7U);uint32_t length=sizeof(data);
        assert(fwrite(header,1U,sizeof(header),file)==sizeof(header));
        assert(fwrite(&length,1U,4U,file)==4U && fwrite(data,1U,sizeof(data),file)==sizeof(data));
        assert(fwrite(footer,1U,sizeof(footer),file)==sizeof(footer) && !fclose(file));
        if(scenario==3U) recovered_tail.missing=1U;
        esp_err_t result=card_io_recovery_service();
        if(scenario==2U) assert(result==ESP_ERR_NOT_FINISHED && s_card_io_recovery.pending && !s_card_io_recovery.skip_retry);
        else { assert(result==ESP_OK);assert(card_io_recovery_consume_retry_locked(&item)==(scenario==0U)); }
        assert(!card_io_recovery_consume_retry_locked(&item));assert(!remove(recovered_tail.path));
    }
    puts("PASS active recovery: uncertain fwrite retry compares recovered tail; exactly once after full persistence, retries truncated tail, conflicting CRC fails closed");
    return 0;
}
