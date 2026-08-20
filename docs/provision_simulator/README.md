# BLE Wi-Fi provisioning integration client

## Device-side Wi-Fi scan

In Real mode, lock the target BLE device and provide the Security 2 identity. The simulator automatically starts the first scan after a 1.2-second input debounce. After each completed scan it waits 15 seconds, then performs the next scan. The manual **扫描设备 Wi-Fi** button remains available for an immediate refresh.

Each refresh establishes Security 2, verifies `wifi_scan` and `device-info.serialNumber`, then calls the managed component's official `prov-scan` implementation. `prov-scan` is request/response: the APP or simulator requests each scan and the ESP32-S3 performs it; the firmware does not use a private endpoint or unsolicited BLE push.

The list below the SSID/password inputs contains the networks visible to the ESP32-S3, deduplicated by SSID and ordered by strongest RSSI. The firmware keeps at most 32 strongest scan entries. An automatic refresh preserves the selected SSID, typed password and input focus; only a user click on a different row fills the SSID, clears a stale password and focuses the password input. BSSIDs, PoP and Wi-Fi passwords are never returned in the scan response or written to exported records.

The password field has an explicit **显示/隐藏** control. It only changes the browser input display type; it does not log, persist or return the password from the bridge.

Wi-Fi scanning and provisioning share one BLE operation lock and never execute concurrently. After a password is entered, the next automatic refresh is cancelled so the Start action remains stable. If Start is clicked during an already-issued scan, the click is accepted once, future scans are cancelled, the current official `prov-scan` request is allowed to complete and release BLE, and provisioning then starts automatically. Scanning remains stopped after either success or failure; the user may use the manual refresh button or Reset before another attempt.

## Real-device result semantics

The bridge preserves the official `prov-config` protobuf failure reason instead of relying on the reduced status string returned by `esp_prov.get_wifi_config()`:

- Wi-Fi authentication failure: `WIFI_AUTH_FAILED`
- AP/SSID not found: `NETWORK_NOT_FOUND`
- Unknown raw reason or local protocol error: `INTERNAL_ERROR`
- Official `connected` (`IP_EVENT_STA_GOT_IP`): successful `WIFI_READY`

The pre-credential `device-info.serialNumber` identity check is mandatory. On failure the bridge reads `device-info` again so the firmware extension can expose the raw `WIFI_EVENT_STA_DISCONNECTED` number/name that the official two-value failure enum cannot represent.

This browser application has two modes:

- `Mock`: contract-driven APP state-machine and error simulation.
- `Real`: local browser UI backed by Espressif's official `esp_prov` BLE,
  protobuf, protocomm Security 2 and network provisioning implementation.

The bridge imports the tooling from the exact managed
`espressif/network_provisioning` component used by the firmware build. It does
not implement or enable a SoftAP fallback.

## Start

From PowerShell:

When this simulator is used from the standalone delivery ZIP, install
ESP-IDF `v6.0` and set `IDF_PATH` before the first start. The packaged
`network_provisioning` client is self-contained, while its Security 2
`protocomm` Python module comes from ESP-IDF:

```powershell
$env:IDF_PATH = "C:\esp\v6.0\esp-idf"
```

The host also requires Windows BLE, Python 3.10 or newer and Node.js 20 or
newer. The start scripts install the small Python requirements when missing;
they do not install ESP-IDF or Node.js.

For the real Wi-Fi + cloud onboarding flow:

```powershell
cd docs/provision_simulator
.\start-full-stack.ps1
```

If the default simulator port is occupied, run
`.\start-full-stack.ps1 -SimulatorPort 8865`. For Bridge-only mode use
`.\start-bridge.ps1 -Port 8865`.

