"""Production start barrier, multi-file validation cache and capacity policies."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
prefetch=(root/'main/offline_replay_prefetch.inc').read_text(encoding='utf-8-sig')

def function(source,name):
    start=source.index(name);at=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[at]=='{')-(source[at]=='}');at+=1
    return source[start:at]+'\n'

common=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_err.h"
'''
lease=common+r'''
#include "sd_capture_admission.h"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define CONFIG_NUWAX_OTA_ENABLE 1
#define REPLAY_ACK_BIT 1U
#define REPLAY_CHUNK_ACK_BIT 2U
#define pdTRUE 1
typedef unsigned nuwax_offline_audio_mode_t;
static sd_capture_admission_t s_capture_admission;
static struct {int admission_lock,events;atomic_bool media_recovery_requested,writer_error,active,closing,capture_running,writer_busy;void *queue;} s={.admission_lock=1,.events=2};
static atomic_bool s_formal_replay_running,s_formal_replay_cancel_requested,s_shutdown_requested,
  s_card_finishing,s_card_failed,s_media_ready=true;
static atomic_uint s_mirror_closers;
static bool initialized=true,can_lock=true,ota,local_active,monitoring;
static unsigned lock_depth,ack_wakes;
static int64_t now_us=1000000;
static int64_t esp_timer_get_time(void){return now_us;}
static uint32_t offline_now_ms(void){return (uint32_t)(now_us/1000);}
static bool runtime_initialized(void){return initialized;}
static bool enabled(void){return true;}
static bool nuwax_ota_device_is_active(void){return ota;}
static bool nuwax_sd_session_active(void){return local_active;}
static bool nuwax_sd_session_monitoring(void){return local_active&&monitoring;}
static bool nuwax_offline_audio_replay_running(void){return atomic_load(&s_formal_replay_running);}
static unsigned uxQueueMessagesWaiting(void *q){(void)q;return 0;}
static int xSemaphoreTake(int m,unsigned ticks){assert(m==1&&!ticks);if(!can_lock)return 0;lock_depth++;return 1;}
static void xSemaphoreGive(int m){assert(m==1&&lock_depth);lock_depth--;}
static unsigned xEventGroupSetBits(int e,unsigned bits){assert(e==2&&bits==3);ack_wakes++;return bits;}
'''
for name in ['bool nuwax_offline_audio_storage_busy(void)',
             'esp_err_t nuwax_sd_session_prepare_start(',
             'void nuwax_sd_session_release_start(']:
    lease+=function(spool,name)
lease+=r'''
int main(void){
  const int64_t end=now_us+4000000;
  s_formal_replay_running=true;
  assert(nuwax_sd_session_prepare_start(0,end)==ESP_ERR_NOT_FINISHED);
  assert(s_formal_replay_cancel_requested&&ack_wakes==1&&!lock_depth&&nuwax_offline_audio_storage_busy());
  s_formal_replay_running=false;s_formal_replay_cancel_requested=false;
  assert(nuwax_sd_session_prepare_start(0,end)==ESP_OK&&nuwax_offline_audio_storage_busy());
  nuwax_sd_session_release_start(1);assert(nuwax_offline_audio_storage_busy());
  assert(nuwax_sd_session_prepare_start(1,end)==ESP_OK);
  nuwax_sd_session_release_start(0);assert(nuwax_offline_audio_storage_busy());
  nuwax_sd_session_release_start(1);assert(!nuwax_offline_audio_storage_busy());
  assert(nuwax_sd_session_prepare_start(2,end)==ESP_ERR_INVALID_ARG);
  can_lock=false;assert(nuwax_sd_session_prepare_start(0,end)==ESP_ERR_NOT_FINISHED);
  /* R213 publishes the bounded lease even when an idle exporter owns the
   * gate, so that exporter can yield. Explicit release still isolates owners. */
  assert(nuwax_offline_audio_storage_busy());nuwax_sd_session_release_start(0);
  assert(!nuwax_offline_audio_storage_busy());can_lock=true;
  initialized=false;assert(nuwax_sd_session_prepare_start(0,end)==ESP_ERR_NOT_FINISHED);initialized=true;
  now_us=end;assert(nuwax_sd_session_prepare_start(0,end)==ESP_ERR_TIMEOUT);
  now_us=((int64_t)UINT32_MAX-1500)*1000;
  assert(nuwax_sd_session_prepare_start(0,now_us+2000000)==ESP_OK);
  now_us+=1999000;assert(nuwax_offline_audio_storage_busy());
  now_us+=2000;assert(!nuwax_offline_audio_storage_busy());
  ota=true;nuwax_sd_session_release_start(0);assert(nuwax_offline_audio_storage_busy());ota=false;
  assert(nuwax_sd_session_prepare_start(0,0)==ESP_OK);now_us+=5001000;
  assert(!nuwax_offline_audio_storage_busy());
  puts("PASS r192: production start barrier cancels/wakes replay, prevents readmission, owner-isolated release, OTA independence, expiry, uint32 wrap and timeout");
}
'''

a=prefetch.index('#define REPLAY_CHECKPOINT_COUNT')
b=prefetch.index('\ntypedef struct {\n    uint8_t *data;',a)
cache=common+r'''
typedef struct {uint32_t position;} replay_stream_t;
typedef struct {uint32_t identity;} archive_header_t;
typedef struct {uint32_t crc;} archive_footer_t;
'''+prefetch[a:b]+r'''
int main(void){
  archive_header_t h={11};archive_footer_t f={22};replay_checkpoint_t c;
  for(unsigned i=0;i<4;i++){
    char key[24];snprintf(key,sizeof(key),"scene_%u",i);
    replay_checkpoint_prepare(&c,key,4193930,&h,&f);c.states[0].position=100+i;replay_checkpoint_store(&c);
  }
  for(unsigned i=0;i<4;i++){
    char key[24];snprintf(key,sizeof(key),"scene_%u",i);
    replay_checkpoint_prepare(&c,key,4193930,&h,&f);assert(c.states[0].position==100+i);
  }
  h.identity++;replay_checkpoint_prepare(&c,"scene_0",4193930,&h,&f);assert(!c.states[0].position);h.identity--;
  f.crc++;replay_checkpoint_prepare(&c,"scene_0",4193930,&h,&f);assert(!c.states[0].position);f.crc--;
  replay_checkpoint_prepare(&c,"scene_0",4193929,&h,&f);assert(!c.states[0].position);
  replay_checkpoint_invalidate();replay_checkpoint_prepare(&c,"scene_0",4193930,&h,&f);assert(!c.states[0].position);
  s_replay_checkpoint_recent=NULL;
  replay_checkpoint_prepare(&c,"a",4193930,&h,&f);c.states[0].position=123;replay_checkpoint_store(&c);
  replay_checkpoint_prepare(&c,"b",4193930,&h,&f);c.states[0].position=456;replay_checkpoint_store(&c);
  replay_checkpoint_prepare(&c,"a",4193930,&h,&f);assert(!c.states[0].position);
  replay_checkpoint_prepare(&c,"b",4193930,&h,&f);assert(c.states[0].position==456);
  puts("PASS r192: four-file checkpoint reuse, immutable identity/length/CRC and media fencing; optional-cache failure safely revalidates");
}
'''

capacity=common+r'''
#include "offline_audio_policy.h"
#include <stdarg.h>
#define CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES 4194304U
#define ARCHIVE_QUOTA_BYTES nuwax_offline_audio_quota(1610612736ULL)
static const char *TAG="test";
static struct {uint64_t used_bytes;} s;
static atomic_bool s_space_blocked;
static uint64_t fake_free;
static int fake_error;
static void log_unused(const char *tag,const char *f,...){(void)tag;(void)f;}
#define ESP_LOGW log_unused
static int esp_vfs_fat_info(const char *p,uint64_t *t,uint64_t *f){(void)p;*t=32ULL<<30;*f=fake_free;return fake_error;}
'''+function(spool,'static esp_err_t refresh_capture_capacity_locked(void)')+r'''
int main(void){
  const uint64_t usage[]={0,959040000ULL,1918080000ULL};
  fake_free=30ULL<<30;
  for(unsigned i=0;i<3;i++){
    s.used_bytes=usage[i];assert(refresh_capture_capacity_locked()==ESP_OK);
    assert(atomic_load(&s_space_blocked)==(!CONFIG_NUWAX_SD_SESSION_ENABLE&&i==2));
  }
  fake_free=20ULL*1024*1024-1;s.used_bytes=0;
  assert(refresh_capture_capacity_locked()==ESP_OK&&s_space_blocked);
  fake_free=30ULL<<30;fake_error=ESP_FAIL;
  assert(refresh_capture_capacity_locked()==ESP_FAIL&&s_space_blocked);
  assert(nuwax_offline_quota_blocked(false,UINT64_MAX,1610612736,1));
  assert(!nuwax_offline_quota_blocked(true,UINT64_MAX,1610612736,1));
  printf("PASS r192: capacity scenes=%u: card uses actual free space, legacy enforces quota; reserve and I/O failure retained\n",CONFIG_NUWAX_SD_SESSION_ENABLE);
}
'''
assert spool.count('nuwax_offline_quota_blocked(')==4
assert 's.used_bytes > ARCHIVE_QUOTA_BYTES' not in spool
with tempfile.TemporaryDirectory(prefix='sd-r192-') as d:
    p=Path(d)
    for name,code,flags in [('lease',lease,[]),('cache',cache,[]),
                            ('capacity-card',capacity,['-DCONFIG_NUWAX_SD_SESSION_ENABLE=1']),
                            ('capacity-legacy',capacity,['-DCONFIG_NUWAX_SD_SESSION_ENABLE=0'])]:
        c=p/(name+'.c');exe=p/(name+'.exe');c.write_text(code,encoding='utf-8')
        subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
            '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),*flags,str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
