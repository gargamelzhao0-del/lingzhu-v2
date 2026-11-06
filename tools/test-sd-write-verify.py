"""Execute the production SD write verifier against deterministic silent-write faults."""
from pathlib import Path
import ctypes,os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];out=root/'build-host-checks/sd-write-verify';out.mkdir(parents=True,exist_ok=True)
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
source=(root/'main/board/sd_write_verify.c').read_text(encoding='utf-8')
source=re.sub(r'^#include "(?:esp_heap_caps|esp_log|esp_rom_crc|esp_timer|sdmmc_cmd)\.h"\n','',source,flags=re.M)
prefix=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_DMA 4U
typedef struct {struct{unsigned sector_size;size_t capacity;}csd;}sdmmc_card_t;
static int64_t clock_us=1000000;
static int64_t esp_timer_get_time(void){return clock_us;}
static size_t free_bytes=32000,allocation_limit=4096;
static void *heap_caps_malloc(size_t n,unsigned flags){assert(flags==(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL));if(n>allocation_limit)return NULL;size_t*p=malloc(n+sizeof(size_t));if(p){*p=n;free_bytes-=n;return p+1;}return NULL;}
static void heap_caps_free(void*p){if(p){size_t*header=(size_t*)p-1;free_bytes+=*header;free(header);}}
static size_t heap_caps_get_free_size(unsigned flags){(void)flags;return free_bytes;}
static uint32_t esp_rom_crc32_le(uint32_t crc,const void*v,uint32_t n){const uint8_t*p=v;crc=~crc;while(n--){crc^=*p++;for(unsigned b=0;b<8;b++)crc=(crc>>1)^((crc&1)?0xedb88320U:0);}return ~crc;}
static unsigned logs;
static void log_event(const char *tag,const char *format,...){(void)tag;(void)format;++logs;}
#define ESP_LOGW(...) log_event(__VA_ARGS__)
static uint8_t disk[256][512];
static unsigned writes,reads,lost_writes,wrong_reads;
static bool write_error,read_error,mutate_source,slow,skip_read_payload;
static esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t*c,const void*p,size_t at,size_t n){
 assert(c&&p&&n&&at+n<=256);writes++;errno=77;clock_us+=slow?1600000:100;
 if(write_error){errno=EIO;return ESP_FAIL;}
 if(mutate_source)((uint8_t*)p)[0]^=1;
 if(lost_writes)lost_writes--;else memcpy(disk[at],p,n*512);
 return ESP_OK;}
static esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t*c,void*p,size_t at,size_t n){
 assert(c&&p&&n&&at+n<=256);reads++;clock_us+=100;
 if(read_error){errno=EIO;return ESP_FAIL;}if(!skip_read_payload)memcpy(p,disk[at],n*512);if(wrong_reads){wrong_reads--;((uint8_t*)p)[0]^=0x40;}return ESP_OK;}
