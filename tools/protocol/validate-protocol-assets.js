#!/usr/bin/env node
"use strict";

const { execFileSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const ROOT = path.resolve(__dirname, "..", "..");
const SCHEMA_FILE = path.join(ROOT, "docs", "protocol", "schemas", "nuwax-device-protocol.schema.json");
const FIXTURE_DIR = path.join(ROOT, "docs", "protocol", "fixtures", "na");
const MANIFEST_FILE = path.join(FIXTURE_DIR, "manifest.json");
const GENERATOR_FILE = path.join(ROOT, "tools", "protocol", "generate-na-fixtures.js");
const FAKE_DEVICE_FILE = path.join(ROOT, "tools", "fake-device", "fake-device-conformance.js");
const HEADER_BYTES = 22;
const CANONICAL_EMOTIONS = Object.freeze([
  "happy",
  "dizziness",
  "excited",
  "thinking",
  "thought_complete",
  "sleeping",
  "provisioning",
  "disconnected",
  "meeting",
  "monitoring",
  "touch",
  "playful",
  "like",
  "angry",
  "question",
]);

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, "utf8"));
}

function resolveRef(root, ref) {
  if (!ref.startsWith("#/")) {
    throw new Error(`Only local JSON Schema refs are supported by this validator: ${ref}`);
  }
  const parts = ref.slice(2).split("/").map((part) => part.replace(/~1/g, "/").replace(/~0/g, "~"));
  let node = root;
  for (const part of parts) {
    node = node && node[part];
  }
  if (!node) {
    throw new Error(`Unresolved JSON Schema ref: ${ref}`);
  }
  return node;
}

function typeOf(value) {
  if (Array.isArray(value)) return "array";
  if (value === null) return "null";
  if (Number.isInteger(value)) return "integer";
  if (typeof value === "number") return "number";
  return typeof value;
}

function validateSchema(root, schema, value, pathLabel = "$") {
  if (schema.$ref) {
    return validateSchema(root, resolveRef(root, schema.$ref), value, pathLabel);
  }
  let errors = [];
  if (schema.allOf) {
    for (const sub of schema.allOf) {
      errors = errors.concat(validateSchema(root, sub, value, pathLabel));
    }
  }
  if (schema.oneOf) {
    const matches = schema.oneOf.map((sub) => validateSchema(root, sub, value, pathLabel));
    const passCount = matches.filter((subErrors) => subErrors.length === 0).length;
    if (passCount !== 1) {
      errors.push(`${pathLabel} must match exactly one schema, matched ${passCount}`);
    }
  }
  if (schema.anyOf) {
    const matches = schema.anyOf.map((sub) => validateSchema(root, sub, value, pathLabel));
    if (!matches.some((subErrors) => subErrors.length === 0)) {
      errors.push(`${pathLabel} must match at least one schema`);
    }
  }
  if (schema.type) {
    const actual = typeOf(value);
    if (schema.type === "number") {
      if (actual !== "number" && actual !== "integer") errors.push(`${pathLabel} must be number`);
    } else if (actual !== schema.type) {
      errors.push(`${pathLabel} must be ${schema.type}, got ${actual}`);
    }
  }
  if (Object.prototype.hasOwnProperty.call(schema, "const") && value !== schema.const) {
    errors.push(`${pathLabel} must equal ${JSON.stringify(schema.const)}`);
  }
  if (schema.enum && !schema.enum.includes(value)) {
    errors.push(`${pathLabel} must be one of ${schema.enum.join(", ")}`);
  }
  if (typeof value === "string" && typeof schema.maxLength === "number" && value.length > schema.maxLength) {
    errors.push(`${pathLabel} length ${value.length} exceeds ${schema.maxLength}`);
  }
  if (typeof value === "string" && typeof schema.minLength === "number" && value.length < schema.minLength) {
    errors.push(`${pathLabel} length ${value.length} is below ${schema.minLength}`);
  }
  if (typeof value === "string" && typeof schema.pattern === "string" &&
      !(new RegExp(schema.pattern, "u")).test(value)) {
    errors.push(`${pathLabel} does not match pattern ${schema.pattern}`);
  }
  if (typeof value === "string" && typeof schema["x-maxUtf8Bytes"] === "number") {
    const bytes = Buffer.byteLength(value, "utf8");
    if (bytes > schema["x-maxUtf8Bytes"]) {
      errors.push(`${pathLabel} UTF-8 bytes ${bytes} exceeds ${schema["x-maxUtf8Bytes"]}`);
    }
  }
  if ((typeof value === "number" || Number.isInteger(value)) && typeof schema.minimum === "number" && value < schema.minimum) {
    errors.push(`${pathLabel} ${value} is below minimum ${schema.minimum}`);
  }
  if ((typeof value === "number" || Number.isInteger(value)) && typeof schema.maximum === "number" && value > schema.maximum) {
    errors.push(`${pathLabel} ${value} is above maximum ${schema.maximum}`);
  }
  if (schema.required && value && typeof value === "object" && !Array.isArray(value)) {
    for (const key of schema.required) {
      if (!Object.prototype.hasOwnProperty.call(value, key)) {
        errors.push(`${pathLabel}.${key} is required`);
      }
    }
  }
  if (schema.properties && value && typeof value === "object" && !Array.isArray(value)) {
    for (const [key, sub] of Object.entries(schema.properties)) {
      if (Object.prototype.hasOwnProperty.call(value, key)) {
        errors = errors.concat(validateSchema(root, sub, value[key], `${pathLabel}.${key}`));
      }
    }
    if (schema.additionalProperties === false) {
      for (const key of Object.keys(value)) {
        if (!Object.prototype.hasOwnProperty.call(schema.properties, key)) {
          errors.push(`${pathLabel}.${key} is not allowed`);
        }
      }
    }
  }
  if (Array.isArray(value)) {
    if (typeof schema.minItems === "number" && value.length < schema.minItems) {
      errors.push(`${pathLabel} item count ${value.length} is below ${schema.minItems}`);
    }
    if (typeof schema.maxItems === "number" && value.length > schema.maxItems) {
      errors.push(`${pathLabel} item count ${value.length} exceeds ${schema.maxItems}`);
    }
    if (schema.items) {
      for (let i = 0; i < value.length; i++) {
        errors = errors.concat(validateSchema(root, schema.items, value[i], `${pathLabel}[${i}]`));
      }
    }
  }
  return errors;
}

