#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "board/sd_dma_bulk.h"
#include "board/sd_io_memory_budget.h"
static uintptr_t probe_task = 1U;
static uintptr_t xTaskGetCurrentTaskHandle(void) { return probe_task; }
static atomic_uint_fast32_t s_bulk_commands, s_bulk_sectors, s_fallback_sectors;
static atomic_uint_fast32_t s_command_us, s_copy_us, s_allocation_us;
static atomic_uint_fast32_t s_native_calls, s_native_sectors;
static atomic_uint_fast32_t s_read_calls, s_read_sectors, s_read_us, s_read_errors;
static atomic_uint_fast32_t s_read_le2ms, s_read_le5ms, s_read_le10ms, s_read_le20ms, s_read_gt20ms;
static uint32_t clock_step=1;
static int64_t esp_timer_get_time(void) {static int64_t us;us+=clock_step;return us;}
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
typedef int esp_err_t;
typedef struct { struct {size_t sector_size, capacity;} csd; } sdmmc_card_t;
static uint8_t external[32*512], disk[64*512];
static size_t free_bytes=65536, allocations, calls, largest_count, fail_call;
static bool alloc_fail, bad_dma;
static bool drop_writes, corrupt_reads;
static size_t allocated_bytes;
static bool pressure_once;
static bool esp_ptr_external_ram(const void *p) {
    return (uintptr_t)p >= (uintptr_t)external && (uintptr_t)p < (uintptr_t)(external+sizeof(external));
}
static size_t heap_caps_get_free_size(int caps) {(void)caps;return free_bytes;}
static void *heap_caps_malloc(size_t n,int caps) {
    assert(caps==(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL));assert(n<=16384);
    assert(free_bytes>=n+16384);if(alloc_fail)return NULL;
    allocations++;allocated_bytes=n;free_bytes-=n;
    if(pressure_once){free_bytes-=35000;pressure_once=false;}
    return malloc(n);
}
static void heap_caps_free(void *p) {assert(p&&allocations);allocations--;free_bytes+=allocated_bytes;allocated_bytes=0;free(p);}
static int transfer(sdmmc_card_t *c,void *p,size_t sector,size_t count,bool write) {
    assert(c && sector+count<=c->csd.capacity);calls++;
    if(count>largest_count)largest_count=count;
    if(esp_ptr_external_ram(p))bad_dma=true;
    if(calls==fail_call)return ESP_FAIL;
    if(write){if(!drop_writes)memcpy(disk+sector*512,p,count*512);}
    else {memcpy(p,disk+sector*512,count*512);if(corrupt_reads)((uint8_t*)p)[0]^=0x80U;}
    return ESP_OK;
}
static int __real_sdmmc_read_sectors(sdmmc_card_t *c,void *p,size_t s,size_t n) {return transfer(c,p,s,n,false);}
static int __real_sdmmc_write_sectors(sdmmc_card_t *c,const void *p,size_t s,size_t n) {return transfer(c,(void *)p,s,n,true);}
#include "sd_dma_bulk_host.inc"
static void reset(void) {assert(!allocations);calls=largest_count=fail_call=0;bad_dma=alloc_fail=drop_writes=corrupt_reads=false;free_bytes=65536;}
int main(void) {
    sdmmc_card_t card={{512,64}};
    for(size_t i=0;i<sizeof(disk);i++)disk[i]=(uint8_t)(i*31+i/512);
    reset();assert(__wrap_sdmmc_read_sectors(&card,external,3,32)==ESP_OK);
    assert(calls==1 && largest_count==32 && !bad_dma && !allocations);
    assert(memcmp(external,disk+3*512,sizeof(external))==0);
    assert(atomic_load(&s_command_us)>0 && atomic_load(&s_copy_us)>0 && atomic_load(&s_allocation_us)>0);
    memset(external,0xa7,sizeof(external));reset();
    assert(__wrap_sdmmc_write_sectors(&card,external,7,31)==ESP_OK);
    assert(calls==2 && !bad_dma && memcmp(external,disk+7*512,31*512)==0);
    reset();free_bytes=16384+2048;assert(__wrap_sdmmc_read_sectors(&card,external,0,32)==ESP_OK);
    assert(calls==8 && largest_count==4 && !bad_dma);
    reset();free_bytes=16384;assert(__wrap_sdmmc_read_sectors(&card,external,0,32)==ESP_OK);
    assert(calls==1 && bad_dma && !allocations); // Delegate to original SDK fallback.
    reset();alloc_fail=true;assert(__wrap_sdmmc_read_sectors(&card,external,0,32)==ESP_OK);
    assert(calls==1 && bad_dma);
    reset();free_bytes=20480;memset(external,0xcc,sizeof(external));fail_call=2;
    assert(__wrap_sdmmc_read_sectors(&card,external,0,32)==ESP_FAIL && calls==2 && !allocations);
    for(size_t i=4096;i<sizeof(external);i++)assert(external[i]==0xcc);
    reset();free_bytes=20480;fail_call=2;assert(__wrap_sdmmc_write_sectors(&card,external,0,32)==ESP_FAIL && calls==2 && !allocations);
    reset();assert(__wrap_sdmmc_write_sectors(&card,external,60,32)==ESP_ERR_INVALID_SIZE && !calls);
    reset();free_bytes=22000;assert(__wrap_sdmmc_read_sectors(&card,external,0,32)==ESP_OK);
    assert(calls==4 && largest_count==8 && free_bytes==22000);
    reset();pressure_once=true;assert(__wrap_sdmmc_read_sectors(&card,external,0,32)==ESP_OK);
    assert(calls==2 && largest_count==16 && free_bytes>=16384);
    uint8_t direct[512];reset();assert(__wrap_sdmmc_read_sectors(&card,direct,0,1)==ESP_OK && calls==1 && !allocations);
    assert(s_native_calls==1 && s_native_sectors==1);
    const uint32_t reads=s_read_calls, sectors=s_read_sectors, errors=s_read_errors;
    const uint32_t b0=s_read_le2ms,b1=s_read_le5ms,b2=s_read_le10ms,b3=s_read_le20ms,b4=s_read_gt20ms;
    const uint32_t durations[]={2000,2001,5000,5001,10000,10001,20000,20001};
    for(unsigned i=0;i<8;++i){clock_step=durations[i];reset();
        assert(__wrap_sdmmc_read_sectors(&card,direct,0,1)==ESP_OK);}
    assert(s_read_calls==reads+8 && s_read_sectors==sectors+8);
    assert(s_read_le2ms==b0+1 && s_read_le5ms==b1+2 && s_read_le10ms==b2+2 &&
           s_read_le20ms==b3+2 && s_read_gt20ms==b4+1);
    reset();fail_call=1;assert(__wrap_sdmmc_read_sectors(&card,direct,0,1)==ESP_FAIL);
    assert(s_read_errors==errors+1);const uint32_t only_reads=s_read_calls;
    reset();assert(__wrap_sdmmc_write_sectors(&card,direct,0,1)==ESP_OK);
    assert(s_read_calls==only_reads);
    reset();nuwax_sd_io_probe_t probe, other;
    assert(!nuwax_sd_io_probe_begin(NULL));assert(nuwax_sd_io_probe_begin(&probe));
    assert(!nuwax_sd_io_probe_begin(&other));memset(external,0x37,1024);
    assert(__wrap_sdmmc_write_sectors(&card,external,2,2)==ESP_OK);
    memset(external,0,1024);assert(__wrap_sdmmc_read_sectors(&card,external,2,2)==ESP_OK);
    assert(probe.writes==1&&probe.reads==1&&probe.read_matches==2&&!probe.read_mismatches);
    probe_task=2U;assert(__wrap_sdmmc_read_sectors(&card,external,2,2)==ESP_OK);
    nuwax_sd_io_probe_end();assert(probe.reads==1&&atomic_load(&s_probe_owner)==1);
    probe_task=1U;drop_writes=true;memset(external,0x91,1024);
    assert(__wrap_sdmmc_write_sectors(&card,external,2,2)==ESP_OK);
    memset(external,0,1024);assert(__wrap_sdmmc_read_sectors(&card,external,2,2)==ESP_OK);
    assert(probe.read_mismatches==2&&probe.first_bad_sector==2&&probe.first_expected_crc!=probe.first_read_crc&&!probe.sdk_errors);
    nuwax_sd_io_probe_end();assert(!s_probe&&!atomic_load(&s_probe_owner));
    assert(nuwax_sd_io_probe_begin(&probe));drop_writes=false;
    assert(__wrap_sdmmc_write_sectors(&card,external,2,2)==ESP_OK);
    fail_call=calls+1;assert(__wrap_sdmmc_write_sectors(&card,external,2,2)==ESP_FAIL);
    fail_call=0;assert(__wrap_sdmmc_read_sectors(&card,external,2,2)==ESP_OK);
    assert(probe.sdk_errors==1&&!probe.read_matches&&!probe.read_mismatches);
    nuwax_sd_io_probe_end();
    puts("PASS: scratch sector witness detects successful-but-lost writes at exact LBA; matched read/write, task isolation, nested scope, failed-write ambiguity and cleanup");
    reset();clock_step=1;assert(!nuwax_sd_bulk_read_recovery_begin(0));
    assert(nuwax_sd_bulk_read_recovery_begin(1500));assert(!nuwax_sd_bulk_read_recovery_begin(1500));
    probe_task=2; nuwax_sd_bulk_read_recovery_end(true);assert(!nuwax_sd_bulk_read_is_degraded());
    probe_task=1;assert(__wrap_sdmmc_read_sectors(&card,external,0,32)==ESP_OK&&calls==32&&largest_count==1&&!allocations);
    nuwax_sd_bulk_read_recovery_end(false);assert(!nuwax_sd_bulk_read_is_degraded());
    reset();assert(nuwax_sd_bulk_read_recovery_begin(1));clock_step=2000;
    assert(__wrap_sdmmc_read_sectors(&card,external,0,32)==ESP_ERR_TIMEOUT&&!calls&&!allocations);
    clock_step=1;nuwax_sd_bulk_read_recovery_end(false);
    reset();assert(nuwax_sd_bulk_read_recovery_begin(1500));free_bytes=16384;
    assert(__wrap_sdmmc_read_sectors(&card,external,0,1)==ESP_OK&&calls==1&&!allocations&&free_bytes==16384);
    assert(!atomic_flag_test_and_set(&s_read_reserve_owner));
    assert(__wrap_sdmmc_read_sectors(&card,external,0,1)==ESP_ERR_NO_MEM&&calls==1&&!allocations);
    atomic_flag_clear(&s_read_reserve_owner);
    nuwax_sd_bulk_read_recovery_end(false);
    reset();assert(nuwax_sd_bulk_read_recovery_begin(1500));
    assert(__wrap_sdmmc_read_sectors(&card,external,0,32)==ESP_OK);
    nuwax_sd_bulk_read_recovery_end(true);assert(nuwax_sd_bulk_read_is_degraded());
    reset();assert(__wrap_sdmmc_read_sectors(&card,external,0,32)==ESP_OK&&calls==32&&largest_count==1&&!allocations);
    assert(!memcmp(external,disk,sizeof(external)));
    puts("PASS independent reads: owner fence, nested rejection, deadline and memory floor, exact single-sector data, compatibility only after original validation");
    puts("PASS: production DMA adapter read/write, sector offsets/tail, adaptive 2/4/8/16 KiB commands and post-allocation reserve, low-heap/allocation fallback, I/O failure stop/cleanup, range validation, direct DMA path");
}