'''
# Production declarations use external linkage, matching the real linker adapters.
prefix=prefix.replace('static esp_err_t __real_', 'esp_err_t __real_')
tests=r'''
static void reset(void){nuwax_sd_write_verify_set_single_reads(false);memset(disk,0x11,sizeof(disk));writes=reads=lost_writes=wrong_reads=0;write_error=read_error=mutate_source=slow=false;free_bytes=32000;allocation_limit=4096;clock_us+=100000;}
int main(void){sdmmc_card_t card={.csd={512,256}};uint8_t data[16384];memset(data,0x5a,sizeof(data));
 reset();lost_writes=1;assert(__real_sdmmc_write_sectors(&card,data,40,8)==ESP_OK&&memcmp(disk[40],data,4096));
 puts("NEGATIVE CONTROL: SDK successful lost write leaves different persistent bytes");
 reset();assert(nuwax_sd_write_verified(&card,data,40,8)==ESP_OK&&writes==1&&reads==1&&errno==77&&!memcmp(disk[40],data,4096)&&free_bytes==32000);
 reset();lost_writes=1;assert(nuwax_sd_write_verified(&card,data,40,8)==ESP_OK&&writes==9&&!memcmp(disk[40],data,4096));
 assert(disk[39][0]==0x11&&disk[48][0]==0x11);puts("PASS production verifier: successful lost write repaired with one exact-range rewrite, independent full-byte readback");
 reset();lost_writes=100;assert(nuwax_sd_write_verified(&card,data,50,8)==ESP_ERR_INVALID_CRC&&writes==9&&errno==EILSEQ);puts("PASS production verifier: persistent corruption never becomes success; one exact single-sector rewrite pass maximum");
 reset();write_error=true;assert(nuwax_sd_write_verified(&card,data,30,1)==ESP_FAIL&&writes==1&&!reads&&errno==EIO);
 reset();read_error=true;assert(nuwax_sd_write_verified(&card,data,30,1)==ESP_FAIL&&writes==1&&reads==1&&errno==EIO);
 reset();mutate_source=true;assert(nuwax_sd_write_verified(&card,data,30,1)==ESP_ERR_INVALID_CRC&&writes==1);data[0]^=1;
 reset();wrong_reads=1;assert(nuwax_sd_write_verified(&card,data,30,1)==ESP_OK&&writes==1&&nuwax_sd_write_verify_single_reads());
 reset();skip_read_payload=true;assert(nuwax_sd_write_verified(&card,data,30,1)==ESP_ERR_INVALID_CRC&&writes==2);skip_read_payload=false;
 reset();nuwax_sd_write_verify_set_single_reads(true);
 assert(nuwax_sd_write_verified(&card,data,40,8)==ESP_OK&&writes==1&&reads==8&&free_bytes==32000);
 nuwax_sd_write_verify_set_single_reads(false);
 puts("PASS incomplete successful read cannot reuse stale buffer; compatibility verifies exact single sectors");
 puts("PASS production verifier: native errors propagated, source mutation rejected, transient read corruption verified again");
 reset();free_bytes=16384;assert(nuwax_sd_write_verified(&card,data,30,1)==ESP_OK&&writes==1&&reads==1&&free_bytes==16384);
 reset();allocation_limit=512;assert(nuwax_sd_write_verified(&card,data,32,32)==ESP_OK&&writes==1&&reads==32&&free_bytes==32000);
 reset();slow=true;assert(nuwax_sd_write_verified(&card,data,30,1)==ESP_ERR_TIMEOUT&&writes==1&&!reads);
 puts("PASS production verifier: internal reserve, allocation fallback, deadline, retained native multi-sector write and bounded read chunks");
 reset();unsigned oldlogs=logs;for(unsigned n=0;n<100;n++){lost_writes=2;assert(nuwax_sd_write_verified(&card,data,60,1)==ESP_ERR_INVALID_CRC);}assert(logs-oldlogs<=1);
 nuwax_sd_write_verify_stats_t stats=nuwax_sd_write_verify_stats();assert(stats.recovered>=2&&stats.failures>=100&&stats.memory_denied&&stats.reserve_uses&&stats.single_read_recoveries&&stats.max_us>=1600000);
 printf("PASS bounded logs and counters: writes=%u recovered=%u failures=%u max_us=%u\n",stats.writes,stats.recovered,stats.failures,stats.max_us);
 return 0;}
'''
bulk=(root/'main/board/sd_dma_bulk.c').read_text(encoding='utf-8')
bulk=re.sub(r'^#include "(?:esp_heap_caps|esp_memory_utils|esp_timer|esp_rom_crc|sdmmc_cmd|freertos/FreeRTOS|freertos/task)\.h"\n','',bulk,flags=re.M)
prefix+='''
#define CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE 1
static const void *external_pointer;
static bool esp_ptr_external_ram(const void *p){return p==external_pointer;}
static uintptr_t xTaskGetCurrentTaskHandle(void){return 1;}
'''
tests=tests.replace(' return 0;}','''
 reset();lost_writes=1;external_pointer=data;
 assert(__wrap_sdmmc_write_sectors(&card,data,40,8)==ESP_OK&&!memcmp(disk[40],data,4096));
 unsigned checked=nuwax_sd_write_verify_stats().writes;uint8_t output[4096];external_pointer=output;
 assert(__wrap_sdmmc_read_sectors(&card,output,40,8)==ESP_OK&&!memcmp(output,data,4096));
 assert(nuwax_sd_write_verify_stats().writes==checked&&free_bytes==32000);
 puts("PASS actual project DMA adapter + verifier: external source, silent-write repair, reads retain original path");
 return 0;}''')
fixture=out/'verify.c';fixture.write_text(prefix+source+tests,encoding='utf-8');exe=out/'verify.exe'
fixture.write_text(prefix+source+bulk+tests,encoding='utf-8')
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-DESP_ERR_INVALID_CRC=0x109',
 '-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),str(fixture),'-o',str(exe)],check=True)
result=subprocess.run([str(exe)],text=True,capture_output=True,check=True,timeout=30)
(out/'result.txt').write_text(result.stdout,encoding='utf-8');print(result.stdout)
