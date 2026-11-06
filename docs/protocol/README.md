# Nuwax Device Protocol Assets

This directory contains machine-readable protocol assets for the device/server WebSocket contract.

## Files

- [`../SD会议监控场次上传协议.md`](../SD会议监控场次上传协议.md)
  - Current card-session wire contract, reviewed against 7.0.173: one session begin, per-file commit, session_end, timestamps, control identity and binary layouts.
- [`../SD会议监控功能逻辑与异常处理.md`](../SD会议监控功能逻辑与异常处理.md)
  - Current recording, capacity-based monitoring retention, recovery, paired PC gateway profile and verification limits.
  - These two documents take precedence for the card-session path. Root/default builds and legacy offline_audio profiles are separate configurations.
- `../SD会议监控功能与协议完整说明.md`
  - Historical 7.0.125/7.0.150 description; not the current integration contract.

- `meeting-monitoring-sd-current-20260910.md`
  - Historical September implementation contract, retained for the legacy path.
  - Does not override the current card-session documents above.
  - The separate complete-v2 document remains a future protocol design.

- `device-cloud-protocol-complete-v1.md`
  - Current complete device/cloud WebSocket protocol entry point.
  - Covers authentication, keepalive, normal conversation, NA v1 uplink/downlink audio, barge-in, all device commands, meeting, monitoring, Wi-Fi, MCP, OTA, ACK/retry rules, and current compatibility messages.

- `正式云端OTA协议与开发指南-v1.md`
  - Formal-cloud-only OTA contract and implementation guide.
  - Defines boot-scoped firmware reporting, the minimal update/ACK/result contract, local-only progress, transaction reconciliation, rollout, security, and acceptance gates.

- `meeting-monitoring-formal-cloud-complete-v1.md`
  - Mode-specific formal-cloud handoff retained for meeting and monitoring integration details.
  - Includes the current two-minute no-speech pause and cloud-ACK-gated resume flow.

- `meeting-mode-current-v1.md`
  - Historical mode-specific meeting reference retained for traceability. The standalone handoff above takes precedence.

- `monitoring-mode-v1.md`
  - Historical mode-specific monitoring reference retained for traceability. The standalone handoff above takes precedence.

- `meeting-mode-v1.md`
  - Obsolete target design containing fields not implemented by current firmware. Do not use it for cloud integration.

- `schemas/nuwax-device-protocol.schema.json`
  - Draft 2020-12 JSON Schema for device/server text messages.
  - Keeps `additionalProperties: true` for forward compatibility, while constraining known field names, types, enums, and device buffer lengths.
  - Fixed device buffers are constrained with `x-maxUtf8Bytes`; JSON Schema `maxLength` alone counts Unicode code points and is not sufficient for ESP32 UTF-8 buffers.
  - `stop_playback` is a scoped terminal command and requires both `turn_id` and `playback_id`. The device waits for the matching speaker generation to stop physically before resuming normal listening.
  - `set_state` is only a compatibility alias for semantic sleep and accepts `sleeping` or `wake_only`. Boot, network, listening, thinking, speaking, idle, and error states are read-only outcomes owned by the device's local transport/audio lifecycle.
  - `playback_begin`/`playback_end` own the logical TTS lifecycle. Binary START/END own stream framing; `tts_result.playback_start/playback_end` are legacy informational fields and cannot advance device playback state.
  - `tts_skip` terminates only the current owned `WAIT_TTS` turn when voice broadcast is disabled. It is mutually exclusive with that turn's playback lifecycle and never interrupts physical playback.
  - The current formal service requires the runtime `device_secret` in the first hello together with the stable `device_id`; the same value is also used as `X-Bridge-Token` by this integration.
  - `credential_id`, `cloud_config_committed` and its ACK are not part of the current dynamic URL/device-secret protocol.
  - `volume_changed` reports device-UI volume commits and reconnect snapshots.
    Rapid local changes are latest-wins, while cloud `set_volume` keeps its
    existing `device_command_ack` path without generating an echo event.

- `set-wake-word-v1.md`
  - Cloud `device_command/set_wake_word` contract for replacing the MultiNet
    pinyin array at runtime (first 10 items retained), plus `reset_wake_word` for restoring
    the firmware default without clearing unrelated NVS data.

- `notification-playback-v1.md`
  - Cloud-initiated notification playback for conversation and sleeping
    states, including ACK ordering, NA audio framing, cancellation, timeout,
    retry, and mode-isolation rules.

