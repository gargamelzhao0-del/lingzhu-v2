"""Production scheduler: one metadata-faulted lane must not stall its peer."""
from pathlib import Path
import subprocess
import tempfile
from c_source_extract import function

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-metadata-lanes'
out.mkdir(parents=True, exist_ok=True)
replay = (root / 'main/sd_audio_session_replay.inc').read_text(encoding='utf-8-sig')
(out / 'sd_file_fault_production_host.inc').write_text(replay[replay.index('static esp_err_t card_confirm_missing_locked('):replay.index('/* Signature retained')], encoding='utf-8')
(out / 'sd_scheduler_host.inc').write_text(replay[replay.index('static esp_err_t card_replay_next_impl('):replay.rindex('#endif')], encoding='utf-8')
fixture = (root / 'main/sd_audio_recovery_host_check.c').read_text(encoding='utf-8-sig')
fixture = fixture[:fixture.index('int main(void)')]
fixture = fixture.replace(function(fixture, 'replay_one_ready'), r'''
static esp_err_t replay_one_ready(nuwax_ws_client_t *ws,uint32_t epoch,const char *path,bool independent) {
    (void)ws;(void)epoch;(void)independent;assert(!held&&strstr(path,"peer"));
    ++new_attempts;return sd_audio_catalog_ack("peer",0U);
}
''')
fixture = fixture.replace('#include "sd_scheduler_host.inc"', r'''
static unsigned bad_mode,bad_calls;
static atomic_uint s_capture_recovery_until;
static bool nuwax_offline_audio_capture_running(void){return true;}
static int64_t recorded_deadline;
static void record_deadline(int64_t until){assert(held);if(until)recorded_deadline=until;sd_audio_catalog_repair_set_deadline(until);}
static esp_err_t injected_meeting(const uint8_t *owner,sd_audio_session_t *scene,sd_audio_segment_t *part,bool *found,bool *file,bool *waiting){
    if(!bad_mode){++bad_calls;*found=*file=*waiting=false;return ESP_ERR_INVALID_CRC;}
    return sd_meeting_fifo_next(owner,scene,part,found,file,waiting);
}
static esp_err_t injected_monitor(const uint8_t *owner,sd_audio_session_t *scene,sd_audio_segment_t *part,bool *found,bool *file,bool *waiting){
    if(bad_mode){++bad_calls;*found=*file=*waiting=false;return ESP_ERR_INVALID_CRC;}
    return sd_monitor_fifo_next(owner,scene,part,found,file,waiting);
}
#define sd_meeting_fifo_next injected_meeting
#define sd_monitor_fifo_next injected_monitor
#define sd_audio_catalog_repair_set_deadline record_deadline
#define ESP_PLATFORM 1
#include "sd_scheduler_host.inc"
#undef ESP_PLATFORM
#undef sd_audio_catalog_repair_set_deadline
#undef sd_monitor_fifo_next
#undef sd_meeting_fifo_next
''')
fixture += r'''
int main(int argc,char **argv){
    assert(argc==2);bad_mode=(unsigned)atoi(argv[1]);assert(bad_mode<2U&&sd_audio_catalog_init()==ESP_OK);
    sd_audio_session_t bad=create_session("retained",bad_mode,1U,1U);
    sd_audio_session_t peer=create_session("peer",1U-bad_mode,1U,1U);
    make_index(bad.session_id,1U,false);make_index(peer.session_id,1U,false);
    sd_audio_segment_t original;assert(sd_audio_catalog_read(peer.session_id,0U,&original)==ESP_OK);
    FILE *f=fopen(original.path,"wb");assert(f&&fputs("retained source",f)!=EOF&&!fclose(f));
    test_now=UINT32_MAX-255U;s_capture_recovery_until=test_now+600U;
    nuwax_ws_client_t ws=0;bool worked=false;
    for(unsigned i=0;i<8U&&(!new_attempts||!bad_calls);++i){
        const esp_err_t err=card_replay_next(&ws,1U,true,&worked);
        assert((err==ESP_OK||err==ESP_ERR_TIMEOUT)&&!held&&!catalog_repair_deadline);
    }
    assert(new_attempts==1U&&bad_calls==1U&&s_card_pass_error==ESP_ERR_INVALID_CRC);
    assert(recorded_deadline==4600000LL); /* 600ms remaining capture budget, not 1500ms. */
    const uint32_t until=bad_mode?s_monitor_retry_until:s_meeting_retry_until;
    assert(until==744U);
    sd_audio_segment_t saved;assert(sd_audio_catalog_read(bad.session_id,0U,&saved)==ESP_OK&&!saved.acknowledged&&!saved.missing);
    test_now+=999U;s_capture_recovery_until=test_now+600U;
    for(unsigned i=0;i<4U;++i)(void)card_replay_next(&ws,1U,true,&worked);
    assert(bad_calls==1U);
    ++test_now;s_capture_recovery_until=test_now+600U;
    for(unsigned i=0;i<4U&&bad_calls==1U;++i)(void)card_replay_next(&ws,1U,true,&worked);
    assert(bad_calls==2U&&!held&&!catalog_repair_deadline);
    puts("PASS actual scheduler: metadata error isolated to its lane; peer file ACK progresses; failed scene retained; retry survives clock wrap; native repair budget capped and cleared");
    return 0;
}
'''
src = out / 'lanes.c'
src.write_text(fixture, encoding='utf-8')
exe = out / 'lanes.exe'
subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
    '-I'+str(out), '-I'+str(root/'main/audio_modes/meeting/host_stubs'), '-I'+str(root/'main'), str(src), '-o', str(exe)], check=True)
for mode in (0, 1):
    with tempfile.TemporaryDirectory(prefix='sd-metadata-lane-') as directory:
        subprocess.run([str(exe), str(mode)], cwd=directory, check=True, timeout=30)
