"""Execute R218 production I/O/replay/recovery with measured fault classes.
No serial access, device reset, or physical-card I/O.
"""
from pathlib import Path
import ast,ctypes,os,re,shutil,subprocess,tempfile,sys
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-io-r218';out.mkdir(parents=True,exist_ok=True)
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)

def function(text,signature):
    at=text.index(signature);end=text.index('{',at)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[at:end]

def run(name,code,units=()):
    src=out/(name+'.c');src.write_text(code,encoding='utf-8');exe=src.with_suffix('.exe')
    subprocess.run(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
        '-Wno-unused-function','-Wno-misleading-indentation','-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
        '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
        '-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),
        str(src),*map(str,units),'-o',str(exe)],check=True)
    with tempfile.TemporaryDirectory(prefix='nuwax-r218-') as cwd:
        p=subprocess.run([str(exe)],cwd=cwd,text=True,capture_output=True,timeout=900 if name=='fatfs-eight-hour' else 120)
    (out/(name+'.log')).write_text(p.stdout+p.stderr,encoding='utf-8')
    print(p.stdout+p.stderr,flush=True);p.check_returncode()

# Reuse the existing SDK/allocator primitive fixture, not the verifier logic.
text=(root/'tools/test-sd-write-verify.py').read_text()
ns={'__file__':str(root/'tools/test-sd-write-verify.py')}
exec(compile(text[:text.index('tests=tests.replace(')],'r218-native-primitives','exec'),ns)
prefix=ns['prefix']
prefix=prefix.replace('static bool write_error,read_error,mutate_source,slow,skip_read_payload;',
    'static bool write_error,read_error,mutate_source,slow,skip_read_payload,cross_write_fault,cross_read_fault,require_boundaries,sdk_nomem_once;')
prefix=prefix.replace('writes++;errno=77;', '''
 if(require_boundaries)assert(at/64==(at+n-1)/64);
 if(sdk_nomem_once){sdk_nomem_once=false;errno=ENOMEM;return ESP_ERR_NO_MEM;}
 writes++;errno=77;''')
prefix=prefix.replace('return ESP_OK;}\nstatic esp_err_t __real_sdmmc_read_sectors', 'return ESP_OK;}\nstatic esp_err_t __real_sdmmc_read_sectors')
prefix=prefix.replace('if(lost_writes)lost_writes--;else memcpy(disk[at],p,n*512);',
    'if(lost_writes)lost_writes--;else memcpy(disk[at],p,n*512);if(cross_write_fault&&at/64!=(at+n-1)/64)disk[(at+63)&~63U][0]^=0x40;')
prefix=prefix.replace('reads++;clock_us+=100;',
    'if(require_boundaries)assert(at/64==(at+n-1)/64);reads++;clock_us+=100;')
prefix=prefix.replace('if(wrong_reads){wrong_reads--;((uint8_t*)p)[0]^=0x40;}',
    'if(wrong_reads){wrong_reads--;((uint8_t*)p)[0]^=0x40;}if(cross_read_fault&&at/64!=(at+n-1)/64)((uint8_t*)p)[((at+63)&~63U)*512-at*512]^=0x40;')
