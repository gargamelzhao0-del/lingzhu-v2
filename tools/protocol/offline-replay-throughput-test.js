"use strict";
const assert = require("node:assert/strict");
const {replayThroughput} = require("./offline-replay-throughput");
const audit = {verifiedComplete: true, segments: [
  {file: "device__a.complete", bytes: 1048576, frameCount: 100, crc: 11},
  {file: "device__b.complete", bytes: 1048576, frameCount: 100, crc: 12}
]};
const line = (second, type, id, extra = {}) =>
  `${new Date(Date.UTC(2026, 8, 16, 0, 0, second)).toISOString()} offline ${type} ${JSON.stringify({
    deviceId: "device", segmentId: id, totalBytes: 1048576,
    nextOffset: 0, frameCount: 100, crc: id === "a" ? 11 : 12, ...extra})}`;
const log = [line(0, "begin", "a"), line(1, "commit", "a"),
  line(3, "begin", "b"), line(4, "commit", "b")].join("\n");
let result = replayThroughput(audit, log);
assert.equal(result.KiBps, 512); assert.equal(result.seconds, 4); assert(result.throughputTargetMet);
const boundaryAudit = {...audit, segments: audit.segments.map(s => ({...s, bytes: 1024000}))};
const boundaryLog = log.replaceAll('"totalBytes":1048576', '"totalBytes":1024000');
assert.equal(replayThroughput(boundaryAudit, boundaryLog).KiBps, 500);
assert(replayThroughput(boundaryAudit, boundaryLog).throughputTargetMet);
result = replayThroughput(audit, log + "\n" + line(10, "commit", "b"));
assert.equal(result.KiBps, 512); // Duplicate ACK recovery cannot inflate bytes.
result = replayThroughput(audit, log.replace(line(4, "commit", "b"), line(5, "commit", "b")));
assert.equal(result.KiBps, 409.6); assert(!result.throughputTargetMet);
for (const broken of [log.replace(line(0, "begin", "a"), line(0, "begin", "a", {nextOffset: 500})),
  log.replace(line(4, "commit", "b"), ""),
  log.replace(line(4, "commit", "b"), line(4, "commit", "b", {crc: 99})),
  log.replace(line(4, "commit", "b"), line(4, "commit", "b", {totalBytes: 123}))]) {
  result = replayThroughput(audit, broken); assert(!result.throughputTargetMet); assert.equal(result.KiBps, null);
}
result = replayThroughput({...audit, verifiedComplete: false}, log);
assert(!result.throughputTargetMet); assert.equal(result.KiBps, null);
// Include retry downtime from the original zero-offset begin; resumed bytes aren't double counted.
result = replayThroughput(audit, log + "\n" + line(2, "begin", "a", {nextOffset: 500}));
assert.equal(result.KiBps, 512);
assert.throws(() => replayThroughput(audit, log, NaN));
const typedLog = log.replaceAll('"nextOffset":0', '"route":"meeting_audio","audioSessionId":"meeting1","nextOffset":0');
assert(!replayThroughput(audit, typedLog).throughputTargetMet);
const endLine = second => `${new Date(Date.UTC(2026,8,16,0,0,second)).toISOString()} SD session durable end ` +
  JSON.stringify({deviceId:"device",session:"meeting1",recordingComplete:true});
result = replayThroughput(audit, `${typedLog}\n${endLine(10)}\n${endLine(20)}`);
assert.equal(result.KiBps, 204.8); assert.equal(result.seconds, 10);
assert.equal(result.scope, "gateway-first-begin-to-durable-scene-end");
assert(!result.throughputTargetMet);
console.log("PASS: durable throughput includes gaps, retries; rejects incomplete/corrupt history; deduplicates commits; 500 KiB/s boundary");
