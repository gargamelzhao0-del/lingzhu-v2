# BLE Wi-Fi provisioning

This directory is the ESP32-S3 Wi-Fi provisioning adapter used by firmware 7.0.
It uses the official ESP-IDF 6.0 stack:

- `espressif/network_provisioning` `1.2.4`
- `protocomm` Security 2
- NimBLE BLE transport

The old custom `FE40` GATT service was removed. The app must use the official
protocomm endpoints (`proto-ver`, `prov-session`, `prov-config`, `prov-scan`)
and the custom `device-info` / `vox-config` endpoints. `vox-config` receives
the dynamic `{deviceId, gatewayUrl, deviceSecret}` values inside the Security
2 session before Wi-Fi is applied. No SoftAP fallback is implemented.

## Rebind after cloud unbinding

The current formal-cloud connection's exact close pair `1008` / `device binding
changed`, an existing binding rejection message, or HTTP 401/403 latches a rebind
request. Other close reasons keep their normal reconnect behavior. RX only
publishes an epoch-bound request; it never writes NVS or restarts the device.

The connection owner stops new audio admission, asks the SD writer to finish
normally, and services meeting/monitoring controllers until they are disabled.
It also waits for replay ownership to end before acknowledging the handoff.
Existing capture errors remain incomplete. No upload or cloud ACK is required
to seal a local file. The configured controlled reboot into BLE is retained.

The existing durable rollback record carries the binding-required flag. Known
rejections prohibit saved-Wi-Fi success, cancellation to that pair, and the
runtime inactivity rollback. BLE remains available for the APP to deliver its
normal `vox-config` and Wi-Fi transaction. A complete replacement commit clears
the latch; application activity still validates the new cloud connection. Wi-Fi
GOT_IP and WebSocket 101 alone cannot clear a rejected binding. Manual network
changes without a known binding rejection retain their existing rollback path.

Both initial provisioning and rebind use a complete Wi-Fi/cloud transaction
journal in `nuwax_reprov/commit_pending`. After the APP candidate reaches GOT_IP
and passes owner/AP checks, the full target pair, transaction ID and CRC are
persisted before modifying the application's saved Wi-Fi or active cloud key.
The owner writes the driver/application Wi-Fi, cloud configuration and verified
marker, then reads back all of them. Only after they agree does it erase the
rollback request, erase the journal last, and publish success. A storage error
keeps the target fenced against cancellation, APP cleanup and old-Wi-Fi recovery.
The supervisor retries with bounded backoff; after its existing failure limit it
restarts with the recovery evidence retained.

On boot the journal is recovered before manager initialization or candidate
cleanup, even if the rollback request or cloud pending key is already absent.
Recovery reapplies the same pair without requiring another BLE session or a new
GOT_IP. A legacy Wi-Fi-only marker cannot prove which cloud key committed; an
incomplete/corrupt marker or rollback record therefore requires a fresh APP pair.
Healthy upgrades with no interrupted transaction keep their saved pair. No
APP/cloud message format, Security 2 configuration or SD gateway route changes.

Regression: `python tools/test-binding-reprovision.py` exercises production
functions with NVS faults/power cuts, exact close payloads and stale epochs,
plus the real meeting/monitoring controllers across the stop/seal boundary.
`python tools/test-provision-pair-transaction.py` separately compiles the actual
cloud/journal modules and production finalizer, startup recovery and cleanup.
It exercises repeated resets, individual durable key boundaries, storage errors,
legacy migration and negative mutants. Physical APP/BLE and power-cut acceptance
on the target remain necessary; host fault injection is not a hardware result.

## Frozen BLE contract

| Item | Value |
| --- | --- |
| Service UUID | `0000ffff-0000-1000-8000-00805f9b34fb` |
| Advertising name | `PROV_` + STA MAC bytes 3..5, uppercase |
| Security | protocomm Security 2 (SRP6a + AES-GCM) |
| `proto-ver.prov.cap` | `wifi_prov`, `wifi_scan` |
| `proto-ver.nuwax.cap` | `wifi_prov`, `wifi_scan`, `cloud_prov` |
| Wi-Fi milestone | `IP_EVENT_STA_GOT_IP` / `WIFI_READY` |
| Wi-Fi provisioning success | `WIFI_READY`; cloud state is reported separately |
| Focused BLE-only window | 300 seconds |
| Advertising lifetime | Continuous until the new pair commits or a saved pair without a known binding rejection recovers |
| Status wait after credentials | 30 seconds |
| BLE after success | retained for 15 seconds, then stopped by firmware |
| Transport cleanup after explicit stop | 1 second |
| SoftAP | unsupported |

The first 300 seconds are dedicated to APP provisioning. After that boundary,
BLE advertising remains active. If the saved pair has no known binding rejection
or unresolved pair transaction,
firmware periodically tries its pre-reprovision Wi-Fi configuration; a rejected
binding continues waiting for a replacement from the APP. A BLE GATT connection immediately pauses
saved-Wi-Fi recovery; a 15-second Security 2 deadline prevents an unauthenticated
client from holding that ownership forever. If the APP disconnects or an
attempt fails, BLE advertising resumes and saved-Wi-Fi recovery becomes eligible
again. First-boot devices without a saved network advertise indefinitely.

An invalid SSID or password is a failed credential attempt, not the end of the
provisioning service. The manager reports the standard failure through
`prov-config`, resets only the failed Wi-Fi state, and keeps the authenticated
BLE session available for corrected credentials. If the APP disconnects, the
uncommitted cloud candidate is discarded and advertising resumes. A new BLE
connection is rejected only during the short session-cleanup window so it cannot
cancel cleanup of the previous connection; the APP can reconnect after
advertising resumes. Repeated internal reset failures use bounded backoff and
finally restart into the persistent provisioning boot profile.

The service UUID is explicitly set in `nuwax_provision_main.c`; the component's
different built-in default UUID is not used.

## Security 2 modes

The checked-in development configuration enables
`CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE`. It uses the Kconfig PoP and generates
salt/verifier at runtime with `esp_srp_gen_salt_verifier()`. This is only for
local APP/firmware interoperability testing.

The integrated UI, Wi-Fi and audio firmware sets
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` and disables IDF 6.0 hardware SHA/AES.
Security 2 uses 3072-bit SRP6a and AES-GCM; the IDF 6.0 PSA hardware drivers
needed internal DMA contexts that were unavailable in the complete firmware.
The mbedTLS software paths keep the same algorithms and wire protocol while
allowing temporary allocations to use normal/PSRAM-backed memory.