native=r'''
static void reset(void){nuwax_sd_write_verify_set_single_reads(false);atomic_store(&s_single_reads,false);
 memset(disk,0x11,sizeof(disk));writes=reads=lost_writes=wrong_reads=0;free_bytes=32000;allocation_limit=4096;
 write_error=read_error=mutate_source=slow=skip_read_payload=cross_write_fault=cross_read_fault=require_boundaries=sdk_nomem_once=false;clock_us+=100000;}
int main(void){sdmmc_card_t card={.csd={512,256}};uint8_t source[16384],dest[16384];
 for(unsigned n=0;n<sizeof(source);n++)source[n]=(uint8_t)(n*17+n/512);
 reset();cross_write_fault=true;assert(__real_sdmmc_write_sectors(&card,source,63,8)==ESP_OK&&memcmp(disk[63],source,4096));
 puts("NEGATIVE CONTROL simulated SDK success across 32KiB boundary leaves wrong bytes");
 reset();cross_write_fault=cross_read_fault=require_boundaries=true;
 assert(__wrap_sdmmc_write_sectors(&card,source,63,8)==ESP_OK&&!memcmp(disk[63],source,4096)&&writes==2);
 assert(__wrap_sdmmc_read_sectors(&card,dest,63,8)==ESP_OK&&!memcmp(dest,source,4096));
 for(unsigned sector=32;sector<128;sector++){
  assert(__wrap_sdmmc_write_sectors(&card,source,sector,32)==ESP_OK);
  assert(__wrap_sdmmc_read_sectors(&card,dest,sector,32)==ESP_OK&&!memcmp(dest,source,16384));
 }
 puts("PASS actual DMA adapter + native verifier: 96 start alignments, no command crosses physical 64-sector boundary, exact original bytes");
 reset();free_bytes=0;allocation_limit=0;
 assert(nuwax_sd_write_verified(&card,source,63,32)==ESP_OK&&writes==32&&reads==32&&free_bytes==0);
 assert(!memcmp(disk[63],source,16384)&&disk[62][0]==0x11&&disk[95][0]==0x11);
 assert(__wrap_sdmmc_read_sectors(&card,dest,63,32)==ESP_OK&&!memcmp(dest,source,16384));
 puts("PASS actual verifier: zero allocatable heap retains original-byte write/read proof via fixed reserve, no range extension or heap loan");
 reset();sdk_nomem_once=true;assert(nuwax_sd_write_verified(&card,source,40,8)==ESP_OK&&!memcmp(disk[40],source,4096)&&free_bytes==32000);
 puts("PASS SDK bounce ENOMEM retries exact source through fixed DMA buffers before FatFs can turn it into media failure");
 reset();wrong_reads=1;assert(nuwax_sd_write_verified(&card,source,40,8)==ESP_OK&&writes==1&&nuwax_sd_bulk_read_is_degraded());
 assert(__wrap_sdmmc_read_sectors(&card,dest,40,8)==ESP_OK&&!memcmp(dest,source,4096));
 puts("PASS bulk read disagreement uses independent original-source comparison before any rewrite; validated mode reaches all DMA reads");
 reset();memset(source,0xa7,sizeof(source));lost_writes=100;skip_read_payload=true;
 assert(nuwax_sd_write_verified(&card,source,40,8)==ESP_ERR_INVALID_CRC&&disk[40][0]==0x11);
 puts("PASS successful unfilled DMA read cannot certify even an all-a7 source sector");
 reset();allocation_limit=0;assert(!atomic_flag_test_and_set(&s_verify_reserve_owner));
 assert(nuwax_sd_write_verified(&card,source,40,8)==ESP_ERR_NO_MEM&&!writes);atomic_flag_clear(&s_verify_reserve_owner);
 assert(nuwax_sd_write_verified(&card,source,40,8)==ESP_OK);
 puts("PASS competing reserve owner rejected without I/O or foreign release; next operation reuses reserve safely");return 0;}
'''
run('native-boundary-reserve',prefix+ns['source']+ns['bulk']+native)

spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
types=''
for name in ['archive_header_t','archive_footer_t']:
    end=spool.index('} '+name+';')+len('} '+name+';');start=spool.rindex('typedef struct',0,end)
    types+=spool[start:end]+'\n'
common=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "esp_err.h"
#include "audio_frame.h"
#include "sd_audio_catalog.h"
#define HEADER_BYTES 96U
#define FOOTER_BYTES 32U
#define MAX_FRAME_BYTES 1024U
#define ARCHIVE_STDIO_BUFFER_BYTES 16384U
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DMA 4
#define MALLOC_CAP_SPIRAM 8
static int64_t clock_us;
static int64_t esp_timer_get_time(void){return ++clock_us;}
static size_t heap_caps_get_free_size(unsigned f){(void)f;return 100000;}
static size_t heap_caps_get_largest_free_block(unsigned f){(void)f;return 100000;}
static unsigned heap_live;
static bool checking_crc,io_held,independent;
static void *heap_caps_malloc(size_t n,unsigned f){(void)f;void*p=malloc(n);if(p)++heap_live;return p;}
static void heap_caps_free(void*p){if(p){assert(heap_live);--heap_live;}free(p);}
static uint32_t crc32_update(uint32_t crc,const void*v,size_t n){if(checking_crc&&!independent)assert(!io_held);const uint8_t*p=v;crc=~crc;while(n--){crc^=*p++;for(unsigned i=0;i<8;i++)crc=(crc>>1)^((crc&1)?0xedb88320U:0);}return ~crc;}
static unsigned media_faults,ends,validated,evictions;
static bool busy,corrupt,permanent,cache_bad;
bool nuwax_sd_bulk_read_recovery_begin(uint32_t ms){assert(ms);if(busy||independent)return false;independent=true;return true;}
void nuwax_sd_bulk_read_recovery_end(bool original){assert(independent);independent=false;++ends;if(original)++validated;}
static void nuwax_sd_card_mark_io_fault(esp_err_t e){assert(e!=ESP_OK);++media_faults;}
static void log_ignore(const char*f,...){(void)f;}
#define ESP_LOGW(...) log_ignore(__VA_ARGS__)
#define ESP_LOGI(...) log_ignore(__VA_ARGS__)
#define TAG "host"
'''+types
reader=common+r'''
static bool read_nomem;
static ssize_t injected_read(int fd,void*p,size_t n){if(read_nomem){errno=ENOMEM;return -1;}off_t at=lseek(fd,0,SEEK_CUR);ssize_t got=read(fd,p,n);
 if(n==1){++evictions;cache_bad=false;}
 if(got>0&&corrupt&&n>1&&at<=96&&at+got>=100&&(!independent||permanent||cache_bad)){
  memset((uint8_t*)p+96-at,0,4);cache_bad=true;
 }return got;}
