"use strict";
// Real TCP coalescing/fragmentation against an isolated production gateway.
// Host compatibility test only: this is not an ESP32 throughput benchmark.
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const net = require("node:net"), crypto = require("node:crypto");
const { once } = require("node:events");
const { archive } = require("./offline-audio-stability-test");
const { startServer, parseArgs, archiveCrc } = require("./offline-audio-replay-gateway");

function frame(opcode, payload) {
  const head = Buffer.alloc(payload.length < 126 ? 6 : 8);
  head[0] = 0x80 | opcode;
  if (payload.length < 126) head[1] = 0x80 | payload.length;
  else { head[1] = 0xfe; head.writeUInt16BE(payload.length, 2); }
  const mask = crypto.randomBytes(4); mask.copy(head, head.length - 4);
  const data = Buffer.from(payload);
  for (let i = 0; i < data.length; i++) data[i] ^= mask[i & 3];
  return Buffer.concat([head, data]);
}
function windowFrames(data, start, end) {
  const result = [];
  for (let at = start; at < end;) {
    const n = Math.min(8064, end - at), header = Buffer.alloc(64);
    header.write("NWXCH1"); header.writeUInt32LE(2, 8);
    header.writeUInt32LE(at, 12); header.writeUInt32LE(n, 16);
    header.write(`window_${start}`, 20); header.writeUInt32LE(at + n === end ? 1 : 0, 60);
    result.push(frame(2, Buffer.concat([header, data.subarray(at, at + n)]))); at += n;
  }
  return result;
}
async function connect(port) {
  const socket = net.createConnection({ host: "127.0.0.1", port });
  let buffer = Buffer.alloc(0), upgraded = false, waiter;
  const pending = [];
  function deliver(message) { if (waiter) { const done = waiter; waiter = null; done(message); } else pending.push(message); }
  socket.on("data", data => {
    buffer = Buffer.concat([buffer, data]);
    if (!upgraded) {
      const end = buffer.indexOf("\r\n\r\n"); if (end < 0) return;
      assert.match(buffer.subarray(0, end).toString(), /^HTTP\/1\.1 101 /);
      buffer = buffer.subarray(end + 4); upgraded = true; deliver({ type: "upgrade" });
    }
    while (buffer.length >= 2) {
      assert.equal(buffer[0], 0x81); assert.equal(buffer[1] & 0x80, 0);
      let n = buffer[1] & 127, head = 2;
      if (n === 126) { if (buffer.length < 4) return; n = buffer.readUInt16BE(2); head = 4; }
      else assert(n < 126);
      if (buffer.length < head + n) return;
      deliver(JSON.parse(buffer.subarray(head, head + n).toString()));
      buffer = buffer.subarray(head + n);
    }
  });
  function next(type) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { waiter = null; reject(new Error(`timeout ${type}`)); }, 5000);
      function accept(message) { clearTimeout(timer); assert.equal(message.type, type); resolve(message); }
      if (pending.length) accept(pending.shift()); else waiter = accept;
    });
  }
  async function json(message, type) {
    const reply = next(type); socket.write(frame(1, Buffer.from(JSON.stringify(message)))); return reply;
  }
  await once(socket, "connect");
  const upgradedReply = next("upgrade");
  socket.write("GET /api/device/ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + crypto.randomBytes(16).toString("base64") + "\r\n\r\n");
  await upgradedReply; await json({ type: "hello", device_id: "batch-device" }, "hello");
  return { socket, next, json, async close() { const closed = once(socket, "close"); socket.destroy(); await closed; } };
}
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-batch-"));
  const args = parseArgs(["node", "test", "--host", "127.0.0.1", "--storage-dir", root]);
  args.port = 0;
  const server = startServer(args); await once(server, "listening");
  let client;
  try {
    client = await connect(server.address().port);
    const data = archive(0, 500);
    const begin = { type: "offline_audio_begin", request_id: "begin", segment_id: "batch",
      total_bytes: data.length, crc32: archiveCrc(data).crc, replay_version: 2, replay_revision: 2, window_bytes: 131072 };
    assert.equal((await client.json(begin, "offline_audio_begin_ack")).next_offset, 0);
    // Cut TCP after a complete frame and only part of the next frame.
    const first = windowFrames(data, 0, 131072);
    client.socket.write(Buffer.concat(first.slice(0, 4)).subarray(0, first[0].length + 17));
    await new Promise(resolve => setTimeout(resolve, 20));
    await client.close(); client = await connect(server.address().port);
    assert.equal((await client.json(begin, "offline_audio_begin_ack")).next_offset, 0);
    let writes = 0, logicalFrames = 0;
    async function sendWindow(start, end, fragment) {
      const frames = windowFrames(data, start, end), reply = client.next("offline_audio_chunk_ack");
      for (let i = 0; i < frames.length; i += 4) {
        const wire = Buffer.concat(frames.slice(i, i + 4)); logicalFrames += Math.min(4, frames.length - i);
        if (fragment && i === 0) {
          // Split header and payload while retaining coalesced later frames.
          for (const part of [wire.subarray(0, 3), wire.subarray(3, 8139), wire.subarray(8139)]) {
            client.socket.write(part); writes++; await new Promise(resolve => setImmediate(resolve));
          }
        } else { client.socket.write(wire); writes++; }
      }
      const ack = await reply; assert(ack.ok); assert.equal(ack.next_offset, end);
    }
    await sendWindow(0, 131072, false);
    await sendWindow(0, 131072, true); // Same durable window after a lost ACK.
    for (let at = 131072; at < data.length; at += 131072)
      await sendWindow(at, Math.min(at + 131072, data.length), true);
    const commit = await client.json({ ...begin, type: "offline_audio_commit", request_id: "commit" }, "offline_audio_commit_ack");
    assert(commit.ok);
    assert(fs.readFileSync(path.join(root, "batch-device__batch.complete")).equals(data));
    assert.equal((await client.json(begin, "offline_audio_begin_ack")).next_offset, data.length);
    assert(writes < logicalFrames);
    console.log(JSON.stringify({ scope: "isolated host TCP compatibility; no hardware throughput claim", bytes: data.length, logicalFrames, writes }));
    console.log("PASS: four masked WS frames per TCP write; fragmented headers/payloads; interrupted batch resumes; duplicate durable window; commit and byte-for-byte archive equality");
  } finally {
    if (client) await client.close();
    await new Promise(resolve => server.close(resolve));
    assert(path.dirname(root) === os.tmpdir() && path.basename(root).startsWith("nuwax-batch-"));
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
