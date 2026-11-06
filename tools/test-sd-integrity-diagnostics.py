"""Run production integrity code with deterministic memory/disk/SDK faults. No device I/O."""
from pathlib import Path
import argparse
import ctypes
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
if os.name=='nt': ctypes.windll.kernel32.SetErrorMode(0x8003)
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--long',action='store_true');args=parser.parse_args()
out=root/'build-host-checks/sd-integrity-diagnostics'/('long' if args.long else 'short');out.mkdir(parents=True,exist_ok=True)
flags=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
       '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
       '-I'+str(out),'-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs')]
def run(name, code, units=(), arguments=(), timeout=90):
    source=out/(name+'.c');source.write_text(code,encoding='utf-8');exe=out/(name+'.exe')
    subprocess.run(flags+[str(source)]+list(map(str,units))+['-o',str(exe)],check=True)
    with tempfile.TemporaryDirectory(prefix='nuwax-integrity-'+name+'-') as cwd:
        subprocess.run([str(exe),*arguments],cwd=cwd,check=True,timeout=timeout)
        if name=='core':
            spec=importlib.util.spec_from_file_location('evidence_decoder',root/'tools/analyze-sd-integrity-evidence.py')
            decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
            evidence=Path(cwd)/'evidence/evidence_0.bin'
            decoded=decoder.decode(evidence)
            assert decoded['first_difference']==519 and decoded['stage']=='record_readback' and decoded['observed_block_file_offset']==512
            for at in [20,520,len(evidence.read_bytes())-1]:
                data=bytearray(evidence.read_bytes());data[at]^=1;bad=Path(cwd)/'bad.bin';bad.write_bytes(data)
                try: decoder.decode(bad)
                except ValueError: pass
                else: raise AssertionError(('decoder accepted corruption',at))
            print('PASS evidence decoder: actual production file plus header/shadow/read-block corruption rejection')

