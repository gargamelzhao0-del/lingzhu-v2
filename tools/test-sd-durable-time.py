from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/sd_audio_session_storage.inc').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='nuwax-sd-durable-time-') as directory:
    work = Path(directory)
    helpers = source[source.index('static uint64_t card_online_since_ms'):source.index('static bool card_owner')]
    network = source[source.index('void nuwax_sd_session_network'):source.index('esp_err_t nuwax_sd_session_check_new_identity')]
    (work / 'sd_network_host.inc').write_text(helpers + network, encoding='utf-8')
    for name in ('durability', 'time'):
        exe = work / (name + '.exe')
        command = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                   '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
                   '-I' + str(work), '-I' + str(root / 'main'),
                   '-I' + str(root / 'main/audio_modes/meeting/host_stubs'),
                   str(root / f'main/sd_audio_{name}_host_check.c')]
        if name == 'time':
            command.append(str(root / 'main/sd_audio_catalog.c'))
        subprocess.run(command + ['-o', str(exe)], check=True)
        subprocess.run([str(exe)], cwd=work, check=True)