#define read injected_read
'''+(root/'main/offline_replay_stream.inc').read_text()+(root/'main/offline_replay_read.inc').read_text()+r'''
#undef read
static bool cancel_reentry;
static unsigned releases,reentries;
static void leave_io(void){assert(io_held&&!heap_live);io_held=false;++releases;}
static esp_err_t enter_io(void*context){(void)context;assert(!io_held&&!heap_live);++reentries;if(cancel_reentry)return ESP_ERR_NOT_FINISHED;io_held=true;return ESP_OK;}
static FILE *audio(archive_header_t*h,archive_footer_t*f){FILE*file=fopen("audio.part","w+b");assert(file);memset(h,0,sizeof(*h));memset(f,0,sizeof(*f));
 memcpy(h->magic,"NWXOFF1",7);h->version=1;h->header_bytes=96;h->sample_rate=16000;h->channels=1;h->codec=NUWAX_AUDIO_CODEC_PCM16;h->frame_duration_ms=20;
 memcpy(f->magic,"NWXEND1",7);f->version=1;f->footer_bytes=32;f->complete=1;f->frame_count=120;f->payload_bytes=120*662;
 assert(fwrite(h,1,96,file)==96);for(unsigned n=0;n<120;n++){int16_t pcm[320];for(unsigned k=0;k<320;k++)pcm[k]=(int16_t)(n*3+k);uint8_t frame[662];size_t size;
  assert(nuwax_audio_frame_build_pcm16(frame,662,pcm,320,n,1000+n*20,0,16000,1,20,&size)==ESP_OK&&size==662);uint32_t bytes=(uint32_t)size;
  f->crc32=crc32_update(f->crc32,frame,size);assert(fwrite(&bytes,1,4,file)==4&&fwrite(frame,1,size,file)==size);}
 assert(fwrite(f,1,32,file)==32&&fflush(file)==0&&replay_read_rewind(file)==ESP_OK);return file;}
