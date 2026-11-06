"""Fault injection of the actual SD writer, repair and SDK fence; no device I/O."""
from pathlib import Path
import ast,ctypes,os,re,subprocess,tempfile,sys

root=Path(__file__).resolve().parents[1]
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
long_run='--long' in sys.argv
out=root/'build-host-checks/sd-batch-repair'/('long' if long_run else 'short');out.mkdir(parents=True,exist_ok=True)
# Reuse the actual-owner fixture generator, not a reimplementation of its
# recording loop. Capture generated translation units without running its tests.
source=(root/'tools/test-sd-integrity-diagnostics.py').read_text(encoding='utf-8')
source=source.replace("out=root/'build-host-checks/sd-integrity-diagnostics'/('long' if args.long else 'short')", "out=root/'build-host-checks/sd-batch-repair'/('long' if args.long else 'short')")
tree=ast.parse(source)
for node in tree.body:
    if isinstance(node,ast.FunctionDef) and node.name=='run':
        node.body=ast.parse('fixtures[name]=(code,units,arguments,timeout)').body
ns={'__file__':str(root/'tools/test-sd-integrity-diagnostics.py'),'fixtures':{},'print':lambda *a,**k:None}
exec(compile(ast.fix_missing_locations(tree),str(root/'tools/test-sd-integrity-diagnostics.py'),'exec'),ns)
flags=ns['flags']
def execute(name,code,units=()):
    path=out/(name+'.c');path.write_text(code,encoding='utf-8');exe=out/(name+'.exe')
    subprocess.run(flags+[str(path),*map(str,units),'-o',str(exe)],check=True)
    with tempfile.TemporaryDirectory(prefix='nuwax-batch-'+name+'-') as cwd:
        subprocess.run([str(exe)],cwd=cwd,check=True,timeout=900 if long_run else 180)

base,units,_,_=ns['fixtures']['writer']
base=base[:base.index('int main(int argc,char **argv)')]
base='#define CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE 1\n#define CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE 1\n'+base
base=base.replace('static int64_t esp_timer_get_time(void)', 'static uint64_t work_us;\nstatic int64_t esp_timer_get_time(void)',1)
base=base.replace('return (int64_t)clock_ms * 1000;', 'return (int64_t)clock_ms * 1000 + work_us;',1)
base=base.replace('#include "sd_audio_integrity.h"','#include "sd_audio_integrity.h"\n#include "board/sd_card_driver.h"\n#include "board/sd_write_verify.h"',1)
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
validate=spool[spool.index('static esp_err_t validate_archive_frame('):spool.index('static void release_slot(')]
dependencies=r'''
static atomic_bool s_shutdown_requested;
static atomic_uint s_media_generation;
static struct {bool identity_valid;nuwax_sd_card_identity_t identity;} s_card_io_recovery;
static bool power_down,identity_changed,scope_busy,scope_abort,scope_active,budget_active;
static unsigned scopes,aborts,ends,repair_writes;
static bool nuwax_power_ctrl_shutdown_preparing(void){return power_down;}
bool nuwax_sd_card_get_identity(nuwax_sd_card_identity_t *v){memset(v,0,sizeof(*v));if(identity_changed)v->bytes[0]=1;return true;}
bool nuwax_sd_integrity_io_repair_begin(void){if(scope_busy)return false;++scopes;scope_active=true;scope_abort=false;return true;}
void nuwax_sd_integrity_io_repair_abort(void){assert(scope_active);scope_abort=true;++aborts;}
void nuwax_sd_integrity_io_repair_end(void){assert(scope_active);scope_active=false;++ends;}
nuwax_sd_write_verify_stats_t nuwax_sd_write_verify_stats(void){return (nuwax_sd_write_verify_stats_t){0};}
static void vTaskDelay(unsigned ticks){work_us+=ticks*1000U;}
static bool permanent_fault,repair_write_error,repair_sync_error,repair_prefix_io,slow_repair,first_frame_damage,periodic_damage,footer_damage,delayed_prefix;
static unsigned cancel_at,change_media_at,change_identity_at;
static size_t repair_fwrite(const void *p,size_t n,size_t count,FILE *f){
 if(scope_active){++repair_writes;if(slow_repair)work_us+=9000000;if(repair_write_error){errno=EIO;return 0;}
  if(cancel_at==repair_writes)s_shutdown_requested=true;
  if(change_media_at==repair_writes)atomic_fetch_add(&s_media_generation,1);
  if(change_identity_at==repair_writes)identity_changed=true;
 }
 return fwrite(p,n,count,f);}
static size_t prefix_fread(void*p,size_t n,size_t count,FILE*f){if(repair_prefix_io)return 0;if(delayed_prefix){work_us+=3000000;delayed_prefix=false;}return fread(p,n,count,f);}
static void flip(FILE *f,long at){long end=ftell(f);uint8_t byte;assert(fseek(f,at,SEEK_SET)==0&&fread(&byte,1,1,f)==1);byte^=0x40;assert(fseek(f,at,SEEK_SET)==0&&fwrite(&byte,1,1,f)==1&&fflush(f)==0&&fseek(f,end,SEEK_SET)==0);}
'''
base=base.replace('static bool fail_first_frame_sync;',dependencies+'\nstatic bool fail_first_frame_sync;',1)
base=base.replace('bool nuwax_sd_integrity_io_budget_begin(uint32_t ms){assert(ms==750);++budget_calls;return !budget_busy;}',
    'bool nuwax_sd_integrity_io_budget_begin(uint32_t ms){assert(ms==750||ms==NUWAX_SD_REPAIR_BUDGET_MS);++budget_calls;if(budget_busy||scope_busy||scope_active||budget_active)return false;budget_active=true;return true;}')
