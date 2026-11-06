#define main previous_exit_controls
#include "audio_mode_exit_host_check.c"
#undef main
typedef struct { bool connected; } nuwax_ws_client_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FINISHED 0x106
static bool s_power_shutdown_requested, mode_takeover;
static void app_task_watchdog_checkpoint(void) {}
static void service_power_key_events(nuwax_ws_client_t *ws) { (void)ws; }
static bool service_music_url_test(bool arm) { (void)arm;return false; }
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws) { return ws->connected; }
static bool exclusive_audio_mode_blocks_normal_audio(void) { return mode_takeover; }
static void complete_during_playback_wake_stop(void) {}
/* Simulate a queued completion releasing the final old audio owner. */
static void drain_session_commands(nuwax_ws_client_t *ws) { (void)ws;speaker=false; }
#include "mode_idle_prefix_host.inc"
int main(void)
{
    nuwax_ws_client_t ws={true};
    conversation_state=1;s_exclusive_audio_sleep_reconcile_pending=true;speaker=true;
    service_exclusive_audio_sleep_reconcile();assert(!commits&&s_exclusive_audio_sleep_reconcile_pending);
    assert(mode_idle_prefix(&ws)==ESP_ERR_NOT_FINISHED);
    assert(commits==1U&&conversation_state==NUWAX_APP_STATE_SLEEPING&&!s_exclusive_audio_sleep_reconcile_pending);
    conversation_state=1;assert(mode_idle_prefix(&ws)==ESP_ERR_NOT_FINISHED);assert(commits==1U);
    s_exclusive_audio_sleep_reconcile_pending=true;mode_takeover=true;
    assert(mode_idle_prefix(&ws)==ESP_ERR_INVALID_STATE&&commits==1U);mode_takeover=false;
    s_notification_transaction=1;assert(mode_idle_prefix(&ws)==ESP_ERR_NOT_FINISHED&&commits==1U);
    s_notification_transaction=0;assert(mode_idle_prefix(&ws)==ESP_ERR_NOT_FINISHED&&commits==2U);
    puts("PASS real idle-wait prefix: completion drains owner and consumes deferred mode exit before waiting for wake; replacement/notification owners retain priority");
    return 0;
}