static void reset(void){independent=busy=corrupt=permanent=cache_bad=read_nomem=false;media_faults=ends=validated=evictions=0;}
int main(void){archive_header_t h;archive_footer_t f;uint8_t block[8192];uint64_t r=0,v=0,w=0;reset();FILE*file=audio(&h,&f);replay_stream_t state={0};replay_read_stats_t stats={0};
 corrupt=true;size_t n=8192;assert(replay_read_validate(file,block,&n,&state,80048,&h,&f,&r,&v,&w,&stats)==ESP_OK&&n==8192&&state.position==8192);
 assert(stats.independent_recoveries==1&&evictions==1&&!media_faults&&!validated&&!independent&&state.frames==12);
 while(state.position<80048){n=80048-state.position;if(n>8192)n=8192;assert(replay_read_validate(file,block,&n,&state,80048,&h,&f,&r,&v,&w,&stats)==ESP_OK);}
 assert(state.frames==120&&state.crc==f.crc32&&validated==1&&!media_faults&&!independent&&fclose(file)==0);
 puts("PASS actual replay: corrupt bulk length retried from stream checkpoint with cache eviction; every frame/CRC once, full original CRC required before mode certification");
 reset();file=audio(&h,&f);state=(replay_stream_t){0};stats=(replay_read_stats_t){0};replay_stream_t original=state;corrupt=permanent=true;n=8192;
 assert(replay_read_validate(file,block,&n,&state,80048,&h,&f,&r,&v,&w,&stats)==ESP_ERR_INVALID_SIZE&&!n&&!memcmp(&state,&original,sizeof(state))&&!validated&&!independent);fclose(file);
 puts("PASS persistent damage publishes zero bytes, restores original parser state and cannot become upload success");
 reset();file=audio(&h,&f);state=(replay_stream_t){0};stats=(replay_read_stats_t){0};corrupt=busy=true;n=8192;
 assert(replay_read_validate(file,block,&n,&state,80048,&h,&f,&r,&v,&w,&stats)==ESP_ERR_NOT_FINISHED&&!n&&!state.position&&!validated&&!media_faults);fclose(file);
 puts("PASS busy independent owner defers replay without treating an unverified slice as permanent missing audio");
 for(unsigned scenario=0;scenario<4U;++scenario){
  reset();file=audio(&h,&f);state=(replay_stream_t){0};original=state;
  stats=(replay_read_stats_t){.enter_storage=enter_io,.leave_storage=leave_io};
  corrupt=scenario!=0U;permanent=scenario==3U;cancel_reentry=scenario==2U;
  releases=reentries=0U;io_held=checking_crc=true;n=8192U;
  esp_err_t result=replay_read_validate(file,block,&n,&state,80048,&h,&f,&r,&v,&w,&stats);
  checking_crc=false;
  assert(releases==1U&&stats.unlocked_validations==1U&&!heap_live);
  if(scenario==0U)assert(result==ESP_OK&&n==8192U&&stats.storage_released&&!io_held&&!reentries);
  if(scenario==1U)assert(result==ESP_OK&&n==8192U&&!stats.storage_released&&io_held&&reentries==1U&&stats.independent_recoveries==1U);
  if(scenario==2U)assert(result==ESP_ERR_NOT_FINISHED&&!n&&stats.storage_released&&!io_held&&reentries==1U&&!memcmp(&state,&original,sizeof(state))&&!media_faults);
  if(scenario==3U)assert(result==ESP_ERR_INVALID_SIZE&&!n&&!stats.storage_released&&io_held&&reentries==1U&&!memcmp(&state,&original,sizeof(state)));
  io_held=false;assert(!fclose(file));
 }
 puts("PASS actual two-phase read: DMA freed before lock release, CRC outside storage, retry reacquires, cancellation/damage publish zero bytes and restore parser");
 reset();file=audio(&h,&f);state=(replay_stream_t){0};stats=(replay_read_stats_t){0};read_nomem=true;n=8192U;
 assert(replay_read_validate(file,block,&n,&state,80048,&h,&f,&r,&v,&w,&stats)==ESP_ERR_NO_MEM&&!n&&!state.position&&!media_faults&&!heap_live);
 assert(!fclose(file));puts("PASS read allocation pressure is typed NO_MEM, retains parser/data and does not mark media bad");return 0;}
'''
run('replay-checkpoint',reader,[root/'main/audio_frame.c'])

metadata=common+r'''
#define ESP_PLATFORM 1
static unsigned metadata_writes;static bool bad_footer;
static uint32_t u32(const uint8_t*p){uint32_t n;memcpy(&n,p,4);return n;}
static esp_err_t sd_audio_catalog_write(const char*id,sd_audio_segment_t*s){assert(id&&s);++metadata_writes;return ESP_OK;}
static size_t injected_fread(void*p,size_t n,size_t count,FILE*f){size_t got=fread(p,n,count,f);
 if(got*n==32&&bad_footer&&(!independent||permanent))((uint8_t*)p)[24]^=0x40;return got;}
