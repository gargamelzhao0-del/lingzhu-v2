"""Execute the actual owner-side diagnostic gate with queued/audio workloads."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser()
parser.add_argument("--gcc", default="gcc")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
main = (root / "main/nuwax_ai_partner_main.c").read_text(encoding="utf-8-sig")
report = (root / "main/device_error_report.c").read_text(encoding="utf-8-sig")
send = function(report, "void nuwax_device_error_report_service(")
assert "persist_snapshot_if_dirty(" not in send, "Live delivery must not write Flash"
assert main.count("nuwax_device_error_report_housekeeping();") == 1, "All persistence must use the gate"
gate = function(main, "static bool diagnostic_persistence_idle(void)")
service = function(main, "static void service_idle_diagnostic_persistence(void)")

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
typedef enum { NUWAX_APP_STATE_IDLE, NUWAX_APP_STATE_SLEEPING,
               NUWAX_APP_STATE_SPEAKING, NUWAX_APP_STATE_LISTENING,
               NUWAX_APP_STATE_THINKING } nuwax_app_state_t;
enum { NUWAX_HEAD_TOUCH_AGENT_STATE_IDLE = 0 };
static nuwax_app_state_t state;
static struct { bool active; } s_awake_session, s_turn_guard,
    s_playback_terminal_guard, s_visual_round, s_notification_transaction,
    s_sleep_transaction, s_deferred_sleep_intent;
static struct { bool pending, capture_active; } s_prelisten;
static struct { int state; } s_head_touch_agent_intent;
static atomic_bool s_reprovision_quiesce_requested;
static atomic_uint s_ws_text_callbacks_active;
static bool capture, uplink, exclusive, power, replay;
#define nuwax_offline_audio_replay_running() replay
#if CONFIG_NUWAX_SPEAKER_ENABLE
static bool speaker;
static unsigned *s_speaker_owner_events;
#endif
#if CONFIG_NUWAX_OTA_ENABLE
static bool ota;
#endif
static unsigned *s_session_commands, *s_session_critical_commands,
    *s_session_notification_commands;
static int writes;
static bool ui_busy;
static unsigned volume_services;
#if CONFIG_NUWAX_SPEAKER_ENABLE
static bool command_during_volume, ui_during_volume;
#endif
static int64_t now_us;
static void nuwax_display_brightness_save(bool force) { assert(!force); }
static bool nuwax_lvgl_port_background_work_deferred(void) { return ui_busy; }
#if CONFIG_NUWAX_SPEAKER_ENABLE
static bool nuwax_speaker_playback_service_volume_persistence(bool force) {
    assert(!force); ++volume_services;
    if(command_during_volume) s_reprovision_quiesce_requested=true;
    if(ui_during_volume) ui_busy=true;
    return true;
}
#endif
static nuwax_app_state_t conversation_state_current(void) { return state; }
#define nuwax_awake_session_state_is_active(p) ((p)->active)
#define nuwax_conversation_visual_round_is_active(p) ((p)->active)
#define conversation_capture_busy_for_sleep() capture
#define nuwax_audio_uplink_sender_busy() uplink
#define exclusive_audio_mode_blocks_normal_audio() exclusive
#define local_power_transition_active() power
#define nuwax_speaker_playback_has_pending() speaker
#define nuwax_ota_device_is_active() ota
#define uxQueueMessagesWaiting(q) (*(q))
#define ESP_LOGW(tag,...) ((void)(tag), (void)printf(__VA_ARGS__))
#define TAG "test"
static int64_t esp_timer_get_time(void) { return now_us; }
/* Model the observed ten-second Flash stall, without sleeping the host. */
static void nuwax_device_error_report_housekeeping(void) {
    ++writes;
    now_us += 10000000;
}
'''
checks = r'''
static void assert_deferred(void) {
    int before = writes;
    unsigned volume_before = volume_services;
    int64_t start = now_us;
    assert(!diagnostic_persistence_idle());
    service_idle_diagnostic_persistence();
    assert(writes == before && now_us == start);
    assert(volume_services == volume_before);
}
#define BLOCK(flag) do { flag = true; assert_deferred(); flag = false; } while (0)
int main(void) {
    state = NUWAX_APP_STATE_SLEEPING;
    assert(diagnostic_persistence_idle());
    service_idle_diagnostic_persistence();
    assert(writes == 1);
    ui_busy=true;
    unsigned volume_before=volume_services;
    service_idle_diagnostic_persistence();
    assert(writes==1 && volume_services==volume_before);
    ui_busy=false;
    state = NUWAX_APP_STATE_IDLE;
    assert(diagnostic_persistence_idle());
    for (state = NUWAX_APP_STATE_SPEAKING; state <= NUWAX_APP_STATE_THINKING; ++state)
        assert_deferred();
    state = NUWAX_APP_STATE_SLEEPING;
    BLOCK(s_awake_session.active);
    BLOCK(s_turn_guard.active);
    BLOCK(s_playback_terminal_guard.active);
    BLOCK(s_visual_round.active);
    BLOCK(s_notification_transaction.active);
    BLOCK(s_sleep_transaction.active);
    BLOCK(s_deferred_sleep_intent.active);
    BLOCK(s_prelisten.pending);
    BLOCK(s_prelisten.capture_active);
    BLOCK(s_head_touch_agent_intent.state);
    BLOCK(replay);
    BLOCK(capture); BLOCK(uplink); BLOCK(exclusive); BLOCK(power);
    BLOCK(s_reprovision_quiesce_requested);
    BLOCK(s_ws_text_callbacks_active);
    unsigned queue = 1;
    s_session_commands = &queue; assert_deferred(); s_session_commands = NULL;
    s_session_critical_commands = &queue; assert_deferred(); s_session_critical_commands = NULL;
    s_session_notification_commands = &queue; assert_deferred(); s_session_notification_commands = NULL;
#if CONFIG_NUWAX_SPEAKER_ENABLE
    BLOCK(speaker);
    s_speaker_owner_events = &queue; assert_deferred(); s_speaker_owner_events = NULL;
#endif
#if CONFIG_NUWAX_OTA_ENABLE
    BLOCK(ota);
#endif
    /* The sound ended but logical END is still queued: no ten-second write. */
    s_playback_terminal_guard.active = true;
    s_session_critical_commands = &queue;
    assert_deferred();
    s_session_critical_commands = NULL;
    assert_deferred();
    s_playback_terminal_guard.active = false;
    /* Listening after completion must also remain responsive. */
    state = NUWAX_APP_STATE_LISTENING;
    assert_deferred();
    state = NUWAX_APP_STATE_SLEEPING;
    service_idle_diagnostic_persistence();
    assert(writes == 2);
#if CONFIG_NUWAX_SPEAKER_ENABLE
    assert(volume_services == 2);
    command_during_volume=true;
    service_idle_diagnostic_persistence();
    assert(writes==2 && volume_services==3);
    command_during_volume=false;s_reprovision_quiesce_requested=false;
    ui_during_volume=true;
    service_idle_diagnostic_persistence();
    assert(writes==2 && volume_services==4);
    ui_during_volume=false;ui_busy=false;
    service_idle_diagnostic_persistence();
    assert(writes==3 && volume_services==5);
#endif
    puts("PASS: playback/terminal/interaction/queues defer Flash; idle resumes persistence");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="nuwax-diagnostic-gate-") as directory:
    work = Path(directory)
    source = work / "check.c"
    source.write_text(harness + gate + "\n" + service + "\n" + checks, encoding="utf-8")
    for speaker in (0, 1):
        for ota in (0, 1):
            exe = work / f"check-{speaker}-{ota}.exe"
            subprocess.run([args.gcc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            f"-DCONFIG_NUWAX_SPEAKER_ENABLE={speaker}",
                            f"-DCONFIG_NUWAX_OTA_ENABLE={ota}", str(source), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
print("PASS: live report delivery is independent of diagnostic Flash persistence")
