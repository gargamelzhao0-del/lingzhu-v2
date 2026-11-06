"use strict";

// Same reflected IEEE CRC-32 and seed semantics as the device archive footer.
// Use the native implementation when the runtime provides it. Older installations
// retain a table fallback, with no dependency or protocol change.
const { crc32: nativeCrc32 } = require("node:zlib");
const table = new Uint32Array(256);
for (let n = 0; n < table.length; n++) {
  let value = n;
  for (let bit = 0; bit < 8; bit++) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
  table[n] = value >>> 0;
}
function crc32Table(crc, data) {
  let value = (~crc) >>> 0;
  for (let i = 0; i < data.length; i++) value = (value >>> 8) ^ table[(value ^ data[i]) & 255];
  return (~value) >>> 0;
}
function crc32Update(crc, data) {
  return typeof nativeCrc32 === "function" ? nativeCrc32(data, crc >>> 0) : crc32Table(crc, data);
}
module.exports = { crc32Update, crc32Table, backend: typeof nativeCrc32 === "function" ? "native" : "table" };