Production must disable that option and inject these blobs into the protected
`nuwax_factory` NVS namespace:

- `sec2_salt`
- `sec2_verifier`

The production PoP is never stored in firmware. It is printed in the per-device
QR/label or make it available to an authenticated APP through a controlled
credential service together with the UID-derived username. Production screen
QRs omit plaintext PoP because the device stores only salt/verifier. The current
development QR payload is an HTTPS URL returned by
`nuwax_provision_main_qr_payload()`. Its percent-encoded `n`, `u`, and `p`
parameters contain the BLE service name, device username, and development PoP.
The development payload must not be used as a production credential.

## Firmware flow

`app_main()` initializes the existing Wi-Fi driver, then calls
`nuwax_provision_main_init()`. If the official Wi-Fi provisioning marker is
absent, it starts continuous BLE provisioning. After DHCP
success it imports the ESP-IDF STA configuration into the application's saved
network list, stops the provisioning manager, and reads the dynamic cloud
configuration from `nuwax_cloud` NVS. No compiled URL or device secret fallback
is used. A device that already has Wi-Fi but has no active
cloud configuration saves its STA rollback record and automatically opens the
same reversible BLE recovery flow.

The APP obtains `deviceSecret` from the existing bind API and obtains the
current `gatewayUrl` from its environment or a backend configuration API. The
currently observed bind response does not contain `gatewayUrl`. The secret must
contain 1..95 printable ASCII characters;
the URL must follow the validation rules frozen in the contract. It sends
`{deviceId, gatewayUrl, deviceSecret}` directly
to `vox-config` within Security 2 before sending Wi-Fi credentials. Firmware
requires `deviceId` to match the hardware UID and validates the WebSocket
URL/token. The success response retains the deployed APP contract
`{ok:true,state:"active",deviceId}`, while the candidate is internally staged
and only replaces the active NVS record after the new Wi-Fi reaches `GOT_IP`.
After Wi-Fi, the device authenticates its WebSocket Upgrade
with `X-Bridge-Token: <deviceSecret>` and identifies itself with the
hardware-derived `device_id` plus the same runtime `device_secret` in hello
version 1, as required by the current formal service. The hello serialization
buffer is cleared after transmission and is never logged. `credentialId`,
`bundleVersion` and `signature` are not part of this protocol.

Official `prov-config=connected` is the Wi-Fi success boundary. A WebSocket
failure cannot turn that result into a provisioning failure, trigger an SSID
switch or erase the received Wi-Fi configuration; the normal application loop
keeps retrying the dynamic cloud target in the background.

During BLE provisioning, WPA2/WPA3 transition APs use the WPA2 association
path by disabling the IDF WPA3 compatibility override on the received STA
configuration. This was validated against `xspaceagi` on channel 4. Pure
WPA3-only AP support is not part of the first-integration acceptance scope.

If the device menu, USB control or cloud control requests reprovisioning while
the official boot provisioning service is already active, firmware joins and
focuses that existing service. It does not create a runtime reprovision
reservation, park the cloud owner, or change the Wi-Fi manager ownership. This
keeps one authoritative provisioning transaction and prevents a boot-time
request from timing out while `app_main()` is synchronously supervising the
same BLE service.

The provisioning manager owns Wi-Fi connection attempts while BLE provisioning
is active. The regular Kconfig/fallback connection path starts only after the
manager has stopped. Subsequent boots prefer the STA configuration stored by
the official manager over build-time SSID values.

