"use strict";
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const assert = require("node:assert/strict");
const { ReplayReceiver, archiveCrc } = require("./offline-audio-replay-gateway");
const { failure } = require("./sd-scene-receiver");
const { fixture } = require("./sd-audio-sessions-test");
const { packet } = require("./sd-scene-protocol-test");
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(test) {
  const deadline = Date.now() + 10000;
  while (!test()) { assert(Date.now() < deadline, "background refresh did not settle"); await delay(20); }
}
function deferred() { let resolve; const promise = new Promise(r => { resolve = r; }); return { resolve, promise }; }
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-scene-resilience-"));
  let receiver = new ReplayReceiver(root), connection = 1, serial = 0;
  const device = "RESILIENCE";
  const control = (type, id, extra = {}) => ({ type, request_id: `r${++serial}`, audio_session_id: id, ...extra });
  const begin = (id, extra = {}) => receiver.scenes.begin(device, control("meeting_audio_begin", id, extra), connection);
  const endMessage = (id, count = 0, complete = true) => control("meeting_audio_session_end", id,
    { segment_count: count, recording_complete: complete });
  const end = message => receiver.scenes.end(device, message, connection);
  const meta = (data, index) => ({ segment_index: index, total_bytes: data.length, crc32: archiveCrc(data).crc });
  const send = (id, index, data) => { for (let at = 0; at < data.length; at += 8064)
    receiver.scenes.chunk(device, packet(data, id, index, at), connection); };
  const commit = (id, index, data) => receiver.scenes.commit(device,
    control("meeting_audio_commit", id, meta(data, index)), connection);
  const manifest = id => JSON.parse(fs.readFileSync(path.join(receiver.audioSessions.sessionDir(device, id), "session.json")));
  const marker = id => receiver.audioSessions.refreshes.marker(device, id);
  try {
    begin("worker_exit");
    const exitRequest = endMessage("worker_exit");
    const pending = end(exitRequest).then(() => null, error => error);
    await [...receiver.audioSessions.finalizers.workers][0].worker.terminate();
    const exited = await pending;
    assert.equal(failure(exitRequest, exited).error_code, "storage_unavailable");
    assert((await end(endMessage("worker_exit"))).ok);
    await assert.rejects(end(endMessage("worker_exit", 1)), error => error.protocolCode === "conflict");

    begin("worker_io");
    const ioRequest = endMessage("worker_io"), obstruction = path.join(receiver.audioSessions.sessionDir(device, "worker_io"), "session.json.tmp");
    fs.mkdirSync(obstruction);
    const ioError = await end(ioRequest).then(() => null, error => error);
    assert.equal(failure(ioRequest, ioError).error_code, "storage_unavailable");
    assert.equal(ioError.causeCode, "EISDIR");
    fs.rmdirSync(obstruction); assert((await end(endMessage("worker_io"))).ok);
    assert.equal(failure(ioRequest, new Error("internal reply mismatch")).error_code, "storage_unavailable");
    await assert.rejects(end(endMessage("worker_io", -1)), error => error.protocolCode === "invalid_argument");

    // Native I/O failure after archive rename, including publication of just one
    // metadata copy. Reconnect/process restart must count the completed file once.
    for (const stage of ["metadata", "index"]) {
      const id = `retry_${stage}`, data = fixture(id, 0, false); begin(id); send(id, 0, data);
      const stored = receiver.paths(device, `sd_${id}_00000000`).complete;
      const rename = fs.renameSync; let injected = false;
      fs.renameSync = function(from, to) {
        const target = stage === "metadata" ? String(to).endsWith(".complete.json") : String(to).endsWith("00000000.json");
        if (!injected && target && fs.existsSync(stored)) {
          injected = true; const error = new Error("injected metadata publication failure"); error.code = "EIO"; throw error;
        }
        return rename.call(this, from, to);
      };
      try { assert.throws(() => commit(id, 0, data), error => failure(ioRequest, error).error_code === "storage_unavailable"); }
      finally { fs.renameSync = rename; }
      assert(injected); receiver.scenes.release(device, connection++);
      if (stage === "index") { await receiver.audioSessions.close(); receiver = new ReplayReceiver(root); }
      assert.equal(begin(id, { resume: [meta(data, 0)] }).resume[0].next_offset, data.length);
      const before = receiver.stats.committedBytes;
      assert(commit(id, 0, data).ok); assert.equal(receiver.stats.committedBytes, before + data.length);
      assert(commit(id, 0, data).ok); assert.equal(receiver.stats.committedBytes, before + data.length);
      assert.deepEqual(fs.readFileSync(stored), data);
    }

    // A journal publication failure must never ACK a summary update which
    // cannot be recovered. The already durable file can be confirmed on retry.
    const journalId = "journal_write_retry", journalData = fixture(journalId, 0, false);
    begin(journalId); assert((await end(endMessage(journalId, 1, false))).ok);
    send(journalId, 0, journalData);
    const renameJournal = fs.renameSync; let failedJournal = false;
    fs.renameSync = function(from, to) {
      if (String(to) === marker(journalId)) {
        failedJournal = true; const error = new Error("injected refresh journal I/O failure"); error.code = "EIO"; throw error;
      }
      return renameJournal.call(this, from, to);
    };
    try { assert.throws(() => commit(journalId, 0, journalData), error => failure(ioRequest, error).error_code === "storage_unavailable"); }
    finally { fs.renameSync = renameJournal; }
    assert(failedJournal && !fs.existsSync(marker(journalId)));
    receiver.scenes.release(device, connection++);
    assert.equal(begin(journalId, { resume: [meta(journalData, 0)] }).resume[0].next_offset, journalData.length);
    assert(commit(journalId, 0, journalData).ok); assert(fs.existsSync(marker(journalId)));
    await until(() => !fs.existsSync(marker(journalId)));
    assert.equal(manifest(journalId).receivedSegments, 1);

    const id = "concurrent_late"; begin(id); assert((await end(endMessage(id, 3, false))).ok);
    const first = fixture(id, 0, false), second = fixture(id, 1, false, 960);
    const release = deferred();
    const pool = receiver.audioSessions.finalizers, run = pool.run;
    let held = false, refreshRuns = 0;
    pool.run = async function(deviceId, message, refresh) {
      const result = await run.call(this, deviceId, message, refresh);
      if (refresh && message.audio_session_id === id) {
        refreshRuns++;
        if (!held) { held = true; await release.promise; }
      }
      return result;
    };
    send(id, 0, first); assert(commit(id, 0, first).ok); assert(fs.existsSync(marker(id)));
    await until(() => held); // Keep the test alive while the unreferenced background timer dispatches.
    send(id, 1, second); assert(commit(id, 1, second).ok);
    const currentRevision = JSON.parse(fs.readFileSync(marker(id))).revision;
    begin("other_scene"); assert((await end(endMessage("other_scene"))).ok);
    const latestEnd = endMessage(id, 3, false); let replied = false;
    const joined = end(latestEnd).then(ack => { replied = true; return ack; });
    await delay(20); assert(!replied); assert.equal(JSON.parse(fs.readFileSync(marker(id))).revision, currentRevision);
    release.resolve(); const ack = await joined; pool.run = run;
    assert.equal(ack.request_id, latestEnd.request_id); assert(ack.ok && refreshRuns >= 2);
    assert.equal(manifest(id).receivedSegments, 2); assert.deepEqual(manifest(id).missingSegments, [[2, 2]]);
    assert.equal(manifest(id).recordingComplete, false); assert(!fs.existsSync(marker(id)));

    // ACK succeeds only after a restart-recoverable summary update is recorded.
    const recover = "refresh_after_restart", data = fixture(recover, 0, false);
    begin(recover); assert((await end(endMessage(recover, 1, false))).ok);
    receiver.audioSessions.refreshes.wake = () => {}; // Simulate process death before dispatch.
    send(recover, 0, data); assert(commit(recover, 0, data).ok); assert(fs.existsSync(marker(recover)));
    assert.equal(manifest(recover).receivedSegments, 0);
    await receiver.audioSessions.close(); receiver = new ReplayReceiver(root); connection++;
    await receiver.audioSessions.refreshes.start();
    await until(() => !fs.existsSync(marker(recover)));
    assert.equal(manifest(recover).receivedSegments, 1); assert.equal(manifest(recover).recordingComplete, false);
    begin(recover, { resume: [meta(data, 0)] }); assert(commit(recover, 0, data).ok);
    assert.equal(receiver.stats.committedBytes, 0, "restart/duplicate confirmation is not new upload traffic");

    // A refresh worker crash retains its journal and retries automatically.
    const retry = "refresh_worker_retry", retryData = fixture(retry, 0, false);
    begin(retry); assert((await end(endMessage(retry, 1, false))).ok);
    const finalizers = receiver.audioSessions.finalizers, realRun = finalizers.run;
    let crashed = false;
    finalizers.run = function(d, message, refresh) {
      const job = realRun.call(this, d, message, refresh);
      if (refresh && message.audio_session_id === retry && !crashed) {
        crashed = true;
        const active = [...this.workers].find(slot => slot.job);
        void active.worker.terminate();
      }
      return job;
    };
    send(retry, 0, retryData); assert(commit(retry, 0, retryData).ok);
    await until(() => crashed); assert(fs.existsSync(marker(retry)));
    await until(() => !fs.existsSync(marker(retry)));
    finalizers.run = realRun;
    assert.equal(manifest(retry).receivedSegments, 1);
    console.log("PASS: typed worker exit/I/O errors and retry; immutable errors preserved; metadata faults/reconnect/restart count once; journal I/O refusal and retry; late ACK durability; revision fence; unrelated scene progress; explicit end joins latest refresh; journal restart and worker-crash recovery");
  } finally {
    await receiver.audioSessions.close();
    assert.equal(path.dirname(root), path.resolve(os.tmpdir())); assert(path.basename(root).startsWith("nuwax-scene-resilience-"));
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
