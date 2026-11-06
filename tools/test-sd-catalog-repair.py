"""Exercise real catalog recovery against poisoned manifests and intact audio."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-catalog-repair'
out.mkdir(parents=True, exist_ok=True)
exe = out / 'repair.exe'
subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
    '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(root / 'main'),
    str(root / 'main/sd_audio_catalog_repair_host_check.c'), '-o', str(exe)], check=True)
with tempfile.TemporaryDirectory(prefix='sd-catalog-repair-') as directory:
    subprocess.run([str(exe)], cwd=directory, check=True, timeout=60)