#define fread injected_fread
'''
# Header declares external linkage; the mocked persistence primitive matches it.
metadata=metadata.replace('static esp_err_t sd_audio_catalog_write(', 'esp_err_t sd_audio_catalog_write(')
catalog=(root/'main/sd_audio_catalog.c').read_text()
metadata+=function(catalog,'static esp_err_t recover_record_sample(')+'\n'+function(catalog,'static esp_err_t recover_record(')
old=(Path('C:/tmp/nuwax-sd-longrun-release-20261103-r217/main/sd_audio_catalog.c')).read_text()
metadata+='\n'+function(old,'static esp_err_t recover_record(').replace('recover_record(', 'legacy_recover_record(',1)+r'''
#undef fread
static sd_audio_segment_t make_file(void){FILE*f=fopen("metadata.ready","wb");assert(f);uint8_t bytes[794]={0};memcpy(bytes+762,"NWXEND1",7);uint32_t crc=0x12345678,length=662,stamp=1000;
 memcpy(bytes+786,&crc,4);bytes[790]=1;memcpy(bytes+96,&length,4);bytes[100]='N';bytes[101]='A';memcpy(bytes+114,&stamp,4);assert(fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes)&&fclose(f)==0);
 sd_audio_segment_t s={.sealed=1,.total_bytes=794,.payload_crc=crc,.start_ms=1000};strcpy(s.path,"metadata.ready");return s;}
int main(void){assert(!cache_bad&&!corrupt&&!evictions);sd_audio_segment_t s=make_file();bool complete=true;bad_footer=true;
 sd_audio_segment_t legacy=s;assert(legacy_recover_record("scene",&legacy,&complete)==ESP_OK&&legacy.payload_crc!=s.payload_crc);
 puts("NEGATIVE CONTROL original metadata recovery replaces the indexed original CRC with a disagreeing footer");
 metadata_writes=0;complete=true;assert(recover_record("scene",&s,&complete)==ESP_OK&&s.payload_crc==0x12345678&&metadata_writes==1&&ends==1&&!validated&&!independent);
 puts("PASS actual catalog recovery: fresh single-sector metadata retry preserves original sealed size/CRC and persists only after successful close");
 s=make_file();sd_audio_segment_t original=s;permanent=true;metadata_writes=0;
 assert(recover_record("scene",&s,&complete)==ESP_ERR_INVALID_CRC&&!metadata_writes&&!memcmp(&s,&original,sizeof(s))&&!independent);
 busy=true;assert(recover_record("scene",&s,&complete)==ESP_ERR_NOT_FINISHED&&!metadata_writes&&!independent);
 puts("PASS persistent or busy metadata recovery cannot synthesize a replacement original CRC or publish a partially changed record");return 0;}
'''
run('metadata-original-crc',metadata)

archive_source=r'''
#define SD_AUDIO_ARCHIVE_RECOVERY_HELPERS_ONLY 1
#include "sd_audio_archive_recovery_host_check.c"
int main(void){enter_case("original_index_proof");sd_audio_session_t scene=prepare(0,1);sd_audio_segment_t original,after;
 assert(sd_audio_catalog_read(scene.session_id,0,&original)==ESP_OK&&(original.sealed&1)&&original.total_bytes==794);
 assert(sd_audio_catalog_drop_backup(original.path)==ESP_OK);
 FILE*f=fopen(original.path,"r+b");assert(f&&fseek(f,-8,SEEK_END)==0);uint32_t wrong=original.payload_crc^0x40;
 assert(fwrite(&wrong,1,4,f)==4&&fclose(f)==0);uint32_t before=physical_crc(scene.session_id,0);bool changed=false;
 assert(catalog_recover_ready(original.path,scene.session_id,0,&changed)==ESP_OK&&!changed);
 assert(sd_audio_catalog_read(scene.session_id,0,&after)==ESP_OK&&!memcmp(&after,&original,sizeof(after))&&physical_crc(scene.session_id,0)==before);
 assert(sd_audio_catalog_recover(8)==ESP_OK);assert(sd_audio_catalog_read(scene.session_id,0,&after)==ESP_OK&&after.missing==1&&after.payload_crc==original.payload_crc);
 assert(sd_audio_catalog_load(scene.session_id,&scene)==ESP_OK&&!scene.complete&&physical_crc(scene.session_id,0)==before);
 puts("PASS actual full catalog scan/recovery: absent optional backup cannot let a disagreeing footer replace the sealed original CRC; source bytes retained and partial marker honest");
 assert(chdir("..")==0);enter_case("surviving_backup_proof");scene=prepare(0,1);
 assert(sd_audio_catalog_read(scene.session_id,0,&original)==ESP_OK&&catalog_segment_backup_write(scene.session_id,&original)==ESP_OK);
 char page[128];uint64_t offset;assert(catalog_index_location(scene.session_id,0,page,&offset)==ESP_OK);f=fopen(page,"r+b");assert(f);
 sd_audio_segment_t stale=original;stale.payload_crc^=0x40;assert(write_pair(f,offset,&stale,sizeof(stale),offsetof(sd_audio_segment_t,crc))==ESP_OK&&fclose(f)==0);
 changed=false;before=physical_crc(scene.session_id,0);assert(catalog_recover_ready(original.path,scene.session_id,0,&changed)==ESP_OK&&changed);
 assert(sd_audio_catalog_read(scene.session_id,0,&after)==ESP_OK&&after.payload_crc==original.payload_crc&&physical_crc(scene.session_id,0)==before);
 puts("PASS original same-owner backup still repairs a stale valid index; checksum protection does not disable existing archive reconstruction");return 0;}
