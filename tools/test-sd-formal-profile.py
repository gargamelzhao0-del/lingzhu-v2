"""A reused PC sdkconfig must not retain the LAN route or lose product settings."""
import importlib.util
from pathlib import Path
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('formal_profile', root/'tools/prepare-sd-formal-config.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

with tempfile.TemporaryDirectory(prefix='nuwax-formal-profile-') as directory:
    work = Path(directory)
    seed = work/'seed'
    seed.write_text('''CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE=y
CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE=y
# CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE is not set
CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_ENABLE=y
CONFIG_NUWAX_SD_GATEWAY_ID="old-pc"
CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_KEY="fixture-secret"
CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL="ws://192.0.2.1:8810/api/device/ws"
CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_TOKEN="fixture-token"
CONFIG_NUWAX_TEST_NETWORK_ENABLE=y
CONFIG_NUWAX_SD_SESSION_ENABLE=n
CONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT=n
CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES=1048576
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y
CONFIG_LWIP_TCPIP_TASK_AFFINITY=1
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
CONFIG_NUWAX_CUSTOM_WAKE_WORD_THRESHOLD_PERCENT=10
CONFIG_NUWAX_PROVISION_RUNTIME_REBOOT_ENABLE=y
''', encoding='utf-8')
    target = work/'sdkconfig'
    module.prepare(target, seed)
    first = target.read_text(encoding='utf-8')
    for value in ('fixture-secret', 'fixture-token', '192.0.2.1', 'old-pc',
                  'CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y'):
        assert value not in first, value
    for value in ('# CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE is not set',
                  '# CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_ENABLE is not set',
                  '# CONFIG_NUWAX_TEST_NETWORK_ENABLE is not set',
                  'CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE=y',
                  'CONFIG_NUWAX_SD_SESSION_ENABLE=y',
                  'CONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT=y',
                  'CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES=4194304',
                  'CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y',
                  'CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=8192',
                  'CONFIG_LWIP_WND_SCALE=y',
                  'CONFIG_LWIP_TCP_RCV_SCALE=0',
                  'CONFIG_LWIP_TCP_SND_BUF_DEFAULT=131072',
                  'CONFIG_LWIP_TCP_WND_DEFAULT=11520',
                  'CONFIG_NUWAX_CUSTOM_WAKE_WORD_THRESHOLD_PERCENT=10',
                  'CONFIG_NUWAX_PROVISION_RUNTIME_REBOOT_ENABLE=y'):
        assert first.count(value) == 1, value
    module.prepare(target, seed)
    assert target.read_text(encoding='utf-8') == first
    module.prepare(target, seed, 16, 'unbound')
    assert 'CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y' not in target.read_text()
    assert 'CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y' in target.read_text()
    assert 'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=16' in target.read_text()
    # Fresh checkout without generated configs still selects the cloud profile.
    fresh = work/'fresh'
    module.prepare(fresh)
    assert 'CONFIG_NUWAX_SD_SESSION_ENABLE=y' in fresh.read_text()
    assert 'CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE=y' in fresh.read_text()
print('PASS: stale/fresh configs, removed LAN credentials, 4 MiB scenes, APP compatibility, idempotent rebuild')
