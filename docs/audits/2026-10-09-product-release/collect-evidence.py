from pathlib import Path
import hashlib
import json
import re
import shutil

out = Path(__file__).resolve().parent
root = out.parents[1]
build = Path('C:/tmp/nuwax-sd-formal-build-20260921')
config = (build / 'config/sdkconfig.h').read_text(encoding='utf-8-sig')
macros = dict(re.findall(r'^#define (\w+) (.+)$', config, re.M))
image_path = root / 'artifacts/release-7.0.113/ota/NuwaxAIPartner-7.0.113-ota.bin'
image = image_path.read_bytes()
secret = json.loads(macros['CONFIG_NUWAX_PROVISION_SEC2_DEV_POP'])
keys = ['CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE', 'CONFIG_NUWAX_CLOUD_CONFIG_ALLOW_INSECURE_WS',
        'CONFIG_NUWAX_PROVISION_USB_CONTROL_ENABLE', 'CONFIG_SECURE_BOOT', 'CONFIG_SECURE_FLASH_ENC_ENABLED',
        'CONFIG_NVS_ENCRYPTION', 'CONFIG_NUWAX_PRODUCTION_SECURITY_REQUIRED', 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE']
summary = dict(config_source=str(build / 'config/sdkconfig.h'),
    config_sha256=hashlib.sha256((build / 'config/sdkconfig.h').read_bytes()).hexdigest(),
    binary_sha256=hashlib.sha256(image).hexdigest(),
    options={k: macros.get(k, 'not defined') for k in keys},
    configured_shared_development_pop_present_in_binary=bool(secret) and secret.encode() in image,
    secret_value_disclosed=False,
    scope='Compile configuration and packaged binary only; device eFuse, active NVS endpoint and account access not read.')
(out / 'security-profile-evidence.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
dest = root / 'docs/audits/2026-10-09-product-release'
dest.mkdir(parents=True, exist_ok=True)
for p in out.rglob('*'):
    if p.is_file() and p.suffix in ('.json', '.log', '.txt', '.c', '.h', '.inc', '.py'):
        target = dest / p.relative_to(out)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, target)
for relative, original in [('audio-ux/reproduce.py', 'ROOT = OUT.parents[2]'),
                           ('data-network/reproduce_false_recording_state.py', 'ROOT = Path(__file__).resolve().parents[3]')]:
    path = dest / relative
    content = path.read_text(encoding='utf-8')
    assert original in content
    content = content.replace(original, 'ROOT = next(p for p in Path(__file__).resolve().parents if (p / "main/nuwax_ai_partner_main.c").is_file())', 1)
    path.write_text(content, encoding='utf-8')
print('Collected audit evidence; development PoP presence checked without disclosure; production source unchanged.')
