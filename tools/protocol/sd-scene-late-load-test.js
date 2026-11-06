"use strict";
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const assert = require("node:assert/strict"), { performance } = require("node:perf_hooks");
const { ReplayReceiver, archiveCrc } = require("./offline-audio-replay-gateway");
const { fixture } = require("./sd-audio-sessions-test"), { packet } = require("./sd-scene-protocol-test");
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-scene-late-load-"));
  const receiver = new ReplayReceiver(root), device = "LOAD", id = "monitor_eight_hours", count = 30000;
  const scene = receiver.scenes;
  let timer;
  try {
    scene.begin(device, { type: "monitoring_audio_begin", request_id: "b", audio_session_id: id }, 1);
    const folder = receiver.audioSessions.sessionDir(device, id);
    for (let i = 0; i < count - 1; i++) fs.writeFileSync(path.join(folder, `${String(i).padStart(8, "0")}.json`), JSON.stringify({
      committed: true, deviceId: device, sessionId: id, segmentIndex: i, mode: "monitoring", timelineBasis: "uptime",
      captureStartMs: 1000 + i * 960, captureEndMs: 1000 + (i + 1) * 960, audioDurationMs: 960, complete: true,
      segmentId: `segment_${i}`, processingClass: "realtime", totalBytes: 32096, crc32: 1, completePath: `fixture_${i}.complete` }));
    const end = { type: "monitoring_audio_session_end", request_id: "e", audio_session_id: id,
      segment_count: count, recording_complete: false };
    console.log("Prepared synthetic 8-hour metadata; finalizing incomplete scene in worker.");
    assert((await scene.end(device, end, 1)).ok);
    const data = fixture(id, count - 1, true, 1000 + (count - 1) * 960);
    for (let at = 0; at < data.length; at += 8064) scene.chunk(device, packet(data, id, count - 1, at), 1);
    let ticks = 0, maxGapMs = 0, previous = performance.now();
    timer = setInterval(() => { const now = performance.now(); maxGapMs = Math.max(maxGapMs, now - previous); previous = now; ticks++; }, 10);
    let indexReads = 0; const read = fs.readFileSync;
    fs.readFileSync = function(file, ...args) { if (/\d{8}\.json$/.test(String(file))) indexReads++; return read.call(this, file, ...args); };
    const started = performance.now(); let commitMs;
    try {
      assert(scene.commit(device, { type: "monitoring_audio_history_commit", request_id: "c", audio_session_id: id,
        segment_index: count - 1, total_bytes: data.length, crc32: archiveCrc(data).crc }, 1).ok);
      commitMs = performance.now() - started;
    } finally { fs.readFileSync = read; }
    const marker = receiver.audioSessions.refreshes.marker(device, id);
    assert(fs.existsSync(marker));
    const deadline = Date.now() + 120000;
    while (fs.existsSync(marker)) { assert(Date.now() < deadline, "background refresh deadline"); await new Promise(r => setTimeout(r, 25)); }
    clearInterval(timer); timer = null;
    const saved = JSON.parse(fs.readFileSync(path.join(folder, "session.json")));
    assert.equal(saved.receivedSegments, count); assert.equal(saved.audioDurationMs, 8 * 3600000);
    assert.equal(saved.recordingComplete, false); assert.deepEqual(saved.missingSegments, []);
    assert(indexReads < 10, `late commit rescanned ${indexReads} indices`);
    assert(commitMs < 6000, `file ACK exceeds device deadline: ${commitMs}`);
    assert(ticks > 1 && maxGapMs < 1000, `network loop blocked: ${maxGapMs}`);
    const result = { result: "PASS", syntheticAudioHours: 8, indices: count, lateFileBytes: data.length,
      commitMs, mainThreadIndexReads: indexReads, backgroundElapsedMs: performance.now() - started,
      mainLoopMaxGapMs: maxGapMs, heartbeatTicks: ticks, hardwareThroughputMeasured: false };
    console.log(JSON.stringify(result));
    if (process.env.SD_SCENE_LOAD_RESULT) fs.writeFileSync(process.env.SD_SCENE_LOAD_RESULT, JSON.stringify(result, null, 2));
  } finally {
    if (timer) clearInterval(timer);
    await receiver.audioSessions.close();
    assert.equal(path.dirname(root), path.resolve(os.tmpdir())); assert(path.basename(root).startsWith("nuwax-scene-late-load-"));
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
