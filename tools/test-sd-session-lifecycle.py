"""Actual SD start/finalize and durable catalog under forced task interleavings."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-session-lifecycle'
out.mkdir(parents=True,exist_ok=True)
storage=(root/'main/sd_audio_session_storage.inc').read_text(encoding='utf-8')
source=(root/'main/sd_audio_identity_host_check.c').read_text(encoding='utf-8')
source=source[:source.index('static void durable_finish(void)')]
source=source.replace('static struct { atomic_bool active, closing; int lock; } s;',
    'static struct { atomic_bool active, closing, writer_busy, capture_complete, degraded; int lock, queue; } s;')
source=source.replace('static bool lock_ready = true, owner_ready = true;',
    '''static bool lock_ready = true, owner_ready = true, inject_finalizer, inject_replacement;
static bool replacement_cloud;
static int64_t replacement_deadline;
static char replacement_id[48];
static atomic_bool s_media_ready=true, s_card_pending_upload;
static unsigned finalize_interleavings;
static void card_finalize(void);
#define ESP_LOGI(...) ((void)0)
#define TAG "sd-lifecycle-host"
#define ESP_LOGW(tag,...) do{(void)(tag);if(0)printf(__VA_ARGS__);}while(0)
static unsigned uxQueueMessagesWaiting(int q) { (void)q; return 0; }
static bool card_stop_time(uint64_t *t) { *t=s_card_stopped_ms; return s_card_stopped; }
static void card_save_time_locked(void) {}
static esp_err_t card_monitor_prepare_space(void) { return ESP_OK; }
static unsigned recoveries;
static esp_err_t recover_open_segments(void) { ++recoveries; return ESP_OK; }
void nuwax_sd_card_mark_io_fault(esp_err_t e) { (void)e; }
static void writer_note_fault(const char *stage, int error) { (void)stage; (void)error; }
''')
source=source.replace('(void)lock; (void)ms; if (!lock_ready)',
    '''(void)lock;
  /* Other core runs the real supervisor finalizer between reservation and lock. */
  if (inject_finalizer && ms == 100) {
      inject_finalizer=false; ++finalize_interleavings; card_finalize();
  }
  if (inject_replacement && ms == 0) {
      inject_replacement=false; ++finalize_interleavings;
      card_finalize();
      assert(!nuwax_sd_session_active());
      esp_err_t r=replacement_cloud ? nuwax_sd_session_start_until(1,replacement_id,replacement_deadline)
                                   : nuwax_sd_session_start(1,replacement_id);
      assert(r==ESP_OK);
  }
  if (lock_depth) return 0;
  if (!lock_ready)''')
source=source.replace('if (!lock_ready) return 0; assert(!lock_depth); ++lock_depth;',
    'if (!lock_ready) { return 0; }\n  assert(!lock_depth); ++lock_depth;')
source=source.replace('#include "sd_identity_start_host.inc"',
    storage[storage.index('esp_err_t nuwax_sd_session_check_new_identity('):storage.index('void nuwax_sd_session_stop(')])
source=source.replace('#include "sd_identity_async_host.inc"',
    storage[storage.index('typedef struct {\n    char session_id[48];'):storage.index('static bool card_recording_locked(')])
source+=r'''
int main(int argc, char **argv)
{
    const bool cloud=argc>1 && strstr(argv[1],"cloud");
    const bool race=argc>1 && strstr(argv[1],"race");
    const bool delayed=argc>1 && strstr(argv[1],"delayed");
    assert(sd_audio_catalog_init()==ESP_OK);
    char old_id[48]="previous_scene", new_id[48]="next_scene";
    assert(nuwax_sd_session_start(0,old_id)==ESP_OK);
    assert(card_prepare_capture());
    atomic_store(&s.capture_complete,true);
    atomic_store(&s_card_finishing,true);
    if (argc > 1 && strstr(argv[1], "retention-stop")) {
        sd_audio_session_t retired;
        assert(sd_audio_catalog_load(old_id,&retired)==ESP_OK);
        retired.complete=0; assert(sd_audio_catalog_save(&retired)==ESP_OK);
        assert(sd_audio_catalog_pin(old_id,0)==ESP_OK);
        card_finalize();
        assert(!nuwax_sd_session_active() && !s_card_session.complete && !recoveries);
        assert(sd_audio_catalog_replay_pinned());
        sd_audio_catalog_unpin();
        puts("PASS: intentional retention gap finalizes without whole-card recovery or invalidating a live upload lease");
        return 0;
    }
    if (argc > 1 && strstr(argv[1], "fault-pin")) {
        atomic_store(&s_card_failed,true);
        assert(sd_audio_catalog_pin(old_id,0)==ESP_OK);
        card_finalize();
        assert(nuwax_sd_session_active() && s_card_finishing && !recoveries && sd_audio_catalog_replay_pinned());
        sd_audio_catalog_unpin(); card_finalize();
        assert(!nuwax_sd_session_active() && recoveries==1);
        puts("PASS: genuine capture fault waits for replay lease release before directory recovery");
        return 0;
    }
    if (!delayed) card_finalize();
    assert(nuwax_sd_session_active()==delayed);
    assert(atomic_load(&s_card_finishing)==delayed);
    sd_audio_session_t saved;
    assert(sd_audio_catalog_load(old_id,&saved)==ESP_OK && saved.ended==!delayed);
    const int64_t deadline=now_us+20000000;
    if(cloud) {
        assert(nuwax_sd_session_poll_identity(1,new_id,deadline)==ESP_ERR_NOT_FINISHED);
        card_identity_service();
        assert(nuwax_sd_session_poll_identity(1,new_id,deadline)==ESP_OK);
    }
    inject_finalizer=race;
    if (delayed) {
        strcpy(replacement_id,new_id); replacement_cloud=cloud; replacement_deadline=deadline;
        inject_replacement=true; card_finalize();
        assert(finalize_interleavings==1 && nuwax_sd_session_active() && !atomic_load(&s_card_finishing));
        assert(card_prepare_capture());
        assert(sd_audio_catalog_load(new_id,&saved)==ESP_OK && !saved.ended);
        assert(sd_audio_catalog_load(old_id,&saved)==ESP_OK && saved.ended);
        puts("PASS: finalizer revalidates owner after lock interleaving");
        return 0;
    }
    const esp_err_t result=cloud ? nuwax_sd_session_start_until(1,new_id,deadline)
                                : nuwax_sd_session_start(1,new_id);
    assert(result==ESP_OK && finalize_interleavings==(unsigned)race && starts==2);
    /* Execute the capture task's real preparation after its successful launch. */
    bool preparation=card_prepare_capture();
    esp_err_t manifest=sd_audio_catalog_load(new_id,&saved);
    printf("%s reservation_vs_finalizer cloud=%d start_ok=%d mic_launches=%u active_after_start=%d prepare_ok=%d new_manifest_found=%d\n",
        "PASS",cloud,result==ESP_OK,starts,nuwax_sd_session_active(),preparation,manifest==ESP_OK);
    assert(preparation);
    assert(nuwax_sd_session_active() && manifest==ESP_OK && !saved.ended);
    return 0;
}
'''
(out/'sd-session-lifecycle.c').write_text(source,encoding='utf-8')
exe=out/'sd-session-lifecycle.exe'
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
    '-DCONFIG_NUWAX_SD_SESSION_ENABLE=1','-DCONFIG_NUWAX_OFFLINE_AUDIO_ENABLE=1',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
    '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),
    str(root/'main/sd_audio_catalog.c'),str(out/'sd-session-lifecycle.c'),'-o',str(exe)],check=True)
logs=[]
for case in ('local-control','cloud-control','local-race','cloud-race','local-delayed','cloud-delayed','retention-stop','fault-pin'):
    with tempfile.TemporaryDirectory(prefix='nuwax-sd-lifecycle-') as d:
        result=subprocess.run([str(exe),case],cwd=d,capture_output=True,text=True,timeout=30)
    logs.append(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
(out/'sd-session-lifecycle.log').write_text(''.join(logs),encoding='utf-8')
print(''.join(logs))
