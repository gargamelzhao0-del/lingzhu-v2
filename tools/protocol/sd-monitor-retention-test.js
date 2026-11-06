"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { ReplayReceiver, archiveCrc } = require("./offline-audio-replay-gateway");
const { fixture } = require("./sd-audio-sessions-test");
const { packet } = require("./sd-scene-protocol-test");

async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-monitor-retention-cloud-"));
  const receiver = new ReplayReceiver(root), device = "RETENTION-TEST", id = "monitor_ring";
  let serial = 0, connection = 1;
  const message = (type, fields = {}) => ({ type, request_id: `r${++serial}`, audio_session_id: id, ...fields });
  const begin = () => receiver.scenes.begin(device, message("monitoring_audio_begin"), connection);
  try {
    assert(begin().ok);
    // This unconfirmed historical file was partially sent before the outage,
    // then legitimately evicted locally while the same scene kept recording.
    const evicted = fixture(id, 0, true, 0, 6297, 0);
    receiver.scenes.chunk(device, packet(evicted, id, 0), connection);
    receiver.scenes.release(device, connection++);
    assert(begin().ok);
    for (const index of [4, 3]) { // New current data first, retained history next.
      const data = fixture(id, index, true, index * 125940, index === 4 ? 7 : 6297, index * 6297);
      for (let offset = 0; offset < data.length; offset += 8064)
        receiver.scenes.chunk(device, packet(data, id, index, offset), connection);
      const commit = message(index === 4 ? "monitoring_audio_realtime_commit" : "monitoring_audio_history_commit",
        { segment_index: index, total_bytes: data.length, crc32: archiveCrc(data).crc });
      assert(receiver.scenes.commit(device, commit, connection).ok);
      const stored = receiver.paths(device, `sd_${id}_${index.toString(16).padStart(8, "0")}`).complete;
      assert.deepEqual(fs.readFileSync(stored), data);
    }
    const end = message("monitoring_audio_session_end", { segment_count: 5, recording_complete: false });
    assert((await receiver.scenes.end(device, end, connection)).ok);
    assert((await receiver.scenes.end(device, end, connection)).ok);
    const manifest = JSON.parse(fs.readFileSync(path.join(receiver.audioSessions.sessionDir(device, id), "session.json")));
    assert.deepEqual(manifest.missingSegments, [[0, 2]]);
    assert.equal(manifest.recordingComplete, false);
    assert.equal(manifest.receivedSegments, 2);
    assert.equal(manifest.audioDurationMs, (6297 + 7) * 20);
    assert.deepEqual(manifest.segments.map(s => s.index), [3, 4]);
    console.log("PASS: existing gateway/protocol accepts retained monitor files, exact bytes/time, missing prefix, idempotent incomplete session_end");
  } finally {
    receiver.scenes.release(device, connection);
    await receiver.audioSessions.close();
    fs.rmSync(root, { recursive: true, force: true });
  }
}
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
