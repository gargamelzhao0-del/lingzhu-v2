"""Execute actual FIFO catalog, archive bytes, ACKs and end gates off-device."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
for source in ['sd_monitor_fifo_host_check.c','sd_monitor_fifo_replay_host_check.c','sd_monitor_fifo_io_host_check.c']:
 with tempfile.TemporaryDirectory(prefix='nuwax-monitor-fifo-') as d:
    exe=Path(d)/'fifo.exe'
    subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
        '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
        '-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),
        '-I'+str(root/'managed_components/espressif__cjson/cJSON'),
        str(root/'main'/source),str(root/'managed_components/espressif__cjson/cJSON/cJSON.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=d,check=True,timeout=90)
