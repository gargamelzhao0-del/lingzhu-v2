"use strict";
const fs = require("node:fs");
const path = require("node:path");
const { SessionFinalizerPool } = require("./sd-session-finalizer-pool");
const { SessionRefreshQueue } = require("./sd-session-refresh");
const { SdProtocolError } = require("./sd-protocol-error");
const { sessionTime, mergeSessionTime } = require("./sd-session-time");
const { fileTime } = require("./sd-file-time");

const prefixes = ["offline_audio", "meeting_audio", "monitoring_audio_realtime", "monitoring_audio_history"];
function prefix(type, suffix) {
  return prefixes.find(value => type === `${value}_${suffix}`) || null;
}
function demand(value, message, code = "invalid_argument") { if (!value) throw new SdProtocolError(code, message); }
function identity(value) {
  demand(typeof value === "string" && /^[A-Za-z0-9_-]{1,47}$/.test(value), "invalid SD session identity");
  return value;
}
function integer(value, name) {
  demand(Number.isSafeInteger(value) && value >= 0, `invalid ${name}`);
  return value;
}
function durableJson(file, value) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const temporary = `${file}.tmp`;
  const fd = fs.openSync(temporary, "w");
  try { fs.writeFileSync(fd, JSON.stringify(value, null, 2)); fs.fsyncSync(fd); }
  finally { fs.closeSync(fd); }
  fs.renameSync(temporary, file);
}
function readJson(file) { return fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, "utf8")) : null; }

function replyIdentity(message) {
  return message.segment_id !== undefined ? { segment_id: message.segment_id } :
    { audio_session_id: message.audio_session_id, segment_index: message.segment_index };
}

// Capture timestamps wrap at 32 bits; the archive's 64-bit start anchors them.
// Use recording time, never arrival time, to place a resumed file on the timeline.
function archiveInterval(data) {
  const start = Number(data.readBigUInt64LE(28));
  demand(Number.isSafeInteger(start), "invalid archive start time");
  let previous = null, elapsed = 0;
  for (let at = 96; at < data.length - 32; at += 666) {
    demand(at + 666 <= data.length - 32 && data.readUInt32LE(at) === 662,
      "invalid timeline record");
    const timestamp = data.readUInt32LE(at + 18);
    if (previous === null) demand(timestamp === (start >>> 0), "archive start differs from first frame");
    else {
      const delta = (timestamp - previous) >>> 0;
      demand(delta >= 20 && delta < 0x80000000, "backward or overlapping audio timestamp", "conflict");
      elapsed += delta;
    }
    previous = timestamp;
  }
  const end = start + elapsed + (previous === null ? 0 : 20);
  demand(Number.isSafeInteger(end), "invalid archive end time");
  return { captureStartMs: start, captureEndMs: end, timelineBasis: "uptime",
    ...fileTime(data, end - start) };
}

function sessionTimeline(records) {
  if (!records.some(record => record.timelineBasis === "uptime")) return records;
  // An upgrade may resume an open scene containing old relative-time metadata.
  // Those files passed CRC/geometry at commit. Read only their boundary records,
  // rather than scanning hours of PCM again in the finalizer worker.
  const intervals = records.map(record => record.timelineBasis === "uptime" ? record :
    { ...record, ...committedInterval(record.completePath) });
  // Older monitor firmware could publish two queued tails out of order.
  // Preserve their identities; play/end by validated capture time. Actual
  // overlaps still fail below. Meetings retain strict file-index ordering.
  if (intervals.every(record => record.mode === "monitoring"))
    intervals.sort((a, b) => a.captureStartMs - b.captureStartMs || a.segmentIndex - b.segmentIndex);
  const origin = intervals[0]?.captureStartMs || 0;
  return intervals.map(record => ({ ...record, captureStartMs: record.captureStartMs - origin,
    captureEndMs: record.captureEndMs - origin }));
}