base=base.replace('void nuwax_sd_integrity_io_budget_end(void){}',
    'void nuwax_sd_integrity_io_budget_end(void){assert(budget_active);budget_active=false;}')
base=base.replace('#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS\nstatic bool writer_integrity_live',validate+'\n#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS\nstatic bool writer_integrity_live',1)
base=base.replace('static bool nuwax_sd_card_is_mounted(', 'bool nuwax_sd_card_is_mounted(')
base=base.replace('static void nuwax_sd_card_mark_io_fault(', 'void nuwax_sd_card_mark_io_fault(')
base=base.replace('assert(append_locked(&item) == ESP_OK && s.writer_frame_written);',r'''esp_err_t e=append_locked(&item);
    if(e!=ESP_OK||!s.writer_frame_written)fprintf(stderr,"actual append failure mode=%u seq=%u frame=%u stage=%s err=%d offset=%ld bytes=%u scopes=%u\n",s.mode,sequence,s.frame_count,s_writer_fault_stage?s_writer_fault_stage:"-",e,s.verify_offset,s.verify_bytes,scopes);
    assert(e==ESP_OK&&s.writer_frame_written);''')
# The production include is compiled verbatim except the I/O injection seams.
repair=(root/'main/sd_audio_batch_repair.inc').read_text(encoding='utf-8')
repair=repair.replace('fwrite(diag->shadow + written, 1U, count, s.file)','repair_fwrite(diag->shadow + written, 1U, count, s.file)')
repair=repair.replace('fread(&header, 1U, sizeof(header), file)','prefix_fread(&header, 1U, sizeof(header), file)')
(out/'sd_audio_batch_repair_injected.inc').write_text(repair,encoding='utf-8')
functions=out/'sd_storage_functions_host.inc'
text=functions.read_text(encoding='utf-8').replace('#include "sd_audio_batch_repair.inc"','#include "sd_audio_batch_repair_injected.inc"')
functions.write_text(text,encoding='utf-8')
base=base.replace('#include "sd_storage_functions_host.inc"','#include "sd_audio_batch_repair_injected.inc"\n#include "sd_storage_functions_host.inc"')
base=base.replace('bool ok=fflush(f)==0&&_commit(fileno(f))==0;', '''
    bool ok=fflush(f)==0&&_commit(fileno(f))==0;
    if(first_frame_damage&&!strcmp(step,"first_frame_sync")){flip(f,200);first_frame_damage=false;}
    if(periodic_damage&&!strcmp(step,"periodic_sync")){flip(f,s.verify_offset+200);periodic_damage=false;}
    if(footer_damage&&!strcmp(step,"footer_sync")){flip(f,2000);footer_damage=false;}
    if(scope_active&&!strcmp(step,"batch_repair_sync")){
     if(permanent_fault)flip(f,s.verify_offset+200);
     if(repair_sync_error)return false;
    }''')
