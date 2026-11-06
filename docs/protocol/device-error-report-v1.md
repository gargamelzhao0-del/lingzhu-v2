# Device Error Report Protocol v1

Status: device implementation contract, 2026-08-21.

## Purpose

The device reports selected, actionable hardware and transport failures and
recoverable warnings. This is an event stream, not a copy of the ESP log. The
queue is bounded and the device never blocks audio or UI work while waiting
for the cloud.

## Device message

Required fields are `type`, `event_id`, `severity`, `component`, and `code`.
`severity` is normally `error` or `warning`; warning events are emitted only
for actionable recoverable conditions, not for every local `ESP_LOGW` line.
`result` and `uptime_ms` are recommended when an error has an ESP-IDF result.
`result` remains one string field in the same envelope and is bounded to 159
characters. It starts with the ESP-IDF error name when available and may
append compact `key=value` context separated by semicolons.
`state`, `generation`, and `turn_id` are optional context and should only be
sent when the failure is bound to a conversation/playback generation.
Warning events use a longer local same-code dedup window (60 seconds) than
errors (3 seconds), so periodic health diagnostics cannot fill the offline
queue while a real fault remains observable.

```json
{
  "type": "device_error_report",
  "event_id": "e_7a91c2d0_000001",
  "severity": "error",
  "component": "speaker",
  "code": "pcm_progress_stall",
  "result": "ESP_ERR_TIMEOUT;w=183;q=5;s=359",
  "uptime_ms": 382259,
  "state": "speaking",
  "generation": 8,
  "turn_id": "turn_xxx"
}
```

### Fields intentionally omitted

- `boot_id`: `event_id` already contains a boot-scoped random nonce and a
  monotonic sequence.
- `ws_epoch`: internal WebSocket implementation detail; the cloud only needs
  the event ID and connection-level ordering.
- `metrics`: heap and task statistics are not useful for every error and make
  the message larger. Add a bounded, versioned `metrics` object only for a
  specifically defined memory/watchdog event.
- Free-form `message`: avoid unstable parsing and accidental sensitive data.

All text fields are bounded ASCII/UTF-8 buffers on the device. Unknown fields
must be ignored for forward compatibility.

## ACK

The cloud must acknowledge each event by ID. ACKs are idempotent; a duplicate
ACK for the same ID is harmless.

```json
{
  "type": "device_error_report_ack",
  "event_id": "e_7a91c2d0_000001",
  "accepted": true
}
```

`accepted=false` means the cloud rejected the event and the device may remove
it from its bounded queue. If no ACK arrives, the device retries up to three
times with a bounded timeout and then drops the event with a local warning.
Events queued while offline remain pending until a later authenticated
connection. The device also keeps a CRC-protected bounded queue snapshot in
NVS, so events that were already committed to the snapshot survive a normal
restart. Snapshot writes are coalesced for up to one second during bursts; a
sudden power loss can therefore lose an event queued after the last snapshot
commit (including an event from that coalescing window).

The queue is intentionally bounded. When it is full and the oldest event is
being transmitted, a new event may be dropped rather than blocking audio,
microphone, or UI work.

## Cloud requirements

Persist the last-seen `event_id` per device for idempotency, accept retries on
the same or a new connection, and return the ACK after durable admission. The
cloud should index `component`, `code`, `severity`, and occurrence time; it
should not require `ws_epoch`, heap metrics, or a boot ID to deduplicate.
