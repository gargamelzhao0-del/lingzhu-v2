#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/stat.h>
#include "sd_audio_scene_protocol.h"
#include "sd_audio_catalog.h"
#include "sd_audio_monitor_fifo.h"
#include "sd_audio_control_limits.h"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_LOGI(tag, fmt, ...) printf(fmt "\n", __VA_ARGS__)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portMAX_DELAY UINT32_MAX
typedef struct { unsigned unused; } nuwax_ws_client_t;
#define NUWAX_WS_ARCHIVE_LIFECYCLE_LOCK_TIMEOUT_MS 25U
#define NUWAX_WS_ARCHIVE_IO_TIMEOUT_MS 250U
static size_t largest_control_text;
static esp_err_t send_frame_with_timeout(nuwax_ws_client_t *ws, uint8_t opcode,
    const uint8_t *data, size_t bytes, uint32_t admission_ms, uint32_t io_ms,
    bool interrupt, bool lifecycle, uint32_t epoch)
{
    (void)ws; (void)data; (void)admission_ms; (void)io_ms; (void)interrupt; (void)lifecycle;
    assert(opcode == 1U && epoch && bytes <= NUWAX_SD_CONTROL_TEXT_MAX_BYTES);
    if (bytes > largest_control_text) largest_control_text = bytes;
    return ESP_OK;
}
#include "sd_archive_text_host.inc"
static struct {int lock; atomic_bool capture_complete; struct { sd_scene_ack_t scene; } replay_ack; } s;
static uint32_t s_card_boot_id=7;
static unsigned begins, ends, queries, query_files, nonce;
static bool held, reject, malformed, end_timeout;
static uint32_t remote_offset;
static char armed_type[48];
static unsigned allocation_count, allocation_failure, live_allocations;
static bool check_time;
static bool fail_scene_allocation;
static void *checked_malloc(size_t bytes) {
    if (++allocation_count == allocation_failure) return NULL;
    void *pointer = malloc(bytes); if (pointer) ++live_allocations; return pointer;
}
static void checked_free(void *pointer) {
    if (pointer) { assert(live_allocations); --live_allocations; free(pointer); }
}
static void *heap_caps_calloc(size_t a,size_t b,unsigned caps) {(void)caps;return fail_scene_allocation?NULL:calloc(a,b);}
static int xSemaphoreTake(int lock,unsigned wait) {(void)lock;(void)wait;assert(!held);held=true;return pdTRUE;}
static void xSemaphoreGive(int lock) {(void)lock;assert(held);held=false;}
static uint64_t esp_timer_get_time(void) {return nonce*1000ULL;}
static uint32_t esp_random(void) {return ++nonce;}
static esp_err_t replay_arm_ack(uint32_t epoch,const char *request,const char *type,const char *key) {
    assert(epoch && request[0] && key[0]);memset(&s.replay_ack,0,sizeof(s.replay_ack));strcpy(armed_type,type);return ESP_OK;
}
static void replay_disarm_ack(void) {}
static esp_err_t replay_send_json(nuwax_ws_client_t *ws,uint32_t epoch,const cJSON *json,bool independent) {
    (void)ws;(void)epoch;(void)independent;assert(!held);
    char *wire = cJSON_PrintUnformatted(json); assert(wire);
    const esp_err_t admitted = nuwax_ws_send_archive_text_if_epoch(ws, wire, epoch);
    free(wire);
    if (admitted != ESP_OK) return admitted;
    const char *type=cJSON_GetObjectItemCaseSensitive(json,"type")->valuestring;
    const cJSON *time=cJSON_GetObjectItemCaseSensitive(json,"session_time");
    assert(cJSON_IsObject(time));
    if(check_time) {
        assert(cJSON_GetObjectItemCaseSensitive(time,"started_at_ms")->valuedouble==1792634399000.0);
        assert(cJSON_GetObjectItemCaseSensitive(time,"duration_ms")->valuedouble==2000.0);
    }
    if(strstr(type,"session_end")){ends++;return ESP_OK;}
    begins++;assert(!cJSON_HasObjectItem(json,"segment_index") && !cJSON_HasObjectItem(json,"total_bytes"));
    assert(!cJSON_HasObjectItem(json,"window_bytes") && !cJSON_HasObjectItem(json,"segment_id"));
    const cJSON *resume=cJSON_GetObjectItemCaseSensitive(json,"resume");
    if(resume) {
        queries++;query_files=cJSON_GetArraySize(resume);assert(query_files>0 && query_files<=16);
        assert(query_files==s.replay_ack.scene.count);
        for(unsigned i=0;i<query_files;i++) {
            const cJSON *item=cJSON_GetArrayItem(resume,(int)i);
            assert(cJSON_GetObjectItemCaseSensitive(item,"segment_index")->valuedouble==s.replay_ack.scene.items[i].index);
            assert(cJSON_GetObjectItemCaseSensitive(item,"total_bytes")->valuedouble==32096);
            s.replay_ack.scene.items[i].offset=remote_offset;
        }
    }
    return ESP_OK;
}
static esp_err_t replay_wait_ack(nuwax_ws_client_t *ws,uint32_t epoch,const char *request,bool chunk,bool bulk,uint32_t *offset) {
    (void)ws;(void)epoch;(void)request;(void)offset;assert(!held && !chunk && !bulk);
    if(end_timeout && strstr(armed_type,"session_end"))return ESP_ERR_TIMEOUT;
    return reject?ESP_FAIL:malformed?ESP_ERR_INVALID_RESPONSE:ESP_OK;
}
#include "sd_audio_scene_replay.inc"
#include "sd_scene_end_host.inc"
static sd_audio_session_t make_owned(const char *id,unsigned count,unsigned boot, uint8_t owner) {
    static unsigned physical_order;
    sd_audio_session_t session={0};strcpy(session.session_id,id);session.boot_id=boot;
    memset(session.owner,owner,sizeof(session.owner));
    session.ended=session.complete=1;session.segment_count=count;
    assert(sd_audio_catalog_create(&session)==ESP_OK);
    for(unsigned i=0;i<count;i++) {
        sd_audio_segment_t file={0};file.index=i;file.sealed=1;file.total_bytes=32096;file.payload_crc=777;
        snprintf(file.path,sizeof(file.path),"./card_%s_%u.ready",id,++physical_order);
        assert(sd_audio_catalog_write(id,&file)==ESP_OK);
    }
    return session;
}
static sd_audio_session_t make(const char *id,unsigned count,unsigned boot) {
    return make_owned(id,count,boot,0);
}
/* These scene-cache tests call end directly, outside the selector. Publish
 * only the cursor proven by the real durable file ACKs, then allow its bounded
 * end proof to span several invocations as the production worker does. */
