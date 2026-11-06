#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned index_opens, manifest_opens;
static FILE *counted_fopen(const char *p, const char *m) {
    if (strstr(p, ".index") && !strcmp(m,"rb")) ++index_opens;
    if (strstr(p, ".session") && !strcmp(m,"rb")) ++manifest_opens;
    return fopen(p,m);
}
#define fopen counted_fopen
#define SD_AUDIO_ARCHIVE_ROOT "."
#define SD_AUDIO_ARCHIVE_NEW_ROOT "."
#include "sd_audio_catalog.c"
#undef fopen
#include <stdatomic.h>
typedef int nuwax_ws_client_t;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
enum {SD_SCENE_RETRY,SD_SCENE_PERMANENT};
static void card_scene_file_done(void) {}
static void card_scene_forget(const char *id) {(void)id;}
static struct {int lock; atomic_bool capture_complete; struct {struct {unsigned error;} scene;} replay_ack;} s;
static atomic_bool s_card_pending_upload=true;
static bool s_card_preempted, held;
static esp_err_t s_card_pass_error;
static uint32_t s_monitor_retry_until, s_monitor_retry_delay;
static uint32_t s_meeting_retry_until, s_meeting_retry_delay;
static const void *s_monitor_retry_transport;
static uint32_t s_monitor_retry_epoch;
static uint8_t s_monitor_retry_owner[32];
static uint32_t test_now=4000U;
static uint32_t offline_now_ms(void){return test_now;}
static bool offline_deadline_elapsed(uint32_t now,uint32_t deadline){return (int32_t)(now-deadline)>=0;}
static unsigned old_attempts, new_attempts, yielded, lock_failures;
static bool damaged, transient_read_error;
static esp_err_t replay_wait_capture_headroom(nuwax_ws_client_t *ws, uint32_t epoch,
    const atomic_bool *cancel, uint64_t *wait_us) {
    (void)ws; (void)epoch; (void)cancel; (void)wait_us; assert(!held); return ESP_OK;
}
static bool nuwax_sd_session_monitoring(void){return false;}
static bool nuwax_offline_audio_storage_busy(void){return false;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws){(void)ws;return true;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws){(void)ws;return 1;}
static bool card_owner(uint8_t *owner){memset(owner,0x31,32);return true;}
static uint64_t esp_timer_get_time(void){return 4000000U;}
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)printf(fmt "\n", __VA_ARGS__))
static int xSemaphoreTake(int lock,unsigned wait){(void)lock;(void)wait;assert(!held);if(lock_failures){lock_failures--;return 0;}held=true;return 1;}
static void xSemaphoreGive(int lock){(void)lock;assert(held);held=false;}
static void vTaskDelay(unsigned ms){assert(!held && ms>0);yielded++;}
static esp_err_t recheck_archive_expected(const sd_audio_segment_t *segment){(void)segment;return damaged?ESP_ERR_INVALID_CRC:ESP_OK;}
static esp_err_t card_session_end(nuwax_ws_client_t *ws,uint32_t epoch,bool independent,sd_audio_session_t *session){
    (void)ws;(void)epoch;(void)independent;assert(!held);session->done=1;return sd_audio_catalog_save(session);
}
static esp_err_t replay_one_ready(nuwax_ws_client_t *ws,uint32_t epoch,const char *path,bool independent){
    (void)ws;(void)epoch;(void)independent;assert(!held);
    if(strstr(path,"meeting_old")){old_attempts++;return damaged||transient_read_error?ESP_ERR_INVALID_CRC:ESP_FAIL;}
    assert(strstr(path,"meeting_new"));new_attempts++;return sd_audio_catalog_ack("meeting_new",0);
}
#include "sd_audio_file_fault_host.inc"
#include "sd_scheduler_host.inc"

