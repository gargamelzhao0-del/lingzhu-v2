#!/usr/bin/env node
"use strict";

/*
 * Test-only receiver for offline_audio_replay_v1.
 *
 * This file deliberately does not share the production cloud gateway. It is
 * a small, dependency-free WebSocket endpoint that can be run on a LAN while
 * validating a real device. --self-test exercises the receiver without a
 * network or ESP32, so protocol tests are repeatable on a developer machine.
 */

const crypto = require("node:crypto");
const { AudioSessions, prefix, replyIdentity } = require("./sd-audio-sessions");
const { SceneReceiver, isSceneBegin, failure: sceneFailure } = require("./sd-scene-receiver");
const fs = require("node:fs");
const http = require("node:http");
const { createAudioWeb } = require("./sd-audio-web");
const { startDiscovery } = require("./sd-gateway-discovery");
const os = require("node:os");
const path = require("node:path");
const { monitorEventLoopDelay, performance } = require("node:perf_hooks");
const { GatewayReportWriter, ARCHIVE_LIMIT, CONNECTION_LIMIT } = require("./sd-gateway-report");

const ARCHIVE_HEADER_BYTES = 96;
const ARCHIVE_FOOTER_BYTES = 32;
const CHUNK_HEADER_BYTES = 64;
const CHUNK_PAYLOAD_BYTES = 8064;
const MAX_ARCHIVE_BYTES = 4 * 1024 * 1024;
// Disk batching is local to the receiver; it never requests a device ACK wait.
const FILE_FLUSH_BYTES = 256 * 1024;
const MAX_FRAME_BYTES = 16 * 1024 * 1024;

function parseArgs(argv) {
  const args = {
    host: process.env.OFFLINE_REPLAY_HOST || "0.0.0.0",
    advertisedHost: process.env.OFFLINE_REPLAY_ADVERTISED_HOST || "",
    discoveryConfig: "",
    port: Number(process.env.OFFLINE_REPLAY_PORT || 8810),
    storageDir: process.env.OFFLINE_REPLAY_STORAGE ||
      path.join(__dirname, "..", "..", "..", "runtime-logs", "offline-audio-replay"),
    delayAckMs: Number(process.env.OFFLINE_REPLAY_DELAY_ACK_MS || 0),
    dropAfterChunks: Number(process.env.OFFLINE_REPLAY_DROP_AFTER_CHUNKS || 0),
    rejectBegin: process.env.OFFLINE_REPLAY_REJECT_BEGIN === "1",
    rejectCommit: process.env.OFFLINE_REPLAY_REJECT_COMMIT === "1",
    controlMode: process.env.OFFLINE_TEST_MODE || "",
    autoOpen: process.env.OFFLINE_TEST_AUTO_OPEN === "1",
    closeAfterMs: Number(process.env.OFFLINE_TEST_CLOSE_AFTER_MS || 0),
    reportFile: process.env.OFFLINE_TEST_REPORT || "",
    selfTest: false,
    help: false,
  };
  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) throw new Error(`Missing value for ${arg}`);
      return argv[++i];
    };
    if (arg === "--help" || arg === "-h") args.help = true;
    else if (arg === "--self-test") args.selfTest = true;
    else if (arg === "--host") args.host = next();
    else if (arg === "--advertised-host") args.advertisedHost = next();
    else if (arg === "--discovery-config") args.discoveryConfig = path.resolve(next());
    else if (arg === "--port") args.port = Number(next());
    else if (arg === "--storage-dir") args.storageDir = path.resolve(next());
    else if (arg === "--delay-ack-ms") args.delayAckMs = Number(next());
    else if (arg === "--drop-after-chunks") args.dropAfterChunks = Number(next());
    else if (arg === "--reject-begin") args.rejectBegin = true;
    else if (arg === "--reject-commit") args.rejectCommit = true;
    else if (arg === "--mode") args.controlMode = next();
    else if (arg === "--auto-open") args.autoOpen = true;
    else if (arg === "--close-after-ms") args.closeAfterMs = Number(next());
    else if (arg === "--report") args.reportFile = path.resolve(next());
    else throw new Error(`Unknown argument: ${arg}`);
  }
  if (!Number.isInteger(args.port) || args.port < 1 || args.port > 65535) {
    throw new Error("--port must be an integer in [1,65535]");
  }
  if (!Number.isInteger(args.delayAckMs) || args.delayAckMs < 0 || args.delayAckMs > 30000) {
    throw new Error("--delay-ack-ms must be an integer in [0,30000]");
  }
  if (!Number.isInteger(args.dropAfterChunks) || args.dropAfterChunks < 0) {
    throw new Error("--drop-after-chunks must be a non-negative integer");
  }
  if (!Number.isInteger(args.closeAfterMs) || args.closeAfterMs < 0 || args.closeAfterMs > 86400000) {
    throw new Error("--close-after-ms must be an integer in [0,86400000]");
  }
  return args;
}

function printHelp() {
  console.log(`Usage:
  node tools/protocol/offline-audio-replay-gateway.js [options]

Options:
  --self-test                 Run local receiver tests and exit.
  --host HOST                 Listen address. Default 0.0.0.0.
  --advertised-host HOST      LAN address displayed by the audio dashboard.
  --discovery-config FILE     Enable paired PC gateway discovery on UDP 8811.
  --port N                    Listen port. Default 8810.
  --storage-dir DIR           Durable test storage directory.
  --delay-ack-ms N            Delay every ACK to test timeout handling.
  --drop-after-chunks N       Close each socket after N accepted chunks.
  --reject-begin              Return ok:false to every begin request.
  --reject-commit             Return ok:false to every commit request.
  --mode meeting|monitoring   Enable control and realtime audio validation.
  --auto-open                 Send set_audio_mode after hello.
  --close-after-ms N          Close each connection after N milliseconds.
  --report FILE               Write aggregate counters and bounded recent diagnostics asynchronously.
`);
}

function log(message, detail = {}) {
  console.log(`${new Date().toISOString()} ${message}${Object.keys(detail).length ? ` ${JSON.stringify(detail)}` : ""}`);
}

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function safePart(value, fallback = "unknown") {
  const text = String(value || "").trim();
  const safe = text.replace(/[^A-Za-z0-9._-]+/g, "_").slice(0, 96);
  return safe || fallback;
}

const { crc32Update, backend: crcBackend } = require("./audio-crc32");

function readU32(buffer, offset) {
  if (offset < 0 || offset + 4 > buffer.length) throw new Error("short u32");
  return buffer.readUInt32LE(offset);
}

function parseControl(value, name) {
  assert(value && typeof value === "object" && !Array.isArray(value), `${name} must be an object`);
  assert(typeof value.type === "string", `${name}.type is required`);
  return value;
}

function requireString(value, field) {
  assert(typeof value === "string" && value.length > 0 && value.length <= 127,
    `${field} must be a non-empty string <=127 bytes`);
  return value;
}

function requireUInt(value, field, max = 0xffffffff) {
  assert(Number.isInteger(value) && value >= 0 && value <= max, `${field} must be an unsigned integer`);
  return value;
}

