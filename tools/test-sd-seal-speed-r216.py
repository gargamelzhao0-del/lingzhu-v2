"""Real Picolibc/FatFs seal regression and joint DMA budget boundary tests.

Only a sparse RAM disk and disposable host files are used. Never opens a drive,
serial port or device. The stale-read injection is a fault model, not proof of
the physical cause of the 2026-11-03 device failure.
"""
from pathlib import Path
import argparse, ast, hashlib, json, re, shutil, subprocess, tempfile, types

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk',type=Path,default=Path('C:/esp/v6.0/esp-idf'))
args=parser.parse_args()
out=root/'build-host-checks/sd-seal-speed-r216';out.mkdir(parents=True,exist_ok=True)

def function(source,signature):
    a=source.index(signature);b=source.index('{',a)+1;depth=1
    while depth: depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]

def run(name,code,units=(),extra=()):
    path=out/(name+'.c');path.write_text(code,encoding='utf-8');exe=path.with_suffix('.exe')
    flags=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
           '-Wno-implicit-fallthrough','-Wno-unused-function','-DESP_ERR_INVALID_CRC=0x109',
           '-I'+str(out),'-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs')]
    subprocess.run(flags+list(extra)+[str(path),*map(str,units),'-o',str(exe)],check=True)
    with tempfile.TemporaryDirectory(prefix='nuwax-r216-') as cwd:
        result=subprocess.run([str(exe)],cwd=cwd,capture_output=True,text=True,timeout=180)
    (out/(name+'.log')).write_text(result.stdout+result.stderr)
    print(result.stdout,flush=True);result.check_returncode()

# Match the existing target FatFs host configuration, not a simplified stdio
# implementation. Keep its SDK sources unchanged.
tree=ast.parse((root/'tools/test-sd-catalog-fatfs.py').read_text())
node=next(n for n in tree.body if isinstance(n,ast.Assign) and isinstance(n.targets[0],ast.Name) and n.targets[0].id=='options')
options={kw.arg:ast.literal_eval(kw.value) for kw in node.value.keywords}
options.update(FF_USE_DYN_BUFFER=1,FF_MAX_SS=4096,FF_USE_LFN=3)
(out/'ffconf.h').write_text('#pragma once\n'+'\n'.join(f'#define {k} {v}' for k,v in options.items())+'\n')
for name in ['ff.c','ff.h','diskio.h','ffunicode.c','ffsystem.c']:
    shutil.copy2(args.sdk/'components/fatfs/src'/name,out/name)

up=root/'tools/fixtures/picolibc-1.8.10'
manifest=json.loads((up/'upstream.json').read_text())
for name,meta in manifest.items():assert hashlib.sha256((up/name).read_bytes()).hexdigest()==meta['sha256']
bufio=(up/'bufio.c').read_text();tell=(up/'ftell.c').read_text()
parts=[function(bufio,s) for s in ['int\n__bufio_flush_locked(','int\n__bufio_setdir_locked(','int\n__bufio_put(','off_t\n__bufio_seek(']]
parts.append(function(tell,'FTELL_TYPE\nFTELL(').replace('FTELL_TYPE','long').replace('FTELL(','pico_ftell('))
actual='\n'.join(parts).replace('FILE *','pico_FILE *')
code=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "ff.h"
#include "diskio.h"
#include "sd_audio_seal_head.h"
enum{SECTORS=8388608,PAGES=32768,FRAMES=6297,BODY=96+FRAMES*666,TOTAL=BODY+32};
typedef struct{uint32_t sector;bool valid;uint8_t data[512];}page_t;
static page_t *image;static uint8_t *original,*expected;static unsigned writes,stale_reads;
static bool inject_stale,fail_write;static LBA_t first_sector;static uint8_t stale[512];
static page_t *page(uint32_t sector,bool create){unsigned at=(sector*2654435761U)&(PAGES-1);for(unsigned n=0;n<PAGES;n++,at=(at+1)&(PAGES-1)){
 page_t*p=&image[at];if(p->valid&&p->sector==sector)return p;if(!p->valid){if(!create)return NULL;p->valid=true;p->sector=sector;return p;}}assert(!"RAM disk full");return NULL;}
DSTATUS disk_initialize(BYTE p){assert(!p);return 0;}DSTATUS disk_status(BYTE p){assert(!p);return 0;}
DRESULT disk_read(BYTE p,BYTE*b,LBA_t sector,UINT n){assert(!p&&sector+n<=SECTORS);for(UINT i=0;i<n;i++){
 if(inject_stale&&sector+i==first_sector){memcpy(b+i*512,stale,512);inject_stale=false;++stale_reads;}
 else{page_t*q=page(sector+i,false);if(q)memcpy(b+i*512,q->data,512);else memset(b+i*512,0,512);}}return RES_OK;}
