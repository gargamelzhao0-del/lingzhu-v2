# NuwaxAIPartner V2

`NuwaxAIPartner-v2` is the ESP-IDF 6.0 product firmware tree for the Miaopan
V2.0 ESP32-S3 hardware. The CMake project name intentionally remains
`NuwaxAIPartner` because the OTA manifest and staged image descriptor require
that product identity.

Status date: 2026-07-30

## Source ownership

- `NuwaxAIPartner` is the formal application baseline: normal conversation,
  meeting and monitoring modes, cloud protocol, BLE/USB provisioning, LVGL UI,
  and MultiNet wake-word behavior.
- `NuwaxAIPartner测试工程` is the source for the Miaopan V2.0 board layer,
  early power hold, SW3 power behavior, BQ27220 battery service, and A/B OTA.
- This tree integrates the two sources. Never replace it wholesale with the
  test project or copy the V1.2 board layer back into it.

The detailed migration and validation record is:

`docs/2026-07-29-NuwaxAIPartner-v2迁移审计与实机验证交接.md`

## Product capabilities

- Normal conversation with PCM16 16 kHz mono cloud audio, 20 ms frames,
  wake-session/preroll handling, playback, software playback reference, Live
  AEC, and near-end voice protection.
- Meeting mode with continuous upload, adaptive speech/quiet classification,
  bounded voice gain, and a 30-minute continuous-quiet exit.
- Monitoring mode with cloud-only enable/disable and unfiltered continuous
  upload.
- BLE Security 2 and USB provisioning control with dynamic cloud binding.
- LVGL V28 round-display UI, CST816-family touch, Wi-Fi/cloud status, QR/device
  views, and BQ27220-backed battery state.
- MultiNet command family `xiao zhu xiao zhu` / `xiao zu xiao zu`, displayed as `小珠小珠`. WakeNet is not
  the selected backend.
- A/B application OTA with HTTPS validation, SHA-256, release-sequence
  anti-rollback, boot health confirmation, and rollback support.

## Hardware authority

The only source-level pin authority is
`main/board/esp_vocat_v1_0_pins.h`. The legacy file name is retained for API
compatibility; its contents target Miaopan V2.0.

| Function | Miaopan V2.0 assignment |
| --- | --- |
| Shared I2C | SCL GPIO1, SDA GPIO2 |
| Audio clocks/data | MCLK GPIO42, BCLK GPIO40, WS GPIO39, DAC DOUT GPIO41, ADC DIN GPIO38 |
| PA | GPIO4, active control for NS4150B |
| Power | PG2 hold GPIO7, SW3/PG1 GPIO16 active-low |
| LCD ST77916 QSPI | D0 GPIO13, D1 GPIO9, D2 GPIO12, D3 GPIO46, SCL GPIO14, CS GPIO10, RST GPIO21, BL GPIO3, TE GPIO17 |
| Screen touch | I2C bus plus TP_INT GPIO11 |
| Separate touch/wake net | GPIO8 |
| Green LED | GPIO18 |
| MicroSD 1-bit | CLK GPIO48, CMD GPIO45, D0 GPIO47 |
| Base UART | TX GPIO43, RX GPIO44 |
| 4G UART/control | RX GPIO5, TX GPIO6, enable GPIO15 |

Audio topology is one full-duplex I2S port: ES8311 DAC TX plus ES7210
four-slot TDM RX. Slot 0 is MIC1, slot 1 is MIC2, slot 2 is the candidate MIC3
playback-reference route, and slot 3 is NC. Slot 2 must not be used unless its
runtime health gate passes. The current sample has not passed that gate, so the
product path remains software-reference AEC.

## Build

Use ESP-IDF v6.0 PowerShell and a short external build path. The product
profile is the ordered combination of `sdkconfig.defaults` and
`sdkconfig.product-ota.defaults`.

```powershell
Set-Location "C:\Users\Administrator\Documents\work\Nuwax AI Partner\Nuwax AI Partner 7.0\NuwaxAIPartner-v2"
. "C:\esp\v6.0\esp-idf\export.ps1"

idf.py -B C:\tmp\nuwax-v2-build `
  -D "SDKCONFIG=C:\tmp\nuwax-v2-build.sdkconfig" `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.product-ota.defaults" `
  set-target esp32s3
idf.py -B C:\tmp\nuwax-v2-build menuconfig
idf.py -B C:\tmp\nuwax-v2-build build
```

Wi-Fi STA credentials and the dynamic cloud binding are provisioned data stored
in NVS. The cloud record owns the runtime URL, hardware-derived device ID, and
device secret; the legacy build-time WebSocket URL/token are not a runtime
fallback. Do not add credentials to tracked defaults, source, logs, or
documentation. An image with empty build-time credentials can update an already
provisioned device only when its NVS is preserved. It is not a standalone
factory-recovery image.

Run the host regression suites before a release build:

```powershell
.\tools\run-v2-product-host-checks.ps1
.\tools\run-conversation-host-checks.ps1
.\tools\run-meeting-host-checks.ps1
.\tools\run-monitoring-host-checks.ps1
```

## Flash and OTA boundaries

- COM5 is the current Miaopan V2.0 validation device. COM8 belongs to the
  earlier formal device and must not be used for V2 validation.
- Do not perform a serial flash or send an OTA command without explicit user
  approval. Serial flashing remains a manual recovery path.
- A complete baseline flash contains bootloader, partition table, OTA data,
  `srmodels.bin`, and the application image.
- The baseline flash must not erase or overwrite the NVS partition at `0x9000`.
  Never use `erase-flash` for an already provisioned COM5 validation device.
- Current OTA updates only an inactive application slot. It does not update the
  bootloader, partition table, model partition, NVS, or storage.
- The current source release is `7.0.17 / 7000017`. Any later release must use
  a greater version and release sequence, new request/update IDs, and an immutable
  direct HTTPS object URL.
- After a new image is confirmed and the gateway acknowledges the durable
  `committed` result, the device reconnects once so its next hello reports
  `ota_state=valid` instead of retaining the pre-confirmation snapshot.

## Current validation boundary

The current tree passes all four host suites and an ESP-IDF v6.0 ESP32-S3
product build. This is not the same as complete V2 hardware acceptance.
MIC3/slot2 hardware reference, BQ27220 profile trust, V2 formal-tree BLE
provisioning, software-AEC acoustic performance, and repeated A/B OTA behavior
still require the ordered COM5 validation recorded in the handoff document.