recovery=spool[spool.index('static esp_err_t recover_open_segments(void)'):spool.index('\nstatic uint64_t scan_used_bytes(void)')]
base+=r'''
#include <dirent.h>
#define fsync _commit
#define ESP_LOGI(...) log_ignore(__VA_ARGS__)
typedef struct {int error;bool read;struct dirent entry;}archive_dir_t;
static archive_dir_t *archive_dir_open(archive_dir_t*p){memset(p,0,sizeof(*p));snprintf(p->entry.d_name,sizeof(p->entry.d_name),"%s",s.open_path);return p;}
static struct dirent *archive_dir_read(archive_dir_t*p){if(p->read)return NULL;p->read=true;return &p->entry;}
static void archive_dir_close(archive_dir_t*p){(void)p;}
static bool compose_child_path(char*out,size_t n,const char*name){return snprintf(out,n,"%s",name)>0&&strlen(name)<n;}
'''+recovery
tests=r'''
static void start_mode(const char *id,unsigned mode){
 begin(id,mode);s_card_io_recovery.identity_valid=true;memset(&s_card_io_recovery.identity,0,sizeof(s_card_io_recovery.identity));
 s_media_ready=true;s_shutdown_requested=false;power_down=identity_changed=scope_busy=scope_abort=scope_active=budget_active=false;
 permanent_fault=repair_write_error=repair_sync_error=repair_prefix_io=slow_repair=first_frame_damage=periodic_damage=footer_damage=delayed_prefix=false;
 cancel_at=change_media_at=change_identity_at=0;scopes=aborts=ends=repair_writes=0;
 s_writer_fault_stage=NULL;card_active=true;
}
static void start(const char *id){start_mode(id,0);}
static void pending(unsigned from,unsigned to){for(unsigned n=from;n<to;n++)frame(n,true);assert(sync_archive_file(s.file,"test"));}
static void damage(void){assert(s.verify_bytes>200);flip(s.file,s.verify_offset+200);}
static void retained(void){assert(!s_card_pending_upload);if(s.file){fclose(s.file);s.file=NULL;}assert(path_exists(s.open_path));}
int main(void){assert(sd_audio_catalog_init()==ESP_OK);
 start("healthy");pending(0,220);assert(commit_segment_locked(true)==ESP_OK&&!scopes);part(0,0);
 puts("PASS actual enabled writer: healthy recording does not enter repair or change file bytes/counts/time");
 start("delayed_recovery");pending(0,99);damage();uint64_t used=s.used_bytes;uint32_t frames=s.frame_count;long end=s.last_good_offset;
 assert(writer_verify_records_locked()==ESP_OK&&scopes==1&&ends==1&&!aborts&&s.file&&s.integrity->repaired_file&&!s.integrity->repair_attempted);
 assert(!s.integrity->frozen&&s.frame_count==frames&&s.last_good_offset==end&&s.used_bytes==used&&!s.dropped_frames&&!s.degraded);
 pending(99,220);assert(commit_segment_locked(true)==ESP_OK);part(0,0);
 puts("PASS actual enabled writer: delayed corruption restored from original batch, full prefix verified, same scene/file/PCM/time and no duplicate counters");
 start("measured_prefix_budget");pending(0,99);damage();delayed_prefix=true;
 assert(writer_verify_records_locked()==ESP_OK&&scopes==1&&!s.integrity->frozen&&work_us>=3000000);
 delayed_prefix=true;assert(commit_segment_locked(true)==ESP_OK&&!budget_active);part(0,0);
 puts("PASS multi-second prefix verification fits bounded repair budget without discarding original full-file CRC");
 start("seal_prefix_budget_busy");pending(0,99);damage();
 assert(writer_verify_records_locked()==ESP_OK&&s.integrity->repaired_file);scope_busy=true;
 assert(commit_segment_locked(true)!=ESP_OK&&!budget_active&&!path_exists(s.ready_path));retained();
 puts("PASS repaired-file seal cannot bypass a busy transaction deadline or publish without full-prefix proof");
 start("first_frame_recovery");
 first_frame_damage=true;frame(0,true);assert(scopes==1&&!s.integrity->frozen);assert(commit_segment_locked(true)==ESP_OK);part(0,0);
 start("repeat_batches");pending(0,99);damage();assert(writer_verify_records_locked()==ESP_OK);pending(99,199);damage();assert(writer_verify_records_locked()==ESP_OK&&scopes==2);assert(commit_segment_locked(true)==ESP_OK);part(0,0);
 start("user_stop_drain");pending(0,99);damage();s.closing=true;s.stop_requested=true;
 assert(writer_verify_records_locked()==ESP_OK&&s.closing&&s.stop_requested&&s.active);assert(commit_segment_locked(true)==ESP_OK);part(0,0);
 puts("PASS normal user stop drains accepted frames without restarting capture; independent later batches remain eligible");
 start("unrepaired_crc_quarantine");pending(0,99);damage();scope_busy=true;
 assert(writer_verify_records_locked()==ESP_ERR_INVALID_CRC&&s.integrity->frozen&&!s.integrity->repaired_file);
 assert(fclose(s.file)==0);s.file=NULL;char unbad[176];snprintf(unbad,sizeof(unbad),"%s.bad",s.open_path);
 assert(recover_open_segments()==ESP_OK&&!path_exists(s.open_path)&&path_exists(unbad)&&!path_exists(s.ready_path));
 puts("PASS confirmed CRC fault without any repair attempt is quarantined; no generic PCM checksum synthesis");
 start("no_optional_shadow");sd_integrity_destroy(s.integrity);s.integrity=NULL;pending(0,99);damage();
 assert(writer_verify_records_locked()==ESP_ERR_INVALID_CRC);assert(fclose(s.file)==0);s.file=NULL;
 snprintf(unbad,sizeof(unbad),"%s.bad",s.open_path);
 assert(recover_open_segments()==ESP_OK&&!path_exists(s.open_path)&&path_exists(unbad)&&!path_exists(s.ready_path));
 puts("PASS optional diagnostic allocation failure does not remove the known-CRC quarantine fence");
 start("permanent");pending(0,99);damage();permanent_fault=true;
 assert(writer_verify_records_locked()==ESP_ERR_INVALID_CRC&&scopes==1&&aborts==1&&ends==1&&!s.file&&s.integrity->frozen);
 assert(commit_segment_locked(false)==ESP_OK&&scopes==1);retained();
 bool handled=false;identity_changed=true;
 assert(writer_repair_quarantine_known(s.open_path,&handled)==ESP_OK&&handled&&path_exists(s.open_path));identity_changed=false;
 char bad_path[176];snprintf(bad_path,sizeof(bad_path),"%s.bad",s.open_path);
 FILE *sentinel=fopen(bad_path,"wb");assert(sentinel&&fwrite("keep",1,4,sentinel)==4&&fclose(sentinel)==0);
 assert(recover_open_segments()==ESP_OK&&path_exists(s.open_path)&&path_exists(bad_path)&&!path_exists(s.ready_path));
 struct stat bs;assert(stat(bad_path,&bs)==0&&bs.st_size==4);assert(remove(bad_path)==0);
 assert(recover_open_segments()==ESP_OK&&!path_exists(s.open_path)&&path_exists(bad_path)&&!path_exists(s.ready_path));
 puts("PASS actual ordered .part recovery: confirmed bad file quarantined without CRC synthesis; foreign card and existing quarantine never overwritten");
 sd_integrity_t *old_snapshot=s.integrity;s.integrity=NULL;
 start("new_scene_old_snapshot");sd_integrity_destroy(s.integrity);s.integrity=old_snapshot;
 pending(0,220);assert(commit_segment_locked(true)==ESP_OK&&!scopes&&old_snapshot->frozen&&!old_snapshot->saved);part(0,0);
 puts("PASS unsaved frozen evidence from an earlier repaired file cannot block a healthy new scene or overwrite original evidence");
 start("salvage_negative_control");pending(0,99);damage();permanent_fault=true;assert(writer_verify_records_locked()==ESP_ERR_INVALID_CRC&&!s.file);
 /* Negative control removes BOTH witnesses; production keeps the stage even
  * when the optional diagnostic allocation is unavailable. */
 sd_integrity_t *evidence=s.integrity;s.integrity=NULL;s_writer_fault_stage=NULL;assert(recover_open_segments()==ESP_OK&&path_exists(s.ready_path));
 FILE *salvaged=fopen(s.ready_path,"rb");archive_footer_t salvaged_footer;
 assert(salvaged&&fseek(salvaged,-32,SEEK_END)==0&&fread(&salvaged_footer,1,32,salvaged)==32&&fclose(salvaged)==0);
 assert(!salvaged_footer.complete&&salvaged_footer.crc32!=s.payload_crc32);s.integrity=evidence;
 puts("NEGATIVE CONTROL actual generic .part salvage without frozen fault: different PCM obtains a newly synthesized partial CRC; repair failure fence prevents this");
 start("earlier_prefix_bad");pending(0,99);assert(writer_verify_records_locked()==ESP_OK);pending(99,199);damage();flip(s.file,200);
 assert(writer_verify_records_locked()==ESP_ERR_INVALID_CRC&&scopes==1&&!s.file&&s.integrity->frozen);retained();
 start("seal_original_prefix");pending(0,99);damage();assert(writer_verify_records_locked()==ESP_OK);pending(99,220);assert(writer_verify_records_locked()==ESP_OK);flip(s.file,200);
 assert(commit_segment_locked(true)==ESP_OK&&!s.file&&s_card_pending_upload);assert(part(0,0).total_bytes==128U+220U*666U);
 puts("PASS seal reconstructs the first sector from the guarded original prefix; full repaired-prefix verification and original PCM CRC still match");
 start("seal_guard_corrupt");pending(0,220);s.seal_head->data[120]^=0x40;
 assert(commit_segment_locked(true)!=ESP_OK&&!s.file&&!s_card_pending_upload&&path_exists(s.open_path)&&!path_exists(s.ready_path));
 assert(s_writer_fault_stage&&!strcmp(s_writer_fault_stage,"seal_prefix_ram"));
 puts("PASS actual writer refuses an invalid original-prefix RAM guard; no ready file, footer success or upload publication");
 start("late_prefix_bad");pending(0,99);damage();assert(writer_verify_records_locked()==ESP_OK);pending(99,220);assert(writer_verify_records_locked()==ESP_OK);flip(s.file,1200);
 assert(commit_segment_locked(true)==ESP_ERR_INVALID_CRC&&!s.file&&!s_card_pending_upload);retained();
 start("footer_damaged_older_batch");pending(0,99);damage();assert(writer_verify_records_locked()==ESP_OK);pending(99,220);footer_damage=true;
 assert(commit_segment_locked(true)==ESP_ERR_INVALID_CRC&&!s.file&&!s_card_pending_upload);retained();
 puts("PASS persistent/current/earlier/later corruption cannot seal or publish; commit cannot repeat a failed batch repair");
 for(unsigned test=0;test<13;test++){
  char id[48];snprintf(id,sizeof(id),"skip_%u",test);start(id);pending(0,99);damage();
  if(test==0)s.integrity->shadow[200]^=1;
  if(test==1)s.integrity->shadow_bytes--;
  if(test==2)s.integrity->generation++;
  if(test==3)s.integrity->index++;
  if(test==4)strcpy(s.integrity->session_id,"foreign");
  if(test==5)strcpy(s.integrity->path,"foreign");
  if(test==6)s.integrity->shadow_covered=false;
  if(test==7)s_shutdown_requested=true;
  if(test==8)power_down=true;
  if(test==9)identity_changed=true;
  if(test==10)s_card_io_recovery.identity_valid=false;
  if(test==11)s.sample_rate=8000;
  if(test==12)scope_busy=true;
  assert(writer_verify_records_locked()==ESP_ERR_INVALID_CRC&&!scopes&&!repair_writes);retained();
 }
 puts("PASS invalid RAM CRC/extent/generation/file/scene/geometry/identity/owner and shutdown skip repair without writing");
 for(unsigned test=0;test<8;test++){
  char id[48];snprintf(id,sizeof(id),"abort_%u",test);start(id);pending(0,99);damage();
  if(test==0)repair_write_error=true;
  if(test==1)repair_sync_error=true;
  if(test==2)repair_prefix_io=true;
  if(test==3)cancel_at=1;
  if(test==4)change_media_at=1;
  if(test==5)change_identity_at=1;
  if(test==6)clock_ms=UINT32_MAX-10ULL;
  if(test==7)slow_repair=true;
  if(test==6){assert(writer_verify_records_locked()==ESP_OK);assert(commit_segment_locked(true)==ESP_OK);part(0,0);continue;}
  assert(writer_verify_records_locked()==ESP_ERR_INVALID_CRC&&scopes==1&&aborts==1&&ends==1&&!scope_active&&!s.file);
  retained();
 }
 puts("PASS partial writes, sync/read faults and mid-repair power/media/identity changes close under sticky write fence; clock wrap does not break duration budget");
 LONG_TESTS
 return 0;}
'''
long_tests=r'''
 for(unsigned mode=0;mode<2;mode++){
  start_mode(mode?"monitor_8h_repair":"meeting_8h_repair",mode);
  uint32_t cursor=0,index=0,total=8*60*60*50;
  for(unsigned n=0;n<total;n++){
   if(n%180000==999)periodic_damage=true;
   unsigned prev=s_card_session.segment_count;frame(n,(n/(60*60*50))%2==0);
   if(s_card_session.segment_count>prev&&n){sd_audio_segment_t f=part(index++,cursor);cursor+=(f.total_bytes-128)/666;assert(cursor==n);}
  }
  assert(commit_segment_locked(true)==ESP_OK);sd_audio_segment_t f=part(index++,cursor);cursor+=(f.total_bytes-128)/666;
  assert(cursor==total&&index==s_card_session.segment_count&&!s.dropped_frames&&!s.integrity->frozen&&scopes==8&&!aborts);
  printf("PASS enabled actual writer mode=%u eight hours frames=%u files=%u repairs=%u no PCM/sequence/count/CRC/time gap\n",mode,total,index,scopes);
 }
'''
tests=tests.replace('LONG_TESTS',long_tests if long_run else '')
execute('writer',base+tests,units)

