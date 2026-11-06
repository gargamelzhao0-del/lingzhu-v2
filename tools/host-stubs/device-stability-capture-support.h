#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "offline_capture_health.h"
typedef int esp_err_t;
typedef int BaseType_t;
typedef int nuwax_offline_audio_mode_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_NOT_FINISHED 3
#define ESP_ERR_INVALID_STATE 4
#define NUWAX_OFFLINE_AUDIO_MODE_MEETING 0
#define NUWAX_OFFLINE_AUDIO_MODE_MONITORING 1
#define pdTRUE 1
#define pdPASS 1
#define DONE_BIT 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define OFFLINE_CAPTURE_TASK_STACK_BYTES 8192
#define OFFLINE_CAPTURE_TASK_PRIORITY 4
#define OFFLINE_CAPTURE_TASK_CORE 1
#define NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES (16ULL*1024ULL*1024ULL)
#define CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES (1024U*1024U)
#define ARCHIVE_QUOTA_BYTES (1536ULL*1024ULL*1024ULL)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static struct {
    int admission_lock, lock, capture_events;
    void *capture_task;
    atomic_bool media_recovery_requested, capture_running, capture_stop, capture_complete;
    uint64_t used_bytes;
} s;
static atomic_bool s_media_ready, s_space_blocked, s_shutdown_requested;
static atomic_uint s_mirror_closers,s_capture_cancel_epoch,s_next_capture_sequence;
static atomic_int s_capture_health;
static atomic_bool s_offline_limit_reached;
static char s_limit_session[48];
static uint64_t s_limit_used_us;
static bool initialized=true, powered_down, cancel_during_alloc;
static unsigned allocations, launches;
static void *launched_config;
static uint64_t fake_free_bytes;
static esp_err_t capacity_error;
static esp_err_t esp_vfs_fat_info(const char *path,uint64_t *total,uint64_t *free_bytes){
    assert(!strcmp(path,"/sdcard"));*total=32ULL*1024ULL*1024ULL*1024ULL;
    *free_bytes=fake_free_bytes;return capacity_error;
}
static bool enabled(void){return true;}
static bool runtime_initialized(void){return initialized;}
static bool valid_session_id(const char *id){return id && id[0];}
static bool valid_audio_geometry(uint32_t rate,uint8_t ch,uint16_t ms,void *out){(void)out;return rate==16000 && ch==1 && ms==20;}
static bool nuwax_power_ctrl_shutdown_preparing(void){return powered_down;}
static bool nuwax_offline_audio_limit_reached(void){return atomic_load(&s_offline_limit_reached);}
static int xSemaphoreTake(int sem,unsigned ticks){(void)ticks;return sem;}
static void xSemaphoreGive(int sem){assert(sem);}
static void xEventGroupClearBits(int events,unsigned bits){(void)events;(void)bits;}
static void offline_capture_task(void *arg){(void)arg;}
esp_err_t nuwax_offline_audio_request_stop(bool complete);
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps){
    (void)caps;allocations++;
    if(cancel_during_alloc)nuwax_offline_audio_request_stop(false);
    return calloc(n,size);
}
static void heap_caps_free(void *ptr){free(ptr);}
static int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *),const char *name,
    unsigned stack,void *arg,unsigned priority,void **handle,int core,unsigned caps){
    (void)fn;(void)name;(void)stack;(void)priority;(void)handle;(void)core;(void)caps;
    launched_config=arg;launches++;return pdPASS;
}
