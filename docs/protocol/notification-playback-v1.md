# Notification Playback Protocol v1

Status: device implementation contract, 2026-08-07.

## Scope

This protocol lets the cloud play an unsolicited notification through the
normal device speaker pipeline. Notifications are accepted only in normal
conversation or sleeping mode. Meeting, monitoring, OTA, and runtime BLE
provisioning cannot start while a notification transaction is active.

Every notification is bound to one WebSocket connection epoch. A reconnect
invalidates the transaction; messages from the old connection cannot complete
or extend a transaction on the new connection.

## Identifiers

All identifiers are non-empty ASCII strings using the device protocol ID
character set.

| Field | Maximum payload length |
| --- | --- |
| `request_id` | 39 bytes |
| `notify_id` | 47 bytes |
| `playback_id` | 47 bytes |

Retries on the same connection must reuse the same IDs and the same payload.
The device rejects a duplicate begin/end message when its audio totals or end
reason differ from the first accepted message.

## Conversation Flow

The cloud first stops the current conversation playback and waits for its ACK:

```text
stop_playback -> device_command_ack
notify_playback_begin -> notify_playback_begin_ack
NA PCM16 audio
notify_playback_end -> notify_playback_end_ack
device enters LISTENING
```

`notify_playback_begin` is not acknowledged successfully until the prior
speaker generation, microphone capture, and playback wake detector have
physically released their owners.

## Sleeping Flow

The cloud prepares the sleeping device and waits for the mandatory command
ACK before beginning audio:

```text
prepare_notify_playback -> device_command_ack
notify_playback_begin -> notify_playback_begin_ack
NA PCM16 audio
notify_playback_end -> notify_playback_end_ack
device creates a new awake session and enters LISTENING
```

Prepare command:

```json
{
  "type": "device_command",
  "command": "prepare_notify_playback",
  "request_id": "cmd_001",
  "notify_id": "ntf_001",
  "text": "optional summary"
}
```

Successful prepare ACK:

```json
{
  "type": "device_command_ack",
  "request_id": "cmd_001",
  "command": "prepare_notify_playback",
  "ok": true,
  "message": "ready"
}
```

## Begin, Audio, and End

```json
{
  "type": "notify_playback_begin",
  "request_id": "req_begin_001",
  "notify_id": "ntf_001",
  "playback_id": "pb_001"
}
```

Optional begin fields are `codec`, `sample_rate`, `channels`,
`frame_duration_ms`, `frames`, and `bytes`. When present, the only supported
format is `pcm16`, 16000 Hz, mono. Binary frames use the negotiated hello NA
container and the existing START/PCM/END sequence rules.

The device returns `notify_playback_begin_ack` with matching IDs. `ok:true`
means the binary stream is admitted. Waiting for this ACK is recommended and
provides explicit rejection handling. For compatibility with a sender that
streams immediately after the ordered `notify_playback_begin` frame, the
device applies RX backpressure until its application owner commits or rejects
BEGIN; such audio can no longer overtake the queued text control event.

```json
{
  "type": "notify_playback_end",
  "request_id": "req_end_001",
  "notify_id": "ntf_001",
  "playback_id": "pb_001",
  "frames": 123,
  "bytes": 78720,
  "reason": "completed"
}
```

The cloud sends binary END before the JSON end. The device tolerates their
callbacks being processed in either order after binary END has been admitted.
It sends `notify_playback_end_ack` with `ok:true` only after all three terminal
conditions are true: binary END accepted, logical end accepted, and the bound
speaker generation physically stopped. It then enters `LISTENING`.

## Cancellation and Timeouts

Head touch and wake-word interruption are allowed. The device stops the bound
speaker generation, releases capture owners, and sends:

```json
{
  "type": "notify_playback_cancelled",
  "request_id": "notify_cancel_1",
  "notify_id": "ntf_001",
  "playback_id": "pb_001",
  "reason": "wake_word_interrupt"
}
```

The notification stream uses a 10-second sliding idle timeout. Each accepted
ingress progress edge refreshes it. If no audio progress or terminal END is
received for 10 seconds, the device cancels the notification and returns to a
valid local state. Prepare, begin, cancel, and terminal barriers are bounded;
no path force-deletes an audio task.

On a WebSocket disconnect the device drops the transaction, resets speaker
runtime at the connection boundary, and does not send an old ACK on a new
connection.