DRESULT disk_write(BYTE p,const BYTE*b,LBA_t sector,UINT n){assert(!p&&sector+n<=SECTORS);++writes;
 for(UINT i=0;i<n;i++)memcpy(page(sector+i,true)->data,b+i*512,512);return RES_OK;}
DRESULT disk_ioctl(BYTE p,BYTE cmd,void*b){assert(!p);switch(cmd){case CTRL_SYNC:return RES_OK;case GET_SECTOR_COUNT:*(LBA_t*)b=SECTORS;return RES_OK;
 case GET_SECTOR_SIZE:*(WORD*)b=512;return RES_OK;case GET_BLOCK_SIZE:*(DWORD*)b=64;return RES_OK;default:return RES_PARERR;}}
DWORD get_fattime(void){return (2026-1980)<<25|11<<21|3<<16;}
enum{__SRD=1,__SWR=2,__SEXT=32,_FDEV_ERR=-1};typedef struct{unsigned flags,unget;}pico_FILE;
typedef off_t __off_t;
struct __file_ext{pico_FILE stream;off_t(*seek)(pico_FILE*,off_t,int);};
struct __file_bufio{struct __file_ext ext;uint8_t dir;char*buf;int len,off,size;off_t pos;unsigned bflags;FIL *file;};
#define __BLBF 1
#define __bufio_lock(f) ((void)(f))
#define __bufio_unlock(f) ((void)(f))
#define __flockfile(f) ((void)(f))
#define __atomic_load_ungetc(p) (*(p))
#define __funlock_return(f,r) do{(void)(f);return(r);}while(0)
#undef __fallthrough
#define __fallthrough __attribute__((fallthrough))
static ssize_t bufio_write(struct __file_bufio*b,const char*p,size_t n){if(fail_write){fail_write=false;errno=EIO;return -1;}
 UINT got=0;if(f_write(b->file,p,(UINT)n,&got)!=FR_OK){errno=EIO;return -1;}return got;}
static off_t bufio_lseek(struct __file_bufio*b,off_t n,int whence){FSIZE_t at=whence==SEEK_SET?(FSIZE_t)n:whence==SEEK_CUR?f_tell(b->file)+(FSIZE_t)n:f_size(b->file)+(FSIZE_t)n;
 if(f_lseek(b->file,at)!=FR_OK){errno=EIO;return -1;}return(off_t)f_tell(b->file);}
'''+actual+r'''
static size_t pico_fwrite(const void*v,size_t size,size_t count,pico_FILE*f){const uint8_t*p=v;size_t n=0;
 for(;n<size*count;n++)if(__bufio_put((char)p[n],f)<0)break;return size?n/size:0;}
