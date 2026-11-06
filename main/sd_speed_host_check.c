#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "board/sd_probe_pattern.h"
#include "board/sd_dma_bulk.h"
bool nuwax_sd_io_probe_begin(nuwax_sd_io_probe_t *p) { (void)p; return false; }
void nuwax_sd_io_probe_end(void) { assert(!"unowned witness end"); }
static uint32_t random_state=1;
static uint32_t esp_random(void) {random_state=random_state*1664525U+1013904223U;return random_state;}
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_RESPONSE 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_INVALID_CRC 5
#define ESP_ERR_INVALID_SIZE 6
typedef void *SemaphoreHandle_t;
typedef int StaticSemaphore_t;
typedef int portMUX_TYPE;
typedef unsigned TickType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
static int lock_depth;
static void *xSemaphoreCreateMutexStatic(void *p) {return p;}
static int xSemaphoreTake(void *s,unsigned t) {(void)s;(void)t;assert(lock_depth++==0);return 1;}
static void xSemaphoreGive(void *s) {(void)s;assert(--lock_depth==0);}
static int64_t esp_timer_get_time(void) {static int64_t t;return ++t;}
static const char *esp_err_to_name(int e) {(void)e;return "stub";}
static void log_stub(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static bool alloc_fail;
static bool faults_high_only;
static bool qualifier_fault(bool fault);
static unsigned allocations;
static void *heap_caps_malloc(size_t n,int caps) {(void)caps;if(qualifier_fault(alloc_fail))return NULL;allocations++;return malloc(n);}
static void heap_caps_free(void *p) {if(p){assert(allocations);allocations--;free(p);}}
#define SDMMC_FREQ_DEFAULT 20000
#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP 1
#define GPIO_NUM_NC -1
#define NUWAX_VOCAT_SD_CLK_GPIO 1
#define NUWAX_VOCAT_SD_CMD_GPIO 2
#define NUWAX_VOCAT_SD_D0_GPIO 3
typedef struct {int max_freq_khz,slot;} sdmmc_host_t;
typedef struct {int width,clk,cmd,d0,d1,d2,d3,flags;} sdmmc_slot_config_t;
#define SDMMC_HOST_DEFAULT() ((sdmmc_host_t){20000,1})
#define SDMMC_SLOT_CONFIG_DEFAULT() ((sdmmc_slot_config_t){0})
typedef struct {bool format_if_mount_failed;int max_files,allocation_unit_size;} esp_vfs_fat_sdmmc_mount_config_t;
typedef struct {struct {size_t capacity,sector_size;} csd;struct {char name[8];uint32_t mfg_id,oem_id,revision,serial,date;} cid;int max_freq_khz;sdmmc_host_t host;} sdmmc_card_t;
typedef struct {bool mounted;uint64_t capacity_bytes;uint32_t sector_size,max_frequency_khz;} nuwax_sd_card_info_t;
typedef struct {uint8_t bytes[40];} nuwax_sd_card_identity_t;
static sdmmc_card_t fake_card;
static bool qualifier_fault(bool fault) { return fault && (!faults_high_only || fake_card.max_freq_khz > 20000); }
static unsigned mounts,unmounts;static int speeds[8];
static bool alive,fail_high_mount,fail_low_mount,fail_unmount,card_slow,probe_fail;
static int esp_vfs_fat_sdmmc_mount(const char *p,const sdmmc_host_t *h,const sdmmc_slot_config_t *s,const esp_vfs_fat_sdmmc_mount_config_t *m,sdmmc_card_t **c) {
    (void)p;assert(!alive && lock_depth==1 && !m->format_if_mount_failed && s->width==1);
    assert(mounts<8);speeds[mounts++]=h->max_freq_khz;
    if ((h->max_freq_khz>20000 && fail_high_mount) || (h->max_freq_khz==20000 && fail_low_mount)) return ESP_FAIL;
    fake_card=(sdmmc_card_t){.csd={100000,512},.cid={.name="TEST",.mfg_id=1,.oem_id=2,.revision=3,.serial=4,.date=5},.max_freq_khz=card_slow?20000:h->max_freq_khz,.host=*h};
    alive=true;*c=&fake_card;return ESP_OK;
}
static int esp_vfs_fat_sdcard_unmount(const char *p,sdmmc_card_t *c) {(void)p;assert(alive && c==&fake_card);alive=false;unmounts++;return fail_unmount?ESP_FAIL:ESP_OK;}
static int sdmmc_host_get_real_freq(int slot,int *f) {(void)slot;*f=fake_card.max_freq_khz;return ESP_OK;}
static int esp_vfs_fat_info(const char *p,uint64_t *t,uint64_t *f) {(void)p;*t=100000;*f=80000;return probe_fail?ESP_FAIL:ESP_OK;}
static int test_stat(const char *p,struct stat *s) {(void)p;memset(s,0,sizeof(*s));s->st_mode=S_IFDIR;return 0;}
static uint8_t file_data[65536];static size_t pos,file_size;
static int open_fd;static bool exists,collision,write_short,read_short,corrupt,fail_sync,fail_close,fail_unlink;
static bool ignore_payload_writes;
static char current_file[128];
static int test_open(const char *p,int flags,...) {
    assert(alive && !open_fd && lock_depth==1);
    if(flags&O_CREAT){assert(flags&O_EXCL);if((exists&&!strcmp(current_file,p))||qualifier_fault(collision))return -1;strcpy(current_file,p);exists=true;file_size=0;open_fd=1;}
    else {assert(exists);open_fd=2;}pos=0;return open_fd;
}
static int test_write(int f,const void *p,unsigned n){assert(f==1 && open_fd==f && pos+n<=sizeof(file_data));if(qualifier_fault(write_short))return 0;if(!ignore_payload_writes)memcpy(file_data+pos,p,n);pos+=n;file_size=pos;return (int)n;}
static int test_read(int f,void *p,unsigned n){assert(f==2 && open_fd==f);if(qualifier_fault(read_short))return 0;assert(pos+n<=file_size);memcpy(p,file_data+pos,n);pos+=n;if(qualifier_fault(corrupt))((uint8_t*)p)[0]^=1;return (int)n;}
static int test_close(int f){assert(f==open_fd);open_fd=0;return qualifier_fault(fail_close)?-1:0;}
static int test_fsync(int f){assert(f==open_fd);return qualifier_fault(fail_sync)?-1:0;}
static int test_unlink(const char *p){(void)p;assert(!open_fd && exists);if(qualifier_fault(fail_unlink))return -1;exists=false;return 0;}
#define open test_open
#define read test_read
#define write test_write
#define close test_close
#define fsync test_fsync
#define unlink test_unlink
#define stat(...) test_stat(__VA_ARGS__)
#include "sd_speed_driver_host.inc"
static void reset(void) {
    assert(!lock_depth && !allocations && !open_fd);
    alive=s_mounted=s_healthy=s_highspeed_disabled=false;s_card=NULL;
    s_media_detected=false; faults_high_only=false; current_file[0]=0;
    s_media_error=ESP_OK;ignore_payload_writes=false;
    s_fallback_stage=NULL;s_fallback_error=s_fallback_errno=0;s_fallback_at_ms=0;
    mounts=unmounts=0;memset(speeds,0,sizeof(speeds));
    fail_high_mount=fail_low_mount=fail_unmount=card_slow=probe_fail=false;
    exists=collision=write_short=read_short=corrupt=fail_sync=fail_close=fail_unlink=alloc_fail=false;
}
int main(void) {
    reset();nuwax_sd_card_info_t info={0};
    nuwax_sd_card_identity_t identity, changed;
    assert(!nuwax_sd_card_get_identity(&identity) && !lock_depth);
    assert(nuwax_sd_card_mount(&info)==ESP_OK && info.mounted && mounts==1);
    assert(nuwax_sd_card_get_identity(&identity) && !lock_depth);
    uint32_t serial;memcpy(&serial,identity.bytes+12U,4U);assert(serial==4U);
    ++fake_card.cid.serial;assert(nuwax_sd_card_get_identity(&changed) && memcmp(&identity,&changed,sizeof(identity)));--fake_card.cid.serial;
    ++fake_card.csd.capacity;assert(nuwax_sd_card_get_identity(&changed) && memcmp(&identity,&changed,sizeof(identity)));--fake_card.csd.capacity;
    assert(nuwax_sd_card_get_identity(&changed) && !memcmp(&identity,&changed,sizeof(identity)));
    puts("PASS: production card identity is stable under healthy probes and distinguishes serial/capacity changes, never valid before mount");
#if CONFIG_NUWAX_SD_CARD_HIGHSPEED_ENABLE
    assert(speeds[0]==40000 && file_size==65536 && !exists && !open_fd && !allocations);
    assert(nuwax_sd_card_is_mounted());assert(nuwax_sd_card_mount(&info)==ESP_OK && mounts==1);
    nuwax_sd_card_mark_io_fault(ESP_FAIL);
    assert(s_highspeed_disabled && !nuwax_sd_card_is_mounted() && unmounts==0 && alive);
    assert(strcmp(s_fallback_stage,"runtime_io")==0);
    assert(nuwax_sd_card_recover(100)==ESP_OK && unmounts==1 && mounts==2 && speeds[1]==20000);
    assert(!nuwax_sd_card_fallback_after_read_error(ESP_ERR_INVALID_CRC));
    assert(nuwax_sd_card_unmount()==ESP_OK);assert(nuwax_sd_card_mount(&info)==ESP_OK && speeds[2]==20000);
    reset();fail_high_mount=true;assert(nuwax_sd_card_mount(&info)==ESP_OK);
    assert(mounts==2 && unmounts==0 && speeds[1]==20000 && s_highspeed_disabled);
    bool *faults[]={&write_short,&read_short,&corrupt,&fail_sync,&fail_close,&fail_unlink,&alloc_fail,&collision};
    const char *stages[]={"qualification_write","qualification_read","qualification_compare","qualification_sync",
        "qualification_write_close","qualification_cleanup","qualification_alloc","qualification_create"};
    for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);++i){
        reset();faults_high_only=true;*faults[i]=true;assert(nuwax_sd_card_mount(&info)==ESP_OK);
        assert(mounts==2 && unmounts==1 && speeds[1]==20000 && s_highspeed_disabled);
        assert(!open_fd && !allocations);
        assert(strcmp(s_fallback_stage,stages[i])==0);
        const esp_err_t original=s_fallback_error;
        latch_highspeed_fallback("later_failure",ESP_ERR_TIMEOUT,99);
        assert(strcmp(s_fallback_stage,stages[i])==0 && s_fallback_error==original);
    }
    reset();corrupt=fail_unmount=true;assert(nuwax_sd_card_mount(&info)!=ESP_OK);
    assert(mounts==1 && unmounts==1 && !s_mounted && !s_card && s_highspeed_disabled);
    reset();fail_high_mount=fail_low_mount=true;assert(nuwax_sd_card_mount(&info)!=ESP_OK);
    assert(!s_mounted && !s_card && !s_healthy && mounts==2 && !nuwax_sd_card_media_detected());
    reset();card_slow=true;assert(nuwax_sd_card_mount(&info)==ESP_OK && info.max_frequency_khz==20000);
    assert(!nuwax_sd_card_fallback_after_read_error(ESP_ERR_INVALID_SIZE));
    reset();assert(nuwax_sd_card_mount(&info)==ESP_OK);
    assert(!nuwax_sd_card_fallback_after_read_error(ESP_ERR_TIMEOUT) && s_healthy);
    assert(nuwax_sd_card_fallback_after_read_error(ESP_ERR_INVALID_CRC));
    assert(!s_healthy && s_highspeed_disabled && alive && unmounts==0);
    assert(strcmp(s_fallback_stage,"archive_validation")==0);
    assert(nuwax_sd_card_recover(100)==ESP_OK && speeds[1]==20000);
    reset();assert(nuwax_sd_card_mount(&info)==ESP_OK);probe_fail=true;
    assert(nuwax_sd_card_probe()!=ESP_OK && s_highspeed_disabled && !s_healthy);
    assert(strcmp(s_fallback_stage,"health_fat_info")==0);
    reset();corrupt=fail_unlink=true;assert(nuwax_sd_card_mount(&info)!=ESP_OK);
    assert(s_fallback_error==ESP_ERR_INVALID_CRC && strcmp(s_fallback_stage,"qualification_compare")==0);
    assert(!s_mounted && !s_card && !s_healthy && nuwax_sd_card_media_detected() && unmounts==2 && !info.mounted);
    reset();corrupt=true;assert(nuwax_sd_card_mount(&info)==ESP_ERR_INVALID_CRC);
    assert(mounts==2 && unmounts==2 && !s_mounted && nuwax_sd_card_media_detected() && !open_fd && !allocations);
    corrupt=false;assert(nuwax_sd_card_recover(100)==ESP_OK && nuwax_sd_card_is_mounted() && speeds[2]==20000);
    puts("PASS: 20MHz readback also required; present bad media remains fenced, never cardless; healthy retry recovers without leaked files/allocations");
    puts("PASS: production SD 40MHz qualification, mount/read/write/CRC/sync/close/allocation/collision failures, cleanup, sticky 20MHz fallback, ordered runtime recovery, slow-card negotiation");
