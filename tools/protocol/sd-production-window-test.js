"use strict";
// Feed the exact packets exported by the production C replay function into
// the unmodified PC receiver. This checks RAM-slot tails inside a 512KiB window.
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { ReplayReceiver, archiveCrc } = require("./offline-audio-replay-gateway");
const input = path.resolve(__dirname, "../../build-host-checks");
const data = fs.readFileSync(path.join(input, "sd-backed-window.bin"));
const wire = fs.readFileSync(path.join(input, "sd-backed-window.packets"));
const dir = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-production-window-"));
try {
 for (const compact of [false, true]) {
  const receiver = new ReplayReceiver(path.join(dir, compact ? "compact" : "legacy")), device = "PRODUCTION-WINDOW-TEST";
  const fields = { audio_session_id: "meeting_stream_host", segment_index: 0,
    segment_id: "sd_meeting_stream_host_00000000", total_bytes: data.length,
    crc32: archiveCrc(data).crc, capture_start_ms: 0,
    capture_end_ms: data.readUInt32LE(data.length - 20) * 20 };
  const begin = receiver.begin(device, { ...fields, type: "meeting_audio_begin", request_id: "begin",
    ...(compact ? {} : { replay_version: 2, replay_revision: 2 }), window_bytes: 524288 });
  assert.equal(begin.window_bytes, 524288);
  let confirmations = 0, shortInterior = 0, previous = 0;
  for (let at = 0; at < wire.length;) {
    const size = wire.readUInt32LE(at); at += 4;
    const packet = wire.subarray(at, at + size); at += size;
    assert.equal(packet.length, size);
    const final = packet.readUInt32LE(60) === 1;
    if (!final && packet.readUInt32LE(16) < 8064) shortInterior++;
    const ack = receiver.chunk(device, packet);
    if (!final) { assert.equal(ack, null); continue; }
    assert(ack.ok && ack.next_offset > previous && ack.next_offset - previous <= 524288);
    previous = ack.next_offset; confirmations++;
  }
  assert.equal(confirmations, Math.ceil(data.length / 524288));
  assert(shortInterior > 0 && previous === data.length);
  const commit = receiver.commit(device, { ...(compact ? { segment_id: fields.segment_id } : fields), type: "meeting_audio_commit", request_id: "commit" });
  assert(commit.ok);
  assert.deepEqual(fs.readFileSync(receiver.paths(device, fields.segment_id).complete), data);
  const end = receiver.audioSessions.end(device, { type: "meeting_audio_session_end", request_id: "end",
    audio_session_id: fields.audio_session_id, segment_count: 1, recording_complete: true });
  assert(end.ok);
  console.log(`PASS: production packets -> existing gateway: ${data.length} exact bytes, ${confirmations} durable ACKs, ${shortInterior} interior short frames, commit and session end`);
 }
} finally {
  assert.equal(path.dirname(dir), path.resolve(os.tmpdir()));
  assert(path.basename(dir).startsWith("nuwax-production-window-"));
  fs.rmSync(dir, { recursive: true, force: true });
}