- `device-error-report-v1.md`
  - Bounded device error event queue, compact payload, idempotent cloud ACK,
    retry and offline delivery rules.

- `offline-audio-replay-v1.md`
  - Minimal JSON begin/chunk/commit contract for replaying meeting or
    monitoring audio archived on the SD card after a WebSocket reconnect.

Legacy offline replay verification (not the current card-session test build):

```powershell
node .\NuwaxAIPartner-v2\tools\protocol\offline-audio-replay-gateway.js --self-test
node .\NuwaxAIPartner-v2\tools\protocol\offline-audio-replay-gateway.js --host 0.0.0.0 --port 8810
```

The second command starts a test-only, unauthenticated WebSocket receiver at
`ws://<LAN-IP>:8810/api/device/ws` (the path is accepted for compatibility).
It persists each chunk before acknowledging it, supports resume from the
durable `next_offset`, validates the `NWXOFF1` archive and CRC at commit, and
renames a verified upload to `.complete`. It never connects to or changes the
formal cloud service. Use `--delay-ack-ms`, `--drop-after-chunks`,
`--reject-begin`, or `--reject-commit` to exercise timeout and negative-ACK
handling; a device must retain its `.ready` file in all those failure cases.

For hardware validation where the device must keep using the formal cloud,
build with the additional defaults file:

```powershell
$env:SDKCONFIG_DEFAULTS = "sdkconfig.defaults;sdkconfig.product-ota.defaults;sdkconfig.offline-replay-test.defaults"
idf.py -B build-offline-replay-test reconfigure build
```

This profile keeps the primary cloud URL and credentials unchanged. After a
formal-cloud reconnect, a separate low-priority WebSocket sends only the
offline archive protocol to the LAN receiver. Failure closes only that test
connection and leaves `.ready` files for retry.

Production builds use the authenticated formal-cloud WebSocket for offline
replay. The product baseline enables
`NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE` and disables the LAN test switch.
The device still retains each `.ready` segment until a matching successful
commit ACK is received. The formal cloud must deploy the receiver and pass the
protocol/CRC/resume acceptance tests before enabling this product image on
fleet devices. The two replay switches remain mutually exclusive.

- `fixtures/na/manifest.json`
  - Metadata for NA binary fixtures.

- `fixtures/na/*.bin`
  - Binary NA frame samples for parser and conformance testing.

## NA Fixtures

Current generated fixtures:

- `pcm16-mono-16k-20ms-start.bin`
  - Valid single PCM16 mono NA frame.

- `pcm16-batch-3frames-16k-20ms.bin`
  - Valid WebSocket binary payload containing three consecutive uplink NA frames.

- `opus-packet-16k-60ms.bin`
  - Valid NA container with one Opus packet payload. This checks NA framing; Opus decoder availability is a separate service dependency.

- `invalid-bad-magic.bin`
  - Invalid magic, must be rejected or dropped without crashing.

- `invalid-truncated-payload.bin`
  - Declared `payload_len` exceeds available bytes, must be rejected or dropped without crashing.

- `invalid-pcm16-odd-payload.bin`
  - PCM16 payload length is odd, must be rejected or dropped without crashing.

- `downlink-stereo-reject.bin`
  - Valid NA framing but invalid for current server-to-device playback because `channels=2`. Server must not send this to the device.

Regenerate fixtures:

```powershell
node .\NuwaxAIPartner\tools\protocol\generate-na-fixtures.js
```

Validate assets:

```powershell
node .\NuwaxAIPartner\tools\protocol\validate-protocol-assets.js
```

Run fake-device conformance against a server:

```powershell
node .\NuwaxAIPartner\tools\fake-device\fake-device-conformance.js --url ws://127.0.0.1:8799/api/device/ws
```

With token:

```powershell
node .\NuwaxAIPartner\tools\fake-device\fake-device-conformance.js --url ws://127.0.0.1:8799/api/device/ws --token <token>
```

Strict playback expectation:

```powershell
node .\NuwaxAIPartner\tools\fake-device\fake-device-conformance.js --url ws://127.0.0.1:8799/api/device/ws --expect-playback
```

The fake device intentionally sends `listen stop` without `audio_session_id` and with a stop `mode` that may differ from start. A conforming server must still close the active audio session for that connection.
