# Nuwax OTA core

ESP-IDF 6.0 HTTPS application OTA implementation. Product builds enable this
directory with `CONFIG_NUWAX_OTA_ENABLE` and wire it into the WebSocket
dispatcher through `nuwax_ota_device`.

## OTA v1 manifest

All fields below are required except `force` and `release_notes`:

```json
{
  "update_id": "ota_7100000_001",
  "url": "https://firmware.example.com/NuwaxAIPartner-7.1.0.bin",
  "version": "7.1.0",
  "project_name": "NuwaxAIPartner",
  "target_board": "miaopan_v2_0_n32r8",
  "target_chip": "esp32s3",
  "partition_schema": 1,
  "build_flavor": "product",
  "release_sequence": 7100000,
  "size_bytes": 2007552,
  "image_sha256": "64-lowercase-hex-characters",
  "force": false,
  "release_notes": "optional text"
}
```

Legacy `size` and `sha256` aliases are deliberately not accepted. `force` may
allow an equal/lower display version, but it never bypasses the strictly
increasing `release_sequence` rule.

## Security and integrity gates

1. The target board/chip/schema/flavor/project must match the compiled device.
2. Non-forced semantic version updates must be newer than the running image.
3. The persisted release sequence must increase, even for forced updates.
4. TLS uses `esp_crt_bundle_attach`; certificate and hostname checks stay on.
5. Redirects are rejected. The response must be HTTP 200 with an exact
   `Content-Length` equal to `size_bytes`.
6. Every response byte is included in mandatory SHA-256 and written through
   `esp_ota_write`; received byte count and `image_sha256` must both match.
7. `esp_ota_end` validates image format, chip and revision. The staged app
   descriptor project/version must match the manifest.
8. Only then does `esp_ota_set_boot_partition` select the new image.

## Integration lifecycle

```text
nuwax_ota_main_register(callbacks)
nuwax_ota_main_init()
nuwax_ota_main_start(manifest_json)       // validates, persists and arms
nuwax_ota_main_run_transport()            // synchronous on an internal-RAM task
nuwax_ota_main_tick(now_ms)               // timeout/dwell accounting when idle
nuwax_ota_main_reboot_staged_image()      // only after DONE is acknowledged
```

The product integration runs transport on its controller task. That task is
created during startup with enough internal-RAM stack for HTTPS and flash
writes, so accepting an update never depends on allocating a large contiguous
worker stack from the fragmented runtime heap. Transport performs its own
periodic timeout accounting while the controller is synchronously occupied.

The transport reserves the configured internal-DRAM I/O buffer before creating
the HTTP client. The product profile requests 4 KiB and falls back to 2 KiB
when that contiguous internal block is unavailable. Do not replace this with
an unconstrained `malloc()`: on a PSRAM-enabled build it may fall back to
external RAM, forcing the flash driver to copy and write in small
cache-suspending fragments. HTTPS open, header and body transport failures may
restart the download from byte zero up to three total attempts. A partial
attempt is always closed, SHA state is discarded and the OTA handle is aborted
before retrying. Cancellation and the configured total deadline remain
authoritative across all attempts; the product OTA profile currently uses a
35-minute deadline.

Before closing a failed HTTP client, the controller records the received byte
offset, system/socket errno, ESP-TLS aggregate error, raw TLS code and flags,
stack watermark and heap state. The compact error tuple is also returned in the
terminal OTA result so field failures remain diagnosable without a serial log.

Cancellation is cooperative during DOWNLOADING and WRITING. VERIFYING is a
short non-cancellable commit phase. Failures abort the active OTA handle and
never select the staged partition.

On the first boot of the new image, run product health checks and then call:

```text
nuwax_ota_main_running_image_pending_verify(&pending)
nuwax_ota_main_confirm_running_image()
```

or, on failed health checks:

```text
nuwax_ota_main_rollback_running_image_and_reboot()
```

## Required project configuration

```text
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

The partition table must contain `otadata` plus exactly two OTA application
slots for partition schema 1. The component dependencies are `app_update`,
`esp_http_client`, `esp-tls`, `mbedtls`, `nvs_flash`, `cjson`, and `esp_timer`.
