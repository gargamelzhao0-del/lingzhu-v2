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
- Missing signed cloud precondition: `CLOUD_CONFIG_REQUIRED`
- Unknown raw reason or local protocol error: `INTERNAL_ERROR`
- Official `connected` (`IP_EVENT_STA_GOT_IP`): `WIFI_READY`, not final success

The pre-credential `device-info.serialNumber` identity check is mandatory. On failure the bridge reads `device-info` again so the firmware extension can expose the raw `WIFI_EVENT_STA_DISCONNECTED` number/name and cloud precondition result that the official two-value failure enum cannot represent.

This browser application has two modes:

- `Mock`: contract-driven APP state-machine and error simulation.
- `Real`: local browser UI backed by Espressif's official `esp_prov` BLE,
  protobuf, protocomm Security 2 and network provisioning implementation.

The bridge imports the tooling from the exact managed
`espressif/network_provisioning` component used by the firmware build. It does
not implement or enable a SoftAP fallback.

## Start

From PowerShell:

For the real Wi-Fi + cloud onboarding flow:

```powershell
cd docs/provision_simulator
.\start-full-stack.ps1
```

`start-bridge.ps1` starts only the loopback BLE bridge. Real mode requires the
test gateway variables configured by `start-full-stack.ps1`.

Open:

```text
http://127.0.0.1:8765/simulator/
```

The bridge intentionally binds only to loopback. PoP and Wi-Fi passwords are
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
   Select an SSID, enter its password, then start provisioning.
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

## Cloud onboarding path

After Security 2 and `device-info` identity verification, the bridge calls the
authenticated test claim API and receives a signed bundle containing the exact
device ID, runtime WebSocket URL, credential ID and per-device secret. It sends
that bundle through the encrypted `cloud-config` endpoint before calling
official `prov-config` set/apply. The device stores it as NVS `pending`, uses
per-device headers during the WebSocket Upgrade and promotes it to `active`
only after an authenticated hello response. It then sends
`cloud_config_committed`; the gateway does not expose final online success
before accepting that durable-commit acknowledgement.

The simulator reports success only after both milestones:

- `WIFI_READY`: official `prov-config=connected` / `IP_EVENT_STA_GOT_IP`.
- `CLOUD_CONFIG_COMMIT_PENDING`: authentication passed, but the matching
  device NVS commit acknowledgement has not arrived; continue waiting.
- `DEVICE_ONLINE`: the gateway observed the matching authenticated credential
  and accepted `cloud_config_committed`.

PoP remains required. It protects the BLE Security 2 session carrying the
Wi-Fi password and cloud credential; it is not replaced by `device_secret`,
which authenticates the device to the cloud.

## Verified real-device path

The previous 2026-07-24 cloud-onboarding firmware is flashed on device `NX-3927-2693-9288` / `PROV_1467C4`. Contract 1.2 source adds commit acknowledgement, raw Wi-Fi diagnostics and reversible rebind and has passed compilation, but it has not been flashed without the user's explicit confirmation. The previous image verified QR-directed selection, Service UUID discovery, GATT/MTU 517, Security 2, encrypted `device-info`, official `prov-scan`, identity match, authenticated test-gateway claim, signed `cloud-config` staging, and official `prov-config` set/apply.

The final firmware completed one real-device three-end run with this simulator and the test gateway on 2026-07-25. The same operation reached `WIFI_READY` at 14.3 s and `DEVICE_ONLINE` at 33.6 s; the gateway confirmed the credential active with no pending slot. This proves the local reference Provider path, not Android Provider interoperability, repeated-run stability, or production readiness.

The firmware uses the WPA2 association path for WPA2/WPA3 transition APs while BLE provisioning is active. Pure WPA3-only AP behavior is not part of this first-integration validation. After GOT_IP, the official status remains queryable for 15 seconds and transport cleanup takes about 1 additional second. The browser records `WIFI_READY` immediately, then waits for cloud `DEVICE_ONLINE`; it never waits for the device screen icon.

## Real-device preparation

An unprovisioned device advertises automatically. For a provisioned device:

1. Open the device `Device` detail page.
2. Tap `Device QR` and scan it with the simulator first.
3. Return to the `Device` detail page.
4. Tap the visible Wi-Fi-icon **BLE** button below the `Meeting` switch.
5. Wait for the device to save its current STA configuration for rollback and restart. The
   simulator waits for and locks the exact QR-selected `PROV_` device.
6. Select a device-scanned SSID, enter its password, optionally use the
   **显示** button to verify it, then start provisioning.

Manual BLE scan and Username/PoP input remain available as a diagnostic fallback.

## Checks

```powershell
node --test qr-codec.test.cjs
python -m py_compile bridge.py
python -m unittest bridge_logic_test.py
```

Current regression result for each simulator copy: 16 QR tests and 13 Bridge logic tests pass; JavaScript and Python syntax checks pass.

Real mode validates the host BLE transport and provisioning protocol. Android
Provider testing remains the release acceptance path.
