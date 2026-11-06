"""Execute production writer-first admission with controlled task interleavings."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-r195';out.mkdir(exist_ok=True,parents=True)
source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#define ESP_PLATFORM 1
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define QUEUE_DEPTH 128U
#define ESP_OK 0
#define ESP_ERR_NOT_FINISHED 1
#define ESP_ERR_INVALID_STATE 2
#define pdTRUE 1
#define pdFALSE 0
#define portTICK_PERIOD_MS 1
#define REPLAY_STORAGE_PROGRESS_BIT 16U
typedef unsigned EventBits_t;
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef struct {bool connected;unsigned epoch;} nuwax_ws_client_t;
static struct {atomic_bool writer_busy;int lock,events;} s;
static atomic_bool s_storage_wait_armed;
static atomic_bool s_card_online=true,s_formal_replay_cancel_requested;
static unsigned queued,wake,ticks,take_calls,gives,scenario,gate_race;
static bool locked,busy;static uint64_t time_us=1;static atomic_bool stop;
static uint32_t replay_capture_queue_depth(void){return queued;}
static uint32_t nuwax_wake_word_capture_backlog_ms(void){return wake;}
static bool nuwax_offline_audio_storage_busy(void){return busy;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *w){return w->connected;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *w){return w->epoch;}
static int64_t esp_timer_get_time(void){return (int64_t)time_us;}
static void vTaskDelay(unsigned t);
static EventBits_t xEventGroupClearBits(int e,EventBits_t b){(void)e;(void)b;return 0;}
static EventBits_t xEventGroupWaitBits(int e,EventBits_t b,int c,int a,unsigned t){(void)e;(void)b;(void)c;(void)a;vTaskDelay(t);return 0;}
static int xSemaphoreTake(int l,unsigned t){(void)l;take_calls++;if(locked&&t)vTaskDelay(t);if(locked)return 0;
 locked=true;if(gate_race==1){s.writer_busy=true;gate_race=0;}else if(gate_race==2){queued=8;gate_race=0;}return pdTRUE;}
static void xSemaphoreGive(int l){(void)l;assert(locked);locked=false;gives++;}
static void vTaskDelay(unsigned t){assert(!locked||scenario==3);assert(t==1);time_us+=1000;ticks++;assert(ticks<20);
 switch(scenario){case 1:queued=ticks==1?6:2;break;case 2:s.writer_busy=false;break;
 case 3:locked=false;break;case 4:stop=true;break;case 5:queued=32;break;
 case 6:wake=ticks==1?40:30;break;case 7:s.writer_busy=false;queued=2;break;default:break;}}
static void nuwax_wake_word_wait_capture_headroom(unsigned t){vTaskDelay(t);}
'''+(root/'main/offline_replay_pressure.inc').read_text(encoding='utf-8')+r'''
static void reset(void){assert(!locked);queued=wake=ticks=take_calls=gives=scenario=gate_race=0;busy=false;
 stop=false;s.writer_busy=false;s_card_online=true;s_formal_replay_cancel_requested=false;time_us=1;}
int main(void){nuwax_ws_client_t w={true,3};uint64_t wait=0;
 reset();assert(replay_read_storage_enter(&w,3,&stop,&wait)==0&&locked);replay_read_storage_leave();assert(!locked);
 reset();queued=8;scenario=1;assert(!replay_read_storage_enter(&w,3,&stop,&wait)&&ticks==2&&locked);replay_read_storage_leave();
 reset();s.writer_busy=true;scenario=2;assert(!replay_read_storage_enter(&w,3,&stop,&wait)&&ticks==1&&take_calls==1);replay_read_storage_leave();
 reset();locked=true;scenario=3;assert(!replay_read_storage_enter(&w,3,&stop,&wait)&&ticks==1);replay_read_storage_leave();
 for(unsigned race=1;race<=2;race++){reset();gate_race=race;scenario=7;
  assert(!replay_read_storage_enter(&w,3,&stop,&wait)&&ticks==1&&gives==1&&locked);replay_read_storage_leave();}
 reset();queued=128;scenario=4;assert(replay_read_storage_enter(&w,3,&stop,&wait)==ESP_ERR_NOT_FINISHED&&!locked&&!take_calls);
 reset();s_card_online=false;assert(replay_read_storage_enter(&w,3,&stop,&wait)==ESP_ERR_NOT_FINISHED&&!locked);
 reset();busy=true;assert(replay_read_storage_enter(&w,3,&stop,&wait)==ESP_ERR_NOT_FINISHED&&!locked);
 reset();s_formal_replay_cancel_requested=true;assert(replay_read_storage_enter(&w,3,&stop,&wait)==ESP_ERR_NOT_FINISHED&&!locked);
 reset();assert(replay_read_storage_enter(&w,4,&stop,&wait)==ESP_ERR_INVALID_STATE&&!locked);
 reset();w.connected=false;assert(replay_read_storage_enter(&w,3,&stop,&wait)==ESP_ERR_INVALID_STATE&&!locked);w.connected=true;
 reset();wake=60;scenario=6;assert(!replay_read_storage_enter(&w,3,&stop,&wait)&&ticks==2);replay_read_storage_leave();
 reset();queued=32;assert(!replay_wait_capture_headroom(&w,3,&stop,&wait)&&!ticks&&!locked);
 reset();queued=64;scenario=5;assert(!replay_wait_capture_headroom(&w,3,&stop,&wait)&&ticks==1&&!locked);
 puts("PASS: actual SD storage lock, writer-first 8/2 queue gate, busy/mutex races, release, cancellation/owner/epoch and wake fences; original transport 64/32 guard retained");
}
'''
src=out/'admission.c';exe=out/'admission.exe';src.write_text(source,encoding='utf-8')
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True,timeout=30)

# Execute the unchanged production DMA adapter at the two resource states.
# 24K free -> read loan holds 8K -> writer sees only its 16K floor (old).
# Serializing that loan with the writer restores an 8K multi-sector window.
# This demonstrates admission/command geometry, not real card or Wi-Fi speed.
fixture=(root/'main/sd_dma_bulk_host_check.c').read_text(encoding='utf-8')
fixture=fixture[:fixture.index('int main(void)')]
bulk=(root/'main/board/sd_dma_bulk.c').read_text(encoding='utf-8')
bulk=bulk[bulk.index('static atomic_uintptr_t s_recovery_reader;'):]
fixture=fixture.replace('#include "sd_dma_bulk_host.inc"',bulk)
fixture+=r'''
int main(void){
 sdmmc_card_t card={{512,64}};memset(external,0x5a,sizeof(external));
 reset();free_bytes=16384;uint32_t fallbacks=s_fallback_sectors;
 assert(__wrap_sdmmc_write_sectors(&card,external,0,32)==ESP_OK);
 assert(bad_dma&&s_fallback_sectors-fallbacks==32&&!allocations);
 reset();free_bytes=24576;fallbacks=s_fallback_sectors;
 assert(__wrap_sdmmc_write_sectors(&card,external,0,32)==ESP_OK);
 assert(!bad_dma&&calls==2&&largest_count==16&&s_fallback_sectors==fallbacks&&!allocations&&free_bytes==24576);
 assert(!memcmp(external,disk,sizeof(external)));
 puts("PASS: production adapter resource-state probe: overlapping 8K read loan forces SDK fallback; serialized loan restores two 8K DMA commands for 16K write, exact bytes and 16K reserve (not a speed benchmark)");
}
'''
src=out/'dma-resources.c';exe=out/'dma-resources.exe';src.write_text(fixture,encoding='utf-8')
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'main'),str(src),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True,timeout=30)
