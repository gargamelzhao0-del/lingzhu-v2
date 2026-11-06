import hashlib
import json
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[2]
out = Path(__file__).resolve().parent
release = root / 'artifacts/release-7.0.113'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
sources = {}
for scope in ('main', 'components', 'bootloader_components', 'tools'):
    for p in sorted((root / scope).rglob('*')):
        if p.is_file() and (p.suffix in ('.c', '.h', '.inc', '.py', '.ps1', '.cmake', '.yml') or p.name in ('CMakeLists.txt', 'Kconfig.projbuild', 'Kconfig')) and not any(x in p.parts for x in ('node_modules', '__pycache__')):
            sources[p.relative_to(root).as_posix()] = sha(p)
for name in ('CMakeLists.txt', 'dependencies.lock', 'sdkconfig', 'sdkconfig.defaults', 'sdkconfig.product-ota.defaults', 'sdkconfig.sd-replay-formal.defaults', 'partitions.customwake.csv', 'tmp/sdkconfig.sd-replay-formal'):
    sources[name] = sha(root / name)
(out / 'source-hashes.json').write_text(json.dumps(sources, indent=2) + '\n', encoding='utf-8')
recorded = json.loads((release / 'verification/source-hashes.json').read_text(encoding='utf-8'))
drift = [p for p, h in recorded.items() if not (root / p).exists() or sha(root / p) != h]
bad = []
lines = (release / 'SHA256SUMS.txt').read_text(encoding='utf-8').splitlines()
for line in lines:
    expected, p = line.split('  ', 1)
    if sha(release / p) != expected: bad.append(p)
tracked = subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no', '--', '.'], cwd=root, text=True)
summary = dict(version='7.0.113', source_files=len(sources), source_manifest_sha256=sha(out / 'source-hashes.json'),
    recorded_release_source_files=len(recorded), changed_recorded_release_sources=drift,
    release_files_checked=len(lines), release_checksum_failures=bad,
    tracked_modified_or_deleted=len(tracked.splitlines()),
    source_snapshot_scope='First-party source, tools and build configuration; third-party source content not exhaustively snapshotted.')
(out / 'baseline-verification.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
print(json.dumps(summary, indent=2))