function committedInterval(file) {
  const fd = fs.openSync(file, "r"), first = Buffer.alloc(122), last = Buffer.alloc(22);
  try {
    const size = fs.fstatSync(fd).size;
    if (size === 128) {
      demand(fs.readSync(fd, first, 0, 96, 0) === 96, "short empty archive");
      const start = Number(first.readBigUInt64LE(28));
      demand(Number.isSafeInteger(start), "invalid empty archive time");
      return { captureStartMs: start, captureEndMs: start, timelineBasis: "uptime", ...fileTime(first, 0) };
    }
    demand(size >= 794 && (size - 128) % 666 === 0, "invalid committed audio size");
    demand(fs.readSync(fd, first, 0, first.length, 0) === first.length &&
      fs.readSync(fd, last, 0, last.length, size - 32 - 662) === last.length, "short committed timeline");
    const start = Number(first.readBigUInt64LE(28));
    const span = (last.readUInt32LE(14) - first.readUInt32LE(114)) >>> 0;
    demand(Number.isSafeInteger(start) && first.readUInt32LE(114) === (start >>> 0) && span < 0x80000000,
      "invalid committed timeline");
    return { captureStartMs: start, captureEndMs: start + span + 20, timelineBasis: "uptime",
      ...fileTime(first, span + 20) };
  } finally { fs.closeSync(fd); }
}

