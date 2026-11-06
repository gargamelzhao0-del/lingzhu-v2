/* Actual selector/end transaction/catalog; only RTOS and wire I/O are doubles. */
#define main catalog_fixture_main
#include "sd_monitor_fifo_host_check.c"
#undef main
#include <stdatomic.h>
#include "sd_audio_scene_protocol.h"

typedef struct { uint32_t epoch; bool connected; } nuwax_ws_client_t;
static struct { int lock; atomic_bool capture_complete; struct { struct { unsigned error; } scene; } replay_ack; } s;
static atomic_bool s_card_pending_upload = true;
static uint32_t clock_ms=4000;
static bool held, s_card_preempted, fail_end_ack, fail_file, fail_persist;
static bool fail_meeting_file;
static unsigned file_attempts, end_sends, end_confirms, meeting_sent, yields;
static char attempted[48], last_end[48], armed_request[48], armed_scene[48];
static int64_t esp_timer_get_time(void) { return (int64_t)clock_ms*1000; }
static uint32_t offline_now_ms(void) { return clock_ms; }
static bool offline_deadline_elapsed(uint32_t now,uint32_t end) { return (int32_t)(now-end)>=0; }
static uint64_t card_online_since_ms(void) { return 0; }
static bool card_owner(uint8_t value[32]) { memcpy(value,owner,32); return true; }
static bool nuwax_sd_session_monitoring(void) { return false; }
static bool nuwax_offline_audio_storage_busy(void) { return false; }
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws) { return ws->connected; }
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws) { return ws->epoch; }
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_LOGI(tag,fmt,...) ((void)printf(fmt "\n",__VA_ARGS__))
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static int xSemaphoreTake(int lock,unsigned ticks) {
    (void)lock;(void)ticks;assert(!held);held=true;return pdTRUE;
}
static void xSemaphoreGive(int lock) { (void)lock;assert(held);held=false; }
static void vTaskDelay(unsigned ticks) { assert(!held&&ticks);yields++; }
static esp_err_t replay_wait_capture_headroom(nuwax_ws_client_t *ws,uint32_t epoch,
    const atomic_bool *cancel,uint64_t *elapsed) {
    (void)ws;(void)epoch;(void)cancel;(void)elapsed;assert(!held);return ESP_OK;
}
static void card_scene_forget(const char *id) { (void)id;assert(!held); }
static void card_scene_file_done(void) { assert(!held); }
static esp_err_t recheck_archive_expected(const sd_audio_segment_t *segment) { (void)segment;return ESP_OK; }
static esp_err_t card_scene_prepare(nuwax_ws_client_t *ws,uint32_t epoch,bool independent,
    sd_audio_session_t *scene,void *part,void *offset) {
    (void)ws;(void)epoch;(void)independent;(void)scene;(void)part;(void)offset;
    assert(!held);return ESP_OK;
}
static void card_scene_time(const sd_audio_session_t *scene,sd_audio_session_time_t *time) {
    (void)scene;memset(time,0,sizeof(*time));
}
static uint32_t esp_random(void) { static uint32_t number;return ++number; }
static esp_err_t replay_arm_ack(uint32_t epoch,const char *request,const char *ack,const char *id) {
    (void)epoch;assert(!held && strstr(ack,"audio_session_end_ack"));
    strcpy(armed_request,request);strcpy(armed_scene,id);return ESP_OK;
}
static esp_err_t replay_send_json(nuwax_ws_client_t *ws,uint32_t epoch,cJSON *json,bool independent) {
    (void)ws;(void)epoch;(void)independent;assert(!held);
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(json,"type");
    const cJSON *id=cJSON_GetObjectItemCaseSensitive(json,"audio_session_id");
    const cJSON *count=cJSON_GetObjectItemCaseSensitive(json,"segment_count");
    assert(cJSON_IsString(type)&&strstr(type->valuestring,"audio_session_end"));
    assert(cJSON_IsString(id)&&!strcmp(id->valuestring,armed_scene));
    sd_audio_session_t scene;assert(sd_audio_catalog_load(id->valuestring,&scene)==ESP_OK);
    assert(scene.ended&&!scene.done);
    if(scene.mode==1)assert(scene.first_pending==scene.segment_count);
    assert(cJSON_IsNumber(count)&&count->valuedouble==scene.segment_count);
    for(unsigned i=0;i<scene.segment_count;i++) {
        sd_audio_segment_t part;assert(sd_audio_catalog_read(scene.session_id,i,&part)==ESP_OK);
        assert(part.acknowledged||part.missing);
    }
    strcpy(last_end,scene.session_id);end_sends++;return ESP_OK;
}
static void replay_disarm_ack(void) { assert(!held); }
static esp_err_t replay_wait_ack(nuwax_ws_client_t *ws,uint32_t epoch,const char *request,
    bool window,bool other,void *result) {
    (void)ws;(void)epoch;(void)window;(void)other;(void)result;
    assert(!held&&!strcmp(request,armed_request));
    if(fail_end_ack)return ESP_FAIL;
    end_confirms++;
    if(fail_persist) {
        /* Fault after remote ACK: change metadata, so local proof must fail. */
        sd_audio_session_t scene;assert(sd_audio_catalog_load(armed_scene,&scene)==ESP_OK);
        scene.ended_ms++;assert(sd_audio_catalog_save(&scene)==ESP_OK);
    }
    return ESP_OK;
}
static esp_err_t replay_one_ready(nuwax_ws_client_t *ws,uint32_t epoch,const char *path,bool independent) {
    (void)ws;(void)epoch;(void)independent;assert(!held);
    char id[48];strcpy(id,selected_session.session_id);
    assert(selected_valid&&!strcmp(selected_segment.path,path));
    sd_audio_session_t scene;assert(sd_audio_catalog_load(id,&scene)==ESP_OK);
    const unsigned index=scene.first_pending;
    sd_audio_segment_t part;assert(sd_audio_catalog_read(id,index,&part)==ESP_OK&&!strcmp(path,part.path));
    strcpy(attempted,id);file_attempts++;
    if(scene.mode==0)meeting_sent++;
    if((fail_file&&scene.mode==1)||(fail_meeting_file&&scene.mode==0))return ESP_FAIL;
    /* Durable file commit ACK. The production archive uploader is separately tested. */
    return sd_audio_catalog_ack(id,index);
}
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define SD_FILE_FAULT_STATE_ONLY 1
#include "sd_audio_file_fault_host.inc"
#include "sd_audio_session_replay.inc"