static esp_err_t checked_scene_end(nuwax_ws_client_t *ws,uint32_t epoch,bool independent,sd_audio_session_t *scene) {
    sd_audio_session_t current;assert(sd_audio_catalog_load(scene->session_id,&current)==ESP_OK);
    for(uint32_t i=0;i<current.segment_count;i++) {
        sd_audio_segment_t file;assert(sd_audio_catalog_read(current.session_id,i,&file)==ESP_OK);
        assert(file.acknowledged||file.missing);
    }
    current.first_pending=current.segment_count;
    assert(sd_audio_catalog_save(&current)==ESP_OK);*scene=current;
    for(unsigned i=0;i<128;i++) {
        const esp_err_t err=card_session_end(ws,epoch,independent,scene);
        if(err!=ESP_ERR_NOT_FINISHED)return err;
    }
    assert(!"end verification did not finish");return ESP_FAIL;
}
static esp_err_t prepare(nuwax_ws_client_t *ws,unsigned epoch,sd_audio_session_t *session,unsigned index,uint32_t *offset) {
    sd_audio_segment_t file;assert(sd_audio_catalog_read(session->session_id,index,&file)==ESP_OK);
    return card_scene_prepare(ws,epoch,true,session,&file,offset);
}
/* Join the real scheduler, catalog and scene cache. Only transport/file bytes
 * are mocked: every failed commit follows a real begin and durable attempted
 * mark, so worker rotation cannot silently depend on a disconnected epoch. */
