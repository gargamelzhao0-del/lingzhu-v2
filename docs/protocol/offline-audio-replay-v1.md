# Offline Audio Replay v1

Legacy archive-only compatibility contract. For the current card-session path, use [SD 会议监控场次上传协议](../SD会议监控场次上传协议.md) and [功能逻辑与异常处理](../SD会议监控功能逻辑与异常处理.md); the message flow below is not the current session upload flow.

This contract replays meeting or monitoring audio archived on the device SD
card after the authenticated device WebSocket reconnects. It uses the existing
WebSocket; no new transport or authentication channel is required.

## Minimal JSON messages

The device sends only the fields required to identify, resume, and verify one
archive segment. Audio metadata is carried by the `NWXOFF1` archive header and
must be parsed after the bytes have been received.

Begin:

```json
{
  "type": "offline_audio_begin",
  "request_id": "offline_begin_7a91c2d0",
  "segment_id": "offline_meeting_session_00000001",
  "total_bytes": 524288,
  "crc32": 305419896
}
```

Begin ACK:

```json
{
  "type": "offline_audio_begin_ack",
  "request_id": "offline_begin_7a91c2d0",
  "ok": true,
  "next_offset": 0
}
```

Each binary `NWXCH1` chunk is acknowledged with:

```json
{
  "type": "offline_audio_chunk_ack",
  "request_id": "offline_chunk_8b12f901",
  "ok": true,
  "next_offset": 8128
}
```

Commit:

```json
{
  "type": "offline_audio_commit",
  "request_id": "offline_commit_23aa0910",
  "segment_id": "offline_meeting_session_00000001",
  "total_bytes": 524288,
  "crc32": 305419896
}
```

Commit ACK:

```json
{
  "type": "offline_audio_commit_ack",
  "request_id": "offline_commit_23aa0910",
  "ok": true
}
```

`request_id`, `segment_id`, `total_bytes`, `crc32`, and `next_offset` are the
reliability fields and must not be removed. The cloud must use
`device_id + segment_id` as the idempotency key because request IDs change
when a device reconnects. `segment_id` is a stable compact identifier derived
from the complete on-device filename; its internal text format is opaque and
must not be parsed by the cloud.

## Binary and archive rules

The binary format remains unchanged: a 64-byte `NWXCH1` header followed by at
most 8064 bytes of the original file. The uploaded file is a complete
`nuwax_pcm16_archive_v1` (`NWXOFF1` header, NA v1 PCM16 records, and `NWXEND1`
footer), not a stream of bare PCM samples.

The cloud must validate the offset and length of every chunk, persist bytes
before acknowledging them, and return `next_offset` as the first byte that is
not yet persisted. It may return `next_offset` greater than zero in begin ACK
to resume an earlier attempt.

The device deletes the local `.ready` file only after a matching
`offline_audio_commit_ack` with `ok:true`. Any timeout, disconnect, negative
ACK, integrity failure, or local delete failure leaves the file for a later
retry. The cloud may discard its own corrupt `.part` object after an integrity
failure and return `ok:false`; this does not authorize the device to delete its
`.ready` file. A subsequent `begin` must then return `next_offset:0`.
