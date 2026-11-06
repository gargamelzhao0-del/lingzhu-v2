"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { once } = require("node:events");
const { ReplayReceiver, archiveCrc, makeChunk, startServer, parseArgs } = require("./offline-audio-replay-gateway");
const { fixture, crc32 } = require("./sd-audio-sessions-test");
const { scanLibrary, renderWav } = require("./sd-audio-library");
const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-file-protocol-"));
const device = "FILE-PROTOCOL";
let receiver = new ReplayReceiver(root), serial = 0;
function fields(data) { return { audio_session_id: data.toString("ascii", 36, 84).replace(/\0.*$/, ""),
  segment_index: data.readUInt32LE(24), total_bytes: data.length, crc32: archiveCrc(data).crc }; }
function identity(data) { const f = fields(data); return { audio_session_id: f.audio_session_id, segment_index: f.segment_index }; }
function begin(data, route = "meeting_audio") {
  const msg = { type: `${route}_begin`, request_id: `b${++serial}`, ...fields(data) };
  const ack = receiver.begin(device, msg);
  assert.deepEqual(ack, { type: `${route}_begin_ack`, request_id: msg.request_id, ok: true,
    mode: undefined, ...identity(data), next_offset: ack.next_offset });
  return { msg, ack };
}
function packet(data, offset, request) {
  const value = makeChunk(data, offset, request, 8064);
  value.writeUInt32LE(2, 8); value.writeUInt32LE(offset + value.length - 64 === data.length ? 1 : 0, 60);
  return value;
}
function send(data, b, end = data.length) {
  for (let offset = b.ack.next_offset; offset < end; offset += 8064)
    assert.equal(receiver.chunk(device, packet(data, offset, b.msg.request_id)), null);
}
function commit(data, route = "meeting_audio") {
  const msg = { type: `${route}_commit`, request_id: `c${++serial}`, ...identity(data) };
  assert.deepEqual(receiver.commit(device, msg), { ...msg, type: `${route}_commit_ack`, ok: true, mode: undefined });
}
function upload(data, route) { const b = begin(data, route); send(data, b); commit(data, route); }
function end(session, count, monitoring = false, complete = true) {
  return receiver.audioSessions.end(device, { type: `${monitoring ? "monitoring" : "meeting"}_audio_session_end`,
    request_id: `e${++serial}`, audio_session_id: session, segment_count: count, recording_complete: complete });
}
function stored(data) { return receiver.paths(device, receiver.audioSessions.fileIdentity(device, fields(data), "meeting_audio")).complete; }
async function main() {
  try {
    const data = fixture("meeting_file", 0, false, 0, 2000);
    const b = begin(data);
    assert.throws(() => receiver.chunk(device, packet(data, 0, "stale")), /begin request/);
    assert.throws(() => receiver.chunk(device, makeChunk(data, 0, b.msg.request_id, 8064)), /version 2/);
    // A malformed packet must not select the transaction's data version.
    send(data, b, 70 * 8064);
    receiver = new ReplayReceiver(root); // Crash: only the fsynced prefix survives.
    const resumed = begin(data);
    assert(resumed.ack.next_offset > 0 && resumed.ack.next_offset < 70 * 8064);
    assert.throws(() => receiver.commit(device, { type: "meeting_audio_commit", request_id: "wrong",
      ...identity(data), segment_index: 1 }));
    send(data, resumed); commit(data);
    assert.deepEqual(fs.readFileSync(stored(data)), data);
    receiver = new ReplayReceiver(root);
    assert.equal(begin(data).ack.next_offset, data.length); commit(data); // Lost commit ACK.
    assert(end("meeting_file", 1).ok);
    const finished = JSON.parse(fs.readFileSync(path.join(root, "sessions", `${device}__meeting_file`, "session.json")));
    assert.equal(finished.timelineDurationMs, 40000); assert.equal(finished.audioDurationMs, 40000);
    assert.equal(finished.segments[0].startMs, 0);
    assert.throws(() => receiver.begin(device, { ...b.msg, crc32: (b.msg.crc32 ^ 1) >>> 0 }), /immutable/);
    assert.equal(receiver.begin("OTHER-DEVICE", b.msg).next_offset, 0);
    for (const segment_index of [-1, 0.5, 1000000, null, "0"])
      assert.throws(() => receiver.begin(device, { ...b.msg, segment_index }));
    for (const audio_session_id of ["../bad", "", "x".repeat(36)])
      assert.throws(() => receiver.begin(device, { ...b.msg, audio_session_id }));

    const history = fixture("monitor_preempt", 0, true), live = fixture("monitor_preempt", 1, true, 2000);
    const h = begin(history, "monitoring_audio_history"); send(history, h, 16128);
    upload(live, "monitoring_audio_realtime");
    const hr = begin(history, "monitoring_audio_history"); assert.equal(hr.ack.next_offset, 16128);
    send(history, hr); commit(history, "monitoring_audio_history");
    assert(end("monitor_preempt", 2, true).ok);
    const expired = fixture("monitor_expired", 0, true);
    const e = begin(expired, "monitoring_audio_realtime");
    [...receiver.transactions.values()].find(tx => tx.deviceId === device).session.started -= 4000000000n;
    send(expired, e); commit(expired, "monitoring_audio_realtime");
    assert.equal(JSON.parse(fs.readFileSync(stored(expired) + ".json")).processingClass, "history");

    // An open scene made with the old identifier/times can finish after upgrade.
    const old = fixture("meeting_upgrade", 0, false), next = fixture("meeting_upgrade", 1, false, 960);
    const oldFields = { ...fields(old), segment_id: "old-arbitrary-name", capture_start_ms: 0, capture_end_ms: 960 };
    receiver.begin(device, { type: "meeting_audio_begin", request_id: "old", ...oldFields });
    for (let offset = 0; offset < old.length; offset += 8064) receiver.chunk(device, makeChunk(old, offset, "old_chunk", 8064));
    receiver.commit(device, { type: "meeting_audio_commit", request_id: "old_commit", segment_id: oldFields.segment_id });
    assert.equal(begin(old).ack.next_offset, old.length); commit(old); // Alias is preserved.
    upload(next); assert(end("meeting_upgrade", 2).ok);
    const upgraded = JSON.parse(fs.readFileSync(path.join(root, "sessions", `${device}__meeting_upgrade`, "session.json")));
    assert.deepEqual(upgraded.segments.map(s => [s.startMs, s.endMs]), [[0, 960], [960, 1920]]);

    // Time wraps at 2^32: keep the 64-bit timeline and CRC-valid frames intact.
    const wrap = fixture("meeting_wrap", 0, false), start = 0xffffffff - 199;
    wrap.writeBigUInt64LE(BigInt(start), 28);
    const frames = [];
    for (let i = 0; i < 48; i++) { const f = wrap.subarray(100 + i * 666, 762 + i * 666);
      f.writeUInt32LE((start + i * 20) >>> 0, 14); frames.push(f); }
    wrap.writeUInt32LE(crc32(Buffer.concat(frames)), wrap.length - 8);
    upload(wrap); assert(end("meeting_wrap", 1).ok);
    assert.equal(JSON.parse(fs.readFileSync(stored(wrap) + ".json")).captureEndMs, start + 960);

    const corrupt = fixture("meeting_corrupt", 0, false), c = begin(corrupt);
    const broken = Buffer.from(corrupt); broken[150] ^= 1;
    send(broken, c); assert.throws(() => commit(corrupt), /CRC/);
    assert.equal(begin(corrupt).ack.next_offset, 0); upload(corrupt); assert(end("meeting_corrupt", 1).ok);
    upload(fixture("meeting_missing", 1, false, 960));
    assert.throws(() => end("meeting_missing", 2), /incomplete/);
    assert(end("meeting_missing", 2, false, false).ok);

    // Delayed monitor tails from older firmware: chronological playback,
    // original file IDs/indices, no synthetic ACK or dropped audio.
    for (const [index, start] of [[0, 0], [1, 1920], [2, 960]])
      upload(fixture("monitor_tail_order", index, true, start, 48, start / 20), "monitoring_audio_history");
    assert(end("monitor_tail_order", 3, true).ok);
    assert(end("monitor_tail_order", 3, true).ok); // lost end ACK
    const tails = scanLibrary(root).sessions.find(s => s.sessionId === "monitor_tail_order");
    assert.equal(tails.state, "complete");
    assert.deepEqual(tails.records.map(r => r.index), [0, 2, 1]);
    const tailWav = renderWav(root, tails.records, path.join(root, "tails.wav"));
    assert.equal(tailWav.durationMs, 2880); assert.equal(tailWav.discontinuities, 0);
    for (const [index, start] of [[0, 0], [1, 500]])
      upload(fixture("monitor_overlap", index, true, start), "monitoring_audio_history");
    assert.throws(() => end("monitor_overlap", 2, true), /overlapping/);
    upload(fixture("meeting_reverse", 0, false, 960));
    upload(fixture("meeting_reverse", 1, false, 0));
    assert.throws(() => end("meeting_reverse", 2), /overlapping/);
    upload(fixture("monitor_tail_missing", 1, true, 1920), "monitoring_audio_history");
    upload(fixture("monitor_tail_missing", 3, true, 960), "monitoring_audio_history");
    assert.throws(() => end("monitor_tail_missing", 4, true), /incomplete/);
    assert(end("monitor_tail_missing", 4, true, false).ok);
    const missingTail = JSON.parse(fs.readFileSync(path.join(root, "sessions", `${device}__monitor_tail_missing`, "session.json")));
    assert.deepEqual(missingTail.missingSegments, [[0, 0], [2, 2]]);

    const library = scanLibrary(root), scene = library.sessions.find(s => s.sessionId === "meeting_upgrade");
    assert.equal(library.issues.length, 0);
    assert.deepEqual(scene.records.map(r => [r.startMs, r.endMs]), [[0, 960], [960, 1920]]);
    const rendered = renderWav(root, scene.records, path.join(root, "test.wav"));
    assert.equal(rendered.durationMs, 1920);

    // Exact JSON and packet bytes emitted by the production C file uploader.
    const production = path.resolve(__dirname, "../../build-host-checks");
    const controls = fs.readFileSync(path.join(production, "sd-file.controls.jsonl"), "utf8").trim().split(/\r?\n/).map(JSON.parse);
    assert.equal(controls.length, 2); assert.notEqual(controls[0].request_id, controls[1].request_id);
    for (const message of controls) for (const field of ["segment_id", "capture_start_ms", "capture_end_ms", "capture_age_ms", "window_bytes", "replay_version", "replay_revision"])
      assert.equal(message[field], undefined);
    const actual = fs.readFileSync(path.join(production, "sd-file.bin"));
    const wire = fs.readFileSync(path.join(production, "sd-file.packets"));
    assert(receiver.scenes.begin(device, controls[0]).ok);
    for (let at = 0; at < wire.length;) {
      const size = wire.readUInt32LE(at); at += 4;
      assert.equal(receiver.scenes.chunk(device, wire.subarray(at, at + size)), null); at += size;
    }
    assert(receiver.scenes.commit(device, controls[1]).ok);
    assert.deepEqual(fs.readFileSync(stored(actual)), actual);

    // Run the published transcripts through a real WebSocket. Any extra data ACK fails.
    const sample = path.resolve(__dirname, "../../docs/examples/sd-audio-cloud/sample");
    const args = parseArgs(["node", "gateway", "--host", "127.0.0.1", "--storage-dir", path.join(root, "wire")]); args.port = 0;
    const server = startServer(args); await once(server, "listening");
    try {
      for (const scenario of JSON.parse(fs.readFileSync(path.join(sample, "manifest.json"))).scenarios) {
        const ws = new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws?device_id=${scenario.name}`);
        const received = []; ws.addEventListener("message", event => received.push(JSON.parse(event.data)));
        await once(ws, "open");
        const events = JSON.parse(fs.readFileSync(path.join(sample, scenario.transcript)));
        for (let i = 0; i < events.length; i++) {
          const event = events[i];
          if (event.direction === "cloud_to_device") continue;
          const expected = events[i + 1]?.direction === "cloud_to_device" ? events[i + 1].json : null;
          const reply = expected ? new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error("wire ACK timeout")), 5000);
            ws.addEventListener("message", message => { clearTimeout(timer); resolve(JSON.parse(message.data)); }, { once: true });
          }) : null;
          ws.send(event.binary_file ? fs.readFileSync(path.join(sample, event.binary_file)) : JSON.stringify(event.json));
          if (reply) assert.deepEqual(await reply, expected);
        }
        assert.equal(received.length, 3); // begin, commit, session_end
        const closed = once(ws, "close"); ws.close(); await closed;
      }
    } finally { await new Promise(resolve => server.close(resolve)); }
    console.log("PASS: triple IDs, file-only confirmation, crash resume/lost ACK, corruption, cross-device isolation, live preemption/expiry, old-format upgrade, timestamp wrap/gaps, PCM playback, published real-WebSocket transcripts");
  } finally {
    assert.equal(path.dirname(root), path.resolve(os.tmpdir()));
    assert(path.basename(root).startsWith("nuwax-file-protocol-")); fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
