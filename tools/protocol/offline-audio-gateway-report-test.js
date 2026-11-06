"use strict";
const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const { once } = require("events");
const { archiveCrc, startServer, makeArchive, makeChunk, parseArgs } =
  require("./offline-audio-replay-gateway");

function updateCrc(crc, data) {
  let v = (~crc) >>> 0;
  for (const byte of data) {
    v ^= byte;
    for (let bit = 0; bit < 8; bit++) v = (v >>> 1) ^ ((v & 1) ? 0xedb88320 : 0);
  }
  return (~v) >>> 0;
}
async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-report-test-"));
  const reportFile = path.join(dir, "report.json");
  const args = parseArgs(["node", "gateway", "--host", "127.0.0.1",
    "--storage-dir", dir, "--report", reportFile]);
  args.port = 0; // OS-assigned ephemeral port for the integration test only.
  const server = startServer(args);
  let ws;
  try {
    await once(server, "listening");
    ws = new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws`);
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
    const exchange = (payload, type) => new Promise((resolve, reject) => {
      const timeout = setTimeout(() => { ws.removeEventListener("message", receive); reject(new Error(`timeout ${type}`)); }, 3000);
      function receive(event) {
        const message = JSON.parse(event.data);
        if (message.type !== type) return;
        clearTimeout(timeout);
        ws.removeEventListener("message", receive);
        resolve(message);
      }
      ws.addEventListener("message", receive);
      ws.send(typeof payload === "object" && !Buffer.isBuffer(payload) ? JSON.stringify(payload) : payload);
    });
    await exchange({ type: "hello", device_id: "report-test", version: 1 }, "hello");
    const archive = makeArchive();
    let p = 96, crc = 0, index = 0;
    const sequences = [0, 1, 4, 5, 6, 7];
    while (p < archive.length - 32) {
      const n = archive.readUInt32LE(p);
      const payload = archive.subarray(p + 4, p + 4 + n);
      payload.write("NA", 0, "ascii");
      payload.writeUInt32LE(sequences[index++], 10);
      crc = updateCrc(crc, payload);
      p += 4 + n;
    }
    archive.writeUInt32LE(crc, archive.length - 8);
    assert.equal(archiveCrc(archive).missingFrames, 2);
    const meta = { segment_id: "gap-test", total_bytes: archive.length, crc32: crc };
    const begin = await exchange({ type: "offline_audio_begin", request_id: "b", ...meta }, "offline_audio_begin_ack");
    assert.equal(begin.next_offset, 0);
    let offset = 0;
    while (offset < archive.length) {
      const ack = await exchange(makeChunk(archive, offset, `c${offset}`, 1024), "offline_audio_chunk_ack");
      assert(ack.ok);
      offset = ack.next_offset;
      if (offset === 1024) {
        await new Promise(resolve => setTimeout(resolve, 1100));
        assert(await args.flushReport());
        const live = JSON.parse(fs.readFileSync(reportFile));
        assert.equal(live.pending[0].nextOffset, 1024);
        assert(live.runtime.eventLoopMaxMs > 0);
        assert(live.runtime.recentUploadIntervals.some(item => item.receivedBytes === 1024));
        const status = await (await fetch(`http://127.0.0.1:${server.address().port}/api/status`)).json();
        assert(status.runtime.recentUploadIntervals.length > 0);
      }
    }
    const commit = await exchange({ type: "offline_audio_commit", request_id: "end", ...meta }, "offline_audio_commit_ack");
    assert(commit.ok);
    assert(await args.flushReport());
    const report = JSON.parse(fs.readFileSync(reportFile));
    assert.equal(ws.readyState, WebSocket.OPEN);
    assert.equal(report.pending.length, 0);
    assert.equal(report.replay.archives[0].missingFrames, 2);
    assert.equal(report.replay.archives[0].committed, true);
    assert(report.replay.durableWriteMaxMs > 0);
    assert(report.replay.durableWriteMaxMs <= report.replay.durableWriteMs);
    console.log("PASS: live pending report, commit report before close, CRC-valid missing frames detected");
  } finally {
    if (ws) ws.close();
    await new Promise(resolve => server.close(resolve));
    await args.closeReport();
    fs.rmSync(dir, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