function archiveCrc(buffer) {
  assert(buffer.length >= ARCHIVE_HEADER_BYTES + ARCHIVE_FOOTER_BYTES, "archive is too short");
  assert(buffer.subarray(0, 7).toString("ascii") === "NWXOFF1", "bad NWXOFF1 magic");
  assert(buffer.readUInt16LE(8) === 1 && buffer.readUInt16LE(10) === ARCHIVE_HEADER_BYTES,
    "unsupported archive header");
  const footerOffset = buffer.length - ARCHIVE_FOOTER_BYTES;
  const footer = buffer.subarray(footerOffset);
  assert(footer.subarray(0, 7).toString("ascii") === "NWXEND1", "bad NWXEND1 magic");
  assert(footer.readUInt16LE(8) === 1 && footer.readUInt16LE(10) === ARCHIVE_FOOTER_BYTES,
    "unsupported archive footer");
  let offset = ARCHIVE_HEADER_BYTES;
  let frameCount = 0;
  let payloadBytes = 0;
  let crc = 0;
  let firstSequence = null;
  let lastSequence = null;
  let missingFrames = 0;
  let sequenceDiscontinuities = 0;
  let sequenceFrames = 0;
  while (offset < footerOffset) {
    assert(offset + 4 <= footerOffset, "truncated archive record length");
    const length = readU32(buffer, offset);
    offset += 4;
    assert(length > 0 && length <= 2048 && offset + length <= footerOffset,
      "invalid archive record length");
    const payload = buffer.subarray(offset, offset + length);
    if (payload.length >= 22 && payload.toString("ascii", 0, 2) === "NA") {
      const sequence = payload.readUInt32LE(10);
      sequenceFrames++;
      if (firstSequence === null) firstSequence = sequence;
      if (lastSequence !== null && sequence !== ((lastSequence + 1) >>> 0)) {
        sequenceDiscontinuities++;
        if (sequence > lastSequence) missingFrames += sequence - lastSequence - 1;
      }
      lastSequence = sequence;
    }
    crc = crc32Update(crc, payload);
    payloadBytes += length;
    frameCount++;
    offset += length;
  }
  assert(offset === footerOffset, "archive payload does not end at footer");
  assert(readU32(footer, 12) === frameCount, "archive frame count mismatch");
  const footerPayloadBytes = Number(footer.readBigUInt64LE(16));
  assert(footerPayloadBytes === payloadBytes, "archive payload byte count mismatch");
  assert(readU32(footer, 24) === crc, "archive CRC mismatch");
  return { frameCount, payloadBytes, crc, complete: footer[28] === 1,
    firstSequence, lastSequence, missingFrames, sequenceDiscontinuities,
    sequenceVerified: sequenceFrames === frameCount };
}

function parseChunk(buffer) {
  assert(Buffer.isBuffer(buffer) && buffer.length >= CHUNK_HEADER_BYTES, "short NWXCH1 frame");
  assert(buffer.subarray(0, 6).toString("ascii") === "NWXCH1", "bad NWXCH1 magic");
  const version = readU32(buffer, 8);
  assert(version === 1 || version === 2, "unsupported NWXCH1 version");
  const offset = readU32(buffer, 12);
  const length = readU32(buffer, 16);
  assert(length > 0 && length <= CHUNK_PAYLOAD_BYTES, "invalid NWXCH1 payload length");
  assert(buffer.length === CHUNK_HEADER_BYTES + length, "NWXCH1 frame length mismatch");
  const requestId = buffer.subarray(20, 60).toString("ascii").replace(/\0.*$/, "");
  assert(requestId.length > 0, "NWXCH1 request id is empty");
  const flags = readU32(buffer, 60);
  assert(version === 1 || (flags & ~1) === 0, "unsupported window flags");
  return { offset, length, requestId, version, final: (flags & 1) !== 0,
    payload: buffer.subarray(CHUNK_HEADER_BYTES) };
}

class ReplayReceiver {
  constructor(storageDir, options = {}) {
    this.storageDir = storageDir;
    this.options = options;
    fs.mkdirSync(storageDir, { recursive: true });
    this.transactions = new Map();
    this.audioSessions = new AudioSessions(storageDir);
    void this.audioSessions.refreshes.start();
    this.scenes = new SceneReceiver(this, archiveCrc);
    this.stats = {
      begins: 0, chunks: 0, commits: 0, committedBytes: 0, crcFailures: 0, receivedBytes: 0,
      durableWrites: 0, durableWriteMs: 0, durableWriteMaxMs: 0, windows: 0,
      archives: [], archivesOmitted: 0, archivesLimit: ARCHIVE_LIMIT,
    };
  }

  key(deviceId, segmentId) {
    return `${safePart(deviceId)}__${safePart(segmentId)}`;
  }

  paths(deviceId, segmentId) {
    const base = this.key(deviceId, segmentId);
    return {
      part: path.join(this.storageDir, `${base}.part`),
      complete: path.join(this.storageDir, `${base}.complete`),
    };
  }

  registerHeader(tx, header, recovered = false) {
    if (!tx || tx.header || !header) return;
    tx.header = header;
    this.noteArchive(tx, header);
    log(recovered ? "offline archive header recovered" : "offline archive header",
      { deviceId: tx.deviceId, segmentId: tx.segmentId, ...header });
  }

  noteArchive(tx, fields) {
    let archive = this.stats.archives.find(item => item.deviceId === tx.deviceId && item.segmentId === tx.segmentId);
    if (!archive) {
      archive = { deviceId: tx.deviceId, segmentId: tx.segmentId, ...tx.header };
      this.stats.archives.push(archive);
    }
    Object.assign(archive, fields);
    const excess = this.stats.archives.length - ARCHIVE_LIMIT;
    if (excess > 0) {
      this.stats.archives.splice(0, excess);
      this.stats.archivesOmitted += excess;
    }
  }

  recoverHeader(tx) {
    if (!tx || tx.header || tx.nextOffset < ARCHIVE_HEADER_BYTES) return;
    const durablePath = fs.existsSync(tx.paths.complete)
      ? tx.paths.complete : tx.paths.part;
    if (!fs.existsSync(durablePath)) return;
    const fd = fs.openSync(durablePath, "r");
    try {
      const headerBytes = Buffer.alloc(ARCHIVE_HEADER_BYTES);
      const read = fs.readSync(fd, headerBytes, 0, headerBytes.length, 0);
      if (read === headerBytes.length &&
          headerBytes.subarray(0, 7).toString("ascii") === "NWXOFF1") {
        this.registerHeader(tx, {
          mode: headerBytes[12],
          sourceFlags: headerBytes.readUInt16LE(22),
          sampleRate: headerBytes.readUInt32LE(16),
          frameDurationMs: headerBytes.readUInt16LE(20),
          sessionId: headerBytes.subarray(36, 84).toString("ascii").replace(/\0.*$/, ""),
        }, true);
      }
    } finally {
      fs.closeSync(fd);
    }
  }

