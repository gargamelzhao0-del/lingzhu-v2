"use strict";
const fs = require("node:fs");
const path = require("node:path");

const ARCHIVE_LIMIT = 256;
const CONNECTION_LIMIT = 128;

// This is diagnostic output, never the durable audio/index commit journal.
// At most one async write and one dirty revision exist. Snapshot sizes are
// bounded by the caller; serialization cannot grow with recording history.
class GatewayReportWriter {
  constructor(file, snapshot, onError = () => {}, delayMs = 100) {
    this.file = file; this.snapshot = snapshot; this.onError = onError;
    this.delayMs = delayMs; this.dirty = false; this.running = null;
    this.timer = null; this.closing = false; this.closed = null;
  }
  request() {
    if (!this.file || this.closing) return;
    this.dirty = true; this.schedule(this.delayMs);
  }
  schedule(delay) {
    if (this.closing || this.timer || this.running || !this.dirty) return;
    this.timer = setTimeout(() => { this.timer = null; void this.run(); }, delay);
    this.timer.unref();
  }
  async run() {
    if (this.running) return this.running;
    if (!this.dirty || !this.file) return true;
    this.dirty = false;
    this.running = (async () => {
      try {
        const data = JSON.stringify(this.snapshot(), null, 2);
        await fs.promises.mkdir(path.dirname(this.file), { recursive: true });
        const temporary = `${this.file}.tmp`;
        await fs.promises.writeFile(temporary, data);
        await fs.promises.rename(temporary, this.file);
        return true;
      } catch (error) {
        this.dirty = true; this.onError(error); return false;
      }
    })();
    const ok = await this.running;
    this.running = null;
    if (this.dirty) this.schedule(ok ? this.delayMs : 1000);
    return ok;
  }
  async flush() {
    for (;;) {
      if (this.timer) { clearTimeout(this.timer); this.timer = null; }
      if (!this.running && !this.dirty) return true;
      if (!(await (this.running || this.run()))) return false;
    }
  }
  close() {
    if (!this.closed) {
      this.closing = true;
      this.closed = this.flush();
    }
    return this.closed;
  }
}
module.exports = { GatewayReportWriter, ARCHIVE_LIMIT, CONNECTION_LIMIT };