crc=r'''
static uint32_t crc32_update(uint32_t crc,const void *v,size_t n){
 const uint8_t *p=v;crc=~crc;while(n--){crc^=*p++;for(unsigned b=0;b<8;b++)crc=(crc>>1)^((crc&1)?0xedb88320U:0);}return ~crc;}
'''
core=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sd_audio_integrity.h"
'''+crc+r'''
int main(void){
 sd_integrity_t *s=sd_integrity_create(crc32_update);assert(s);uint8_t header[96]={0}, frame[662], observed[512];
 for(unsigned i=0;i<sizeof(frame);i++)frame[i]=(uint8_t)(i*73U+11U);
 assert(sd_integrity_begin(s,"meeting_test","card_test.part",7,38,header,sizeof(header)));
 for(unsigned n=0;n<100;n++){memcpy(frame+10,&n,4);sd_integrity_record(s,sizeof(frame),frame,crc32_update(0,frame,sizeof(frame)),true,crc32_update(0,frame,sizeof(frame)));}
 assert(s->shadow_bytes==66696 && s->body_end==66696 && s->shadow_covered);
 const uint32_t expected=s->shadow_crc;assert(expected==crc32_update(0,s->shadow,s->shadow_bytes));
 sd_integrity_read_attempt(s);memcpy(observed,s->shadow+512,512);observed[7]^=0x40;
 sd_integrity_compare(s,observed,512,512);assert(s->first_difference==519 && s->differing_bytes==1 && s->observed_bytes==512);
 sd_integrity_freeze(s,SD_INTEGRITY_RECORD,690789,expected,expected^7U);assert(s->snapshot_crc==expected && s->frozen);
 assert(!sd_integrity_begin(s,"do_not_overwrite","new",8,1,header,96));
 uint32_t saved_crc=s->snapshot_crc;sd_integrity_verified(s,123);assert(s->snapshot_crc==saved_crc&&s->shadow_bytes==66696);
 char path[192];assert(sd_integrity_save(s,"evidence",path,sizeof(path))==ESP_OK&&s->saved);
 FILE *f=fopen(path,"rb");assert(f);uint8_t lead[512];assert(fread(lead,1,512,f)==512&&fclose(f)==0);assert(!memcmp(lead,"NWXDIA2",7));
 for(unsigned i=0;i<3;i++){assert(sd_integrity_begin(s,"next","next",8,i,header,96));sd_integrity_freeze(s,1,1,0,0);assert(sd_integrity_save(s,"evidence",path,sizeof(path))==ESP_OK);}
 assert(sd_integrity_begin(s,"last","last",9,1,header,96));sd_integrity_freeze(s,1,1,0,0);assert(sd_integrity_save(s,"evidence",path,sizeof(path))==ESP_ERR_NO_MEM&&!s->saved);
 puts("PASS actual core: exact 66696-byte batch, first-difference block, frozen snapshot, exclusive four evidence slots");
 sd_integrity_destroy(s);s=sd_integrity_create(crc32_update);assert(sd_integrity_begin(s,"edges","part",1,1,header,96));
 sd_integrity_record(s,662,frame,0,false,0);uint8_t tail[512],footer[32];memcpy(tail,s->tail,512);memset(footer,0x6b,32);
 sd_integrity_verified(s,762);sd_integrity_prepare_seal(s,0x0102030405060708ULL,1234,footer,32);
 assert(!memcmp(s->tail,tail+32,480)&&!memcmp(s->tail+480,footer,32)&&s->head[84]==8&&s->head[91]==1);
 uint8_t first[512];memcpy(first,s->tail,512);sd_integrity_prepare_seal(s,9,4,footer,32);assert(!memcmp(first,s->tail,512));
 sd_integrity_seal_difference(s,762+32-512,s->tail,tail,512);assert(s->offset==282 && s->shadow_bytes==512&&s->differing_bytes);
 sd_integrity_freeze(s,4,10,1,2);s->shadow[0]^=1;assert(s->snapshot_crc!=crc32_update(0,s->shadow,s->shadow_bytes));
 sd_integrity_destroy(s);s=sd_integrity_create(crc32_update);assert(sd_integrity_begin(s,"overflow","part",1,1,header,96));
 uint8_t big[2048]={0};for(unsigned n=0;n<100;n++)sd_integrity_record(s,2048,big,0,false,0);
 assert(!s->shadow_covered&&s->shadow_bytes<=SD_INTEGRITY_SHADOW_BYTES&&s->body_end==96+100*2052);
 sd_integrity_verified(s,s->body_end);assert(s->shadow_covered&&s->shadow_bytes==0);
 printf("PASS production core size=%zu; edge/time/footer, retry idempotence, shadow mutation, oversized coverage bounds\n",sizeof(*s));sd_integrity_destroy(s);
 return 0;}
'''
run('core',core,[root/'main/sd_audio_integrity.c'])

io=(root/'main/board/sd_integrity_io.c').read_text(encoding='utf-8')
io=re.sub(r'^#include "(?:esp_heap_caps|esp_rom_crc|esp_timer|sdmmc_cmd|freertos/FreeRTOS|freertos/task)\.h"\n','',io,flags=re.M)
io_fixture=r'''
#include <assert.h>
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <errno.h>
#include "esp_err.h"
typedef SRWLOCK portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED SRWLOCK_INIT
#define portENTER_CRITICAL(p) AcquireSRWLockExclusive(p)
#define portEXIT_CRITICAL(p) ReleaseSRWLockExclusive(p)
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_INTERNAL 4U
#define MALLOC_CAP_DMA 8U
typedef struct {uint32_t timeout_ms;} sdmmc_command_t;
typedef struct {struct {uint32_t sector_size;size_t capacity;} csd;
 struct {esp_err_t (*do_transaction)(int,sdmmc_command_t*);uint32_t command_timeout_ms;}host;} sdmmc_card_t;
