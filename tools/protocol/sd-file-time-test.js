"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), path = require("node:path"), os = require("node:os");
const { ReplayReceiver, archiveCrc } = require("./offline-audio-replay-gateway");
const { packet } = require("./sd-scene-protocol-test");
const { fixture } = require("./sd-audio-sessions-test");
const { scanLibrary, renderWav } = require("./sd-audio-library");
const { fileTime } = require("./sd-file-time");

async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-file-time-gateway-"));
  let receiver = new ReplayReceiver(dir), serial = 0;
  const device = "FILE-TIME-TEST", scenes = new Map();
  const control = (type, id, extra = {}) => ({ type, request_id: `time_${++serial}`, audio_session_id: id, ...extra });
  try {
    const samples = process.argv[2] ? fs.readdirSync(process.argv[2]).filter(n => n.endsWith(".nwx"))
      .map(n => fs.readFileSync(path.join(process.argv[2], n))) : [];
    const legacy = fixture("time_legacy", 0, false, 0, 2); samples.push(legacy);
    for (const data of samples) {
      const id = data.toString("ascii", 36, 84).replace(/\0.*$/, ""), index = data.readUInt32LE(24);
      const mode = data[12] ? "monitoring" : "meeting", prefix = `${mode}_audio`;
      if (!scenes.has(id)) {
        assert(receiver.scenes.begin(device, control(`${prefix}_begin`, id), 1).ok);
        scenes.set(id, { mode, count: 0, complete: true });
      }
      const expectedTime = fileTime(data), summary = archiveCrc(data);
      const commit = control(`${mode === "monitoring" ? "monitoring_audio_history" : prefix}_commit`, id,
        { segment_index: index, total_bytes: data.length, crc32: summary.crc });
      for (let offset = 0; offset < data.length; offset += 8064)
        assert.equal(receiver.scenes.chunk(device, packet(data, id, index, offset), 1), null);
      assert((await receiver.scenes.commit(device, commit, 1)).ok);
      const saved = receiver.paths(device, `sd_${id}_${index.toString(16).padStart(8, "0")}`).complete;
      assert.deepEqual(fs.readFileSync(saved), data);
      const meta = JSON.parse(fs.readFileSync(`${saved}.json`));
      for (const [key, value] of Object.entries(expectedTime)) assert.equal(meta[key], value, `${id}.${key}`);
      assert.equal(meta.audioDurationMs, summary.frameCount * 20);
      // Duplicate commit must not modify header time or repeat PCM publication.
      assert((await receiver.scenes.commit(device, commit, 1)).ok);
      assert.deepEqual(fs.readFileSync(saved), data);
      const scene = scenes.get(id); scene.count++; scene.complete &&= summary.complete;
    }
    for (const [id, scene] of scenes) {
      assert((await receiver.scenes.end(device, control(`${scene.mode}_audio_session_end`, id,
        { segment_count: scene.count, recording_complete: scene.complete }), 1)).ok);
    }
    const library = scanLibrary(dir); assert.deepEqual(library.issues, []);
    for (const scene of library.sessions) {
      for (const record of scene.records) {
        const header = fs.readFileSync(path.join(dir, record.file));
        for (const [key, value] of Object.entries(fileTime(header))) assert.equal(record[key], value);
      }
    }
    const gap = library.sessions.find(s => s.sessionId === "time_gap");
    if (gap) {
      assert.equal(gap.records[0].fileDurationMs, 120040); assert.equal(gap.durationMs, 40);
      const wav = renderWav(dir, gap.records, path.join(dir, "gap.wav"));
      assert.equal(wav.durationMs, 40); // Playback is still unmodified saved PCM.
    }
    // Unknown old headers and bad metadata are distinguished from valid UTC.
    assert.deepEqual(fileTime(legacy), { fileStartedAtMs: null, fileDurationMs: null, fileEndedAtMs: null });
    const bad = Buffer.from(legacy); bad.writeBigUInt64LE(1n, 84);
    assert.throws(() => fileTime(bad), /UTC/);
    bad.writeBigUInt64LE(1800000000000n, 84); bad.writeUInt32LE(999, 92);
    assert.throws(() => fileTime(bad, 40), /duration/);
    // The real receiver must reject an incorrect span before durable commit ACK.
    const invalid = fixture("time_invalid", 0, false, 0, 2);
    invalid.writeBigUInt64LE(1800000000000n, 84); invalid.writeUInt32LE(999, 92);
    assert(receiver.scenes.begin(device, control("meeting_audio_begin", "time_invalid"), 1).ok);
    receiver.scenes.chunk(device, packet(invalid, "time_invalid", 0), 1);
    assert.throws(() => receiver.scenes.commit(device, control("meeting_audio_commit", "time_invalid",
      { segment_index: 0, total_bytes: invalid.length, crc32: archiveCrc(invalid).crc }), 1), /duration/);
    await receiver.audioSessions.close(); receiver = new ReplayReceiver(dir);
    assert.deepEqual(scanLibrary(dir).sessions.filter(s => s.sessionId !== "time_invalid").map(s => s.records),
      library.sessions.map(s => s.records));
    console.log(`PASS: ${samples.length} device/legacy archives through begin, binary, commit, session_end; exact bytes/CRC, immutable retries, file UTC/span, playback, restart and invalid-time rejection`);
  } finally { await receiver.audioSessions.close(); fs.rmSync(dir, { recursive: true, force: true }); }
}
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