static void step(nuwax_ws_client_t *ws) {
    bool worked;
    const esp_err_t err = card_replay_next(ws,ws->epoch,true,&worked);
    assert((err==ESP_OK || (err==ESP_ERR_TIMEOUT && !worked))&&!held);
    assert(!sd_audio_catalog_replay_pinned());
}
#ifndef SD_FIFO_REPLAY_HELPERS_ONLY
int main(void) {
    nuwax_ws_client_t ws={1,true};enter();
    sd_audio_session_t old=make_scene("transaction_old",1,2,10,7,1000,true,false);
    sd_audio_session_t young=make_scene("transaction_new",1,1,20,8,1,true,false);
    make_scene("transaction_meeting",0,1,30,8,2,true,false);
    fail_file=true;
    for(unsigned i=0;i<8;i++)step(&ws);
    assert(file_attempts==2&&meeting_sent==1&&end_sends==1&&s_monitor_retry_delay==1000);
    assert(!strcmp(last_end,"transaction_meeting"));
    assert(sd_audio_catalog_load(young.session_id,&young)==ESP_OK&&!young.done&&young.first_pending==0);
    /* Retry deadline does not block meetings; reconnect immediately retries same monitor. */
    fail_file=false;ws.epoch++;step(&ws);assert(!strcmp(attempted,old.session_id));
    step(&ws);assert(!strcmp(attempted,old.session_id));
    assert(sd_audio_catalog_load(old.session_id,&old)==ESP_OK&&old.first_pending==1);
    fail_end_ack=true;step(&ws);assert(!strcmp(last_end,old.session_id));
    unsigned attempts=file_attempts;
    for(unsigned i=0;i<8;i++)step(&ws);
    assert(file_attempts==attempts);
    assert(sd_audio_catalog_load(old.session_id,&old)==ESP_OK&&!old.done);
    fail_end_ack=false;fail_persist=true;clock_ms+=31000;step(&ws);
    assert(sd_audio_catalog_load(old.session_id,&old)==ESP_OK&&!old.done);
    assert(file_attempts==attempts);fail_persist=false;clock_ms+=31000;
    step(&ws);assert(sd_audio_catalog_load(old.session_id,&old)==ESP_OK&&old.done);
    step(&ws);assert(!strcmp(attempted,young.session_id));step(&ws);
    assert(sd_audio_catalog_load(young.session_id,&young)==ESP_OK&&young.done);
    puts("PASS FIFO transaction: actual selector/end JSON, file ACK order, failure backoff, independent meeting, reconnect, lost end ACK and failed local proof never let next monitor pass");
    leave();enter();atomic_store(&s_card_pending_upload,true);s_card_pass_error=ESP_OK;
    old=make_scene("wrap_old",1,2,10,7,1000,true,false);
    young=make_scene("wrap_new",1,1,20,8,1,true,false);
    clock_ms=0U-1000U;fail_file=true;step(&ws);
    assert(s_monitor_retry_delay==1000U&&s_monitor_retry_until==0U);
    attempts=file_attempts;
    step(&ws);assert(file_attempts==attempts); /* Zero is a valid wrapped deadline. */
    clock_ms+=500U;step(&ws);assert(file_attempts==attempts);
    make_scene("wrap_meeting",0,1,30,8,2,true,false);
    unsigned meetings_before=meeting_sent;
    step(&ws);assert(meeting_sent==meetings_before+1U&&!strcmp(attempted,"wrap_meeting"));
    step(&ws);assert(file_attempts==attempts+1U);
    assert(sd_audio_catalog_load(young.session_id,&young)==ESP_OK&&!young.done&&!young.first_pending);
    clock_ms+=500U;fail_file=false;assert(clock_ms==0U);
    step(&ws);assert(!strcmp(attempted,old.session_id));
    step(&ws);assert(!strcmp(attempted,old.session_id));
    step(&ws);assert(sd_audio_catalog_load(old.session_id,&old)==ESP_OK&&old.done);
    step(&ws);assert(!strcmp(attempted,young.session_id));step(&ws);
    assert(sd_audio_catalog_load(young.session_id,&young)==ESP_OK&&young.done);
    puts("PASS FIFO retry wrap: wrapped zero deadline still defers old monitor until due; meeting progresses; old then new complete after wrap");
    leave();return 0;
}
#endif