'''
run('catalog-original-proof',archive_source,[root/'main/audio_frame.c'])
print('PASS R218 measured fault-class regressions',flush=True)

if '--long' in sys.argv:
    # Compose the actual FatFs, DMA adapter, native verifier, archive guards
    # and NA builder. Two FILs interleave under the same serialized disk API,
    # with allocator pressure at write time, not just an isolated unit call.
    template=root/'build-host-checks/sd-catalog-fatfs-dynamic'
    for name in ['ff.c','ff.h','ffconf.h','diskio.h','ffunicode.c','ffsystem.c']:
        shutil.copyfile(template/name,out/name)
    tree=ast.parse((root/'tools/test-sd-write-fatfs.py').read_text())
    value=next(n.value for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='code' for t in n.targets))
    code=eval(compile(ast.Expression(value),'fatfs-primitives','eval'),{'verify':ns['source']})
    code=code[:code.index('int main(void)')]
    code=code.replace('enum{SECTORS=131072};static uint8_t *image;', r'''
enum{SECTORS=8388608};static uint8_t **image;
static void sparse_store(size_t sector,const void*data){size_t page=sector/64;if(!image[page]){image[page]=calloc(64,512);assert(image[page]);}memcpy(image[page]+(sector%64)*512,data,512);}
static void sparse_load(void*out,size_t sector,size_t n){uint8_t*p=out;for(size_t i=0;i<n;i++){size_t at=sector+i;if(image[at/64])memcpy(p+i*512,image[at/64]+(at%64)*512,512);else memset(p+i*512,0,512);}}
''')
    code=code.replace('memcpy(image+(at+i)*512,block,512);','sparse_store(at+i,block);')
    code=code.replace('memcpy(p,image+at*512,n*512);','sparse_load(p,at,n);')
    old_crc=function(code,'static uint32_t esp_rom_crc32_le(')
    code=code.replace(old_crc,r'''static uint32_t esp_rom_crc32_le(uint32_t c,const void*v,uint32_t n){
 static uint32_t table[256];static bool ready;if(!ready){for(unsigned k=0;k<256;k++){uint32_t x=k;for(unsigned j=0;j<8;j++)x=(x>>1)^((x&1)?0xedb88320U:0);table[k]=x;}ready=true;}
 const uint8_t*p=v;c=~c;while(n--)c=(c>>8)^table[(c^*p++)&255];return ~c;}''')
    code=code.replace('static bool guarded;static unsigned drops_remaining,drops;',
        'static bool guarded,verify_read_pending,inject_bulk_read;static unsigned drops_remaining,drops;')
    code=code.replace('assert(c&&p&&at+n<=SECTORS);clock_us+=100;',
        'assert(c&&p&&at+n<=SECTORS);if(guarded)assert(at/64==(at+n-1)/64);clock_us+=100;',1)
    code=code.replace(' }return ESP_OK;}',' }verify_read_pending=true;return ESP_OK;}',1)
    original='clock_us+=100;sparse_load(p,at,n);return ESP_OK;}'
    code=code.replace(original,'''if(guarded)assert(at/64==(at+n-1)/64);clock_us+=100;sparse_load(p,at,n);
 if(verify_read_pending&&inject_bulk_read&&n>1){((uint8_t*)p)[0]^=0x40;inject_bulk_read=false;}verify_read_pending=false;return ESP_OK;}''')
    insert=r'''
#define CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE 1
static uintptr_t io_task=1;
static uintptr_t xTaskGetCurrentTaskHandle(void){return io_task;}
static bool esp_ptr_external_ram(const void*p){return p!=NULL;}
'''+ns['bulk']+'\n'
    code=code.replace('DSTATUS disk_initialize(BYTE p)',insert+'DSTATUS disk_initialize(BYTE p)',1)
    code=code.replace('return __real_sdmmc_read_sectors(&card,out,at,count)==ESP_OK?',
        'return __wrap_sdmmc_read_sectors(&card,out,at,count)==ESP_OK?')
    code=code.replace('guarded?nuwax_sd_write_verified(&card,in,at,count):',
        'guarded?__wrap_sdmmc_write_sectors(&card,in,at,count):')
    code+=types+r'''
