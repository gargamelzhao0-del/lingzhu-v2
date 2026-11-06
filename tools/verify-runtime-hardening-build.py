"""Verify linked runtime fixes, real cJSON budget, and honest release profile."""
from pathlib import Path
import hashlib
import json
import re
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
binary = (build/'NuwaxAIPartner.bin').read_bytes()
elf = build/'NuwaxAIPartner.elf'
sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == sha
config = (build/'config/sdkconfig.h').read_text(encoding='utf-8')
assert '#define CONFIG_CJSON_NESTING_LIMIT 32' in config
commands = json.loads((build/'compile_commands.json').read_text(encoding='utf-8'))
parser = [c for c in commands if Path(c['file']).name == 'cJSON.c']
assert len(parser) == 1 and '-DCJSON_NESTING_LIMIT=32 ' in parser[0]['command']
for marker in [b'revision=20260924-runtime-v1', b'notification_startup_buffer_timeout',
               b'endpoint_configured=%d secret_present=%d secret_len=%u', b'content=redacted']:
    assert marker in binary, marker
for removed in [b'secret_hint=%s', b'secret_fingerprint=%s', b'%s len=%u preview=%s',
                b'%s WebSocket connected to %s epoch=%lu',
                b'dynamic cloud config updated device=%s url=%s']:
    assert removed not in binary, removed
sources = ['main/conversation/speaker_playback.c','main/conversation/speaker_jitter_policy.h',
           'main/session_protocol.c','main/nuwax_ai_partner_main.c','main/ws_transport.c',
           'main/nuwax_provision_new/nuwax_cloud_config.c','CMakeLists.txt',
           'sdkconfig.defaults','sdkconfig.product-ota.defaults','sdkconfig.sd-replay-formal.defaults']
for name in [p for p in sources if p.endswith('.c')]:
    path = root/name
    assert len([c for c in commands if Path(c['file']).resolve() == path.resolve()]) == 1
    obj = build/'esp-idf/main/CMakeFiles/__idf_main.dir'/(name.removeprefix('main/')+'.obj')
    assert path.stat().st_mtime <= obj.stat().st_mtime <= elf.stat().st_mtime, name
flags = ['NUWAX_PROVISION_SEC2_DEV_MODE','NUWAX_CLOUD_CONFIG_ALLOW_INSECURE_WS','NUWAX_PROVISION_USB_CONTROL_ENABLE']
development = [f for f in flags if '#define CONFIG_'+f+' 1' in config]
result = dict(revision='20260924-runtime-v1', elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary), jsonMaxDepth=32,
              notificationStartupMs=10000, hardwareValidated=False,
              provisioningProfile='compatibility' if development else 'restricted',
              developmentFlags=development, releaseSecurityValidated=False,
              sourceHashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources})
(build/'runtime-hardening-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(f'PASS: runtime markers, real cJSON depth=32, sensitive formats absent, fresh objects; BIN/ELF {sha}')
print('PROFILE: '+result['provisioningProfile']+'; production security migration is NOT certified')
if '--require-release' in sys.argv and development:
    raise SystemExit('FAIL: release admission rejected development provisioning/transport/debug settings')
