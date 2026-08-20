#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");

const ROOT = path.resolve(__dirname, "..", "..");
const OUT_DIR = path.join(ROOT, "docs", "protocol", "fixtures", "na");
const HEADER_BYTES = 22;
const CODEC_PCM16 = 1;
const CODEC_OPUS = 2;
const FLAG_START = 0x01;
const FLAG_END = 0x02;
const FLAG_VAD_SPEECH = 0x04;

function writeU16LE(buffer, offset, value) {
  buffer.writeUInt16LE(value & 0xffff, offset);
}

function writeU32LE(buffer, offset, value) {
  buffer.writeUInt32LE(value >>> 0, offset);
}

function buildNaFrame({
  codec = CODEC_PCM16,
  flags = 0,
  channels = 1,
  sampleRate = 16000,
  frameDurationMs = 20,
  sequence = 0,
  timestampMs = sequence * frameDurationMs,
  payload = Buffer.alloc(0),
  magic = "NA",
  version = 1,
  declaredPayloadLen = null,
}) {
  const body = Buffer.from(payload);
  const payloadLen = declaredPayloadLen === null ? body.length : declaredPayloadLen;
  const frame = Buffer.alloc(HEADER_BYTES + body.length);
  frame[0] = magic.charCodeAt(0);
  frame[1] = magic.charCodeAt(1);
  frame[2] = version;
  frame[3] = codec;
  frame[4] = flags;
  frame[5] = channels;
  writeU16LE(frame, 6, sampleRate);
  writeU16LE(frame, 8, frameDurationMs);
  writeU32LE(frame, 10, sequence);
  writeU32LE(frame, 14, timestampMs);
  writeU16LE(frame, 18, payloadLen);
  writeU16LE(frame, 20, 0);
  body.copy(frame, HEADER_BYTES);
  return frame;
}

function pcm16Sine(sampleRate, durationMs, frequencyHz, amplitude) {
  const samples = Math.round((sampleRate * durationMs) / 1000);
  const out = Buffer.alloc(samples * 2);
  for (let i = 0; i < samples; i++) {
    const sample = Math.round(Math.sin((2 * Math.PI * frequencyHz * i) / sampleRate) * amplitude);
    out.writeInt16LE(sample, i * 2);
  }
  return out;
}