  begin(deviceId, message, connectionId) {
    this.stats.begins++;
    parseControl(message, "offline_audio_begin");
    const route = prefix(message.type, "begin");
    assert(route, "wrong begin type");
    const segmentId = this.audioSessions.fileIdentity(deviceId, message, route);
    const requestId = requireString(message.request_id, "request_id");
    const tupleIdentity = route !== "offline_audio" && message.segment_id === undefined;
    if (tupleIdentity) {
      assert(/^[A-Za-z0-9_-]{1,39}$/.test(requestId), "invalid binary request_id");
      assert(message.window_bytes === undefined && message.replay_version === undefined &&
        message.replay_revision === undefined, "new file transaction cannot negotiate a window");
    }
    const totalBytes = requireUInt(message.total_bytes, "total_bytes", MAX_ARCHIVE_BYTES);
    assert(totalBytes >= ARCHIVE_HEADER_BYTES + ARCHIVE_FOOTER_BYTES, "total_bytes is too small");
    const crc32 = requireUInt(message.crc32, "crc32");
    const ingestMode = route === "offline_audio" ? (message.mode === undefined ? "archive_only" : String(message.mode)) : undefined;
    assert(route !== "offline_audio" || ingestMode === "archive_only", "offline replay must use archive_only mode");
    const paths = this.paths(deviceId, segmentId);
    if (this.options.rejectBegin) {
      return { type: `${route}_begin_ack`, request_id: requestId, ...replyIdentity(message), ok: false, next_offset: 0 };
    }
    const session = this.audioSessions.begin(deviceId, { ...message, segment_id: segmentId }, paths, route, tupleIdentity);
    const fileAck = route !== "offline_audio" && message.window_bytes === undefined &&
      message.replay_version === undefined && message.replay_revision === undefined;
    // A live file may preempt history on this connection. Save its received
    // prefix before rebinding, including when begin retries the same file.
    for (const previous of this.transactions.values()) {
      if (previous.deviceId === deviceId && previous.fileAck && previous.dataVersion === 2)
        this.flushFile(previous);
    }
    let nextOffset = 0;
    if (fs.existsSync(paths.complete)) {
      const stat = fs.statSync(paths.complete);
      assert(stat.size === totalBytes, "completed segment length differs");
      nextOffset = totalBytes;
    } else if (fs.existsSync(paths.part)) {
      // A process may have died during a batch write, before its ACK. Make
      // the recovered prefix durable before advertising its length again.
      const fd = fs.openSync(paths.part, "r+");
      try { fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
      const stat = fs.statSync(paths.part);
      assert(stat.size <= totalBytes, "partial segment is larger than declared length");
      nextOffset = stat.size;
    }
    // Revision 1 devices have an unaligned PCM validation buffer in v2.
    // Keep those devices on their existing v1 path until reflashed.
    // New typed transactions use window_bytes alone. Explicit old revisions
    // retain their safety gate; legacy archive_only never opts in implicitly.
    const compact = fileAck || (route !== "offline_audio" && message.replay_version === undefined &&
      message.replay_revision === undefined && message.window_bytes !== undefined);
    const windowBytes = !fileAck && (compact || (message.replay_version === 2 && message.replay_revision === 2))
      ? Math.min(requireUInt(message.window_bytes, "window_bytes", 524288), 524288) : 0;
    assert(!windowBytes || windowBytes >= CHUNK_PAYLOAD_BYTES, "window too small");
    const tx = { deviceId, connectionId, segmentId, requestId, totalBytes, crc32, paths, nextOffset, chunks: 0, ingestMode, route, session, compact,
      fileAck, tupleIdentity, dataVersion: 0, fileFinal: false,
      windowBytes, pending: [], pendingBytes: 0, pendingOffset: 0, pendingRequest: "" };
    // A connection owns one transaction. Keep durable files, but discard
    // abandoned RAM windows when a new begin supersedes the old transfer.
    for (const [key, previous] of this.transactions) {
      if (previous.deviceId === deviceId) this.transactions.delete(key);
    }
    this.transactions.set(this.key(deviceId, segmentId), tx);
    /* A fully or partially durable transaction may resume without sending a
     * chunk at offset zero. Load its header before returning begin ACK so the
     * report remains complete even when the device jumps straight to commit. */
    this.recoverHeader(tx);
    log("offline begin", { deviceId, connectionId, segmentId, totalBytes, nextOffset,
      replayVersion: fileAck || windowBytes ? 2 : 1, compact, windowBytes, route, audioSessionId: tx.session?.metadata.sessionId,
      ackScope: fileAck ? "file_commit" : "legacy" });
    return { type: `${route}_begin_ack`, request_id: requestId, ok: true,
      mode: ingestMode, ...replyIdentity(message), next_offset: nextOffset,
      ...(windowBytes ? { ...(compact ? {} : { replay_version: 2 }), window_bytes: windowBytes } : {}) };
  }

  chunk(deviceId, buffer) {
    this.stats.chunks++;
    const chunk = parseChunk(buffer);
    const candidates = [...this.transactions.values()].filter(tx => tx.deviceId === deviceId);
    assert(candidates.length === 1, "no unique active transaction");
    const tx = candidates[0];
    if (tx.fileAck) {
      assert(!tx.tupleIdentity || chunk.version === 2, "new file transaction requires NWXCH1 version 2");
      assert(!tx.dataVersion || tx.dataVersion === chunk.version, "mixed file transfer formats");
      tx.dataVersion = chunk.version;
      if (chunk.version === 2) return this.fileChunk(tx, chunk);
      // Older installed firmware still sends per-packet data on a typed
      // begin without a window. Keep its existing ACK behavior until reflashed.
    }
    if (chunk.version === 2) return this.windowChunk(deviceId, chunk);
    return this.persistChunk(deviceId, chunk);
  }

  flushFile(tx) {
    if (!tx.pendingBytes) return;
    this.persistChunk(tx.deviceId, { offset: tx.pendingOffset, length: tx.pendingBytes,
      requestId: tx.requestId, payload: Buffer.concat(tx.pending, tx.pendingBytes) });
    tx.pending = []; tx.pendingBytes = 0;
  }

  fileChunk(tx, chunk) {
    assert(chunk.requestId === tx.requestId, "file packet differs from begin request");
    const end = chunk.offset + chunk.length;
    assert(end <= tx.totalBytes, "file packet exceeds archive");
    assert(!chunk.final || end === tx.totalBytes, "premature file end flag");
    if (chunk.offset < tx.nextOffset) {
      // Verify exact durable duplicates without appending or acknowledging.
      this.persistChunk(tx.deviceId, chunk);
      if (chunk.final) tx.fileFinal = true;
      return null;
    }
    assert(chunk.offset === tx.nextOffset + tx.pendingBytes, "noncontiguous file data");
    assert(!tx.fileFinal, "file data after final packet");
    if (!tx.pendingBytes) tx.pendingOffset = chunk.offset;
    tx.pending.push(Buffer.from(chunk.payload));
    tx.pendingBytes += chunk.length;
    if (chunk.final) tx.fileFinal = true;
    if (tx.pendingBytes >= FILE_FLUSH_BYTES || chunk.final) this.flushFile(tx);
    return null; // Only commit produces a file-receipt confirmation.
  }

  windowChunk(deviceId, chunk) {
    const candidates = [...this.transactions.values()].filter(tx => tx.deviceId === deviceId);
    assert(candidates.length === 1, "no unique window transaction");
    const tx = candidates[0];
    assert(tx.windowBytes > 0, "v2 not negotiated");
    assert(chunk.offset + chunk.length <= tx.totalBytes, "window exceeds archive");
    if (!tx.pendingBytes) {
      tx.pendingOffset = chunk.offset;
      tx.pendingRequest = chunk.requestId;
    }
    assert(chunk.requestId === tx.pendingRequest, "interleaved window request");
    assert(chunk.offset === tx.pendingOffset + tx.pendingBytes, "noncontiguous window");
    assert(tx.pendingBytes + chunk.length <= tx.windowBytes, "window limit exceeded");
    tx.pending.push(Buffer.from(chunk.payload));
    tx.pendingBytes += chunk.length;
    if (!chunk.final) return null;
    const batch = { offset: tx.pendingOffset, length: tx.pendingBytes,
      requestId: tx.pendingRequest, payload: Buffer.concat(tx.pending, tx.pendingBytes) };
    // Persist the entire bounded batch before returning a cumulative ACK.
    const ack = this.persistChunk(deviceId, batch);
    this.stats.windows++;
    tx.pending = []; tx.pendingBytes = 0; tx.pendingRequest = "";
    return ack;
  }

  persistChunk(deviceId, chunk) {
    const started = process.hrtime.bigint();
    const candidates = [...this.transactions.values()].filter((tx) => tx.deviceId === deviceId);
    assert(candidates.length === 1, "no unique active offline transaction");
    const tx = candidates[0];
    assert(chunk.offset <= tx.totalBytes && chunk.offset + chunk.length <= tx.totalBytes,
      "chunk exceeds declared archive length");
    if (chunk.offset < tx.nextOffset) {
      assert(chunk.offset + chunk.length <= tx.nextOffset, "chunk overlaps an unknown offset");
      const durablePath = fs.existsSync(tx.paths.complete)
        ? tx.paths.complete : tx.paths.part;
      const existing = fs.readFileSync(durablePath).subarray(
        chunk.offset, chunk.offset + chunk.length);
      assert(existing.equals(chunk.payload), "duplicate chunk bytes differ from durable prefix");
    } else {
      assert(chunk.offset === tx.nextOffset, `chunk offset ${chunk.offset} expected ${tx.nextOffset}`);
      /* Use positional writes. O_APPEND is deliberately avoided: on some
       * platforms it ignores the position argument and would turn a retry
       * into an accidental duplicate suffix. */
      const fd = fs.openSync(tx.paths.part, fs.existsSync(tx.paths.part) ? "r+" : "w+");
      try {
        let written = 0;
        while (written < chunk.length) {
          const count = fs.writeSync(fd, chunk.payload, written,
            chunk.length - written, chunk.offset + written);
          assert(count > 0, "archive write made no progress");
          written += count;
        }
        fs.fsyncSync(fd);
        this.stats.durableWrites++;
      } finally {
        fs.closeSync(fd);
      }
      tx.nextOffset += chunk.length;
      this.stats.receivedBytes += chunk.length;
      tx.chunks++;
      if (!tx.header && chunk.offset === 0 && chunk.length >= ARCHIVE_HEADER_BYTES &&
          chunk.payload.subarray(0, 7).toString("ascii") === "NWXOFF1") {
        this.registerHeader(tx, {
          mode: chunk.payload[12],
          sourceFlags: chunk.payload.readUInt16LE(22),
          sampleRate: chunk.payload.readUInt32LE(16),
          frameDurationMs: chunk.payload.readUInt16LE(20),
          sessionId: chunk.payload.subarray(36, 84).toString("ascii").replace(/\0.*$/, ""),
        });
      }
    }
    /* A resumed begin may start at a non-zero offset, so the current chunk
     * cannot carry the archive header. Recover it from the durable prefix so
     * report metadata remains complete after a reconnect; this does not alter
     * the byte-level resume or CRC checks. */
    this.recoverHeader(tx);
    const persistMs = Number(process.hrtime.bigint() - started) / 1e6;
    this.stats.durableWriteMs += persistMs;
    this.stats.durableWriteMaxMs = Math.max(this.stats.durableWriteMaxMs, persistMs);
    log("offline chunk", { deviceId, connectionId: tx.connectionId, segmentId: tx.segmentId, offset: chunk.offset, bytes: chunk.length, nextOffset: tx.nextOffset,
      persistMs: Math.round(persistMs * 1000) / 1000 });
    return { type: `${tx.route}_chunk_ack`, request_id: chunk.requestId, ok: true, next_offset: tx.nextOffset, tx };
  }

  commit(deviceId, message) {
    this.stats.commits++;
    parseControl(message, "offline_audio_commit");
    const route = prefix(message.type, "commit");
    assert(route, "wrong commit type");
    const segmentId = this.audioSessions.fileIdentity(deviceId, message, route);
    const requestId = requireString(message.request_id, "request_id");
    const tx = this.transactions.get(this.key(deviceId, segmentId));
    const compact = tx?.compact === true;
    const totalBytes = requireUInt(compact && message.total_bytes === undefined ? tx.totalBytes : message.total_bytes, "total_bytes", MAX_ARCHIVE_BYTES);
    const crc32 = requireUInt(compact && message.crc32 === undefined ? tx.crc32 : message.crc32, "crc32");
    const ingestMode = route === "offline_audio" ? (message.mode === undefined ? "archive_only" : String(message.mode)) : undefined;
    assert(route !== "offline_audio" || ingestMode === "archive_only", "offline replay must use archive_only mode");
    const key = this.key(deviceId, segmentId);
    if (this.options.rejectCommit) {
      return { type: `${route}_commit_ack`, request_id: requestId, ok: false,
        mode: ingestMode, ...replyIdentity(message) };
    }
    assert(tx, "commit has no matching begin");
    assert(tx.tupleIdentity === (route !== "offline_audio" && message.segment_id === undefined), "commit identity format differs from begin");
    if (message.audio_session_id !== undefined || message.segment_index !== undefined)
      assert(tx.session?.metadata.sessionId === message.audio_session_id &&
        tx.session?.metadata.segmentIndex === message.segment_index, "commit file identity differs from begin");
    assert(tx.route === route, "commit type differs from begin");
    assert(tx.totalBytes === totalBytes && tx.crc32 === crc32, "commit metadata differs from begin");
    if (tx.fileAck && tx.dataVersion === 2) {
      assert(tx.fileFinal, "commit before final file packet");
      this.flushFile(tx);
    }
    assert(tx.nextOffset === totalBytes, `commit incomplete at ${tx.nextOffset}/${totalBytes}`);
    /* A lost commit ACK causes the device to retry begin/commit after
     * reconnect. Treat an already finalized file as an idempotent commit. */
    const finalized = fs.existsSync(tx.paths.complete);
    const data = fs.readFileSync(finalized ? tx.paths.complete : tx.paths.part);
    try {
      assert(data.length === totalBytes, "persisted archive length mismatch");
      const summary = archiveCrc(data);
      assert(summary.crc === crc32, "commit CRC mismatch");
      this.audioSessions.validate(tx, data);
      if (!finalized) {
        fs.renameSync(tx.paths.part, tx.paths.complete);
        this.stats.committedBytes += totalBytes;
      }
      this.audioSessions.commit(tx, summary);
      this.transactions.delete(key);
      this.noteArchive(tx, { ...summary, committed: true, totalBytes });
      log("offline commit", { deviceId, connectionId: tx.connectionId, segmentId, totalBytes, ...summary });
      if (this.options.persistReport) this.options.persistReport();
      return { type: `${route}_commit_ack`, request_id: requestId, ok: true,
        mode: ingestMode, ...replyIdentity(message) };
    } catch (error) {
      this.stats.crcFailures++;
      /* An integrity failure means the durable prefix itself is not
       * trustworthy. Remove only that corrupt temporary object; transient
       * disconnects never reach this branch and remain resumable. */
      if (!finalized) {
        try { fs.rmSync(tx.paths.part, { force: true }); } catch { /* best effort */ }
      }
      this.transactions.delete(key);
      throw error;
    }
  }
}

function wsFrame(opcode, payload) {
  const body = Buffer.from(payload || "");
  let header;
  if (body.length < 126) header = Buffer.from([0x80 | opcode, body.length]);
  else if (body.length <= 0xffff) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode;
    header[1] = 126;
    header.writeUInt16BE(body.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(body.length), 2);
  }
  return Buffer.concat([header, body]);
}

