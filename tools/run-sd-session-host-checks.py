from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
output = root / 'build-host-checks'
output.mkdir(exist_ok=True)
lease_exe = output / 'sd_audio_lease_host_check.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
    '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(root / 'main'),
    str(root / 'main/sd_audio_lease_host_check.c'), '-o', str(lease_exe)], check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-sd-lease-') as directory:
    subprocess.run([str(lease_exe)], cwd=directory, check=True)
replay = (root / 'main/sd_audio_session_replay.inc').read_text(encoding='utf-8')
(output / 'sd_file_fault_production_host.inc').write_text(replay[replay.index('static esp_err_t card_confirm_missing_locked('):replay.index('/* Signature retained')],encoding='utf-8')
(output / 'sd_scheduler_host.inc').write_text(replay[replay.index('static esp_err_t card_replay_next_impl('):replay.rindex('#endif')], encoding='utf-8')
recovery_exe = output / 'sd_audio_recovery_host_check.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
    '-I' + str(output), '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(root / 'main'),
    str(root / 'main/sd_audio_recovery_host_check.c'), '-o', str(recovery_exe)], check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-sd-recovery-') as directory:
    subprocess.run([str(recovery_exe)], cwd=directory, check=True)
exe = output / 'sd_audio_catalog_host_check.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
    '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(root / 'main'),
    str(root / 'main/sd_audio_catalog.c'), str(root / 'main/sd_audio_catalog_host_check.c'),
    '-o', str(exe)], check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-sd-catalog-') as directory:
    subprocess.run([str(exe)], cwd=directory, check=True)

# Exercise the actual archive writer and catalog together against real files.
spool = (root / 'main/offline_audio_spool.c').read_text(encoding='utf-8')
catalog = (root / 'main/sd_audio_session_storage.inc').read_text(encoding='utf-8')
(output / 'sd_identity_start_host.inc').write_text(
    catalog[catalog.index('esp_err_t nuwax_sd_session_check_new_identity('):catalog.index('void nuwax_sd_session_stop(')],
    encoding='utf-8')
(output / 'sd_identity_async_host.inc').write_text(
    catalog[catalog.index('typedef struct {\n    char session_id[48];'):catalog.index('static bool card_prepare_capture(')],
    encoding='utf-8')
identity_exe = output / 'sd_audio_identity_host_check.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-DCONFIG_NUWAX_SD_SESSION_ENABLE=1',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
    '-I' + str(output), '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(root / 'main'),
    str(root / 'main/sd_audio_catalog.c'), str(root / 'main/sd_audio_identity_host_check.c'),
    '-o', str(identity_exe)], check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-sd-identity-') as directory:
    subprocess.run([str(identity_exe)], cwd=directory, check=True)
from c_source_extract import function
pieces = {
    'types': spool[spool.index('typedef struct {\n    uint8_t type;'):spool.index('static spool_state_t s;')],
    'crc': spool[spool.index('static uint32_t crc32_update('):spool.index('static esp_err_t validate_archive_frame(')],
    'catalog': catalog[catalog.index('static bool card_recording_locked('):catalog.rindex('#endif')],
    'functions': spool[spool.index('static FILE *archive_create_exclusive('):spool.index('static void writer_schedule_retry_locked(')],
    'loss': function(catalog, 'card_note_mirror_result') + '\n' + spool[spool.index('static void mirror_note_loss('):spool.index('static void mirror_drain(')],
}
for name, text in pieces.items():
    (output / f'sd_storage_{name}_host.inc').write_text(text, encoding='utf-8')
exe = output / 'sd_audio_storage_host_check.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
    '-I' + str(output), '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(root / 'main'),
    str(root / 'main/sd_audio_catalog.c'), str(root / 'main/audio_frame.c'), str(root / 'main/sd_audio_storage_host_check.c'),
    '-o', str(exe)], check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-sd-writer-') as directory:
    subprocess.run([str(exe)], cwd=directory, check=True)

# Same writer with the actual 4MiB test profile: full-file monitor sealing,
# rollover/CRC/timestamps and online/offline tail boundaries must still work.
large_exe = output / 'sd_audio_storage_4m_host_check.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-DCONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES=4194304',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
    '-I' + str(output), '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(root / 'main'),
    str(root / 'main/sd_audio_catalog.c'), str(root / 'main/audio_frame.c'), str(root / 'main/sd_audio_storage_host_check.c'),
    '-o', str(large_exe)], check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-sd-writer-4m-') as directory:
    subprocess.run([str(large_exe)], cwd=directory, check=True)

for name in ('meeting', 'monitoring'):
    mode_dir = root / 'main/audio_modes' / name
    wrapper = output / f'sd_{name}_mode_host_check.c'
    definitions = {'MODE_TYPE': f'nuwax_{name}_mode_t', 'CALLBACK_TYPE': f'nuwax_{name}_mode_callbacks_t'}
    for macro, suffix in [('MODE_INIT','init'), ('MODE_DEINIT','deinit'), ('MODE_UI','request_ui'),
                          ('MODE_SERVICE','service_sd'), ('MODE_RUN','run'), ('MODE_DISCONNECT','notify_ws_disconnected')]:
        definitions[macro] = f'nuwax_{name}_mode_{suffix}'
    definitions['MODE_NAME'] = '"' + name + '"'
    definitions['MODE_TEST_MEETING'] = '1' if name == 'meeting' else '0'
    for state in ('STARTING','ACTIVE','STOPPING','DISABLED'):
        definitions['STATE_' + state] = f'NUWAX_{name.upper()}_STATE_{state}'
    wrapper.write_text('#define main legacy_controller_main\n#define nuwax_ws_send_text sd_base_send_text\n' +
        f'#include "{(mode_dir / (name + "_mode_host_check.c")).as_posix()}"\n#undef main\n' +
        '#undef nuwax_ws_send_text\nstatic bool sd_fail_command_ack;\n' +
        'esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *ws, const char *payload) {\n' +
        ' if (sd_fail_command_ack && strstr(payload, "device_command_ack")) return ESP_FAIL;\n' +
        ' return sd_base_send_text(ws, payload);\n}\n' +
        '\n'.join(f'#define {key} {value}' for key, value in definitions.items()) +
        '\n#include "sd_audio_mode_host_check.inc"\n', encoding='utf-8')
    sources = [mode_dir / f'{name}_mode.c', mode_dir / f'{name}_protocol.c', wrapper,
               root / 'main/audio_modes/meeting/host_stubs/device_error_report.c',
               root / 'managed_components/espressif__cjson/cJSON/cJSON.c']
    if name == 'meeting': sources.append(mode_dir / 'meeting_processor.c')
    for compat in (0, 1):
        suffix = '' if compat else '_strict'
        executable = output / f'sd_{name}_mode_host_check{suffix}.exe'
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
            '-DCONFIG_NUWAX_SD_SESSION_ENABLE=1', '-DCONFIG_NUWAX_OFFLINE_AUDIO_ENABLE=1',
            f'-DCONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT={compat}',
            '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(mode_dir),
            '-I' + str(root / 'main'), '-I' + str(root / 'main/conversation'),
            '-I' + str(root / 'managed_components/espressif__cjson/cJSON'),
            *map(str, sources), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
