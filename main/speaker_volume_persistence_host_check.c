#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "conversation/volume_save_pending.h"
static nuwax_volume_save_pending_t s_volume_save;
static uint64_t test_ms;
static int64_t esp_timer_get_time(void) { return (int64_t)test_ms * 1000; }
static bool speaker_locked;
static void speaker_lock(void) { assert(!speaker_locked); speaker_locked=true; }
static void speaker_unlock(void) { assert(speaker_locked); speaker_locked=false; }
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 2
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NUWAX_SETTINGS_NVS_NAMESPACE "nuwax"
#define NUWAX_SETTINGS_NVS_KEY_VOLUME "spk_vol"
#define TAG "test"
static const char *esp_err_to_name(int err) {(void)err;return "test";}
static void test_log(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGD(...) test_log(__VA_ARGS__)
#define ESP_LOGI(...) test_log(__VA_ARGS__)
static bool save_speaker_volume_percent(uint8_t value);
static uint8_t stored=4, staged=4;
static unsigned guards, ends, writes, commits, releases, handles, reads;
static bool guard_held, read_error, open_error, fail_commit, no_space, fail_guard, fail_release, concurrent;
static bool replace_pending;
static int nvs_open(const char *ns,int mode,nvs_handle_t *h) {
    assert(strcmp(ns,"nuwax")==0);
    if(open_error && mode==NVS_READONLY)return ESP_FAIL;
    if(mode==NVS_READWRITE)assert(guard_held);
    handles++;*h=mode+1;return ESP_OK;
}
static int nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *v) {
    assert(h==1 && strcmp(key,"spk_vol")==0);reads++;
    if(read_error)return ESP_FAIL;
    *v=staged;return ESP_OK;
}
static void nvs_close(nvs_handle_t h) {(void)h;assert(handles);handles--;}
static int nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v) {
    assert(h==2 && strcmp(key,"spk_vol")==0 && guard_held);writes++;
    if(no_space)return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    staged=v;return ESP_OK;
}
static int nvs_commit(nvs_handle_t h) {
    assert(h==2 && guard_held);commits++;
    if(fail_commit)return ESP_FAIL;
    stored=staged;return ESP_OK;
}
static int nuwax_wake_word_detector_begin_flash_operation(unsigned ms) {
    assert(ms==500 && !guard_held);guards++;
    assert(!speaker_locked);
    if (replace_pending) {
        replace_pending=false;
        nuwax_volume_save_request(&s_volume_save, 71, test_ms);
    }
    if(fail_guard)return ESP_FAIL;
    guard_held=true;
    if(concurrent) {
        concurrent=false;
        unsigned before=reads;
        save_speaker_volume_percent(4); // Must not observe a stale value during another save.
        assert(reads==before);
    }
    return ESP_OK;
}
static void nuwax_wake_word_detector_end_flash_operation(void) {
    assert(guard_held);guard_held=false;ends++;
}
static int nuwax_device_error_report_release_persisted_storage(void) {
    assert(!guard_held);releases++;
    if(fail_release)return ESP_FAIL;
    no_space=false;return ESP_OK;
}
#include "speaker_volume_persistence_host.inc"
int main(void) {
    save_speaker_volume_percent(4);
    assert(!guards && !writes && !handles);
    save_speaker_volume_percent(8); // Runtime may already be 8; persisted 4 must change.
    assert(stored==8 && guards==1 && commits==1 && ends==1);
    save_speaker_volume_percent(8);
    assert(guards==1);
    read_error=true;save_speaker_volume_percent(8);read_error=false;
    assert(guards==2 && commits==2);
    open_error=true;save_speaker_volume_percent(8);open_error=false;
    assert(guards==3 && commits==3);
    fail_commit=true;save_speaker_volume_percent(9);fail_commit=false;
    assert(staged==9 && stored==8);
    unsigned before=commits;
    save_speaker_volume_percent(9);
    assert(stored==9 && commits==before+1); // Read-back of failed write cannot skip retry.
    fail_guard=true;save_speaker_volume_percent(10);fail_guard=false;
    assert(stored==9 && !guard_held);
    save_speaker_volume_percent(10);assert(stored==10);
    no_space=true;save_speaker_volume_percent(11);
    assert(stored==11 && releases==1);
    no_space=true;fail_release=true;save_speaker_volume_percent(12);
    assert(stored==11 && releases==2);
    no_space=false;fail_release=false;save_speaker_volume_percent(12);
    assert(stored==12);
    concurrent=true;save_speaker_volume_percent(13);
    assert(stored==13 && !concurrent);
    save_speaker_volume_percent(4);assert(stored==4);
    before=guards;save_speaker_volume_percent(4);assert(guards==before);
    assert(guards==ends+1 && !guard_held && !handles);
    before=commits;
    for (unsigned i=0;i<11;i++) {
        test_ms=1000+i*100;
        nuwax_volume_save_request(&s_volume_save, 20+i, test_ms);
        assert(!nuwax_speaker_playback_service_volume_persistence(false));
    }
    assert(commits==before);
    test_ms=3499;assert(!nuwax_speaker_playback_service_volume_persistence(false));
    test_ms=3500;assert(nuwax_speaker_playback_service_volume_persistence(false));
    assert(stored==30 && commits==before+1);
    nuwax_volume_save_request(&s_volume_save, 40, test_ms);
    fail_guard=true;test_ms+=1500;
    assert(!nuwax_speaker_playback_service_volume_persistence(false));
    fail_guard=false;before=guards;test_ms+=4999;
    assert(!nuwax_speaker_playback_service_volume_persistence(false) && guards==before);
    test_ms++;assert(nuwax_speaker_playback_service_volume_persistence(false) && stored==40);
    nuwax_volume_save_request(&s_volume_save, 41, test_ms);
    fail_commit=true;
    assert(!nuwax_speaker_playback_service_volume_persistence(true));
    fail_commit=false;
    assert(nuwax_speaker_playback_service_volume_persistence(true) && stored==41);
    nuwax_volume_save_request(&s_volume_save, 70, test_ms);
    replace_pending=true;
    assert(!nuwax_speaker_playback_service_volume_persistence(true));
    assert(stored==70 && s_volume_save.pending && s_volume_save.value==71);
    test_ms+=1500;
    assert(nuwax_speaker_playback_service_volume_persistence(false) && stored==71);
    assert(!speaker_locked && !guard_held && !handles);
    puts("PASS: 11 requests -> one commit, debounce, failed save retained, backoff, shutdown flush, newer request wins");
    puts("speaker volume persistence production checks passed: dedup, runtime-only update, read/commit/guard failures, NVS recovery, concurrent save");
    return 0;
}