function validMessageSamples() {
  return [
    {
      type: "hello",
      version: 1,
      device_id: "esp32s3-demo",
      device_secret: "runtime-device-secret",
      transport: "websocket",
      features: {
        audio_upload: true,
        audio_playback: true,
        mcp: true,
        meeting_mode_v1: true,
        monitoring_mode_v1: true,
        monitoring_pause_resume_v1: true,
        remote_ble_reprovision: true,
        volume_changed_v1: true,
        notify_playback_v1: true,
      },
      audio_params: {
        input_format: "pcm16",
        codec: "pcm16",
        opus: false,
        output_format: "pcm16",
        sample_rate: 16000,
        output_sample_rate: 16000,
        channels: 1,
        mic_channels: 4,
        frame_duration_ms: 20,
        speaker: true,
        wake_word: true,
        vad: false,
        full_duplex: true,
        shared_i2s: true,
        hardware_profile: "esp_vocat_v1_0",
        aec: false,
        aec_reference: "es7210_slot2",
        has_ref: true,
        ref_slot: 2,
        afe_input_format: "MR",
      },
    },
    {
      type: "firmware_report",
      report_id: "fw_0123456789abcdef",
      device_id: "NX-AABB-CCDD-EEFF",
      device_secret: "runtime-device-secret",
      reset_reason: "power_on",
      firmware: {
        firmware_type: "NuwaxAIPartner_V2",
        version: "7.0.48",
        image_sha256: "a".repeat(64),
      },
    },
    { type: "firmware_report_ack", report_id: "fw_0123456789abcdef", persisted: true },
    {
      type: "device_command", command: "ota_update",
      update_id: "ota_7000047_001",
      url: "https://firmware.example.com/NuwaxAIPartner-7.0.49.bin",
      version: "7.0.49", size_bytes: 2007552,
      image_sha256: "b".repeat(64),
    },
    { type: "ota_ack", update_id: "ota_7000047_001", accepted: true },
    { type: "ota_result", update_id: "ota_7000047_001", ok: true, message: "staged" },
    { type: "ota_result_ack", update_id: "ota_7000047_001", ok: true },
    { type: "ping", now_ms: 123456 },
    { type: "pong", now_ms: 123456, server_ms: 123457 },
    { type: "listen", state: "start", mode: "wake_word" },
    { type: "listen", state: "stop", mode: "i2s-mic-stream" },
    { type: "monitoring_capture_control", request_id: "mon_pause_1", mode: "monitoring", state: "pause", audio_session_id: "monitoring_stream_1", reason: "no_speech", silence_ms: 120000 },
    { type: "monitoring_capture_control_ack", request_id: "mon_pause_1", mode: "monitoring", state: "pause", audio_session_id: "monitoring_stream_1", ok: true, reason: "acknowledged" },
    { type: "monitoring_capture_control", request_id: "mon_resume_1", mode: "monitoring", state: "resume", audio_session_id: "monitoring_stream_2", reason: "speech_detected" },
    { type: "monitoring_capture_control_ack", request_id: "mon_resume_1", mode: "monitoring", state: "resume", audio_session_id: "monitoring_stream_2", ok: true, reason: "acknowledged" },
    { type: "text_request", request_id: "req_1", text: "hello" },
    { type: "cancel_turn", request_id: "cancel_1", turn_id: "turn_1", reason: "device_cancel" },
    { type: "input_event", source: "touch", action: "press", touch: { x: 1, y: 2, gesture: 0, points: 1, event: 0 } },
    { type: "wifi_scan_result", request_id: "cmd_scan_1", ok: true, message: "ok", count: 1, networks: [{ ssid: "OfficeWiFi", rssi: -45, authmode: 3, secure: true }] },
    { type: "wifi_status", request_id: "cmd_wifi_1", phase: "connected", ssid: "OfficeWiFi", ip: "192.168.1.10", ok: true, message: "ok" },
    { type: "wifi_status", phase: "recovery_pending", ssid: "", ip: "", ok: false, message: "waiting" },
    { type: "volume_changed", event_id: "volume_deadbeef_00000001", volume_percent: 65, source: "device_ui" },
    { type: "volume_changed", event_id: "volume_deadbeef_00000002", volume_percent: 65, source: "device_sync" },
    { type: "device_command_ack", request_id: "cmd_1", command: "set_emotion", ok: true, message: "happy" },
    { type: "device_command_ack", command: "set_wake_word", ok: true, message: "wake_words=4 primary=ni hao ling zhu persist=1 truncated=0" },
    { type: "notify_playback_begin_ack", request_id: "req_notify_1", notify_id: "ntf_1", playback_id: "pb_notify_1", ok: true, message: "ready" },
    { type: "notify_playback_end_ack", request_id: "req_notify_end_1", notify_id: "ntf_1", playback_id: "pb_notify_1", ok: true, message: "completed" },
    { type: "notify_playback_cancelled", request_id: "notify_cancel_1", notify_id: "ntf_1", playback_id: "pb_notify_1", reason: "touch_interrupt" },
    { type: "pc_status_request", request_id: "pcstat_1" },
    { type: "status_app_request", request_id: "app_1", app: "main" },
    { type: "device_command", request_id: "cmd_emotion_1", command: "set_emotion", emotion: "happy" },
    ...CANONICAL_EMOTIONS.map((emotion, index) => ({
      type: "device_command",
      request_id: `cmd_emotion_catalog_${index}`,
      command: "set_emotion",
      emotion,
    })),
    { type: "device_command", request_id: "cmd_emotion_alias", command: "set_emotion", assistantEmotion: "happy" },
    { type: "device_command", request_id: "cmd_volume_1", command: "set_volume", volume_percent: 35, persist: true },
    { type: "device_command", request_id: "cmd_volume_alias", command: "set_volume", volumePercent: 35, volumePersist: true },
    { type: "device_command", request_id: "cmd_wake_1", command: "set_wake_word", wake_word: ["ni hao ling zhu", "ni hao lin zu", "ni hao ling zu", "ni hao lin zhu"], persist: true },
    { command: "set_wake_word", wake_word: ["ni hao ling zhu", "ni hao lin zu", "ni hao ling zu", "ni hao lin zhu"], persist: true },
    { type: "device_command", request_id: "cmd_wake_legacy", command: "set_wake_word", wake_word: "ni hao ling zhu", persist: true },
    { type: "device_command", request_id: "cmd_wake_reset", command: "reset_wake_word" },
    { type: "device_command", request_id: "cmd_stop_1", command: "stop_playback", turn_id: "turn_1", playback_id: "pb_1", title: "VOICE", text: "stopped" },
    { type: "device_command", request_id: "cmd_state_1", command: "set_state", state: "sleeping" },
    { type: "device_command", request_id: "cmd_sleep_1", command: "sleep_conversation", turn_id: "turn_1", text: "sleep" },
    { type: "device_command", request_id: "cmd_notify_1", command: "prepare_notify_playback", notify_id: "ntf_1", text: "reminder" },
    { type: "device_command", request_id: "cmd_status_1", command: "show_status", title: "NETWORK", lines: ["WiFi connected"] },
    { type: "device_command", request_id: "cmd_scan_1", command: "wifi_scan" },
    { type: "device_command", request_id: "cmd_wifi_1", command: "wifi_connect", ssid: "OfficeWiFi", password: "password123" },
    { type: "device_command", request_id: "cmd_ble_1", command: "start_ble_provisioning" },
    { type: "asr_partial", provider: "asr", audio_session_id: "aud_1", turn_id: "turn_1", text: "hello", definite: false, sequence: 1, at_ms: 20 },
    { type: "asr_final", provider: "asr", audio_session_id: "aud_1", turn_id: "turn_1", text: "hello", definite: true, sequence: 2, at_ms: 40 },
    { type: "asr_result", ok: true, provider: "asr", audio_session_id: "aud_1", turn_id: "turn_1", text: "hello" },
    { type: "audio_session", state: "closed", audio_session_id: "aud_1", frames: 3, duration_ms: 60, rms: 1000 },
    { type: "text_response", ok: true, request_id: "req_1", turn_id: "turn_1", user_text: "hello", assistant_text: "hi", emotion: "happy", tts_pending: true },
    { type: "playback_begin", request_id: "req_1", turn_id: "turn_1", playback_id: "pb_1", provider: "tts", sample_rate: 16000, channels: 1, frame_duration_ms: 20, frames: 3, bytes: 1920 },
    { type: "playback_end", request_id: "req_1", turn_id: "turn_1", playback_id: "pb_1", provider: "tts", sample_rate: 16000, channels: 1, frame_duration_ms: 20, frames: 3, bytes: 1920, reason: "completed" },
    { type: "notify_playback_begin", request_id: "req_notify_1", notify_id: "ntf_1", playback_id: "pb_notify_1", codec: "pcm16", sample_rate: 16000, channels: 1, frame_duration_ms: 20 },
    { type: "notify_playback_end", request_id: "req_notify_end_1", notify_id: "ntf_1", playback_id: "pb_notify_1", frames: 3, bytes: 1920, reason: "completed" },
    { type: "tts_result", ok: false, request_id: "req_1", turn_id: "turn_1", provider: "tts", playback_id: "pb_1", frames: 0, duration_ms: 0, sample_rate: 16000, channels: 1, playback_start: false, playback_end: false, error: "tts_failed" },
    { type: "tts_skip", ok: true, request_id: "", turn_id: "", reason: "voice_broadcast_disabled" },
    { type: "mcp", payload: { jsonrpc: "2.0", id: "mcp_1", method: "tools/call", params: {} } },
    { type: "input_event_ack", source: "touch", action: "press", events_received: 1 },
    { type: "cancel_turn_ack", ok: true, turn_id: "turn_1", new_turn_id: "turn_2", reason: "device_cancel" },
  ];
}