static atomic_int_fast64_t clock_us=1000000;
static int64_t esp_timer_get_time(void){return atomic_load(&clock_us);}
static uintptr_t xTaskGetCurrentTaskHandle(void){return (uintptr_t)GetCurrentThreadId();}
static bool alloc_fail;static size_t internal_free=99999;
static void *heap_caps_calloc(size_t n,size_t b,unsigned c){(void)c;return alloc_fail?NULL:calloc(n,b);}
static void *heap_caps_malloc(size_t b,unsigned c){(void)c;return alloc_fail?NULL:malloc(b);}
static void heap_caps_free(void *p){free(p);}
static size_t heap_caps_get_free_size(unsigned c){(void)c;return internal_free;}
'''+crc+r'''
static uint32_t esp_rom_crc32_le(uint32_t c,const void *p,size_t n){return crc32_update(c,p,n);}
static uint8_t disk[2048][512];static bool sdk_fail,change_source,change_read;
static unsigned calls,seen_timeout;static size_t normal_sector;static uint64_t advance;
static esp_err_t normal_transaction(int slot,sdmmc_command_t *cmd){(void)slot;seen_timeout=cmd->timeout_ms;atomic_fetch_add(&clock_us,advance);return ESP_OK;}
static esp_err_t sdk(sdmmc_card_t *card,void *p,size_t sec,size_t n,bool write){
 ++calls;sdmmc_command_t cmd={1000};esp_err_t e=card->host.do_transaction(0,&cmd);if(e!=ESP_OK){errno=91;return e;}
 if(sdk_fail){errno=87;return ESP_FAIL;}assert(sec+n<=2048);
 if(write){memcpy(disk[sec],p,n*512);if(change_source)((uint8_t*)p)[0]^=1;}
 else{memcpy(p,disk[sec],n*512);if(change_read && card->host.do_transaction==normal_transaction)((uint8_t*)p)[0]^=1;}
 errno=77;return ESP_OK;
}
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *c,void *p,size_t s,size_t n){return sdk(c,p,s,n,false);}
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *c,const void *p,size_t s,size_t n){return sdk(c,(void*)p,s,n,true);}
'''+io+r'''
static sdmmc_card_t card={.csd={512,2048},.host={normal_transaction,1000}};
static DWORD WINAPI foreign_writer(LPVOID p){assert(nuwax_sd_integrity_io_transfer(&card,p,normal_sector,1,true)==ESP_OK);return 0;}
int main(void){
 alloc_fail=true;assert(!nuwax_sd_integrity_io_init());alloc_fail=false;assert(nuwax_sd_integrity_io_init());
 uint8_t block[512],got[512];memset(block,0x42,512);nuwax_sd_integrity_io_report_t report;
 nuwax_sd_integrity_io_arm();assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_OK&&errno==77);
 nuwax_sd_integrity_io_read_phase(true);assert(nuwax_sd_integrity_io_transfer(&card,got,10,1,false)==ESP_OK&&errno==77);
 nuwax_sd_integrity_io_freeze(&report);assert(report.matched_sectors==1&&!report.raw_attempts);
 puts("PASS actual SDK observer: normal result, errno and bytes unchanged, no healthy raw reread");
 nuwax_sd_integrity_io_arm();assert(nuwax_sd_integrity_io_transfer(&card,block,11,1,true)==ESP_OK);disk[11][7]^=0x40;
 nuwax_sd_integrity_io_read_phase(true);assert(nuwax_sd_integrity_io_transfer(&card,got,11,1,false)==ESP_OK&&errno==77);
 assert(nuwax_sd_integrity_io_transfer(&card,got,11,1,false)==ESP_OK);nuwax_sd_integrity_io_freeze(&report);
 assert(report.mismatched_sectors==2&&report.raw_attempts==1&&report.raw_valid&&report.observed_crc==report.raw_crc&&report.expected_crc!=report.raw_crc);
 nuwax_sd_integrity_io_arm();assert(nuwax_sd_integrity_io_transfer(&card,block,12,1,true)==ESP_OK);change_read=true;nuwax_sd_integrity_io_read_phase(true);
 assert(nuwax_sd_integrity_io_transfer(&card,got,12,1,false)==ESP_OK&&errno==77);change_read=false;nuwax_sd_integrity_io_freeze(&report);
 assert(report.mismatched_sectors==1&&report.raw_valid&&report.raw_crc==report.expected_crc&&report.raw_crc!=report.observed_crc);
 puts("PASS actual SDK observer: persistent media-byte change distinguished from regular read-path change, one raw comparison, no result masking");
 nuwax_sd_integrity_io_arm();assert(nuwax_sd_integrity_io_transfer(&card,block,13,1,true)==ESP_OK);memset(block,0x73,512);normal_sector=13;
 HANDLE thread=CreateThread(NULL,0,foreign_writer,block,0,NULL);assert(thread);assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
 nuwax_sd_integrity_io_read_phase(true);assert(nuwax_sd_integrity_io_transfer(&card,got,13,1,false)==ESP_OK);nuwax_sd_integrity_io_freeze(&report);
 assert(report.foreign_writes==1&&report.matched_sectors==1&&!report.mismatched_sectors);
 nuwax_sd_integrity_io_arm();change_source=true;assert(nuwax_sd_integrity_io_transfer(&card,block,14,1,true)==ESP_OK);change_source=false;
 nuwax_sd_integrity_io_freeze(&report);assert(report.source_mutations==1);
 nuwax_sd_integrity_io_arm();assert(nuwax_sd_integrity_io_transfer(&card,block,15,1,true)==ESP_OK);sdk_fail=true;
 assert(nuwax_sd_integrity_io_transfer(&card,block,15,1,true)==ESP_FAIL&&errno==87);sdk_fail=false;nuwax_sd_integrity_io_read_phase(true);
 assert(nuwax_sd_integrity_io_transfer(&card,got,15,1,false)==ESP_OK);nuwax_sd_integrity_io_freeze(&report);assert(report.sdk_errors==1&&report.unknown_sectors==1);
 puts("PASS actual SDK observer: cross-task writes replace witnesses, input mutation detected, failed write cannot certify data");
 nuwax_sd_integrity_io_arm();for(unsigned n=0;n<600;n++)assert(nuwax_sd_integrity_io_transfer(&card,block,n,1,true)==ESP_OK);
 nuwax_sd_integrity_io_read_phase(true);assert(nuwax_sd_integrity_io_transfer(&card,got,1700,1,false)==ESP_OK);nuwax_sd_integrity_io_freeze(&report);assert(report.evictions&&report.unknown_sectors);
 assert(nuwax_sd_integrity_io_budget_begin(200));advance=201000;assert(nuwax_sd_integrity_io_transfer(&card,got,10,1,false)==ESP_OK);advance=0;
 unsigned before=calls;assert(nuwax_sd_integrity_io_transfer(&card,got,10,1,false)==ESP_ERR_TIMEOUT&&calls==before);nuwax_sd_integrity_io_budget_end();
 assert(seen_timeout<=200&&card.host.do_transaction==normal_transaction&&card.host.command_timeout_ms==1000);
 nuwax_sd_integrity_io_arm();assert(nuwax_sd_integrity_io_transfer(&card,block,19,1,true)==ESP_OK);change_read=true;internal_free=16000;nuwax_sd_integrity_io_read_phase(true);
 assert(nuwax_sd_integrity_io_transfer(&card,got,19,1,false)==ESP_OK);nuwax_sd_integrity_io_freeze(&report);change_read=false;assert(!report.raw_valid&&report.raw_error==ESP_ERR_NO_MEM);
 puts("PASS actual SDK observer: bounded/partial sector coverage, command deadline on private descriptor, DMA reserve respected");
 printf("PASS production IO observer size=%zu\n",sizeof(io_state_t));return 0;}
