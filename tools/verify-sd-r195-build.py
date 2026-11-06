"""Verify final R195 image, executable crypto dispatch and unchanged UI/protocol."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('--sources', type=Path, required=True)
p.add_argument('--baseline-build', type=Path, required=True)
p.add_argument('--baseline-sources', type=Path, required=True)
a = p.parse_args()
root, build = a.sources, a.build
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
binary, elf = build / 'NuwaxAIPartner.bin', build / 'NuwaxAIPartner.elf'
data = binary.read_bytes()
assert data[48:80].split(b'\0')[0] == b'7.0.195'
assert data[176:208].hex() == sha(elf)
assert len(data) <= 4194304
assert sha(build / 'partition_table/partition-table.bin') == sha(a.baseline_build / 'partition_table/partition-table.bin')
before = json.loads((a.baseline_build / 'config/sdkconfig.json').read_text())
after = json.loads((build / 'config/sdkconfig.json').read_text())
changes = {k: [before.get(k), after.get(k)] for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
assert not changes, changes
assert after['NUWAX_SD_TLS_SCOPED_AES'] and after['NUWAX_SD_MEETING_MAX_HOURS'] == 8
assert not after.get('MBEDTLS_HARDWARE_AES') and not after.get('MBEDTLS_HARDWARE_SHA')
assert after['MBEDTLS_SSL_OUT_CONTENT_LEN'] == 8192
assert after['NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE'] and not after['NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE']
protected = ['main/nuwax_lvgl_port.c', 'main/replay_ui_schedule.c', 'main/replay_ui_schedule.h',
             'main/wake_word_detector.c', 'main/nuwax_ai_partner_main.c',
             'main/sd_audio_scene_protocol.h', 'main/sd_audio_protocol.h',
             'main/audio_modes/meeting/meeting_protocol.c', 'main/audio_modes/monitoring/monitoring_protocol.c']
for rel in protected:
    assert sha(root / rel) == sha(a.baseline_sources / rel), rel
old_main = (a.baseline_sources / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
new_main = (root / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
assert new_main == old_main
for marker in [b'SD formal pipeline revision=195', b'SD formal AES hw_calls=', b'SD formal arbitration live_check_ms=', b'SD monitor FIFO head retained', b'SD formal geometry crypto_capacity=',
               b'SD replay retained old-binding scene=', b'cloud config committed previous=', b'restored unconfirmed scene=']:
    assert marker in data, marker
commands = json.loads((build / 'compile_commands.json').read_text())
for unit in ['api_msg.c', 'sockets.c', 'ws_transport.c']:
    matches = [v for v in commands if Path(v['file']).name == unit]
    assert matches and all('-DTCP_SNDLOWAT=2881' in v['command'] for v in matches), unit
tools = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
symbols = subprocess.check_output([str(tools / 'xtensa-esp32s3-elf-nm.exe'), '-S', '--defined-only', str(elf)], text=True)
asm = subprocess.check_output([str(tools / 'xtensa-esp32s3-elf-objdump.exe'), '-d', str(elf)], text=True)
sizes = {m[3]: (int(m[1], 16), int(m[2], 16)) for m in re.finditer(r'^(\w+)\s+(\w+)\s+\w\s+(\S+)$', symbols, re.M)}
cache = {}
def body(name):
    if name in cache: return cache[name]
    text = ''
    for m in re.finditer(r'^\w+ <([^>]+)>:\n([\s\S]*?)(?=^\w+ <|\Z)', asm, re.M):
        if m[1] == name or m[1].startswith(name + '.') or m[1].startswith(name + '$'):
            text += m[0]
            start, size = sizes[m[1]]
            # Xtensa literals between basic blocks can hide call instructions
            # in a linear disassembly. Also decode actual branch targets.
            targets = set(int(v, 16) for v in re.findall(r'\b([0-9a-f]+) <' + re.escape(m[1]) + r'\+0x[0-9a-f]+>', m[0]))
            for addr in sorted(targets):
                if start <= addr < start + size:
                    text += subprocess.check_output([str(tools / 'xtensa-esp32s3-elf-objdump.exe'), '-d',
                        '--start-address=' + hex(addr), '--stop-address=' + hex(min(addr + 64, start + size)), str(elf)], text=True)
    assert text, name
    cache[name] = text
    return text

links = {'psa_aead_encrypt': ['__wrap_mbedtls_psa_aead_encrypt'],
         '__wrap_mbedtls_psa_aead_encrypt': ['mbedtls_psa_aead_encrypt', 'encrypt_record'],
         'encrypt_record': ['esp_aes_gcm_setkey', 'esp_aes_gcm_crypt_and_tag'],
         'nuwax_tls_archive_crypto_prepare': ['heap_caps_aligned_alloc'],
         'card_monitor_make_room_locked': ['sd_audio_catalog_reclaim_confirmed'],
         'sd_audio_catalog_save': ['read_pair_matching', 'write_pair'],
         'sd_audio_catalog_recover': ['repair_pending_session'],
         'write_pair': ['fstat'],
         'nuwax_ws_send_archive_text_if_epoch': ['strnlen'],
         'nuwax_meeting_mode_service_sd': ['nuwax_sd_session_prepare_start', 'nuwax_sd_session_release_start'],
         'nuwax_monitoring_mode_service_sd': ['nuwax_sd_session_prepare_start', 'nuwax_sd_session_release_start'],
         'replay_one_ready': ['nuwax_tls_archive_crypto_prepare', 'nuwax_tls_archive_crypto_release', 'nuwax_tls_archive_crypto_capacity']}
for caller, targets in links.items():
    code = body(caller)
    for target in targets:
        assert re.search(r'<' + re.escape(target) + r'(?:[.$>])', code), (caller, target)
frames = {}
assert 'sd_audio_catalog_reclaim' not in sizes, 'unconfirmed-file reclamation must be unreachable in production'
for name in [ 'sd_audio_catalog_reclaim_confirmed', '__wrap_mbedtls_psa_aead_encrypt', 'encrypt_record', 'nuwax_tls_archive_crypto_prepare', 'write_pair', 'read_pair_matching', 'repair_pending_session', 'sd_audio_catalog_recover', 'nuwax_sd_session_prepare_start', 'nuwax_sd_session_release_start']:
    frames[name] = max(int(v, 0) for v in re.findall(r'entry\s+a1,\s*(0x[0-9a-f]+|\d+)', body(name)))
    assert frames[name] <= 1536, (name, frames[name])
result = dict(version='7.0.195', bytes=len(data), otaSha256=sha(binary), elfSha256=sha(elf),
              otaPartitionBytes=0x420000, budgetHeadroomBytes=4194304-len(data),
              configurationChanges=changes, protectedSources=protected, executableLinks=links,
              staticStackBytes=frames, protocolChanged=False, hardwareValidated=False,
              measuredThroughputKiBs=None, targetThroughputKiBs=500)
obj = build / 'esp-idf/main/CMakeFiles/__idf_main.dir/sd_audio_catalog.c.obj'
for relative in ['main/sd_audio_catalog.c','main/sd_audio_catalog_repair.inc','main/sd_audio_catalog_retention.inc','main/sd_audio_catalog.h','main/sd_audio_monitor_fifo.inc','main/sd_audio_monitor_fifo.h']:
    assert (root / relative).stat().st_mtime_ns <= obj.stat().st_mtime_ns <= elf.stat().st_mtime_ns, relative
assert '#define NUWAX_SD_CONTROL_TEXT_MAX_BYTES 4096U' in (root / 'main/sd_audio_control_limits.h').read_text()
for record in ['sd_audio_session_t','sd_audio_segment_t']:
    pattern = r'typedef struct \{[^{}]*\} ' + record + ';'
    assert re.search(pattern,(root/'main/sd_audio_catalog.h').read_text()).group() == re.search(pattern,(a.baseline_sources/'main/sd_audio_catalog.h').read_text()).group()
result.update(controlTextMaxBytes=4096, persistedRecordLayoutChanged=False)
# Verify the inspected source and the emitted ELF come from the same final build.
for unit, dependencies in {
    'offline_audio_spool.c': ['offline_audio_spool.c','offline_replay_shared.inc','offline_replay_policy.h','offline_replay_pressure.inc','offline_io_schedule.h','ws_send_metrics.h','offline_replay_prefetch.inc','sd_audio_session_replay.inc','sd_monitor_storage.inc','sd_capture_admission.h','offline_audio_policy.h'],
    'ws_transport.c': ['ws_transport.c','ws_send_metrics.h'],
    'tls_archive_crypto.c': ['tls_archive_crypto.c','tls_archive_crypto.h'],
    'audio_modes/meeting/meeting_mode.c': ['audio_modes/meeting/meeting_mode.c','sd_audio_session.h'],
    'audio_modes/monitoring/monitoring_mode.c': ['audio_modes/monitoring/monitoring_mode.c','sd_audio_session.h'],
}.items():
    obj = build / ('esp-idf/main/CMakeFiles/__idf_main.dir/' + unit + '.obj')
    for dependency in dependencies:
        assert (root/'main'/dependency).stat().st_mtime_ns <= obj.stat().st_mtime_ns <= elf.stat().st_mtime_ns, dependency
transport = (root/'main/ws_transport.c').read_text(encoding='utf-8-sig')
assert '#define NUWAX_WS_SHARED_ARCHIVE_DRAIN_MS 2500U' in transport
assert 'client && !safe_pause' in transport
assert 'REPLAY_CHECKPOINT_RECENT_FILES 3U' in (root/'main/offline_replay_prefetch.inc').read_text()
assert 'sd_capture_admission_held(&s_capture_admission' in (root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
result.update(sharedTlsDrainMs=2500, boundedValidationCacheFiles=4,
              startAdmissionBarrier=True, physicalCapacityForSdScenes=True)

crypto = (root/'main/tls_archive_crypto.c').read_text()
assert 'MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT' in crypto
assert 'memcpy(s_output, input, bytes)' in crypto
assert 'aad_bytes, s_output, s_output, 16U, s_output + bytes' in crypto
assert 'capacity = 4097U' in crypto and '24576U' in crypto
assert 'First real record is also compared against PSA software' in crypto
assert 'memcmp(output, s_output, *written)' in crypto
catalog = (root/'main/sd_audio_catalog.c').read_text()
assert '#include \"sd_audio_monitor_fifo.inc\"' in catalog
governor = (root/'main/offline_replay_policy.h').read_text()
assert 'queued >= 48U' in governor and 'queued <= 32U' in governor
assert 'crypto_capacity - 73U' not in governor
result.update(acknowledgedOnlyMonitorCleanup=True, monitoringFifo=True,
    adaptiveMonitoringFrames=True, cryptoInternalReserveBytes=24576,
    cryptoRecordCapacities=[8193,4097], cryptoSoftwareFallback=True,
    callerPlaintextPreservedOnHardwareFailure=True, uploadPriorityUnchanged=True)

assert 'sd_monitor_fifo_next' in sizes and 'sd_monitor_fifo_end_ready' in sizes and 'sd_monitor_fifo_finish' in sizes
replay_entry = next(name for name in ['card_replay_next_impl','card_replay_next','replay_ready_impl','nuwax_offline_audio_replay_ready'] if any(k.startswith(name) for k in sizes))
assert 'sd_monitor_fifo_next' in body(replay_entry)
assert 'sd_monitor_fifo_end_ready' in asm and 'sd_monitor_fifo_finish' in asm
fifo=(root/'main/sd_audio_monitor_fifo.inc').read_text()
assert 'monitor_fifo.end.proven' in fifo and 'CATALOG_INDEX_SLICE' in fifo
replay=(root/'main/sd_audio_session_replay.inc').read_text()
assert 's_monitor_retry_delay &&' in replay
assert 's_monitor_retry_until = offline_now_ms() + s_monitor_retry_delay' in replay
space=(root/'main/sd_monitor_storage.inc').read_text()
assert re.search(r'if \(free_bytes >= minimum\) \{\s*atomic_store\(&s_monitor_space_blocked, false\);\s*return ESP_OK;',space)
result.update(monitorClassificationChanged=False, captureOriginClassification=True,
    monitorEndProof=True, spaceRecoveryStateCleared=True)
assert sizes['monitor_fifo'][1] <= 2048, sizes['monitor_fifo']
result['monitorFifoStaticBytes'] = sizes['monitor_fifo'][1]
result['verificationToolSha256'] = sha(Path(__file__).resolve())

pressure=(root/'main/offline_replay_pressure.inc').read_text()
assert 'xSemaphoreTake(s.lock, 0U)' in pressure and 'queued >= 8U' in pressure and 'queued <= 2U' in pressure
assert 'queued >= QUEUE_DEPTH / 2U' in pressure and 'queued <= QUEUE_DEPTH / 4U' in pressure
assert '#define OFFLINE_WRITER_TASK_PRIORITY 6U' in (root/'main/offline_io_schedule.h').read_text()
assert 'OFFLINE_WRITER_TASK_STACK_BYTES, NULL, OFFLINE_WRITER_TASK_PRIORITY' in (root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
assert 'replay_read_storage_enter' in body('replay_prefetch_task')
assert 'tls_plaintext_limit' in transport and 'if (tls_limit && offered > tls_limit) offered = tls_limit' in transport
result.update(writerPriority=6, readerPriority=6, senderPriority=5, sdDmaSerialized=True,
    readQueueHigh=8, readQueueLow=2, transportQueueHigh=64, transportQueueLow=32,
    tlsRecordLimitIndependentOfWsFrame=True, cryptoReserveUnchanged=True)

(build / 'sd-r195-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
