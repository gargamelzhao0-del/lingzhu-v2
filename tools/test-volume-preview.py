"""Exercise production volume ordering functions and reject targeted regressions.

Only the mutex, output hardware and persistence boundary are stubbed. These
checks do not access hardware or certify real-time scheduling on the device.
"""
from pathlib import Path
import ctypes
import hashlib
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'main/conversation/speaker_playback.c'
source = SOURCE.read_text(encoding='utf-8-sig')
OUT = ROOT / 'build-host-checks/volume-preview'
OUT.mkdir(parents=True, exist_ok=True)
if os.name == 'nt':
    ctypes.windll.kernel32.SetErrorMode(0x8003)


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int portMUX_TYPE;
#define ESP_OK 0
#define pdTRUE 1
#define CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT 60
#define portMUX_INITIALIZER_UNLOCKED 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static bool locked, ui_mailbox_held;
static unsigned critical_depth;
#define taskENTER_CRITICAL(m) do { (void)(m); assert(!critical_depth); critical_depth++; } while(0)
#define taskEXIT_CRITICAL(m) do { (void)(m); assert(critical_depth==1); critical_depth--; } while(0)
static struct {
 void *mutex; uint8_t volume_percent;
 bool probe_duck_active,output_volume_sync_pending;
 uint32_t volume_generation; int output;
 struct { void (*on_volume_changed)(uint8_t); } callbacks;
} s_speaker_ctx;
static int s_volume_save;
static unsigned saves,syncs,clears,waits,locks,reports;
static uint8_t saved_volume,reported_volume,hardware_volume;
static bool inject_preview_during_sync;
static uint32_t injected_sequence;
uint32_t nuwax_speaker_playback_preview_volume_percent_ordered(int);
static void speaker_lock(void) { assert(!ui_mailbox_held && !critical_depth && !locked); locked=true; locks++; }
static void speaker_unlock(void) { assert(!ui_mailbox_held && !critical_depth && locked); locked=false; }
static int xSemaphoreTake(void *mutex,unsigned timeout) {
 (void)mutex; assert(!ui_mailbox_held && !critical_depth); if(timeout)waits++;
 if(locked)return 0;
 locked=true; return pdTRUE;
}
static uint32_t speaker_next_generation(uint32_t n) { return n==UINT32_MAX?1:n+1; }
static esp_err_t speaker_clear_probe_duck_locked(bool restore) {
 assert(!restore&&locked&&!critical_depth); clears++;
 s_speaker_ctx.probe_duck_active=false; return ESP_OK;
}
static esp_err_t speaker_sync_output_volume_locked(void) {
 assert(locked&&!critical_depth); syncs++;
 hardware_volume=s_speaker_ctx.volume_percent;
 s_speaker_ctx.output_volume_sync_pending=false;
 if(inject_preview_during_sync) {
   inject_preview_during_sync=false;
   injected_sequence=nuwax_speaker_playback_preview_volume_percent_ordered(88);
 }
 return ESP_OK;
}
static bool speaker_output_ready_locked(void) { return true; }
static esp_err_t nuwax_speaker_output_init(int *output) { (void)output; return ESP_OK; }
static uint64_t esp_timer_get_time(void) { return 1000000; }
static void nuwax_volume_save_request(int *save,uint8_t v,uint64_t ms) {
 (void)save;(void)ms;assert(locked&&!critical_depth);saves++;saved_volume=v;
}
static void changed(uint8_t v) { assert(!locked&&!critical_depth); reports++;reported_volume=v; }
'''
state_start = source.index('static portMUX_TYPE s_volume_order_mux')
state_end = source.index('\n};', state_start) + len('\n};')
signatures = [
    'static bool volume_sequence_after(',
    'static uint32_t volume_next_sequence_locked(',
    'static uint8_t clamp_speaker_volume_percent(',
    'static void speaker_consume_volume_preview_locked(',
    'static esp_err_t ensure_speaker_output_ready(',
    'void nuwax_speaker_playback_note_volume_commit_queued(',
    'bool nuwax_speaker_playback_apply_volume_commit_percent(',
    'void nuwax_speaker_playback_apply_volume_percent(',
    'static void speaker_try_consume_volume_preview(',
    'uint32_t nuwax_speaker_playback_preview_volume_percent_ordered(',
    'void nuwax_speaker_playback_preview_volume_percent(',
    'void nuwax_speaker_playback_cancel_volume_preview(',
    'uint8_t nuwax_speaker_playback_get_confirmed_volume_percent(',
    'void nuwax_speaker_playback_apply_current_volume(',
    'uint8_t nuwax_speaker_playback_get_volume_percent(',
]
production = source[state_start:state_end] + '\n' + '\n'.join(map(function, signatures))

CHECKS = r'''
static void reset(void) {
 memset(&s_volume_order,0,sizeof(s_volume_order));
 memset(&s_speaker_ctx,0,sizeof(s_speaker_ctx));
 locked=ui_mailbox_held=false;critical_depth=0;saves=syncs=clears=waits=locks=reports=0;
 s_speaker_ctx.mutex=(void*)1;s_speaker_ctx.volume_percent=60;
 s_speaker_ctx.volume_generation=1;s_speaker_ctx.callbacks.on_volume_changed=changed;
 s_volume_order.confirmed_volume=60;
 hardware_volume=saved_volume=reported_volume=60;
 inject_preview_during_sync=false;injected_sequence=0;
}
static void expect_volume(unsigned v) {
 assert(nuwax_speaker_playback_get_volume_percent()==v);
 assert(ensure_speaker_output_ready()==ESP_OK);
 assert(hardware_volume==v && waits==0 && !critical_depth && !locked);
}
int main(void) {
 uint32_t a,b,c,confirmed_sequence;
 /* UI callbacks are pure publications even when the speaker is idle. */
 reset();ui_mailbox_held=true;
 a=nuwax_speaker_playback_preview_volume_percent_ordered(70);
 nuwax_speaker_playback_note_volume_commit_queued(70,a);
 b=nuwax_speaker_playback_preview_volume_percent_ordered(90);
 nuwax_speaker_playback_cancel_volume_preview(b);
 ui_mailbox_held=false;
 assert(!locked && !locks && !syncs && !saves && !reports && s_speaker_ctx.volume_percent==60);
 assert(nuwax_speaker_playback_get_confirmed_volume_percent(&confirmed_sequence)==70 && confirmed_sequence==a);
 assert(!locks && !syncs && s_speaker_ctx.volume_percent==60);
 expect_volume(70);
 /* Busy playback owner cannot turn a touch preview into blocking I/O. */
 reset();locked=true;
 a=nuwax_speaker_playback_preview_volume_percent_ordered(99);
 assert(a && locked && s_speaker_ctx.volume_percent==60 && !waits && !syncs && !saves);
 b=nuwax_speaker_playback_preview_volume_percent_ordered(10);
 assert(b!=a);c=nuwax_speaker_playback_preview_volume_percent_ordered(100);
 nuwax_speaker_playback_note_volume_commit_queued(100,c);
 assert(locked && !syncs && !saves && !waits);
 locked=false;expect_volume(100);assert(c && !saves && !reports);
 assert(nuwax_speaker_playback_apply_volume_commit_percent(100,true,c));
 assert(saves==1 && saved_volume==100 && reports==1);

 /* Original witness: pending80 -> authoritative cloud20 -> getter/PCM. */
 reset();locked=true;a=nuwax_speaker_playback_preview_volume_percent_ordered(80);locked=false;
 nuwax_speaker_playback_apply_volume_percent(20,true);expect_volume(20);
 assert(!s_volume_order.pending && saves==1 && saved_volume==20 && reported_volume==20);
 assert(!nuwax_speaker_playback_apply_volume_commit_percent(80,true,a));expect_volume(20);
 assert(saves==1 && reports==1);
 nuwax_speaker_playback_cancel_volume_preview(a);expect_volume(20);

 /* Older queued70 may persist while newer90 preview is pending OR consumed. */
 for(int consumed=0;consumed<2;consumed++) {
   reset();a=nuwax_speaker_playback_preview_volume_percent_ordered(70);
   nuwax_speaker_playback_note_volume_commit_queued(70,a);
   b=nuwax_speaker_playback_preview_volume_percent_ordered(90);
   if(consumed)expect_volume(90);
   assert(nuwax_speaker_playback_apply_volume_commit_percent(70,true,a));
   expect_volume(90);assert(saves==1 && saved_volume==70 && reported_volume==70);
   nuwax_speaker_playback_note_volume_commit_queued(90,b);
   assert(nuwax_speaker_playback_apply_volume_commit_percent(90,true,b));
   expect_volume(90);assert(saves==2 && saved_volume==90 && reported_volume==90);
   assert(!nuwax_speaker_playback_apply_volume_commit_percent(70,true,a));
   assert(!nuwax_speaker_playback_apply_volume_commit_percent(90,true,b));
   assert(saves==2 && reports==2);
 }

 /* Accepted70 unprocessed; cancel90 restores70 despite stale UI, then70 persists. */
 for(int consumed=0;consumed<2;consumed++) {
   reset();a=nuwax_speaker_playback_preview_volume_percent_ordered(70);
   nuwax_speaker_playback_note_volume_commit_queued(70,a);
   b=nuwax_speaker_playback_preview_volume_percent_ordered(90);
   if(consumed)expect_volume(90);
   nuwax_speaker_playback_cancel_volume_preview(b);
   expect_volume(70);assert(!saves);
   assert(nuwax_speaker_playback_apply_volume_commit_percent(70,true,a));
   expect_volume(70);assert(saves==1 && saved_volume==70);
 }

 /* Cancel may neither roll back a newer gesture nor a cloud update. */
 reset();a=nuwax_speaker_playback_preview_volume_percent_ordered(70);
 b=nuwax_speaker_playback_preview_volume_percent_ordered(90);
 nuwax_speaker_playback_cancel_volume_preview(a);expect_volume(90);
 nuwax_speaker_playback_apply_volume_percent(20,true);
 nuwax_speaker_playback_cancel_volume_preview(b);expect_volume(20);
 assert(!nuwax_speaker_playback_apply_volume_commit_percent(90,true,b));expect_volume(20);
 assert(saves==1 && reported_volume==20);

 /* A cancellation may publish or consume the old rollback before the
  * renderer resumes from a successful business queue send. */
 for(int consumed=0;consumed<2;consumed++) {
   reset();a=nuwax_speaker_playback_preview_volume_percent_ordered(70);
   b=nuwax_speaker_playback_preview_volume_percent_ordered(90);
   expect_volume(90);
   nuwax_speaker_playback_cancel_volume_preview(b);
   if(consumed)expect_volume(60);
   nuwax_speaker_playback_note_volume_commit_queued(70,a);
   expect_volume(70);assert(!saves && !reports);
   assert(nuwax_speaker_playback_apply_volume_commit_percent(70,true,a));
   expect_volume(70);assert(saves==1 && reported_volume==70);
 }

 /* Even cloud BEFORE the next gesture is authoritative when the UI lags. */
 reset();nuwax_speaker_playback_apply_volume_percent(20,true);
 b=nuwax_speaker_playback_preview_volume_percent_ordered(90);
 nuwax_speaker_playback_cancel_volume_preview(b);expect_volume(20);

 /* A contended rollback cannot outlive a newer authoritative commit. */
 reset();a=nuwax_speaker_playback_preview_volume_percent_ordered(90);
 locked=true;nuwax_speaker_playback_cancel_volume_preview(a);locked=false;
 nuwax_speaker_playback_apply_volume_percent(20,true);expect_volume(20);

 /* New preview arriving AFTER cloud linearization during hardware I/O wins. */
 reset();inject_preview_during_sync=true;
 nuwax_speaker_playback_apply_volume_percent(20,true);
 assert(injected_sequence && s_volume_order.pending && saved_volume==20);
 expect_volume(88);
 assert(nuwax_speaker_playback_apply_volume_commit_percent(88,true,injected_sequence));
 expect_volume(88);assert(saved_volume==88 && saves==2);

 /* Queue notification may arrive after processing; it cannot revert newer cloud. */
 reset();a=nuwax_speaker_playback_preview_volume_percent_ordered(70);
 assert(nuwax_speaker_playback_apply_volume_commit_percent(70,true,a));
 nuwax_speaker_playback_note_volume_commit_queued(70,a);
 nuwax_speaker_playback_apply_volume_percent(20,true);
 nuwax_speaker_playback_note_volume_commit_queued(70,a);
 b=nuwax_speaker_playback_preview_volume_percent_ordered(90);
 nuwax_speaker_playback_cancel_volume_preview(b);expect_volume(20);

 /* Two accepted local releases coalesce without letting an older queued one dip. */
 reset();a=nuwax_speaker_playback_preview_volume_percent_ordered(70);
 nuwax_speaker_playback_note_volume_commit_queued(70,a);
 b=nuwax_speaker_playback_preview_volume_percent_ordered(90);
 nuwax_speaker_playback_note_volume_commit_queued(90,b);
 c=nuwax_speaker_playback_preview_volume_percent_ordered(95);
 nuwax_speaker_playback_cancel_volume_preview(c);expect_volume(90);
 assert(!nuwax_speaker_playback_apply_volume_commit_percent(70,true,a));
 assert(nuwax_speaker_playback_apply_volume_commit_percent(90,true,b));
 expect_volume(90);assert(saved_volume==90 && saves==1);

 /* Reconnect resync cannot invalidate the eventual release of a live drag. */
 reset();a=nuwax_speaker_playback_preview_volume_percent_ordered(76);
 assert(nuwax_speaker_playback_get_confirmed_volume_percent(&confirmed_sequence)==60 && !confirmed_sequence);
 nuwax_speaker_playback_apply_current_volume();expect_volume(76);
 assert(!saves && s_volume_order.committed_sequence==0 && reported_volume==60);
 assert(nuwax_speaker_playback_apply_volume_commit_percent(76,true,a));assert(saves==1);

 /* Equal values are distinct operations; value equality is not identity. */
 reset();a=nuwax_speaker_playback_preview_volume_percent_ordered(70);
 b=nuwax_speaker_playback_preview_volume_percent_ordered(70);assert(a!=b);
 assert(nuwax_speaker_playback_apply_volume_commit_percent(70,true,a));
 nuwax_speaker_playback_cancel_volume_preview(b);expect_volume(70);
 assert(!s_volume_order.preview_active);

 /* 32-bit operation clock skips zero and orders work correctly across wrap. */
 reset();s_volume_order.sequence=UINT32_MAX-1;
 a=nuwax_speaker_playback_preview_volume_percent_ordered(70);assert(a==UINT32_MAX);
 b=nuwax_speaker_playback_preview_volume_percent_ordered(90);assert(b==1);
 assert(nuwax_speaker_playback_apply_volume_commit_percent(70,true,a));expect_volume(90);
 assert(nuwax_speaker_playback_apply_volume_commit_percent(90,true,b));expect_volume(90);
 assert(!nuwax_speaker_playback_apply_volume_commit_percent(70,true,a));expect_volume(90);
 assert(saves==2);

 /* Zero-token compatibility is authoritative; valid previews never return zero. */
 reset();nuwax_speaker_playback_preview_volume_percent(80);
 assert(nuwax_speaker_playback_apply_volume_commit_percent(-20,true,0));expect_volume(0);
 for(int v=-20;v<=120;v++) {
   locked=true;a=nuwax_speaker_playback_preview_volume_percent_ordered(v);locked=false;
   assert(a);expect_volume(v<0?0:v>100?100:(unsigned)v);
 }
 assert(saves==1 && waits==0);
 puts("PASS: real volume APIs preserve ordering, nonblocking preview, cancellation, resync, persistence and wrap");
 return 0;
}
'''


def run_case(name, body, should_pass=True):
    c_path = OUT / (name + '.c')
    exe = c_path.with_suffix('.exe')
    c_path.write_text(PRE + body + CHECKS, encoding='utf-8')
    build = subprocess.run(
        ['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
         '-Wno-unused-but-set-variable', str(c_path), '-o', str(exe)],
        capture_output=True, text=True, timeout=30)
    (OUT / (name + '.compile.log')).write_text(build.stdout + build.stderr, encoding='utf-8')
    assert build.returncode == 0, build.stderr
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    (OUT / (name + '.run.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
    assert (result.returncode == 0) == should_pass, name + ': ' + result.stdout + result.stderr
    print(('PASS' if should_pass else 'NEGATIVE CONTROL REJECTED') + ': ' + name)
    return {'name': name, 'passed': should_pass, 'exitCode': result.returncode}


def mutant(old, new):
    assert production.count(old) == 1, old
    return production.replace(old, new)


results = [run_case('ordered_preview', production)]
results.append(run_case('mutant_pending_survives_cloud', mutant(
    's_volume_order.pending = false;\n    }\n    taskEXIT_CRITICAL',
    '/* stale pending retained */\n    }\n    taskEXIT_CRITICAL'), False))
results.append(run_case('mutant_consumed_preview_lost', mutant(
    'const bool preserve_newer_preview = s_volume_order.preview_active &&',
    'const bool preserve_newer_preview = s_volume_order.pending &&'), False))
results.append(run_case('mutant_late_commit_allowed', mutant(
    'if (!volume_sequence_after(commit_sequence, s_volume_order.committed_sequence) ||\n        volume_sequence_after(s_volume_order.confirmed_sequence, commit_sequence)) {',
    'if (false) {'), False))
results.append(run_case('mutant_stale_cancel_allowed', mutant(
    's_volume_order.preview_sequence != preview_sequence ||', 'false ||'), False))
results.append(run_case('mutant_cancel_retires_queued_commit', mutant(
    's_volume_order.preview_active = false;\n    s_volume_order.pending_volume = s_volume_order.confirmed_volume;',
    's_volume_order.preview_active = false;\n    s_volume_order.committed_sequence = preview_sequence;\n    s_volume_order.pending_volume = s_volume_order.confirmed_volume;'), False))
results.append(run_case('mutant_stale_ui_restore', mutant(
    's_volume_order.pending_volume = s_volume_order.confirmed_volume;',
    's_volume_order.pending_volume = CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT;'), False))
results.append(run_case('mutant_resync_publishes_preview', mutant(
    'const uint8_t volume = nuwax_speaker_playback_get_confirmed_volume_percent(NULL);',
    'const uint8_t volume = s_speaker_ctx.volume_percent;'), False))
results.append(run_case('mutant_publish_takes_speaker_lock', mutant(
    'taskEXIT_CRITICAL(&s_volume_order_mux);\n    return sequence;',
    'taskEXIT_CRITICAL(&s_volume_order_mux);\n    speaker_try_consume_volume_preview();\n    return sequence;'), False))
results.append(run_case('mutant_late_note_keeps_old_restore', mutant(
    'if (!s_volume_order.preview_active) {\n            s_volume_order.pending_volume = next;\n            s_volume_order.pending = true;\n        }',
    '/* Cancelled target remains stale. */'), False))
(OUT / 'report.json').write_text(json.dumps({
    'sourceSha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
    'results': results, 'hardwareValidated': False,
}, indent=2), encoding='utf-8')
