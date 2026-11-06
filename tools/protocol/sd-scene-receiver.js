"use strict";

// Scene registration is independent of file transactions. File bytes keep the
// existing archive format and storage names, including pre-upgrade aliases.
const fs = require("node:fs");
const path = require("node:path");
const { performance } = require("node:perf_hooks");
const { durableJson } = require("./sd-audio-sessions");
const { SdProtocolError, errorCode } = require("./sd-protocol-error");
const { sessionTime, mergeSessionTime } = require("./sd-session-time");
const MAX_BYTES = 4194304;
const FLUSH_BYTES = 262144;
class SceneError extends SdProtocolError {}
function requireThat(value, code, message) { if (!value) throw new SceneError(code, message); }
function id(value, max = 35) {
  requireThat(typeof value === "string" && new RegExp(`^[A-Za-z0-9_-]{1,${max}}$`).test(value), "invalid_argument", "invalid identity");
  return value;
}
function uint(value, max, name) {
  requireThat(Number.isSafeInteger(value) && value >= 0 && value <= max, "invalid_argument", `invalid ${name}`);
  return value;
}
function read(file) { return fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, "utf8")) : null; }
function isSceneBegin(message) {
  return (message.type === "meeting_audio_begin" || message.type === "monitoring_audio_begin") &&
    message.segment_index === undefined && message.segment_id === undefined;
}
function failure(message, error) {
  return { type: `${message.type}_ack`, request_id: message.request_id,
    audio_session_id: message.audio_session_id,
    ...(message.segment_index !== undefined ? { segment_index: message.segment_index } : {}),
    ok: false, error_code: errorCode(error) };
}
function parseScenePacket(bytes) {
  requireThat(Buffer.isBuffer(bytes) && bytes.length >= 65 && bytes.toString("ascii", 0, 6) === "NWXCH1" &&
    bytes.readUInt16LE(6) === 0 && bytes.readUInt32LE(8) === 3, "invalid_argument", "invalid scene packet header");
  const n = uint(bytes.readUInt32LE(16), 8064, "payload length");
  requireThat(n > 0 && bytes.length === n + 64, "invalid_argument", "invalid scene packet length");
  const name = bytes.subarray(20, 56), zero = name.indexOf(0);
  requireThat(zero > 0 && name.subarray(zero).every(b => b === 0), "invalid_argument", "invalid scene id padding");
  const session = id(name.subarray(0, zero).toString("ascii"));
  requireThat(name.subarray(0, zero).every(b => b < 128), "invalid_argument", "non-ASCII scene id");
  const index = uint(bytes.readUInt32LE(56), 999999, "segment index");
  const offset = bytes.readUInt32LE(12), flags = bytes.readUInt32LE(60);
  requireThat(flags <= 1 && offset + n <= MAX_BYTES, "invalid_argument", "invalid scene packet range");
  return { session, index, offset, payload: bytes.subarray(64), final: flags === 1 };
}

