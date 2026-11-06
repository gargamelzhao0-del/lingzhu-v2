"""Exercise the actual VerifyOnly path; never open serial or modify firmware."""
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parent.parent

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

with tempfile.TemporaryDirectory(prefix='nuwax-flash-identity-') as temp:
    test = Path(temp)
    (test/'config').mkdir()
    elf, app, extra = (test/n for n in ['NuwaxAIPartner.elf', 'NuwaxAIPartner.bin', 'assets.bin'])
    elf.write_bytes(b'host-only-elf-fixture')
    binary = bytearray(224)
    binary[176:208] = bytes.fromhex(sha(elf))
    app.write_bytes(binary)
    extra.write_bytes(b'host-only-asset')
    (test/'config/sdkconfig.h').write_text('#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 1\n')
    manifest = {'write_flash_args': [], 'flash_files': {'0x0': app.name, '0x400000': extra.name}}
    (test/'flasher_args.json').write_text(json.dumps(manifest))
    result = {'revision': 'host-fixture', 'elfSha256': sha(elf), 'binSha256': sha(app),
              'images': [dict(offset=k, file=v, sha256=sha(test/v)) for k,v in manifest['flash_files'].items()]}
    (test/'build-result.json').write_text(json.dumps(result))
    cmd = ['powershell', '-ExecutionPolicy', 'Bypass', '-File', str(root/'tools/flash-sd-replay-test.ps1'),
           '-Port', 'NOT_A_PORT', '-BuildDir', str(test), '-VerifyOnly']

    def run(ok, word):
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding='utf-8', errors='replace')
        assert (p.returncode == 0) == ok, p.stdout
        assert word in p.stdout, p.stdout

    run(True, 'serial port was not opened')
    elf.write_bytes(b'wrong-elf')
    run(False, 'BIN and ELF')
    elf.write_bytes(b'host-only-elf-fixture')
    app.write_bytes(binary + b'x')
    run(False, 'Snapshot identity')
    app.write_bytes(binary)
    extra.write_bytes(b'wrong-assets')
    run(False, 'Snapshot image checksum')
    extra.write_bytes(b'host-only-asset')
    run(True, 'serial port was not opened')
    manifest['flash_files'].pop('0x400000')
    (test/'flasher_args.json').write_text(json.dumps(manifest))
    run(False, 'Snapshot manifest is incomplete')
print('PASS: flash preflight accepts matching images and rejects wrong ELF, BIN and assets without opening serial')
