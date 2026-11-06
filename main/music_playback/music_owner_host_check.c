/* Exercises the production owner include, with asynchronous owners simulated. */
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_NUWAX_MUSIC_PLAYBACK_ENABLE 1
#define CONFIG_NUWAX_MUSIC_CONTROL_TEST_ENABLE 0
#define CONFIG_NUWAX_MUSIC_URL_TEST_ENABLE 0
#define CONFIG_NUWAX_OTA_ENABLE 1
#define NUWAX_SESSION_MUSIC_URL_MAX_LEN 1024
#define NUWAX_APP_STATE_IDLE 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_OK 0
typedef int esp_err_t;
typedef struct { bool connected; uint32_t epoch; } nuwax_ws_client_t;
typedef struct { char state[16], request_id[80]; } nuwax_music_snapshot_t;
static atomic_bool s_product_music_owned, s_product_music_cancel;
static bool s_power_shutdown_requested, power, provisioning, exclusive, mode, notification, ota;
static bool capture, uplink, wake, speaker, worker, worker_audio, race_cancel;
static bool s_post_playback_resume_pending;
static struct { bool capture_active; } s_prelisten;
static int s_notification_transaction;
typedef struct { bool active; } sleep_t;
static sleep_t s_sleep_transaction, s_deferred_sleep_intent;
static void *s_wake_word_events;
static uint32_t now_ms;
static uint32_t release_delay_ms;
static bool cancel_on_release;
static unsigned starts, wake_stops, fade_stops, sleeps, restores, releases, terminals, visuals;
static char started_id[80], cancelled_reason[48], terminal_reason[48];
static size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if(n) {size_t c = l < n-1 ? l : n-1; memcpy(d,s,c);d[c]=0;}return l; }
static uint32_t monotonic_ms(void) { return now_ms; }
static bool nuwax_power_ctrl_shutdown_preparing(void) { return power; }
static bool reprovision_request_pending(void) { return provisioning; }
static bool exclusive_audio_mode_blocks_normal_audio(void) { return exclusive; }
static bool meeting_or_monitoring_mode_blocks_sleep(void) { return mode; }
static bool nuwax_notification_blocks_conversation(void *p) { (void)p;return notification; }
static bool nuwax_ota_device_blocks_audio(void) { return ota; }
static bool nuwax_ota_device_is_active(void) { return ota; }
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws) { return ws->connected; }
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws) { return ws->epoch; }
static bool nuwax_music_status_reserve(nuwax_ws_client_t *ws, uint32_t epoch, const char *id) {
    (void)id;return ws->connected && ws->epoch == epoch;
}
static void nuwax_music_status_post(nuwax_ws_client_t *ws, uint32_t epoch, const char *id, const char *state, const char *reason) {
    (void)id;(void)state;assert(ws->connected && ws->epoch == epoch);
    terminals++;strlcpy(terminal_reason,reason,sizeof(terminal_reason));
}
static bool product_music_blocks_audio(void) { return atomic_load(&s_product_music_owned); }
static void nuwax_music_playback_cancel(const char *reason) { if(worker) strlcpy(cancelled_reason,reason,sizeof(cancelled_reason)); }
static bool nuwax_music_playback_active(void) { return worker; }
static bool nuwax_music_playback_audio_active(void) { return worker && worker_audio; }
static void update_normal_downlink_admission(bool open) { assert(!open); }
static void cancel_auto_followup_capture(const char *r) { (void)r; }
static void clear_awake_session(const char *r) { (void)r; }
static void clear_deferred_shell_listen_actions(const char *r) { (void)r; }
static bool invalidate_all_pending_semantic_emotions(const char *r) { (void)r;return true; }
static void nuwax_sleep_transaction_reset_for_wake(sleep_t *s) { assert(!s->active); }
static void clear_sleep_request(void) {}
static void inhibit_sleep_wake_word_detector(const char *r) { (void)r;wake_stops++; }
static void nuwax_wake_word_detector_request_stop(void) {}
static void xQueueReset(void *q) { (void)q; }
static void nuwax_speaker_playback_request_fade_cancel(const char *r,unsigned ms) { (void)r;assert(ms);fade_stops++; }
static bool conversation_capture_busy_for_sleep(void) { return capture; }
static bool nuwax_audio_uplink_sender_busy(void) { return uplink; }
static bool nuwax_wake_word_detector_is_running(void) { return wake; }
static bool nuwax_speaker_playback_has_pending(void) { return speaker; }
static esp_err_t nuwax_speaker_playback_deinit_output(void) {
    assert(!capture&&!uplink&&!wake&&!speaker);releases++;now_ms+=release_delay_ms;
    if(cancel_on_release) atomic_store(&s_product_music_cancel,true);
    return ESP_OK;
}
static bool nuwax_music_playback_start_test(const char *id,const char *url,const char *format) {
    (void)url;(void)format; assert(!worker && !capture && !uplink && !wake && !speaker);
    starts++;worker=true;worker_audio=true;strlcpy(started_id,id,sizeof(started_id));
    if(race_cancel) atomic_store(&s_product_music_cancel,true);
    return true;
}
static bool nuwax_music_playback_start(nuwax_ws_client_t *ws,const char *id,const char *url,const char *format) {
    assert(ws->connected);return nuwax_music_playback_start_test(id,url,format);
}
static void set_state(int state) { assert(state==NUWAX_APP_STATE_IDLE); }
static void nuwax_state_bridge_clear_semantic_emotion_if_owner(const char *s,unsigned r) { (void)r;assert(!strcmp(s,"music")); }
static bool commit_sleeping_conversation_state(const char *r) { (void)r;sleeps++;return true; }
static void request_sleep_wake_word_detector(const char *r) { (void)r;restores++; }
static void nuwax_music_playback_snapshot(nuwax_music_snapshot_t *s) {strcpy(s->state,"playing");strcpy(s->request_id,started_id);}
static bool nuwax_state_bridge_set_semantic_emotion_name_owned(const char *v,const char *s,unsigned r) {
    (void)r;assert(!strcmp(v,"happy")||!strcmp(v,"neutral"));assert(!strcmp(s,"music"));visuals++;return true;
}
#include "product_music.inc"
static void reset(void) {
    s_product_music_owned=s_product_music_cancel=false;
    s_music_pending.valid=false; s_music_worker_ws=NULL;
    s_power_shutdown_requested=power=provisioning=exclusive=mode=notification=ota=false;
    capture=uplink=wake=speaker=worker=worker_audio=race_cancel=false;
    s_sleep_transaction.active=s_deferred_sleep_intent.active=false;
    s_prelisten.capture_active=false;
    now_ms=100;starts=wake_stops=fade_stops=sleeps=restores=releases=terminals=visuals=0;
    release_delay_ms=0;cancel_on_release=false;
    started_id[0]=cancelled_reason[0]=terminal_reason[0]=0;
}
static bool request(nuwax_ws_client_t *ws,const char *id) {
    return product_music_request(ws,id,"https://example.test/song.mp3","mp3",false);
}
int main(void) {
    nuwax_ws_client_t ws={true,1};
    reset();capture=uplink=wake=speaker=true;
    assert(request(&ws,"one") && product_music_blocks_audio());
    assert(request(&ws,"one") && wake_stops==1);
    assert(wake_stops==1 && fade_stops==1);
    service_product_music(&ws); assert(!starts);
    capture=uplink=wake=speaker=false;
    service_product_music(&ws);assert(starts==1 && worker && releases==1);
    assert(request(&ws,"one") && !cancelled_reason[0]);
    assert(request(&ws,"two"));assert(!strcmp(cancelled_reason,"replaced"));
    assert(request(&ws,"three"));assert(terminals==1&&!strcmp(terminal_reason,"replaced"));
    service_product_music(&ws);assert(starts==1);
    worker=false;service_product_music(&ws);assert(starts==2&&!strcmp(started_id,"three"));
    worker=false;service_product_music(&ws);assert(!product_music_blocks_audio()&&sleeps==1&&restores==1);
    puts("PASS: capture/voice handoff, latest request wins, completion restores wake");

    bool *blockers[]={&s_power_shutdown_requested,&power,&provisioning,&exclusive,&mode,&notification,&ota,&s_sleep_transaction.active,&s_deferred_sleep_intent.active};
    for(unsigned i=0;i<sizeof(blockers)/sizeof(blockers[0]);i++) {
        reset();*blockers[i]=true;assert(!request(&ws,"blocked")&&!product_music_blocks_audio());
        *blockers[i]=false;assert(request(&ws,"playing"));service_product_music(&ws);assert(worker);
        *blockers[i]=true;service_product_music(&ws);assert(!strcmp(cancelled_reason,"higher_priority"));
        worker=false;service_product_music(&ws);assert(!product_music_blocks_audio()&&!restores);
    }
    puts("PASS: OTA, power, meeting/monitoring and provisioning preemption");

    reset();assert(request(&ws,"cancelled_pending"));s_product_music_cancel=true;
    service_product_music(&ws);assert(!starts&&!product_music_blocks_audio());
    reset();assert(request(&ws,"cancel_during_start"));race_cancel=true;
    service_product_music(&ws);assert(worker&&!strcmp(cancelled_reason,"preempted"));
    worker=false;service_product_music(&ws);assert(!product_music_blocks_audio());
    reset();assert(request(&ws,"stale_pending"));ws.epoch++;
    service_product_music(&ws);assert(!starts&&!product_music_blocks_audio());
    reset();assert(request(&ws,"retry_across_reconnect"));ws.epoch++;
    assert(request(&ws,"retry_across_reconnect"));
    service_product_music(&ws);assert(starts==1 && s_music_worker_epoch==ws.epoch);
    reset();assert(request(&ws,"disconnect"));service_product_music(&ws);ws.connected=false;
    service_product_music(&ws);assert(!strcmp(cancelled_reason,"connection_changed"));ws.connected=true;
    reset();wake=true;assert(request(&ws,"timeout"));now_ms+=8001;
    service_product_music(&ws);assert(!starts&&!product_music_blocks_audio()&&!strcmp(terminal_reason,"audio_release_timeout"));
    puts("PASS: cancellation races, stale epochs, disconnect and admission timeout");
    reset();assert(request(&ws,"slow_http_close"));service_product_music(&ws);
    worker_audio=false;service_product_music(&ws);
    assert(worker && !product_music_blocks_audio() && restores==1);
    assert(request(&ws,"replacement_after_close"));service_product_music(&ws);
    assert(starts==1 && s_music_pending.valid && product_music_blocks_audio());
    worker=false;service_product_music(&ws);
    assert(starts==2 && !strcmp(started_id,"replacement_after_close"));
    puts("PASS: restored audio resumes wake during slow HTTP cleanup; replacement remains serialized");
    reset();release_delay_ms=8001;assert(request(&ws,"release_crosses_deadline"));
    service_product_music(&ws);assert(!starts);
    service_product_music(&ws);assert(!product_music_blocks_audio()&&!strcmp(terminal_reason,"audio_release_timeout"));
    reset();cancel_on_release=true;assert(request(&ws,"cancel_inside_release"));
    service_product_music(&ws);assert(!starts);
    service_product_music(&ws);assert(!product_music_blocks_audio()&&!strcmp(terminal_reason,"preempted"));
    puts("PASS: cancellation and admission deadline rechecked after physical output release");
    reset();assert(request(&ws,"old_download"));service_product_music(&ws);
    worker_audio=false;service_product_music(&ws);
    assert(request(&ws,"next_during_cleanup"));now_ms+=8001;
    service_product_music(&ws);
    assert(worker && !s_music_pending.valid && !product_music_blocks_audio());
    assert(!strcmp(terminal_reason,"audio_release_timeout"));
    reset();assert(request(&ws,"restoring_clock"));service_product_music(&ws);
    assert(request(&ws,"next_during_restore"));now_ms+=8001;
    service_product_music(&ws);
    assert(!s_music_pending.valid && product_music_blocks_audio() && worker_audio);
    puts("PASS: replacement deadline covers old download; dirty audio remains fenced");
}