# Compile the actual SDK observer and native verifier together. The mock only
# implements SDK/card/RTOS primitives; all repair scope logic is production C.
sdk=ns['fixtures']['sdk'][0]
sdk=sdk[:sdk.index('int main(void)')]
sdk='#define CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE 1\n#define CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE 1\n'+sdk
verify=(root/'main/board/sd_write_verify.c').read_text(encoding='utf-8')
verify=re.sub(r'^#include "(?:esp_heap_caps|esp_log|esp_rom_crc|esp_timer|sdmmc_cmd)\.h"\n','',verify,flags=re.M)
extra=r'''
static bool ready_ok=true,ready_error;
static unsigned status_calls;
static esp_err_t sdmmc_send_cmd_send_status(sdmmc_card_t*c,uint32_t*out){++status_calls;sdmmc_command_t probe={1000};esp_err_t e=c->host.do_transaction(0,&probe);if(e!=ESP_OK)return e;*out=ready_ok?1:0;return ready_error?ESP_FAIL:ESP_OK;}
static bool sdmmc_ready_for_data(uint32_t status){return status==1;}
static void quiet_log(const char*t,const char*f,...){(void)t;(void)f;}
#define ESP_LOGW(...) quiet_log(__VA_ARGS__)
'''
(out/'esp_private').mkdir(exist_ok=True)
(out/'esp_private/sdmmc_common.h').write_text('/* SDK private primitives supplied by fixture. */\n',encoding='utf-8')
sdk=sdk.replace('#include "board/sd_integrity_io.h"',extra+verify+'\n#include "board/sd_integrity_io.h"',1)
sdk+=r'''
static DWORD WINAPI wrong_scope(LPVOID p){(void)p;assert(!nuwax_sd_integrity_io_repair_begin());nuwax_sd_integrity_io_repair_end();return 0;}
int main(void){assert(nuwax_sd_integrity_io_init());uint8_t block[512]={42};
 assert(!nuwax_sd_integrity_io_repair_begin());nuwax_sd_integrity_io_arm();assert(!nuwax_sd_integrity_io_repair_begin());
 assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_OK&&!status_calls);
 assert(nuwax_sd_integrity_io_repair_begin());assert(!nuwax_sd_integrity_io_repair_begin());
 HANDLE h=CreateThread(NULL,0,wrong_scope,NULL,0,NULL);assert(h&&WaitForSingleObject(h,5000)==WAIT_OBJECT_0);CloseHandle(h);
 assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_OK&&status_calls==1);
 unsigned before=calls;ready_ok=false;assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_ERR_INVALID_STATE&&calls==before&&status_calls==2);
 ready_ok=true;assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_ERR_INVALID_STATE&&calls==before&&status_calls==2);
 nuwax_sd_integrity_io_repair_end();assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_OK&&status_calls==2);
 assert(nuwax_sd_integrity_io_repair_begin());sdmmc_card_t other=card;before=calls;
 assert(nuwax_sd_integrity_io_transfer(&other,block,10,1,true)==ESP_ERR_INVALID_STATE&&calls==before);
 nuwax_sd_integrity_io_repair_end();assert(nuwax_sd_integrity_io_repair_begin());ready_error=true;before=calls;
 assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_FAIL&&calls==before);ready_error=false;
 nuwax_sd_integrity_io_repair_end();assert(nuwax_sd_integrity_io_repair_begin());nuwax_sd_integrity_io_repair_abort();before=calls;
 assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_ERR_INVALID_STATE&&calls==before);
 nuwax_sd_integrity_io_repair_end();assert(nuwax_sd_integrity_io_repair_begin());sdk_fail=true;
 assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_FAIL);sdk_fail=false;before=calls;
 assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_FAIL&&calls==before);nuwax_sd_integrity_io_repair_end();
 assert(nuwax_sd_integrity_io_repair_begin());nuwax_sd_integrity_io_budget_end();
 assert(s_io->budget_owner==(uintptr_t)xTaskGetCurrentTaskHandle());
 int64_t end=s_io->deadline;atomic_store(&clock_us,end+1000);before=calls;
 assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,false)==ESP_ERR_TIMEOUT&&calls==before);
 assert(nuwax_sd_integrity_io_transfer(&card,block,10,1,true)==ESP_ERR_TIMEOUT&&calls==before);
 nuwax_sd_integrity_io_repair_end();assert(!s_io->budget_owner&&!s_io->repair_owner);
 puts("PASS repair command deadline: readiness, reads, writes and close share the bounded scope; generic budget end cannot disable it");
 puts("PASS actual SDK observer + verifier: healthy writes unchanged, repair checks native readiness, task/card fences, SDK failures and abort remain sticky through cleanup");return 0;}
'''
execute('sdk',sdk)
print('PASS bounded SD batch repair actual writer and SDK fault checks')

