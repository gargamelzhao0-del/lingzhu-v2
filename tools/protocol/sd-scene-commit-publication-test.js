"use strict";
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const assert = require("node:assert/strict");
const { ReplayReceiver, archiveCrc } = require("./offline-audio-replay-gateway");
const { fixture } = require("./sd-audio-sessions-test");
const { packet } = require("./sd-scene-protocol-test");

async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-scene-publication-"));
  let receiver = new ReplayReceiver(dir), connection = 1, serial = 0;
  const device = "PUBLICATION";
  const control = (type, id, fields = {}) => ({ type, request_id: `pub_${++serial}`, audio_session_id: id, ...fields });
  const metadata = (data, index = 0) => ({ segment_index: index, total_bytes: data.length, crc32: archiveCrc(data).crc });
  const begin = (id, fields = {}) => receiver.scenes.begin(device, control("meeting_audio_begin", id, fields), connection);
  const send = (id, data) => { for (let at = 0; at < data.length; at += 8064)
    receiver.scenes.chunk(device, packet(data, id, 0, at), connection); };
  const commit = (id, data) => receiver.scenes.commit(device, control("meeting_audio_commit", id, metadata(data)), connection);
  const stored = id => receiver.paths(device, `sd_${id}_00000000`).complete;
  const indexPath = id => path.join(receiver.audioSessions.sessionDir(device, id), "00000000.json");
  const read = p => JSON.parse(fs.readFileSync(p));
  try {
    const data = fixture("two_writes", 0, false, 0, 6297);
    begin("two_writes"); send("two_writes", data);
    const sync = fs.fsyncSync, rename = fs.renameSync;
    let syncs = 0; const publications = [];
    fs.fsyncSync = function(fd) { ++syncs; return sync(fd); };
    fs.renameSync = function(from, to) {
      if (String(to).endsWith(".json")) publications.push(read(from));
      return rename(from, to);
    };
    try { assert(commit("two_writes", data).ok); }
    finally { fs.fsyncSync = sync; fs.renameSync = rename; }
    assert.equal(syncs, 2); assert.equal(publications.length, 2);
    assert(publications.every(value => value.committed === true));
    assert.deepEqual(fs.readFileSync(stored("two_writes")), data);
    assert.deepEqual(read(stored("two_writes") + ".json"), read(indexPath("two_writes")));
    assert.equal(receiver.stats.sceneCommitTimings.length, 1);
    console.log("PASS: full 4MiB file publishes exactly two durable final metadata records, no intermediate committed:false writes");

    // Simulate a sync failure in either final publication. The caller receives
    // no success, audio remains, and a fresh receiver can recover/retry.
    for (const failAt of [1, 2]) {
      const id = `sync_${failAt}`, bytes = fixture(id, 0, false);
      begin(id); send(id, bytes); let count = 0;
      fs.fsyncSync = function(fd) {
        if (++count === failAt) { const error = new Error("injected sync failure"); error.code = "EIO"; throw error; }
        return sync(fd);
      };
      try { assert.throws(() => commit(id, bytes), /injected sync failure/); }
      finally { fs.fsyncSync = sync; }
      assert.deepEqual(fs.readFileSync(stored(id)), bytes);
      if (failAt === 1) assert(!fs.existsSync(stored(id) + ".json"));
      if (failAt === 2) assert(read(stored(id) + ".json").committed && !fs.existsSync(indexPath(id)));
      receiver.scenes.release(device, connection); await receiver.audioSessions.close();
      receiver = new ReplayReceiver(dir); ++connection;
      assert.equal(begin(id, { resume: [metadata(bytes)] }).resume[0].next_offset, bytes.length);
      assert(commit(id, bytes).ok);
      assert.deepEqual(read(stored(id) + ".json"), read(indexPath(id)));
      assert.equal(receiver.stats.committedBytes, bytes.length);
      assert(commit(id, bytes).ok); assert.equal(receiver.stats.committedBytes, bytes.length);
      const end = control("meeting_audio_session_end", id, { segment_count: 1, recording_complete: true });
      assert((await receiver.scenes.end(device, end, connection)).ok);
    }
    console.log("PASS: either final sync failing retains audio; process restart/resume/retry repairs metadata before ACK, duplicate commit stays idempotent");

    const invalid = fixture("invalid_flags", 0, false); invalid.writeUInt16LE(0, 22);
    begin("invalid_flags");
    assert.throws(() => send("invalid_flags", invalid), /archive header differs/);
    assert(!fs.existsSync(stored("invalid_flags") + ".json"));
    assert(!fs.existsSync(indexPath("invalid_flags")));
    console.log("PASS: invalid archive identity/geometry cannot publish successful file metadata");
  } finally {
    await receiver.audioSessions.close(); fs.rmSync(dir, { recursive: true, force: true });
  }
}
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
module.exports = { main };
