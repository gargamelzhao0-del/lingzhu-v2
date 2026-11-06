"use strict";
const assert = require("node:assert/strict"), fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { AudioSessions } = require("./sd-audio-sessions");
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-finalizer-bound-"));
  try {
    const receiver = new AudioSessions(root);
    const message = { type: "meeting_audio_session_end", audio_session_id: "meeting_a", segment_count: 0, recording_complete: true };
    const requests = Array.from({ length: 4 }, (_, i) => receiver.endAsync(`DEVICE_${i}`, { ...message, request_id: `end_${i}` }));
    assert.equal(receiver.endJobs.size, 4); assert.equal(receiver.finalizers.workers.size, 4);
    await assert.rejects(receiver.endAsync("DEVICE_4", { ...message, request_id: "overflow" }), /busy/);
    const results = await Promise.all(requests);
    for (let i = 0; i < 4; i++) {
      assert.equal(results[i].request_id, `end_${i}`); assert(results[i].ok);
      const record = JSON.parse(fs.readFileSync(path.join(receiver.sessionDir(`DEVICE_${i}`, message.audio_session_id), "session.json")));
      assert.equal(record.deviceId, `DEVICE_${i}`); assert.equal(record.receivedSegments, 0);
    }
    assert.equal(receiver.finalizers.workers.size, 1); assert.equal(receiver.endJobs.size, 0);
    assert((await receiver.endAsync("DEVICE_4", { ...message, request_id: "retry" })).ok);
    assert.equal(receiver.finalizers.started, 4);
    console.log("PASS: four-job bound, busy rejection, reply/device isolation, durable files, one idle worker retained and reused");
  } finally {
    assert.equal(path.dirname(root), path.resolve(os.tmpdir())); assert(path.basename(root).startsWith("nuwax-finalizer-bound-"));
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
