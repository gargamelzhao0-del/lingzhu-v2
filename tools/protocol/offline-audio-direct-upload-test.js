"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { once } = require("node:events");
const { archiveCrc, startServer, makeArchive, makeChunk, parseArgs } =
  require("./offline-audio-replay-gateway");

async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-direct-upload-"));
  const args = parseArgs(["node", "gateway", "--host", "127.0.0.1", "--storage-dir", dir]);
  args.port = 0;
  const server = startServer(args);
  const clients = new Set();
  const received = [];
  async function connect(id) {
    const ws = new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws?device_id=${id}`);
    clients.add(ws);
    ws.addEventListener("message", event => received.push(JSON.parse(event.data)));
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
    return ws;
  }
  function exchange(ws, payload, type) {
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        ws.removeEventListener("message", receive);
        reject(new Error(`timeout ${type}`));
      }, 3000);
      function receive(event) {
        const msg = JSON.parse(event.data);
        if (msg.type !== type) return;
        clearTimeout(timeout);
        ws.removeEventListener("message", receive);
        resolve(msg);
      }
      ws.addEventListener("message", receive);
      ws.send(Buffer.isBuffer(payload) ? payload : JSON.stringify(payload));
    });
  }
  async function close(ws) {
    await new Promise(resolve => { ws.onclose = resolve; ws.close(); });
    clients.delete(ws);
  }
  try {
    await once(server, "listening");
    const archive = makeArchive();
    const meta = { mode: "archive_only", segment_id: "same-segment",
      total_bytes: archive.length, crc32: archiveCrc(archive).crc };
    // The first application message is begin: neither side sends hello.
    let ws = await connect("NX-AABB-CCDD-EEFF");
    let ack = await exchange(ws, { type: "offline_audio_begin", request_id: "b1", ...meta }, "offline_audio_begin_ack");
    assert(ack.ok); assert.equal(ack.next_offset, 0);
    ack = await exchange(ws, makeChunk(archive, 0, "c1", 1024), "offline_audio_chunk_ack");
    assert(ack.ok); assert.equal(ack.next_offset, 1024);
    // Disconnect before commit, then resume under the same device/segment.
    await close(ws);
    ws = await connect("NX-AABB-CCDD-EEFF");
    ack = await exchange(ws, { type: "offline_audio_begin", request_id: "b2", ...meta }, "offline_audio_begin_ack");
    assert(ack.ok); assert.equal(ack.next_offset, 1024);
    let offset = ack.next_offset;
    // A different device using the same segment ID must start at zero.
    const other = await connect("NX-1122-3344-5566");
    const otherAck = await exchange(other, { type: "offline_audio_begin", request_id: "b3", ...meta }, "offline_audio_begin_ack");
    assert(otherAck.ok); assert.equal(otherAck.next_offset, 0);
    await close(other);
    while (offset < archive.length) {
      ack = await exchange(ws, makeChunk(archive, offset, `c${offset}`, 1024), "offline_audio_chunk_ack");
      assert(ack.ok); assert(ack.next_offset > offset);
      offset = ack.next_offset;
    }
    const commit = { type: "offline_audio_commit", request_id: "end", ...meta };
    ack = await exchange(ws, commit, "offline_audio_commit_ack");
    assert(ack.ok);
    await close(ws);
    ws = await connect("NX-AABB-CCDD-EEFF");
    ack = await exchange(ws, { type: "offline_audio_begin", request_id: "b4", ...meta }, "offline_audio_begin_ack");
    assert(ack.ok); assert.equal(ack.next_offset, archive.length);
    ack = await exchange(ws, { ...commit, request_id: "end2" }, "offline_audio_commit_ack");
    assert(ack.ok);
    const stored = path.join(dir, "NX-AABB-CCDD-EEFF__same-segment.complete");
    assert.deepEqual(fs.readFileSync(stored), archive);
    assert(!received.some(msg => msg.type === "hello"));
    console.log("PASS: no-hello begin/chunk/commit, durable resume, device isolation, repeated commit and byte-identical archive");
  } finally {
    await Promise.all([...clients].map(close));
    await new Promise(resolve => server.close(resolve));
    // This test owns only its freshly created OS temporary directory.
    assert(path.dirname(dir) === path.resolve(os.tmpdir()));
    assert(path.basename(dir).startsWith("nuwax-direct-upload-"));
    fs.rmSync(dir, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
