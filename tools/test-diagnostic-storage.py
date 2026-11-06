"""Run the actual diagnostic persistence/restore code with NVS fault injection."""
from pathlib import Path
import re, subprocess
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/diagnostic-storage';out.mkdir(parents=True,exist_ok=True)
s=(root/'main/device_error_report.c').read_text(encoding='utf-8')
s=s[:s.index('bool nuwax_device_error_report_enqueue_detail(')]
s=re.sub(r'^#include .*$', '', s, flags=re.M)
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define NUWAX_DEVICE_ERROR_RESULT_BYTES 160U
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 257
#define ESP_ERR_INVALID_SIZE 260
#define ESP_ERR_NVS_NOT_FOUND 4354
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 4357
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_LOGI(tag,...) ((void)(tag), (void)printf(__VA_ARGS__))
#define ESP_LOGW(tag,...) ((void)(tag), (void)printf(__VA_ARGS__))
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef int portMUX_TYPE;
static uint8_t blob[7000];
static size_t blob_size;
static int write_error, guard_error, guard_calls, end_calls, writes;
static size_t available=1000;
typedef struct { size_t available_entries; } nvs_stats_t;
static int nvs_get_stats(const char*p,nvs_stats_t*s){assert(!p);s->available_entries=available;return 0;}
static bool storage_busy, replay, deferred, mutation_on_guard;
static int64_t clock_us;
static void mutate(void);
static const char *esp_err_to_name(int err){(void)err;return "fault";}
static int64_t esp_timer_get_time(void){return clock_us;}
static void *heap_caps_calloc(size_t n,size_t size,int caps){(void)caps;return calloc(n,size);}
static bool nuwax_offline_audio_storage_busy(void){return storage_busy;}
static bool nuwax_offline_audio_replay_running(void){return replay;}
static bool nuwax_lvgl_port_background_work_deferred(void){return deferred;}
static int nuwax_wake_word_detector_begin_flash_operation(unsigned ms){assert(ms==500);++guard_calls;if(mutation_on_guard)mutate();return guard_error;}
static void nuwax_wake_word_detector_end_flash_operation(void){++end_calls;}
static int nvs_open(const char*n,int mode,int*h){assert(!strcmp(n,"nuwax_err"));(void)mode;*h=1;return 0;}
static int nvs_get_blob(int h,const char*k,void*p,size_t*size){assert(h==1&&!strcmp(k,"queue"));if(!blob_size)return ESP_ERR_NVS_NOT_FOUND;if(!p){*size=blob_size;return 0;}if(*size<blob_size)return ESP_ERR_INVALID_SIZE;memcpy(p,blob,blob_size);*size=blob_size;return 0;}
static int nvs_set_blob(int h,const char*k,const void*p,size_t size){assert(h==1&&!strcmp(k,"queue")&&size<=sizeof(blob));++writes;if(write_error)return write_error;memcpy(blob,p,size);blob_size=size;return 0;}
static int nvs_commit(int h){assert(h==1);return 0;}
static void nvs_close(int h){assert(h==1);}
static int nvs_erase_key(int h,const char*k){assert(h==1&&!strcmp(k,"queue"));blob_size=0;return 0;}
static int nvs_erase_all(int h){assert(h==1);blob_size=0;return 0;}
static uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t*p,size_t n){crc=~crc;while(n--){crc^=*p++;for(int i=0;i<8;i++)crc=(crc>>1)^((crc&1)?0xedb88320U:0);}return ~crc;}
'''
checks=r'''
static void mutate(void){s_persist_storage_blocked=true;s_persist_revision++;}
static void reset_state(void){
 memset(s_events,0,sizeof(s_events));s_head=s_count=0;s_persist_loaded=true;
 s_persist_busy=s_persist_dirty=s_persist_storage_blocked=s_persist_cleanup_pending=false;
 s_persist_next_retry_ms=s_persist_revision=s_persist_no_space_failures=0;
 s_persist_retry_delay_ms=0;write_error=guard_error=0;storage_busy=replay=deferred=mutation_on_guard=false;
 clock_us+=100000000;blob_size=0;available=1000;
}
static void fill(unsigned n){
 s_head=13;s_count=n;s_persist_dirty=true;
 for(unsigned i=0;i<n;i++){
  error_report_event_t *e=&s_events[(s_head+i)%16];e->occupied=true;
  e->generation=i;e->in_flight=true;e->attempts=3;strcpy(e->severity,"error");
  snprintf(e->event_id,sizeof(e->event_id),"event_%u",i);
 }
}
int main(void){
 assert(sizeof(error_report_event_t)==396&&sizeof(error_report_persisted_queue_t)==6364);
 assert(nuwax_device_error_report_init()==0);assert(s_persist_snapshot);
 reset_state();fill(16);persist_snapshot_if_dirty();
 assert(blob_size==1612&&s_count==16&&!s_persist_dirty&&!s_persist_busy);
 memset(s_events,0,sizeof(s_events));assert(load_persisted_queue()==0&&s_count==4&&s_head==0);
 for(unsigned i=0;i<4;i++)assert(s_events[i].generation==i+12&&!s_events[i].in_flight&&!s_events[i].attempts);
 blob[100]^=1;assert(load_persisted_queue()==ESP_ERR_INVALID_SIZE);blob[100]^=1;
 --blob_size;assert(load_persisted_queue()==ESP_ERR_INVALID_SIZE);++blob_size;
 ((error_report_persisted_queue_t*)blob)->count=0xffffffff;assert(load_persisted_queue()==ESP_ERR_INVALID_SIZE);
 reset_state();fill(1);persist_snapshot_if_dirty();assert(blob_size==424);
 assert(load_persisted_queue()==0&&s_count==1);
 reset_state();fill(2);strcpy(s_events[13].severity,"warning");strcpy(s_events[14].severity,"warning");
 int before=writes;persist_snapshot_if_dirty();assert(writes==before&&blob_size==0);
 reset_state();fill(3);guard_error=-1;before=writes;int old_ends=end_calls;
 persist_snapshot_if_dirty();assert(writes==before&&end_calls==old_ends&&!s_persist_busy&&s_persist_dirty);
 guard_error=0;clock_us+=5000001;persist_snapshot_if_dirty();assert(blob_size==1216);
 before=writes;assert(nuwax_device_error_report_release_persisted_storage()==0);
 assert(blob_size==0&&s_count==3&&s_persist_storage_blocked);
 s_persist_dirty=true;clock_us+=100000000;persist_snapshot_if_dirty();assert(writes==before&&s_count==3);
 reset_state();fill(1);write_error=ESP_ERR_NVS_NOT_ENOUGH_SPACE;before=writes;
 persist_snapshot_if_dirty();assert(writes==before+1&&s_persist_storage_blocked&&s_count==1);
 clock_us+=1000000000;persist_snapshot_if_dirty();assert(writes==before+1);
 reset_state();fill(1);mutation_on_guard=true;before=writes;persist_snapshot_if_dirty();
 assert(writes==before&&!s_persist_busy&&s_persist_storage_blocked);
 /* Real legacy layout, circular head and warning filter; migrate on next write. */
 reset_state();error_report_persisted_queue_t legacy={0};legacy.magic=ERROR_REPORT_PERSIST_MAGIC;
 legacy.version=2;legacy.head=15;legacy.count=2;legacy.events[15].occupied=true;
 strcpy(legacy.events[15].severity,"error");strcpy(legacy.events[15].event_id,"legacy2");
 legacy.events[0].occupied=true;strcpy(legacy.events[0].severity,"warning");legacy.crc32=persisted_crc(&legacy);
 memcpy(blob,&legacy,sizeof(legacy));blob_size=sizeof(legacy);
 assert(load_persisted_queue()==0&&s_count==1&&s_persist_dirty);
 assert(!strcmp(s_events[0].event_id,"legacy2"));persist_snapshot_if_dirty();assert(blob_size==424);
 reset_state();error_report_persisted_queue_v1_t old={0};old.magic=ERROR_REPORT_PERSIST_MAGIC;old.version=1;old.head=8;old.count=1;
 old.events[8].occupied=true;strcpy(old.events[8].severity,"error");strcpy(old.events[8].result,"legacy short result");old.crc32=persisted_crc_v1(&old);
 memcpy(blob,&old,sizeof(old));blob_size=sizeof(old);
 assert(load_persisted_queue()==0&&s_count==1&&s_persist_dirty);
 assert(!strcmp(s_events[0].result,"legacy short result"));persist_snapshot_if_dirty();assert(blob_size==424);
 reset_state();fill(2);before=writes;storage_busy=true;persist_snapshot_if_dirty();storage_busy=false;
 replay=true;persist_snapshot_if_dirty();replay=false;deferred=true;persist_snapshot_if_dirty();assert(writes==before);
 reset_state();fill(1);available=1;before=guard_calls;int previous_writes=writes;
 persist_snapshot_if_dirty();assert(guard_calls==before&&writes==previous_writes&&s_count==1&&s_persist_storage_blocked);
 puts("PASS: production compact persistence 424/1612 bytes, last four errors, RAM queue retained, CRC/size/count rejection, v1/v2 migration, guard/no-space/reclaim races, audio/UI deferral");
 free(s_persist_snapshot);
}
'''
c=out/'check.c';c.write_text(pre+s+checks,encoding='utf-8');exe=out/'check.exe'
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',str(c),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