`start-bridge.ps1` starts only the loopback BLE bridge and is enough to test
formal-service configuration delivery plus Wi-Fi. `start-full-stack.ps1`
additionally starts the local gateway, securely prompts for the runtime Device
Secret, and allows the simulator to observe `DEVICE_ONLINE`. The bundled local
gateway uses bidirectional mock ASR, server-side sentence endpointing, automatic
chat relay and mock TTS playback so the device receives ASR progress before its
15-second watchdog and can exercise the complete continuous-conversation loop.
Its local speech threshold is tuned to RMS 80 for the current Miaopan microphone
capture; production deployments must calibrate endpointing against their own
microphone gain and ambient-noise distribution.

Open:

```text
http://127.0.0.1:8765/simulator/
```

When a custom port is selected, replace `8765` with that value.

The bridge intentionally binds only to loopback. Device Secret, PoP and Wi-Fi passwords are
kept in operation memory, are not placed in job responses, and are not exported
in test traces. Static HTTP serving is restricted to this simulator directory;
the bridge does not serve repository `sdkconfig`, source, build or runtime-log
files. Device discovery, device Wi-Fi scan and provisioning share one backend
BLE operation lock.

The bridge also validates the loopback `Host`, same-origin `Origin` and
`application/json` request type before any BLE POST operation. Device discovery
uses POST rather than a side-effecting GET. A 90-second operation guard prevents
an abnormal BLE call from holding the shared lock indefinitely.

## QR-directed real provisioning

The preferred real-device path mirrors the Android Provider flow:

1. Select `Real ESP32-S3` and click `Scan device QR`.
2. Scan the QR shown at `Device > Device QR`, or select a QR image.
3. The client validates Nuwax `v1`, BLE, Wi-Fi, Security 2, the `PROV_` name
   and username. A development PoP is accepted from the QR; an identity-only
   production QR leaves the PoP field editable for the controlled factory
   credential handoff.
4. The bridge scans only advertisements containing the frozen provisioning
   Service UUID. The client accepts only the exact device name from the QR and
   never substitutes another nearby `PROV_` device.
5. The Wi-Fi input opens immediately. In parallel, the client waits up to 30
   seconds for the exact device, allowing time to leave the QR screen, tap the
   device `BLE` action and let the provisioned device reboot.
6. Once the target is locked, the device Wi-Fi list refreshes automatically.
   Enter the dynamic Gateway URL from the APP environment/backend configuration
   and the latest Device Secret returned by `bind`, select an SSID, enter its
   password, then start provisioning. The currently observed `bind` response
   does not contain `gatewayUrl`.
7. After Security 2 succeeds, the bridge requires
   `device-info.serialNumber == QR username` before sending Wi-Fi credentials.

Native `BarcodeDetector` is used when available. The bundled Apache-2.0
`jsQR 1.4.0` decoder is the offline camera/image fallback; the simulator does
not require a CDN at runtime. Raw QR payloads are not logged or exported.

The current development QR is JSON with these fields:

```json
{
  "ver": "v1",
  "name": "PROV_A1B2C3",
  "transport": "ble",
  "network": "wifi",
  "security": 2,
  "username": "NX-A1B2-C3D4-E5F6",
  "pop": "development-only PoP"
}
```

Production screen QR intentionally omits plaintext PoP because production
firmware stores only SRP salt/verifier. The simulator accepts this identity-only
QR, locks the exact device, and leaves PoP editable so the APP can obtain it
from the controlled manufacturing label/credential handoff. A screen-only QR
without that separate PoP source cannot complete Security 2 provisioning.
The supplied PoP is accepted for an identity-only QR; equality is required only
when the QR itself contains a PoP.

## Dynamic cloud configuration path

After Security 2 and `device-info` identity verification, the bridge sends this
UTF-8 JSON to the encrypted `vox-config` endpoint before calling official
`prov-config` set/apply:

```json
{
  "deviceId": "NX-A1B2-C3D4-E5F6",
  "gatewayUrl": "wss://testagent.xspaceagi.com/api/device/ws",
  "deviceSecret": "runtime value from bind"
}
```

