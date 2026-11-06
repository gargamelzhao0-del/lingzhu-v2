"""Production catalog completed-scene cache: performance and safe fallbacks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nuwax-completed-cache-') as directory:
    work = Path(directory)
    exe = work / 'check.exe'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
        '-I' + str(root / 'main'), '-I' + str(root / 'main/audio_modes/meeting/host_stubs'),
        str(root / 'main/sd_audio_completed_cache_host_check.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=work, check=True)
