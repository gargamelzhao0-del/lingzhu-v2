#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NOT_FINISHED 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_SIZE 4
#define pdPASS 1
#define ESP_LOGI(...) ((void)0)
typedef struct { bool connected; uint32_t epoch; } nuwax_ws_client_t;
static struct { atomic_bool media_recovery_requested; } s;
static atomic_bool s_shutdown_requested, s_replay_retry_required;
static atomic_bool s_formal_replay_running, s_formal_replay_cancel_requested;
static atomic_bool s_media_ready;
static struct { nuwax_ws_client_t *ws; uint32_t epoch; } s_formal_replay_context;
static uint32_t s_replay_launch_epoch, s_replay_launch_deadline_ms;
static bool shutdown, busy, initialized, feature_enabled, ready, background;
static bool ota_reserved, ota_race;
static bool replay_ota_active(void) { return ota_reserved; }
static uint32_t now_ms;
static unsigned created;
static int create_result;
static bool nuwax_power_ctrl_shutdown_preparing(void) { return shutdown; }
static bool nuwax_offline_audio_storage_busy(void) { return busy || ota_reserved; }
static bool enabled(void) { return feature_enabled; }
static bool runtime_initialized(void) { return initialized; }
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws) { return ws->connected; }
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws) { return ws->epoch; }
static bool nuwax_offline_audio_has_ready(void) { return ready; }
static bool nuwax_audio_uplink_background_ready(void) { if (ota_race) ota_reserved=true; return background; }
static uint32_t offline_now_ms(void) { return now_ms; }
static bool offline_deadline_elapsed(uint32_t now, uint32_t due) { return (int32_t)(now-due)>=0; }
static int create_worker(void) { ++created; return create_result; }
#define xTaskCreateWithCaps(...) create_worker()
static const char *test_url, *test_uid;
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL test_url
static const char *nuwax_device_uid_string(void) { return test_uid; }

/* Both functions are extracted verbatim from the production spool. */
#include "offline_formal_start_host.inc"

static void reset(void) {
    atomic_store(&s.media_recovery_requested, false);
    atomic_store(&s_shutdown_requested, false);
    atomic_store(&s_replay_retry_required, false);
    atomic_store(&s_formal_replay_running, false);
    atomic_store(&s_formal_replay_cancel_requested, false);
    atomic_store(&s_media_ready, true);
    s_replay_launch_epoch=0; s_replay_launch_deadline_ms=0;
    shutdown=busy=false;
    ota_reserved=ota_race=false;
    initialized=feature_enabled=ready=background=true;
    now_ms=100; created=0; create_result=pdPASS;
}

int main(void) {
    nuwax_ws_client_t ws={true,7};
    reset();
    /* No hello response or capability publication precedes either launch. */
    assert(replay_formal_start_locked(&ws,7)==ESP_OK && created==1);
    assert(atomic_load(&s_formal_replay_running));
    assert(s_formal_replay_context.ws==&ws && s_formal_replay_context.epoch==7);
    assert(replay_formal_start_locked(&ws,7)==ESP_OK && created==1);
    atomic_store(&s_formal_replay_running,false);
    assert(replay_formal_start_locked(&ws,7)==ESP_ERR_NOT_FINISHED && created==1);
    ws.epoch=8;
    assert(replay_formal_start_locked(&ws,8)==ESP_OK && created==2);

    reset(); ota_reserved=true;
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_NOT_FINISHED && created==0);
    assert(!atomic_load(&s_formal_replay_running));
    reset(); ota_race=true; // OTA reserved between the first check and worker publication.
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_NOT_FINISHED && created==0);
    assert(!atomic_load(&s_formal_replay_running));
    ota_reserved=ota_race=false; now_ms+=5000;
    assert(replay_formal_start_locked(&ws,8)==ESP_OK && created==1);
    reset();
    assert(replay_formal_start_locked(&ws,7)==ESP_ERR_INVALID_STATE);
    assert(replay_formal_start_locked(&ws,0)==ESP_ERR_INVALID_STATE);
    assert(replay_formal_start_locked(NULL,8)==ESP_ERR_INVALID_STATE);
    ws.connected=false;
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_INVALID_STATE && created==0);
    ws.connected=true;
    initialized=false;
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_INVALID_STATE);
    initialized=true; feature_enabled=false;
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_INVALID_STATE);
    feature_enabled=true; ready=false;
    assert(replay_formal_start_locked(&ws,8)==ESP_OK && created==0);
    ready=true; busy=true;
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_NOT_FINISHED);
    assert(atomic_load(&s_replay_retry_required) && created==0);
    busy=false; atomic_store(&s.media_recovery_requested,true);
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_NOT_FINISHED);
    atomic_store(&s.media_recovery_requested,false);
    atomic_store(&s_media_ready,false);
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_NOT_FINISHED);
    atomic_store(&s_media_ready,true); background=false;
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_NOT_FINISHED);
    background=true; shutdown=true;
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_INVALID_STATE);
    shutdown=false; atomic_store(&s_shutdown_requested,true);
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_INVALID_STATE && created==0);
    reset(); create_result=0;
    assert(replay_formal_start_locked(&ws,8)==ESP_ERR_NO_MEM);
    assert(!atomic_load(&s_formal_replay_running));
    create_result=pdPASS; now_ms+=5000;
    assert(replay_formal_start_locked(&ws,8)==ESP_OK && created==2);

    char url[384];
    test_uid="NX-AABB-CCDD-EEFF";
    test_url="ws://127.0.0.1:8810/api/device/ws";
    assert(replay_test_build_url(url,sizeof(url))==ESP_OK);
    assert(strcmp(url,"ws://127.0.0.1:8810/api/device/ws?device_id=NX-AABB-CCDD-EEFF")==0);
    test_url="ws://127.0.0.1:8810/api/device/ws?route=test";
    assert(replay_test_build_url(url,sizeof(url))==ESP_OK);
    assert(strstr(url,"?route=test&device_id=NX-AABB-CCDD-EEFF"));
    assert(replay_test_build_url(url,8)==ESP_ERR_INVALID_SIZE);
    test_uid="";
    assert(replay_test_build_url(url,sizeof(url))==ESP_ERR_INVALID_STATE);
    puts("PASS: SD starts without hello on first/reconnected epoch; recording, media, shutdown, memory and identity guards retained");
    return 0;
}
