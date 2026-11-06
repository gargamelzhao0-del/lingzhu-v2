from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/sd_audio_session_storage.inc').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='nuwax-session-time-') as directory:
    work = Path(directory)
    helpers = source[source.index('static uint64_t s_card_stopped_ms'):source.index('static void card_save_time_locked')]
    (work / 'sd_stop_time_host.inc').write_text(helpers, encoding='utf-8')
    exe = work / 'time.exe'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
        '-I' + str(work), '-I' + str(root / 'main'),
        '-I' + str(root / 'main/audio_modes/meeting/host_stubs'),
        '-I' + str(root / 'managed_components/espressif__cjson/cJSON'),
        str(root / 'main/sd_audio_session_time_host_check.c'), str(root / 'main/cloud_time.c'),
        str(root / 'managed_components/espressif__cjson/cJSON/cJSON.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=work, check=True)
