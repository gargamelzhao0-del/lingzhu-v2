"use strict";
// Exercise the production gateway on an ephemeral loopback port. Never contacts
// the user's running gateway or reads/writes their audio storage.
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const net = require("node:net"), crypto = require("node:crypto");
const { once } = require("node:events");
const { startServer, parseArgs, archiveCrc } = require("./offline-audio-replay-gateway");
const { archive } = require("./offline-audio-stability-test");

function frame(opcode, payload) {
  payload = Buffer.from(payload);
  const head = Buffer.alloc(payload.length < 126 ? 6 : 8);
  head[0] = 0x80 | opcode;
  if (payload.length < 126) head[1] = 0x80 | payload.length;
  else { head[1] = 0xfe; head.writeUInt16BE(payload.length, 2); }
  const mask = crypto.randomBytes(4); mask.copy(head, head.length - 4);
  const data = Buffer.from(payload);
  for (let i = 0; i < data.length; i++) data[i] ^= mask[i & 3];
  return Buffer.concat([head, data]);
}
const textFrame = message => frame(1, JSON.stringify(message));
function chunk(data, offset, length, requestId, final = true) {
  const head = Buffer.alloc(64);
  head.write("NWXCH1"); head.writeUInt32LE(2, 8);
  head.writeUInt32LE(offset, 12); head.writeUInt32LE(length, 16);
  head.write(requestId, 20); head.writeUInt32LE(final ? 1 : 0, 60);
  return frame(2, Buffer.concat([head, data.subarray(offset, offset + length)]));
}
function bounded(promise, label, ms = 3000) {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`timeout: ${label}`)), ms);
  })]).finally(() => clearTimeout(timer));
}