'''
run('sdk',io_fixture)

recovery=(root/'main/sd_audio_active_recovery_host_check.c').read_text(encoding='utf-8')
needle='    reset();request();replay=true;'
assert needle in recovery
recovery=recovery.replace(needle,r'''
    const char *stages[]={"frame_queue_crc","frame_write_crc","record_readback_crc","seal_readback_crc","seal_readback_size"};
    for(unsigned n=0;n<sizeof(stages)/sizeof(stages[0]);n++){
        reset();s_writer_fault_stage=stages[n];s.file=(FILE*)1;
        assert(!card_io_recovery_request_locked(ESP_FAIL,NULL)&&s_card_failed&&stop_requested&&!s.capture_complete&&!s_card_io_recovery.pending&&!mounts);
    }
    puts("PASS actual recovery owner diagnostics ON: every confirmed integrity stage stops capture incomplete, no remount-pass restart");
'''+needle,1)
run('recovery','#define CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS 1\n'+recovery)

# Compile actual storage owner functions with diagnostics ON, not only the
# legacy fixture configuration where these hooks are preprocessed out.
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
storage=(root/'main/sd_audio_session_storage.inc').read_text(encoding='utf-8-sig')
pieces={
 'types':spool[spool.index('typedef struct {\n    uint8_t type;'):spool.index('static spool_state_t s;')],
 'crc':spool[spool.index('static uint32_t crc32_update('):spool.index('static esp_err_t validate_archive_frame(')],
 'catalog':storage[storage.index('static bool card_recording_locked('):storage.rindex('#endif')],
 'functions':spool[spool.index('static FILE *archive_create_exclusive('):spool.index('static void writer_schedule_retry_locked(')],
 'loss':spool[spool.index('static void mirror_note_loss('):spool.index('static void mirror_drain(')]}
for name,source in pieces.items():
    if name=='functions':
        source=source.replace('fread(block, 1U, n, readback)', 'fault_read(block, 1U, n, readback)')
        source=source.replace('fread(block, 1U, sizeof(block), file)','fault_read(block, 1U, sizeof(block), file)')
        source=source.replace('fwrite(item->data, 1U, item->bytes, s.file)', 'fault_write(item->data, 1U, item->bytes, s.file)')
    (out/('sd_storage_'+name+'_host.inc')).write_text(source,encoding='utf-8')
fixture=(root/'main/sd_audio_storage_host_check.c').read_text(encoding='utf-8')
hooks=(root/'main/sd_audio_integrity_writer.inc').read_text(encoding='utf-8')
hooks=hooks.replace('"/sdcard/sd_integrity"', '"local_evidence"')
hooks=hooks.replace('fread(block, 1U, sizeof(block), file)', 'fault_read(block, 1U, sizeof(block), file)')
hooks=hooks.replace('fstat(fileno(file), &st)', 'fault_stat(fileno(file), &st)')
prefix=r'''
#define CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES 4194304U
#include "sd_audio_integrity.h"
#include "sd_capture_admission.h"
static void nuwax_sd_integrity_io_arm(void){}
static void nuwax_sd_integrity_io_read_phase(bool value){(void)value;}
static void nuwax_sd_integrity_io_freeze(nuwax_sd_integrity_io_report_t *p){if(p)memset(p,0,sizeof(*p));}
static bool corrupt_read,corrupt_head,corrupt_size,corrupt_write,disk_header,disk_tail;
static size_t fault_read(void *p,size_t n,size_t count,FILE *file){size_t got=fread(p,n,count,file);if((corrupt_read||corrupt_head)&&got)((uint8_t*)p)[0]^=0x40;return got;}
static size_t fault_write(const void *p,size_t n,size_t count,FILE *file){size_t got=fwrite(p,n,count,file);if(corrupt_write&&got)((uint8_t*)p)[100]^=1;return got;}
static int fault_stat(int fd,struct stat *st){int e=fstat(fd,st);if(!e&&corrupt_size)st->st_size--;return e;}
'''
# Prototypes above need the real API linkage and system headers.
prefix=prefix.replace('static void nuwax_sd_integrity_io_', 'void nuwax_sd_integrity_io_')
deferred=r'''
static atomic_bool s_media_ready=true;
static sd_capture_admission_t s_capture_admission;
static bool replay_busy,budget_busy;
static unsigned budget_calls;
bool nuwax_sd_integrity_io_budget_begin(uint32_t ms){assert(ms==750);++budget_calls;return !budget_busy;}
void nuwax_sd_integrity_io_budget_end(void){}
static bool nuwax_offline_audio_replay_running(void){return replay_busy;}
static bool nuwax_offline_audio_storage_busy(void){return budget_busy;}
#define pdTRUE 1
static int xSemaphoreTake(void *lock,unsigned wait){(void)lock;(void)wait;return 1;}
static void xSemaphoreGive(void *lock){(void)lock;}
'''
fixture=fixture.replace('#include "sd_storage_functions_host.inc"',deferred+hooks+'\n#include "sd_storage_functions_host.inc"')
fixture=fixture.replace('memset(&s, 0, sizeof(s));','sd_integrity_destroy(s.integrity); memset(&s, 0, sizeof(s)); s.integrity=sd_integrity_create(crc32_update);assert(s.integrity);')
fixture=fixture.replace('assert(append_locked(&item) == ESP_OK', 'item.queued_crc_valid=true;item.queued_crc=crc32_update(0,item.data,item.bytes);assert(append_locked(&item) == ESP_OK')
fixture=fixture.replace('int main(', 'int old_storage_main(')
fixture=fixture.replace('return fflush(f) == 0 && _commit(fileno(f)) == 0;',r'''
    bool ok=fflush(f)==0&&_commit(fileno(f))==0;
    if(ok&&!strcmp(step,"footer_sync")&&disk_header){uint8_t bad=0x42;assert(fseek(f,0,SEEK_SET)==0&&fwrite(&bad,1,1,f)==1&&fflush(f)==0);}
    if(ok&&!strcmp(step,"footer_sync")&&disk_tail){long end=ftell(f);assert(end>32&&ftruncate(fileno(f),end-5)==0);}
    return ok;''')
writer=r'''
int main(int argc,char **argv){(void)argv;assert(sd_audio_catalog_init()==ESP_OK);
 begin("healthy",0);for(unsigned n=0;n<500;n++)frame(n,true);assert(commit_segment_locked(true)==ESP_OK);assert(!s.integrity->active);part(0,0);
 puts("PASS actual writer diagnostics ON: header, periodic batches and sealed file retain original audio/counts/CRC/time");
 begin("queue_changed",0);frame(0,true);spool_item_t item={.type=1,.generation=1,.realtime=true};int16_t pcm[320]={0};
 assert(nuwax_audio_frame_build_pcm16(item.data,sizeof(item.data),pcm,320,1,(uint32_t)clock_ms,0,16000,1,20,&item.bytes)==ESP_OK);
 item.queued_crc_valid=true;item.queued_crc=crc32_update(0,item.data,item.bytes);item.data[100]^=1;
 s_writer_fault_stage="earlier_recovered_io";assert(append_locked(&item)==ESP_ERR_INVALID_CRC&&s.integrity->stage==1&&!strcmp(s_writer_fault_stage,"frame_queue_crc"));
 assert(commit_segment_locked(false)==ESP_OK);assert(s.integrity->frozen);
 unsigned budgets=budget_calls;writer_integrity_save_if_idle();assert(budget_calls==budgets);
 s.active=false;card_active=false;s.capture_running=true;writer_integrity_save_if_idle();assert(budget_calls==budgets);
 s.capture_running=false;s.writer_busy=true;writer_integrity_save_if_idle();assert(budget_calls==budgets);
 s.writer_busy=false;replay_busy=true;writer_integrity_save_if_idle();assert(budget_calls==budgets);
 replay_busy=false;budget_busy=true;writer_integrity_save_if_idle();assert(!s.integrity->save_attempts);
 budget_busy=false;sd_capture_admission_hold(&s_capture_admission,0,(uint32_t)clock_ms+5000U);
 writer_integrity_save_if_idle();assert(!s.integrity->save_attempts);
 sd_capture_admission_release(&s_capture_admission,0);
 budget_busy=false;writer_integrity_save_if_idle();assert(s.integrity->saved&&s.integrity->save_attempts==1);
 budgets=budget_calls;writer_integrity_save_if_idle();assert(budget_calls==budgets);card_active=true;
 puts("PASS actual deferred export: capture/session/writer/replay/start-reservation gates, fixed local evidence, saved snapshot not exported again");
 begin("source_changed_during_write",0);frame(0,true);
 item.generation=s.capture_generation;item.queued_crc_valid=false;corrupt_write=true;s_writer_fault_stage=NULL;
 assert(append_locked(&item)==ESP_ERR_INVALID_CRC&&s.integrity->stage==2&&!strcmp(s_writer_fault_stage,"frame_write_crc"));
 corrupt_write=false;assert(commit_segment_locked(false)==ESP_OK);
 begin("read_changed",1);frame(0,true);corrupt_read=true;s_writer_fault_stage="earlier_recovered_io";
 for(unsigned n=1;n<100;n++){
  item.generation=s.capture_generation;item.queued_crc_valid=false;
  assert(nuwax_audio_frame_build_pcm16(item.data,sizeof(item.data),pcm,320,n,(uint32_t)clock_ms,0,16000,1,20,&item.bytes)==ESP_OK);
  esp_err_t e=append_locked(&item);assert(n==99?e==ESP_FAIL:e==ESP_OK);clock_ms+=20;
 }
 corrupt_read=false;assert(s.integrity->frozen&&s.integrity->stage==3&&s.integrity->differing_bytes&&s.integrity->snapshot_crc==s.integrity->expected_crc&&!strcmp(s_writer_fault_stage,"record_readback_crc"));
 assert(commit_segment_locked(false)==ESP_OK);
 puts("PASS actual writer diagnostics ON: queue change and independent read corruption freeze exact prewrite batch, original CRC guard rejects");
 begin("seal_changed",0);frame(0,true);frame(1,true);
 // Flush/verify tail first, then corrupt only the newly added fresh seal read.
 assert(sync_archive_file(s.file,"test")&&writer_verify_records_locked()==ESP_OK);corrupt_head=true;s_writer_fault_stage=NULL;
 assert(commit_segment_locked(true)==ESP_ERR_INVALID_CRC&&!s.file&&path_exists(s.open_path)&&!s_card_pending_upload);
 assert(s.integrity->stage==4&&s.integrity->frozen&&s.integrity->shadow_bytes==512);corrupt_head=false;
 puts("PASS actual writer diagnostics ON: header/footer readback corruption cannot rename, seal, publish or delete .part");
 begin("seal_bad_size",0);frame(0,true);corrupt_size=true;s_writer_fault_stage=NULL;
 assert(commit_segment_locked(true)==ESP_ERR_INVALID_SIZE&&!s.file&&path_exists(s.open_path)&&!s_card_pending_upload);
 assert(s.integrity->stage==5&&s.integrity->frozen);corrupt_size=false;
 puts("PASS actual writer diagnostics ON: source mutation during fwrite and persisted file extent failures are independently detected");
 begin("actual_bad_header",0);frame(0,true);disk_header=true;s_writer_fault_stage=NULL;
 assert(commit_segment_locked(true)==ESP_ERR_INVALID_CRC&&!s.file&&path_exists(s.open_path)&&!s_card_pending_upload);disk_header=false;
 begin("actual_bad_tail",0);frame(0,true);disk_tail=true;s_writer_fault_stage=NULL;
 assert(commit_segment_locked(true)==ESP_ERR_INVALID_SIZE&&!s.file&&path_exists(s.open_path)&&!s_card_pending_upload);disk_tail=false;
 puts("PASS actual writer diagnostics ON: actual file header corruption and truncated footer retained, never published");
 begin("optional_no_memory",0);sd_integrity_destroy(s.integrity);s.integrity=NULL;frame(0,true);assert(commit_segment_locked(true)==ESP_OK);part(0,0);
 puts("PASS actual writer diagnostics ON: optional shadow allocation failure leaves original recording/readback available");
 if(argc>1)for(unsigned mode=0;mode<2;mode++){
  begin(mode?"monitor_8h_diag":"meeting_8h_diag",mode);uint32_t cursor=0,index=0,total=8*60*60*50;
  for(unsigned n=0;n<total;n++){unsigned prev=s_card_session.segment_count;frame(n,(n/(60*60*50))%2==0);
   if(s_card_session.segment_count>prev&&n){sd_audio_segment_t f=part(index++,cursor);cursor+=(f.total_bytes-128)/666;assert(cursor==n);}}
  assert(commit_segment_locked(true)==ESP_OK);sd_audio_segment_t f=part(index++,cursor);cursor+=(f.total_bytes-128)/666;
  assert(cursor==total&&index==s_card_session.segment_count&&!s.dropped_frames&&!s.integrity->frozen);
  printf("PASS actual writer diagnostics ON: mode=%u eight hours frames=%u files=%u no sequence/count/CRC/time gap\n",mode,total,index);
 }
 return 0;}
'''
flags+=['-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"','-Wno-misleading-indentation']
run('writer','#include <stdio.h>\n#include <string.h>\n#include <sys/stat.h>\n'+prefix+fixture+writer,
    [root/'main/audio_frame.c',root/'main/sd_audio_catalog.c',root/'main/sd_audio_integrity.c'],
    ['long'] if args.long else [],900 if args.long else 90)

print('PASS production diagnostics core + SDK observer + actual recording owner (enabled configuration)')
