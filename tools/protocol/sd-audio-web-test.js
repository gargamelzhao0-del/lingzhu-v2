"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { once } = require("node:events");
const { fixture, crc32 } = require("./sd-audio-sessions-test");
const { ReplayReceiver, archiveCrc, makeChunk, startServer, parseArgs } = require("./offline-audio-replay-gateway");
const { scanLibrary, renderWav } = require("./sd-audio-library");
const { rangeOf } = require("./sd-audio-web");
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-audio-web-"));
  let server, ws;
  try {
    const receiver = new ReplayReceiver(root), device = "TEST-DEVICE";
    function upload(session, index, mode = "meeting", start = index * 960) {
      const data = fixture(session, index, mode !== "meeting", start);
      const route = mode === "meeting" ? "meeting_audio" : `monitoring_audio_${mode}`;
      const fields = { request_id: `b${index}`, segment_id: `sd_${session}_${index}`, total_bytes: data.length,
        crc32: archiveCrc(data).crc, audio_session_id: session, segment_index: index,
        capture_start_ms: start, capture_end_ms: start + 960, ...(mode === "realtime" ? { capture_age_ms: 0 } : {}) };
      receiver.begin(device, { type: `${route}_begin`, ...fields });
      for (let offset = 0; offset < data.length;) offset = receiver.chunk(device, makeChunk(data, offset, `c${offset}`)).next_offset;
      receiver.commit(device, { type: `${route}_commit`, ...fields });
      return data;
    }
    const second = upload("meeting_a", 1), first = upload("meeting_a", 0);
    receiver.audioSessions.end(device, { type: "meeting_audio_session_end", request_id: "end",
      audio_session_id: "meeting_a", segment_count: 2, recording_complete: true });
    upload("monitor_a", 1, "realtime", 2000); upload("monitor_a", 0, "history", 0);
    // Legacy firmware is visible, but its missing session_end never becomes a complete scene.
    const old = fixture("old_scene", 0, false), oldPayloads = [];
    for (let i = 0; i < 48; i++) {
      old.writeUInt32LE(1000 + i * 20 + (i % 2 ? 4 : 0), 114 + i * 666);
      oldPayloads.push(old.subarray(100 + i * 666, 762 + i * 666));
    }
    old.writeUInt32LE(crc32(Buffer.concat(oldPayloads)), old.length - 8);
    fs.writeFileSync(path.join(root, "OLD__old_file.complete"), old);
    fs.writeFileSync(path.join(root, "OTHER__other.complete"), fixture("meeting_a", 0, false));
    fs.writeFileSync(path.join(root, "TEST__in_flight.part"), first.subarray(0, 100));
    const library = scanLibrary(root);
    assert.equal(library.sessions.length, 4);
    const scene = library.sessions.find(s => s.deviceId === device && s.sessionId === "meeting_a");
    assert.equal(scene.state, "complete"); assert.equal(scene.durationMs, 1920);
    assert.deepEqual(scene.records.map(r => r.index), [0, 1]);
    assert.equal(library.sessions.find(s => s.sessionId === "old_scene").state, "legacy");
    assert.equal(renderWav(root, library.sessions.find(s => s.sessionId === "old_scene").records,
      path.join(root, "legacy.wav")).discontinuities, 0);
    assert.equal(library.sessions.find(s => s.sessionId === "monitor_a").state, "receiving");
    const output = path.join(root, "direct.wav");
    const result = renderWav(root, scene.records, output);
    const wav = fs.readFileSync(output), expected = [];
    for (const data of [first, second]) for (let p = 96; p < data.length - 32; p += 666) expected.push(data.subarray(p + 26, p + 666));
    assert.equal(wav.toString("ascii", 0, 4), "RIFF"); assert.equal(wav.readUInt32LE(24), 16000);
    assert.equal(wav.readUInt32LE(40), 96 * 640); assert.deepEqual(wav.subarray(44), Buffer.concat(expected));
    assert.equal(result.durationMs, 1920); assert.equal(result.discontinuities, 0);
    const monitor = library.sessions.find(s => s.sessionId === "monitor_a");
    assert.equal(renderWav(root, monitor.records, path.join(root, "monitor.wav")).discontinuities, 1);
    // Bit corruption never produces a playable output or a surviving partial WAV.
    const broken = path.join(root, "BROKEN__bad.complete"), brokenData = fixture("broken", 0, false);
    brokenData[150] ^= 1; fs.writeFileSync(broken, brokenData);
    const bad = scanLibrary(root).sessions.find(s => s.sessionId === "broken");
    assert.throws(() => renderWav(root, bad.records, path.join(root, "bad.wav")), /CRC/);
    assert(!fs.existsSync(path.join(root, "bad.wav.tmp")));
    assert.equal(rangeOf("bytes=0-", 100).end, 99);
    assert.deepEqual(rangeOf("bytes=-20", 100), { start: 80, end: 99, partial: true });
    for (const range of ["bytes=100-", "bytes=-0", "bytes=9-1", "bytes=0-1,3-4", "bytes=999999999999999999999-"]) assert.equal(rangeOf(range, 100), null);
    const args = parseArgs(["node", "gateway", "--host", "127.0.0.1", "--storage-dir", root]); args.port = 0;
    server = startServer(args); await once(server, "listening");
    const base = `http://127.0.0.1:${server.address().port}`;
    const page = await fetch(base); assert.equal(page.status, 200); assert.match(await page.text(), /SD 音频工作台/);
    assert.match(page.headers.get("content-security-policy"), /frame-ancestors 'none'/);
    const status = await (await fetch(`${base}/api/status`)).json(); assert.equal(status.sessions.length, 5);
    assert(!JSON.stringify(status).includes(root));
    const detail = await (await fetch(`${base}/api/sessions/${scene.id}`)).json(); assert.equal(detail.records.length, 2);
    assert.equal((await fetch(`${base}/api/sessions/${scene.id}/prepare`, { method: "POST", headers: { Origin: "https://example.com" } })).status, 403);
    let job = await (await fetch(`${base}/api/sessions/${scene.id}/prepare`, { method: "POST" })).json();
    const deadline = Date.now() + 10000;
    while (job.state === "preparing") { assert(Date.now() < deadline); await new Promise(r => setTimeout(r, 50)); job = await (await fetch(`${base}/api/jobs/${job.id}`)).json(); }
    assert.equal(job.state, "ready");
    const response = await fetch(base + job.url); assert.equal(response.status, 200); assert.equal(response.headers.get("content-type"), "audio/wav");
    assert.deepEqual(Buffer.from(await response.arrayBuffer()), wav);
    const range = await fetch(base + job.url, { headers: { Range: "bytes=44-683" } });
    assert.equal(range.status, 206); assert.deepEqual(Buffer.from(await range.arrayBuffer()), wav.subarray(44, 684));
    assert.equal((await fetch(base + job.url, { headers: { Range: "bytes=999999-" } })).status, 416);
    assert.equal((await fetch(base + job.url, { method: "HEAD" })).headers.get("content-length"), String(wav.length));
    assert.equal((await fetch(`${base}/audio/%2e%2e%2fsecret.wav`)).status, 404);
    // WebSocket ingress and the web player share a port without requiring hello.
    ws = new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws?device_id=WEBTEST`);
    await once(ws, "open");
    const request = { type: "meeting_audio_begin", request_id: "web_b", segment_id: "sd_web_0", audio_session_id: "web",
      segment_index: 0, capture_start_ms: 0, capture_end_ms: 960, total_bytes: first.length, crc32: archiveCrc(first).crc };
    const ack = once(ws, "message"); ws.send(JSON.stringify(request));
    assert.equal(JSON.parse((await ack)[0].data).ok, true);
    assert((await (await fetch(`${base}/api/status`)).json()).devices.includes("WEBTEST"));
    const closed = once(ws, "close"); ws.close(); await closed; ws = null;
    await new Promise(resolve => server.close(resolve)); server = null;
    // Restart: use the same durable WAV cache, no conversion or device retransmission.
    server = startServer(args); await once(server, "listening");
    const restart = `http://127.0.0.1:${server.address().port}`;
    const cached = await (await fetch(`${restart}/api/sessions/${scene.id}/prepare`, { method: "POST" })).json();
    assert.equal(cached.state, "ready"); assert.equal(cached.url, job.url);
    console.log("PASS: device isolation, ordered exact PCM, classification, partial/legacy state, corruption rejection, HTTP + WebSocket, WAV range/HEAD/download, worker conversion and restart cache");
  } finally {
    if (ws) ws.close(); if (server) await new Promise(resolve => server.close(resolve));
    assert.equal(path.dirname(root), path.resolve(os.tmpdir())); assert(path.basename(root).startsWith("nuwax-audio-web-"));
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
