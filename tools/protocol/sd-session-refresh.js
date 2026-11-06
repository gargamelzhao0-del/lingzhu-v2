"use strict";
const fs = require("node:fs"), path = require("node:path"), { randomUUID } = require("node:crypto");
const { SdProtocolError } = require("./sd-protocol-error");

// Late files are already durably indexed when acknowledged. Journal the summary
// update before ACK, coalesce it off the network thread, and recover it on restart.
// A new revision during a scan remains pending: an older scan cannot clear it.
class SessionRefreshQueue {
  constructor(sessions, durableJson, readJson) {
    this.sessions = sessions; this.write = durableJson; this.read = readJson;
    this.root = path.join(sessions.storageDir, "session-refresh");
    this.pending = new Map(); this.running = false; this.closed = false;
    this.timer = this.recoverTimer = null; this.ready = null;
  }
  marker(device, session) {
    return path.join(this.root, path.basename(this.sessions.sessionDir(device, session)) + ".json");
  }
  validate(entry, file) {
    try {
      if (!entry || typeof entry.revision !== "string" || !entry.revision ||
          entry.message?.recording_complete !== false ||
          this.marker(entry.device, entry.message.audio_session_id) !== file) throw new Error("invalid identity");
      this.sessions.endParameters(entry.device, entry.message);
    } catch (error) {
      throw new SdProtocolError("storage_unavailable", `invalid pending session refresh: ${error.message}`);
    }
    return entry;
  }
  put(entry) {
    const folder = this.sessions.sessionDir(entry.device, entry.message.audio_session_id);
    const previous = this.pending.get(folder);
    if (previous?.entry.revision === entry.revision) return;
    this.pending.set(folder, { entry, retryAt: previous?.retryAt || 0, backoff: previous?.backoff || 0 });
    this.wake();
  }
  start() {
    if (!this.ready) this.ready = this.recover();
    return this.ready;
  }
  async recover() {
    let retry = false;
    try {
      const files = await fs.promises.readdir(this.root);
      for (const name of files) {
        if (this.closed) break;
        if (!name.endsWith(".json")) continue;
        const file = path.join(this.root, name);
        try {
          const entry = this.validate(JSON.parse(await fs.promises.readFile(file, "utf8")), file);
          // Live commits may have published a newer revision while the read awaited.
          const folder = this.sessions.sessionDir(entry.device, entry.message.audio_session_id);
          if (!this.closed && !this.pending.has(folder)) this.put(entry);
        } catch (error) {
          if (error.code !== "ENOENT") { retry = true; console.warn("SD refresh recovery deferred:", error.message); }
        }
      }
    } catch (error) {
      if (error.code !== "ENOENT") { retry = true; console.warn("SD refresh scan deferred:", error.message); }
    }
    if (retry && !this.closed) {
      this.recoverTimer = setTimeout(() => { this.recoverTimer = null; void this.recover(); }, 30000);
      this.recoverTimer.unref();
    }
  }
  mark(device, ended) {
    if (this.closed) throw new SdProtocolError("storage_unavailable", "session receiver is closing");
    const entry = { device, revision: randomUUID(), message: {
      type: `${ended.mode}_audio_session_end`, request_id: "late_file",
      audio_session_id: ended.audioSessionId, segment_count: ended.segmentCount, recording_complete: false,
    } };
    const file = this.marker(device, ended.audioSessionId);
    this.validate(entry, file); this.write(file, entry); this.put(entry);
  }
  get(device, message) {
    const file = this.marker(device, message.audio_session_id), entry = this.read(file);
    if (entry) { this.validate(entry, file); this.put(entry); }
    return entry;
  }
  complete(entry) {
    const file = this.marker(entry.device, entry.message.audio_session_id), current = this.read(file);
    const folder = this.sessions.sessionDir(entry.device, entry.message.audio_session_id);
    if (current && current.revision !== entry.revision) { this.put(this.validate(current, file)); return; }
    if (current) fs.unlinkSync(file);
    if (this.pending.get(folder)?.entry.revision === entry.revision) this.pending.delete(folder);
  }
  defer(entry, error) {
    const folder = this.sessions.sessionDir(entry.device, entry.message.audio_session_id);
    const item = this.pending.get(folder);
    if (item) {
      item.backoff = Math.min(30000, item.backoff ? item.backoff * 2 : 1000);
      item.retryAt = Date.now() + item.backoff;
    }
    console.warn("SD session refresh deferred:", entry.message.audio_session_id, error.message);
    this.wake();
  }
  wake() {
    if (this.closed || this.running || this.timer || !this.pending.size) return;
    let due = Infinity;
    for (const item of this.pending.values()) due = Math.min(due, item.retryAt);
    this.timer = setTimeout(() => { this.timer = null; void this.pump(); }, Math.max(100, due - Date.now()));
    this.timer.unref();
  }
  async pump() {
    if (this.closed || this.running) return;
    const selected = [...this.pending.entries()].find(([folder, item]) =>
      item.retryAt <= Date.now() && !this.sessions.endJobs.has(folder));
    if (!selected || this.sessions.endJobs.size >= 4) { this.wake(); return; }
    const entry = selected[1].entry; this.running = true;
    try { await this.sessions.runEndJob(entry.device, entry.message, entry); }
    catch (error) { this.defer(entry, error); }
    finally { this.running = false; this.wake(); }
  }
  close() {
    this.closed = true; clearTimeout(this.timer); clearTimeout(this.recoverTimer);
    this.timer = this.recoverTimer = null;
  }
}
module.exports = { SessionRefreshQueue };