function readWsFrame(buffer) {
  if (buffer.length < 2) return null;
  const opcode = buffer[0] & 0x0f;
  const masked = (buffer[1] & 0x80) !== 0;
  let length = buffer[1] & 0x7f;
  let offset = 2;
  if (length === 126) {
    if (buffer.length < offset + 2) return null;
    length = buffer.readUInt16BE(offset); offset += 2;
  } else if (length === 127) {
    if (buffer.length < offset + 8) return null;
    const big = buffer.readBigUInt64BE(offset); offset += 8;
    if (big > BigInt(MAX_FRAME_BYTES)) throw new Error("WebSocket frame too large");
    length = Number(big);
  }
  const maskOffset = masked ? 4 : 0;
  if (buffer.length < offset + maskOffset + length) return null;
  const mask = masked ? buffer.subarray(offset, offset + 4) : null;
  offset += maskOffset;
  let payload = Buffer.from(buffer.subarray(offset, offset + length));
  if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
  assert((buffer[0] & 0x70) === 0 && length <= MAX_FRAME_BYTES, "invalid WebSocket frame");
  return { frame: { opcode, payload, fin: (buffer[0] & 0x80) !== 0 }, rest: buffer.subarray(offset + length) };
}

class ReplayConnections {
  constructor(connections) {
    this.connections = connections;
    this.latest = new Map();
    this.sequence = 0;
  }