static sd_audio_session_t create_session(const char *id, unsigned mode, unsigned count, unsigned ended) {
    sd_audio_session_t value={0}; strcpy(value.session_id,id); memset(value.owner,0x31,32);
    value.mode=mode;value.segment_count=count;value.ended=ended;value.complete=1;value.boot_id=7;value.start_ms=1000;value.ended_ms=2000;
    assert(sd_audio_catalog_create(&value)==ESP_OK);return value;
}
static void make_index(const char *id, unsigned count, bool acknowledged) {
    char path[128];assert(catalog_path(path,id,"index"));FILE *f=fopen(path,"wb");assert(f);
    for (unsigned i=0;i<count;++i) {
        sd_audio_segment_t pair[2]={{0}};sd_audio_segment_t *part=&pair[1];
        part->generation=1;part->magic=SEGMENT_MAGIC;part->index=i;part->sealed=1;part->acknowledged=acknowledged;
        const unsigned order=!strcmp(id,"meeting_old")?1U:!strcmp(id,"meeting_new")?2U:100U+i;
        snprintf(part->path,sizeof(part->path),"./card_%s_%u.ready",id,order);
        part->start_ms=1000+i*960U;part->end_ms=part->start_ms+960U;part->total_bytes=32096;
        part->crc=checksum(part,offsetof(sd_audio_segment_t,crc));
        assert(fwrite(pair,sizeof(pair),1,f)==1);
    } assert(fclose(f)==0);
}
int main(void) {
    assert(sd_audio_catalog_init()==ESP_OK);
    sd_audio_session_t good=create_session("good",0,0,1), out;
    FILE *t=fopen("catalog/interrupted_create.session","wb");assert(t);fclose(t);
    t=fopen("catalog/not_published.creating","wb");assert(t);fputs("torn",t);fclose(t);
    assert(sd_audio_catalog_recover(8)==ESP_OK && sd_audio_catalog_load("good",&out)==ESP_OK);
    t=fopen("catalog/interrupted_create.session.bad.0","rb");assert(t);fclose(t);
    good.done=1;assert(sd_audio_catalog_save(&good)==ESP_OK);
    sd_audio_session_t old=create_session("meeting_old",0,1,1), next=create_session("meeting_new",0,1,1);
    next.ended_ms=3000;assert(sd_audio_catalog_save(&next)==ESP_OK);make_index(old.session_id,1,false);make_index(next.session_id,1,false);
    sd_audio_segment_t segment;bool found,file,worked;nuwax_ws_client_t ws=0;
    lock_failures=1;
    assert(card_replay_next(&ws,1,true,&worked)==ESP_OK && worked && yielded==1 && !held);
    assert(sd_audio_catalog_read(old.session_id,0,&segment)==ESP_OK);
    t=fopen(segment.path,"wb");assert(t);fputs("retained after peer rejection",t);fclose(t);
    sd_audio_catalog_replay_reset();s_card_pass_error=ESP_OK;
    for(unsigned i=0;i<4;i++) {
        const esp_err_t err=card_replay_next(&ws,1,true,&worked);
        assert(err==ESP_OK || (err==ESP_ERR_TIMEOUT&&!worked));
    }
    assert(old_attempts==1 && new_attempts==0 && s_card_pass_error==ESP_FAIL && atomic_load(&s_card_pending_upload));
    assert(sd_audio_catalog_load(next.session_id,&out)==ESP_OK && !out.done);
    assert(sd_audio_catalog_read(old.session_id,0,&segment)==ESP_OK && !segment.acknowledged && !segment.missing);
    assert(!sd_audio_file_fault_confirm(&s_card_file_fault[0],old.session_id,0,atomic_load(&s_media_generation)));
    atomic_fetch_add(&s_media_generation,1U);transient_read_error=true;
    test_now+=30000U;sd_audio_catalog_replay_reset();s_card_pass_error=ESP_OK;
    assert(card_replay_next(&ws,1,true,&worked)==ESP_OK && worked && old_attempts==2);
    assert(!s_card_file_fault[0].armed && fault_media_healthy && !s_card_failed);
    assert(sd_audio_catalog_read(old.session_id,0,&segment)==ESP_OK && !segment.acknowledged && !segment.missing);
    transient_read_error=false;
    puts("PASS healthy independent recheck clears stale media-fault evidence without inventing an upload ACK");
    /* Next pass retries the rejected scene; confirmed damage is explicit and
     * retained, never turned into an ACK or a successful full recording. */
    t=fopen(segment.path,"wb");assert(t);fputs("damaged evidence",t);fclose(t);
    damaged=true;test_now+=30000U;sd_audio_catalog_replay_reset();s_card_pass_error=ESP_OK;
    assert(card_replay_next(&ws,1,true,&worked)==ESP_OK && worked && old_attempts==3);
    assert(sd_audio_catalog_read(old.session_id,0,&segment)==ESP_OK && !segment.missing && !segment.acknowledged);
    assert(!fault_media_healthy && s_replay_media_fault);
    fault_media_healthy=true;atomic_fetch_add(&s_media_generation,1U);
    test_now+=30000U;sd_audio_catalog_replay_reset();
    fault_active_capture=true;s_card_session=old;s.capture_complete=true;
    assert(card_replay_next(&ws,1,true,&worked)==ESP_OK && worked && old_attempts==4);
    assert(s_card_partial && !s_card_failed && s.capture_complete && !fault_stop_requested);fault_active_capture=false;
    assert(sd_audio_catalog_load(old.session_id,&out)==ESP_OK && !out.complete);
    assert(sd_audio_catalog_read(old.session_id,0,&segment)==ESP_OK && segment.missing && !segment.acknowledged);
    assert(card_replay_next(&ws,1,true,&worked)==ESP_OK && worked);
    struct stat discarded_st;assert(stat(segment.path,&discarded_st)!=0&&errno==ENOENT);
    for(unsigned i=0;i<4;i++) assert(card_replay_next(&ws,1,true,&worked)==ESP_OK);
    assert(new_attempts==1&&sd_audio_catalog_load(next.session_id,&out)==ESP_OK&&out.done);
    sd_audio_session_t mon=create_session("monitor_backlog",1,4096,0);make_index(mon.session_id,4096,true);
    unsigned calls=0;
    do {
        index_opens=0;
        assert(card_replay_next(&ws,1,true,&worked)==ESP_OK);
        assert(index_opens<=16);
        assert(++calls<300);
    } while(worked);
    assert(yielded>=255 && sd_audio_catalog_load(mon.session_id,&out)==ESP_OK && out.first_pending==4096);
    index_opens=0;
    assert(sd_audio_catalog_next(mon.owner,7,9000000,0,"monitor_backlog",true,1,&out,&segment,&found,&file)==ESP_OK);
    assert(!found && index_opens==0);
    puts("PASS: torn-manifest isolation, unpublished create, meeting FIFO head retry, retained damaged audio, bounded 4096-record cursor and unlocked yields");
    return 0;
}