The device validates the hardware identity, URL and token, then atomically
stores the active URL/secret pair. It connects with
`X-Bridge-Token: <deviceSecret>` and announces the stable hardware UID plus
the same runtime `device_secret` in hello version 1, as required by the current
formal service. `credentialId`, `bundleVersion` and `signature` are intentionally
absent because the current formal service does not issue or validate them.
The local test gateway publishes the same active protocol through `/api/status`:
`x-bridge-token+hello.device_secret`, `vox-config-url-device-secret-v1`, and
`signed_bundle_required=false`. `start-full-stack.ps1` refuses to start the
simulator when these values do not match, so an old signed-bundle gateway cannot
silently produce a false-positive integration result.

The simulator does not hold an APP login token and does not call the formal
`bind` API. It validates the post-bind path: Security 2 delivery, firmware
persistence, Wi-Fi provisioning and the device's WSS authentication. The
Android APP must still be accepted end to end for
`bind -> vox-config -> Wi-Fi -> formal backend online`.

The simulator reports these states independently:

- `WIFI_READY`: official `prov-config=connected` / `IP_EVENT_STA_GOT_IP`; this
  is the provisioning success result.
- `DEVICE_ONLINE`: optional local-test-gateway observation after Wi-Fi success.
- `NOT_OBSERVED` or `CLOUD_CONNECT_TIMEOUT`: cloud status is unavailable or
  timed out; it is displayed as a warning and cannot change `WIFI_READY`.

PoP remains required. It protects the BLE Security 2 session carrying the
Wi-Fi password and cloud credential; it is not replaced by `device_secret`,
which authenticates the device to the cloud.

## Post-provisioning BLE and test-service controls

The device does not send a private "provisioning complete" notification. The
APP sends encrypted `vox-config`, receives `{ok:true,state:"active",deviceId}`
without a secret echo, then sends and applies the official `prov-config` Wi-Fi
credentials. The APP polls `prov-config`; official `connected`, backed by
`IP_EVENT_STA_GOT_IP`, is the authoritative `WIFI_READY` result.

After `WIFI_READY`, firmware keeps the provisioning service available for 15
seconds so the APP can read the final status. It then calls
`network_prov_mgr_stop_provisioning()` and `network_prov_mgr_deinit()`. BLE
advertising, provisioning GATT characteristics and the active GATT connection
therefore end. The APP should close/dispose its BLE Provider transport after it
reads the final result and must not use that connection for later device
control.

The simulator's **测试服务设备控制** panel uses the local test gateway after
BLE has ended. It is enabled only when the entered `gatewayUrl` exactly matches
the active `NUWAX_TEST_GATEWAY_WS_URL` and the exact `deviceId` is online.
Formal service URLs are rejected both in the browser and by the loopback
Bridge. No gateway token or device secret is exposed to the page.

The meeting and monitoring switches reuse the existing device protocol without
adding or changing any meeting/monitoring fields:

```json
{"type":"device_command","request_id":"sim_meeting_open_<unique>","command":"set_audio_mode","mode":"meeting","state":"enabled","interrupt_current_turn":true}
```

Monitoring uses `mode:"monitoring"`; disabling either mode uses
`state:"disabled"`. `interrupt_current_turn:true` remains meeting-enable-only,
as defined by the existing protocol. The two modes are exclusive: the active
mode must receive its matching `device_command_ack` and close before the other
switch is enabled. A switch changes to its final state only after the Bridge
observes the same `request_id`, `command:"set_audio_mode"`, an accepted ACK
message (`active`, `already_active`, `disabled`, or `already_disabled`), and the
corresponding test-gateway audio-session state. Device capability flags
`meeting_mode_v1` and `monitoring_mode_v1` are also enforced.

Volume control is enabled only when no meeting or monitoring audio session is
active. The Bridge sends:

```json
{"type":"device_command","request_id":"volume_<unique>","command":"set_volume","volume_percent":52,"persist":false}
```

Success is reported only after a matching device ACK is observed:

```json
{"type":"device_command_ack","request_id":"volume_<same>","command":"set_volume","ok":true,"message":"volume=52 persist=0"}
```

