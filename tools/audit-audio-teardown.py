"""W05 lifecycle regression against extracted production functions.

Exit 1 means an ownership invariant failed; compiler errors are not findings.
The board/I2S/mutex boundary is simulated, so this is not a hardware test.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build-host-checks/w05-fix/teardown"
OUT.mkdir(parents=True, exist_ok=True)


def function(source, signature):
    start = source.index(signature)
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


paths = ["main/board/audio_board.c", "main/speaker_output.c",
         "main/speaker_output.h", "main/conversation/speaker_playback.c",
         "main/board/audio_board.h", "main/mic_input.h", "main/mic_input.c"]
sources = {p: (ROOT / p).read_text(encoding="utf-8-sig") for p in paths}
board = sources[paths[0]]
speaker = sources[paths[1]]
header = sources[paths[2]]
playback = sources[paths[3]]
struct_start = header.index("typedef struct {")
struct_end = header.index("} nuwax_speaker_output_t;") + len("} nuwax_speaker_output_t;")
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef int esp_err_t;
typedef void *SemaphoreHandle_t;
typedef void *i2s_chan_handle_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_TIMEOUT -4
#define ESP_ERR_NO_MEM -5
#define ESP_ERR_NOT_SUPPORTED -6
#define ESP_ERROR_CHECK_WITHOUT_ABORT(expr) ((void)(expr))
#define ESP_RETURN_ON_FALSE(expr,code,...) do { if(!(expr)) return code; } while(0)
#define CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0 1
#define CONFIG_NUWAX_VOCAT_MIC3_REFERENCE_PRODUCTION_PROVEN 1
#define CONFIG_NUWAX_SPEAKER_SD_GPIO -1
#define CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT 60
#define NUWAX_SPEAKER_OUTPUT_LOCK_TIMEOUT_MS 100
#define portMAX_DELAY 0xffffffffU
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int rc=(expr); if(rc) return rc; } while(0)
static bool fail_teardown, fail_init, fail_output_lock, tx_live, rx_live;
static bool s_music_rate_dirty, fail_music_rate;
#define VOCAT_AUDIO_SAMPLE_RATE_HZ 16000U
static int nuwax_vocat_audio_board_set_music_rate(unsigned rate) { (void)rate;return fail_music_rate ? ESP_FAIL : ESP_OK; }
static unsigned board_init_calls, board_deinit_calls, heap_frees, lock_frees;
static bool partial_teardown;
static int handles[8], mutex_object;
static int ensure_lock(SemaphoreHandle_t *lock) { *lock=&mutex_object; return ESP_OK; }
static int xSemaphoreTake(void *lock, unsigned timeout) { (void)lock;(void)timeout;return 1; }
static void xSemaphoreGive(void *lock) { (void)lock; }
static void vSemaphoreDelete(void *lock) { (void)lock;lock_frees++; }
static void *xSemaphoreCreateMutex(void) { return &mutex_object; }
static bool speaker_output_lock(void *lock, unsigned timeout) { (void)lock;(void)timeout;return !fail_output_lock; }
static void heap_caps_free(void *ptr) { if(ptr) heap_frees++; }
static int i2s_channel_disable(void *ptr) { (void)ptr;return ESP_OK; }
static int i2s_del_channel(void *ptr) { (void)ptr;return ESP_OK; }
static int nuwax_vocat_audio_board_init(void) {
 board_init_calls++;if(fail_init)return ESP_FAIL;tx_live=rx_live=true;return ESP_OK;
}
static int nuwax_vocat_audio_board_deinit(void) {
 board_deinit_calls++;if(partial_teardown)tx_live=false;
 if(fail_teardown)return ESP_FAIL;tx_live=rx_live=false;return ESP_OK;
}
static uint8_t clamp_volume_percent(uint8_t v) { return v; }
static void *nuwax_vocat_audio_board_get_tx_handle(void) { return tx_live ? &handles[0] : NULL; }
static void *nuwax_vocat_audio_board_get_rx_handle(void) { return rx_live ? &handles[1] : NULL; }
static void *xTaskGetCurrentTaskHandle(void) { return &handles[2]; }
static void vTaskDelay(unsigned ms) { (void)ms; }
static void esp_restart(void) { assert(!"unexpected controlled restart"); }
#define pdMS_TO_TICKS(ms) (ms)
'''
support = r'''
static int init_p3_software_ref_if_enabled(nuwax_speaker_output_t *speaker) {
    speaker->software_ref_required=false;return ESP_OK;
}
/* Hardware sequencing is covered by test-speaker-pa-sequence.py. This suite
 * exercises the production board reference/lease teardown independently. */
static int nuwax_speaker_output_stop(nuwax_speaker_output_t *speaker) {
    speaker->muted=true;speaker->stop_settled=true;return ESP_OK;
}
static struct { nuwax_speaker_output_t output; } s_speaker_ctx;
esp_err_t nuwax_speaker_output_deinit(nuwax_speaker_output_t *speaker);
'''
tests = r"""
int main(void) {
    nuwax_speaker_output_t *out=&s_speaker_ctx.output;
    assert(nuwax_audio_board_init(NULL)==ESP_ERR_INVALID_ARG);
    assert(nuwax_audio_board_deinit(NULL)==ESP_ERR_INVALID_ARG);
    assert(nuwax_speaker_output_init(out)==ESP_OK);
    unsigned before=board_init_calls;
    assert(nuwax_speaker_output_init(out)==ESP_OK && board_init_calls==before);
    assert(speaker_output_ready_locked() && out->board_lease.token);
    fail_output_lock=true;
    assert(nuwax_speaker_output_deinit(out)==ESP_ERR_TIMEOUT);
    assert(out->initialized && out->board_ref.token && out->board_lease.token);
    fail_output_lock=false;
    assert(nuwax_speaker_output_deinit(out)==ESP_OK && s_init_refcount==0);
    puts("normal_idempotent_init_and_lock_timeout=PASS");

    for(unsigned partial=0;partial<2;partial++) {
        partial_teardown=partial;
        assert(nuwax_speaker_output_init(out)==ESP_OK);
        nuwax_audio_board_ref_t stale=out->board_ref;
        out->stereo_buffer=(void*)&handles[7];out->stereo_capacity=1;
        unsigned frees=heap_frees,locks=lock_frees;
        fail_teardown=true;
        assert(nuwax_speaker_output_deinit(out)==ESP_FAIL);
        assert(out->board_lease.token==0 && out->board_ref.token==0 && s_teardown_pending);
        assert(!speaker_output_ready_locked() && !out->tx_handle && !out->lock);
        assert(s_init_refcount==0 && heap_frees==frees+1 && lock_frees==locks+1);
        before=board_init_calls;
        assert(nuwax_speaker_output_init(out)==ESP_FAIL && board_init_calls==before);
        assert(!speaker_output_ready_locked());
        fail_teardown=false;
        nuwax_audio_board_ref_t a={0},b={0};
        assert(nuwax_audio_board_init(&a)==ESP_OK);
        assert(nuwax_audio_board_init(&b)==ESP_OK && s_init_refcount==2);
        assert(nuwax_speaker_output_deinit(out)==ESP_OK && s_init_refcount==2);
        assert(nuwax_audio_board_deinit(&stale)==ESP_ERR_INVALID_STATE && s_init_refcount==2);
        before=board_deinit_calls;
        assert(nuwax_audio_board_retry_teardown()==ESP_OK && board_deinit_calls==before);
        assert(nuwax_audio_board_deinit(&a)==ESP_OK && !a.token && s_init_refcount==1);
        assert(nuwax_audio_board_deinit(&a)==ESP_OK && s_init_refcount==1);
        assert(nuwax_audio_board_deinit(&b)==ESP_OK && !b.token && s_init_refcount==0);
    }
    puts("teardown_failure_before_and_after_TX_delete_cross_generation=PASS");

    assert(nuwax_speaker_output_init(out)==ESP_OK);
    nuwax_audio_board_ref_t rx_ref={0};nuwax_audio_board_lease_t rx_lease={0};void *rx=NULL;
    assert(nuwax_audio_board_init(&rx_ref)==ESP_OK);
    assert(nuwax_audio_board_acquire_rx(&rx,&rx_lease)==ESP_OK);
    nuwax_audio_board_lease_t saved_lease=rx_lease;
    void *saved_rx=rx;
    assert(nuwax_audio_board_acquire_rx(&rx,&rx_lease)==ESP_ERR_INVALID_STATE);
    assert(!memcmp(&rx_lease,&saved_lease,sizeof(rx_lease)) && rx==saved_rx);
    assert(nuwax_audio_board_acquire_tx(&rx,&rx_lease)==ESP_ERR_INVALID_STATE);
    assert(!memcmp(&rx_lease,&saved_lease,sizeof(rx_lease)) && rx==saved_rx);
    puts("duplicate_same_and_cross_channel_acquire_preserves_owner=PASS");
    assert(nuwax_speaker_output_deinit(out)==ESP_OK && s_init_refcount==1 && rx_live);
    assert(nuwax_audio_board_deinit(&rx_ref)==ESP_ERR_INVALID_STATE && rx_ref.token);
    assert(nuwax_audio_board_release(&rx_lease)==ESP_OK);
    assert(nuwax_audio_board_deinit(&rx_ref)==ESP_OK);
    puts("shared_RX_TX_last_reference_lease_guard=PASS");

    nuwax_audio_board_ref_t music_ref={0};nuwax_audio_board_lease_t music_lease={0};void *music_tx=NULL;
    assert(nuwax_audio_board_init(&music_ref)==ESP_OK);
    assert(nuwax_audio_board_acquire_music(&music_tx,&music_lease)==ESP_OK);
    assert(nuwax_audio_board_music_rate(&music_lease,44100)==ESP_OK && s_music_rate_dirty);
    assert(nuwax_audio_board_release(&music_lease)==ESP_ERR_INVALID_STATE);
    nuwax_audio_board_lease_t blocked_rx={0};void *blocked_handle=NULL;
    assert(nuwax_audio_board_acquire_rx(&blocked_handle,&blocked_rx)==ESP_ERR_INVALID_STATE);
    fail_music_rate=true;
    assert(nuwax_audio_board_music_rate(&music_lease,16000)==ESP_FAIL && s_music_rate_dirty);
    assert(nuwax_audio_board_release(&music_lease)==ESP_ERR_INVALID_STATE);
    fail_music_rate=false;
    assert(nuwax_audio_board_music_rate(&music_lease,16000)==ESP_OK && !s_music_rate_dirty);
    assert(nuwax_audio_board_release(&music_lease)==ESP_OK);
    assert(nuwax_audio_board_deinit(&music_ref)==ESP_OK);
    puts("music_shared_clock_lease_blocks_mic_until_successful_voice_restore=PASS");

    assert(nuwax_speaker_output_init(out)==ESP_OK);
    uint32_t token=out->board_lease.token;out->board_lease.token++;
    assert(nuwax_speaker_output_deinit(out)==ESP_ERR_INVALID_STATE);
    assert(!speaker_output_ready_locked() && !out->initialized && out->board_ref.token);
    out->board_lease.token=token;
    assert(nuwax_speaker_output_deinit(out)==ESP_OK);
    puts("lease_release_rejection_retains_cleanup_but_blocks_output=PASS");

    nuwax_audio_board_ref_t refs[AUDIO_BOARD_REF_CAPACITY+1]={0};
    for(unsigned i=0;i<AUDIO_BOARD_REF_CAPACITY;i++) assert(nuwax_audio_board_init(&refs[i])==ESP_OK);
    before=board_init_calls;
    assert(nuwax_audio_board_init(&refs[AUDIO_BOARD_REF_CAPACITY])==ESP_ERR_NO_MEM);
    assert(board_init_calls==before && !refs[AUDIO_BOARD_REF_CAPACITY].token);
    nuwax_audio_board_ref_t duplicate=refs[0];
    assert(nuwax_audio_board_init(&refs[0])==ESP_ERR_INVALID_STATE);
    assert(nuwax_audio_board_deinit(&refs[0])==ESP_OK);
    assert(nuwax_audio_board_deinit(&duplicate)==ESP_ERR_INVALID_STATE);
    assert(s_init_refcount==AUDIO_BOARD_REF_CAPACITY-1);
    for(unsigned i=1;i<AUDIO_BOARD_REF_CAPACITY;i++)assert(nuwax_audio_board_deinit(&refs[i])==ESP_OK);
    fail_init=true;
    assert(nuwax_audio_board_init(&refs[0])==ESP_FAIL && !refs[0].token && s_init_refcount==0);
    fail_init=false;
    puts("capacity_duplicate_same_generation_and_init_failure=PASS");

    nuwax_mic_input_t mic={0};
    assert(nuwax_audio_board_init(&mic.board_ref)==ESP_OK);
    assert(nuwax_audio_board_acquire_rx(&mic.rx_handle,&mic.board_lease)==ESP_OK);
    mic.vocat_shared_rx=mic.initialized=mic.owner_acquired=true;
    mic.raw_samples=&handles[6];
    fail_teardown=true;
    assert(nuwax_mic_input_deinit(&mic)==ESP_FAIL);
    assert(!mic.vocat_shared_rx && !mic.owner_acquired && !mic.board_ref.token && !mic.rx_handle);
    fail_teardown=false;
    assert(nuwax_audio_board_init(&refs[0])==ESP_OK);
    assert(nuwax_mic_input_deinit(&mic)==ESP_OK && s_init_refcount==1);
    assert(nuwax_audio_board_deinit(&refs[0])==ESP_OK);
    puts("mic_failed_teardown_then_new_owner_and_old_cleanup=PASS");
    s_next_ref_token=0;
    assert(nuwax_audio_board_init(&refs[0])==ESP_OK && refs[0].token==1);
    s_next_ref_token=UINT32_MAX;
    assert(nuwax_audio_board_init(&refs[1])==ESP_OK && refs[1].token==2);
    assert(nuwax_audio_board_deinit(&refs[0])==ESP_OK);
    assert(nuwax_audio_board_deinit(&refs[1])==ESP_OK);
    puts("reference_token_wrap_skips_live_token=PASS");
    return 0;
}
"""
board_header=sources[paths[4]]
types=board_header[board_header.index('typedef enum {\n    NUWAX_AUDIO_BOARD_LEASE_NONE'):board_header.index('/* Zero-initialize refs;')]
board_state=board[board.index('static int s_init_refcount;'):board.index('static TaskHandle_t s_diagnostic_owner;')]
mic_header=sources[paths[5]]
mic_types=mic_header[mic_header.index('typedef struct {'):mic_header.index('esp_err_t nuwax_mic_input_init(')]
mic_stubs=r"""
static void mic_dma_cache_reset(nuwax_mic_input_t *mic) { mic->rx_cached_offset=mic->rx_cached_bytes=0; }
static void release_owner(nuwax_mic_input_t *mic) { mic->owner_acquired=false; }
"""
parts = [prefix,types,board_state,'static void *s_diagnostic_owner;\nstatic const struct { bool shared_i2s; } s_caps={true};',
         header[struct_start:struct_end],mic_types,mic_stubs,support]
for signature in ["static void clear_teardown_state_locked(void)",
                  "static esp_err_t retry_teardown_locked(void)",
                  "esp_err_t nuwax_audio_board_retry_teardown(void)",
                  "static uint32_t next_ref_token_locked(void)",
                  "esp_err_t nuwax_audio_board_init(",
                  "esp_err_t nuwax_audio_board_deinit(",
                  "static uint32_t next_token(void)",
                  "static bool lease_matches(","static esp_err_t acquire_impl(",
                  "esp_err_t nuwax_audio_board_acquire_rx(",
                  "esp_err_t nuwax_audio_board_acquire_tx(",
                  "esp_err_t nuwax_audio_board_acquire_music(",
                  "esp_err_t nuwax_audio_board_music_rate(",
                  "esp_err_t nuwax_audio_board_release("]:
    parts.append(function(board, signature))
parts.extend([function(speaker, "esp_err_t nuwax_speaker_output_init("),
              function(speaker, "esp_err_t nuwax_speaker_output_deinit("),
              function(playback, "static bool speaker_output_ready_locked(void)"),
              function(sources[paths[6]], "esp_err_t nuwax_mic_input_deinit("),tests])
def run_variant(name, text):
    source_file = OUT / (name + ".c")
    source_file.write_text(text, encoding="utf-8")
    exe = OUT / (name + ".exe")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-misleading-indentation", str(source_file), "-o", str(exe)],
                   check=True, timeout=60)
    return subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)


production = "\n".join(parts)
if len(sys.argv) == 3 and sys.argv[1] == '--emit-core':
    # Share the production lifecycle bodies and hardware-boundary stubs with
    # the music handoff integration test, without replacing lease ownership.
    Path(sys.argv[2]).write_text('\n'.join(parts[:-1]), encoding='utf-8')
    raise SystemExit(0)
result = run_variant("teardown", production)
mutants = []
if result.returncode == 0:
    variants = [
        ("overwrite_live_lease", "out_lease->token == 0U", "true"),
        ("empty_ref_consumes_new_owner", "if (ref->token == 0) {",
         "if (ref->token == 0) { if (s_init_refcount > 0) s_init_refcount--;"),
        ("caller_ref_not_consumed", "memset(ref, 0, sizeof(*ref));", "/* lost consume */"),
        ("failed_output_still_initialized", "speaker->initialized = false;",
         "/* lost invalidation */"),
        ("pending_cleanup_not_recorded", "s_teardown_pending = true;",
         "s_teardown_pending = false;"),
    ]
    for name, old, new in variants:
        assert production.count(old) == 1, f"Mutation target drifted: {name}"
        broken = run_variant(name, production.replace(old, new, 1))
        mutants.append({"name": name, "rejected": broken.returncode != 0,
                        "exit_code": broken.returncode, "stderr": broken.stderr})
        assert broken.returncode != 0, f"Broken lifecycle escaped test: {name}"
report = {
    "kind": "production-function fault injection; simulated hardware boundary",
    "status": "INVARIANT_FAILURE" if result.returncode else "PASS",
    "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr,
    "negative_mutants": mutants,
    "source_sha256": {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in paths},
}
(OUT / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(result.stdout, end="")
if mutants:
    print(f"Rejected {len(mutants)} compiled negative mutants")
print(f"Report: {OUT / 'report.json'}")
raise SystemExit(result.returncode)
