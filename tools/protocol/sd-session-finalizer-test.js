"use strict";
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const assert = require("node:assert/strict"), { performance } = require("node:perf_hooks");
const { AudioSessions } = require("./sd-audio-sessions");
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-sd-finalizer-"));
  try {
    const receiver = new AudioSessions(root), device = "FINALIZER", session = "monitor_eight_hours";
    receiver.begin(device, { segment_id: "warm_file", audio_session_id: "warm_capture", segment_index: 0,
      total_bytes: 794, crc32: 1 }, { complete: path.join(root, "warm.complete") }, "meeting_audio", true);
    assert.equal(receiver.finalizers.started, 1);
    const warmDeadline = Date.now() + 10000;
    while (![...receiver.finalizers.workers].every(slot => slot.ready)) {
      assert(Date.now() < warmDeadline, "worker did not become ready");
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    const dir = receiver.sessionDir(device, session); fs.mkdirSync(dir, { recursive: true });
    const count = 30000;
    for (let i = 0; i < count; i++) fs.writeFileSync(path.join(dir, `${String(i).padStart(8,"0")}.json`), JSON.stringify({
      committed: true, deviceId: device, sessionId: session, segmentIndex: i, mode: "monitoring", timelineBasis: "uptime",
      captureStartMs: 1000 + i * 960, captureEndMs: 1000 + (i + 1) * 960,
      audioDurationMs: 960, complete: true, segmentId: `segment_${i}`, processingClass: "realtime",
      totalBytes: 32096, crc32: 1, completePath: `fixture_${i}.complete` }));
    const message = { type: "monitoring_audio_session_end", request_id: "end1", audio_session_id: session,
      segment_count: count, recording_complete: true };
    let ticks = 0, maxGapMs = 0, previous = performance.now();
    const timer = setInterval(() => { const now = performance.now(); maxGapMs = Math.max(maxGapMs, now - previous); previous = now; ticks++; }, 10);
    const started = performance.now();
    let acknowledgements;
    try {
      const first = receiver.endAsync(device, message);
      const duplicate = receiver.endAsync(device, { ...message, request_id: "end2" });
      assert.equal(receiver.endJobs.size, 1);
      await assert.rejects(receiver.endAsync(device, { ...message, segment_count: count + 1 }), /changed while/);
      assert.throws(() => receiver.begin(device, { audio_session_id: session, segment_id: "late" }, {}, "monitoring_audio_history"), /finalization in progress/);
      acknowledgements = await Promise.all([first, duplicate]);
    } finally { clearInterval(timer); }
    const firstMs = performance.now() - started;
    assert(acknowledgements.every(ack => ack.ok));
    assert.deepEqual(acknowledgements.map(ack => ack.request_id), ["end1", "end2"]);
    assert.equal(receiver.endJobs.size, 0);
    assert.equal(receiver.finalizers.started, 1); // Reuse the worker started at begin.
    const saved = JSON.parse(fs.readFileSync(path.join(dir, "session.json")));
    assert.equal(saved.receivedSegments, count); assert.equal(saved.audioDurationMs, 8 * 3600000);
    assert.equal(saved.timelineDurationMs, 8 * 3600000); assert.equal(saved.segments[0].startMs, 0);
    assert.deepEqual(saved.missingSegments, []); assert(ticks > 1 && maxGapMs < 1000);
    // A restarted receiver reads only the durable summary, without re-opening 30000 indices.
    let indexReads = 0; const read = fs.readFileSync;
    fs.readFileSync = function(file, ...args) { if (/\d{8}\.json$/.test(String(file))) indexReads++; return read.call(this, file, ...args); };
    const retryStart = performance.now();
    try {
      assert(new AudioSessions(root).end(device, { ...message, request_id: "retry" }).ok);
      assert.throws(() => receiver.end(device, { ...message, recording_complete: false }), /changed after/);
    } finally { fs.readFileSync = read; }
    assert.equal(indexReads, 0);
    // Warm worker remains reusable after a validation error, and a process
    // crash must reject (never ACK) the active job and allow the next retry.
    await assert.rejects(receiver.endAsync(device, { ...message, recording_complete: false }), /changed after/);
    assert((await receiver.endAsync(device, { ...message, request_id: "warm_retry" })).ok);
    assert.equal(receiver.finalizers.started, 1);
    const slot = [...receiver.finalizers.workers][0];
    const crashed = receiver.endAsync(device, { ...message, request_id: "crash_retry" });
    const rejected = assert.rejects(crashed, /exited/);
    await slot.worker.terminate(); await rejected;
    assert.equal(receiver.endJobs.size, 0);
    assert((await receiver.endAsync(device, { ...message, request_id: "after_crash" })).ok);
    assert.equal(receiver.finalizers.started, 2);
    console.log(JSON.stringify({ result: "PASS", segments: count, syntheticAudioHours: 8,
      firstMs, retryMs: performance.now() - retryStart, mainLoopMaxGapMs: maxGapMs, heartbeatTicks: ticks,
      checks: "begin prewarm, worker reuse, validation failure/retry, crash rejects/replaces, durable-before-ACK, retry dedupe, conflicting end/late begin fenced, restart fast ACK, network loop responsive" }));
  } finally {
    assert.equal(path.dirname(root), path.resolve(os.tmpdir())); assert(path.basename(root).startsWith("nuwax-sd-finalizer-"));
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