For a configured device, runtime rebind persists the verified Wi-Fi/cloud
rollback record and reboots into the clean provisioning boot profile before
starting BLE (`CONFIG_NUWAX_PROVISION_RUNTIME_REBOOT_ENABLE=y`). This avoids
reinitializing the NimBLE controller after normal audio/UI/WebSocket operation.
The older in-process rebind path remains available only when that diagnostic
option is explicitly disabled.

## APP sequence

```text
scan(service UUID/name) -> connect -> secure session(Security 2)
-> parse proto-ver(prov.cap and nuwax.cap) -> get device-info
-> vox-config(deviceId/gatewayUrl/deviceSecret) -> optional prov-scan
-> prov-config(set) -> prov-config(apply)
-> get Wi-Fi status until WIFI_READY -> disconnect BLE
-> observe cloud DEVICE_ONLINE separately when the backend exposes it
```

`prov-ctrl` is reserved for reset/reprovision control and is not used to apply
ordinary Wi-Fi credentials.

The official status endpoint is queryable after a BLE reconnect while the
service is still active. The `device-info` response also includes the last
firmware-side provisioning outcome and reason.

An unauthenticated BLE connection is closed after the Security 2 setup timeout.
After Security 2 succeeds, the APP has 300 seconds to finish the Wi-Fi
selection flow and deliver `vox-config`. Once `vox-config` is staged, Wi-Fi
provisioning must start within 60 seconds. An idle session is closed, its
uncommitted cloud candidate is discarded, and BLE remains available for the
APP to reconnect without restarting the device. An active Wi-Fi attempt keeps
its separate 30-second result window.

Android reference integration is pinned in the machine contract to Espressif
`lib-2.4.4` / `99719ae2d89b9a209cc83ca88bc6f3f4318028`. That library exposes
`vox-config` through `sendDataToCustomEndPoint`; endpoint characteristic UUIDs
are discovered from 0x2901 descriptors and must not be hardcoded. Its
`getDeviceCapabilities()` only exposes `prov.cap`, so APP code must parse raw
`getVersionInfo()` JSON to read `nuwax.cap.cloud_prov`.

## Error mapping

| APP error | Firmware source |
| --- | --- |
| `SECURITY_AUTH_FAILED` | Security 2 username/PoP credential mismatch |
| `WIFI_AUTH_FAILED` | Wi-Fi `AuthError(0)` / `NETWORK_PROV_WIFI_STA_AUTH_ERROR` |
| `NETWORK_NOT_FOUND` | Wi-Fi `WifiNetworkNotFound(1)` |
| `TIMEOUT` | No terminal result 30 seconds after credential receipt |
| `VOX_CONFIG_INVALID` | Invalid device ID, gateway URL or device secret |
| `VOX_CONFIG_PERSIST_FAILED` | Valid dynamic configuration could not be committed to NVS |
| `CLOUD_CREDENTIAL_REJECTED` | Target rejected `X-Bridge-Token` before WebSocket 101; Wi-Fi remains successful |
| `CLOUD_CONNECT_TIMEOUT` | Optional cloud observation timed out after `WIFI_READY`; Wi-Fi remains successful |
| `INTERNAL_ERROR` | Invalid security parameters, component/NVS failure, or protocomm `InternalError(5)` |
| `DHCP_FAILED` | Reserved for a future explicit custom status; official manager has no native DHCP error |

## Build and validation

The dependency is locked in `dependencies.lock`. Build with ESP-IDF 6.0:

```text
idf.py reconfigure
idf.py build
```

The dynamic `vox-config` implementation compiles with ESP-IDF `v6.0`
(`662a3be354759d9487bf4b1a629fadb766cb1800`). Host-side Bridge, QR, JavaScript
and contract checks pass. The image was flashed to the target ESP32-S3 on
2026-07-25. Device `NX-3927-2693-9288` reconnected to the local test gateway
with the saved dynamic URL and secret, and the gateway observed the device
online with normal ping/pong traffic.

The formal `wss://testagent.xspaceagi.com/api/device/ws` endpoint accepted both
the WebSocket Upgrade `X-Bridge-Token` and the same runtime value in
`hello.device_secret` on 2026-07-26. The device then received repeated
application `pong` messages. Four consecutive hard-reset cycles loaded the
active dynamic record from NVS, restored the provisioned Wi-Fi and reconnected
the formal WSS without another BLE delivery. The accepted secret is not stored
in this README, contract or simulator source. APP must always send the latest
`deviceSecret` returned by `bind` for the same `deviceId`; firmware has no
compiled fallback credential.

One `StoreProhibited` reboot was observed during the first persistence audit
reset. The device recovered automatically and the fault was not reproduced in
the next five hard resets. This does not invalidate the BLE/Wi-Fi/formal-WSS
protocol result, but longer-duration stability testing remains required before
production release.
