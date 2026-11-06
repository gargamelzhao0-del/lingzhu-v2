"""Verify effective config, linked routes, and embedded identity; no device I/O."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('--nm', default='C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe')
a = p.parse_args()
b = a.build.resolve()
config = (b/'config/sdkconfig.h').read_text(encoding='utf-8')
def enabled(name):
    return re.search(r'^#define CONFIG_' + re.escape(name) + r' 1$', config, re.M) is not None
for name in ['NUWAX_TEST_PANEL_ENABLE', 'NUWAX_TEST_NETWORK_ENABLE',
             'NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE', 'NUWAX_OFFLINE_AUDIO_REPLAY_LANE_PROBE',
             'NUWAX_SD_GATEWAY_DISCOVERY_ENABLE', 'NUWAX_SD_REPLAY_EARLY_TCP_WRITABLE']:
    if enabled(name): raise SystemExit('FAIL: test feature still enabled: ' + name)
for name in ['NUWAX_OFFLINE_AUDIO_ENABLE', 'NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE',
             'NUWAX_SD_SESSION_ENABLE', 'NUWAX_SD_LEGACY_CONTROL_COMPAT']:
    if not enabled(name): raise SystemExit('FAIL: required formal feature disabled: ' + name)
if not re.search(r'^#define CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES 4194304$', config, re.M):
    raise SystemExit('FAIL: SD scene file size must remain 4 MiB')
for name in ['NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL', 'NUWAX_OFFLINE_AUDIO_REPLAY_TEST_TOKEN',
             'NUWAX_SD_GATEWAY_ID', 'NUWAX_SD_GATEWAY_DISCOVERY_KEY']:
    if re.search(r'^#define CONFIG_' + name + r'\b', config, re.M):
        raise SystemExit('FAIL: LAN gateway setting retained: ' + name)
for hours in ['NUWAX_MEETING_OFFLINE_MAX_HOURS', 'NUWAX_MONITORING_OFFLINE_MAX_HOURS']:
    if not re.search(r'^#define CONFIG_' + hours + r' 8$', config, re.M):
        raise SystemExit('FAIL: offline duration changed: ' + hours)
elf = b/'NuwaxAIPartner.elf'
binary = (b/'NuwaxAIPartner.bin').read_bytes()
elf_sha = hashlib.sha256(elf.read_bytes()).hexdigest()
if binary[176:208].hex() != elf_sha: raise SystemExit('FAIL: BIN/ELF mismatch')
symbols = subprocess.check_output([a.nm, '--defined-only', str(elf)], text=True)
names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
required = ['formal_replay_task', 'nuwax_offline_audio_replay_formal_start',
            'nuwax_sd_session_start', 'sd_audio_catalog_next',
            'nuwax_session_send_hello', 'nuwax_cloud_config_get',
            'nuwax_ota_progress_create', 'nuwax_ota_progress_update',
            'nuwax_ota_logo', 'nuwax_ota_capsule', 'nuwax_drawer_set_ota_blocked']
for name in required:
    if name not in names: raise SystemExit('FAIL: real formal route not linked: ' + name)
for name in ['nuwax_offline_audio_observe_formal_hello', 's_archive_capability_epoch']:
    if name in names: raise SystemExit('FAIL: obsolete SD hello gate still linked: ' + name)
if b'offline_audio_replay_v1' in binary:
    raise SystemExit('FAIL: obsolete SD capability declaration still embedded')
assert b'OTA display takeover revision=ota-drawer-v1' in binary
for name in ['replay_test_task', 'nuwax_sd_gateway_discover', 'nuwax_test_panel_create', 'nuwax_font_test_panel_11',
             'preview_tick']:
    if name in names: raise SystemExit('FAIL: test implementation linked: ' + name)
for value in [b'ws://192.168.31.180:8810', b'offline-replay-test',
              b'offline replay test scheduled url=', b'offline replay test task started',
              b'OTA visual boot preview active', b'OTA visual preview stopped']:
    if value in binary: raise SystemExit('FAIL: test route/token present in BIN: ' + value.decode())
result = dict(profile='formal-cloud', elfSha256=elf_sha,
              binSha256=hashlib.sha256(binary).hexdigest(),
              requiredSymbols=required, testImplementationsAbsent=True,
              testUrlAndTokenAbsent=True, otaBootPreviewAbsent=True,
              otaProductionUiLinked=True, sdHelloRequired=False,
              sdSessionEnabled=True, legacyControlCompatible=True, maxFileBytes=4194304,
              pcGatewayDiscoveryAbsent=True,
              offlineHours=8, hardwareValidated=False)
(b/'formal-route-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS: formal config, real worker, no test UI/gateway/token, 8h archive, BIN/ELF identity: ' + elf_sha)