static uint32_t crc(uint32_t c,const void*p,size_t n){return esp_rom_crc32_le(c,p,(uint32_t)n);}
static size_t fat_fwrite(const void*p,size_t size,size_t n,FIL*f){UINT written=0;return f_write(f,p,size*n,&written)==FR_OK?written/size:0;}
#define FILE FIL
#define ftell(f) ((long)f_tell(f))
#define fseek(f,p,m) ((void)(m),f_lseek(f,(FSIZE_t)(p))==FR_OK?0:-1)
#define fwrite fat_fwrite
#define fflush(f) (f_sync(f)==FR_OK?0:-1)
#include "sd_audio_file_time.h"
#undef fflush
#undef fwrite
#undef fseek
#undef ftell
#undef FILE
typedef struct{char path[48];unsigned first,frames,index;uint32_t crc;} saved_file_t;
static uint8_t batch[16384];static size_t buffered;
static void flush_batch(FIL*f){if(buffered){UINT wrote=0;assert(f_write(f,batch,buffered,&wrote)==FR_OK&&wrote==buffered);buffered=0;}}
static void put(FIL*f,const void*data,size_t n){const uint8_t*p=data;while(n){size_t take=sizeof(batch)-buffered;if(take>n)take=n;memcpy(batch+buffered,p,take);buffered+=take;p+=take;n-=take;if(buffered==sizeof(batch))flush_batch(f);}}
static void make_na(uint8_t frame[662],unsigned sequence){int16_t pcm[320];for(unsigned k=0;k<320;k++)pcm[k]=(int16_t)(sequence*3+k);size_t size;
 assert(nuwax_audio_frame_build_pcm16(frame,662,pcm,320,sequence,sequence*20,0,16000,1,20,&size)==ESP_OK&&size==662);}
static void verify_one(FIL*r,const saved_file_t*file,unsigned*done,uint32_t*running){uint32_t length=0;UINT got=0;uint8_t frame[662],original[662];
 assert(f_read(r,&length,4,&got)==FR_OK&&got==4&&length==662);assert(f_read(r,frame,662,&got)==FR_OK&&got==662);
 make_na(original,file->first+*done);assert(!memcmp(frame,original,662));*running=crc(*running,frame,662);++*done;}
static void open_reader(FIL*r,const saved_file_t*file,unsigned mode){archive_header_t h;UINT got=0;assert(f_open(r,file->path,FA_READ)==FR_OK&&f_read(r,&h,96,&got)==FR_OK&&got==96);
 assert(!memcmp(h.magic,"NWXOFF1",7)&&h.segment_index==file->index&&h.mode==mode&&f_size(r)==128+file->frames*666);}
static void finish_reader(FIL*r,const saved_file_t*file,unsigned*done,uint32_t*running){while(*done<file->frames)verify_one(r,file,done,running);archive_footer_t footer;UINT got=0;
 assert(f_read(r,&footer,32,&got)==FR_OK&&got==32&&f_tell(r)==f_size(r));assert(!memcmp(footer.magic,"NWXEND1",7)&&footer.complete==1&&footer.frame_count==file->frames&&footer.payload_bytes==file->frames*662ULL&&footer.crc32==*running&&footer.crc32==file->crc);
 assert(f_close(r)==FR_OK&&f_unlink(file->path)==FR_OK);}
