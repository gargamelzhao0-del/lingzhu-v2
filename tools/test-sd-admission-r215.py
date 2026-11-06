"""Offline SD admission and command-evidence regressions. Never opens a physical card."""
from pathlib import Path
import ast, ctypes, hashlib, importlib.util, json, os, re, subprocess
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-admission-r215';out.mkdir(parents=True,exist_ok=True)
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
strip=lambda text:re.sub(r'(?m)^#include[^\r\n]*','',text)
flags=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
    '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x10b',
    '-I'+str(out),'-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs')]
results=[]
def run(name,source,extra=(),args=()):
    exe=out/(name+'.exe')
    subprocess.run(flags+list(extra)+[str(source),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)]+list(map(str,args)),capture_output=True,text=True,timeout=90)
    (out/(name+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8')
    result.check_returncode();print(result.stdout.strip(),flush=True);results.append(name)
for module in ['sd_directory_guard','sd_command_history']:
    source=root/'main/board'/(module+'.c')
    (out/(module+'_actual.inc')).write_text(strip(source.read_text()),encoding='utf-8')
    run(module,root/'main'/(module+'_host_check.c'))

# Execute the pinned SDK's actual command/read/write/wait bodies with both
# observers enabled. Verify native evidence captures CMD13 failure, while the
# original command order, return codes and accepted R1 policy stay identical.
sdk=Path('C:/esp/v6.0/esp-idf')
def function(text,name):
    match=re.search(r'^(?:(?:static|inline)\s+)*(?:esp_err_t|bool)\s+'+name+r'\([^;{}]*\)\s*\{',text,re.M)
    assert match,name
    end=text.index('{',match.start())+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[match.start():end]
spec=importlib.util.spec_from_file_location('sd_status_fix',root/'tools/prepare-sd-status-fix.py')
fix=importlib.util.module_from_spec(spec);spec.loader.exec_module(fix)
patched=fix.patch((sdk/'components/sdmmc/sdmmc_cmd.c').read_bytes()).decode()
common=(sdk/'components/sdmmc/sdmmc_common.c').read_text()
header=(sdk/'components/sdmmc/include/esp_private/sdmmc_common.h').read_text()
pieces=[function(header,'host_is_spi'),function(header,'sdmmc_ready_for_data'),
    function(patched,'sdmmc_send_cmd'),function(patched,'sdmmc_send_cmd_send_status'),
    function(common,'sdmmc_wait_for_idle'),function(patched,'sdmmc_write_sectors_dma'),function(patched,'sdmmc_read_sectors_dma')]
(out/'sd_status_sdk_host.inc').write_text('\n\n'.join(pieces))
fixture=(root/'main/sd_status_sdk_host_check.c').read_text()
stubs=r'''
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) assert(!(*(p))++)
#define portEXIT_CRITICAL(p) assert((*(p))--==1)
static uintptr_t xTaskGetCurrentTaskHandle(void){return 1;}
static void*heap_caps_calloc(size_t n,size_t b,unsigned flags){(void)flags;return calloc(n,b);}
static void history_log(const char*tag,const char*fmt,...){(void)tag;(void)fmt;}
#define ESP_LOGW(...) history_log(__VA_ARGS__)
#undef ESP_LOGE
#define ESP_LOGE(...) ((void)++error_logs,history_log(__VA_ARGS__))
static const char*esp_err_to_name(int e){return e?"ERR":"ESP_OK";}
static uint32_t esp_rom_crc32_le(uint32_t c,const void*v,size_t n){const uint8_t*p=v;c=~c;while(n--){c^=*p++;for(unsigned b=0;b<8;b++)c=c>>1^((c&1)?0xedb88320U:0);}return ~c;}
'''+strip((root/'main/board/sd_command_history.c').read_text())
fixture=fixture.replace('#include "sd_status_sdk_host.inc"',stubs+'\n#include "sd_status_sdk_host.inc"')
fixture=fixture.replace('for(unsigned scope=0;', 'assert(nuwax_sd_command_history_init());uint32_t generation=0;\n    for(unsigned scope=0;')
fixture=fixture.replace('int err=write?', '''nuwax_sd_command_history_arm(++generation);
        assert(nuwax_sd_command_history_begin(&card,NULL,buffer,1492384,1,write,!write,generation,1,true));
        int err=write?''')
fixture=fixture.replace('int expected=scenario', '''nuwax_sd_command_history_end();assert(s_history->count==1);
        command_event_t*event=&s_history->events[0];assert(event->range_valid&&event->lba==1492384&&event->source_valid==(write!=0));
        assert(event->polls==status_calls);if(scenario==1)assert(event->last_error==ESP_ERR_INVALID_CRC);
        if(scenario==4)assert(event->last_status&(1U<<26));
        int expected=scenario''')
fixture=fixture.replace('puts("PASS actual SDK:', 'free(s_history);s_history=NULL;\n    puts("PASS enabled native history + actual SDK:')
source=out/'sdk-history.c';source.write_text(fixture)
run('sdk-history',source,['-DEXPECT_FIXED=1','-Wno-unused-variable','-I'+str(sdk/'components/sdmmc/include')])

# Run the existing real IO observer fault scenarios with the new history ON,
# including cross-task writes and raw-comparison descriptor copies. Reuse the
# fixture expression only, not the test runner or any generated stale bodies.
tree=ast.parse((root/'tools/test-sd-integrity-diagnostics.py').read_text())
assignments={n.targets[0].id:n.value for n in tree.body if isinstance(n,ast.Assign) and len(n.targets)==1 and isinstance(n.targets[0],ast.Name)}
crc=ast.literal_eval(assignments['crc'])
io_source=(root/'main/board/sd_integrity_io.c').read_text()
io_fixture=eval(compile(ast.Expression(assignments['io_fixture']),'fixture','eval'),
    {'crc':crc,'io':'#include "board/sd_integrity_io.h"\n'+strip(io_source)})
io_fixture='#define CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE 1\n#define SD_OCR_SDHC_CAP (1U<<30)\n'+io_fixture
io_fixture=io_fixture.replace('} csd;\n struct {', '} csd;uint32_t ocr;\n struct {')
history_stubs='''
static void history_log(const char*tag,const char*fmt,...){(void)tag;(void)fmt;}
#define ESP_LOGW(...) history_log(__VA_ARGS__)
#define ESP_LOGE(...) history_log(__VA_ARGS__)
'''+strip((root/'main/board/sd_command_history.c').read_text())
io_fixture=io_fixture.replace('static uint8_t disk[2048][512];',history_stubs+'\nstatic uint8_t disk[2048][512];')
io_fixture=io_fixture.replace('errno=77;return ESP_OK;', '''nuwax_sd_command_history_command(card,write?24:17,(uint32_t)(sec*512),0x900,ESP_OK,p,n*512);
 nuwax_sd_command_history_command(card,13,1,0x900,ESP_OK,NULL,0);
 errno=77;return ESP_OK;''')
io_fixture=io_fixture.replace('assert(report.mismatched_sectors==2&&report.raw_attempts==1',
    'assert(s_history->frozen&&s_history->count==2&&s_history->events[0].lba==11&&s_history->events[1].lba==11);\n assert(report.mismatched_sectors==2&&report.raw_attempts==1')
io_fixture=io_fixture.replace('sizeof(io_state_t));return 0;', 'sizeof(io_state_t));free(s_history);s_history=NULL;return 0;')
source=out/'io-history.c';source.write_text(io_fixture)
run('io-history',source,['-Wno-unused-function'])

# Actual driver admission/health transitions with the directory API enabled.
# A directory failure must be rejected BEFORE the scratch qualification writes,
# remain present-but-unhealthy, and recover only through the ordered path.
(out/'sd_speed_driver_host.inc').write_text(strip((root/'main/board/sd_card_driver.c').read_text()))
fixture=(root/'main/sd_speed_host_check.c').read_text()
driver_stubs=r'''
#define CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE 1
#define ESP_ERR_INVALID_ARG 7
#define ESP_ERR_NOT_SUPPORTED 8
#ifndef FF_MULTI_PARTITION
#define FF_MULTI_PARTITION 0
#endif
#define FF_VOLUMES 2
typedef unsigned char BYTE;
#if FF_MULTI_PARTITION
static struct {BYTE pd,pt;} VolToPart[FF_VOLUMES]={{0,0},{1,0}};
#endif
static unsigned directory_checks;static int directory_error;static bool core_directory_bad;
static BYTE ff_diskio_get_pdrv_card(const sdmmc_card_t*c){assert(c&&lock_depth==1);return 0;}
static esp_err_t nuwax_sd_directory_validate(void*c,const char*p,bool missing){
 assert(c&&lock_depth==1&&!strncmp(p,"0:/",3));(void)missing;++directory_checks;
 return strstr(p,"sd_integrity")||strstr(p,"00000001")||
   (core_directory_bad&&strstr(p,"audio_sessions"))?directory_error:ESP_OK;}
'''
fixture=fixture.replace('#include "sd_speed_driver_host.inc"',driver_stubs+'\n#include "sd_speed_driver_host.inc"')
assert driver_stubs in fixture
at=fixture.rindex('    return 0;')
extra=r'''
    reset();file_size=0;directory_error=ESP_ERR_INVALID_CRC;directory_checks=0;core_directory_bad=true;
    assert(nuwax_sd_card_mount(&info)==ESP_ERR_INVALID_CRC);
    assert(directory_checks==6&&mounts==2&&unmounts==2&&file_size==0&&!allocations&&!open_fd);
    assert(nuwax_sd_card_media_detected()&&!nuwax_sd_card_is_mounted()&&nuwax_sd_card_media_error()==ESP_ERR_INVALID_CRC);
    core_directory_bad=false;directory_error=ESP_OK;assert(nuwax_sd_card_recover(100)==ESP_OK&&nuwax_sd_card_is_mounted());
    directory_error=ESP_ERR_INVALID_CRC;
    assert(nuwax_sd_card_validate_evidence_directory("/sdcard/sd_integrity",true)==ESP_ERR_INVALID_CRC);
    assert(nuwax_sd_card_is_mounted()&&nuwax_sd_card_media_error()==ESP_OK);
    directory_error=ESP_ERR_INVALID_CRC;unsigned old_unmounts=unmounts;
    assert(nuwax_sd_card_validate_directory("/sdcard/offline2/00000001",false)==ESP_ERR_INVALID_CRC);
    assert(!nuwax_sd_card_is_mounted()&&nuwax_sd_card_media_detected()&&unmounts==old_unmounts);
    directory_error=ESP_OK;assert(nuwax_sd_card_recover(100)==ESP_OK);
    directory_error=ESP_ERR_NO_MEM;assert(nuwax_sd_card_validate_directory("/sdcard/sd_integrity",true)==ESP_ERR_NO_MEM);
    assert(nuwax_sd_card_is_mounted()&&nuwax_sd_card_media_error()==ESP_OK);
    assert(nuwax_sd_card_validate_directory("/sdcard2/offline2",true)==ESP_ERR_INVALID_ARG&&nuwax_sd_card_is_mounted());
    directory_error=ESP_OK;assert(nuwax_sd_card_unmount()==ESP_OK);
#if FF_MULTI_PARTITION
    reset();VolToPart[0].pd=1;assert(nuwax_sd_card_mount(&info)==ESP_ERR_INVALID_STATE&&!nuwax_sd_card_is_mounted());
    VolToPart[0].pd=0;assert(nuwax_sd_card_recover(100)==ESP_OK);
    assert(nuwax_sd_card_unmount()==ESP_OK);
#endif
    reset();directory_checks=0;fail_high_mount=fail_low_mount=true;
    assert(nuwax_sd_card_mount(&info)!=ESP_OK&&!directory_checks&&!nuwax_sd_card_media_detected());
    puts("PASS actual directory-enabled driver: corrupt media rejects before scratch writes, present/absent distinction, no inline unmount, ordered recovery, transient ENOMEM, correct mount mapping");
'''
fixture=fixture[:at]+extra+fixture[at:]
source=out/'driver-admission.c';source.write_text(fixture)
# This long-standing driver fixture has its own compact ESP error definitions.
driver_flags=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
    '-DCONFIG_NUWAX_SD_CARD_HIGHSPEED_ENABLE=1','-I'+str(out),'-I'+str(root/'main')]
for multi in [0,1]:
    name='driver-admission-multi'+str(multi);exe=out/(name+'.exe')
    subprocess.run(driver_flags+['-DFF_MULTI_PARTITION='+str(multi),str(source),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
    (out/(name+'.log')).write_text(result.stdout+result.stderr);result.check_returncode()
    print('Variant multi-partition='+str(multi)+': '+result.stdout.strip(),flush=True);results.append(name)

# Actual SDK FatFs on the preserved FAT32 backup, with writes confined to an
# in-memory sector overlay. An absent local backup skips only this optional
# reproduction, not the portable guard/history checks above.
evidence=root/'runtime-logs/analysis-sd-card-20261102-2024'
saved=root/'runtime-logs/analysis-sd-code-20261102/saved-card-fatfs.c'
if saved.exists() and (evidence/'allocated-clusters.bin').exists():
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    assert sha(evidence/'allocated-clusters.bin')=='209cff55f3232d06806f11bed08a7250d7d1119b670c09faf13dce9044d47545'
    code=saved.read_text();code=code[:code.index('int main(int argc,char**argv)')]
    # This historical image fixture predates R218's two diagnostic counters.
    # Preserve its original verifier behavior; enabled R218 logic is exercised
    # by test-sd-io-r218 and the real-FatFs write regressions.
    old_stats='atomic_load(&s_memory_denied), atomic_load(&s_max_us)};'
    assert code.count(old_stats)==1
    code=code.replace(old_stats,'atomic_load(&s_memory_denied), atomic_load(&s_max_us), 0U, 0U};')
    # This fixture's existing sdmmc type/backend and real FatFs are reused;
    # the guard body is the current production source, not the old snapshot.
    code+='\n#define CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE 1\n'
    code+='static esp_err_t sdmmc_read_sectors(sdmmc_card_t*c,void*p,size_t lba,size_t n){return __real_sdmmc_read_sectors(c,p,lba,n);}\n'
    code+=strip((root/'main/board/sd_directory_guard.c').read_text())+r'''
int main(int argc,char**argv){assert(argc==2);const char*dir=argv[1];char path[512];snprintf(path,sizeof(path),"%s/allocated-clusters.bin",dir);original=fopen(path,"rb");assert(original);
 fat0=malloc(7449*512);fat1=malloc(7449*512);overlay=calloc(32768,sizeof(*overlay));assert(fat0&&fat1&&overlay);
 load(dir,"volume-boot-sector.bin",boot,512);load(dir,"fat-copy-0.bin",fat0,7449*512);load(dir,"fat-copy-1.bin",fat1,7449*512);
 FATFS fs;assert(f_mount(&fs,"0:",1)==FR_OK&&fs.csize==64);unsigned writes_before=sector_writes;
 assert(nuwax_sd_directory_validate(&card,"0:/sd_integrity",true)==ESP_ERR_INVALID_CRC);
 assert(nuwax_sd_directory_validate(&card,"0:/offline2/00000001",true)==ESP_ERR_INVALID_CRC);
 assert(nuwax_sd_directory_validate(&card,"0:/offline_audio",false)==ESP_OK);
 assert(nuwax_sd_directory_validate(&card,"0:/offline2",false)==ESP_OK);
 assert(nuwax_sd_directory_validate(&card,"0:/audio_sessions",false)==ESP_OK);
 assert(nuwax_sd_directory_validate(&card,"0:/not_created_yet",true)==ESP_OK);
 assert(nuwax_sd_directory_validate(&card,"0:/not_created_yet",false)==ESP_ERR_NOT_FOUND);
 assert(sector_writes==writes_before);
 puts("PASS real SDK FatFs + preserved card image: both damaged directories rejected, three intact directories accepted, missing paths distinguished, zero SD image writes");
 assert(f_mkdir("0:/guard_normal")==FR_OK);
 assert(nuwax_sd_directory_validate(&card,"0:/guard_normal",false)==ESP_OK);
 assert(f_mkdir("0:/guard_normal/nested")==FR_OK);
 assert(nuwax_sd_directory_validate(&card,"0:/guard_normal/nested",false)==ESP_OK);
 f_mount(NULL,"0:",0);fclose(original);free(fat0);free(fat1);free(overlay);
 puts("PASS real SDK FatFs: newly created root/nested directories accepted; all writes confined to disposable memory overlay");return 0;}
'''
    source=out/'saved-image.c';source.write_text(code,encoding='utf-8')
    ff=root/'build-host-checks/sd-catalog-fatfs-dynamic'
    assert sha(ff/'ff.c')==sha(Path('C:/esp/v6.0/esp-idf/components/fatfs/src/ff.c'))
    run('saved-image',source,['-Wno-unused-function','-Wno-implicit-fallthrough','-I'+str(ff),
        str(ff/'ff.c'),str(ff/'ffunicode.c'),str(ff/'ffsystem.c'),
        str(root/'main/audio_frame.c'),str(root/'main/sd_audio_integrity.c')],[evidence])
    assert sha(evidence/'allocated-clusters.bin')=='209cff55f3232d06806f11bed08a7250d7d1119b670c09faf13dce9044d47545'
else:print('Saved image reproduction unavailable; portable guard/history checks passed',flush=True)

guard=(root/'main/board/sd_directory_guard.c').read_text()
assert not re.search(r'\b(?:f_write|f_mkdir|f_unlink|sdmmc_write_sectors|sdmmc_erase_sectors)\s*\(',guard)
history=(root/'main/board/sd_command_history.c').read_text()
assert not re.search(r'\b(?:sdmmc_read_sectors|sdmmc_write_sectors|do_transaction)\s*\(',history)
io=(root/'main/board/sd_integrity_io.c').read_text()
assert io.index('nuwax_sd_command_history_freeze();',io.index('if (raw) {'))<io.index('raw_comparison(card, sector + n, generation);')
(out/'results.json').write_text(json.dumps(dict(passed=results,physicalCardOpened=False,physicalCardModified=False),indent=2)+'\n')
print('PASS R215 SD admission/evidence: actual enabled production bodies, original card untouched',flush=True)