class SceneReceiver {
  constructor(receiver, archiveCrc) {
    this.receiver = receiver; this.archiveCrc = archiveCrc; this.bindings = new Map();
    // Survive connection replacement during a partial metadata publication.
    // Counters describe this process; existing fully committed files are not new traffic.
    this.pendingCommitStats = new Set();
  }
  key(device, session) { return `${id(device, 47)}__${id(session)}`; }
  bound(device, session) { return this.bindings.has(this.key(device, session)); }
  binding(device, session, connectionId, pending = false) {
    const binding = this.bindings.get(this.key(device, session));
    requireThat(binding && binding.connectionId === connectionId && (pending || binding.ready), "conflict", "scene not registered on this connection");
    return binding;
  }
  control(message) { id(message.request_id, 39); id(message.audio_session_id); }
  begin(device, message, connectionId = 0) {
    this.control(message);
    const requestedTime = sessionTime(message);
    requireThat(isSceneBegin(message) && message.total_bytes === undefined && message.crc32 === undefined,
      "invalid_argument", "scene begin must not declare a file");
    const session = message.audio_session_id, key = this.key(device, session);
    const mode = message.type === "meeting_audio_begin" ? "meeting" : "monitoring";
    const folder = this.receiver.audioSessions.sessionDir(device, session);
    const scenePath = path.join(folder, "scene.json");
    const existing = read(scenePath), ended = read(path.join(folder, "session.json"));
    requireThat((!existing || existing.mode === mode) && (!ended || ended.mode === mode), "conflict", "scene mode changed");
    const previous = this.bindings.get(key);
    requireThat(!previous || previous.connectionId <= connectionId, "conflict", "retired scene connection");
    const resume = message.resume;
    requireThat(resume === undefined || (Array.isArray(resume) && resume.length <= 16), "invalid_argument", "invalid resume list");
    const seen = new Set();
    for (const file of resume || []) {
      requireThat(file && typeof file === "object", "invalid_argument", "invalid resume item");
      uint(file.segment_index, 999999, "segment index");
      requireThat(!seen.has(file.segment_index), "invalid_argument", "duplicate resume index");
      seen.add(file.segment_index); this.declaration(file);
    }
    if (this.receiver.options.rejectBegin) throw new SceneError("busy", "injected begin rejection");
    const time = mergeSessionTime(mergeSessionTime(existing?.sessionTime, ended?.sessionTime), requestedTime);
    if (!existing || JSON.stringify(time) !== JSON.stringify(existing.sessionTime))
      durableJson(scenePath, { ...existing, deviceId: device, audioSessionId: session, mode,
        ...(time ? { sessionTime: time } : {}) });
    this.bindings.set(key, { connectionId, mode, ready: false });
    const result = [];
    for (const file of resume || []) {
      const tx = this.file(device, session, file.segment_index, connectionId, true);
      this.declare(tx, file);
      // Include buffered bytes only after durable flush. A new process recovers
      // only the on-disk contiguous prefix, and fsyncs before advertising it.
      this.flush(tx);
      const stored = fs.existsSync(tx.paths.complete) ? tx.paths.complete : tx.paths.part;
      if (fs.existsSync(stored)) {
        const fd = fs.openSync(stored, "r+");
        try { fs.fsyncSync(fd); tx.nextOffset = fs.fstatSync(fd).size; } finally { fs.closeSync(fd); }
      }
      requireThat(tx.nextOffset <= file.total_bytes, "conflict", "stored prefix exceeds file length");
      if (tx.nextOffset === file.total_bytes) {
        tx.fileFinal = true; tx.finalOffset = file.total_bytes;
      }
      this.persistState(tx);
      result.push({ segment_index: file.segment_index, next_offset: tx.nextOffset });
    }
    this.bindings.get(key).ready = true;
    this.receiver.stats.begins++;
    return { type: `${message.type}_ack`, request_id: message.request_id, audio_session_id: session,
      ok: true, ...(resume !== undefined ? { resume: result } : {}) };
  }
  declaration(message) {
    uint(message.total_bytes, MAX_BYTES, "total bytes"); uint(message.crc32, 0xffffffff, "crc32");
    requireThat(message.total_bytes >= 128, "invalid_argument", "file is too short");
  }
  declare(tx, message) {
    this.declaration(message);
    requireThat(tx.totalBytes === undefined || (tx.totalBytes === message.total_bytes && tx.crc32 === message.crc32),
      "conflict", "immutable file length or CRC changed");
    requireThat(tx.nextOffset + tx.pendingBytes <= message.total_bytes, "conflict", "received data exceeds declared length");
    tx.totalBytes = message.total_bytes; tx.crc32 = message.crc32;
  }
  persistState(tx) {
    durableJson(tx.statePath, { totalBytes: tx.totalBytes, crc32: tx.crc32,
      finalOffset: tx.fileFinal ? tx.finalOffset : undefined });
  }
  file(device, session, index, connectionId, pending = false) {
    const binding = this.binding(device, session, connectionId, pending);
    uint(index, 999999, "segment index");
    const fields = { audio_session_id: session, segment_index: index };
    const segmentId = this.receiver.audioSessions.fileIdentity(device, fields, "meeting_audio");
    const key = this.receiver.key(device, segmentId);
    let tx = this.receiver.transactions.get(key);
    if (tx && tx.sceneProtocol && tx.connectionId === connectionId) return tx;
    if (tx?.sceneProtocol) this.flush(tx);
    const paths = this.receiver.paths(device, segmentId), statePath = paths.part + ".scene.json";
    const metadata = read(paths.complete + ".json"), state = read(statePath);
    requireThat(!metadata || (metadata.sessionId === session && metadata.segmentIndex === index && metadata.mode === binding.mode),
      "conflict", "file identity or mode changed");
    const folder = this.receiver.audioSessions.sessionDir(device, session);
    const ended = read(path.join(folder, "session.json"));
    const indexed = read(path.join(folder, `${String(index).padStart(8, "0")}.json`));
    requireThat(!ended || (index < ended.segmentCount && (!ended.recordingComplete || metadata?.committed)),
      "conflict", "new file outside finalized scene");
    const stored = fs.existsSync(paths.complete) ? paths.complete : paths.part;
    const size = fs.existsSync(stored) ? fs.statSync(stored).size : 0;
    requireThat(size <= MAX_BYTES, "conflict", "stored file too large");
    if (!metadata?.committed || !indexed?.committed) this.pendingCommitStats.add(key);
    tx = { sceneProtocol: true, deviceId: device, connectionId, audioSessionId: session, segmentIndex: index,
      segmentId, paths, statePath, mode: binding.mode, totalBytes: metadata?.totalBytes ?? state?.totalBytes,
      crc32: metadata?.crc32 ?? state?.crc32, nextOffset: size, pending: [], pendingBytes: 0,
      fileFinal: Boolean(metadata?.committed || state?.finalOffset !== undefined),
      finalOffset: metadata?.committed ? size : state?.finalOffset };
    this.receiver.transactions.set(key, tx);
    return tx;
  }
  validateHeader(tx) {
    if (tx.headerChecked || tx.nextOffset + tx.pendingBytes < 96) return;
    let header;
    if (!tx.nextOffset) header = Buffer.concat(tx.pending, tx.pendingBytes).subarray(0,96);
    else {
      // A recovered prefix may stop anywhere, including inside the header.
      const stored = fs.existsSync(tx.paths.complete) ? tx.paths.complete : tx.paths.part;
      const fd = fs.openSync(stored,"r"), durable = Buffer.alloc(Math.min(96,tx.nextOffset));
      try { requireThat(fs.readSync(fd,durable,0,durable.length,0)===durable.length,"storage_unavailable","short archive header"); }
      finally { fs.closeSync(fd); }
      header = Buffer.concat([durable,...tx.pending]).subarray(0,96);
    }
    requireThat(header.toString("ascii",0,7)==="NWXOFF1" && header.readUInt16LE(8)===1 &&
      header.readUInt16LE(10)===96 && header[12]===(tx.mode==="meeting"?0:1) &&
      header.readUInt32LE(24)===tx.segmentIndex && header.readUInt16LE(22)===2 &&
      header.subarray(36,84).toString("ascii").replace(/\0.*$/,"")===tx.audioSessionId,
      "conflict","archive header differs from scene/file identity");
    tx.headerChecked = true;
  }
  flush(tx) {
    if (!tx.pendingBytes) return;
    const data = Buffer.concat(tx.pending, tx.pendingBytes);
    const fd = fs.openSync(tx.paths.part, fs.existsSync(tx.paths.part) ? "r+" : "w+");
    const started = performance.now();
    try {
      let written = 0;
      while (written < data.length) {
        const n = fs.writeSync(fd, data, written, data.length - written, tx.nextOffset + written);
        requireThat(n > 0, "storage_unavailable", "short file write"); written += n;
      }
      fs.fsyncSync(fd);
    } finally { fs.closeSync(fd); }
    tx.nextOffset += data.length; tx.pending = []; tx.pendingBytes = 0;
    const elapsed = performance.now() - started, stats = this.receiver.stats;
    stats.receivedBytes += data.length; stats.durableWrites++; stats.durableWriteMs += elapsed;
    stats.durableWriteMaxMs = Math.max(stats.durableWriteMaxMs, elapsed);
    if (tx.nextOffset >= 96) this.receiver.recoverHeader(tx);
  }
  chunk(device, bytes, connectionId = 0) {
    const packet = parseScenePacket(bytes);
    const tx = this.file(device, packet.session, packet.index, connectionId);
    // Bound buffered memory across interleaved files, without discarding the
    // interrupted history transaction or requiring another begin.
    for (const other of this.receiver.transactions.values())
      if (other !== tx && other.sceneProtocol && other.deviceId === device) this.flush(other);
    let offset = packet.offset, payload = packet.payload;
    const end = offset + payload.length;
    requireThat(tx.totalBytes === undefined || end <= tx.totalBytes, "conflict", "packet exceeds declared file");
    if (offset < tx.nextOffset + tx.pendingBytes) {
      this.flush(tx);
      const overlap = Math.min(payload.length, tx.nextOffset - offset);
      const stored = fs.existsSync(tx.paths.complete) ? tx.paths.complete : tx.paths.part;
      const fd = fs.openSync(stored, "r"), old = Buffer.alloc(overlap);
      try { requireThat(fs.readSync(fd, old, 0, overlap, offset) === overlap && old.equals(payload.subarray(0, overlap)),
        "conflict", "duplicate bytes differ"); } finally { fs.closeSync(fd); }
      offset += overlap; payload = payload.subarray(overlap);
    }
    requireThat(offset === tx.nextOffset + tx.pendingBytes || payload.length === 0, "incomplete", "noncontiguous scene data");
    if (payload.length) {
      requireThat(!tx.fileFinal && !fs.existsSync(tx.paths.complete), "conflict", "data after file end");
      tx.pending.push(Buffer.from(payload)); tx.pendingBytes += payload.length;
    }
    this.validateHeader(tx);
    if (packet.final) {
      requireThat(end === tx.nextOffset + tx.pendingBytes && (!tx.fileFinal || end === tx.finalOffset), "conflict", "inconsistent final packet");
      tx.fileFinal = true; tx.finalOffset = end;
    }
    if (tx.pendingBytes >= FLUSH_BYTES || packet.final) this.flush(tx);
    if (packet.final) this.persistState(tx);
    this.receiver.stats.chunks++;
    return null;
  }
  commit(device, message, connectionId = 0) {
    const started = performance.now();
    this.control(message); this.declaration(message);
    const tx = this.file(device, message.audio_session_id, message.segment_index, connectionId);
    const route = message.type.replace(/_commit$/, "");
    requireThat((tx.mode === "meeting" && route === "meeting_audio") ||
      (tx.mode === "monitoring" && ["monitoring_audio_realtime", "monitoring_audio_history"].includes(route)),
      "conflict", "commit mode differs from scene");
    if (this.receiver.options.rejectCommit) throw new SceneError("busy", "injected commit rejection");
    this.declare(tx, message); this.flush(tx);
    requireThat(tx.fileFinal && tx.finalOffset === tx.totalBytes && tx.nextOffset === tx.totalBytes,
      "incomplete", "commit before complete file");
    const finalized = fs.existsSync(tx.paths.complete);
    const data = fs.readFileSync(finalized ? tx.paths.complete : tx.paths.part);
    const loaded = performance.now();
    let summary;
    try {
      summary = this.archiveCrc(data);
      requireThat(summary.crc === tx.crc32, "crc_mismatch", "archive CRC mismatch");
    } catch (error) {
      if (!finalized) {
        fs.rmSync(tx.paths.part, { force: true }); fs.rmSync(tx.statePath, { force: true });
        this.receiver.transactions.delete(this.receiver.key(device, tx.segmentId));
      }
      this.receiver.stats.crcFailures++;
      throw new SceneError(finalized ? "conflict" : "crc_mismatch", error.message);
    }
    const fields = { ...message, segment_id: tx.segmentId };
    tx.session = this.receiver.audioSessions.begin(device, fields, tx.paths, route, true, true);
    tx.session.sceneProtocol = true;
    this.receiver.audioSessions.validate(tx, data);
    requireThat(data.readUInt16LE(22) === 2, "invalid_argument", "invalid SD source flags");
    const validated = performance.now();
    if (!finalized) fs.renameSync(tx.paths.part, tx.paths.complete);
    this.receiver.audioSessions.commit(tx, summary);
    if (this.pendingCommitStats.delete(this.receiver.key(device, tx.segmentId)))
      this.receiver.stats.committedBytes += tx.totalBytes;
    this.receiver.stats.commits++;
    this.receiver.transactions.delete(this.receiver.key(device, tx.segmentId));
    fs.rmSync(tx.statePath, { force: true });
    const published = performance.now();
    this.receiver.noteArchive(tx, { ...summary, committed: true, totalBytes: tx.totalBytes });
    const round = n => Math.round(n * 1000) / 1000;
    const recent = this.receiver.stats.sceneCommitTimings ||= [];
    recent.push({ requestId: message.request_id, sessionId: message.audio_session_id,
      segmentIndex: message.segment_index, at: new Date().toISOString(),
      loadMs: round(loaded - started), validateMs: round(validated - loaded),
      publishMs: round(published - validated), totalMs: round(performance.now() - started) });
    if (recent.length > 32) recent.splice(0, recent.length - 32);
    this.receiver.options.persistReport?.();
    return { type: `${message.type}_ack`, request_id: message.request_id, audio_session_id: message.audio_session_id,
      segment_index: message.segment_index, ok: true };
  }
  async end(device, message, connectionId = 0) {
    this.control(message);
    const binding = this.binding(device, message.audio_session_id, connectionId);
    requireThat(message.type === `${binding.mode === "meeting" ? "meeting" : "monitoring"}_audio_session_end`,
      "conflict", "scene end mode changed");
    return this.receiver.audioSessions.endAsync(device, message);
  }
  release(device, connectionId) {
    for (const [key, tx] of this.receiver.transactions) {
      if (tx.sceneProtocol && tx.deviceId === device && tx.connectionId === connectionId) {
        // RAM is not acknowledged. Reconnect recovers the durable prefix.
        this.receiver.transactions.delete(key);
      }
    }
    for (const [key, binding] of this.bindings)
      if (key.startsWith(device + "__") && binding.connectionId === connectionId) this.bindings.delete(key);
  }
}
module.exports = { SceneReceiver, SceneError, isSceneBegin, parseScenePacket, failure };