# Exercise production repair/readback functions on the SDK's real FatFs too.
# stdio is adapted to FIL calls here; it is not a claim to emulate Newlib/VFS.
fat_tree=ast.parse((root/'tools/test-sd-write-fatfs.py').read_text(encoding='utf-8'))
fat_ns={'root':root,'re':re}
for node in fat_tree.body:
    if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id in {'verify','code'} for t in node.targets):
        exec(compile(ast.Module(body=[node],type_ignores=[]),'fatfs-fixture','exec'),fat_ns)
fat=fat_ns['code'].split('int main(void)')[0]
fat='#define CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE 1\n#define CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE 1\n#define CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS 1\n#define CONFIG_NUWAX_SD_SESSION_ENABLE 1\n'+fat
fat+=r'''
#include <stdatomic.h>
#include <sys/stat.h>
#include "sd_audio_integrity.h"
#include "board/sd_card_driver.h"
#define HEADER_BYTES 96U
#define FOOTER_BYTES 32U
#define ARCHIVE_STDIO_BUFFER_BYTES 16384U
#define TAG "fat-repair"
#define ESP_LOGE(...) log_ignore(__VA_ARGS__)
'''
fat+=spool[spool.index('typedef struct __attribute__((packed)) {'):spool.index('_Static_assert(sizeof(archive_header_t)')]
fat+=r'''
static struct {
 FIL *file;char *write_buffer;sd_integrity_t *integrity;
 atomic_bool active,degraded,writer_error;uint32_t capture_generation,segment_index;
 uint32_t sample_rate;uint8_t channels;uint16_t frame_duration_ms;
 long last_good_offset,verify_offset;uint32_t verify_bytes,verify_crc32,verify_calls;
 uint64_t verify_us,payload_bytes;uint32_t payload_crc32,frame_count;
 char session_id[48],open_path[128];
}s;
static atomic_bool s_shutdown_requested,s_card_failed,s_media_ready;
static const char *s_writer_fault_stage;
static atomic_uint s_media_generation;
static struct {bool identity_valid;nuwax_sd_card_identity_t identity;}s_card_io_recovery;
static struct {char session_id[48];}s_card_session;
static bool scope_active,scope_aborted;static unsigned scopes;
static bool nuwax_power_ctrl_shutdown_preparing(void){return false;}
bool nuwax_sd_card_is_mounted(void){return true;}
void nuwax_sd_card_mark_io_fault(esp_err_t error){(void)error;}
bool nuwax_sd_card_get_identity(nuwax_sd_card_identity_t*p){memset(p,0,sizeof(*p));return true;}
static bool card_recording_locked(void){return true;}
static bool writer_integrity_live(void){return s.integrity&&s.integrity->active&&!s.integrity->frozen;}
void nuwax_sd_integrity_io_arm(void){}
void nuwax_sd_integrity_io_read_phase(bool read){(void)read;}
bool nuwax_sd_integrity_io_repair_begin(void){assert(!scope_active);scope_active=true;scope_aborted=false;scopes++;return true;}
void nuwax_sd_integrity_io_repair_abort(void){scope_aborted=true;}
void nuwax_sd_integrity_io_repair_end(void){scope_active=false;}
bool nuwax_sd_integrity_io_budget_begin(uint32_t ms){return ms>0&&!scope_active;}
void nuwax_sd_integrity_io_budget_end(void){}
static void writer_note_fault(const char*p,int e){(void)p;(void)e;}
static void writer_integrity_fault(uint32_t stage,uint32_t expected,uint32_t observed){sd_integrity_freeze(s.integrity,stage,0,expected,observed);}
static uint32_t crc32_update(uint32_t c,const void*p,size_t n){return esp_rom_crc32_le(c,p,n);}
static void vTaskDelay(unsigned n){clock_us+=n*1000;}
static FIL *fat_fopen(const char*path,const char*mode){(void)mode;FIL*f=calloc(1,sizeof(*f));assert(f);if(f_open(f,path,FA_READ)!=FR_OK){free(f);return NULL;}return f;}
static int fat_fclose(FIL*f){FRESULT e=f_close(f);free(f);return e==FR_OK?0:-1;}
static int fat_fseek(FIL*f,long pos,int mode){assert(mode==SEEK_SET&&pos>=0);return f_lseek(f,pos)==FR_OK?0:-1;}
static long fat_ftell(FIL*f){return (long)f_tell(f);}
static size_t fat_fread(void*p,size_t size,size_t n,FIL*f){UINT got=0;return f_read(f,p,size*n,&got)==FR_OK?got/size:0;}
static size_t fat_fwrite(const void*p,size_t size,size_t n,FIL*f){UINT got=0;if(scope_active&&scope_aborted)return 0;return f_write(f,p,size*n,&got)==FR_OK?got/size:0;}
static int fat_setvbuf(FIL*f,char*b,int m,size_t n){(void)f;(void)b;(void)m;(void)n;return 0;}
static int fat_ferror(FIL*f){return f_error(f);}
static int fat_fstat(FIL*f,struct stat*st){memset(st,0,sizeof(*st));st->st_size=f_size(f);return 0;}
static bool sync_archive_file(FIL*f,const char*phase){(void)phase;return f_sync(f)==FR_OK;}
'''+validate
repair=(root/'main/sd_audio_batch_repair.inc').read_text(encoding='utf-8')
writer=spool[spool.index('static esp_err_t writer_verify_records_locked(void)'):spool.index('\nstatic esp_err_t commit_segment_locked(')]
fat='#include "sd_audio_read_recovery_host.h"\n'+fat
translation=repair+'\n'+writer
translation=translation.replace('fstat(fileno(file), &st)','fat_fstat(file, &st)')
for a,b in [('FILE *','FIL *'),('fopen(', 'fat_fopen('),('fclose(', 'fat_fclose('),('fseek(', 'fat_fseek('),
            ('ftell(', 'fat_ftell('),('fread(', 'fat_fread('),('fwrite(', 'fat_fwrite('),('ferror(', 'fat_ferror('),('setvbuf(', 'fat_setvbuf(')]:
    translation=translation.replace(a,b)
