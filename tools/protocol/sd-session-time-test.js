"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { once } = require("node:events");
const { ReplayReceiver, archiveCrc, parseArgs, startServer } = require("./offline-audio-replay-gateway");
const { fixture } = require("./sd-audio-sessions-test");
const { packet } = require("./sd-scene-protocol-test");
const { scanLibrary, renderWav } = require("./sd-audio-library");
const { sessionTime } = require("./sd-session-time");

async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-scene-time-"));
  let receiver = new ReplayReceiver(root), serial = 0, conn = 1;
  const device = "TIME-TEST", started = 1792634400123;
  const control = (id, suffix, time, extra = {}, mode = "meeting") => ({
    type: `${mode}_audio_${suffix}`, request_id: `t${++serial}`, audio_session_id: id,
    ...(time === undefined ? {} : { session_time: time }), ...extra });
  const begin = (id, time, mode = "meeting") => receiver.scenes.begin(device, control(id, "begin", time, {}, mode), conn);
  const end = (id, time, count = 1, mode = "meeting", complete = true) => receiver.scenes.end(device,
    control(id, "session_end", time, { segment_count: count, recording_complete: complete }, mode), conn);
  const stored = (id, name = "session.json") => JSON.parse(fs.readFileSync(path.join(receiver.audioSessions.sessionDir(device, id), name)));
  const upload = (id, index = 0, monitoring = false) => {
    const data = fixture(id, index, monitoring, index * 960);
    for (let at = 0; at < data.length; at += 8064)
      assert.equal(receiver.scenes.chunk(device, packet(data, id, index, at), conn), null);
    const ack = receiver.scenes.commit(device, control(id, monitoring ? "history_commit" : "commit", undefined,
      { segment_index: index, total_bytes: data.length, crc32: archiveCrc(data).crc }, monitoring ? "monitoring" : "meeting"), conn);
    assert(ack.ok);
    const saved = receiver.paths(device, `sd_${id}_${index.toString(16).padStart(8, "0")}`).complete;
    assert.deepEqual(fs.readFileSync(saved), data); // Time metadata never changes archive/CRC.
    return data;
  };
  try {
    for (const invalid of [null, [], {}, { started_at_ms: "123", duration_ms: null },
      { started_at_ms: started, duration_ms: -1 }, { started_at_ms: started, duration_ms: 0.5 },
      { started_at_ms: Number.MAX_SAFE_INTEGER + 1, duration_ms: 1 }]) {
      assert.throws(() => begin("invalid", invalid), /session_time/);
    }
    assert.equal(sessionTime({}), undefined);
    const meetingTime = { started_at_ms: started, duration_ms: 8 * 3600000 };
    assert(begin("meeting", meetingTime).ok);
    upload("meeting");
    assert((await end("meeting", meetingTime)).ok);
    assert.deepEqual(stored("meeting").sessionTime, meetingTime);
    const before = scanLibrary(root).sessions.find(s => s.sessionId === "meeting");
    assert.equal(before.durationMs, 960);
    assert.deepEqual(before.sessionTime, meetingTime);
    const wav = path.join(root, "preview.wav");
    assert.equal(renderWav(root, before.records, wav).durationMs, 960);
    const audio = fs.readFileSync(wav);

    await receiver.audioSessions.close(); receiver = new ReplayReceiver(root); conn++;
    assert(begin("meeting", { started_at_ms: started + 20000, duration_ms: 10 }).ok);
    assert((await end("meeting", { started_at_ms: null, duration_ms: null })).ok);
    assert.deepEqual(stored("meeting").sessionTime, meetingTime);
    assert.deepEqual(stored("meeting", "scene.json").sessionTime, meetingTime);
    const after = scanLibrary(root).sessions.find(s => s.sessionId === "meeting");
    assert.equal(after.revision, before.revision);
    renderWav(root, after.records, wav);
    assert.deepEqual(fs.readFileSync(wav), audio);

    assert(begin("monitor", { started_at_ms: null, duration_ms: null }, "monitoring").ok);
    upload("monitor", 0, true);
    assert.equal(scanLibrary(root).sessions.find(s => s.sessionId === "monitor").sessionTime.duration_ms, null);
    const monitorTime = { started_at_ms: started - 5000, duration_ms: 60000 };
    assert((await end("monitor", monitorTime, 1, "monitoring")).ok);
    assert.deepEqual(stored("monitor").sessionTime, monitorTime);

    assert(begin("legacy").ok); upload("legacy");
    assert((await end("legacy", undefined)).ok);
    assert.equal(stored("legacy").sessionTime, undefined);
    assert(begin("empty", { started_at_ms: started, duration_ms: 0 }).ok);
    assert((await end("empty", { started_at_ms: started + 1, duration_ms: 123 }, 0)).ok);
    assert.equal(stored("empty").sessionTime.duration_ms, 0);

    // Retries sharing the same worker must persist each request's new known fields.
    assert(begin("parallel", { started_at_ms: null, duration_ms: null }).ok);
    const done = await Promise.all([end("parallel", undefined, 0), end("parallel", meetingTime, 0)]);
    assert(done.every(ack => ack.ok)); assert.deepEqual(stored("parallel").sessionTime, meetingTime);
    assert.notEqual(done[0].request_id, done[1].request_id);

    assert(begin("late", meetingTime).ok); upload("late");
    assert((await end("late", meetingTime, 2, "meeting", false)).ok);
    upload("late", 1); // Late audio refresh must preserve time even without a time field in its job.
    assert((await end("late", undefined, 2, "meeting", false)).ok);
    assert.equal(stored("late").receivedSegments, 2);
    assert.deepEqual(stored("late").sessionTime, meetingTime);

    // A fieldless BEGIN can be backfilled without rewriting any confirmed audio.
    assert(begin("backfill").ok); upload("backfill");
    assert(begin("backfill", { started_at_ms: started, duration_ms: null }).ok);
    assert((await end("backfill", { started_at_ms: null, duration_ms: 2000 })).ok);
    assert.deepEqual(stored("backfill").sessionTime, { started_at_ms: started, duration_ms: 2000 });
    assert.equal(scanLibrary(root).issues.length, 0);

    const args = parseArgs(["node", "gateway", "--host", "127.0.0.1", "--storage-dir", root]); args.port = 0;
    const server = startServer(args); await once(server, "listening");
    let ws;
    try {
      ws = new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws?device_id=${device}`);
      await once(ws, "open");
      const reply = once(ws, "message", { signal: AbortSignal.timeout(5000) });
      const now = Date.now(); ws.send(JSON.stringify({ type: "ping", now_ms: 0 }));
      const pong = JSON.parse((await reply)[0].data);
      assert.equal(pong.type, "pong"); assert.equal(pong.now_ms, 0);
      assert(Number.isSafeInteger(pong.server_ms) && pong.server_ms >= now && pong.server_ms <= Date.now());
    } finally {
      if (ws) { const closed = once(ws, "close"); ws.close(); await closed; }
      await new Promise(resolve => server.close(resolve));
    }
    console.log("PASS: time fields survive real scene/file/end transfer, restart, lost ACK, concurrent end, late files, legacy nulls and backfill; audio bytes/WAV unchanged; application pong carries UTC");
  } finally {
    await receiver.audioSessions.close();
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
