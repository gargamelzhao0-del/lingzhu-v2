"use strict";
const assert = require("node:assert/strict"), fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { once } = require("node:events"), { performance } = require("node:perf_hooks");
const { fixture } = require("./sd-audio-sessions-test");
const { startServer, parseArgs, archiveCrc, ReplayReceiver } = require("./offline-audio-replay-gateway");
const compact = process.argv.includes("--compact");
const compactTransactions = new WeakSet();
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
function packets(data, start, end, request, version = 2) {
  const result = [];
  for (let at = start; at < end;) {
    const count = Math.min(8064, end - at), value = Buffer.alloc(64 + count);
    value.write("NWXCH1"); value.writeUInt32LE(version, 8); value.writeUInt32LE(at, 12); value.writeUInt32LE(count, 16);
    value.write(request, 20); if (version === 2) value.writeUInt32LE(at + count === end ? 1 : 0, 60);
    data.copy(value, 64, at, at + count); result.push(value); at += count;
  }
  return result;
}
function part(session, index = 0, monitor = false, count = 1574, start = index * count * 20, firstSequence = index * count) {
  const data = fixture(session, index, monitor, start, count, firstSequence);
  return { data, fields: { audio_session_id: session, segment_index: index, segment_id: `sd_${session}_${index}`,
    total_bytes: data.length, crc32: archiveCrc(data).crc, capture_start_ms: start,
    capture_end_ms: start + count * 20 } };
}
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-session-window-"));
  let server, ws, serial = 0;
  const device = "HOST-WINDOW-TEST", results = [], types = [];
  async function connect() {
    ws = new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws?device_id=${device}`);
    await once(ws, "open");
    ws.addEventListener("message", event => types.push(JSON.parse(event.data).type));
  }
  async function close() { if (ws) { const done = once(ws, "close"); ws.close(); await done; ws = null; } }
  async function runServer(directory, delay = 0) {
    const args = parseArgs(["node", "gateway", "--host", "127.0.0.1", "--storage-dir", directory]);
    args.port = 0; args.delayAckMs = delay; server = startServer(args); await once(server, "listening"); await connect();
  }
  async function stopServer() { await close(); await new Promise(resolve => server.close(resolve)); server = null; }
  function exchange(values, type, request) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { ws.removeEventListener("message", receive); reject(new Error(`timeout ${type}`)); }, 6000);
      function receive(event) {
        const value = JSON.parse(event.data); if (value.type !== type || value.request_id !== request) return;
        clearTimeout(timer); ws.removeEventListener("message", receive); resolve(value);
      }
      ws.addEventListener("message", receive);
      for (const value of Array.isArray(values) ? values : [values]) ws.send(Buffer.isBuffer(value) ? value : JSON.stringify(value));
    });
  }
  async function begin(p, route, windowed = true) {
    const request = `b${++serial}`;
    const value = await exchange({ ...p.fields, type: `${route}_begin`, request_id: request,
      ...(route === "monitoring_audio_realtime" ? { capture_age_ms: 0 } : {}),
      ...(windowed ? { ...(compact ? {} : { replay_version: 2, replay_revision: 2 }), window_bytes: 524288 } : {}) }, `${route}_begin_ack`, request);
    assert(value.ok); assert.equal(value.replay_version, windowed && !compact ? 2 : undefined);
    if (windowed && compact) compactTransactions.add(p); else compactTransactions.delete(p);
    return value.next_offset;
  }
  async function window(p, route, start, end, request = `w${++serial}`, version = 2) {
    const value = await exchange(packets(p.data, start, end, request, version), `${route}_chunk_ack`, request);
    assert(value.ok); assert.equal(value.segment_id, p.fields.segment_id); assert.equal(value.next_offset, end);
  }
  async function upload(p, route, start = 0, windowed = true, readKiBps = 0) {
    for (let at = start; at < p.data.length;) {
      const end = Math.min(p.data.length, at + (windowed ? 131072 : 8064));
      // Conservative source model: even serializing 1 MiB/s reads with ACK
      // waits must exceed 500 KiB/s. This is NOT an ESP32 SD/Wi-Fi benchmark.
      if (readKiBps) await pause((end - at) * 1000 / (readKiBps * 1024));
      await window(p, route, at, end, undefined, windowed ? 2 : 1); at = end;
    }
    const request = `c${++serial}`;
    const ack = await exchange({ ...(compactTransactions.has(p) ? { segment_id: p.fields.segment_id } : p.fields), type: `${route}_commit`, request_id: request }, `${route}_commit_ack`, request);
    assert(ack.ok);
  }
  async function end(p, monitor, count) {
    const type = `${monitor ? "monitoring" : "meeting"}_audio_session_end`, request = `e${++serial}`;
    const ack = await exchange({ type, request_id: request, audio_session_id: p.fields.audio_session_id,
      segment_count: count, recording_complete: true }, `${type}_ack`, request); assert(ack.ok);
  }
  try {
    const dir = path.join(root, "faults"); await runServer(dir);
    const history = part("monitor_windows", 0, true), live = part("monitor_windows", 1, true, 48, 32000, 1574);
    assert.equal(await begin(history, "monitoring_audio_history"), 0);
    await window(history, "monitoring_audio_history", 0, 131072, "lost_ack");
    await window(history, "monitoring_audio_history", 0, 131072, "lost_ack"); // durable ACK lost/retry
    assert.equal(await begin(live, "monitoring_audio_realtime", false), 0); // live preemption at ACK boundary
    await upload(live, "monitoring_audio_realtime", 0, false);
    await begin(live, "monitoring_audio_history"); await upload(live, "monitoring_audio_history", live.data.length);
    assert.equal(await begin(history, "monitoring_audio_history"), 131072);
    ws.send(packets(history.data, 131072, 262144, "unconfirmed")[0]);
    await pause(20); await stopServer(); await runServer(dir); // discard unacknowledged RAM window
    assert.equal(await begin(history, "monitoring_audio_history"), 131072);
    await upload(history, "monitoring_audio_history", 131072); await end(history, true, 2);
    assert.deepEqual(fs.readFileSync(path.join(dir, `${device}__${history.fields.segment_id}.complete`)), history.data);
    const rx = new ReplayReceiver(dir);
    assert.equal(rx.begin(device, { ...history.fields, type: "monitoring_audio_history_begin", request_id: "restart" }).next_offset, history.data.length);
    const monitor = JSON.parse(fs.readFileSync(path.join(dir, "sessions", `${device}__monitor_windows`, "session.json")));
    assert.deepEqual(monitor.segments.map(s => s.type), ["history", "realtime"]);
    // Device may lose its bulk allocation race after begin. V1 fallback is
    // allowed only before its first data packet; retain the same transaction.
    const fallback = part("meeting_low_memory", 0, false, 48);
    await begin(fallback, "meeting_audio");
    await upload(fallback, "meeting_audio", 0, false); await end(fallback, false, 1);
    assert.deepEqual(fs.readFileSync(path.join(dir, `${device}__${fallback.fields.segment_id}.complete`)), fallback.data);
    await stopServer();
    for (const version of [1, 2]) {
      const directory = path.join(root, `speed-v${version}`); await runServer(directory, 50);
      const parts = [part(`meeting_v${version}`, 0), part(`meeting_v${version}`, 1)];
      const start = performance.now();
      for (const p of parts) { await begin(p, "meeting_audio", version === 2); await upload(p, "meeting_audio", 0, version === 2, 1024); }
      await end(parts[0], false, parts.length);
      const seconds = (performance.now() - start) / 1000, bytes = parts.reduce((n, p) => n + p.data.length, 0);
      const result = { version, ackDelayMs: 50, modeledReadKiBps: 1024, bytes, seconds, KiBps: bytes / 1024 / seconds, hardware: false };
      results.push(result);
      for (const p of parts) assert.deepEqual(fs.readFileSync(path.join(directory, `${device}__${p.fields.segment_id}.complete`)), p.data);
      if (version === 2) assert(result.KiBps >= 500, `host protocol throughput below target: ${result.KiBps}`);
      await stopServer();
    }
    assert(!types.includes("hello"));
    console.log(JSON.stringify({ results, speedup: results[1].KiBps / results[0].KiBps }, null, 2));
    console.log("PASS: typed v2 ACKs, live preemption, lost ACK dedupe, cross-type metadata, restart/durable prefix, exact archive bytes, no hello, modeled 500 KiB/s gate");
  } finally {
    if (server) await stopServer();
    assert.equal(path.dirname(root), path.resolve(os.tmpdir())); assert(path.basename(root).startsWith("nuwax-session-window-"));
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