function invalidMessageSamples() {
  return [
    { name: "firmware report missing secret", value: { type: "firmware_report", report_id: "fw_1", device_id: "NX-AABB-CCDD-EEFF", reset_reason: "power_on", firmware: { firmware_type: "NuwaxAIPartner_V2", version: "7.0.48", image_sha256: "a".repeat(64) } } },
    { name: "firmware report contains removed project name", value: { type: "firmware_report", report_id: "fw_1", device_id: "NX-AABB-CCDD-EEFF", device_secret: "runtime-device-secret", reset_reason: "power_on", firmware: { project_name: "NuwaxAIPartner", version: "7.0.48", image_sha256: "a".repeat(64) } } },
    { name: "firmware report ACK not persisted", value: { type: "firmware_report_ack", report_id: "fw_1" } },
    { name: "OTA update insecure URL", value: { type: "device_command", command: "ota_update", update_id: "ota_1", url: "http://firmware.example.com/app.bin", version: "7.0.49", size_bytes: 2007552, image_sha256: "b".repeat(64) } },
    { name: "OTA update invalid SHA", value: { type: "device_command", command: "ota_update", update_id: "ota_1", url: "https://firmware.example.com/app.bin", version: "7.0.49", size_bytes: 2007552, image_sha256: "bad" } },
    { name: "OTA update contains removed fields", value: { type: "device_command", command: "ota_update", protocol_version: 1, update_id: "ota_1", url: "https://firmware.example.com/app.bin", version: "7.0.49", size_bytes: 2007552, image_sha256: "b".repeat(64) } },
    { name: "OTA update id too long", value: { type: "device_command", command: "ota_update", update_id: "o".repeat(64), url: "https://firmware.example.com/app.bin", version: "7.0.49", size_bytes: 2007552, image_sha256: "b".repeat(64) } },
    { name: "OTA ACK contains removed ok field", value: { type: "ota_ack", update_id: "ota_1", ok: true, accepted: true } },
    { name: "OTA result contains removed status", value: { type: "ota_result", update_id: "ota_1", status: "failed", ok: false, message: "download_failed" } },
    { name: "OTA progress protocol removed", value: { type: "ota_progress", update_id: "ota_1", phase: "downloading", percent: 50 } },
    { name: "OTA cancel protocol removed", value: { type: "device_command", command: "ota_cancel", update_id: "ota_1" } },
    { name: "bad emotion", value: { type: "text_response", ok: true, turn_id: "turn_1", emotion: "booting", tts_pending: true } },
    { name: "missing tts_pending", value: { type: "text_response", ok: true, turn_id: "turn_1" } },
    { name: "stereo playback begin", value: { type: "playback_begin", turn_id: "turn_1", playback_id: "pb_1", sample_rate: 16000, channels: 2, frame_duration_ms: 20 } },
    { name: "overlong request_id", value: { type: "device_command", request_id: "x".repeat(40), command: "wifi_scan" } },
    { name: "request_id exceeds byte contract", value: { type: "device_command", request_id: "\u4e2d".repeat(20), command: "wifi_scan" } },
    { name: "stop playback missing playback scope", value: { type: "device_command", request_id: "cmd_stop_bad", command: "stop_playback", turn_id: "turn_1" } },
    { name: "prepare notification missing notify id", value: { type: "device_command", request_id: "cmd_notify_bad", command: "prepare_notify_playback" } },
    { name: "notification begin missing request id", value: { type: "notify_playback_begin", notify_id: "ntf_1", playback_id: "pb_1" } },
    { name: "notification begin wrong codec", value: { type: "notify_playback_begin", request_id: "req_bad", notify_id: "ntf_1", playback_id: "pb_1", codec: "opus", sample_rate: 16000, channels: 1 } },
    { name: "notification begin stereo", value: { type: "notify_playback_begin", request_id: "req_bad", notify_id: "ntf_1", playback_id: "pb_1", codec: "pcm16", sample_rate: 16000, channels: 2 } },
    { name: "notification id exceeds byte contract", value: { type: "notify_playback_end", request_id: "req_bad", notify_id: "n".repeat(48), playback_id: "pb_1" } },
    { name: "tts skip wrong reason", value: { type: "tts_skip", ok: true, request_id: "", turn_id: "", reason: "tts_failed" } },
    { name: "tts skip must be successful", value: { type: "tts_skip", ok: false, request_id: "", turn_id: "", reason: "voice_broadcast_disabled" } },
    { name: "remote lifecycle state is device owned", value: { type: "device_command", request_id: "cmd_state_bad", command: "set_state", state: "thinking" } },
    { name: "conflicting emotion aliases", value: { type: "device_command", request_id: "cmd_emotion_bad", command: "set_emotion", emotion: "happy", assistantEmotion: "sad" } },
    { name: "conflicting volume aliases", value: { type: "device_command", request_id: "cmd_volume_bad", command: "set_volume", volume_percent: 20, volumePercent: 80 } },
    { name: "volume changed out of range", value: { type: "volume_changed", event_id: "volume_bad", volume_percent: 101, source: "device_ui" } },
    { name: "volume changed invalid source", value: { type: "volume_changed", event_id: "volume_bad", volume_percent: 50, source: "cloud_command" } },
    { name: "monitoring resume missing ack", value: { type: "monitoring_capture_control_ack", request_id: "mon_resume_1", mode: "monitoring", state: "resume", audio_session_id: "monitoring_stream_2", reason: "acknowledged" } },
  ];
}

