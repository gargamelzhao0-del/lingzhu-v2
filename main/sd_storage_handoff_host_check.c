/* Actual production ESP admission + writer publication; deterministic RTOS. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define ESP_PLATFORM 1
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define ESP_OK 0
#define ESP_ERR_NOT_FINISHED 1
#define ESP_ERR_INVALID_STATE 2
#define pdTRUE 1
#define pdFALSE 0
#define portTICK_PERIOD_MS 10U
#define REPLAY_STORAGE_PROGRESS_BIT 16U
#define QUEUE_DEPTH 128U
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef unsigned EventBits_t;
typedef struct {bool connected;unsigned epoch;} nuwax_ws_client_t;
static struct {atomic_bool writer_busy;int lock,events;} s;
static atomic_bool s_card_online=true,s_formal_replay_cancel_requested,s_storage_wait_armed;
static unsigned now,queued,wake,bits,scenario,releases,waits,takes,gives,race;
static bool held,external,applied;
static nuwax_ws_client_t ws;
static void writer_storage_idle(void);
static void release_at_two(void){now=2000;applied=true;external=false;queued=2;wake=30;releases++;writer_storage_idle();}
static EventBits_t xEventGroupSetBits(int event,EventBits_t set){assert(event==1);bits|=set;return bits;}
static EventBits_t xEventGroupClearBits(int event,EventBits_t clear){assert(event==1);bits&=~clear;
 if(scenario==1&&!applied)release_at_two();return bits;}
static EventBits_t xEventGroupWaitBits(int event,EventBits_t mask,int clear,int all,unsigned ticks){
 assert(event==1&&mask==REPLAY_STORAGE_PROGRESS_BIT&&clear==pdTRUE&&all==pdFALSE&&ticks==1&&!held);
 assert(atomic_load(&s_storage_wait_armed));assert(++waits<8);
 if(!applied){if(scenario==7){now=2000;s_formal_replay_cancel_requested=true;applied=true;}
  else release_at_two();}
 if(!(bits&mask))now=(now/10000+1)*10000;
 const EventBits_t result=bits;if(clear)bits&=~mask;return result;
}
static bool nuwax_offline_audio_storage_busy(void){return false;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *w){return w->connected;}
static unsigned nuwax_ws_connection_epoch(nuwax_ws_client_t *w){return w->epoch;}
static unsigned replay_capture_queue_depth(void){return queued;}
static unsigned nuwax_wake_word_capture_backlog_ms(void){return wake;}
static int64_t esp_timer_get_time(void){return now;}
static int xSemaphoreTake(int lock,unsigned ticks){assert(lock==1);takes++;
 if(ticks){assert(ticks==1&&!held);if(external&&!applied){release_at_two();if(scenario==6)ws.epoch++;}}
 if(held||external)return 0;held=true;
 if(race){if(race==1)s.writer_busy=true;else queued=8;race=0;}
 return pdTRUE;
}
static void xSemaphoreGive(int lock){assert(lock==1&&held);held=false;gives++;}
static void vTaskDelay(unsigned ticks){assert(ticks==1&&!held);now=(now/10000+1)*10000;}
static void nuwax_wake_word_wait_capture_headroom(unsigned ms){assert(ms==10&&!held);release_at_two();}
#include "sd-writer-idle.inc"
#include "offline_replay_pressure.inc"
static void reset(unsigned test){
 now=1000;queued=wake=bits=waits=takes=gives=race=releases=0;scenario=test;held=external=applied=false;
 s.writer_busy=false;s.lock=s.events=1;s_card_online=true;s_formal_replay_cancel_requested=false;s_storage_wait_armed=false;
 ws=(nuwax_ws_client_t){true,7};
}
int main(void){uint64_t elapsed;replay_storage_stats_t stats;
 for(unsigned test=1;test<=4;test++){
  reset(test);s.writer_busy=true;elapsed=0;memset(&stats,0,sizeof(stats));
  if(test==3){s.writer_busy=false;queued=8;}if(test==4)bits=REPLAY_STORAGE_PROGRESS_BIT;
  assert(replay_read_storage_enter_profiled(&ws,7,NULL,&elapsed,&stats)==ESP_OK);
  assert(held&&now==2000&&!s_storage_wait_armed);replay_read_storage_leave();
  assert(!held&&releases==1);
  if(test!=1)assert(elapsed==1000&&stats.signals==1&&stats.timeouts==0&&stats.total_us==1000);
 }
 puts("PASS handoff: release before arm, release during wait entry, queue drain, stale bit; 2ms release resumes at 2ms, not next 10ms tick");
 reset(5);external=true;elapsed=0;memset(&stats,0,sizeof(stats));
 assert(replay_read_storage_enter_profiled(&ws,7,NULL,&elapsed,&stats)==ESP_OK&&held&&now==2000);
 assert(stats.mutex_waits==1&&stats.mutex_us==1000);replay_read_storage_leave();
 reset(6);external=true;assert(replay_read_storage_enter(&ws,7,NULL,NULL)==ESP_ERR_INVALID_STATE&&!held&&!s_storage_wait_armed);
 reset(7);s.writer_busy=true;assert(replay_read_storage_enter(&ws,7,NULL,NULL)==ESP_ERR_NOT_FINISHED&&!held&&now==10000&&!s_storage_wait_armed);
 puts("PASS mutex wake and cancellation: recheck epoch after lock wake, cancel during signal timeout, no leaked lock or armed waiter");
 for(unsigned kind=1;kind<=2;kind++){
  reset(2);race=kind;assert(replay_read_storage_enter(&ws,7,NULL,NULL)==ESP_OK&&held&&now==2000&&gives==1);
  replay_read_storage_leave();
 }
 reset(8);wake=60;elapsed=0;assert(replay_read_storage_enter(&ws,7,NULL,&elapsed)==ESP_OK&&held&&now==2000);replay_read_storage_leave();
 reset(9);atomic_bool cancelled=true;assert(replay_read_storage_enter(&ws,7,&cancelled,NULL)==ESP_ERR_NOT_FINISHED&&!held);
 reset(9);s_card_online=false;assert(replay_read_storage_enter(&ws,7,NULL,NULL)==ESP_ERR_NOT_FINISHED&&!held);
 puts("PASS writer/queue race after lock, wake hysteresis, offline/cancel fences and original TX guard compile");
 return 0;
}
