"""Run the production ACK parser, including the new typed window negotiation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/offline_audio_spool.c').read_text(encoding='utf-8')
start = source.index('bool nuwax_offline_audio_handle_protocol_text(')
body = source[start:]
assert body.rstrip().endswith('}')
with tempfile.TemporaryDirectory(prefix='nuwax-sd-ack-') as directory:
    work = Path(directory)
    (work / 'ack.inc').write_text(body, encoding='utf-8')
    exe = work / 'ack.exe'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(root / 'main'), '-I' + str(work),
        '-I' + str(root / 'managed_components/espressif__cjson/cJSON'),
        str(root / 'main/sd_audio_ack_host_check.c'),
        str(root / 'managed_components/espressif__cjson/cJSON/cJSON.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-DCONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE=1',
        '-I' + str(root / 'main'), '-I' + str(work), '-I' + str(root / 'managed_components/espressif__cjson/cJSON'),
        str(root / 'main/sd_audio_ack_host_check.c'), str(root / 'managed_components/espressif__cjson/cJSON/cJSON.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    start = source.index('static esp_err_t replay_ack_cancel_status(')
    end = source.index('/* Reuse the exact request', start)
    (work / 'ack_wait.inc').write_text(source[start:end], encoding='utf-8')
    exe = work / 'ack_wait.exe'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(root / 'main'), '-I' + str(work),
        '-I' + str(root / 'main/audio_modes/meeting/host_stubs'),
        str(root / 'main/sd_audio_ack_wait_host_check.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