function parseOneNa(buffer, offset) {
  if (buffer.length - offset < HEADER_BYTES) return { error: "short_header" };
  if (buffer[offset] !== 0x4e || buffer[offset + 1] !== 0x41) return { error: "bad_magic" };
  const version = buffer.readUInt8(offset + 2);
  const codec = buffer.readUInt8(offset + 3);
  const flags = buffer.readUInt8(offset + 4);
  const channels = buffer.readUInt8(offset + 5);
  const sampleRate = buffer.readUInt16LE(offset + 6);
  const frameDurationMs = buffer.readUInt16LE(offset + 8);
  const sequence = buffer.readUInt32LE(offset + 10);
  const timestampMs = buffer.readUInt32LE(offset + 14);
  const payloadLen = buffer.readUInt16LE(offset + 18);
  const frameEnd = offset + HEADER_BYTES + payloadLen;
  if (version !== 1) return { error: "bad_version" };
  if (codec !== 1 && codec !== 2) return { error: "bad_codec" };
  if (frameEnd > buffer.length) return { error: "truncated_payload" };
  if (codec === 1 && payloadLen % 2 !== 0) return { error: "bad_pcm16_payload_length" };
  return {
    frame: {
      codec,
      flags,
      channels,
      sample_rate: sampleRate,
      frame_duration_ms: frameDurationMs,
      sequence,
      timestamp_ms: timestampMs,
      payload_len: payloadLen,
      frame_bytes: HEADER_BYTES + payloadLen,
    },
    nextOffset: frameEnd,
  };
}