  register(connection) {
    connection.connectionId = ++this.sequence;
    this.connections.add(connection);
  }

  claim(connection) {
    const previous = this.latest.get(connection.deviceId);
    // Acceptance order also covers a legacy socket that sends hello late.
    // It must not reclaim a device already identified on a newer socket.
    if (previous && previous.id > connection.connectionId) {
      connection.shutdown("stale_identity");
      return false;
    }
    this.latest.set(connection.deviceId, { id: connection.connectionId, connection });
    if (previous?.connection && previous.connection !== connection) {
      previous.connection.shutdown("superseded");
    }
    this.prune();
    return true;
  }

  owns(connection) {
    return this.latest.get(connection.deviceId)?.connection === connection;
  }

  release(connection) {
    this.connections.delete(connection);
    const current = this.latest.get(connection.deviceId);
    if (current?.connection === connection) current.connection = null;
    this.prune();
  }

  prune() {
    // Retain a closed generation only while an older unidentified socket can
    // still send a late hello. Normal disconnects leave no registry entry.
    let oldestUnknown = Infinity;
    for (const connection of this.connections) {
      if (!connection.deviceId) oldestUnknown = Math.min(oldestUnknown, connection.connectionId);
    }
    for (const [deviceId, current] of this.latest) {
      if (!current.connection && current.id < oldestUnknown) this.latest.delete(deviceId);
    }
  }
}

class ReplayConnection {
  constructor(socket, receiver, options, report, deviceId, connections) {
    this.socket = socket;
    this.receiver = receiver;
    this.options = options;
    this.buffer = Buffer.alloc(0);
    this.deviceId = "";
    this.closed = false;
    this.chunks = 0;
    this.realtimeFrames = 0;
    this.realtimeBytes = 0;
    this.lastSequence = null;
    this.audioGaps = [];
    this.audioSessions = new Set();
    this.report = report;
    this.closeTimer = null;
    this.shutdownTimer = null;
    this.ackTimers = new Set();
    this.transaction = null;
    this.connections = connections;
    connections.register(this);
    socket.setNoDelay(true);
    socket.on("data", (data) => this.onData(data));
    // Upgraded HTTP sockets allow half-open TCP by default. A FIN otherwise
    // never reaches close and retains both the connection and its RAM window.
    socket.on("end", () => this.shutdown("peer_eof"));
    socket.on("close", () => {
      if (this.shutdownTimer) clearTimeout(this.shutdownTimer);
      this.release("socket_close");
    });
    socket.on("error", (error) => {
      log("offline socket error", { connectionId: this.connectionId, deviceId: this.deviceId, error: error.message });
      this.shutdown("socket_error");
    });
    if (deviceId) this.identifyDevice(deviceId);
  }

  release(reason) {
    if (this.closed) return false;
    this.closed = true;
    this.buffer = Buffer.alloc(0);
    this.fragment = null;
    this.receiver.scenes.release(this.deviceId, this.connectionId);
    if (this.transaction) {
      // Only this connection's abandoned RAM is discarded. Durable .part and
      // .complete files, and a successor's transaction, remain untouched.
      this.transaction.pending = [];
      this.transaction.pendingBytes = 0;
      this.transaction.pendingRequest = "";
      this.transaction = null;
    }
    if (this.closeTimer) clearTimeout(this.closeTimer);
    for (const timer of this.ackTimers) clearTimeout(timer);
    this.ackTimers.clear();
    this.connections.release(this);
    const summary = { deviceId: this.deviceId, connectionId: this.connectionId, reason,
      realtimeFrames: this.realtimeFrames, realtimeBytes: this.realtimeBytes,
      audioGaps: this.audioGaps, replayChunks: this.chunks };
    log("device session summary", summary);
    if (this.report) {
      this.report.sessions.push(summary);
      const excess = this.report.sessions.length - CONNECTION_LIMIT;
      if (excess > 0) {
        this.report.sessions.splice(0, excess);
        this.report.sessionsOmitted = (this.report.sessionsOmitted || 0) + excess;
      }
      if (this.options.persistReport) this.options.persistReport();
    }
    return true;
  }

  shutdown(reason, closePayload) {
    const first = this.release(reason);
    if (first && closePayload !== undefined && !this.socket.destroyed) {
      this.socket.end(wsFrame(0x8, closePayload));
      // A client that never returns FIN must not retain an upgraded socket.
      this.shutdownTimer = setTimeout(() => this.socket.destroy(), 1000);
      this.shutdownTimer.unref();
    } else {
      this.socket.destroy();
    }
  }

  identifyDevice(deviceId) {
    if (this.closed) return false;
    assert(!this.deviceId || this.deviceId === deviceId, "connection device identity cannot change");
    if (this.deviceId) return this.connections.owns(this);
    this.deviceId = requireString(deviceId, "device_id");
    if (!this.connections.claim(this)) return false;
    log("device connected", { deviceId: this.deviceId, connectionId: this.connectionId,
      remoteAddress: this.socket.remoteAddress, remotePort: this.socket.remotePort });
    if (this.options.closeAfterMs > 0) {
      this.closeTimer = setTimeout(() => {
        if (!this.closed) {
          log("fault injection: closing connection", { deviceId: this.deviceId });
          this.shutdown("fault_close_timer");
        }
      }, this.options.closeAfterMs);
    }
    return true;
  }

  sendJson(message) {
    if (!this.closed && !this.socket.destroyed && !this.socket.writableEnded &&
        (!this.deviceId || this.connections.owns(this))) {
      this.socket.write(wsFrame(0x1, JSON.stringify(message)));
    }
  }

  sendJsonDelayed(message) {
    if (this.closed) return;
    const delay = this.options.delayAckMs;
    if (delay > 0) {
      const timer = setTimeout(() => {
        this.ackTimers.delete(timer);
        this.sendJson(message);
      }, delay);
      this.ackTimers.add(timer);
    } else this.sendJson(message);
  }

  onData(data) {
    if (this.closed) return;
    try {
      this.buffer = Buffer.concat([this.buffer, data]);
      while (!this.closed) {
        const parsed = readWsFrame(this.buffer);
        if (!parsed) return;
        this.buffer = parsed.rest;
        this.handleFrame(parsed.frame);
      }
    } catch (error) {
      log("offline protocol error", { connectionId: this.connectionId, deviceId: this.deviceId, error: error.message });
      this.shutdown("protocol_error");
    }
  }