class AudioSessions {
  constructor(storageDir) {
    this.storageDir = storageDir; this.endJobs = new Map();
    this.finalizers = new SessionFinalizerPool(storageDir);
    this.refreshes = new SessionRefreshQueue(this, durableJson, readJson);
  }
  sessionDir(device, session) {
    return path.join(this.storageDir, "sessions", `${identity(device)}__${identity(session)}`);
  }
  fileIdentity(device, message, route) {
    if (message.segment_id !== undefined || route === "offline_audio") return identity(message.segment_id);
    const session = identity(message.audio_session_id);
    demand(session.length <= 35, "session identity too long");
    const index = integer(message.segment_index, "segment_index");
    demand(index < 1000000, "too many session segments");
    // Retain a previous receiver's filename when upgrading mid-transfer.
    const occupied = readJson(path.join(this.sessionDir(device, session), `${String(index).padStart(8, "0")}.json`));
    return occupied ? identity(occupied.segmentId) : `sd_${session}_${index.toString(16).padStart(8, "0")}`;
  }
  begin(device, message, paths, route, tupleIdentity = false, deferPublication = false) {
    if (route === "offline_audio") return null;
    identity(device); identity(message.segment_id); identity(message.audio_session_id);
    const job = this.endJobs.get(this.sessionDir(device, message.audio_session_id));
    demand(!job || job.refresh, "session finalization in progress", "busy");
    demand(message.mode === undefined, "new SD types must omit archive_only");
    integer(message.segment_index, "segment_index");
    demand(message.segment_index < 1000000, "too many session segments");
    const legacyTimes = !tupleIdentity && (message.capture_start_ms !== undefined || message.capture_end_ms !== undefined);
    if (legacyTimes) {
      integer(message.capture_start_ms, "capture_start_ms");
      integer(message.capture_end_ms, "capture_end_ms");
      demand(message.capture_end_ms > message.capture_start_ms, "empty capture interval");
    }
    if (message.capture_age_ms !== undefined) integer(message.capture_age_ms, "capture_age_ms");
    const metadata = {
      deviceId: device, segmentId: message.segment_id,
      sessionId: message.audio_session_id, segmentIndex: message.segment_index,
      mode: route === "meeting_audio" ? "meeting" : "monitoring",
      totalBytes: message.total_bytes, crc32: message.crc32,
    };
    const file = `${paths.complete}.json`;
    const existing = readJson(file);
    if (existing) {
      for (const [key, value] of Object.entries(metadata)) demand(existing[key] === value, `immutable ${key} changed`, "conflict");
      if (legacyTimes && existing.captureStartMs !== undefined && existing.timelineBasis !== "uptime") {
        demand(existing.captureStartMs === message.capture_start_ms && existing.captureEndMs === message.capture_end_ms,
          "immutable capture interval changed", "conflict");
      }
    }
    const folder = this.sessionDir(device, metadata.sessionId);
    const indexFile = path.join(folder, `${String(metadata.segmentIndex).padStart(8, "0")}.json`);
    const occupied = readJson(indexFile);
    demand(!occupied || occupied.segmentId === metadata.segmentId, "session index already belongs to another file", "conflict");
    const ended = readJson(path.join(folder, "session.json"));
    const sceneProtocol = fs.existsSync(path.join(folder, "scene.json"));
    demand(!deferPublication || (tupleIdentity && sceneProtocol), "deferred publication requires a registered scene");
    demand(!ended || (sceneProtocol && !ended.recordingComplete && metadata.segmentIndex < ended.segmentCount) || (existing?.committed && occupied?.committed &&
      ended.segments.some(item => item.index === metadata.segmentIndex && item.segmentId === metadata.segmentId)),
      "new data after session end", "conflict");
    const saved = existing || { ...metadata, committed: false, firstType: route,
      ...(legacyTimes ? { captureStartMs: message.capture_start_ms, captureEndMs: message.capture_end_ms } : {}) };
    // Scene packets already have a durable scene and resume marker. Preparing
    // their commit needs identity validation, not two intermediate writes of
    // committed:false immediately overwritten by the final durable metadata.
    if (!deferPublication) {
      if (!existing) durableJson(file, saved);
      if (!occupied) durableJson(indexFile, saved);
    }
    this.finalizers.prewarm();
    return { file, indexFile, metadata: saved, route, deferPublication, ageMs: message.capture_age_ms || 0,
      deriveTime: !legacyTimes || (existing && (existing.captureStartMs === undefined || existing.timelineBasis === "uptime")),
      started: process.hrtime.bigint() };
  }
  validate(tx, data) {
    if (!tx.session) return;
    const meta = tx.session.metadata;
    demand(data[12] === (meta.mode === "meeting" ? 0 : 1), "archive mode differs from begin");
    demand(data.readUInt32LE(24) === meta.segmentIndex, "archive segment index differs from begin");
    demand(data.subarray(36, 84).toString("ascii").replace(/\0.*$/, "") === meta.sessionId,
      "archive session differs from begin");
    demand(data.readUInt32LE(16) === 16000 && data[13] === 1 && data.readUInt16LE(14) === 1 &&
      data.readUInt16LE(20) === 20 && data[data.length - 4] <= 1,
      "unsupported SD audio geometry");
    let offset = 96;
    while (offset < data.length - 32) {
      const size = data.readUInt32LE(offset); offset += 4;
      demand(size === 662 && data.toString("ascii", offset, offset + 2) === "NA" &&
        data[offset + 2] === 1 && data[offset + 3] === 1 &&
        data[offset + 5] === 1 && data.readUInt16LE(offset + 6) === 16000 &&
        data.readUInt16LE(offset + 8) === 20 && data.readUInt16LE(offset + 18) === 640,
        "invalid SD PCM record");
      offset += size;
    }
    if (tx.session.deriveTime) tx.session.interval = archiveInterval(data);
    else {
      tx.session.interval = fileTime(data);
      if (tx.session.interval.fileDurationMs != null) archiveInterval(data);
    }
  }
  commit(tx, summary) {
    if (!tx.session) return;
    const info = tx.session;
    const job = this.endJobs.get(this.sessionDir(info.metadata.deviceId, info.metadata.sessionId));
    demand(!job || job.refresh, "session finalization in progress", "busy");
    const previous = readJson(info.file);
    demand(previous || info.deferPublication, "file metadata missing before commit", "storage_unavailable");
    const ended = readJson(path.join(this.sessionDir(info.metadata.deviceId, info.metadata.sessionId), "session.json"));
    demand(!ended || (info.sceneProtocol && !ended.recordingComplete && info.metadata.segmentIndex < ended.segmentCount) || (previous?.committed && ended.segments.some(item =>
      item.index === previous.segmentIndex && item.segmentId === previous.segmentId)), "new commit after session end", "conflict");
    if (!previous?.committed) {
      const expired = !info.sceneProtocol && info.route === "monitoring_audio_realtime" &&
        info.ageMs + Number(process.hrtime.bigint() - info.started) / 1e6 > 3000;
      const saved = { ...(previous || info.metadata), committed: true, ...summary, ...info.interval,
        processingClass: info.route === "meeting_audio" ? "meeting" :
          (info.route === "monitoring_audio_history" || expired ? "history" : "realtime"),
        committedAt: new Date().toISOString(), completePath: tx.paths.complete,
        audioDurationMs: summary.frameCount * 20 };
      durableJson(info.file, saved);
      durableJson(info.indexFile, saved);
    } else {
      // Repair a crash between the two metadata publications, idempotently.
      durableJson(info.indexFile, previous);
    }
    if (info.sceneProtocol && ended && !ended.recordingComplete) {
      this.refreshes.mark(info.metadata.deviceId, ended);
    }
  }
  endParameters(device, message) {
    sessionTime(message);
    const mode = message.type === "meeting_audio_session_end" ? "meeting" :
      message.type === "monitoring_audio_session_end" ? "monitoring" : null;
    demand(mode, "invalid session end type");
    identity(message.audio_session_id);
    demand(typeof message.request_id === "string" && message.request_id.length > 0, "request_id required");
    integer(message.segment_count, "segment_count");
    demand(message.segment_count <= 1000000 && typeof message.recording_complete === "boolean", "invalid session end");
    const folder = this.sessionDir(device, message.audio_session_id);
    return { folder, mode, signature: JSON.stringify([mode, message.segment_count, message.recording_complete]) };
  }
  async endAsync(device, message) {
    const { signature } = this.endParameters(device, message);
    for (;;) {
      const refresh = this.refreshes.get(device, message);
      if (refresh) demand(this.endParameters(refresh.device, refresh.message).signature === signature,
        "session end changed after confirmation", "conflict");
      let ack;
      try { ack = await this.runEndJob(device, message, refresh); }
      catch (error) { if (refresh) this.refreshes.defer(refresh, error); throw error; }
      // A concurrent late commit may have advanced the durable revision. An
      // explicit end waits for that revision; background refreshes coalesce it.
      if (!this.refreshes.get(device, message)) {
        // A retry may share a running finalizer whose original request had no
        // time. Fill it durably before acknowledging this particular request.
        const folder = this.sessionDir(device, message.audio_session_id);
        const file = path.join(folder, "session.json"), stored = readJson(file);
        const scene = readJson(path.join(folder, "scene.json"));
        const time = mergeSessionTime(mergeSessionTime(stored?.sessionTime, scene?.sessionTime), sessionTime(message));
        if (stored && JSON.stringify(time) !== JSON.stringify(stored.sessionTime))
          durableJson(file, { ...stored, sessionTime: time });
        return ack;
      }
    }
  }
  async runEndJob(device, message, refresh = null) {
    const { folder, signature } = this.endParameters(device, message);
    let job = this.endJobs.get(folder);
    if (job) demand(job.signature === signature, "session end changed while finalizing", "conflict");
    else {
      // Bound host resource usage and deduplicate retries from reconnecting devices.
      demand(this.endJobs.size < 4, "session finalizer busy; retry later", "busy");
      const promise = this.finalizers.run(device, message, !!refresh).then(result => {
        if (refresh) this.refreshes.complete(refresh);
        console.log(`${new Date().toISOString()} SD session finalizer ${JSON.stringify({
          device, session: message.audio_session_id, warm: result.warm,
          elapsedMs: result.elapsedMs, workMs: result.workMs })}`);
        return result.ack;
      }).finally(() => this.endJobs.delete(folder));
      job = { signature, promise, refresh: !!refresh }; this.endJobs.set(folder, job);
    }
    const ack = await job.promise;
    return { ...ack, request_id: message.request_id };
  }
  async close() {
    this.refreshes.close();
    await Promise.all([...this.finalizers.workers].map(slot => slot.worker.terminate()));
  }
  end(device, message, refresh = false) {
    const { folder, mode } = this.endParameters(device, message);
    const ack = { type: `${message.type}_ack`, request_id: message.request_id,
      audio_session_id: message.audio_session_id, ok: true };
    const finalPath = path.join(folder, "session.json");
    const previous = readJson(finalPath);
    const scene = readJson(path.join(folder, "scene.json"));
    const time = mergeSessionTime(mergeSessionTime(previous?.sessionTime, scene?.sessionTime), sessionTime(message));
    if (previous) {
      demand(previous.deviceId === device && previous.audioSessionId === message.audio_session_id &&
        previous.mode === mode && previous.segmentCount === message.segment_count &&
        previous.recordingComplete === message.recording_complete, "session end changed after confirmation", "conflict");
      if (!refresh) {
        if (JSON.stringify(time) !== JSON.stringify(previous.sessionTime))
          durableJson(finalPath, { ...previous, sessionTime: time });
        return ack; // Already durable: no per-segment rescan on a lost ACK.
      }
    }
    fs.mkdirSync(folder, { recursive: true });
    const records = fs.readdirSync(folder).filter(file => /^\d{8}\.json$/.test(file))
      .map(file => readJson(path.join(folder, file))).sort((a, b) => a.segmentIndex - b.segmentIndex);
    const indexed = records.filter(record => record.committed);
    demand(indexed.every(record => Number.isSafeInteger(record.segmentIndex) && record.segmentIndex >= 0 &&
      record.segmentIndex < message.segment_count && record.mode === mode &&
      record.deviceId === device && record.sessionId === message.audio_session_id),
      "session segment count or mode mismatch", "conflict");
    const missing = [];
    let next = 0, duration = 0, previousEnd = 0;
    for (const record of indexed) {
      demand(record.segmentIndex >= next, "duplicate session index", "conflict");
      if (record.segmentIndex > next) missing.push([next, record.segmentIndex - 1]);
      next = record.segmentIndex + 1;
    }
    if (next < message.segment_count) missing.push([next, message.segment_count - 1]);
    const committed = sessionTimeline(indexed);
    for (const record of committed) {
      demand(record.captureStartMs >= previousEnd, "overlapping session audio", "conflict");
      demand(record.captureEndMs >= record.captureStartMs, "invalid session audio interval");
      previousEnd = record.captureEndMs;
      duration += record.audioDurationMs;
    }
    if (message.recording_complete) demand(missing.length === 0 && committed.every(item => item.complete), "session is incomplete", "incomplete");
    const output = { schema: "nuwax.sd-session.v1", deviceId: device,
      audioSessionId: message.audio_session_id, mode, segmentCount: message.segment_count,
      receivedSegments: committed.length, recordingComplete: message.recording_complete,
      missingSegments: missing, audioDurationMs: duration, timelineDurationMs: previousEnd,
      ...(time ? { sessionTime: time } : {}),
      segments: committed.map(record => ({ segmentId: record.segmentId, index: record.segmentIndex,
        type: record.processingClass, startMs: record.captureStartMs, endMs: record.captureEndMs,
        fileStartedAtMs: record.fileStartedAtMs ?? null, fileDurationMs: record.fileDurationMs ?? null,
        fileEndedAtMs: record.fileEndedAtMs ?? null,
        bytes: record.totalBytes, crc32: record.crc32, path: record.completePath })) };
    durableJson(finalPath, output);
    return ack;
  }
}
module.exports = { AudioSessions, prefix, durableJson, replyIdentity, archiveInterval };