static atomic_bool s_card_pending_upload=true;
static uint32_t s_monitor_retry_until, s_monitor_retry_delay;
static uint32_t s_meeting_retry_until, s_meeting_retry_delay;
static const void *s_monitor_retry_transport;
static uint32_t s_monitor_retry_epoch;
static uint8_t s_monitor_retry_owner[32];
static uint32_t offline_now_ms(void) {return (uint32_t)(esp_timer_get_time()/1000);}
static bool offline_deadline_elapsed(uint32_t now,uint32_t end) {return (int32_t)(now-end)>=0;}
static esp_err_t s_card_pass_error;
static bool s_card_preempted;
static unsigned scheduler_bad, scheduler_good;
static bool nuwax_sd_session_monitoring(void) {return false;}
static bool nuwax_offline_audio_storage_busy(void) {return false;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws) {(void)ws;return true;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws) {(void)ws;return 6;}
static bool card_owner(uint8_t owner[32]) {memset(owner,0x31,32);return true;}
static void vTaskDelay(unsigned ms) {assert(!held && ms);}
static esp_err_t replay_wait_capture_headroom(nuwax_ws_client_t *ws,uint32_t epoch,
    const atomic_bool *cancel,uint64_t *wait_us) {
    (void)ws;(void)epoch;(void)cancel;(void)wait_us;assert(!held);return ESP_OK;
}
static esp_err_t recheck_archive_expected(const sd_audio_segment_t *segment) {(void)segment;return ESP_OK;}
static esp_err_t replay_one_ready(nuwax_ws_client_t *ws,uint32_t epoch,const char *path,bool independent) {
    (void)independent;assert(!held);
    const char *name=strrchr(path,'/');assert(name&& !strncmp(++name,"card_",5));
    const char *number=strrchr(name,'_');assert(number&&number>name+5);
    char id[48];const size_t bytes=(size_t)(number-(name+5));assert(bytes<sizeof(id));
    memcpy(id,name+5,bytes);id[bytes]='\0';
    sd_audio_session_t session;assert(sd_audio_catalog_load(id,&session)==ESP_OK);
    uint32_t offset=0;
    esp_err_t err=prepare(ws,epoch,&session,0,&offset);
    if(err!=ESP_OK)return err;
    if(strstr(id,"bad")){scheduler_bad++;return ESP_FAIL;}
    scheduler_good++;return sd_audio_catalog_ack(id,0);
}
#include "sd_audio_file_fault_host.inc"
#include "sd_scene_scheduler_host.inc"

