"""R217 integrity recovery regressions; host files and RAM FatFs only."""
from pathlib import Path
import ast,ctypes,os,re,subprocess,tempfile

root=Path(__file__).resolve().parents[1]
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
out=root/'build-host-checks/sd-longrun-r217';out.mkdir(parents=True,exist_ok=True)
source=(root/'tools/test-sd-integrity-diagnostics.py').read_text()
source=source.replace("out=root/'build-host-checks/sd-integrity-diagnostics'/('long' if args.long else 'short')", "out=root/'build-host-checks/sd-longrun-r217'")
tree=ast.parse(source)
for node in tree.body:
    if isinstance(node,ast.FunctionDef) and node.name=='run':
        node.body=ast.parse('fixtures[name]=(code,units,arguments,timeout)').body
ns={'__file__':str(root/'tools/test-sd-integrity-diagnostics.py'),'fixtures':{},'print':lambda *a,**kw:None}
exec(compile(ast.fix_missing_locations(tree),'r217-fixture-generator','exec'),ns)

def run(name,code,units=(),extra=()):
    path=out/(name+'.c');path.write_text(code);exe=out/(name+'.exe')
    flags=ns['flags']+['-Wno-misleading-indentation',*extra]
    subprocess.run(flags+[str(path),*map(str,units),'-o',str(exe)],check=True)
    with tempfile.TemporaryDirectory(prefix='nuwax-r217-') as cwd:
        result=subprocess.run([str(exe)],cwd=cwd,text=True,capture_output=True,timeout=180)
    (out/(name+'.log')).write_text(result.stdout+result.stderr)
    print(result.stdout+result.stderr,flush=True);result.check_returncode()

dma=(root/'main/board/sd_dma_bulk.c').read_text()
dma=dma[dma.index('static atomic_uintptr_t s_recovery_reader;'):]
driver=(root/'main/sd_dma_bulk_host_check.c').read_text()
native_hooks=r'''
#define CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS 1
static bool native_budget_busy,native_budget_denied;
bool nuwax_sd_integrity_io_budget_begin(uint32_t ms){if(!ms||native_budget_busy||native_budget_denied)return false;native_budget_busy=true;return true;}
void nuwax_sd_integrity_io_budget_end(void){assert(native_budget_busy);native_budget_busy=false;}
esp_err_t nuwax_sd_integrity_io_transfer(void *c,void*p,size_t sector,size_t count,bool write){return transfer(c,p,sector,count,write);}
'''
driver=driver.replace('#include "sd_dma_bulk_host.inc"',native_hooks+dma)
driver=driver.replace('    puts("PASS: production DMA adapter read/write,', '''
    native_budget_denied=true;assert(!nuwax_sd_bulk_read_recovery_begin(1500)&&!atomic_load(&s_recovery_reader)&&!native_budget_busy);
    native_budget_denied=false;assert(nuwax_sd_bulk_read_recovery_begin(1500)&&native_budget_busy);
    nuwax_sd_bulk_read_recovery_end(false);assert(!native_budget_busy);
    puts("PASS SDK-enabled recovery budget busy/denial releases owner; no nested or foreign budget release");
    puts("PASS: production DMA adapter read/write,''')
run('native-read-recovery',driver)