async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-connection-lifecycle-"));
  const args = parseArgs(["node", "gateway", "--host", "127.0.0.1", "--storage-dir", root,
    "--report", path.join(root, "report.json")]);
  args.port = 0;
  const server = startServer(args), clients = new Set(), peers = new Map();
  server.on("upgrade", (_, socket) => peers.set(socket.remotePort, socket));
  await once(server, "listening");
  const port = server.address().port;
  async function connect(deviceId) {
    const socket = net.connect({ host: "127.0.0.1", port, allowHalfOpen: true });
    const client = { socket, messages: [], errors: [], wire: [], waiters: [] };
    clients.add(client);
    client.closed = new Promise(resolve => socket.once("close", resolve));
    socket.on("error", error => client.errors.push(error));
    const deliver = value => {
      client.messages.push(value);
      const index = client.waiters.findIndex(waiter => waiter.type === value.type);
      if (index >= 0) client.waiters.splice(index, 1)[0].resolve(value);
      else client.wire.push(value);
    };
    let buffer = Buffer.alloc(0), upgraded = false;
    socket.on("data", data => {
      buffer = Buffer.concat([buffer, data]);
      if (!upgraded) {
        const end = buffer.indexOf("\r\n\r\n"); if (end < 0) return;
        assert.match(buffer.subarray(0, end).toString(), /^HTTP\/1\.1 101 /);
        buffer = buffer.subarray(end + 4); upgraded = true; deliver({ type: "upgrade" });
      }
      while (buffer.length >= 2) {
        const opcode = buffer[0] & 15;
        assert.equal(buffer[1] & 128, 0);
        let length = buffer[1] & 127, head = 2;
        if (length === 126) { if (buffer.length < 4) return; length = buffer.readUInt16BE(2); head = 4; }
        else assert(length < 126);
        if (buffer.length < head + length) return;
        const body = buffer.subarray(head, head + length);
        buffer = buffer.subarray(head + length);
        deliver(opcode === 8 ? { type: "ws_close" } : JSON.parse(body.toString()));
      }
    });
    client.next = type => {
      const index = client.wire.findIndex(value => value.type === type);
      if (index >= 0) return Promise.resolve(client.wire.splice(index, 1)[0]);
      let waiter;
      const promise = new Promise(resolve => { waiter = { type, resolve }; client.waiters.push(waiter); });
      return bounded(promise, type).finally(() => {
        const index = client.waiters.indexOf(waiter); if (index >= 0) client.waiters.splice(index, 1);
      });
    };
    client.send = wire => socket.write(wire, () => {});
    client.json = (message, type) => { const reply = client.next(type); client.send(textFrame(message)); return reply; };
    client.binary = (wire, type = "offline_audio_chunk_ack") => { const reply = client.next(type); client.send(wire); return reply; };
    client.finish = async () => {
      if (!socket.destroyed) socket.end();
      // A raw FIN must close the SERVER socket without relying on this peer to
      // destroy it. Then dispose the test client's allowHalfOpen write side.
      const peer = peers.get(socket.localPort);
      if (peer && !peer.destroyed) await bounded(once(peer, "close"), "peer FIN cleanup");
      socket.destroy(); await bounded(client.closed, "client cleanup");
    };
    await once(socket, "connect");
    const ready = client.next("upgrade");
    socket.write(`GET /api/device/ws${deviceId ? `?device_id=${deviceId}` : ""} HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString("base64")}\r\n\r\n`);
    await ready;
    client.peer = peers.get(socket.localPort);
    return client;
  }
  const status = async () => (await fetch(`http://127.0.0.1:${port}/api/status`)).json();
  const report = async () => { assert(await args.flushReport()); return JSON.parse(fs.readFileSync(args.reportFile, "utf8")); };
  const data = archive(0, 60), crc = archiveCrc(data).crc;
  const metadata = segment => ({ segment_id: segment, total_bytes: data.length, crc32: crc,
    replay_version: 2, replay_revision: 2, window_bytes: 524288 });
  const begin = (client, segment, request) => client.json({ type: "offline_audio_begin",
    request_id: request, ...metadata(segment) }, "offline_audio_begin_ack");
  async function commitRest(client, segment, offset) {
    while (offset < data.length) {
      const count = Math.min(8064, data.length - offset);
      const ack = await client.binary(chunk(data, offset, count, `resume_${offset}`));
      assert(ack.ok); assert.equal(ack.next_offset, offset + count); offset += count;
    }
    const ack = await client.json({ type: "offline_audio_commit", request_id: "commit",
      ...metadata(segment) }, "offline_audio_commit_ack");
    assert(ack.ok);
  }
  try {
    // FIN discards only an unacknowledged RAM window. Resume starts at the last
    // fsynced prefix, and duplicate end/close/error callbacks are harmless.
    const first = await connect("fin-device");
    assert.equal((await begin(first, "fin-segment", "begin_fin")).next_offset, 0);
    assert.equal((await first.binary(chunk(data, 0, 1024, "durable"))).next_offset, 1024);
    first.send(chunk(data, 1024, 2048, "unfinished", false));
    await first.json({ type: "ping" }, "pong");
    await first.finish();
    first.peer.emit("end"); first.peer.emit("close");
    first.peer.emit("error", new Error("test duplicate error after FIN"));
    let current = await status();
    assert(!current.devices.includes("fin-device"));
    assert.equal(current.pending.find(value => value.deviceId === "fin-device").connected, false);
    assert.equal(fs.statSync(path.join(root, "fin-device__fin-segment.part")).size, 1024);
    assert.equal((await report()).sessions.filter(value => value.deviceId === "fin-device").length, 1);
    const resumed = await connect("fin-device");
    assert.equal((await begin(resumed, "fin-segment", "begin_resume")).next_offset, 1024);
    await commitRest(resumed, "fin-segment", 1024);
    assert.deepEqual(fs.readFileSync(path.join(root, "fin-device__fin-segment.complete")), data);
    await resumed.finish();

    // A new identified connection retires the older one before begin. Exercise
    // actual sockets plus deterministic already-queued data/close callbacks on
    // the retired socket; neither can replace or clear the successor's state.
    const old = await connect("takeover-device");
    await begin(old, "takeover", "old_begin");
    await old.binary(chunk(data, 0, 1024, "old_durable"));
    old.send(chunk(data, 1024, 2048, "old_pending", false));
    await old.json({ type: "ping" }, "pong");
    const lateBegin = textFrame({ type: "offline_audio_begin", request_id: "late_begin", ...metadata("wrong-segment") });
    const partialReceived = once(old.peer, "data"); old.send(lateBegin.subarray(0, 11)); await partialReceived;
    const fresh = await connect("takeover-device");
    assert(old.peer.destroyed, "takeover must close old server socket immediately");
    assert.equal((await begin(fresh, "takeover", "new_begin")).next_offset, 1024);
    // Queue a new owner's incomplete RAM window before stale callbacks fire.
    fresh.send(chunk(data, 1024, 2048, "new_pending", false));
    await fresh.json({ type: "ping" }, "pong");
    old.send(lateBegin.subarray(11));
    old.peer.emit("data", lateBegin);
    old.peer.emit("data", chunk(data, 1024, 512, "stale_chunk"));
    old.peer.emit("close"); old.peer.emit("end");
    const newAck = await fresh.binary(chunk(data, 3072, 1024, "new_pending", true));
    assert.equal(newAck.next_offset, 4096, "stale close cleared the new RAM window");
    assert(!fs.existsSync(path.join(root, "takeover-device__wrong-segment.part")));
    await commitRest(fresh, "takeover", 4096);
    assert.deepEqual(fs.readFileSync(path.join(root, "takeover-device__takeover.complete")), data);
    await old.finish(); await fresh.finish();

    // Acceptance generation survives a newer connection closing while an
    // older anonymous socket is still able to announce its identity later.
    const anonymous = await connect();
    const newest = await connect("late-identity");
    await newest.finish();
    const anonymousClosed = once(anonymous.peer, "close");
    anonymous.send(Buffer.concat([textFrame({ type: "hello", device_id: "late-identity" }),
      textFrame({ type: "offline_audio_begin", request_id: "stale_hello_begin", ...metadata("stale-identity") })]));
    await bounded(anonymousClosed, "reject stale anonymous identity");
    assert(!anonymous.messages.some(value => value.type === "hello"));
    assert(!fs.existsSync(path.join(root, "late-identity__stale-identity.part")));
    assert(!((await status()).devices.includes("late-identity")));
    await anonymous.finish();

    const identified = await connect("hello-upgrade");
    const newerAnonymous = await connect();
    assert((await newerAnonymous.json({ type: "hello", device_id: "hello-upgrade" }, "hello")).ok);
    assert(identified.peer.destroyed);
    assert((await newerAnonymous.json({ type: "hello", device_id: "hello-upgrade" }, "hello")).ok);
    const changedClosed = once(newerAnonymous.peer, "close");
    newerAnonymous.send(textFrame({ type: "hello", device_id: "different-device" }));
    await bounded(changedClosed, "reject changed identity");
    await identified.finish(); await newerAnonymous.finish();

    // Error and WebSocket close share idempotent cleanup. Even a peer that
    // ignores our close reply cannot hold the upgraded socket indefinitely.
    const errored = await connect("error-device");
    // Use a close-only promise: events.once(close) rejects on the injected error.
    const errorDone = new Promise(resolve => errored.peer.once("close", resolve));
    errored.peer.emit("error", new Error("test live socket error"));
    await bounded(errorDone, "error cleanup"); await errored.finish();
    const closing = await connect("close-device");
    const closeDone = once(closing.peer, "close");
    closing.send(Buffer.concat([frame(8, Buffer.alloc(0)), frame(8, Buffer.alloc(0)),
      textFrame({ type: "offline_audio_begin", request_id: "after_close", ...metadata("after-close") })]));
    await closing.next("ws_close");
    await bounded(closeDone, "close handshake deadline", 2000);
    assert.equal(closing.messages.filter(value => value.type === "ws_close").length, 1);
    assert.equal((await report()).sessions.filter(value => value.deviceId === "close-device").length, 1);
    assert(!fs.existsSync(path.join(root, "close-device__after-close.part")));
    await closing.finish();

    // An anonymous connection cannot upload before identifying. Delayed ACKs
    // are cancelled at takeover instead of surviving on a retired connection.
    const unidentified = await connect();
    const unidentifiedDone = once(unidentified.peer, "close");
    unidentified.send(chunk(data, 0, 1024, "no_identity"));
    await bounded(unidentifiedDone, "reject unidentified binary");
    await unidentified.finish();
    args.delayAckMs = 100;
    const delayed = await connect("delayed-ack");
    const beginProcessed = once(delayed.peer, "data");
    delayed.send(textFrame({ type: "offline_audio_begin", request_id: "cancelled_ack", ...metadata("delayed") }));
    await beginProcessed;
    const replacement = await connect("delayed-ack");
    args.delayAckMs = 0;
    assert((await begin(replacement, "delayed", "replacement_ack")).ok);
    // Waiting past the old deadline verifies the delayed send cannot leak into
    // the successor or resurrect the old connection's dashboard status.
    await new Promise(resolve => setTimeout(resolve, 120));
    assert(!delayed.messages.some(value => value.request_id === "cancelled_ack"));
    assert(!replacement.messages.some(value => value.request_id === "cancelled_ack"));
    await delayed.finish(); await replacement.finish();

    current = await status();
    assert.deepEqual(current.devices, []);
    assert(current.pending.every(value => !value.connected));
    console.log("PASS: real-socket FIN/error/close cleanup, idempotent teardown, stale begin/chunk/close isolation, late/changing/missing identity rejection, legacy takeover, cancelled stale ACK, durable byte-identical resume, dashboard offline state");
  } finally {
    for (const client of clients) client.socket.destroy();
    for (const socket of peers.values()) socket.destroy();
    await new Promise(resolve => server.close(resolve));
    await args.closeReport();
    assert.equal(path.dirname(root), path.resolve(os.tmpdir()));
    assert(path.basename(root).startsWith("nuwax-connection-lifecycle-"));
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
