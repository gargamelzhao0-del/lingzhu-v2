"""Real catalog/NA files; deterministic allocation, directory and slow-I/O faults."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-reconcile-lifecycle';out.mkdir(parents=True,exist_ok=True)
exe=out/'reconcile.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
    '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
    '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),
    str(root/'main/sd_audio_reconcile_host_check.c'),str(root/'main/audio_frame.c'),'-o',str(exe)],check=True)
for case in ('guard','head','cursor','bounded','fast','cancel','io','nomem','expired','file_nomem','file_busy','duplicate','modes','absence','unknown','upper_name','partial_name',
             'discard_meeting','discard_monitor','discard_live_meeting','discard_live_monitor','discard_guards','discard_tail','discard_interrupted','discard_ack_backup','discard_reference'):
    with tempfile.TemporaryDirectory(prefix='sd-reconcile-') as directory:
        result=subprocess.run([str(exe),case],cwd=directory,text=True,capture_output=True,timeout=120)
    (out/(case+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8')
    print(result.stdout+result.stderr,flush=True);result.check_returncode()

# Force only the production repair include through its ESP_PLATFORM branch.
# The surrounding catalog remains real host stdio; SDK scopes/allocator are
# explicit doubles. The native transaction deadline itself has its own test.
catalog=(root/'main/sd_audio_catalog.c').read_text(encoding='utf-8-sig')
archives=(root/'main/sd_audio_catalog_archives.inc').read_text(encoding='utf-8-sig')
native=r'''
static bool native_scope_busy;
static int64_t native_scope_until;
static unsigned native_scope_begin,native_scope_end;
static int64_t esp_timer_get_time(void){return virtual_us;}
static esp_err_t nuwax_sd_card_io_deadline_begin(int64_t until){
    if(native_scope_busy)return ESP_ERR_NOT_FINISHED;
    assert(!native_scope_until&&until>virtual_us);native_scope_until=until;++native_scope_begin;return ESP_OK;
}
static void nuwax_sd_card_io_deadline_end(void){assert(native_scope_until);native_scope_until=0;++native_scope_end;}
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps){assert(caps==3U);return checked_calloc(n,size);}
#define ESP_PLATFORM 1
#include "sd_audio_catalog_reconcile.inc"
#undef ESP_PLATFORM
'''
archives=archives.replace('#include "sd_audio_catalog_reconcile.inc"',native)
catalog=catalog.replace('#include "sd_audio_catalog_archives.inc"',archives)
generated=out/'native-catalog.inc';generated.write_text(catalog,encoding='utf-8')
native_exe=out/'native-reconcile.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation',
    '-DCATALOG_RECONCILE_NATIVE_TEST=1','-DCATALOG_RECONCILE_SOURCE="'+generated.as_posix()+'"',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
    '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
    '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),
    str(root/'main/sd_audio_reconcile_host_check.c'),str(root/'main/audio_frame.c'),'-o',str(native_exe)],check=True)
with tempfile.TemporaryDirectory(prefix='sd-native-reconcile-') as directory:
    subprocess.run([str(native_exe),'native'],cwd=directory,check=True,timeout=30)

# Compile the actual optional-cleanup ESP branch. The native SD transaction
# timeout itself is covered by sd_card_deadline_host_check; this checks that
# cleanup cannot steal an existing scope or leak its own on an early return.
from c_source_extract import function
cleanup=function((root/'main/sd_audio_catalog_discard.inc').read_text(), 'catalog_discard_unlink')
cleanup_src=out/'cleanup-native.c'
cleanup_src.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define ESP_PLATFORM 1
typedef int esp_err_t;
#define ESP_OK 0
typedef int sd_audio_session_t;
typedef int sd_audio_segment_t;
static bool held,busy;
static unsigned work,ended;
static int64_t esp_timer_get_time(void){return 123456LL;}
static esp_err_t nuwax_sd_card_io_deadline_begin(int64_t until){assert(until==373456LL);if(busy)return 1;assert(!held);held=true;return 0;}
static void nuwax_sd_card_io_deadline_end(void){assert(held);held=false;++ended;}
static void catalog_discard_unlink_impl(const sd_audio_session_t *s,const sd_audio_segment_t *p){(void)s;(void)p;assert(held);++work;return;}
''' + cleanup + r'''
int main(void){busy=true;held=true;catalog_discard_unlink(NULL,NULL);assert(!work&&!ended&&held);
busy=false;held=false;catalog_discard_unlink(NULL,NULL);assert(work==1U&&ended==1U&&!held);
puts("PASS actual ESP cleanup wrapper: 250ms native deadline, busy owner not stolen, every acquired scope released despite early cleanup return");return 0;}
''',encoding='utf-8')
cleanup_exe=out/'cleanup-native.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(cleanup_src),'-o',str(cleanup_exe)],check=True)
subprocess.run([str(cleanup_exe)],check=True,timeout=10)
