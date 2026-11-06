# Segment format v1

The prototype stores one monitoring segment as a sequence of fixed metadata,
length-prefixed binary frames, and a commit footer. The binary frame bytes are
not decoded or rewritten, so an eventual uploader can preserve NA-v1 frames.

All integer fields are little-endian.

```text
header (136 bytes)
  0..7    magic "NWA-SEG1"
  8..9    version = 1
 10..11   header_bytes = 136
 12..19   start_unix_ms
 20..23   sample_rate
 24..25   channels
 26..27   codec (1=PCM16, 2=Opus by current NA convention)
 28..29   frame_duration_ms
 30..33   segment_index
 34..81   archive_id (NUL padded, max 47 bytes)
 82..129  audio_session_id (NUL padded, max 47 bytes)
130..135  reserved

repeated frame records
  0..3    frame_bytes
  4..     exact binary frame payload

footer (32 bytes)
  0..7    magic "NWA-END1"
  8..9    version = 1
 10..11   footer_bytes = 32
 12..15   frame_count
 16..23   payload_bytes
 24..27   CRC32 of concatenated frame payloads
 28       complete (1 normal close, 0 recovered/truncated boundary)
 29..31   reserved
```

Files ending in `.open` are never upload candidates. Commit performs a flush,
platform file sync, close, and rename to `.ready`. Recovery validates the
header and records, truncates a partial length or payload record, appends a
footer with `complete=0`, and then promotes the file to `.ready`. A valid
footer already present in an `.open` file is validated and promoted without
appending a duplicate footer. Malformed archive files are moved to the
`quarantine/` directory so they cannot block other segments. The cloud
uploader must transmit the complete flag and must not delete a `.ready` file
until the server acknowledges the archive and segment idempotently.
