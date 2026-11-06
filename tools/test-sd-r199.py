"""Actual metadata qualification and failure-only diagnostics, with target log branches."""
from pathlib import Path
import os,ctypes,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
start=spool.index('esp_err_t nuwax_offline_audio_init(void)')
end=spool.index('static uint32_t recovery_remaining_ms(',start)
initial=spool[start:end]
assert initial.index('sd_audio_catalog_prepare_remount()') < initial.index('nuwax_sd_card_mount(&info)')
assert initial.index('nuwax_sd_card_mark_io_fault(media_result)') < initial.index('(void)nuwax_sd_card_unmount()')
out=root/'build-host-checks/sd-r199';out.mkdir(parents=True,exist_ok=True)
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
(out/'esp_heap_caps.h').write_text('''#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_malloc(size_t n,unsigned caps){(void)caps;return malloc(n);}
''',encoding='utf-8')
(out/'esp_log.h').write_text('''#pragma once
#define ESP_LOGE(tag,...) ((void)(tag),catalog_host_log(__VA_ARGS__))
#define ESP_LOGW(tag,...) ESP_LOGE(tag,__VA_ARGS__)
#define ESP_LOGI(tag,...) ESP_LOGE(tag,__VA_ARGS__)
#define ESP_LOGD(tag,...) ESP_LOGE(tag,__VA_ARGS__)
''',encoding='utf-8')
flags=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
 '-DESP_PLATFORM=1','-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
 '-DSD_AUDIO_CATALOG_ROOT="catalog"','-I'+str(out),
 '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main')]
exe=out/'metadata.exe'
subprocess.run(flags+[str(root/'main/sd_audio_catalog_io_host_check.c'),str(root/'main/audio_frame.c'),'-o',str(exe)],check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-r199-') as cwd:
 subprocess.run([str(exe)],cwd=cwd,check=True,timeout=40)
# The same executable must catch an initialization path skipping the probe.
source=(root/'main/sd_audio_catalog.c').read_text(encoding='utf-8')
needle='return metadata_io_qualified ? ESP_OK : sd_audio_catalog_qualify_storage();'
assert source.count(needle)==1
(out/'sd_audio_catalog.c').write_text(source.replace(needle,'return ESP_OK;'),encoding='utf-8')
# Quoted include in the harness normally resolves next to its source file.
# Place a copy beside the mutant to select that one without editing production.
(out/'negative.c').write_text((root/'main/sd_audio_catalog_io_host_check.c').read_text(encoding='utf-8'),encoding='utf-8')
negative=out/'negative.exe'
subprocess.run(flags+[str(out/'negative.c'),str(root/'main/audio_frame.c'),'-o',str(negative)],check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-r199-negative-') as cwd:
 result=subprocess.run([str(negative)],cwd=cwd,capture_output=True,timeout=15)
 assert result.returncode!=0,'Skipping metadata qualification must fail the actual initialization test'
print('PASS negative: skipping initialization metadata qualification is rejected')