int main(void){image=calloc(SECTORS/64,sizeof(*image));assert(image);FATFS fs;uint8_t work[4096];MKFS_PARM options={.fmt=FM_FAT32|FM_SFD,.n_fat=2,.align=1,.au_size=32768};
 assert(f_mkfs("0:",&options,work,sizeof(work))==FR_OK&&f_mount(&fs,"0:",1)==FR_OK);guarded=true;
 for(unsigned mode=0;mode<2;mode++){unsigned cursor=0,index=0,checked=0,read_done=0;uint32_t read_crc=0;saved_file_t previous={0};FIL reader;bool reading=false;
  nuwax_sd_write_verify_set_single_reads(false);atomic_store(&s_single_reads,false);inject_bulk_read=true;
  while(cursor<1440000){sd_buffer_pressure=false;io_task=1;FIL writer;saved_file_t current={.first=cursor,.index=index};snprintf(current.path,sizeof(current.path),"0:/m%u_%06u.part",mode,index);
   assert(f_open(&writer,current.path,FA_CREATE_NEW|FA_READ|FA_WRITE)==FR_OK);archive_header_t header={0};memcpy(header.magic,"NWXOFF1",7);header.version=1;header.header_bytes=96;header.mode=mode;header.channels=1;header.codec=NUWAX_AUDIO_CODEC_PCM16;header.sample_rate=16000;header.frame_duration_ms=20;header.segment_index=index;
   snprintf(header.session_id,sizeof(header.session_id),"r218_long_%u",mode);sd_audio_seal_head_t guard;assert(sd_audio_seal_head_begin(&guard,&header,96,mode+1,index,crc));
   UINT wrote=0;assert(f_write(&writer,&header,96,&wrote)==FR_OK&&wrote==96&&f_sync(&writer)==FR_OK);buffered=0;
   if(index%29==4)drops_remaining=1;
   while(cursor<1440000&&current.frames<6297){sd_buffer_pressure=(cursor%500)<50;io_task=1;uint8_t frame[662];make_na(frame,cursor);uint32_t size=662;
    put(&writer,&size,4);put(&writer,frame,662);assert(sd_audio_seal_head_record(&guard,frame,662,mode+1,index,crc));current.crc=crc(current.crc,frame,662);++current.frames;++cursor;
    if(current.frames==1||current.frames%100==0){flush_batch(&writer);assert(f_sync(&writer)==FR_OK);}
    if(reading&&read_done<previous.frames){io_task=2;verify_one(&reader,&previous,&read_done,&read_crc);}
   }
   if(reading){io_task=2;finish_reader(&reader,&previous,&read_done,&read_crc);checked+=read_done;reading=false;}
   io_task=1;flush_batch(&writer);assert(f_sync(&writer)==FR_OK);const uint64_t body=96+current.frames*666ULL;assert(f_tell(&writer)==body);
   archive_footer_t footer={0};memcpy(footer.magic,"NWXEND1",7);footer.version=1;footer.footer_bytes=32;footer.complete=1;footer.frame_count=current.frames;footer.payload_bytes=current.frames*662ULL;footer.crc32=current.crc;
   uint8_t seal[1056];uint64_t tail_at;size_t n=sd_audio_seal_head_prepare(&guard,mode+1,index,1793700000000ULL+current.first*20ULL,current.frames*20,seal,sizeof(seal),crc);
   assert(n&&sd_audio_file_head_write(&writer,seal,n));n=sd_audio_seal_tail_prepare(&guard,mode+1,index,body,1793700000000ULL+current.first*20ULL,current.frames*20,&footer,32,seal,sizeof(seal),&tail_at,crc);
   assert(n&&sd_audio_file_tail_write(&writer,body,tail_at,seal,n)&&f_sync(&writer)==FR_OK&&f_close(&writer)==FR_OK);
   sd_buffer_pressure=false;io_task=2;previous=current;open_reader(&reader,&previous,mode);reading=true;read_done=0;read_crc=0;++index;
  }
  io_task=2;finish_reader(&reader,&previous,&read_done,&read_crc);checked+=read_done;assert(checked==1440000&&index==229);
  printf("PASS actual SDK FatFs + DMA + verifier + guarded seal: mode=%u eight-hour frames=%u files=%u interleaved_capture_upload=1 original_PCM_sequence_time_CRC=1\n",mode,checked,index);fflush(stdout);
 }
 nuwax_sd_write_verify_stats_t stats=nuwax_sd_write_verify_stats();
 printf("LONG counters reserve=%u independent=%u failures=%u drops=%u rewrites=%u\n",stats.reserve_uses,stats.single_read_recoveries,stats.failures,drops,stats.rewrites);fflush(stdout);
 assert(stats.reserve_uses&&stats.single_read_recoveries>=2&&stats.failures==0&&drops==16);
 assert(f_mount(NULL,"0:",0)==FR_OK);for(unsigned i=0;i<SECTORS/64;i++)free(image[i]);free(image);return 0;}
'''
    run('fatfs-eight-hour',code,[out/'ff.c',out/'ffunicode.c',out/'ffsystem.c',root/'main/audio_frame.c'])
