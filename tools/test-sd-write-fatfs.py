"""Validate the actual write verifier under the SDK's FatFs on a RAM disk."""
from pathlib import Path
import ctypes,os,re,shutil,subprocess
root=Path(__file__).resolve().parents[1];out=root/'build-host-checks/sd-write-fatfs';out.mkdir(parents=True,exist_ok=True)
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
template=root/'build-host-checks/sd-catalog-fatfs-dynamic'
if not (template/'ffconf.h').exists():subprocess.run(['python',str(root/'tools/test-sd-catalog-fatfs.py'),'--dynamic'],check=True)
for name in ['ff.c','ff.h','ffconf.h','diskio.h','ffunicode.c','ffsystem.c']:shutil.copy2(template/name,out/name)
verify=(root/'main/board/sd_write_verify.c').read_text(encoding='utf-8')
verify=re.sub(r'^#include "(?:esp_heap_caps|esp_log|esp_rom_crc|esp_timer|sdmmc_cmd)\.h"\n','',verify,flags=re.M)
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "esp_err.h"
#include "ff.h"
#include "diskio.h"
#include "audio_frame.h"
#define MALLOC_CAP_DMA 1U
#define MALLOC_CAP_INTERNAL 2U
#define MALLOC_CAP_8BIT 4U
static bool sd_buffer_pressure;
static void *heap_caps_malloc(size_t n,unsigned flags){(void)flags;return sd_buffer_pressure?NULL:malloc(n);}
static void heap_caps_free(void*p){free(p);}
static size_t heap_caps_get_free_size(unsigned flags){(void)flags;return sd_buffer_pressure?0:64000;}
static int64_t clock_us=1000000;
static int64_t esp_timer_get_time(void){return clock_us;}
static uint32_t esp_rom_crc32_le(uint32_t c,const void*v,uint32_t n){const uint8_t*p=v;c=~c;while(n--){c^=*p++;for(unsigned b=0;b<8;b++)c=(c>>1)^((c&1)?0xedb88320U:0);}return ~c;}
static void log_ignore(const char *tag,const char *format,...){(void)tag;(void)format;}
#define ESP_LOGW(...) log_ignore(__VA_ARGS__)
enum{SECTORS=131072};static uint8_t *image;
typedef struct{struct{unsigned sector_size;size_t capacity;}csd;}sdmmc_card_t;
static sdmmc_card_t card={.csd={512,SECTORS}};
static bool guarded;static unsigned drops_remaining,drops;
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t*c,const void*p,size_t at,size_t n){
 assert(c&&p&&at+n<=SECTORS);clock_us+=100;
 for(size_t i=0;i<n;i++){
  const uint8_t *block=(const uint8_t*)p+i*512;
  if(drops_remaining&&!memcmp(block,"NWXOFF1",7)){drops_remaining--;drops++;continue;}
  memcpy(image+(at+i)*512,block,512);
 }return ESP_OK;}
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t*c,void*p,size_t at,size_t n){assert(c&&p&&at+n<=SECTORS);clock_us+=100;memcpy(p,image+at*512,n*512);return ESP_OK;}
'''+verify+r'''
DSTATUS disk_initialize(BYTE p){(void)p;return 0;}
DSTATUS disk_status(BYTE p){(void)p;return 0;}
DRESULT disk_read(BYTE p,BYTE*out,LBA_t at,UINT count){(void)p;return __real_sdmmc_read_sectors(&card,out,at,count)==ESP_OK?RES_OK:RES_ERROR;}
DRESULT disk_write(BYTE p,const BYTE*in,LBA_t at,UINT count){(void)p;esp_err_t e=guarded?nuwax_sd_write_verified(&card,in,at,count):__real_sdmmc_write_sectors(&card,in,at,count);return e==ESP_OK?RES_OK:RES_ERROR;}
DRESULT disk_ioctl(BYTE p,BYTE cmd,void *arg){(void)p;switch(cmd){case CTRL_SYNC:return RES_OK;case GET_SECTOR_COUNT:*(LBA_t*)arg=SECTORS;return RES_OK;case GET_SECTOR_SIZE:*(WORD*)arg=512;return RES_OK;case GET_BLOCK_SIZE:*(DWORD*)arg=1;return RES_OK;default:return RES_PARERR;}}
int main(void){image=calloc(SECTORS,512);assert(image);FATFS fs;uint8_t work[4096];MKFS_PARM options={.fmt=FM_FAT32|FM_SFD,.n_fat=2,.align=1,.au_size=512};
 assert(f_mkfs("0:",&options,work,sizeof(work))==FR_OK);assert(f_mount(&fs,"0:",1)==FR_OK);
 uint8_t data[762]={0},actual[762];memcpy(data,"NWXOFF1",7);uint32_t length=662;memcpy(data+96,&length,4);int16_t pcm[320];for(unsigned i=0;i<320;i++)pcm[i]=(int16_t)(i*33U);
 size_t bytes=0;assert(nuwax_audio_frame_build_pcm16(data+100,662,pcm,320,0,10000,0,16000,1,20,&bytes)==ESP_OK&&bytes==662);
 for(unsigned enabled=0;enabled<2;enabled++){
  guarded=enabled;FIL file;UINT written=0;const char*name=enabled?"0:/guard.part":"0:/control.part";
  assert(f_open(&file,name,FA_READ|FA_WRITE|FA_CREATE_NEW)==FR_OK);drops_remaining=1;unsigned before=drops;
  assert(f_write(&file,data,sizeof(data),&written)==FR_OK&&written==sizeof(data));assert(f_sync(&file)==FR_OK);assert(f_close(&file)==FR_OK);assert(drops==before+1);
  assert(f_open(&file,name,FA_READ)==FR_OK);UINT read=0;assert(f_read(&file,actual,sizeof(actual),&read)==FR_OK&&read==sizeof(actual));assert(f_close(&file)==FR_OK);
  if(enabled)assert(!memcmp(actual,data,sizeof(data)));else assert(memcmp(actual,data,sizeof(data)));
  puts(enabled?"PASS real SDK FatFs + production verifier: non-sector-aligned NA audio file survives acknowledged lost sector with exact bytes":"NEGATIVE CONTROL real SDK FatFs: unverified successful write loses first file sector");
 }
 guarded=true;sd_buffer_pressure=true;FIL low;UINT low_written=0;
 assert(f_open(&low,"0:/reserve.part",FA_READ|FA_WRITE|FA_CREATE_NEW)==FR_OK);
 assert(f_write(&low,data,sizeof(data),&low_written)==FR_OK&&low_written==sizeof(data));
 assert(f_sync(&low)==FR_OK&&f_close(&low)==FR_OK);
 assert(f_open(&low,"0:/reserve.part",FA_READ)==FR_OK);UINT low_read=0;
 assert(f_read(&low,actual,sizeof(actual),&low_read)==FR_OK&&low_read==sizeof(actual)&&!memcmp(actual,data,sizeof(data))&&f_close(&low)==FR_OK);
 assert(nuwax_sd_write_verify_stats().reserve_uses);sd_buffer_pressure=false;
 puts("PASS real SDK FatFs + verifier: SD buffer allocation pressure preserves metadata and exact audio bytes without FR_DISK_ERR");
 guarded=true;FIL bad;UINT written=0;assert(f_open(&bad,"0:/persistent.part",FA_READ|FA_WRITE|FA_CREATE_NEW)==FR_OK);drops_remaining=1000;
 FRESULT e=f_write(&bad,data,sizeof(data),&written);if(e==FR_OK)e=f_sync(&bad);assert(e==FR_DISK_ERR);(void)f_close(&bad);
 puts("PASS real SDK FatFs + production verifier: persistent corruption remains disk error, never successful persistence");
 assert(nuwax_sd_write_verify_stats().recovered>=1&&nuwax_sd_write_verify_stats().failures>=1);f_mount(NULL,"0:",0);free(image);return 0;}
'''
source=out/'fatfs.c';source.write_text(code,encoding='utf-8');exe=out/'fatfs.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-implicit-fallthrough',
 '-DESP_ERR_INVALID_CRC=0x109','-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),'-I'+str(out),
 str(source),str(out/'ff.c'),str(out/'ffunicode.c'),str(out/'ffsystem.c'),str(root/'main/audio_frame.c'),'-o',str(exe)],check=True)
result=subprocess.run([str(exe)],text=True,capture_output=True,check=True,timeout=60)
(out/'result.txt').write_text(result.stdout,encoding='utf-8');print(result.stdout)
