"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), vm = require("node:vm"), path = require("node:path");
const { performance } = require("node:perf_hooks");
const { crc32Update, crc32Table, backend } = require("./audio-crc32");
const { archiveCrc, makeArchive } = require("./offline-audio-replay-gateway");
function original(crc, data) {
  let value = (~crc) >>> 0;
  for (const byte of data) {
    value ^= byte;
    for (let bit = 0; bit < 8; bit++) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
  }
  return (~value) >>> 0;
}
assert.equal(crc32Update(0, Buffer.from("123456789")), 0xcbf43926);
const fallback = { module: { exports: {} }, require: name => { assert.equal(name, "node:zlib"); return {}; } };
vm.runInNewContext(fs.readFileSync(path.join(__dirname, "audio-crc32.js"), "utf8"), fallback);
assert.equal(fallback.module.exports.backend, "table");
let seed = 0x12345678;
const random = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed; };
for (const len of [0,1,2,3,4,15,16,17,64,662,8064,8192,65536]) {
  const backing = Buffer.alloc(len + 17);
  for (let i = 0; i < backing.length; i++) backing[i] = random() >>> 24;
  const data = backing.subarray(7, 7 + len), before = Buffer.from(backing);
  for (const start of [0, 1, 0x7fffffff, 0x80000000, 0xffffffff, random()]) {
    const expected = original(start, data);
    for (const update of [crc32Update, crc32Table, fallback.module.exports.crc32Update]) {
      assert.equal(update(start, data), expected);
      let crc = start, at = 0;
      while (at < len) { const end = Math.min(len, at + 1 + random()%713); crc = update(crc, data.subarray(at,end)); at=end; }
      assert.equal(crc, expected);
    }
  }
  assert.deepEqual(backing, before);
}
// Validate an actual durable 4 MiB archive if supplied; otherwise use fixture.
const data = process.argv[2] ? fs.readFileSync(process.argv[2]) : makeArchive();
const expectedSummary = archiveCrc(data), records = [];
for(let at=96;at<data.length-32;){const len=data.readUInt32LE(at);at+=4;records.push(data.subarray(at,at+len));at+=len;}
function run(update) { let crc=0;for(const r of records)crc=update(crc,r);return crc; }
for (const update of [original,crc32Update,crc32Table]) assert.equal(run(update), expectedSummary.crc);
const damaged=Buffer.from(data);damaged[120]^=1;
assert.throws(()=>archiveCrc(damaged), /CRC mismatch/);
const result={backend,archiveBytes:data.length,frames:records.length,hardwareThroughput:false};
for(const [name,update] of [["original",original],["nativeOrTable",crc32Update],["table",crc32Table]]){
  run(update);const samples=[];
  for(let k=0;k<7;k++){const begin=performance.now();assert.equal(run(update),expectedSummary.crc);samples.push(performance.now()-begin);}
  samples.sort((a,b)=>a-b);result[name+"MedianMs"]=+samples[3].toFixed(3);
}
console.log(JSON.stringify(result));
console.log("PASS: known vector, all seed bits, empty/unaligned buffers, randomized incremental boundaries, native-absent fallback, source preservation, real archive CRC and corruption rejection");