  handleFrame(frame) {
    if (this.closed) return;
    assert(!this.deviceId || this.connections.owns(this), "connection no longer owns the device");
    if (frame.opcode === 0x8) {
      this.shutdown("websocket_close", frame.payload);
      return;
    }
    if (frame.opcode === 0x9) { this.socket.write(wsFrame(0xA, frame.payload)); return; }
    if (frame.opcode === 0xA) return;
    if (frame.opcode === 0) {
      assert(this.fragment, "unexpected WebSocket continuation");
      this.fragment.bytes += frame.payload.length;
      assert(this.fragment.bytes <= MAX_FRAME_BYTES, "WebSocket message too large");
      this.fragment.parts.push(frame.payload);
      if (!frame.fin) return;
      frame = { opcode: this.fragment.opcode, payload: Buffer.concat(this.fragment.parts), fin: true };
      this.fragment = null;
    } else {
      assert(!this.fragment, "interleaved WebSocket messages");
      if (!frame.fin) {
        assert(frame.opcode === 1 || frame.opcode === 2, "invalid fragmented message");
        this.fragment = { opcode: frame.opcode, parts: [frame.payload], bytes: frame.payload.length };
        return;
      }
    }
    if (frame.opcode === 0x1) return this.handleText(frame.payload.toString("utf8"));
    if (frame.opcode === 0x2) return this.handleBinary(frame.payload);
    throw new Error(`unsupported WebSocket opcode ${frame.opcode}`);
  }

  handleText(text) {
    let message;
    try {
      message = parseControl(JSON.parse(text), "text");
    } catch (error) {
      this.sendJson({ type: "error", error: error.message });
      return;
    }
    if (message.type === "hello") {
      // Legacy/online test clients still use their ordinary connection hello.
      if (!this.identifyDevice(message.device_id || "unknown-device")) return;
      this.sendJson({ type: "hello", ok: true, session_id: "offline-replay-test", transport: "cloud-websocket", mode: "cloud-pc-relay" });
      if (this.options.autoOpen && (this.options.controlMode === "meeting" || this.options.controlMode === "monitoring")) {
        this.sendJson({ type: "device_command", command: "set_audio_mode", request_id: `test_open_${Date.now()}`, mode: this.options.controlMode, state: "enabled" });
      }
      return;
    }
    if (message.type === "ping") {
      this.sendJson({ type: "pong", now_ms: Number(message.now_ms ?? Date.now()), server_ms: Date.now() });
      return;
    }
    if (message.type === "listen" || message.type === "monitoring_capture_control") {
      if (message.type === "listen" && message.state === "start") this.lastSequence = null;
      this.sendJson({ type: message.type === "listen" ? "listen_ack" : "monitoring_capture_control_ack", request_id: message.request_id || "", mode: message.mode || this.options.controlMode, state: message.state, ok: true, audio_session_id: message.audio_session_id || "" });
      log("control accepted", { deviceId: this.deviceId, type: message.type, state: message.state, session: message.audio_session_id || "" });
      return;
    }
    if (message.type === "audio_mode_state" || message.type === "device_command_ack") {
      log("device state", { deviceId: this.deviceId, type: message.type, mode: message.mode, state: message.state, ok: message.ok });
      return;
    }
    if (!this.deviceId) throw new Error("device identity required before protocol messages");
    const sceneBegin = isSceneBegin(message);
    const sceneBound = message.audio_session_id && this.receiver.scenes.bound(this.deviceId, message.audio_session_id);
    if (sceneBegin || (sceneBound && (prefix(message.type, "commit") || /_audio_session_end$/.test(message.type)))) {
      try {
        const result = sceneBegin ? this.receiver.scenes.begin(this.deviceId, message, this.connectionId) :
          /_audio_session_end$/.test(message.type) ? this.receiver.scenes.end(this.deviceId, message, this.connectionId) :
          this.receiver.scenes.commit(this.deviceId, message, this.connectionId);
        Promise.resolve(result).then(ack => {
          log("SD scene control accepted", { deviceId: this.deviceId, type: message.type,
            session: message.audio_session_id, index: message.segment_index });
          this.sendJsonDelayed(ack);
        }).catch(error => this.sendJsonDelayed(sceneFailure(message, error)));
      } catch (error) {
        log("SD scene control rejected", { deviceId: this.deviceId, type: message.type, error: error.message });
        this.sendJsonDelayed(sceneFailure(message, error));
      }
      return;
    }
    if (prefix(message.type, "begin")) {
      try {
        const ack = this.receiver.begin(this.deviceId, message, this.connectionId);
        if (ack.ok) this.transaction = this.receiver.transactions.get(this.receiver.key(this.deviceId,
          this.receiver.audioSessions.fileIdentity(this.deviceId, message, prefix(message.type, "begin"))));
        this.sendJsonDelayed(ack);
      } catch (error) {
        log("offline begin rejected", { deviceId: this.deviceId, error: error.message });
        this.sendJsonDelayed({ type: `${message.type}_ack`, ...replyIdentity(message), request_id: String(message.request_id || ""), ok: false, next_offset: 0 });
      }
      return;
    }
    if (prefix(message.type, "commit")) {
      try {
        this.sendJsonDelayed(this.receiver.commit(this.deviceId, message));
      } catch (error) {
        log("offline commit rejected", { deviceId: this.deviceId, segmentId: String(message.segment_id || ""), error: error.message });
        this.sendJsonDelayed({ type: `${message.type}_ack`, ...replyIdentity(message), request_id: String(message.request_id || ""), ok: false });
      }
      return;
    }
    if (message.type === "meeting_audio_session_end" || message.type === "monitoring_audio_session_end") {
      this.receiver.audioSessions.endAsync(this.deviceId, message).then(ack => {
        log("SD session durable end", { deviceId: this.deviceId, session: message.audio_session_id,
          segments: message.segment_count, recordingComplete: message.recording_complete });
        this.sendJsonDelayed(ack);
      }).catch(error => {
        log("SD session end rejected", { deviceId: this.deviceId, session: message.audio_session_id, error: error.message });
        this.sendJsonDelayed({ type: `${message.type}_ack`, request_id: message.request_id,
          audio_session_id: message.audio_session_id, ok: false, error: error.message });
      });
      return;
    }
    /* The local endpoint is intentionally not a conversation service. Keep
     * unrelated device traffic harmless while the replay test is running. */
    this.sendJson({ type: "ack", received_type: message.type || "unknown" });
  }

  handleBinary(payload) {
    if (!this.deviceId) throw new Error("device identity required before binary messages");
    if (payload.length >= 22 && payload[0] === 0x4e && payload[1] === 0x41 && payload[2] === 1) {
      const frameLength = 22 + payload.readUInt16LE(18);
      if (frameLength !== payload.length) throw new Error(`invalid NA frame length ${payload.length}/${frameLength}`);
      const sequence = payload.readUInt32LE(10);
      const timestamp = payload.readUInt32LE(14);
      if (this.lastSequence !== null && sequence !== this.lastSequence + 1) this.audioGaps.push({ from: this.lastSequence + 1, to: sequence - 1 });
      this.lastSequence = sequence;
      this.realtimeFrames++;
      this.realtimeBytes += payload.length;
      log("realtime audio", { deviceId: this.deviceId, sequence, timestamp, bytes: payload.length, gaps: this.audioGaps.length });
      return;
    }
    const scenePacket = payload.length >= 12 && payload.toString("ascii", 0, 6) === "NWXCH1" && payload.readUInt32LE(8) === 3;
    if (!scenePacket) assert(this.transaction && this.receiver.transactions.get(
      this.receiver.key(this.deviceId, this.transaction.segmentId)) === this.transaction,
      "connection no longer owns the replay transaction");
    const ack = scenePacket ? this.receiver.scenes.chunk(this.deviceId, payload, this.connectionId) : this.receiver.chunk(this.deviceId, payload);
    this.chunks++;
    if (ack) this.sendJsonDelayed({
      type: ack.type,
      request_id: ack.request_id,
      ok: ack.ok,
      ...(ack.tx && ack.tx.route === "offline_audio" ? { mode: "archive_only" } : {}),
      segment_id: ack.tx ? ack.tx.segmentId : undefined,
      next_offset: ack.next_offset,
    });
    if (this.options.dropAfterChunks > 0 && this.chunks >= this.options.dropAfterChunks) {
      log("fault injection: closing after chunk", { chunks: this.chunks });
      this.shutdown("fault_drop_after_chunks");
    }
  }
}

