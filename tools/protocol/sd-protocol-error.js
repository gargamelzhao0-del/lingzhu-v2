"use strict";

const codes = new Set(["busy", "invalid_argument", "conflict", "storage_unavailable", "crc_mismatch", "incomplete"]);
class SdProtocolError extends Error {
  constructor(code, message) {
    super(message);
    this.code = this.protocolCode = codes.has(code) ? code : "storage_unavailable";
  }
}
function errorCode(error) {
  // Only explicit validation failures may permanently stop a device scene.
  // Unknown exceptions, worker exits and native I/O failures remain retryable.
  return codes.has(error?.protocolCode) ? error.protocolCode : "storage_unavailable";
}
function serializeError(error) {
  return { message: String(error?.message || error), protocolCode: errorCode(error),
    ...(typeof error?.code === "string" ? { causeCode: error.code } : {}) };
}
function restoreError(value) {
  const error = new SdProtocolError(value?.protocolCode,
    typeof value === "string" ? value : String(value?.message || "session worker failed"));
  if (value?.causeCode) error.causeCode = value.causeCode;
  return error;
}
module.exports = { SdProtocolError, errorCode, serializeError, restoreError };
