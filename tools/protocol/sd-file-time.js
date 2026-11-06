"use strict";
const { SdProtocolError } = require("./sd-protocol-error");
const minUtc = 1704067200000n, maxUtc = 253402300799999n;

// NWXOFF1 offsets 84..95; zero-filled older headers remain readable.
function fileTime(header, captureSpanMs) {
  if (header.length < 96) throw new SdProtocolError("invalid_argument", "short file time header");
  if (header.readUInt16LE(22) !== 2) return {};
  const utc = header.readBigUInt64LE(84), duration = header.readUInt32LE(92);
  if (utc !== 0n && (utc < minUtc || utc > maxUtc))
    throw new SdProtocolError("invalid_argument", "invalid file UTC time");
  if (duration && captureSpanMs !== undefined && duration !== captureSpanMs)
    throw new SdProtocolError("invalid_argument", "file duration differs from captured frames");
  if (utc && duration && utc + BigInt(duration) > maxUtc)
    throw new SdProtocolError("invalid_argument", "file end time exceeds UTC range");
  return { fileStartedAtMs: utc ? Number(utc) : null,
    fileDurationMs: duration || null,
    fileEndedAtMs: utc && duration ? Number(utc) + duration : null };
}
module.exports = { fileTime };
