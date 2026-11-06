"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { ReplayReceiver, archiveCrc } = require("./offline-audio-replay-gateway");
const { fixture } = require("./sd-audio-sessions-test");
const { packet } = require("./sd-scene-protocol-test");

async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-discard-protocol-"));
  const receiver = new ReplayReceiver(root), device = "DISCARD-TEST";
  let serial = 0, connection = 1;
  const message = (type, id, fields = {}) => ({ type, request_id: `r${++serial}`, audio_session_id: id, ...fields });
  try {
    for (const monitoring of [false, true]) {
      const prefix = monitoring ? "monitoring" : "meeting", id = `${prefix}_discard`;
      const begin = scene => receiver.scenes.begin(device, message(`${prefix}_audio_begin`, scene), connection);
      assert(begin(id).ok);
      // A bad file may already have a partial cloud-side staging object.
      // It is not committed and must not block indexes 3/4 or partial end.
      const bad = fixture(id, 2, monitoring, 80, 2, 4);
      assert.equal(receiver.scenes.chunk(device, packet(bad, id, 2, 0, 512), connection), null);
      receiver.scenes.release(device, connection++);
      assert(begin(id).ok);
      for (const index of [0, 1, 3, 4]) {
        const data = fixture(id, index, monitoring, index * 40, 2, index * 2);
        for (let at = 0; at < data.length; at += 8064)
          assert.equal(receiver.scenes.chunk(device, packet(data, id, index, at), connection), null);
        const commitType = monitoring ? "monitoring_audio_history_commit" : "meeting_audio_commit";
        assert(receiver.scenes.commit(device, message(commitType, id,
          { segment_index: index, total_bytes: data.length, crc32: archiveCrc(data).crc }), connection).ok);
        const stored = receiver.paths(device, `sd_${id}_${index.toString(16).padStart(8, "0")}`).complete;
        assert.deepEqual(fs.readFileSync(stored), data);
      }
      const end = message(`${prefix}_audio_session_end`, id, { segment_count: 5, recording_complete: false });
      assert((await receiver.scenes.end(device, end, connection)).ok);
      assert((await receiver.scenes.end(device, end, connection)).ok);
      const manifest = JSON.parse(fs.readFileSync(path.join(receiver.audioSessions.sessionDir(device, id), "session.json")));
      assert.deepEqual(manifest.missingSegments, [[2, 2]]);
      assert.deepEqual(manifest.segments.map(segment => segment.index), [0, 1, 3, 4]);
      assert.equal(manifest.recordingComplete, false);
      assert.equal(manifest.receivedSegments, 4);
      assert.equal(manifest.audioDurationMs, 160);
      // The following scene starts normally with its own index zero.
      const next = `${id}_next`;
      assert(begin(next).ok);
      const data = fixture(next, 0, monitoring, 0, 2, 0);
      receiver.scenes.chunk(device, packet(data, next, 0), connection);
      assert(receiver.scenes.commit(device, message(monitoring ? "monitoring_audio_history_commit" : "meeting_audio_commit", next,
        { segment_index: 0, total_bytes: data.length, crc32: archiveCrc(data).crc }), connection).ok);
      assert((await receiver.scenes.end(device, message(`${prefix}_audio_session_end`, next,
        { segment_count: 1, recording_complete: true }), connection)).ok);
      console.log(`PASS existing ${prefix} gateway: partial staging at bad index 2 cannot block exact indexes 0,1,3,4, count=5 partial end or next scene; no protocol change`);
    }
  } finally {
    receiver.scenes.release(device, connection);
    await receiver.audioSessions.close();
    fs.rmSync(root, { recursive: true, force: true });
  }
}
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
