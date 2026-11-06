from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/sd_audio_session_replay.inc').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='nuwax-scene-state-') as directory:
    work = Path(directory)
    (work / 'sd_file_fault_production_host.inc').write_text(source[source.index('static esp_err_t card_confirm_missing_locked('):source.index('/* Signature retained')],encoding='utf-8')
    transport = (root / 'main/ws_transport.c').read_text(encoding='utf-8')
    start = transport.index('esp_err_t nuwax_ws_send_archive_text_if_epoch(')
    end = transport.index('\nesp_err_t nuwax_ws_send_ping(', start)
    (work / 'sd_archive_text_host.inc').write_text(transport[start:end], encoding='utf-8')
    (work / 'sd_scene_end_host.inc').write_text(source[source.index('static esp_err_t card_session_end('):source.index('static esp_err_t card_replay_next_impl(')], encoding='utf-8')
    (work / 'sd_scene_scheduler_host.inc').write_text(source[source.index('static esp_err_t card_replay_next_impl('):source.rindex('#endif')], encoding='utf-8')
    exe = work / 'scene.exe'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
        '-DSD_AUDIO_ARCHIVE_ROOT="."', '-DSD_AUDIO_ARCHIVE_NEW_ROOT="."',
        '-I' + str(work), '-I' + str(root / 'main'),
        '-I' + str(root / 'main/audio_modes/meeting/host_stubs'),
        '-I' + str(root / 'managed_components/espressif__cjson/cJSON'),
        str(root / 'main/sd_audio_scene_host_check.c'), str(root / 'main/sd_audio_catalog.c'),
        str(root / 'main/cloud_time.c'),
        str(root / 'managed_components/espressif__cjson/cJSON/cJSON.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=work, check=True)
