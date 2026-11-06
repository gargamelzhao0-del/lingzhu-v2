#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include "board/sd_probe_trace.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_CRC 0x109
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define SD_OCR_SDHC_CAP (1U<<30)
typedef struct {uint32_t opcode,arg,response[4],timeout_ms;void*data;size_t datalen;} sdmmc_command_t;
typedef struct {struct {size_t sector_size,capacity;}csd;
    struct{uint32_t mfg_id,oem_id,serial;}cid;uint32_t max_freq_khz,ocr;bool is_mmc;
    struct{esp_err_t(*do_transaction)(int,sdmmc_command_t*);uint32_t command_timeout_ms;}host;} sdmmc_card_t;
static uintptr_t task=1;
static uintptr_t xTaskGetCurrentTaskHandle(void){return task;}
static int64_t now_us;
static int64_t esp_timer_get_time(void){return ++now_us;}
static unsigned allocations,allocation_calls,fail_allocation;
static size_t internal_free=65536;
static bool allocation_pressure;
static struct {void*p;int caps;} memory[16];
static void*heap_caps_malloc(size_t n,int caps){
    if(++allocation_calls==fail_allocation)return NULL;
    void*p=malloc(n);assert(p);if(caps&MALLOC_CAP_INTERNAL){internal_free-=n;if(allocation_pressure)internal_free=8192;}
    for(unsigned i=0;i<16;i++)if(!memory[i].p){memory[i].p=p;memory[i].caps=caps;allocations++;return p;}
    assert(!"allocation slots exhausted");return NULL;
}
static void*heap_caps_calloc(size_t n,size_t bytes,int caps){void*p=heap_caps_malloc(n*bytes,caps);if(p)memset(p,0,n*bytes);return p;}
static size_t heap_caps_get_free_size(int caps){assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return internal_free;}
static void heap_caps_free(void*p){if(!p)return;for(unsigned i=0;i<16;i++)if(memory[i].p==p){if(memory[i].caps&MALLOC_CAP_INTERNAL)internal_free+=1024;memory[i].p=NULL;--allocations;free(p);return;}assert(!"foreign allocation");}
static bool esp_ptr_external_ram(const void*p){for(unsigned i=0;i<16;i++)if(memory[i].p==p)return(memory[i].caps&MALLOC_CAP_SPIRAM)!=0;return false;}
static const char*esp_err_to_name(int e){return e==ESP_OK?"ESP_OK":e==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":"ESP_ERR_INVALID_CRC";}
static uint32_t esp_rom_crc32_le(uint32_t crc,const void*data,uint32_t n){
    const uint8_t*p=data;crc=~crc;while(n--){crc^=*p++;for(unsigned b=0;b<8;b++)crc=(crc>>1)^((crc&1)?0xedb88320U:0U);}return~crc;
}
static char logs[32768];static size_t logged;
static void log_event(const char*tag,const char*format,...){
    (void)tag;va_list args;va_start(args,format);int n=vsnprintf(logs+logged,sizeof(logs)-logged,format,args);va_end(args);
    assert(n>=0&&(size_t)n+2<sizeof(logs)-logged);logged+=(size_t)n;logs[logged++]='\n';logs[logged]=0;
}
#define ESP_LOGW(...) log_event(__VA_ARGS__)
static unsigned raw_reads,transactions;static bool busy,untouched_read;
static void put16(uint8_t*p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static esp_err_t host_transaction(int slot,sdmmc_command_t*cmd){
    (void)slot;transactions++;assert(cmd->timeout_ms&&cmd->timeout_ms<=200);now_us+=busy?cmd->timeout_ms*1000U:100U;
    cmd->response[0]=busy?0:0x900;return ESP_OK;
}
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t*card,void*buffer,size_t lba,size_t count){
    assert((count==1||count==2)&&card->host.do_transaction!=host_transaction);raw_reads++;
    sdmmc_command_t cmd={.opcode=count==1?17:18,.arg=(uint32_t)lba,.data=buffer,.datalen=count*512,.timeout_ms=200};
    esp_err_t err;
    do{err=card->host.do_transaction(0,&cmd);if(err!=ESP_OK)break;}while(busy);
    if(err==ESP_OK&&!untouched_read){
        uint8_t*p=buffer;memset(p,0,count*512);
        if(lba==0){p[450]=0x0b;put32(p+454,2048);p[510]=0x55;p[511]=0xaa;}
        else if(lba==2048){p[0]=0xeb;put16(p+11,512);p[13]=64;put16(p+14,32);p[16]=2;put32(p+32,8000000);put32(p+36,1000);put16(p+48,1);p[510]=0x55;p[511]=0xaa;}
        else if(lba==2049){put32(p,0x41615252);put32(p+484,0x61417272);put32(p+488,123);put32(p+492,456);put32(p+508,0xaa550000);}
        else{assert(lba==1492384);memset(p,0x31,count*512);}
    }
    nuwax_sd_probe_trace_command(card,cmd.opcode,(uint32_t)lba,cmd.response[0],err,buffer,count*512);
    return err;
}
#include "sd_probe_trace_host.inc"
static sdmmc_card_t card={.csd={512,8388608},.cid={1,2,3},.max_freq_khz=40000,
    .ocr=SD_OCR_SDHC_CAP,
    .host={.do_transaction=host_transaction,.command_timeout_ms=1234}};
static void reset(void){
    assert(!allocations&&!s_trace&&!atomic_load(&s_owner));atomic_store(&s_diagnosed,false);
    allocation_calls=fail_allocation=raw_reads=transactions=0;logged=0;logs[0]=0;busy=untouched_read=false;task=1;now_us=0;
    internal_free=65536;allocation_pressure=false;
}
static void record(void){nuwax_sd_probe_trace_command(&card,17,1492384,0x900,ESP_OK,NULL,512);}
static void failed(void){nuwax_sd_probe_trace_finish("metadata",ESP_ERR_INVALID_CRC,1492384,1,2);}
int main(void){
    reset();record();assert(!nuwax_sd_probe_trace_active()&&!allocation_calls&&!raw_reads&&!logged);
    fail_allocation=1;errno=EBUSY;assert(!nuwax_sd_probe_trace_begin()&&errno==EBUSY);assert(!atomic_load(&s_owner));
    reset();assert(nuwax_sd_probe_trace_begin());assert(nuwax_sd_probe_trace_active());assert(!nuwax_sd_probe_trace_begin());
    task=2;record();failed();assert(!s_trace->events&&!raw_reads&&allocations==1);task=1;
    record();errno=ERANGE;nuwax_sd_probe_trace_finish("bulk",ESP_OK,0,0,0);assert(errno==ERANGE&&!allocations&&!raw_reads&&!logged);
    assert(!atomic_load(&s_diagnosed));
    reset();assert(nuwax_sd_probe_trace_begin());record();errno=EAGAIN;
    nuwax_sd_probe_trace_finish("logical_pass_witness_conflict",ESP_OK,1492384,1,2);
    assert(errno==EAGAIN&&raw_reads==6&&!allocations&&atomic_load(&s_diagnosed));
    assert(strstr(logs,"logical_pass_witness_conflict"));
    reset();assert(nuwax_sd_probe_trace_begin());record();
    nuwax_sd_probe_trace_finish("matching_witness",ESP_OK,1492384,7,7);
    assert(!raw_reads&&!logged&&!allocations&&!atomic_load(&s_diagnosed));
    reset();assert(nuwax_sd_probe_trace_begin());for(unsigned i=0;i<80;i++)record();
    errno=EACCES;failed();assert(errno==EACCES&&raw_reads==6&&!allocations);
    assert(strstr(logs,"seq=0 ")&&strstr(logs,"seq=79 ")&&!strstr(logs,"seq=9 "));
    assert(strstr(logs,"path=native_internal_1")&&strstr(logs,"path=native_internal_2")&&strstr(logs,"path=native_psram"));
    assert(strstr(logs,"BPB revision=206 valid=1")&&strstr(logs,"FSInfo revision=206 valid=1"));
    assert(card.host.do_transaction==host_transaction&&card.host.command_timeout_ms==1234);
    const unsigned old_allocations=allocation_calls;assert(!nuwax_sd_probe_trace_begin());failed();assert(raw_reads==6&&allocation_calls==old_allocations);
    reset();assert(nuwax_sd_probe_trace_begin());
    nuwax_sd_probe_trace_command(&card,25,1492384,0x900,ESP_OK,NULL,1024);
    nuwax_sd_probe_trace_command(&card,13,1,0x4000900,ESP_OK,NULL,0);
    for(unsigned i=0;i<32;i++)nuwax_sd_probe_trace_command(&card,25,1492400U+2U*i,0x900,ESP_OK,NULL,1024);
    for(unsigned i=0;i<80;i++)record();
    failed();
    assert(raw_reads==7&&strstr(logs,"path=native_internal_multi")&&strstr(logs,"first_status=04000900")&&!allocations);
    reset();assert(nuwax_sd_probe_trace_begin());card.ocr=0;
    nuwax_sd_probe_trace_command(&card,25,1492384U*512U,0x900,ESP_OK,NULL,1024);
    failed();assert(raw_reads==7&&strstr(logs,"path=native_internal_multi"));card.ocr=SD_OCR_SDHC_CAP;
    reset();assert(nuwax_sd_probe_trace_begin());
    nuwax_sd_probe_trace_command(&card,25,1492383,0x900,ESP_OK,NULL,1024);
    failed();assert(raw_reads==6&&!strstr(logs,"path=native_internal_multi"));
    reset();assert(nuwax_sd_probe_trace_begin());
    nuwax_sd_probe_trace_command(&card,25,1492384,0x900,ESP_ERR_INVALID_CRC,NULL,1024);
    failed();assert(raw_reads==6&&!strstr(logs,"path=native_internal_multi"));
    reset();assert(nuwax_sd_probe_trace_begin());record();busy=true;failed();assert(raw_reads==1&&transactions<=4&&now_us<752000&&!allocations);
    reset();assert(nuwax_sd_probe_trace_begin());record();untouched_read=true;failed();assert(strstr(logs,"untouched=1")&&!allocations);
    for(unsigned n=2;n<=4;n++){reset();assert(nuwax_sd_probe_trace_begin());record();fail_allocation=n;failed();assert(!allocations&&!s_trace&&!atomic_load(&s_owner));}
    reset();assert(nuwax_sd_probe_trace_begin());record();internal_free=16384U+1536U-1U;failed();assert(!raw_reads&&!allocations);
    reset();assert(nuwax_sd_probe_trace_begin());record();allocation_pressure=true;failed();assert(!raw_reads&&!allocations);
    reset();assert(nuwax_sd_probe_trace_begin());record();sdmmc_card_t foreign=card;
    nuwax_sd_probe_trace_command(&foreign,17,1492384,0x900,ESP_OK,NULL,512);failed();assert(!raw_reads&&!allocations&&strstr(logs,"foreign_card=1"));
    puts("PASS actual trace: inactive/healthy no I/O or logging, owner/nested isolation, first+tail bound, read-only native memory comparison, poison detection, BPB/FSInfo, deadline incl polling, unchanged card descriptor, errno, one attempt/boot, allocation cleanup");
}
