/* Exercise the production mode-end sleep transaction with controlled owners. */
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 1
#define CONFIG_NUWAX_SPEAKER_ENABLE 1
#define NUWAX_MEETING_STATE_DISABLED 0
#define NUWAX_MONITORING_STATE_DISABLED 0
#define NUWAX_APP_STATE_SLEEPING 0
#define NUWAX_HEAD_TOUCH_AGENT_STATE_IDLE 0
#define ESP_LOGI(...) ((void)0)
typedef struct { int state; bool pending, claim; } mode_t;
static mode_t s_meeting_mode, s_monitoring_mode;
static bool s_exclusive_audio_sleep_reconcile_pending;
static atomic_bool s_reprovision_quiesce_requested;
static struct { bool capture_active; } s_prelisten;
static struct { bool active; } s_turn_guard;
static struct { int state; } s_head_touch_agent_intent;
static int s_notification_transaction, conversation_state = 1;
static unsigned wake_queue, shell_queue, commits;
static unsigned *s_wake_word_events = &wake_queue, *s_shell_actions = &shell_queue;
static bool lease, capture, conversation_capture, speaker, admit = true;
static int nuwax_meeting_mode_state(const mode_t *m) { return m->state; }
static int nuwax_monitoring_mode_state(const mode_t *m) { return m->state; }
static bool nuwax_meeting_mode_has_pending(const mode_t *m) { return m->pending; }
static bool nuwax_monitoring_mode_has_pending(const mode_t *m) { return m->pending; }
static bool nuwax_meeting_mode_has_audio_claim(const mode_t *m) { return m->claim; }
static bool nuwax_monitoring_mode_has_audio_claim(const mode_t *m) { return m->claim; }
static bool offline_mode_lease_pending(void) { return lease; }
static bool nuwax_offline_audio_capture_running(void) { return capture; }
static bool conversation_capture_busy_for_sleep(void) { return conversation_capture; }
static bool nuwax_notification_blocks_conversation(const int *n) { return *n != 0; }
static bool nuwax_speaker_playback_has_pending(void) { return speaker; }
static int conversation_state_current(void) { return conversation_state; }
static unsigned uxQueueMessagesWaiting(const unsigned *q) { return *q; }
static bool commit_sleeping_conversation_state(const char *reason)
{
    assert(!strcmp(reason, "exclusive_audio_released"));
    ++commits;
    if (admit) conversation_state = NUWAX_APP_STATE_SLEEPING;
    return admit;
}
#include "audio_mode_exit_host.inc"

int main(void)
{
    for (unsigned monitor = 0; monitor < 2; ++monitor) {
        mode_t *mode = monitor ? &s_monitoring_mode : &s_meeting_mode;
        for (unsigned guard = 0; guard < 12; ++guard) {
            const unsigned before = commits;
            conversation_state = 1;
            s_exclusive_audio_sleep_reconcile_pending = true;
            mode->state = 2; mode->claim = true;
            service_exclusive_audio_sleep_reconcile();
            assert(commits == before);
            mode->state = NUWAX_MEETING_STATE_DISABLED; mode->claim = false;
            switch (guard) {
                case 0: mode->pending = true; break;
                case 1: mode->claim = true; break;
                case 2: lease = true; break;
                case 3: capture = true; break;
                case 4: conversation_capture = true; break;
                case 5: s_prelisten.capture_active = true; break;
                case 6: s_turn_guard.active = true; break;
                case 7: s_notification_transaction = 1; break;
                case 8: speaker = true; break;
                case 9: wake_queue = 1; break;
                case 10: shell_queue = 1; break;
                case 11: s_head_touch_agent_intent.state = 1; break;
            }
            service_exclusive_audio_sleep_reconcile();
            assert(commits == before && s_exclusive_audio_sleep_reconcile_pending);
            mode->pending = mode->claim = lease = capture = conversation_capture = false;
            s_prelisten.capture_active = s_turn_guard.active = speaker = false;
            s_notification_transaction = s_head_touch_agent_intent.state = 0;
            wake_queue = shell_queue = 0;
            atomic_store(&s_reprovision_quiesce_requested, true);
            service_exclusive_audio_sleep_reconcile(); assert(commits == before);
            atomic_store(&s_reprovision_quiesce_requested, false);
            /* An owner race retries; a successful exit consumes the latch and
             * cannot later force sleep over a newly started conversation. */
            admit = false; service_exclusive_audio_sleep_reconcile();
            assert(commits == before + 1 && s_exclusive_audio_sleep_reconcile_pending);
            admit = true; service_exclusive_audio_sleep_reconcile();
            assert(commits == before + 2 && conversation_state == NUWAX_APP_STATE_SLEEPING);
            assert(!s_exclusive_audio_sleep_reconcile_pending);
            conversation_state = 1; service_exclusive_audio_sleep_reconcile();
            assert(commits == before + 2 && conversation_state == 1);
        }
    }
    puts("PASS: production meeting/monitoring exit restores sleep, waits for capture owners, preserves new interactions and retries owner races");
    return 0;
}