base,units,_,_=ns['fixtures']['writer'];base=base[:base.index('int main(int argc,char **argv)')]
hooks=r'''
#define NUWAX_SD_READ_RECOVERY_HOST_LINKED 1
#include <stdbool.h>
#include <stdint.h>
static bool independent_reads,recovery_enabled=true;
static unsigned recovered_reads;
bool nuwax_sd_bulk_read_recovery_begin(uint32_t ms){if(!ms||!recovery_enabled||independent_reads)return false;independent_reads=true;return true;}
void nuwax_sd_bulk_read_recovery_end(bool original){if(original)++recovered_reads;independent_reads=false;}
bool nuwax_sd_bulk_read_is_degraded(void){return recovered_reads!=0;}
'''
base=hooks+base.replace('(corrupt_read||corrupt_head)', '(corrupt_head||(corrupt_read&&!independent_reads))')
tests=r'''
int main(void){assert(sd_audio_catalog_init()==ESP_OK);
 begin("record_independent",0);frame(0,true);unsigned before=recovered_reads;corrupt_read=true;
 for(unsigned n=1;n<100;n++)frame(n,true);
 assert(recovered_reads==before+1&&!independent_reads&&!s.integrity->frozen&&!s.verify_bytes&&!s.dropped_frames);
 corrupt_read=false;assert(commit_segment_locked(true)==ESP_OK);part(0,0);
 puts("PASS actual writer: bulk CRC mismatch independently recovers original batch; no capture stop, PCM/count/sequence/time unchanged");
 begin("seal_independent",0);for(unsigned n=0;n<120;n++)frame(n,true);
 assert(sync_archive_file(s.file,"tail")&&writer_verify_records_locked()==ESP_OK);before=recovered_reads;corrupt_read=true;
 assert(commit_segment_locked(true)==ESP_OK&&recovered_reads==before+1&&!independent_reads&&!s.integrity->frozen);corrupt_read=false;part(0,0);
 puts("PASS actual seal: distinct read mode must match original head/tail before publication");
 begin("permanent_seal",0);for(unsigned n=0;n<100;n++)frame(n,true);before=recovered_reads;corrupt_head=true;
 assert(commit_segment_locked(true)!=ESP_OK&&s.integrity->frozen&&s.integrity->stage==4&&recovered_reads==before&&!independent_reads);
 assert(path_exists(s.open_path)&&!path_exists(s.ready_path));corrupt_head=false;
 puts("PASS actual seal: both read modes failing cannot publish or replace the original CRC");
 begin("tail_ram_guard",0);s_writer_fault_stage=NULL;for(unsigned n=0;n<100;n++)frame(n,true);s.seal_head->tail[100]^=1;
 assert(commit_segment_locked(true)!=ESP_OK&&!path_exists(s.ready_path));
 assert(s_writer_fault_stage&&!strcmp(s_writer_fault_stage,"seal_tail_ram"));
 puts("PASS invalid original tail RAM CRC refuses footer/publication");
 begin("reader_busy",0);frame(0,true);corrupt_read=true;recovery_enabled=false;before=recovered_reads;
 for(unsigned n=1;n<99;n++)frame(n,true);
 spool_item_t item={.type=1,.generation=1,.realtime=true};int16_t pcm[320]={0};
 assert(nuwax_audio_frame_build_pcm16(item.data,sizeof(item.data),pcm,320,99,(uint32_t)clock_ms,0,16000,1,20,&item.bytes)==ESP_OK);
 item.queued_crc_valid=true;item.queued_crc=crc32_update(0,item.data,item.bytes);
 assert(append_locked(&item)!=ESP_OK&&recovered_reads==before&&!independent_reads);corrupt_read=false;recovery_enabled=true;
 puts("PASS recovery scope unavailable remains failure; no unvalidated success");return 0;}
'''
run('writer-recovery',base+tests,units)

