"use strict";

// This measures wall time, including retries, inter-segment pauses and commits.
// It never counts the same durable archive twice or extrapolates a resumed tail.
function replayThroughput(audit, log, targetKiBps = 500) {
  if (!Number.isFinite(targetKiBps) || targetKiBps <= 0) throw new Error("Invalid throughput target");
  const entries = new Map();
  const errors = [];
  const sceneEnds = [], typedScenes = new Map();
  for (const segment of audit.segments) {
    const match = /^(.*?)__(.*?)\.complete$/.exec(segment.file);
    if (!match || !Number.isSafeInteger(segment.bytes) || segment.bytes <= 0) {
      errors.push(`Invalid durable archive metadata: ${segment.file}`);
      continue;
    }
    entries.set(`${match[1]}|${match[2]}`, {segment, begins: [], commits: []});
  }
  for (const line of log.split(/\r?\n/)) {
    const end = /^(\S+) SD session durable end (\{.*\})$/.exec(line);
    if (end) {
      try { sceneEnds.push({ at: Date.parse(end[1]), ...JSON.parse(end[2]) }); }
      catch { errors.push("Malformed gateway scene end"); }
    }
    const match = /^(\S+) offline (begin|commit) (\{.*\})$/.exec(line);
    if (!match) continue;
    let data;
    try { data = JSON.parse(match[3]); } catch { errors.push("Malformed gateway begin/commit"); continue; }
    const entry = entries.get(`${data.deviceId}|${data.segmentId}`);
    if (!entry) continue;
    const at = Date.parse(match[1]);
    if (!Number.isFinite(at) || data.totalBytes !== entry.segment.bytes) {
      errors.push(`Gateway timestamp/size mismatch: ${data.segmentId}`);
      continue;
    }
    if (match[2] === "begin") {
      entry.begins.push({at, offset: data.nextOffset});
      if (data.route && data.route !== "offline_audio" && data.audioSessionId)
        entry.scene = `${data.deviceId}|${data.audioSessionId}`;
    }
    else {
      if (data.frameCount !== entry.segment.frameCount || data.crc !== entry.segment.crc) {
        errors.push(`Gateway commit disagrees with durable archive: ${data.segmentId}`);
        continue;
      }
      entry.commits.push(at);
    }
  }
  let first = Infinity, last = -Infinity, bytes = 0;
  for (const {segment, begins, commits, scene} of entries.values()) {
    const start = Math.min(...begins.map(b => b.at));
    const end = Math.min(...commits); // Lost commit ACK/repeated commit must not add bytes.
    if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start ||
        !begins.some(b => b.at === start && b.offset === 0)) {
      errors.push(`Missing complete zero-offset transfer history: ${segment.file}`);
      continue;
    }
    first = Math.min(first, start); last = Math.max(last, end); bytes += segment.bytes;
    if (scene) typedScenes.set(scene, Math.max(typedScenes.get(scene) || -Infinity, end));
  }
  const lastCommit = last;
  for (const [scene, committedAt] of typedScenes) {
    const ends = sceneEnds.filter(end => `${end.deviceId}|${end.session}` === scene &&
      end.recordingComplete === true && Number.isFinite(end.at) && end.at >= committedAt);
    if (!ends.length) errors.push(`Missing durable whole-scene confirmation: ${scene}`);
    else last = Math.max(last, Math.min(...ends.map(end => end.at)));
  }
  const valid = audit.verifiedComplete === true && entries.size > 0 && errors.length === 0;
  const seconds = valid ? (last - first) / 1000 : null;
  const KiBps = valid ? bytes / 1024 / seconds : null;
  return {scope: typedScenes.size ? "gateway-first-begin-to-durable-scene-end" : "gateway-first-begin-to-last-durable-commit", targetKiBps,
    verifiedComplete: audit.verifiedComplete === true, completeTransferHistory: valid,
    archiveBytes: valid ? bytes : null, seconds, KiBps,
    firstBegin: valid ? new Date(first).toISOString() : null,
    lastCommit: valid ? new Date(lastCommit).toISOString() : null,
    durableSceneEnd: valid && typedScenes.size ? new Date(last).toISOString() : null,
    throughputTargetMet: valid && KiBps >= targetKiBps, errors};
}

module.exports = {replayThroughput};
