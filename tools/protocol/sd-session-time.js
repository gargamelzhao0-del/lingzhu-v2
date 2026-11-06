"use strict";
const { SdProtocolError } = require("./sd-protocol-error");
function sessionTime(message) {
  const value = message.session_time;
  if (value === undefined) return undefined; // Existing firmware remains compatible.
  const valid = value && typeof value === "object" && !Array.isArray(value) &&
    (value.started_at_ms === null || (Number.isSafeInteger(value.started_at_ms) && value.started_at_ms > 0)) &&
    (value.duration_ms === null || (Number.isSafeInteger(value.duration_ms) && value.duration_ms >= 0));
  if (!valid) throw new SdProtocolError("invalid_argument", "invalid session_time");
  return { started_at_ms: value.started_at_ms, duration_ms: value.duration_ms };
}
// Time is descriptive metadata, not identity. Repeated/late requests can fill
// unknown fields, but cannot move a timestamp already durably published.
function mergeSessionTime(existing, incoming) {
  if (!existing && !incoming) return undefined;
  return { started_at_ms: existing?.started_at_ms ?? incoming?.started_at_ms ?? null,
    duration_ms: existing?.duration_ms ?? incoming?.duration_ms ?? null };
}
module.exports = { sessionTime, mergeSessionTime };
