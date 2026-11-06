from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks'
out.mkdir(exist_ok=True)
spool = (root / 'main/offline_audio_spool.c').read_text(encoding='utf-8')
catalog = (root / 'main/sd_audio_session_storage.inc').read_text(encoding='utf-8')
pieces = {
    'sd_storage_types_host.inc': spool[spool.index('typedef struct {\n    uint8_t type;'):spool.index('static spool_state_t s;')],
    'sd_storage_crc_host.inc': spool[spool.index('static uint32_t crc32_update('):spool.index('static esp_err_t validate_archive_frame(')],
    'sd_storage_catalog_host.inc': catalog[catalog.index('static bool card_recording_locked('):catalog.rindex('#endif')],
    'sd_storage_functions_host.inc': spool[spool.index('static FILE *archive_create_exclusive('):spool.index('static void writer_schedule_retry_locked(')],
    'sd_storage_loss_host.inc': spool[spool.index('static void mirror_note_loss('):spool.index('static void mirror_drain(')],
    'sd_file_validator_host.inc': spool[spool.index('static esp_err_t validate_archive_frame('):spool.index('static void release_slot(')],
    'sd_file_recovery_host.inc': spool[spool.index('static esp_err_t recover_open_segments('):spool.index('static uint64_t scan_used_bytes(')],
}
for name, content in pieces.items():
    (out / name).write_text(content, encoding='utf-8')
exe = out / 'sd_audio_file_time_host_check.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-DCONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES=4194304',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
    '-I' + str(out), '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(root / 'main'),
    str(root / 'main/sd_audio_catalog.c'), str(root / 'main/audio_frame.c'),
    str(root / 'main/sd_audio_file_time_host_check.c'), '-o', str(exe)], check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-sd-file-time-') as directory:
    fixtures = Path(directory) / 'fixtures'
    fixtures.mkdir()
    subprocess.run([str(exe), str(fixtures)], cwd=directory, check=True)
    subprocess.run(['node', str(root / 'tools/protocol/sd-file-time-test.js'), str(fixtures)], cwd=root, check=True)