static int pico_fseek(pico_FILE*f,long at,int whence){return __bufio_seek(f,at,whence)<0?-1:0;}
static int pico_fflush(pico_FILE*f){return __bufio_flush_locked(f);}
static uint32_t crc(uint32_t c,const void*v,size_t bytes){const uint8_t*p=v;c=~c;while(bytes--){c^=*p++;for(unsigned n=0;n<8;n++)c=(c>>1)^((c&1)?0xedb88320U:0);}return~c;}
'''
time=(root/'main/sd_audio_file_time.h').read_text()
for signature in ['static inline bool sd_audio_file_time_write(', 'static inline bool sd_audio_file_head_write(']:
    body=function(time,signature).replace('FILE *','pico_FILE *')
    for name in ['ftell','fseek','fwrite','fflush']:body=re.sub(r'\b'+name+r'\(', 'pico_'+name+'(',body)
    code+='\n'+body+'\n'
code+=r'''
static void seal_case(bool full,bool poison,bool fail){
 memset(image,0,PAGES*sizeof(*image));FATFS fs;uint8_t work[4096];MKFS_PARM opts={.fmt=FM_FAT32|FM_SFD,.n_fat=2,.align=1,.au_size=32768};
 assert(f_mkfs("0:",&opts,work,sizeof(work))==FR_OK&&f_mount(&fs,"0:",1)==FR_OK);FIL file;
 assert(f_open(&file,"0:/record.part",FA_CREATE_ALWAYS|FA_WRITE|FA_READ)==FR_OK);
 char buffer[16384];struct __file_bufio b={.ext={{__SEXT,0},__bufio_seek},.buf=buffer,.size=sizeof(buffer),.file=&file};pico_FILE*f=&b.ext.stream;
 sd_audio_seal_head_t head;assert(sd_audio_seal_head_begin(&head,original,96,7,74,crc));
 assert(pico_fwrite(original,1,96,f)==96&&pico_fflush(f)==0&&f_sync(&file)==FR_OK);
 for(unsigned n=0;n<FRAMES;n++){
  assert(pico_fwrite(original+96+n*666,1,666,f)==666);
  assert(sd_audio_seal_head_record(&head,original+100+n*666,662,7,74,crc));
  if(!n||(n+1)%100==0)assert(pico_fflush(f)==0&&f_sync(&file)==FR_OK);
 }
 assert(pico_fflush(f)==0&&f_sync(&file)==FR_OK&&pico_ftell(f)==BODY);
 first_sector=fs.database+(file.obj.sclust-2)*fs.csize;
 memcpy(stale,original,512);unsigned altered=0;for(unsigned n=0;n<512&&altered<323;n++)if(n<84||n>=96){stale[n]^=0x40;++altered;}
 inject_stale=poison;stale_reads=0;writes=0;fail_write=fail;
 const uint64_t start=UINT64_C(1793671810053)+74*125940;uint8_t prefix[512];
 assert(sd_audio_seal_head_prepare(&head,7,74,start,125940,prefix,sizeof(prefix),crc)==512);
 for(unsigned n=0;n<512;n++)if(n<84||n>=96)assert(prefix[n]==original[n]);
 bool ok=full?sd_audio_file_head_write(f,prefix,512):sd_audio_file_time_write(f,start,125940);
 inject_stale=false;
 if(fail){assert(!ok&&!fail_write);assert(f_close(&file)==FR_OK&&f_mount(NULL,"0:",0)==FR_OK);puts("PASS failed seal flush returns failure; no footer/publication success");return;}
 assert(ok&&pico_ftell(f)==BODY);
 assert(pico_fwrite(original+BODY,1,32,f)==32&&pico_fflush(f)==0&&f_sync(&file)==FR_OK&&f_close(&file)==FR_OK);
 memcpy(expected,original,TOTAL);memcpy(expected,prefix,512);
 assert(f_open(&file,"0:/record.part",FA_READ)==FR_OK);uint8_t got[4096];size_t at=0;unsigned different=0;
 while(at<TOTAL){UINT n=TOTAL-at>sizeof(got)?sizeof(got):TOTAL-at,have=0;assert(f_read(&file,got,n,&have)==FR_OK&&have==n);
  for(UINT j=0;j<n;j++)different+=got[j]!=expected[at+j];at+=n;}
 assert(f_size(&file)==TOTAL&&f_close(&file)==FR_OK&&f_mount(NULL,"0:",0)==FR_OK);
 if(poison&&!full){assert(different==323&&stale_reads==1);puts("NEGATIVE CONTROL real Picolibc/FatFs: successful 12-byte RMW can persist a successful stale read, 323 wrong first-sector bytes");}
 else{assert(!different);if(poison)assert(!stale_reads);printf("PASS real Picolibc/FatFs full prefix: stale_read=%u exact_4MiB=1 first_sector_read=%u changed_only_time=1\n",poison,stale_reads);}
}
int main(void){image=calloc(PAGES,sizeof(*image));original=malloc(TOTAL);expected=malloc(TOTAL);assert(image&&original&&expected);
 for(unsigned n=0;n<TOTAL;n++)original[n]=(uint8_t)(n*31U+n/666U);memcpy(original,"NWXOFF1",7);
 const uint32_t length=662;for(unsigned n=0;n<FRAMES;n++)memcpy(original+96+n*666,&length,4);
 seal_case(false,false,false);seal_case(false,true,false);seal_case(true,false,false);seal_case(true,true,false);seal_case(true,false,true);
 sd_audio_seal_head_t head;uint8_t output[512];assert(sd_audio_seal_head_begin(&head,original,96,7,74,crc));
 assert(sd_audio_seal_head_record(&head,original+100,662,7,74,crc));
 unsigned before=writes;head.data[120]^=1;assert(!sd_audio_seal_head_prepare(&head,7,74,1,2,output,512,crc));assert(writes==before);
 head.data[120]^=1;assert(!sd_audio_seal_head_prepare(&head,8,74,1,2,output,512,crc));assert(!sd_audio_seal_head_prepare(&head,7,75,1,2,output,512,crc));
 assert(sd_audio_seal_head_begin(&head,original,96,7,74,crc));uint8_t small[20]={0};assert(sd_audio_seal_head_record(&head,small,20,7,74,crc));
 assert(head.count==120&&sd_audio_seal_head_prepare(&head,7,74,1,2,output,512,crc)==120);
 puts("PASS original prefix RAM CRC/generation/file fencing and short-prefix length; no padding over audio/footer");
 free(image);free(original);free(expected);return 0;}