function makeFixtures() {
  const pcmPayload = pcm16Sine(16000, 20, 440, 10000);
  const pcmFrame = buildNaFrame({
    codec: CODEC_PCM16,
    flags: FLAG_START | FLAG_VAD_SPEECH,
    sampleRate: 16000,
    frameDurationMs: 20,
    sequence: 0,
    payload: pcmPayload,
  });

  const batchFrames = [
    buildNaFrame({
      codec: CODEC_PCM16,
      flags: FLAG_START | FLAG_VAD_SPEECH,
      sampleRate: 16000,
      frameDurationMs: 20,
      sequence: 0,
      payload: pcmPayload,
    }),
    buildNaFrame({
      codec: CODEC_PCM16,
      flags: FLAG_VAD_SPEECH,
      sampleRate: 16000,
      frameDurationMs: 20,
      sequence: 1,
      payload: pcm16Sine(16000, 20, 660, 9000),
    }),
    buildNaFrame({
      codec: CODEC_PCM16,
      flags: FLAG_END | FLAG_VAD_SPEECH,
      sampleRate: 16000,
      frameDurationMs: 20,
      sequence: 2,
      payload: pcm16Sine(16000, 20, 880, 8000),
    }),
  ];

  const opusPacket = Buffer.from([0xf8, 0xff, 0xfe]);
  const opusFrame = buildNaFrame({
    codec: CODEC_OPUS,
    flags: FLAG_START | FLAG_END | FLAG_VAD_SPEECH,
    sampleRate: 16000,
    frameDurationMs: 60,
    sequence: 0,
    payload: opusPacket,
  });

  const badMagicFrame = buildNaFrame({
    codec: CODEC_PCM16,
    flags: FLAG_START,
    sampleRate: 16000,
    frameDurationMs: 20,
    sequence: 0,
    payload: pcmPayload,
    magic: "XX",
  });

  const truncatedPayloadFrame = buildNaFrame({
    codec: CODEC_PCM16,
    flags: FLAG_START,
    sampleRate: 16000,
    frameDurationMs: 20,
    sequence: 0,
    payload: pcmPayload.subarray(0, 12),
    declaredPayloadLen: 640,
  });

  const oddPcmPayloadFrame = buildNaFrame({
    codec: CODEC_PCM16,
    flags: FLAG_START,
    sampleRate: 16000,
    frameDurationMs: 20,
    sequence: 0,
    payload: Buffer.from([0x01, 0x02, 0x03]),
  });

  const stereoPayload = Buffer.alloc(480 * 2 * 2);
  for (let i = 0; i < 480; i++) {
    stereoPayload.writeInt16LE(6000, i * 4);
    stereoPayload.writeInt16LE(-6000, i * 4 + 2);
  }
  const downlinkStereoFrame = buildNaFrame({
    codec: CODEC_PCM16,
    flags: FLAG_START | FLAG_END,
    channels: 2,
    sampleRate: 24000,
    frameDurationMs: 20,
    sequence: 0,
    payload: stereoPayload,
  });

  return [
    {
      file: "pcm16-mono-16k-20ms-start.bin",
      description: "Valid single PCM16 mono NA frame, 16 kHz, 20 ms, START|VAD_SPEECH.",
      direction: "device_to_server",
      valid_na: true,
      expected_frame_count: 1,
      expected: {
        codec: 1,
        codec_name: "pcm16",
        flags: FLAG_START | FLAG_VAD_SPEECH,
        channels: 1,
        sample_rate: 16000,
        frame_duration_ms: 20,
        sequence: 0,
        timestamp_ms: 0,
        payload_len: pcmPayload.length,
      },
      data: pcmFrame,
    },
    {
      file: "pcm16-batch-3frames-16k-20ms.bin",
      description: "Valid WS binary payload containing three consecutive PCM16 NA frames.",
      direction: "device_to_server",
      valid_na: true,
      expected_frame_count: 3,
      expected_sequences: [0, 1, 2],
      data: Buffer.concat(batchFrames),
    },
    {
      file: "opus-packet-16k-60ms.bin",
      description: "Valid NA container with one Opus packet payload. The fixture verifies NA framing; Opus decoder availability is a separate service dependency.",
      direction: "device_to_server",
      valid_na: true,
      expected_frame_count: 1,
      expected: {
        codec: 2,
        codec_name: "opus",
        flags: FLAG_START | FLAG_END | FLAG_VAD_SPEECH,
        channels: 1,
        sample_rate: 16000,
        frame_duration_ms: 60,
        sequence: 0,
        timestamp_ms: 0,
        payload_len: opusPacket.length,
      },
      data: opusFrame,
    },
    {
      file: "invalid-bad-magic.bin",
      description: "Invalid NA frame: magic is XX instead of NA.",
      direction: "device_to_server",
      valid_na: false,
      expected_error: "bad_magic",
      data: badMagicFrame,
    },
    {
      file: "invalid-truncated-payload.bin",
      description: "Invalid NA frame: payload_len declares more bytes than the file contains.",
      direction: "device_to_server",
      valid_na: false,
      expected_error: "truncated_payload",
      data: truncatedPayloadFrame,
    },
    {
      file: "invalid-pcm16-odd-payload.bin",
      description: "Invalid PCM16 NA frame: payload_len is odd.",
      direction: "device_to_server",
      valid_na: false,
      expected_error: "bad_pcm16_payload_length",
      data: oddPcmPayloadFrame,
    },
    {
      file: "downlink-stereo-reject.bin",
      description: "Valid NA framing but invalid for current server-to-device playback because downlink channels=2. Server must not send this to the device.",
      direction: "server_to_device_negative",
      valid_na: true,
      expected_frame_count: 1,
      server_must_not_send_to_device: true,
      expected: {
        codec: 1,
        codec_name: "pcm16",
        flags: FLAG_START | FLAG_END,
        channels: 2,
        sample_rate: 24000,
        frame_duration_ms: 20,
        sequence: 0,
        timestamp_ms: 0,
        payload_len: stereoPayload.length,
      },
      data: downlinkStereoFrame,
    },
  ];
}

function writeFixtures(fixtures) {
  fs.mkdirSync(OUT_DIR, { recursive: true });
  const manifest = {
    version: 1,
    generated_by: "tools/protocol/generate-na-fixtures.js",
    header_bytes: HEADER_BYTES,
    codec: {
      pcm16: CODEC_PCM16,
      opus: CODEC_OPUS,
    },
    flags: {
      START: FLAG_START,
      END: FLAG_END,
      VAD_SPEECH: FLAG_VAD_SPEECH,
    },
    fixtures: [],
  };

  for (const fixture of fixtures) {
    const outputPath = path.join(OUT_DIR, fixture.file);
    fs.writeFileSync(outputPath, fixture.data);
    const { data, ...entry } = fixture;
    entry.bytes = data.length;
    manifest.fixtures.push(entry);
  }

  fs.writeFileSync(path.join(OUT_DIR, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
  return manifest;
}

function main() {
  const fixtures = makeFixtures();
  const manifest = writeFixtures(fixtures);
  console.log(`Wrote ${manifest.fixtures.length} NA fixtures to ${path.relative(process.cwd(), OUT_DIR)}`);
}

main();
