"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { once } = require("node:events");
const { startServer, parseArgs, ReplayReceiver, makeChunk, archiveCrc } = require("./offline-audio-replay-gateway");

function crc32(data) {
  let crc = 0xffffffff;
  for (const b of data) { crc ^= b; for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0); }
  return (crc ^ 0xffffffff) >>> 0;
}
function fixture(session, index, monitoring, start = 0, count = 48, firstSequence = index * count) {
  const data = Buffer.alloc(96 + count * 666 + 32);
  data.write("NWXOFF1"); data.writeUInt16LE(1, 8); data.writeUInt16LE(96, 10);
  data[12] = monitoring ? 1 : 0; data[13] = 1;
  data.writeUInt16LE(1, 14); data.writeUInt32LE(16000, 16); data.writeUInt16LE(20, 20);
  data.writeUInt16LE(2, 22); data.writeUInt32LE(index, 24);
  data.writeBigUInt64LE(BigInt(1000 + start), 28); data.write(session, 36);
  const payloads = [];
  for (let i = 0; i < count; i++) {
    const pos = 96 + i * 666; data.writeUInt32LE(662, pos);
    const frame = data.subarray(pos + 4, pos + 666);
    frame.write("NA"); frame[2] = frame[3] = frame[5] = 1;
    frame.writeUInt16LE(16000, 6); frame.writeUInt16LE(20, 8);
    frame.writeUInt32LE(firstSequence + i, 10); frame.writeUInt32LE(1000 + start + i * 20, 14);
    frame.writeUInt16LE(640, 18); frame.fill(i + 1, 22); payloads.push(frame);
  }
  const footer = data.subarray(data.length - 32);
  footer.write("NWXEND1"); footer.writeUInt16LE(1, 8); footer.writeUInt16LE(32, 10);
  footer.writeUInt32LE(count, 12); footer.writeBigUInt64LE(BigInt(count * 662), 16);
  footer.writeUInt32LE(crc32(Buffer.concat(payloads)), 24); footer[28] = 1;
  return data;
}
async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-sd-sessions-"));
  const args = parseArgs(["node", "gateway", "--host", "127.0.0.1", "--storage-dir", dir]); args.port = 0;
  const server = startServer(args);
  await once(server, "listening");
  let ws;
  let serial = 0;
  const device = "NX-SESSION-TEST";
  const receivedTypes = [];
  async function connect() {
    ws = new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws?device_id=${device}`);
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
    ws.addEventListener("message", event => receivedTypes.push(JSON.parse(event.data).type));
  }
  async function close() { if (ws) await new Promise(resolve => { ws.onclose = resolve; ws.close(); }); ws = null; }
  function exchange(value, type) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { ws.removeEventListener("message", receive); reject(new Error(`timeout ${type}`)); }, 3000);
      function receive(event) {
        const message = JSON.parse(event.data); if (message.type !== type) return;
        clearTimeout(timer); ws.removeEventListener("message", receive); resolve(message);
      }
      ws.addEventListener("message", receive); ws.send(Buffer.isBuffer(value) ? value : JSON.stringify(value));
    });
  }
  function meta(session, index, monitoring, start = 0) {
    const data = fixture(session, index, monitoring, start);
    return { data, fields: { audio_session_id: session, segment_index: index,
      segment_id: `sd_${session}_${index}`, total_bytes: data.length, crc32: archiveCrc(data).crc,
      capture_start_ms: start, capture_end_ms: start + 960 } };
  }
  async function begin(part, route, extra = {}) {
    return exchange({ ...part.fields, type: `${route}_begin`, request_id: `b${++serial}`,
      ...(route === "monitoring_audio_realtime" ? { capture_age_ms: 100 } : {}), ...extra }, `${route}_begin_ack`);
  }
  async function upload(part, route, offset = 0) {
    while (offset < part.data.length) {
      const ack = await exchange(makeChunk(part.data, offset, `c${++serial}`, 4096), `${route}_chunk_ack`);
      assert(ack.ok); assert.equal(ack.segment_id, part.fields.segment_id); offset = ack.next_offset;
    }
    const ack = await exchange({ type: `${route}_commit`, request_id: `e${++serial}`,
      segment_id: part.fields.segment_id, total_bytes: part.fields.total_bytes, crc32: part.fields.crc32 }, `${route}_commit_ack`);
    assert(ack.ok);
    assert.deepEqual(fs.readFileSync(path.join(dir, `${device}__${part.fields.segment_id}.complete`)), part.data);
  }
  async function end(session, count, monitoring, complete = true) {
    const type = `${monitoring ? "monitoring" : "meeting"}_audio_session_end`;
    return exchange({ type, request_id: `s${++serial}`, audio_session_id: session,
      segment_count: count, recording_complete: complete }, `${type}_ack`);
  }
  try {
    await connect();
    const old = meta("monitor_one", 0, true);
    assert((await begin(old, "monitoring_audio_history")).ok);
    const partial = await exchange(makeChunk(old.data, 0, "old_first", 4096), "monitoring_audio_history_chunk_ack");
    const live = meta("monitor_one", 1, true, 2000);
    assert((await begin(live, "monitoring_audio_realtime")).ok);
    await upload(live, "monitoring_audio_realtime");
    // The commit ACK was lost from the device's perspective: rebind as history.
    await close(); await connect();
    assert.equal((await begin(live, "monitoring_audio_history")).next_offset, live.data.length);
    await upload(live, "monitoring_audio_history", live.data.length);
    const resumed = await begin(old, "monitoring_audio_history");
    assert.equal(resumed.next_offset, partial.next_offset);
    await upload(old, "monitoring_audio_history", resumed.next_offset);
    assert(!(await end("monitor_one", 3, true)).ok);
    assert((await end("monitor_one", 2, true)).ok);
    assert((await end("monitor_one", 2, true)).ok);
    for (const session of ["meeting_one", "meeting_two"]) {
      const part = meta(session, 0, false);
      assert((await begin(part, "meeting_audio")).ok);
      await upload(part, "meeting_audio");
      assert((await end(session, 1, false)).ok);
    }
    const bad = await begin(live, "monitoring_audio_history", { capture_end_ms: 9999 });
    assert(!bad.ok);
    const abandoned = meta("meeting_partial", 0, false);
    assert((await begin(abandoned, "meeting_audio")).ok);
    assert((await end("meeting_partial", 1, false, false)).ok);
    assert(!(await begin(abandoned, "meeting_audio")).ok); // Missing data cannot mutate a finalized scene.
    assert(!(await end("meeting_partial", 1, false, true)).ok);
    const index = JSON.parse(fs.readFileSync(path.join(dir, "sessions", `${device}__monitor_one`, "session.json")));
    assert.equal(index.audioDurationMs, 1920);
    assert.deepEqual(index.segments.map(value => value.index), [0, 1]);
    assert.deepEqual(index.segments.map(value => value.type), ["history", "realtime"]);
    assert(!receivedTypes.includes("hello"));
    // Receiver restart retains immutable metadata and cross-type deduplication.
    const restarted = new ReplayReceiver(dir);
    const ack = restarted.begin(device, { ...live.fields, type: "monitoring_audio_history_begin", request_id: "restart" });
    assert.equal(ack.next_offset, live.data.length);
    assert.throws(() => restarted.begin(device, { ...live.fields, segment_id: "another-file", type: "monitoring_audio_history_begin", request_id: "conflict" }), /already belongs/);
    console.log("PASS: three wire types, no hello, live preemption, durable resume, lost ACK cross-type dedupe, multiple meetings, whole-session validation and restart");
  } finally {
    await close(); await new Promise(resolve => server.close(resolve));
    assert.equal(path.dirname(dir), path.resolve(os.tmpdir()));
    assert(path.basename(dir).startsWith("nuwax-sd-sessions-"));
    fs.rmSync(dir, { recursive: true, force: true });
  }
}
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
module.exports = { fixture, crc32 };
