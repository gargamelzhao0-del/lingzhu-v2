"""Exercise the music owner with real voice release and board lease functions.

RTOS and physical codec/I2S are simulated. The idle keep-initialized path,
forced output release, reference accounting, and RX/TX acquisition are taken
from production, so the previously missed retained-TX conflict is observable.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build-host-checks/music-handoff'
OUT.mkdir(parents=True, exist_ok=True)
core = OUT / 'lifecycle.c'
subprocess.run([sys.executable, str(ROOT / 'tools/audit-audio-teardown.py'),
                '--emit-core', str(core)], check=True)
base = core.read_text(encoding='utf-8')
playback = (ROOT / 'main/conversation/speaker_playback.c').read_text(encoding='utf-8-sig')
owner = (ROOT / 'main/music_playback/product_music.inc').read_text(encoding='utf-8-sig')
owner_fixture = (ROOT / 'main/music_playback/music_owner_host_check.c').read_text(encoding='utf-8-sig')

def function(source, signature):
    start = source.index(signature)
    while ';' in source[start:source.index('{', start)]:
        start = source.index(signature, start+len(signature))
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

context = '''static struct {
 nuwax_speaker_output_t output;
 bool keep_initialized, playback_active, playback_session_active;
 bool start_gate_pending, start_gate_authorized, runtime_reset_in_progress;
 uint32_t io_claim_generation, completion_claim_generation, maintenance_claim_generation;
 uint32_t cancel_pending_generation, output_stop_owner_generation, maintenance_claim_sequence;
} s_speaker_ctx;'''
old_context = 'static struct { nuwax_speaker_output_t output; } s_speaker_ctx;'
assert base.count(old_context) == 1
base = base.replace(old_context, context)
support = r'''
static void speaker_lock(void) {}
static void speaker_unlock(void) {}
static unsigned speaker_packet_ring_count(void) { return 0; }
static uint32_t speaker_next_generation(uint32_t n) { return n+1U ? n+1U : 1U; }
static nuwax_audio_board_ref_t active_music_ref;
static nuwax_audio_board_lease_t active_music_lease;
static void *active_music_tx;
'''
release_code = '\n'.join(function(playback, signature) for signature in [
    'static bool speaker_output_absent_locked(',
    'static uint32_t speaker_claim_idle_output_action(',
    'static void speaker_release_idle_output_action(',
    'static esp_err_t speaker_mute_idle_output(',
    'void nuwax_speaker_playback_release_idle_output(',
    'esp_err_t nuwax_speaker_playback_deinit_output('])

# Reuse non-audio product services from the owner tests, replacing its release
# and start mocks with production release and real RX/TX lease acquisition.
prefix = owner_fixture[:owner_fixture.index('#include "product_music.inc"')]
prefix = prefix.replace(function(prefix, 'static esp_err_t nuwax_speaker_playback_deinit_output('), '')
prefix = prefix.replace(function(prefix, 'static bool nuwax_music_playback_start_test('), r'''
static bool nuwax_music_playback_start_test(const char *id,const char *url,const char *format) {
 (void)url;(void)format;
 assert(!worker && !capture && !uplink && !wake && !speaker);
 assert(nuwax_audio_board_init(&active_music_ref)==ESP_OK);
 if(nuwax_audio_board_acquire_music(&active_music_tx,&active_music_lease)!=ESP_OK) {
   assert(nuwax_audio_board_deinit(&active_music_ref)==ESP_OK);
   return false;
 }
 assert(nuwax_audio_board_music_rate(&active_music_lease,44100)==ESP_OK);
 starts++;worker=worker_audio=true;strlcpy(started_id,id,sizeof(started_id));
 return true;
}
''')
reset = function(owner_fixture, 'static void reset(')
request = function(owner_fixture, 'static bool request(')
tests = r'''
static void finish_music(nuwax_ws_client_t *ws) {
 assert(worker && active_music_lease.token);
 assert(nuwax_audio_board_music_rate(&active_music_lease,16000)==ESP_OK);
 assert(nuwax_audio_board_release(&active_music_lease)==ESP_OK);
 assert(nuwax_audio_board_deinit(&active_music_ref)==ESP_OK);
 active_music_tx=NULL;worker=worker_audio=false;service_product_music(ws);
 assert(!product_music_blocks_audio() && !s_init_refcount);
}
static void voice_ready(void) {
 s_speaker_ctx.keep_initialized=true;
 assert(nuwax_speaker_output_init(&s_speaker_ctx.output)==ESP_OK);
 assert(speaker_output_ready_locked() && s_speaker_ctx.output.board_lease.token);
}
int main(void) {
 nuwax_ws_client_t ws={true,1};
 reset();voice_ready();
 const uint32_t old_tx=s_speaker_ctx.output.board_lease.token;
 nuwax_speaker_playback_release_idle_output("ordinary_idle");
 assert(s_speaker_ctx.output.board_lease.token==old_tx && speaker_output_ready_locked());
 assert(nuwax_audio_board_init(&active_music_ref)==ESP_OK);
 assert(nuwax_audio_board_acquire_music(&active_music_tx,&active_music_lease)==ESP_ERR_INVALID_STATE);
 assert(nuwax_audio_board_deinit(&active_music_ref)==ESP_OK);
 puts("PASS: reproduced old failure: keep-initialized voice retains TX and blocks music");
 for(unsigned cycle=0;cycle<100;cycle++) {
   reset();assert(request(&ws,"handoff"));service_product_music(&ws);
   assert(starts==1 && worker && speaker_output_absent_locked());
   assert(s_rx_lease.token==active_music_lease.token && s_tx_lease.token==active_music_lease.token);
   assert(s_speaker_ctx.keep_initialized && !s_speaker_ctx.maintenance_claim_generation);
   finish_music(&ws);voice_ready();
 }
 assert(nuwax_speaker_playback_deinit_output()==ESP_OK);
 puts("PASS: 100 voice->music->voice cycles; exact ownership and no reference leaks");

 reset();voice_ready();fail_output_lock=true;
 assert(request(&ws,"lock_busy"));service_product_music(&ws);
 assert(!starts && s_music_pending.valid && s_music_pending.output_release_attempts==1);
 service_product_music(&ws);assert(s_music_pending.output_release_attempts==1);
 assert(s_speaker_ctx.output.board_lease.token && !s_speaker_ctx.maintenance_claim_generation);
 fail_output_lock=false;now_ms+=250;service_product_music(&ws);
 assert(starts==1);finish_music(&ws);
 puts("PASS: real output-lock failure waits with bounded retry; no early music start");

 reset();voice_ready();s_speaker_ctx.io_claim_generation=99;
 assert(request(&ws,"io_busy"));service_product_music(&ws);
 assert(!starts && s_music_pending.valid && s_speaker_ctx.output.board_lease.token);
 s_speaker_ctx.io_claim_generation=0;now_ms+=250;service_product_music(&ws);
 assert(starts==1);finish_music(&ws);
 puts("PASS: in-flight voice I/O refuses deinit until the real maintenance claim succeeds");

 reset();voice_ready();fail_output_lock=true;
 assert(request(&ws,"permanent_busy"));
 for(unsigned i=0;i<=32;i++){service_product_music(&ws);now_ms+=250;}
 assert(!starts && !s_music_pending.valid && !product_music_blocks_audio());
 assert(!strcmp(terminal_reason,"audio_release_timeout"));
 assert(s_speaker_ctx.output.board_lease.token);
 fail_output_lock=false;assert(nuwax_speaker_playback_deinit_output()==ESP_OK);
 puts("PASS: persistent release failure expires without taking another owner's TX");

 reset();voice_ready();fail_output_lock=true;
 assert(request(&ws,"cancel_waiting"));service_product_music(&ws);
 s_product_music_cancel=true;service_product_music(&ws);
 assert(!starts && !product_music_blocks_audio() && !s_music_pending.valid);
 fail_output_lock=false;assert(nuwax_speaker_playback_deinit_output()==ESP_OK);
 assert(!s_init_refcount && !s_rx_lease.token && !s_tx_lease.token);
 puts("PASS: cancel while handoff waits leaves no pending music or maintenance owner");
}
'''

def run(name, owner_source, negative=False):
    source = '\n'.join([base, support, release_code, prefix, owner_source, reset, request, tests])
    cfile, exe = OUT/f'{name}.c', OUT/f'{name}.exe'
    cfile.write_text(source, encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-O1','-Wall','-Wextra','-Wno-unused-function',
                    '-Wno-unused-variable','-Wno-misleading-indentation',str(cfile),'-o',str(exe)],
                   check=True,timeout=60,capture_output=True)
    result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
    if negative:
        assert result.returncode != 0, f'Negative mutant escaped: {name}'
        print(f'PASS: {name} detected at runtime')
    else:
        assert result.returncode == 0, result.stdout+result.stderr
        print(result.stdout,end='')
    return dict(name=name,negative=negative,exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr)

results=[run('production',owner)]
results.append(run('old_idle_release',owner.replace(
    'const esp_err_t release_err = nuwax_speaker_playback_deinit_output();',
    'nuwax_speaker_playback_release_idle_output("music_start"); const esp_err_t release_err = ESP_OK;'),True))
results.append(run('ignore_release_error',owner.replace(
    'if (release_err == ESP_OK && !atomic_load', 'if (!atomic_load'),True))
paths=['main/music_playback/product_music.inc','main/conversation/speaker_playback.c',
       'main/speaker_output.c','main/board/audio_board.c']
(OUT/'report.json').write_text(json.dumps(dict(status='PASS',hardwareValidated=False,results=results,
    sourceHashes={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in paths}),indent=2),encoding='utf-8')