The `persist` option writes the selected value as the device default; leave it
off for a runtime-only test. Commands are blocked during meeting/monitoring
because those exclusive audio loops do not consume the normal session command
queue.

## Validation status

The revised `vox-config` firmware compiles successfully and the Bridge, QR,
JavaScript and contract checks pass. It was flashed to the target ESP32-S3 on
2026-07-25, and the saved dynamic configuration reconnected device
`NX-3927-2693-9288` to the local test gateway with normal ping/pong traffic.

The formal WSS service accepted the dynamic `X-Bridge-Token` and matching
`hello.device_secret` for device `NX-3927-2693-9288` on 2026-07-26. The device
received repeated application `pong` messages. Four consecutive hard resets
then loaded the same active NVS record, restored `xspaceagi` and reconnected
`wss://testagent.xspaceagi.com/api/device/ws` without BLE redelivery. The
accepted secret is deliberately absent from source, documentation and exported
traces.

For a formal-service target the simulator reports cloud state as
`NOT_OBSERVED`, because its optional online query is intentionally restricted
to the local test gateway. This is not a cloud failure; formal-cloud acceptance
must be verified by a backend device-status API or device/gateway logs.

The firmware uses the WPA2 association path for WPA2/WPA3 transition APs while BLE provisioning is active. Pure WPA3-only AP behavior is not part of this first-integration validation. After GOT_IP, the official status remains queryable for 15 seconds and transport cleanup takes about 1 additional second. The browser records `WIFI_READY` immediately and reports cloud observation separately; it never waits for the device screen icon.

## Real-device preparation

An unprovisioned device advertises automatically. For a provisioned device:

1. Open the device `Device` detail page.
2. Tap `Device QR` and scan it with the simulator first.
3. Return to the `Device` detail page.
4. Tap the visible Wi-Fi-icon **BLE** button below the `Meeting` switch.
5. Wait for the device to save its current STA configuration for rollback and restart. The
   simulator waits for and locks the exact QR-selected `PROV_` device.
6. Enter Gateway URL and Device Secret, select a device-scanned SSID, enter its
   password, optionally use the **显示** buttons, then start provisioning.

Manual BLE scan and Username/PoP input remain available as a diagnostic fallback.

## Re-enter BLE provisioning from the simulator

Real mode exposes **让设备进入配网模式** after a QR target or manually selected
BLE identity is available. The operation uses an in-page two-click confirmation
and sends `{deviceId, serviceName}` only to the loopback Bridge.

For local hardware integration, the preferred path is the ESP32-S3 native USB
Serial/JTAG port. The Bridge sends the exact command
`NUWAX_CTRL start_ble_provisioning`; the integration firmware acknowledges it,
queues the same reversible worker used by the physical Device-menu action,
saves the previous Wi-Fi configuration, and reboots. The button is not reported
successful until the Bridge observes the exact `serviceName` together with the
frozen provisioning Service UUID.

If no usable ESP USB serial port is present and the device is still connected
to the bundled local test gateway, the Bridge may instead forward authenticated
WebSocket command `start_ble_provisioning`. A gateway `command_forwarded` value
is internal progress only. The public `/api/reprovision` response still requires
the exact BLE advertisement and returns `ble_advertisement_confirmed`.

USB control is a physical-host integration helper and must be disabled in a
production release build. It is not the APP production path and does not grant
an APP access to the device WebSocket. A production remote trigger requires a backend API
that authenticates the APP user, verifies ownership of `deviceId`, applies
rate/audit controls, and only then forwards the same device command. The local
screen action remains the offline recovery path.

## Checks

```powershell
node --test qr-codec.test.cjs
python -m py_compile bridge.py
python -m unittest bridge_logic_test.py
```

Current regression result: 16 QR tests and 36 Bridge logic tests pass;
JavaScript and Python syntax checks pass.

Real mode validates the host BLE transport and provisioning protocol. Android
Provider testing remains the release acceptance path.
