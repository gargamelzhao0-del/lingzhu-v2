"use strict";
const path = require("node:path");
const { Worker } = require("node:worker_threads");
const { SdProtocolError, restoreError } = require("./sd-protocol-error");

// One warm worker; at most four concurrent durable finalizations. Idle workers
// do not hold the process open. A crash rejects its job; the source is retained
// by the device and the next request gets a fresh worker.
class SessionFinalizerPool {
  constructor(storageDir) {
    this.storageDir = storageDir;
    this.workers = new Set(); this.serial = 0; this.started = 0;
  }
  create() {
    if (this.workers.size >= 4) throw new SdProtocolError("busy", "session finalizer busy; retry later");
    const worker = new Worker(path.join(__dirname, "sd-audio-session-finalizer.js"),
      { workerData: { storageDir: this.storageDir } });
    const slot = { worker, job: null, ready: false };
    this.workers.add(slot); this.started++;
    const failed = error => {
      if (!this.workers.delete(slot)) return;
      if (slot.job) {
        slot.job.reject(new SdProtocolError("storage_unavailable", error.message)); slot.job = null;
      }
    };
    worker.on("error", failed);
    worker.on("exit", code => failed(new Error(`session finalizer exited ${code}`)));
    worker.on("message", value => {
      if (value.ready) { slot.ready = true; return; }
      const job = slot.job;
      if (!job || value.id !== job.id) {
        failed(new Error("session finalizer reply mismatch")); void worker.terminate(); return;
      }
      slot.job = null; worker.unref();
      if (value.error) job.reject(restoreError(value.error));
      else if (!value.ack) job.reject(new SdProtocolError("storage_unavailable", "session finalizer missing acknowledgement"));
      else job.resolve({ ack: value.ack, warm: job.warm,
        elapsedMs: Number(process.hrtime.bigint() - job.started) / 1e6, workMs: value.workMs });
      // A burst may need extra workers. Keep only one idle worker afterwards.
      if ([...this.workers].filter(item => !item.job).length > 1) {
        this.workers.delete(slot); void worker.terminate();
      }
    });
    worker.unref();
    return slot;
  }
  prewarm() {
    if (this.workers.size) return;
    try { this.create(); } catch (_) { /* endAsync reports/retries creation failure. */ }
  }
  run(device, message, refresh = false) {
    const slot = [...this.workers].find(item => !item.job) || this.create();
    return new Promise((resolve, reject) => {
      const id = ++this.serial;
      slot.job = { id, resolve, reject, warm: slot.ready, started: process.hrtime.bigint() };
      slot.worker.ref();
      try { slot.worker.postMessage({ id, device, message, refresh }); }
      catch (error) {
        slot.job = null; this.workers.delete(slot); void slot.worker.terminate();
        reject(new SdProtocolError("storage_unavailable", error.message));
      }
    });
  }
}
module.exports = { SessionFinalizerPool };