#else
    assert(speeds[0]==20000 && !s_highspeed_disabled && !exists && !allocations);
    nuwax_sd_card_mark_io_fault(ESP_FAIL);
    assert(nuwax_sd_card_recover(100)==ESP_OK && speeds[1]==20000 && !s_highspeed_disabled);
    assert(file_size==65536U);
    puts("PASS: production default 20MHz profile validates readback before storage admission");
#endif
    assert(!lock_depth && !allocations && !open_fd);
    reset();assert(nuwax_sd_card_media_error()==ESP_OK);
    assert(nuwax_sd_card_mount(&info)==ESP_OK);
    nuwax_sd_card_mark_io_fault(ESP_ERR_INVALID_CRC);
    lock_depth=1;assert(nuwax_sd_card_media_error()==ESP_ERR_INVALID_CRC&&lock_depth==1);lock_depth=0;
    assert(nuwax_sd_card_unmount()==ESP_OK&&nuwax_sd_card_media_error()==ESP_ERR_INVALID_CRC);
    assert(nuwax_sd_card_mount(&info)==ESP_OK&&nuwax_sd_card_media_error()==ESP_OK);
    assert(nuwax_sd_card_unmount()==ESP_OK);
    /* Return a previous successful probe's bytes while claiming every new
     * write succeeded. A fixed payload would falsely pass this remount. */
    ignore_payload_writes=true;
    assert(nuwax_sd_card_mount(&info)==ESP_ERR_INVALID_CRC);
    assert(!nuwax_sd_card_is_mounted()&&nuwax_sd_card_media_error()==ESP_ERR_INVALID_CRC);
    assert(nuwax_sd_card_media_detected()&&!allocations&&!open_fd);
    puts("PASS: successful-but-lost writes returning prior probe data fail fresh-challenge readback; lock-free fault publication survives unmount and clears on qualified mount");
    return 0;
}