function parseNaBatch(buffer) {
  const frames = [];
  let offset = 0;
  while (offset < buffer.length) {
    const result = parseOneNa(buffer, offset);
    if (result.error) return { error: result.error, frames };
    frames.push(result.frame);
    offset = result.nextOffset;
  }
  return { frames };
}

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function validateSchemaFile() {
  const schema = readJson(SCHEMA_FILE);
  assert(schema.$schema && schema.$defs, "schema must define $schema and $defs");
  for (const sample of validMessageSamples()) {
    const errors = validateSchema(schema, schema, sample);
    assert(errors.length === 0, `valid sample ${sample.type} failed schema: ${errors.join("; ")}`);
  }
  for (const sample of invalidMessageSamples()) {
    const errors = validateSchema(schema, schema, sample.value);
    assert(errors.length > 0, `invalid sample ${sample.name} unexpectedly passed schema`);
  }
  return schema;
}

function validateFixtures() {
  const manifest = readJson(MANIFEST_FILE);
  assert(manifest.header_bytes === HEADER_BYTES, "manifest.header_bytes must be 22");
  assert(Array.isArray(manifest.fixtures) && manifest.fixtures.length >= 7, "manifest must list fixtures");
  for (const fixture of manifest.fixtures) {
    const file = path.join(FIXTURE_DIR, fixture.file);
    assert(fs.existsSync(file), `fixture file missing: ${fixture.file}`);
    const data = fs.readFileSync(file);
    assert(data.length === fixture.bytes, `${fixture.file} byte count mismatch manifest=${fixture.bytes} actual=${data.length}`);
    const parsed = parseNaBatch(data);
    if (fixture.valid_na) {
      assert(!parsed.error, `${fixture.file} should parse but failed: ${parsed.error}`);
      assert(parsed.frames.length === fixture.expected_frame_count, `${fixture.file} frame count mismatch`);
      if (fixture.expected) {
        const frame = parsed.frames[0];
        for (const [key, expected] of Object.entries(fixture.expected)) {
          if (key === "codec_name") continue;
          assert(frame[key] === expected, `${fixture.file} ${key} expected ${expected} got ${frame[key]}`);
        }
      }
      if (fixture.expected_sequences) {
        const got = parsed.frames.map((frame) => frame.sequence).join(",");
        assert(got === fixture.expected_sequences.join(","), `${fixture.file} sequence mismatch ${got}`);
      }
      if (fixture.server_must_not_send_to_device) {
        assert(parsed.frames[0].channels !== 1, `${fixture.file} negative downlink fixture should not be mono`);
      }
    } else {
      assert(parsed.error === fixture.expected_error, `${fixture.file} expected error ${fixture.expected_error} got ${parsed.error || "none"}`);
    }
  }
}

function validateNodeSyntax() {
  for (const file of [GENERATOR_FILE, FAKE_DEVICE_FILE]) {
    execFileSync(process.execPath, ["--check", file], { stdio: "pipe" });
  }
}

function main() {
  validateSchemaFile();
  validateFixtures();
  validateNodeSyntax();
  console.log("Protocol assets validation passed");
}

main();