fat+=translation+r'''
static char cache[2*ARCHIVE_STDIO_BUFFER_BYTES];
static void init_file(const char *path){
 sd_integrity_destroy(s.integrity);memset(&s,0,sizeof(s));s.integrity=sd_integrity_create(crc32_update);assert(s.integrity);
 s.file=calloc(1,sizeof(FIL));assert(s.file&&f_open(s.file,path,FA_CREATE_NEW|FA_READ|FA_WRITE)==FR_OK);
 s.active=true;s_media_ready=true;s.capture_generation=1;s.segment_index=35;s.sample_rate=16000;s.channels=1;s.frame_duration_ms=20;s.write_buffer=cache;
 strcpy(s.session_id,"meeting_fat_repair");strcpy(s_card_session.session_id,s.session_id);strcpy(s.open_path,path);s_card_io_recovery.identity_valid=true;
 archive_header_t h={0};memcpy(h.magic,"NWXOFF1",7);h.version=1;h.header_bytes=96;h.channels=1;h.codec=NUWAX_AUDIO_CODEC_PCM16;
 h.sample_rate=16000;h.frame_duration_ms=20;h.reserved0=2;h.segment_index=0;strcpy(h.session_id,s.session_id);
 assert(sd_integrity_begin(s.integrity,s.session_id,path,1,s.segment_index,&h,sizeof(h)));
 assert(fat_fwrite(&h,1,sizeof(h),s.file)==sizeof(h)&&sync_archive_file(s.file,"header"));
 s.last_good_offset=96;s.verify_offset=0;s.verify_bytes=96;s.verify_crc32=crc32_update(0,&h,96);
}
static void frame(unsigned seq){uint8_t na[662];int16_t pcm[320];for(unsigned i=0;i<320;i++)pcm[i]=(int16_t)(seq*3+i);size_t size=0;
 assert(nuwax_audio_frame_build_pcm16(na,sizeof(na),pcm,320,seq,10000+20*seq,0,16000,1,20,&size)==ESP_OK);
 uint32_t len=(uint32_t)size;sd_integrity_record(s.integrity,len,na,crc32_update(0,na,len),true,crc32_update(0,na,len));
 assert(fat_fwrite(&len,1,4,s.file)==4&&fat_fwrite(na,1,len,s.file)==len);
 s.last_good_offset=fat_ftell(s.file);s.verify_crc32=crc32_update(s.verify_crc32,&len,4);s.verify_crc32=crc32_update(s.verify_crc32,na,len);s.verify_bytes+=4+len;
 s.payload_crc32=crc32_update(s.payload_crc32,na,len);s.payload_bytes+=len;s.frame_count++;
 if(seq==0){assert(sync_archive_file(s.file,"first")&&writer_verify_records_locked()==ESP_OK);}
}
static void disturb(FATFS*fs,long offset){FIL*probe=fat_fopen(s.open_path,"rb");assert(probe&&fat_fseek(probe,offset+1,SEEK_SET)==0);
 LBA_t sector=fs->database+(LBA_t)(probe->clust-2)*fs->csize+(offset/512)%fs->csize;
 assert(fat_fclose(probe)==0);image[(size_t)sector*512+(offset%512)]^=0x40;
}
int main(void){image=calloc(SECTORS,512);assert(image);FATFS fs;uint8_t work[4096];MKFS_PARM opts={.fmt=FM_FAT32|FM_SFD,.n_fat=2,.align=1,.au_size=512};
 assert(f_mkfs("0:",&opts,work,sizeof(work))==FR_OK&&f_mount(&fs,"0:",1)==FR_OK);guarded=true;
 init_file("0:/repair.part");for(unsigned n=0;n<100;n++)frame(n);assert(sync_archive_file(s.file,"periodic"));
 disturb(&fs,65536);assert(writer_verify_records_locked()==ESP_OK&&scopes==1&&!s.integrity->frozen&&s.frame_count==100&&s.last_good_offset==66696);
 assert(writer_repair_verify_prefix(&(writer_repair_guard_t){1,0,clock_us,s.file},false)==ESP_OK);
 assert(fat_fclose(s.file)==0);s.file=NULL;
 puts("PASS real SDK FatFs + actual batch repair/readback + native verifier: later 65536-byte sector disturbance repaired, fresh full prefix and PCM CRC match");
 init_file("0:/older.part");for(unsigned n=0;n<100;n++)frame(n);assert(sync_archive_file(s.file,"periodic")&&writer_verify_records_locked()==ESP_OK);
 for(unsigned n=100;n<200;n++)frame(n);assert(sync_archive_file(s.file,"periodic"));disturb(&fs,108032);disturb(&fs,4096);
 assert(writer_verify_records_locked()==ESP_ERR_INVALID_CRC&&!s.file&&s.integrity->frozen&&scopes==2);
 puts("PASS real SDK FatFs: repairing current batch cannot hide damage in a previous batch; original file retained");
 sd_integrity_destroy(s.integrity);f_mount(NULL,"0:",0);free(image);return 0;}
'''
fat_dir=root/'build-host-checks/sd-catalog-fatfs-dynamic'
flags+=['-I'+str(fat_dir),'-Wno-implicit-fallthrough']
execute('fatfs',fat,[fat_dir/'ff.c',fat_dir/'ffunicode.c',fat_dir/'ffsystem.c',root/'main/audio_frame.c',root/'main/sd_audio_integrity.c'])
print('PASS real SDK FatFs delayed-corruption repair checks')
