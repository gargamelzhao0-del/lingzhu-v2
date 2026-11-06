"""Apply the formal SD route even when a reused sdkconfig selects the PC gateway."""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ENTRY = re.compile(r'^(?:CONFIG_([A-Z0-9_]+)=(.*)|# CONFIG_([A-Z0-9_]+) is not set)$')


def prepare(config: Path, seed: Path | None = None, wifi_cache: int = 32,
            tcpip_affinity: str = 'cpu0') -> None:
    source = config if config.exists() else seed
    text = source.read_text(encoding='utf-8-sig') if source and source.exists() else ''
    overlay = (ROOT / 'sdkconfig.sd-replay-formal.defaults').read_text(encoding='utf-8')
    settings = {}
    for line in overlay.splitlines():
        match = ENTRY.fullmatch(line)
        if match:
            settings[match[1] or match[3]] = match[2] if match[1] else 'n'
    settings['ESP_WIFI_CACHE_TX_BUFFER_NUM'] = str(wifi_cache)
    choice = 'CPU0' if tcpip_affinity == 'cpu0' else 'NO_AFFINITY'
    settings.pop('LWIP_TCPIP_TASK_AFFINITY_CPU0', None)
    settings['LWIP_TCPIP_TASK_AFFINITY_' + choice] = 'y'
    kept = []
    for line in text.splitlines():
        match = ENTRY.fullmatch(line)
        name = (match[1] or match[3]) if match else ''
        # Drop inactive LAN credentials too. The provisioned cloud credentials
        # live in device NVS and are never read or rewritten by this tool.
        if (name in settings or name.startswith('NUWAX_SD_GATEWAY_') or
                name.startswith('NUWAX_OFFLINE_AUDIO_REPLAY_TEST_') or
                name.startswith('LWIP_TCPIP_TASK_AFFINITY')):
            continue
        kept.append(line)
    kept.extend(f'# CONFIG_{name} is not set' if value == 'n' else f'CONFIG_{name}={value}'
                for name, value in settings.items())
    config.parent.mkdir(parents=True, exist_ok=True)
    temporary = config.with_name(config.name + '.formal.tmp')
    temporary.write_text('\n'.join(kept).rstrip() + '\n', encoding='utf-8')
    temporary.replace(config)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=Path)
    parser.add_argument('--seed', type=Path)
    parser.add_argument('--wifi-cache', type=int, choices=(16, 32), default=32)
    parser.add_argument('--tcpip-affinity', choices=('cpu0', 'unbound'), default='cpu0')
    args = parser.parse_args()
    prepare(args.config, args.seed, args.wifi_cache, args.tcpip_affinity)
    print('PASS: formal SD route selected; PC route/discovery disabled and LAN credentials removed')
