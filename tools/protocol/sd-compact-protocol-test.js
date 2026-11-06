"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { ReplayReceiver, archiveCrc, makeChunk } = require("./offline-audio-replay-gateway");
const { fixture } = require("./sd-audio-sessions-test");
const dir = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-compact-"));
try {
  const device = "COMPACT-HOST", data = fixture("meeting_compact", 0, false);
  const base = { type: "meeting_audio_begin", request_id: "begin", segment_id: "compact_part",
    audio_session_id: "meeting_compact", segment_index: 0, total_bytes: data.length,
    crc32: archiveCrc(data).crc, capture_start_ms: 0, capture_end_ms: 960 };
  let receiver = new ReplayReceiver(dir);
  for (const [fields, window] of [
    [{}, 0], [{ replay_version: 2, replay_revision: 1, window_bytes: 524288 }, 0],
    [{ replay_version: 2, window_bytes: 524288 }, 0],
    [{ replay_revision: 2, window_bytes: 524288 }, 0],
    [{ replay_version: null, window_bytes: 524288 }, 0],
    [{ replay_version: 2, replay_revision: 2, window_bytes: 524288 }, 524288],
    [{ window_bytes: 524288 }, 524288],
  ]) {
    const ack = receiver.begin(device, { ...base, ...fields });
    assert.equal(ack.window_bytes || 0, window);
    assert.equal(ack.replay_version, fields.replay_version === 2 && window ? 2 : undefined);
  }
  for (const window_bytes of [1, 8063, -1, 524289, 8192.5, null])
    assert.throws(() => receiver.begin(device, { ...base, window_bytes }));
  assert.equal(receiver.begin(device, { ...base, type: "offline_audio_begin", window_bytes: 524288 }).window_bytes, undefined);
  const begin = () => receiver.begin(device, { ...base, window_bytes: 524288 });
  const commit = extra => receiver.commit(device, { type: "meeting_audio_commit", request_id: "commit", segment_id: base.segment_id, ...extra });
  begin();
  assert.throws(() => commit(), /incomplete/);
  // V1 fallback is still accepted after a compact begin (low device memory).
  let offset = 0;
  for (; offset < 16128; offset += 8064) {
    const ack = receiver.chunk(device, makeChunk(data, offset, "chunk", 8064));
    assert.equal(ack.next_offset, offset + 8064);
  }
  receiver = new ReplayReceiver(dir); // restart obtains a durable offset
  assert.equal(begin().next_offset, offset);
  for (; offset < data.length;) {
    const end = Math.min(offset + 8064, data.length);
    const packet = makeChunk(data, offset, "window", end - offset);
    packet.writeUInt32LE(2, 8); packet.writeUInt32LE(end === data.length ? 1 : 0, 60);
    const ack = receiver.chunk(device, packet);
    assert.equal(ack?.next_offset, end === data.length ? end : undefined);
    offset = end;
  }
  assert.throws(() => commit({ total_bytes: data.length - 1 }), /differs/);
  assert.throws(() => commit({ crc32: (base.crc32 ^ 1) >>> 0 }), /differs/);
  assert(commit().ok);
  // Lost commit ACK: repeat begin/commit without repeated metadata.
  receiver = new ReplayReceiver(dir);
  assert.equal(begin().next_offset, data.length);
  assert(commit().ok);
  assert.deepEqual(fs.readFileSync(receiver.paths(device, base.segment_id).complete), data);
  assert(receiver.audioSessions.end(device, { type: "meeting_audio_session_end", request_id: "end",
    audio_session_id: base.audio_session_id, segment_count: 1, recording_complete: true }).ok);
  console.log("PASS: compact and legacy version gates; malformed windows; v1 fallback; durable restart/resume; metadata mismatch; compact commit retry; exact audio and session end");
} finally {
  assert.equal(path.dirname(dir), path.resolve(os.tmpdir()));
  assert(path.basename(dir).startsWith("nuwax-compact-"));
  fs.rmSync(dir, { recursive: true, force: true });
}