'''
run('seal-real-picolibc-fatfs',code,[out/'ff.c',out/'ffunicode.c',out/'ffsystem.c'])

# Exercise the actual outer adapter and actual verifier against ONE tracked
# heap, including the SDK's extra sector when the original source is external.
tree=ast.parse((root/'tools/test-sd-write-verify.py').read_text())
prefix=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and isinstance(n.targets[0],ast.Name) and n.targets[0].id=='prefix' and isinstance(n.value,ast.Constant))
prefix=prefix.replace('static esp_err_t __real_','esp_err_t __real_')
prefix=prefix.replace('static size_t free_bytes=32000,allocation_limit=4096;','static size_t free_bytes=32000,allocation_limit=4096,overhead;static const void *external_pointer;static size_t min_free;')
prefix=prefix.replace('if(n>allocation_limit)return NULL;', 'if(n>allocation_limit||n+overhead>free_bytes)return NULL;')
prefix=prefix.replace('*p=n;free_bytes-=n;', '*p=n+overhead;free_bytes-=*p;if(free_bytes<min_free)min_free=free_bytes;')
prefix=prefix.replace('if(lost_writes)lost_writes--;else memcpy(disk[at],p,n*512);',
'''void *sdk=NULL;if(p==external_pointer){sdk=heap_caps_malloc(512,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);if(!sdk)return ESP_ERR_NO_MEM;}
 if(lost_writes)lost_writes--;else memcpy(disk[at],p,n*512);heap_caps_free(sdk);''')
prefix+='''
#define CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE 1
static bool esp_ptr_external_ram(const void*p){return p==external_pointer;}
static uintptr_t xTaskGetCurrentTaskHandle(void){return 1;}
'''
verifier=(root/'main/board/sd_write_verify.c').read_text()
verifier=re.sub(r'^#include "(?:esp_heap_caps|esp_log|esp_rom_crc|esp_timer|sdmmc_cmd)\.h"\n','',verifier,flags=re.M)
bulk=(root/'main/board/sd_dma_bulk.c').read_text()
bulk=re.sub(r'^#include "(?:esp_heap_caps|esp_memory_utils|esp_timer|esp_rom_crc|sdmmc_cmd|freertos/FreeRTOS|freertos/task)\.h"\n','',bulk,flags=re.M)
matrix=r'''
int main(void){sdmmc_card_t card={.csd={512,256}};uint8_t data[16384];memset(data,0x5a,sizeof(data));
 const size_t heaps[]={17800,18000,18727,19000,20700,21000,24600,25200,32395,32768,33000,33300,40000};
 const size_t limits[]={512,1024,4096,8192,16384},costs[]={0,20,64,128};unsigned cases=0;
 for(unsigned c=0;c<4;c++)for(unsigned l=0;l<5;l++)for(unsigned i=0;i<sizeof(heaps)/sizeof(heaps[0]);i++){
  free_bytes=min_free=heaps[i];overhead=costs[c];allocation_limit=limits[l];external_pointer=data;writes=reads=0;
  unsigned denied=nuwax_sd_write_verify_stats().memory_denied;memset(disk,0x11,sizeof(disk));
  esp_err_t e=__wrap_sdmmc_write_sectors(&card,data,40,32);
  assert(e==ESP_OK&&!memcmp(disk[40],data,sizeof(data))&&free_bytes==heaps[i]);
  assert(min_free>=16384&&nuwax_sd_write_verify_stats().memory_denied==denied);++cases;
 }
 free_bytes=min_free=17000;overhead=20;allocation_limit=16384;writes=reads=0;
 assert(__wrap_sdmmc_write_sectors(&card,data,40,32)==ESP_OK&&writes==32&&reads==32&&free_bytes==17000);
 assert(!memcmp(disk[40],data,sizeof(data))&&min_free==17000&&nuwax_sd_write_verify_stats().reserve_uses);
 printf("PASS actual joint DMA/verifier: %u heap/fragmentation/allocator-overhead cases, original failed boundaries fixed, SDK bounce included, 16KiB floor and byte-exact writes\n",cases);
 puts("PASS low heap uses fixed DMA verification reserve without new loans; every original byte verified");return 0;}
'''
run('joint-memory-budget',prefix+verifier+bulk+matrix)
print(json.dumps(dict(checks=2,physicalDeviceTouched=False,physicalCardTouched=False)),flush=True)
