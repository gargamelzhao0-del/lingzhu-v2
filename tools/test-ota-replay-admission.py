"""Run production OTA admission and replay ownership with deterministic faults."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]

def function(path, name):
    source = (root / path).read_text(encoding='utf-8')
    match = re.search(r'(?ms)^(?:static )?(?:bool|esp_err_t) ' + name + r'\(.*?^}', source)
    assert match, name
    return match.group()

common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FINISHED 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_INVALID_CRC 4
#define ESP_ERR_TIMEOUT 5
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
static bool shutdown, ota;
static bool nuwax_power_ctrl_shutdown_preparing(void){return shutdown;}
static bool enabled(void){return true;}
static bool runtime_initialized(void){return true;}
static atomic_bool s_replay_test_transport_quarantined;
'''

admission = common + r'''
static bool reprovision, notification, lease, capture, archive;
static bool meeting, monitoring, replay;
static int s_notification_transaction, s_meeting_mode, s_monitoring_mode;
static bool reprovision_request_pending(void){return reprovision;}
static bool nuwax_notification_blocks_conversation(int *x){(void)x;return notification;}
static bool nuwax_meeting_mode_blocks_ota(int *x){(void)x;return meeting;}
static bool nuwax_monitoring_mode_blocks_ota(int *x){(void)x;return monitoring;}
static bool offline_mode_lease_pending(void){return lease;}
static bool nuwax_offline_audio_capture_running(void){return capture;}
static bool nuwax_offline_audio_is_active(void){return archive;}
''' + function('main/offline_audio_spool.c', 'nuwax_offline_audio_replay_blocks_ota') + '\n' + function(
    'main/nuwax_ai_partner_main.c', 'ota_continuous_audio_busy') + r'''
int main(void){
 (void)ota; (void)replay;
 assert(!ota_continuous_audio_busy(NULL));
 replay=true; assert(!ota_continuous_audio_busy(NULL));
 bool *guards[]={&shutdown,&reprovision,&notification,&lease,&capture,&archive,&meeting,&monitoring};
 for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){
  *guards[i]=true; assert(ota_continuous_audio_busy(NULL));
  *guards[i]=false; assert(!ota_continuous_audio_busy(NULL));
 }
 atomic_store(&s_replay_test_transport_quarantined,true);
 assert(ota_continuous_audio_busy(NULL));
 puts("PASS: production OTA admission allows background replay; recording/mode/lease/provisioning/notification/shutdown/quarantined transport still block");
}
'''

owner = common + r'''
typedef int nuwax_ws_client_t;
static struct {atomic_bool replay_running, media_recovery_requested; atomic_uint ready_count; int lock;}s;
static atomic_bool s_card_online, s_shutdown_requested, s_quarantine_recheck_pending;
static int s_card_pass_error;
static bool lock_ok=true, media_ok=true, busy, cancel_wait;
static unsigned resets, locks, unlocks, uploads, delays;
static unsigned repair_resets,fail_reset,owned_dirs;
static bool inject_repair_cancel;
static bool nuwax_offline_audio_storage_busy(void){return busy||ota;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *w){(void)w;return true;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *w){(void)w;return 7;}
static int replay_ack_cancel_status(nuwax_ws_client_t *w,uint32_t e,bool b){
 (void)w;(void)e;(void)b;return cancel_wait?ESP_ERR_NOT_FINISHED:ESP_OK;
}
static void vTaskDelay(unsigned t){assert(t==1);if(++delays==2)lock_ok=true;}
static bool nuwax_sd_card_is_mounted(void){return media_ok;}
static bool replay_ota_active(void){return ota;}
static int xSemaphoreTake(int l,unsigned t){(void)l;(void)t;locks++;return lock_ok;}
static void xSemaphoreGive(int l){(void)l;unlocks++;}
static void sd_audio_catalog_replay_reset(void){resets++;}
static int card_commit_flush(void){assert(s.replay_running);return ESP_OK;}
static void card_commit_release_idle(void){assert(s.replay_running&&!owned_dirs);}
static int sd_audio_catalog_repair_reset(void){
 assert(s.replay_running&&locks>unlocks);owned_dirs=0;
 return ++repair_resets==fail_reset?ESP_FAIL:ESP_OK;
}
static int card_replay_next(nuwax_ws_client_t *w,uint32_t e,bool i,bool *work){
 (void)w;(void)e;(void)i;*work=false;
 if(busy)return ESP_ERR_NOT_FINISHED;
 if(inject_repair_cancel){inject_repair_cancel=false;owned_dirs=2;busy=true;*work=true;}
 return ESP_OK;
}
static int find_next_ready_name(const char *a,char *n,size_t s,bool *f){(void)a;(void)n;(void)s;*f=false;return ESP_OK;}
static bool compose_child_path(char *p,size_t s,const char *n){(void)p;(void)s;(void)n;return false;}
static bool replay_path_quarantined(const char *p){(void)p;return false;}
static int restore_quarantined_archive(const char *p){(void)p;return ESP_OK;}
static int replay_one_ready(nuwax_ws_client_t *w,uint32_t e,const char *p,bool i){(void)w;(void)e;(void)p;(void)i;uploads++;return ESP_OK;}
static int recheck_archive_path(const char *p){(void)p;return ESP_OK;}
static size_t strlcpy(char *d,const char *s,size_t n){size_t k=strlen(s);if(n)snprintf(d,n,"%s",s);return k;}
''' + function('main/offline_audio_spool.c', 'replay_ready_impl') + r'''
int main(void){
 (void)s_replay_test_transport_quarantined;
 nuwax_ws_client_t ws=0; atomic_store(&s_card_online,true);
 lock_ok=false;cancel_wait=true;
 assert(replay_ready_impl(&ws,7,true)==ESP_ERR_NOT_FINISHED);
 assert(!atomic_load(&s.replay_running) && locks==1 && !unlocks && !uploads);
 cancel_wait=false; /* Temporary lock contention may wait, but remains cancellable. */
 assert(replay_ready_impl(&ws,7,true)==ESP_OK);
 assert(!atomic_load(&s.replay_running) && resets==1 && unlocks==2 && repair_resets==2 && !uploads && delays==2);
 const unsigned before_ota=locks;
 ota=true;
 assert(replay_ready_impl(&ws,7,true)==ESP_ERR_NOT_FINISHED);
 assert(replay_ready_impl(&ws,7,false)==ESP_ERR_NOT_FINISHED && locks==before_ota);
 ota=false; shutdown=true;
 assert(replay_ready_impl(&ws,7,true)==ESP_ERR_NOT_FINISHED && !atomic_load(&s.replay_running));
 shutdown=false;inject_repair_cancel=true;busy=false;
 assert(replay_ready_impl(&ws,7,true)==ESP_ERR_NOT_FINISHED&&!s.replay_running&&!owned_dirs);
 busy=false;fail_reset=repair_resets+1;
 assert(replay_ready_impl(&ws,7,true)==ESP_FAIL&&!s.replay_running&&!owned_dirs);
 fail_reset=repair_resets+2;
 assert(replay_ready_impl(&ws,7,true)==ESP_FAIL&&!s.replay_running&&!owned_dirs);
 fail_reset=0;
 atomic_store(&s.replay_running,true);
 assert(replay_ready_impl(&ws,7,true)==ESP_ERR_INVALID_STATE && atomic_load(&s.replay_running));
 puts("PASS: actual replay entry releases ownership on catalog lock failure; next attempt succeeds; OTA blocks both routes and concurrent replay retains its owner's flag");
 puts("PASS: actual replay cancellation/start failure/cleanup failure closes suspended repair state under storage lock before publishing the owner idle");
}
'''

with tempfile.TemporaryDirectory(prefix='nuwax-ota-replay-') as directory:
    for name, text in [('admission', admission), ('owner', owner)]:
        source = Path(directory) / (name + '.c')
        exe = source.with_suffix('.exe')
        source.write_text(text, encoding='utf-8')
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