static void check_scheduler_cache(nuwax_ws_client_t *ws) {
    card_scene_reset();remote_offset=0;
    for(unsigned i=0;i<=65;i++) {
        char id[48];
        if(i==65) strcpy(id,"zz_sched_good");else snprintf(id,sizeof(id),"sched_bad_%03u",i);
        sd_audio_session_t session=make_owned(id,1,7,0x31);
        session.ended_ms=1000+i;
        assert(sd_audio_catalog_save(&session)==ESP_OK);
        sd_audio_segment_t file;assert(sd_audio_catalog_read(id,0,&file)==ESP_OK);
        FILE *f=fopen(file.path,"wb");assert(f);fputs("retained audio",f);assert(!fclose(f));
    }
    for(unsigned pass=0;pass<65;pass++) {
        sd_audio_catalog_replay_reset();s_card_pass_error=ESP_OK;
        s_meeting_retry_delay=s_meeting_retry_until=0;
        for(unsigned i=0;i<1200 && scheduler_bad<=pass;i++) {
            bool worked=false;assert(card_replay_next(ws,6,false,&worked)==ESP_OK);
            assert(!held);
            unsigned contexts=0;
            for(unsigned j=0;j<SD_SCENE_CONTEXTS;j++) contexts+=s_scene_contexts[j].id[0]!=0;
            assert(contexts<=1); // Failed queue heads release volatile scene state.
        }
        assert(scheduler_bad==pass+1&&!scheduler_good&&s_meeting_retry_delay);
        char id[48];snprintf(id,sizeof(id),"sched_bad_%03u",pass);
        sd_audio_segment_t file;assert(sd_audio_catalog_read(id,0,&file)==ESP_OK);
        assert(!file.acknowledged&&!file.missing&&(file.sealed&SD_AUDIO_SEGMENT_ATTEMPTED));
        FILE *f=fopen(file.path,"rb");assert(f);fclose(f);
        /* Simulate a later durable commit resolution, then the next scene can
         * run. Never skip an unconfirmed file to test cache pressure. */
        assert(sd_audio_catalog_ack(id,0)==ESP_OK);
    }
    s_meeting_retry_delay=s_meeting_retry_until=0;
    for(unsigned i=0;i<1200&&!scheduler_good;i++) {
        bool worked=false;assert(card_replay_next(ws,6,false,&worked)==ESP_OK);
    }
    assert(scheduler_good==1 && scheduler_bad>=65);
    for(unsigned i=0;i<65;i++) {
        char id[48];snprintf(id,sizeof(id),"sched_bad_%03u",i);
        sd_audio_segment_t file;assert(sd_audio_catalog_read(id,0,&file)==ESP_OK);
        assert(file.acknowledged && !file.missing && (file.sealed&SD_AUDIO_SEGMENT_ATTEMPTED));
    }
    puts("PASS: scheduler + scene cache + catalog: 65 failed heads release cache without bypass; later scene succeeds after durable resolution on same connection");
}
static cJSON *control_json(unsigned kind) {
    if (kind < 4) return sd_audio_commit_json(kind, "req", "legacy", "scene", 1, 32096, 777, false);
    if (kind == 4) return sd_audio_commit_json(SD_UPLOAD_LEGACY, "req", "legacy", "scene", 1, 32096, 777, true);
    return sd_audio_end_json(kind == 6, "req", "scene", 2, true, NULL);
}
static void check_allocation_failures(nuwax_ws_client_t *ws) {
    cJSON_Hooks hooks = {.malloc_fn=checked_malloc, .free_fn=checked_free};
    for (unsigned kind=0;kind<7;kind++) {
        allocation_count=allocation_failure=0;cJSON_InitHooks(&hooks);
        cJSON *json=control_json(kind);assert(json);
        const unsigned count=allocation_count;
        cJSON_Delete(json);assert(!live_allocations);
        for(unsigned fail=1;fail<=count;fail++) {
            allocation_count=0;allocation_failure=fail;
            assert(!control_json(kind));assert(!live_allocations);
        }
        cJSON_InitHooks(NULL);
    }
    sd_audio_session_t session=make("oom_retry",0,7);
    card_scene_context_t *ctx=card_scene_context(ws,2,session.session_id);assert(ctx);ctx->begun=true;
    const unsigned before=ends;
    allocation_count=allocation_failure=0;cJSON_InitHooks(&hooks);
    cJSON *json=sd_audio_end_json(false,"req",session.session_id,0,true,NULL);assert(json);
    const unsigned count=allocation_count;cJSON_Delete(json);
    for(unsigned fail=1;fail<=count;fail++) {
        allocation_count=0;allocation_failure=fail;
        assert(checked_scene_end(ws,2,true,&session)==ESP_ERR_NO_MEM);
        assert(ends==before && !session.done && !live_allocations);
    }
    cJSON_InitHooks(NULL);
    assert(checked_scene_end(ws,2,true,&session)==ESP_OK && ends==before+1 && session.done);
    puts("PASS: every control allocation fault frees partial JSON; end emits nothing/keeps scene pending; memory recovery succeeds");
}
static void check_scene_cache_pressure(nuwax_ws_client_t *ws) {
    card_scene_reset();
    sd_audio_session_t active=make("cache_active",1,7);
    sd_audio_session_t oldest=make("cache_evicted",1,7);
    uint32_t offset=0;remote_offset=7000;
    assert(prepare(ws,5,&oldest,0,&offset)==ESP_OK && offset==0);
    card_scene_sent(9000); // Volatile bytes exceed the peer's durable prefix.
    assert(prepare(ws,5,&active,0,&offset)==ESP_OK && offset==0);
    card_scene_sent(8000);
    for(unsigned i=0;i<200;i++) {
        char id[48];snprintf(id,sizeof(id),"cache_busy_%03u",i);
        sd_audio_session_t session=make(id,1,7);
        assert(prepare(ws,5,&session,0,&offset)==ESP_OK && offset==0);
        card_scene_file_done(); // A commit failed after a successful begin.
        sd_audio_catalog_replay_reset(); // Worker restart, same transport/epoch.
        unsigned before=begins;
        assert(prepare(ws,5,&active,0,&offset)==ESP_OK && offset==8000 && begins==before);
    }
    unsigned before=begins, before_queries=queries;
    assert(prepare(ws,5,&oldest,0,&offset)==ESP_OK && offset==7000);
    assert(begins==before+1 && queries==before_queries+1 && query_files==1);
    sd_audio_segment_t retained;
    assert(sd_audio_catalog_read(oldest.session_id,0,&retained)==ESP_OK);
    assert(!retained.acknowledged && !retained.missing && (retained.sealed&SD_AUDIO_SEGMENT_ATTEMPTED));
    card_scene_forget(oldest.session_id);offset=123;reject=true;
    for(unsigned i=0;i<40;i++) {
        char id[48];snprintf(id,sizeof(id),"cache_reject_%03u",i);
        sd_audio_session_t session=make(id,1,7);
        assert(prepare(ws,5,&session,0,&offset)==ESP_FAIL && offset==123);
    }
    reject=false;
    assert(prepare(ws,5,&oldest,0,&offset)==ESP_OK && offset==7000);
    free(s_scene_contexts);s_scene_contexts=NULL;card_scene_reset();
    fail_scene_allocation=true;before=begins;
    assert(prepare(ws,5,&oldest,0,&offset)==ESP_ERR_NO_MEM && begins==before);
    fail_scene_allocation=false;
    assert(prepare(ws,5,&oldest,0,&offset)==ESP_OK && offset==7000);
    puts("PASS: 200 scenes on one epoch; hot preemption offset retained; evicted scene queries durable prefix; rejected begin and real allocation failure recover without source ACK");
}
int main(void) {
    assert(sd_audio_catalog_init()==ESP_OK);
    nuwax_ws_client_t ws={0};uint32_t offset;
    sd_audio_session_t session=make("meeting_one",3,7);
    assert(prepare(&ws,1,&session,0,&offset)==ESP_OK && offset==0 && begins==1 && queries==0);
    card_scene_sent(8000);
    assert(prepare(&ws,1,&session,1,&offset)==ESP_OK && offset==0 && begins==1);
    assert(sd_audio_catalog_ack(session.session_id,1)==ESP_OK);card_scene_file_done();
    assert(prepare(&ws,1,&session,0,&offset)==ESP_OK && offset==8000 && begins==1);
    card_scene_file_done();remote_offset=7000; // Failed transfer: only durable prefix is trusted.
    assert(prepare(&ws,1,&session,0,&offset)==ESP_OK && offset==7000 && begins==2 && query_files==1);
    card_scene_reset();remote_offset=4000; // Recreated test transport may reuse its epoch.
    assert(prepare(&ws,1,&session,0,&offset)==ESP_OK && offset==4000 && begins==3);
    assert(prepare(&ws,2,&session,0,&offset)==ESP_OK && begins==4);
    // Restart recovery batches: sixteen files, then the remaining four.
    sd_audio_session_t old=make("meeting_before_restart",20,6);remote_offset=32096;
    unsigned before=begins;
    for(unsigned i=0;i<20;i++) {
        assert(prepare(&ws,2,&old,i,&offset)==ESP_OK && offset==32096);
        assert(sd_audio_catalog_ack(old.session_id,i)==ESP_OK);card_scene_file_done();
    }
    assert(begins==before+2 && query_files==4);
    before=begins;end_timeout=true;
    assert(checked_scene_end(&ws,2,true,&old)==ESP_ERR_TIMEOUT);
    assert(begins==before && ends==1);
    end_timeout=false;assert(checked_scene_end(&ws,2,true,&old)==ESP_OK && begins==before && ends==2);
    // Empty session still registers once before final confirmation.
    sd_audio_session_t empty=make("empty",0,7);
    assert(checked_scene_end(&ws,2,true,&empty)==ESP_OK && begins==before+1);
    // Malformed/rejected recovery cannot publish offsets or mark the local file ACKed.
    card_scene_reset();malformed=true;offset=123;
    assert(prepare(&ws,2,&session,0,&offset)==ESP_ERR_INVALID_RESPONSE && offset==123);
    malformed=false;reject=true;assert(prepare(&ws,2,&session,0,&offset)==ESP_FAIL && offset==123);
    reject=false;assert(prepare(&ws,2,&session,0,&offset)==ESP_OK && offset==32096);
    sd_audio_segment_t retained;assert(sd_audio_catalog_read(session.session_id,0,&retained)==ESP_OK);
    assert(!retained.acknowledged && (retained.sealed&SD_AUDIO_SEGMENT_ATTEMPTED));
    assert(sd_audio_catalog_block(session.session_id)==ESP_OK);
    sd_audio_catalog_replay_reset();bool found,file;sd_audio_session_t picked;sd_audio_segment_t part;
    const uint8_t owner[32]={0};
    assert(sd_audio_catalog_next(owner,7,10000,0,NULL,false,0,&picked,&part,&found,&file)==ESP_OK && !found);
    check_allocation_failures(&ws);
    sd_audio_session_t timed=make("timed",0,7);
    nuwax_cloud_time_anchor_t anchor={UINT64_C(1792634400000),1000};
    sd_audio_session_time_t time;
    assert(sd_audio_catalog_session_time(&timed,7,&anchor,true,2000,&time)==ESP_OK);
    check_time=true;
    assert(checked_scene_end(&ws,3,true,&timed)==ESP_OK); // Production begin + end load the same persisted time.
    check_time=false;
    check_scene_cache_pressure(&ws);
    check_scheduler_cache(&ws);
    assert(largest_control_text > 512U && largest_control_text <= NUWAX_SD_CONTROL_TEXT_MAX_BYTES);
    char edge[NUWAX_SD_CONTROL_TEXT_MAX_BYTES+2U];memset(edge,'x',sizeof(edge));
    edge[NUWAX_SD_CONTROL_TEXT_MAX_BYTES]=0;
    assert(nuwax_ws_send_archive_text_if_epoch(&ws,edge,9)==ESP_OK);
    edge[NUWAX_SD_CONTROL_TEXT_MAX_BYTES]='x';edge[NUWAX_SD_CONTROL_TEXT_MAX_BYTES+1U]=0;
    assert(nuwax_ws_send_archive_text_if_epoch(&ws,edge,9)==ESP_ERR_INVALID_ARG);
    assert(nuwax_ws_send_archive_text_if_epoch(NULL,"x",9)==ESP_ERR_INVALID_ARG);
    assert(nuwax_ws_send_archive_text_if_epoch(&ws,"x",0)==ESP_ERR_INVALID_ARG);
    free(s_scene_contexts);
    puts("PASS: production scene state/cache; once per scene; preemption offsets; durable resume; epoch/restart; 16-file batches; end ACK retry; empty scene; malformed/refused ACK retention; permanent conflict isolation");
    return 0;
}
