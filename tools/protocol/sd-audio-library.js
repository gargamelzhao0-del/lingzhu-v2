"use strict";
// Runs in workers: indexing and PCM extraction never occupy the ACK event loop.
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const { parentPort, workerData } = require("node:worker_threads");
const { archiveCrc } = require("./offline-audio-replay-gateway");
const { mergeSessionTime } = require("./sd-session-time");
const { fileTime } = require("./sd-file-time");
const digest = value => crypto.createHash("sha256").update(value).digest("hex");
const need = (condition, message) => { if (!condition) throw new Error(message); };
const cache = new Map();
function json(file) { return JSON.parse(fs.readFileSync(file, "utf8")); }
function inspect(root, name) {
  const file = path.join(root, name), stat = fs.statSync(file);
  need(stat.isFile() && stat.size >= 128 && stat.size <= 4 * 1024 * 1024, "录音文件大小不合法");
  const metaFile = `${file}.json`, metaStat = fs.existsSync(metaFile) ? fs.statSync(metaFile) : null;
  const stamp = `${stat.size}:${stat.mtimeMs}:${metaStat?.mtimeMs || 0}`;
  if (cache.get(file)?.stamp === stamp) return cache.get(file).record;
  const fd = fs.openSync(file, "r"), header = Buffer.alloc(96), footer = Buffer.alloc(32);
  try {
    need(fs.readSync(fd, header, 0, 96, 0) === 96 && fs.readSync(fd, footer, 0, 32, stat.size - 32) === 32, "录音头尾不完整");
  } finally { fs.closeSync(fd); }
  need(header.toString("ascii", 0, 7) === "NWXOFF1" && footer.toString("ascii", 0, 7) === "NWXEND1", "录音格式不合法");
  const separator = name.indexOf("__");
  need(separator > 0, "录音缺少设备标识");
  const meta = metaStat ? json(metaFile) : null;
  // A crash between archive rename and metadata commit is not a published file.
  if (meta && !meta.committed) return null;
  const record = { file: name, stamp, deviceId: name.slice(0, separator),
    sessionId: header.toString("ascii", 36, 84).replace(/\0.*$/, "") || name,
    segmentId: name.slice(separator + 2, -9), index: header.readUInt32LE(24),
    mode: header[12] === 0 ? "meeting" : "monitoring", type: meta?.processingClass || "legacy",
    startMs: meta?.captureStartMs ?? null, endMs: meta?.captureEndMs ?? null,
    captureUptimeMs: Number(header.readBigUInt64LE(28)), timelineBasis: meta?.timelineBasis,
    bytes: stat.size, durationMs: footer.readUInt32LE(12) * header.readUInt16LE(20),
    complete: footer[28] === 1, updatedAt: stat.mtime.toISOString(), ...fileTime(header) };
  if (meta) need(meta.deviceId === record.deviceId && meta.sessionId === record.sessionId &&
    meta.segmentIndex === record.index && meta.mode === record.mode, "录音元数据不一致");
  cache.set(file, { stamp, record });
  return record;
}
function scanLibrary(root) {
  const groups = new Map(), issues = [];
  const group = (device, session, mode) => {
    const id = digest(`${device}\0${session}\0${mode}`);
    if (!groups.has(id)) groups.set(id, { id, deviceId: device, sessionId: session, mode, records: [], end: null, scene: null });
    return groups.get(id);
  };
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    if (!entry.isFile() || !/^[A-Za-z0-9._-]+\.complete$/.test(entry.name)) continue;
    try {
      const record = inspect(root, entry.name);
      if (record) group(record.deviceId, record.sessionId, record.mode).records.push(record);
    } catch (error) { issues.push({ file: entry.name, error: error.message }); }
  }
  const sessions = path.join(root, "sessions");
  if (fs.existsSync(sessions)) for (const entry of fs.readdirSync(sessions, { withFileTypes: true })) {
    if (!entry.isDirectory() || !/^[A-Za-z0-9_-]+$/.test(entry.name)) continue;
    for (const [name, key] of [["session.json", "end"], ["scene.json", "scene"]]) {
      const file = path.join(sessions, entry.name, name);
      if (!fs.existsSync(file)) continue;
      try {
        const metadata = json(file);
        group(metadata.deviceId, metadata.audioSessionId, metadata.mode)[key] = metadata;
      } catch (error) { issues.push({ file: entry.name, error: error.message }); }
    }
  }
  const output = [...groups.values()].map(item => {
    item.records.sort((a, b) => a.index - b.index || a.segmentId.localeCompare(b.segmentId));
    if (item.records.some(record => record.timelineBasis === "uptime")) {
      if (item.mode === "monitoring") item.records.sort((a, b) => a.captureUptimeMs - b.captureUptimeMs || a.index - b.index);
      const origin = item.records[0].captureUptimeMs;
      item.records = item.records.map(record => ({ ...record,
        startMs: record.captureUptimeMs - origin,
        endMs: record.captureUptimeMs - origin + (record.endMs - record.startMs) }));
    }
    const indices = new Set(item.records.map(r => r.index));
    const verified = item.end?.recordingComplete === true && item.records.length === item.end.segmentCount &&
      indices.size === item.records.length && item.records.every((r, i) => r.complete && indices.has(i));
    const legacy = item.records.some(r => r.type === "legacy");
    const types = { meeting: 0, realtime: 0, history: 0, legacy: 0 };
    for (const record of item.records) types[record.type] = (types[record.type] || 0) + 1;
    return { ...item, end: undefined, scene: undefined,
      sessionTime: mergeSessionTime(item.end?.sessionTime, item.scene?.sessionTime),
      state: verified ? "complete" : item.end ? "incomplete" : legacy ? "legacy" : "receiving",
      expectedSegments: item.end?.segmentCount ?? null, missingSegments: item.end?.missingSegments || [],
      count: item.records.length, types, bytes: item.records.reduce((n, r) => n + r.bytes, 0),
      durationMs: item.records.reduce((n, r) => n + r.durationMs, 0),
      updatedAt: item.records.reduce((s, r) => s > r.updatedAt ? s : r.updatedAt, ""),
      revision: digest(JSON.stringify(item.records.map(r => [r.file, r.stamp]))) };
  });
  output.sort((a, b) => b.updatedAt.localeCompare(a.updatedAt));
  return { sessions: output, issues };
}
function wavHeader(bytes) {
  need(bytes <= 0xffffffff - 36, "音频超过 WAV 的 4 GiB 限制");
  const h = Buffer.alloc(44);
  h.write("RIFF"); h.writeUInt32LE(bytes + 36, 4); h.write("WAVEfmt ", 8);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(1, 22);
  h.writeUInt32LE(16000, 24); h.writeUInt32LE(32000, 28); h.writeUInt16LE(2, 32);
  h.writeUInt16LE(16, 34); h.write("data", 36); h.writeUInt32LE(bytes, 40);
  return h;
}
function writeAll(fd, data) {
  let written = 0;
  while (written < data.length) { const n = fs.writeSync(fd, data, written, data.length - written); need(n > 0, "WAV 写入失败"); written += n; }
}
function renderWav(root, records, target, progress = () => {}) {
  need(records.length > 0, "尚无已确认的音频");
  const temp = `${target}.tmp`;
  fs.mkdirSync(path.dirname(target), { recursive: true });
  const fd = fs.openSync(temp, "w");
  let bytes = 0, frames = 0, discontinuities = 0, lastTimestamp = null, lastSequence = null;
  try {
    writeAll(fd, wavHeader(0));
    for (let index = 0; index < records.length; index++) {
      const record = records[index];
      need(/^[A-Za-z0-9._-]+\.complete$/.test(record.file), "无效录音路径");
      const current = inspect(root, record.file);
      need(current && current.stamp === record.stamp, "录音已变化，请刷新后重试");
      const data = fs.readFileSync(path.join(root, record.file));
      archiveCrc(data); // Revalidate every byte; never turn a corrupt archive into playable audio.
      need(data[13] === 1 && data.readUInt16LE(14) === 1 && data.readUInt32LE(16) === 16000, "仅支持 16 kHz 单声道 PCM16");
      const pcm = Buffer.allocUnsafe(data.length);
      let length = 0;
      for (let offset = 96; offset < data.length - 32;) {
        const size = data.readUInt32LE(offset); offset += 4;
        const f = data.subarray(offset, offset + size);
        need(size === 662 && f.toString("ascii", 0, 2) === "NA" && f[2] === 1 && f[3] === 1 && f[5] === 1 &&
          f.readUInt16LE(6) === 16000 && f.readUInt16LE(8) === 20 && f.readUInt16LE(18) === 640, "录音帧格式不支持播放");
        const timestamp = f.readUInt32LE(14);
        const sequence = f.readUInt32LE(10);
        // Legacy timestamps measure task scheduling, not the sample timeline.
        // Their 4/15/30 ms jitter must not be reported as thousands of audio gaps.
        if (record.type === "legacy") {
          if (lastSequence !== null && sequence !== ((lastSequence + 1) >>> 0)) discontinuities++;
        } else if (lastTimestamp !== null && timestamp !== ((lastTimestamp + 20) >>> 0)) discontinuities++;
        lastSequence = sequence;
        lastTimestamp = timestamp;
        f.copy(pcm, length, 22); length += 640; frames++; offset += size;
      }
      bytes += length; need(bytes <= 0xffffffff - 36, "音频超过 WAV 的 4 GiB 限制");
      writeAll(fd, pcm.subarray(0, length));
      progress({ completed: index + 1, total: records.length });
    }
    const header = wavHeader(bytes);
    need(fs.writeSync(fd, header, 0, 44, 0) === 44, "WAV 头写入失败");
    fs.fsyncSync(fd);
  } catch (error) {
    fs.closeSync(fd); fs.unlinkSync(temp); throw error;
  }
  fs.closeSync(fd); fs.renameSync(temp, target);
  return { bytes: bytes + 44, durationMs: frames * 20, discontinuities };
}
if (parentPort && workerData?.sdAudioLibrary) parentPort.on("message", message => {
  try {
    const result = message.operation === "scan" ? scanLibrary(workerData.root) :
      renderWav(workerData.root, message.records, message.target, progress => parentPort.postMessage({ id: message.id, progress }));
    parentPort.postMessage({ id: message.id, result });
  } catch (error) { parentPort.postMessage({ id: message.id, error: error.message }); }
});
module.exports = { scanLibrary, renderWav, wavHeader, digest };
