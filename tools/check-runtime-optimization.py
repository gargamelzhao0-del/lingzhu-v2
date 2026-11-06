"""Execute changed production C functions with deterministic host dependencies.

These checks inject allocation failure and pause/owner interleavings. They do
not simulate ESP-IDF scheduling, flash durability or target timing.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function(path, signature):
    source = (ROOT / path).read_text(encoding="utf-8-sig")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[{}]', re.S)
    for token in tokens.finditer(source, brace):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:token.end()]
    raise ValueError(signature)


BASE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <setjmp.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NOT_FINISHED 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_ARG 4
#define ESP_ERR_NOT_SUPPORTED 5
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
'''


def run(gcc, name, source):
    with tempfile.TemporaryDirectory(prefix="nuwax-opt-") as temp:
        path = pathlib.Path(temp)
        cfile, exe = path / "test.c", path / "test.exe"
        cfile.write_text(BASE + source, encoding="utf-8")
        subprocess.run([gcc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-variable", "-Wno-unused-function",
                        str(cfile), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print(f"PASS {name}", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gcc", default="gcc")
    args = parser.parse_args()
    app = "main/nuwax_ai_partner_main.c"
    player = "main/nuwax_ui_v28/face/emotion_animation_player.c"
    restore = function(app, "static esp_err_t restore_audio_after_runtime_provisioning(void)")
    for aec in (0, 1):
        run(args.gcc, f"AEC={aec}: guard allocation, failure and collision", f'''
#define CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC 1
#define CONFIG_NUWAX_LIVE_AEC_ENABLE {aec}
#define CONFIG_NUWAX_WAKE_WORD_ENABLE 0
#define NUWAX_LIVE_AEC_PREP_MIN_INTERNAL_LARGEST_BYTES 15360
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static atomic_bool s_conversation_live_aec_post_reprovision_restart;
static atomic_bool s_conversation_live_aec_degraded;
static void *s_conversation_live_aec_post_reprovision_guard;
static int allocations, frees;
static bool fail_alloc;
static unsigned char storage[15360], existing;
static void *heap_caps_malloc(size_t size, int caps) {{
    (void)caps; assert(size == sizeof(storage)); allocations++;
    return fail_alloc ? NULL : storage;
}}
static void heap_caps_free(void *p) {{ assert(p == storage); frees++; }}
''' + restore + r'''
int main(void) {
    assert(restore_audio_after_runtime_provisioning() == ESP_OK);
    assert(allocations == CONFIG_NUWAX_LIVE_AEC_ENABLE);
    assert((s_conversation_live_aec_post_reprovision_guard != NULL) ==
           CONFIG_NUWAX_LIVE_AEC_ENABLE);
    assert(!s_conversation_live_aec_degraded);
    s_conversation_live_aec_post_reprovision_guard = NULL;
    fail_alloc = true;
    assert(restore_audio_after_runtime_provisioning() ==
           (CONFIG_NUWAX_LIVE_AEC_ENABLE ? ESP_ERR_NO_MEM : ESP_OK));
    assert(s_conversation_live_aec_degraded == CONFIG_NUWAX_LIVE_AEC_ENABLE);
    fail_alloc = false;
    s_conversation_live_aec_post_reprovision_guard = &existing;
    assert(restore_audio_after_runtime_provisioning() ==
           (CONFIG_NUWAX_LIVE_AEC_ENABLE ? ESP_ERR_INVALID_STATE : ESP_OK));
    assert(s_conversation_live_aec_post_reprovision_guard == &existing);
    assert(frees == CONFIG_NUWAX_LIVE_AEC_ENABLE);
}
''')
    run(args.gcc, "discard volatile rollback without changing durable record", r'''
static bool s_reprovision_record_durable, force;
static struct { unsigned char bytes[64]; } s_reprovision_record;
static atomic_int s_cancel_state;
#define PROVISION_CANCEL_DISABLED 0
static void set_force_reprovision(bool enabled) { force = enabled; }
''' + function("main/nuwax_provision_new/nuwax_provision_main.c",
               "void nuwax_provision_main_discard_volatile_rollback(void)") + r'''
int main(void) {
    memset(&s_reprovision_record, 0xa5, sizeof(s_reprovision_record));
    force = true; s_cancel_state = 1;
    s_reprovision_record_durable = true;
    nuwax_provision_main_discard_volatile_rollback();
    assert(force && s_cancel_state == 1);
    assert(s_reprovision_record.bytes[0] == 0xa5);
    s_reprovision_record_durable = false;
    nuwax_provision_main_discard_volatile_rollback();
    assert(!force && s_cancel_state == 0);
    for (unsigned i=0; i<sizeof(s_reprovision_record); i++)
        assert(s_reprovision_record.bytes[i] == 0);
    nuwax_provision_main_discard_volatile_rollback();
    assert(!force);
}
''')
    run(args.gcc, "decode pause/resume preserves notifications, errors and frame order", r'''
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define FRAME_RGB_BYTES 2
#define NUWAX_EMOTION_ASSET_PACK_ABI_V2 2
#define DECODE_COOPERATIVE_DELAY_THRESHOLD_US 25000
enum { DECODE_WORKER_IDLE, DECODE_WORKER_REQUESTED, DECODE_WORKER_RUNNING,
       DECODE_WORKER_READY, DECODE_WORKER_FAILED };
enum { DECODE_SLOT_FREE, DECODE_SLOT_FILLING, DECODE_SLOT_READY };
enum { BRIDGE_DECODE_IDLE, BRIDGE_DECODE_REQUESTED, BRIDGE_DECODE_RUNNING,
       BRIDGE_DECODE_READY, BRIDGE_DECODE_FAILED };
typedef struct { atomic_int state; uint32_t generation, sequence, decode_us; }
    decode_frame_slot_t;
typedef struct { int asset_id, segment_index, frame_index; uint32_t generation;
    void *source_rgb565, *visible_rgb565, *target_indices, *target_rgb565;
    uint32_t decode_us; esp_err_t result; } bridge_decode_job_t;
static bridge_decode_job_t s_bridge_decode_job;
static atomic_int s_bridge_decode_state, s_decode_worker_state;
static atomic_bool s_bridge_source_ready, s_decode_stop_requested,
    s_decode_task_exited, s_decode_paused;
static atomic_uint s_bridge_decode_generation, s_decode_generation,
    s_queued_asset_id, s_health_decode_cooperative_blocks,
    s_health_decode_switch_aborts;
static uint32_t s_next_decode_sequence, s_pending_decode_us;
static esp_err_t s_decode_worker_error;
static struct { int frame_index; const struct segment_type { int frame_count; } *segment; }
    s_decoder;
static const struct segment_type actual_segment = {10};
static bool s_ready = true, s_interaction_active;
static bool s_background_decode = true;
static void *s_image = (void*)1, *s_rgb565 = (void*)1, *s_decode_task = (void*)1;
static int64_t s_next_frame_us;
static struct { bool active; int64_t pause_started_us, started_us, next_frame_us; }
    s_component_bridge;
static decode_frame_slot_t slot;
static jmp_buf stopped;
static int step, prepares, notifications;
static int64_t esp_timer_get_time(void) { return 1000; }
static void xTaskNotifyGive(void *p) { assert(p); notifications++; }
static void vTaskDelay(int n) { (void)n; }
static void taskYIELD(void) {}
static void vTaskSuspend(void *p) { (void)p; longjmp(stopped, 1); }
static int nuwax_emotion_asset_pack_abi(void) { return 2; }
static void release_decode_slot(decode_frame_slot_t *p) { p->state = DECODE_SLOT_FREE; }
static decode_frame_slot_t *find_free_decode_slot(void) {
    if(slot.state != DECODE_SLOT_FREE) return NULL;
    slot.state = DECODE_SLOT_FILLING; return &slot;
}
static esp_err_t decode_v4_handoff_frame(int a,int b,int c,void *d,void *e) {
    (void)a;(void)b;(void)c;(void)d;(void)e; abort();
}
static esp_err_t prepare_next_frame(decode_frame_slot_t *p) {
    prepares++; assert(p->sequence == (unsigned)prepares);
    s_decoder.frame_index++;
    if(prepares == 1) s_decode_paused = true;
    else s_decode_stop_requested = true;
    return ESP_OK;
}
esp_err_t nuwax_emotion_animation_player_freeze(void);
void nuwax_emotion_animation_player_set_interaction_active(bool active);
static unsigned ulTaskNotifyTake(int clear, unsigned timeout) {
    (void)clear;(void)timeout;
    switch(++step) {
    case 1: s_decode_paused=true; s_decode_worker_state=DECODE_WORKER_FAILED; break;
    case 2:
        assert(s_decode_worker_state == DECODE_WORKER_FAILED);
        s_decode_worker_state=DECODE_WORKER_REQUESTED; break;
    case 3:
        assert(s_decode_worker_state == DECODE_WORKER_REQUESTED);
        s_interaction_active=true;
        nuwax_emotion_animation_player_set_interaction_active(false);
        assert(notifications == 1); break;
    case 4:
        assert(prepares==1 && slot.state==DECODE_SLOT_READY);
        assert(s_decode_worker_state==DECODE_WORKER_READY); break;
    case 5:
        assert(s_decode_worker_state==DECODE_WORKER_READY);
        s_interaction_active=true;
        nuwax_emotion_animation_player_set_interaction_active(false);
        release_decode_slot(&slot); s_decode_worker_state=DECODE_WORKER_REQUESTED; break;
    default: abort();
    }
    return 1;
}
''' + function(player, "static void decode_worker_task(void *argument)") +
        function(player, "void nuwax_emotion_animation_player_set_interaction_active(bool active)") +
        function(player, "esp_err_t nuwax_emotion_animation_player_freeze(void)") + r'''
int main(void) {
    s_decoder.segment=&actual_segment; s_decode_generation=1;
    s_next_decode_sequence=1;
    assert(nuwax_emotion_animation_player_freeze()==ESP_OK);
    assert(s_decode_paused && s_interaction_active);
    if(!setjmp(stopped)) decode_worker_task(NULL);
    assert(prepares==2 && s_decoder.frame_index==2 && s_decode_task_exited);
}
''')


if __name__ == "__main__":
    main()