function startServer(args) {
  const receiver = new ReplayReceiver(args.storageDir, args);
  const report = {
    schema: "nuwax.test-gateway.report.v1",
    startedAt: new Date().toISOString(),
    updatedAt: new Date().toISOString(),
    options: { mode: args.controlMode, autoOpen: args.autoOpen },
    sessions: [],
  };
  const loopDelay = monitorEventLoopDelay({ resolution: 20 });
  loopDelay.enable();
  let runtimeAt = performance.now(), runtimeBytes = 0, runtimeDiskMs = 0;
  let runtimeUtilization = performance.eventLoopUtilization();
  const runtime = { eventLoopMaxMs: 0, recentUploadIntervals: [] };
  args.runtimeStatus = () => runtime;
  const reportWriter = new GatewayReportWriter(args.reportFile, () => ({
    ...report, sessions: report.sessions.slice(-CONNECTION_LIMIT),
    replay: { ...receiver.stats, archives: receiver.stats.archives.slice(-ARCHIVE_LIMIT) },
  }), error => log("gateway report write failed", { error: error.message }));
  args.flushReport = () => reportWriter.flush();
  args.closeReport = () => reportWriter.close();
  args.persistReport = () => {
    const now = performance.now(), intervalMs = now - runtimeAt;
    if (intervalMs >= 900) {
      const utilization = performance.eventLoopUtilization();
      const activeMs = utilization.active - runtimeUtilization.active;
      const idleMs = utilization.idle - runtimeUtilization.idle;
      const loopMaxMs = loopDelay.count ? loopDelay.max / 1e6 : 0;
      runtime.eventLoopMaxMs = Math.max(runtime.eventLoopMaxMs, loopMaxMs);
      const bytes = receiver.stats.receivedBytes - runtimeBytes;
      if (bytes || receiver.transactions.size) {
        runtime.recentUploadIntervals.push({ at: new Date().toISOString(),
          intervalMs: Math.round(intervalMs), receivedBytes: bytes,
          eventLoopMaxMs: Math.round(loopMaxMs * 1000) / 1000,
          eventLoopBusyPercent: Math.round(1000 * activeMs / Math.max(1, activeMs + idleMs)) / 10,
          durableWriteMs: Math.round((receiver.stats.durableWriteMs - runtimeDiskMs) * 1000) / 1000 });
        if (runtime.recentUploadIntervals.length > 120) runtime.recentUploadIntervals.shift();
      }
      runtimeAt = now; runtimeBytes = receiver.stats.receivedBytes;
      runtimeDiskMs = receiver.stats.durableWriteMs; runtimeUtilization = utilization;
      loopDelay.reset();
    }
    report.runtime = runtime;
    report.replay = receiver.stats;
    report.pending = [...receiver.transactions.values()].map((tx) => ({
      deviceId: tx.deviceId, segmentId: tx.segmentId,
      nextOffset: tx.nextOffset, totalBytes: tx.totalBytes,
    }));
    report.updatedAt = new Date().toISOString();
    reportWriter.request();
  };
  // Publish active transactions even if the peer never closes cleanly.
  const reportTimer = setInterval(args.persistReport, 1000);
  reportTimer.unref();
  const connections = new Set();
  const connectionOwners = new ReplayConnections(connections);
  const web = createAudioWeb(args.storageDir, receiver, connections, args);
  const server = http.createServer(web.handle);
  server.on("upgrade", (request, socket, head) => {
      socket.on("error", (error) => log("gateway socket error", { error: error.message }));
      const key = request.headers["sec-websocket-key"];
      if (!key || request.method !== "GET" || request.headers.upgrade?.toLowerCase() !== "websocket" ||
          request.headers["sec-websocket-version"] !== "13") { socket.destroy(); return; }
      // LAN test routing only; production resolves identity from authentication.
      // No application hello/ACK is required before offline_audio_begin.
      let deviceId = "";
      try {
        const target = new URL(request.url, "http://localhost");
        assert(target.pathname === "/api/device/ws", "invalid WebSocket endpoint");
        const ids = target.searchParams.getAll("device_id");
        assert(ids.length <= 1, "duplicate device_id");
        if (ids.length) {
          assert(/^[A-Za-z0-9._-]{1,96}$/.test(ids[0]), "invalid device_id");
          deviceId = ids[0];
        }
      } catch (error) {
        log("test gateway identity rejected", { error: error.message });
        socket.destroy();
        return;
      }
      const accept = crypto.createHash("sha1").update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`).digest("base64");
      socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
      const connection = new ReplayConnection(socket, receiver, args, report, deviceId, connectionOwners);
      if (head.length) connection.onData(head);
  });
  let discovery = null;
  server.on("close", () => {
    clearInterval(reportTimer); loopDelay.disable(); web.close(); discovery?.close();
    args.persistReport(); void args.closeReport();
    void receiver.audioSessions.close();
  });
  let shuttingDown = false;
  const shutdown = (signal) => {
    if (shuttingDown) return;
    shuttingDown = true;
    args.persistReport();
    log("offline replay gateway stopping", { signal });
    server.close(() => { void args.closeReport().finally(() => process.exit(0)); });
    // A stuck client must not prevent the supervisor from restarting us.
    setTimeout(() => process.exit(0), 2000).unref();
  };
  process.once("SIGINT", () => shutdown("SIGINT"));
  process.once("SIGTERM", () => shutdown("SIGTERM"));
  server.on("error", (error) => {
    log("gateway server error", { error: error.message });
    args.persistReport();
    // A listener error (for example a missing interface or occupied port)
    // leaves no usable gateway. Exit so the supervisor can retry cleanly.
    void args.closeReport().finally(() => process.exit(1));
  });
  server.listen(args.port, args.host, () => {
    if (args.discoveryConfig) {
      discovery = startDiscovery(args.discoveryConfig, log);
      args.discoveryStatus = discovery.status;
    }
    args.persistReport();
    log("offline replay test gateway listening", { host: args.host, port: args.port, storageDir: args.storageDir, crcBackend });
  });
  return server;
}

function makeArchive() {
  const header = Buffer.alloc(ARCHIVE_HEADER_BYTES);
  header.write("NWXOFF1", 0, "ascii");
  header.writeUInt16LE(1, 8);
  header.writeUInt16LE(ARCHIVE_HEADER_BYTES, 10);
  header[12] = 0; // meeting
  header[13] = 1; // channels
  header.writeUInt16LE(1, 14); // PCM16
  header.writeUInt32LE(16000, 16);
  header.writeUInt16LE(20, 20);
  header.write("self_test", 48, "ascii");
  const payloads = [
    Buffer.alloc(40, 0x11),
    Buffer.alloc(2048, 0x22),
    Buffer.alloc(2048, 0x24),
    Buffer.alloc(2048, 0x26),
    Buffer.alloc(2048, 0x28),
    Buffer.alloc(40, 0x33),
  ];
  const records = payloads.map((payload) => {
    const length = Buffer.alloc(4); length.writeUInt32LE(payload.length, 0);
    return Buffer.concat([length, payload]);
  });
  let crc = 0;
  for (const payload of payloads) crc = crc32Update(crc, payload);
  const footer = Buffer.alloc(ARCHIVE_FOOTER_BYTES);
  footer.write("NWXEND1", 0, "ascii");
  footer.writeUInt16LE(1, 8);
  footer.writeUInt16LE(ARCHIVE_FOOTER_BYTES, 10);
  footer.writeUInt32LE(payloads.length, 12);
  footer.writeBigUInt64LE(BigInt(payloads.reduce((sum, item) => sum + item.length, 0)), 16);
  footer.writeUInt32LE(crc, 24);
  footer[28] = 1;
  return Buffer.concat([header, ...records, footer]);
}

function makeChunk(archive, offset, requestId, payloadLimit = CHUNK_PAYLOAD_BYTES) {
  const length = Math.min(payloadLimit, archive.length - offset);
  const header = Buffer.alloc(CHUNK_HEADER_BYTES);
  header.write("NWXCH1", 0, "ascii");
  header.writeUInt32LE(1, 8);
  header.writeUInt32LE(offset, 12);
  header.writeUInt32LE(length, 16);
  header.write(requestId.slice(0, 40), 20, "ascii");
  return Buffer.concat([header, archive.subarray(offset, offset + length)]);
}

function runSelfTest() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-offline-replay-"));
  try {
    const receiver = new ReplayReceiver(dir);
    const archive = makeArchive();
    const deviceId = "self-test-device";
    const segmentId = "offline_meeting_self_test_0001";
    const begin = receiver.begin(deviceId, {
      type: "offline_audio_begin", request_id: "begin_1", segment_id: segmentId,
      total_bytes: archive.length, crc32: archive.readUInt32LE(archive.length - 8),
    });
    assert(begin.ok && begin.next_offset === 0, "begin did not start at zero");
    const first = receiver.chunk(deviceId, makeChunk(archive, 0, "chunk_1"));
    assert(first.next_offset === CHUNK_PAYLOAD_BYTES, "first chunk did not stop at the chunk limit");
    const second = receiver.chunk(deviceId, makeChunk(archive, first.next_offset, "chunk_1b"));
    assert(second.next_offset === archive.length, "second chunk did not advance to EOF");
    const commit = receiver.commit(deviceId, {
      type: "offline_audio_commit", request_id: "commit_1", segment_id: segmentId,
      total_bytes: archive.length, crc32: archive.readUInt32LE(archive.length - 8),
    });
    assert(commit.ok && fs.existsSync(path.join(dir, "self-test-device__offline_meeting_self_test_0001.complete")), "commit did not finalize archive");

    const resumeSegment = "offline_meeting_resume_0001";
    const resumeCrc = archive.readUInt32LE(archive.length - 8);
    receiver.begin(deviceId, { type: "offline_audio_begin", request_id: "begin_2", segment_id: resumeSegment, total_bytes: archive.length, crc32: resumeCrc });
    const split = 32;
    receiver.chunk(deviceId, makeChunk(archive, 0, "chunk_2"));
    const resumePath = receiver.paths(deviceId, resumeSegment).part;
    /* Simulate a reconnect after a durable prefix. */
    const partial = fs.readFileSync(resumePath).subarray(0, split);
    fs.writeFileSync(resumePath, partial);
    receiver.transactions.delete(receiver.key(deviceId, resumeSegment));
    const resumed = receiver.begin(deviceId, { type: "offline_audio_begin", request_id: "begin_3", segment_id: resumeSegment, total_bytes: archive.length, crc32: resumeCrc });
    assert(resumed.next_offset === split, "begin did not return durable resume offset");
    while (receiver.transactions.get(receiver.key(deviceId, resumeSegment)).nextOffset < archive.length) {
      const offset = receiver.transactions.get(receiver.key(deviceId, resumeSegment)).nextOffset;
      receiver.chunk(deviceId, makeChunk(archive, offset, `chunk_3_${offset}`));
    }
    const resumedCommit = receiver.commit(deviceId, { type: "offline_audio_commit", request_id: "commit_2", segment_id: resumeSegment, total_bytes: archive.length, crc32: resumeCrc });
    assert(resumedCommit.ok, "resumed commit failed");

    const shortSegment = "offline_short_chunks_0001";
    receiver.begin(deviceId, { type: "offline_audio_begin", mode: "archive_only",
      request_id: "begin_short", segment_id: shortSegment,
      total_bytes: archive.length, crc32: resumeCrc });
    let shortOffset = 0;
    while (shortOffset < archive.length) {
      const ack = receiver.chunk(deviceId,
        makeChunk(archive, shortOffset, `short_${shortOffset}`, 1024));
      assert(ack.next_offset > shortOffset && ack.next_offset <= shortOffset + 1024,
        "short archive chunk ACK advanced beyond the transmitted bytes");
      shortOffset = ack.next_offset;
    }
    assert(receiver.commit(deviceId, { type: "offline_audio_commit", mode: "archive_only",
      request_id: "commit_short", segment_id: shortSegment,
      total_bytes: archive.length, crc32: resumeCrc }).ok, "short chunk commit failed");

    /* A duplicate chunk already covered by next_offset is idempotent and
     * must not append bytes a second time. */
    const duplicate = receiver.begin(deviceId, {
      type: "offline_audio_begin", request_id: "begin_2b", segment_id: resumeSegment,
      total_bytes: archive.length, crc32: resumeCrc,
    });
    assert(duplicate.next_offset === archive.length, "completed segment did not resume at EOF");
    const committedBeforeDuplicate = receiver.stats.committedBytes;
    receiver.chunk(deviceId, makeChunk(archive, 0, "chunk_duplicate"));
    const duplicateCommit = receiver.commit(deviceId, {
      type: "offline_audio_commit", request_id: "commit_2b", segment_id: resumeSegment,
      total_bytes: archive.length, crc32: resumeCrc,
    });
    assert(duplicateCommit.ok, "idempotent commit after lost ACK failed");
    assert(receiver.stats.committedBytes === committedBeforeDuplicate,
      "idempotent commit was counted as duplicate bytes");

    const badSegment = "offline_meeting_bad_crc_0001";
    const bad = Buffer.from(archive); bad[ARCHIVE_HEADER_BYTES + 4] ^= 0xff;
    const badCrc = archive.readUInt32LE(archive.length - 8);
    receiver.begin(deviceId, { type: "offline_audio_begin", request_id: "begin_4", segment_id: badSegment, total_bytes: bad.length, crc32: badCrc });
    receiver.chunk(deviceId, makeChunk(bad, 0, "chunk_4"));
    while (receiver.transactions.get(receiver.key(deviceId, badSegment)).nextOffset < bad.length) {
      const offset = receiver.transactions.get(receiver.key(deviceId, badSegment)).nextOffset;
      receiver.chunk(deviceId, makeChunk(bad, offset, `chunk_4_${offset}`));
    }
    let rejected = false;
    try {
      receiver.commit(deviceId, { type: "offline_audio_commit", request_id: "commit_3", segment_id: badSegment, total_bytes: bad.length, crc32: badCrc });
    } catch (error) {
      rejected = /CRC|archive/.test(error.message);
    }
    assert(rejected && !fs.existsSync(receiver.paths(deviceId, badSegment).part) &&
      !fs.existsSync(receiver.paths(deviceId, badSegment).complete),
    "bad CRC was not rejected and corrupt temporary data was not cleared");
    console.log("offline replay self-test: PASS");
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

function main() {
  const args = parseArgs(process.argv);
  if (args.help) return printHelp();
  if (args.selfTest) return runSelfTest();
  startServer(args);
}

if (require.main === module) try {
  main();
} catch (error) {
  console.error(`offline replay test gateway: FAIL: ${error.message}`);
  process.exitCode = 1;
}

module.exports = { archiveCrc, startServer, makeArchive, makeChunk, ReplayReceiver, parseArgs };