# Actual SDK FatFs/Picolibc seal code, including a stale FILE tail cache.
fatdir=root/'build-host-checks/sd-seal-speed-r216'
fat=(fatdir/'seal-real-picolibc-fatfs.c').read_text();fat=fat[:fat.index('static void seal_case(')]
time=(root/'main/sd_audio_file_time.h').read_text()
start=time.index('static inline bool sd_audio_file_tail_write(');end=time.index('\n}',start)+2
tail=time[start:end].replace('FILE *','pico_FILE *')
for name in ['ftell','fseek','fwrite','fflush']:tail=re.sub(r'\b'+name+r'\(', 'pico_'+name+'(',tail)
fat+=tail+r'''
static void tail_case(unsigned frames,bool protected_tail,bool poison){
 memset(image,0,PAGES*sizeof(*image));FATFS fs;uint8_t work[4096];MKFS_PARM opts={.fmt=FM_FAT32|FM_SFD,.n_fat=2,.align=1,.au_size=32768};
 assert(f_mkfs("0:",&opts,work,sizeof(work))==FR_OK&&f_mount(&fs,"0:",1)==FR_OK);FIL file;
 assert(f_open(&file,"0:/tail.part",FA_CREATE_ALWAYS|FA_WRITE|FA_READ)==FR_OK);
 char buffer[16384];struct __file_bufio b={.ext={{__SEXT,0},__bufio_seek},.buf=buffer,.size=sizeof(buffer),.file=&file};pico_FILE*f=&b.ext.stream;
 sd_audio_seal_head_t guard;assert(sd_audio_seal_head_begin(&guard,original,96,7,74,crc));
 assert(pico_fwrite(original,1,96,f)==96);
 for(unsigned n=0;n<frames;n++){assert(pico_fwrite(original+96+n*666,1,666,f)==666);assert(sd_audio_seal_head_record(&guard,original+100+n*666,662,7,74,crc));
  if(!n||(n+1)%100==0)assert(pico_fflush(f)==0&&f_sync(&file)==FR_OK);}
 const unsigned body=96+frames*666,total=body+32;uint8_t footer[32]={0};memcpy(footer,"NWXEND1",7);
 assert(pico_fflush(f)==0&&f_sync(&file)==FR_OK);uint8_t prefix[512],prepared[1056];uint64_t offset;
 assert(sd_audio_seal_head_prepare(&guard,7,74,1793687514559ULL,frames*20,prefix,512,crc)==512);
 assert(sd_audio_file_head_write(f,prefix,512));if(poison)memset(file.buf,0,body%512);
 if(protected_tail){size_t bytes=sd_audio_seal_tail_prepare(&guard,7,74,body,1793687514559ULL,frames*20,footer,32,prepared,sizeof(prepared),&offset,crc);
  assert(bytes&&sd_audio_file_tail_write(f,body,offset,prepared,bytes));}
 else assert(pico_fwrite(footer,1,32,f)==32);
 assert(pico_fflush(f)==0&&f_sync(&file)==FR_OK&&f_close(&file)==FR_OK);
 memcpy(expected,original,total);memcpy(expected,prefix,512);memcpy(expected+body,footer,32);
 assert(f_open(&file,"0:/tail.part",FA_READ)==FR_OK);uint8_t block[4096];unsigned at=0,diff=0;
 while(at<total){UINT want=total-at>sizeof(block)?sizeof(block):total-at,got=0;assert(f_read(&file,block,want,&got)==FR_OK&&got==want);for(UINT n=0;n<want;n++)diff+=block[n]!=expected[at+n];at+=want;}
 assert(f_size(&file)==total&&f_close(&file)==FR_OK&&f_mount(NULL,"0:",0)==FR_OK);
 if(poison&&!protected_tail){assert(diff);puts("NEGATIVE CONTROL real FatFs/Picolibc: tiny footer RMW can persist stale tail cache despite successful writes");}
 else{assert(!diff);printf("PASS guarded aligned tail real FatFs/Picolibc frames=%u stale_cache=%u exact_size_and_original_bytes=1\n",frames,poison);}
}
int main(void){image=calloc(PAGES,sizeof(*image));original=malloc(TOTAL);expected=malloc(TOTAL);assert(image&&original&&expected);
 for(unsigned n=0;n<TOTAL;n++)original[n]=(uint8_t)(n*31U+n/666U);memcpy(original,"NWXOFF1",7);
 uint32_t length=662;for(unsigned n=0;n<FRAMES;n++)memcpy(original+96+n*666,&length,4);
 tail_case(6297,false,true);tail_case(6297,true,true);tail_case(1,true,true);tail_case(2,true,true);tail_case(100,true,true);
 sd_audio_seal_head_t guard;assert(sd_audio_seal_head_begin(&guard,original,96,8,0,crc));uint8_t plan[1056],footer[32]={0};memcpy(footer,"NWXEND1",7);uint64_t at;
 for(unsigned n=0;n<300;n++){
  assert(sd_audio_seal_head_record(&guard,original+100+n*666,662,8,0,crc));unsigned body=96+(n+1)*666;
  size_t bytes=sd_audio_seal_tail_prepare(&guard,8,0,body,1793687514559ULL,(n+1)*20,footer,32,plan,sizeof(plan),&at,crc);
  assert(bytes&&!(at%512)&&bytes==body+32-at&&bytes<=1056);
  memcpy(expected,original,body);for(unsigned i=0;i<8;i++)expected[84+i]=(uint8_t)(1793687514559ULL>>(8*i));for(unsigned i=0;i<4;i++)expected[92+i]=(uint8_t)(((n+1)*20)>>(8*i));
  assert(!memcmp(plan,expected+at,bytes-32)&&!memcmp(plan+bytes-32,footer,32));
 }
 guard.tail[17]^=1;assert(!sd_audio_seal_tail_prepare(&guard,8,0,guard.body_end,1,2,footer,32,plan,sizeof(plan),&at,crc));
 puts("PASS 300 tail alignments and short files: original bytes/footer only, exact end, RAM corruption rejected");return 0;}
'''
run('fatfs-tail',fat,[fatdir/'ff.c',fatdir/'ffunicode.c',fatdir/'ffsystem.c'],['-I'+str(fatdir),'-Wno-implicit-fallthrough'])
print('PASS R217 read recovery and guarded tail; no physical card or device I/O',flush=True)
