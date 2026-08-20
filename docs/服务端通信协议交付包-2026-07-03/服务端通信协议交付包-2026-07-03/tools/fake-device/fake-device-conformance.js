#!/usr/bin/env node
"use strict";

const crypto = require("crypto");
const fs = require("fs");
const net = require("net");
const path = require("path");
const tls = require("tls");

const ROOT = path.resolve(__dirname, "..", "..");
const DEFAULT_FIXTURE = path.join(ROOT, "docs", "protocol", "fixtures", "na", "pcm16-batch-3frames-16k-20ms.bin");
const HEADER_BYTES = 22;
const CODEC_PCM16 = 1;
const FLAG_END = 0x02;
const MAX_DEVICE_INBOUND_PAYLOAD_BYTES = 16 * 1024;
const WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const EMOTIONS = new Set([
  "neutral",
  "normal",
  "idle",
  "happy",
  "smile",
  "surprised",
  "wink",
  "tongue",
  "thinking",
  "think",
  "focused",
  "pensive",
  "analyzing",
  "analysis",
  "processing",
  "received",
  "what",
  "confused",
  "understood",
  "ok",
  "awesome",
  "great",
  "like",
  "sad",
  "sorry",
  "error",
  "lost",
  "angry",
  "shy",
  "tired",
]);

function parseArgs(argv) {
  const args = {
    timeoutMs: 8000,
    fixture: DEFAULT_FIXTURE,
    deviceId: "fake-esp32s3-conformance",
    inputSampleRate: 16000,
    outputSampleRate: 16000,
    frameMs: 20,
    micChannels: 4,
    expectPlayback: false,
    stopMode: "i2s-mic-stream",
    insecureTls: false,
  };

  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) {
        throw new Error(`Missing value for ${arg}`);
      }
      return argv[++i];
    };
    if (arg === "--help" || arg === "-h") {
      args.help = true;
    } else if (arg === "--url") {
      args.url = next();
    } else if (arg === "--token") {
      args.token = next();
    } else if (arg === "--timeout-ms") {
      args.timeoutMs = Number(next());
    } else if (arg === "--fixture") {
      args.fixture = path.resolve(next());
    } else if (arg === "--device-id") {
      args.deviceId = next();
    } else if (arg === "--input-sample-rate") {
      args.inputSampleRate = Number(next());
    } else if (arg === "--output-sample-rate") {
      args.outputSampleRate = Number(next());
    } else if (arg === "--frame-ms") {
      args.frameMs = Number(next());
    } else if (arg === "--mic-channels") {
      args.micChannels = Number(next());
    } else if (arg === "--expect-playback") {
      args.expectPlayback = true;
    } else if (arg === "--stop-mode") {
      args.stopMode = next();
    } else if (arg === "--insecure-tls") {
      args.insecureTls = true;
    } else {
      throw new Error(`Unknown argument: ${arg}`);
    }
  }
  return args;
}

function printHelp() {
  console.log(`Usage:
  node tools/fake-device/fake-device-conformance.js --url ws://HOST:PORT/api/device/ws [options]

Options:
  --token TOKEN              Send X-Bridge-Token during WebSocket handshake.
  --fixture FILE            NA binary payload to upload. Defaults to pcm16-batch-3frames-16k-20ms.bin.
  --timeout-ms N            Total conformance window. Default: 8000.
  --device-id ID            Device ID in hello. Default: fake-esp32s3-conformance.
  --input-sample-rate N     hello.audio_params.sample_rate. Default: 16000.
  --output-sample-rate N    hello.audio_params.output_sample_rate. Default: 16000.
  --frame-ms N              hello.audio_params.frame_duration_ms. Default: 20.
  --mic-channels N          hello.audio_params.mic_channels. Default: 4.
  --expect-playback         Fail unless playback_begin, downlink NA END, and playback_end are observed.
  --stop-mode MODE          listen stop mode. Default intentionally differs from wake_word: i2s-mic-stream.
  --insecure-tls            Allow self-signed TLS certificates for wss:// local testing.
`);
}

function sha1Base64(input) {
  return crypto.createHash("sha1").update(input).digest("base64");
}

function escapeRegExp(value) {
  return String(value).replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function buildHello(args) {
  return {
    type: "hello",
    version: 1,
    device_id: args.deviceId,
    transport: "websocket",
    features: {
      audio_upload: true,
      audio_playback: true,
      mcp: true,
    },
    audio_params: {
      input_format: "pcm16",
      codec: "pcm16",
      opus: false,
      output_format: "pcm16",
      sample_rate: args.inputSampleRate,
      output_sample_rate: args.outputSampleRate,
      channels: 1,
      mic_channels: args.micChannels,
      frame_duration_ms: args.frameMs,
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
  };
}

function parseNaFrame(buffer) {
  if (!Buffer.isBuffer(buffer) || buffer.length < HEADER_BYTES) {
    throw new Error("short NA header");
  }
  if (buffer[0] !== 0x4e || buffer[1] !== 0x41) {
    throw new Error("bad NA magic");
  }
  const version = buffer.readUInt8(2);
  const codec = buffer.readUInt8(3);
  const flags = buffer.readUInt8(4);
  const channels = buffer.readUInt8(5);
  const sampleRate = buffer.readUInt16LE(6);
  const frameDurationMs = buffer.readUInt16LE(8);
  const sequence = buffer.readUInt32LE(10);
  const timestampMs = buffer.readUInt32LE(14);
  const payloadLen = buffer.readUInt16LE(18);
  const end = HEADER_BYTES + payloadLen;
  if (version !== 1) {
    throw new Error(`unsupported NA version ${version}`);
  }
  if (codec !== 1 && codec !== 2) {
    throw new Error(`unsupported NA codec ${codec}`);
  }
  if (end > buffer.length) {
    throw new Error("truncated NA payload");
  }
  if (codec === CODEC_PCM16 && payloadLen % 2 !== 0) {
    throw new Error("bad PCM16 payload length");
  }
  return {
    version,
    codec,
    flags,
    channels,
    sampleRate,
    frameDurationMs,
    sequence,
    timestampMs,
    payloadLen,
    payload: buffer.subarray(HEADER_BYTES, end),
    frameBytes: end,
  };
}

class MinimalWebSocketClient {
  constructor(url, options) {
    this.url = new URL(url);
    this.options = options;
    this.socket = null;
    this.buffer = Buffer.alloc(0);
    this.handshakeBuffer = Buffer.alloc(0);
    this.handshakeResolve = null;
    this.handshakeReject = null;
    this.handlers = {
      text: () => {},
      binary: () => {},
      ping: () => {},
      pong: () => {},
      close: () => {},
      error: () => {},
    };
  }

  on(event, handler) {
    this.handlers[event] = handler;
  }

  async connect() {
    const secure = this.url.protocol === "wss:";
    if (!secure && this.url.protocol !== "ws:") {
      throw new Error(`Unsupported URL protocol: ${this.url.protocol}`);
    }
    const port = Number(this.url.port || (secure ? 443 : 80));
    const host = this.url.hostname;
    this.socket = secure
      ? tls.connect({
          host,
          port,
          servername: host,
          rejectUnauthorized: !this.options.insecureTls,
        })
      : net.connect({ host, port });

    this.socket.on("data", (chunk) => this.handleData(chunk));
    this.socket.on("error", (err) => this.handlers.error(err));
    this.socket.on("close", () => this.handlers.close());

    await new Promise((resolve, reject) => {
      this.socket.once("connect", resolve);
      this.socket.once("secureConnect", resolve);
      this.socket.once("error", reject);
    });

    const key = crypto.randomBytes(16).toString("base64");
    const pathAndQuery = `${this.url.pathname || "/"}${this.url.search || ""}`;
    const headers = [
      `GET ${pathAndQuery} HTTP/1.1`,
      `Host: ${host}:${port}`,
      "Upgrade: websocket",
      "Connection: Upgrade",
      `Sec-WebSocket-Key: ${key}`,
      "Sec-WebSocket-Version: 13",
    ];
    if (this.options.token) {
      headers.push(`X-Bridge-Token: ${this.options.token}`);
    }
    this.socket.write(`${headers.join("\r\n")}\r\n\r\n`);

    await new Promise((resolve, reject) => {
      this.handshakeResolve = resolve;
      this.handshakeReject = reject;
    });

    const response = this.handshakeBuffer.toString("utf8");
    const expectedAccept = sha1Base64(`${key}${WS_GUID}`);
    if (!/^HTTP\/1\.1 101\b/m.test(response) && !/^HTTP\/1\.0 101\b/m.test(response)) {
      throw new Error(`WebSocket handshake failed: ${response.split("\r\n")[0] || response}`);
    }
    if (!new RegExp(`Sec-WebSocket-Accept:\\s*${escapeRegExp(expectedAccept)}`, "i").test(response)) {
      throw new Error("WebSocket handshake missing expected Sec-WebSocket-Accept");
    }
  }

  handleData(chunk) {
    if (this.handshakeResolve) {
      this.handshakeBuffer = Buffer.concat([this.handshakeBuffer, chunk]);
      const marker = this.handshakeBuffer.indexOf("\r\n\r\n");
      if (marker < 0) {
        return;
      }
      const rest = this.handshakeBuffer.subarray(marker + 4);
      this.handshakeBuffer = this.handshakeBuffer.subarray(0, marker + 4);
      const resolve = this.handshakeResolve;
      this.handshakeResolve = null;
      this.handshakeReject = null;
      resolve();
      if (rest.length) {
        this.handleFrameBytes(rest);
      }
      return;
    }
    this.handleFrameBytes(chunk);
  }

  handleFrameBytes(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= 2) {
      const b0 = this.buffer[0];
      const b1 = this.buffer[1];
      const fin = (b0 & 0x80) !== 0;
      const opcode = b0 & 0x0f;
      const masked = (b1 & 0x80) !== 0;
      let payloadLen = b1 & 0x7f;
      let offset = 2;
      if (payloadLen === 126) {
        if (this.buffer.length < offset + 2) return;
        payloadLen = this.buffer.readUInt16BE(offset);
        offset += 2;
      } else if (payloadLen === 127) {
        if (this.buffer.length < offset + 8) return;
        const big = this.buffer.readBigUInt64BE(offset);
        if (big > BigInt(Number.MAX_SAFE_INTEGER)) {
          this.handlers.error(new Error("WebSocket payload too large"));
          return;
        }
        payloadLen = Number(big);
        offset += 8;
      }
      let mask = null;
      if (masked) {
        if (this.buffer.length < offset + 4) return;
        mask = this.buffer.subarray(offset, offset + 4);
        offset += 4;
      }
      if (this.buffer.length < offset + payloadLen) return;
      let payload = Buffer.from(this.buffer.subarray(offset, offset + payloadLen));
      this.buffer = this.buffer.subarray(offset + payloadLen);
      if (!fin) {
        this.handlers.error(new Error("Server sent fragmented WebSocket message"));
        continue;
      }
      if (payloadLen > MAX_DEVICE_INBOUND_PAYLOAD_BYTES) {
        this.handlers.error(new Error(`Server payload exceeds 16 KiB: ${payloadLen}`));
        continue;
      }
      if (mask) {
        for (let i = 0; i < payload.length; i++) {
          payload[i] ^= mask[i % 4];
        }
      }
      if (opcode === 0x1) {
        this.handlers.text(payload.toString("utf8"));
      } else if (opcode === 0x2) {
        this.handlers.binary(payload);
      } else if (opcode === 0x8) {
        this.handlers.close();
      } else if (opcode === 0x9) {
        this.handlers.ping(payload);
      } else if (opcode === 0xA) {
        this.handlers.pong(payload);
      } else {
        this.handlers.error(new Error(`Unsupported server WebSocket opcode 0x${opcode.toString(16)}`));
      }
    }
  }

  sendFrame(opcode, payload) {
    const body = Buffer.from(payload || Buffer.alloc(0));
    const mask = crypto.randomBytes(4);
    let header;
    if (body.length < 126) {
      header = Buffer.alloc(2);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | body.length;
    } else if (body.length <= 0xffff) {
      header = Buffer.alloc(4);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | 126;
      header.writeUInt16BE(body.length, 2);
    } else {
      header = Buffer.alloc(10);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | 127;
      header.writeBigUInt64BE(BigInt(body.length), 2);
    }
    const masked = Buffer.alloc(body.length);
    for (let i = 0; i < body.length; i++) {
      masked[i] = body[i] ^ mask[i % 4];
    }
    this.socket.write(Buffer.concat([header, mask, masked]));
  }

  sendText(value) {
    this.sendFrame(0x1, Buffer.from(JSON.stringify(value), "utf8"));
  }

  sendBinary(buffer) {
    this.sendFrame(0x2, buffer);
  }

  sendPong(payload) {
    this.sendFrame(0xA, payload);
  }

  close() {
    if (this.socket && !this.socket.destroyed) {
      this.sendFrame(0x8, Buffer.alloc(0));
      this.socket.end();
    }
  }
}

function validateServerJson(msg, state, violations) {
  if (!msg || typeof msg !== "object") {
    violations.push("Server text frame is not a JSON object");
    return;
  }
  switch (msg.type) {
    case "pong":
      if (typeof msg.now_ms !== "number") {
        violations.push("pong.now_ms must be number");
      }
      state.gotPong = true;
      break;
    case "text_response":
      if (typeof msg.ok !== "boolean") violations.push("text_response.ok must be boolean");
      if (typeof msg.turn_id !== "string") violations.push("text_response.turn_id must be string");
      if (typeof msg.tts_pending !== "boolean") violations.push("text_response.tts_pending must be boolean");
      if (msg.emotion && !EMOTIONS.has(String(msg.emotion))) {
        violations.push(`text_response.emotion is not supported: ${msg.emotion}`);
      }
      state.gotTextResponse = true;
      break;
    case "playback_begin":
      if (msg.channels !== 1) violations.push(`playback_begin.channels must be 1, got ${msg.channels}`);
      if (msg.sample_rate !== state.outputSampleRate) {
        violations.push(`playback_begin.sample_rate=${msg.sample_rate} does not match hello output_sample_rate=${state.outputSampleRate}`);
      }
      state.gotPlaybackBegin = true;
      state.activePlaybackId = String(msg.playback_id || "");
      break;
    case "playback_end":
      state.gotPlaybackEnd = true;
      if (state.activePlaybackId && msg.playback_id && String(msg.playback_id) !== state.activePlaybackId) {
        violations.push(`playback_end.playback_id=${msg.playback_id} does not match active playback_id=${state.activePlaybackId}`);
      }
      break;
    case "device_command":
      if (typeof msg.command !== "string") violations.push("device_command.command must be string");
      if (msg.command === "set_emotion" && msg.emotion && !EMOTIONS.has(String(msg.emotion))) {
        violations.push(`device_command.set_emotion unsupported emotion: ${msg.emotion}`);
      }
      break;
    case "asr_partial":
    case "asr_final":
    case "asr_result":
    case "audio_session":
    case "tts_result":
    case "input_event_ack":
    case "cancel_turn_ack":
    case "mcp":
      break;
    case "ping":
      break;
    default:
      console.log(`[fake-device] ignored unknown server message type=${msg.type || "-"}`);
  }
}

function commandAckFor(msg) {
  const command = String(msg.command || "");
  if (!command) {
    return null;
  }
  const known = new Set([
    "set_emotion",
    "set_volume",
    "stop_playback",
    "set_state",
    "sleep_conversation",
    "show_status",
    "wifi_scan",
    "wifi_connect",
  ]);
  return {
    type: "device_command_ack",
    request_id: String(msg.request_id || ""),
    command,
    ok: known.has(command),
    message: known.has(command) ? "fake-device accepted" : "unsupported command",
  };
}

async function run(args) {
  if (!args.url) {
    throw new Error("--url is required");
  }
  const fixture = fs.readFileSync(args.fixture);
  const ws = new MinimalWebSocketClient(args.url, {
    token: args.token || "",
    insecureTls: args.insecureTls,
  });
  const state = {
    gotPong: false,
    gotTextResponse: false,
    gotPlaybackBegin: false,
    gotPlaybackEnd: false,
    gotAudioEnd: false,
    outputSampleRate: args.outputSampleRate,
    frameMs: args.frameMs,
    activePlaybackId: "",
  };
  const violations = [];
  let closed = false;

  ws.on("text", (payload) => {
    let msg;
    try {
      msg = JSON.parse(payload);
    } catch (err) {
      violations.push(`Server sent invalid JSON: ${err.message}`);
      return;
    }
    console.log(`[fake-device] RX text type=${msg.type || "-"}`);
    validateServerJson(msg, state, violations);
    if (msg.type === "ping") {
      ws.sendText({ type: "pong", now_ms: Number(msg.now_ms || Date.now()), server_ms: Date.now() });
    }
    if (msg.type === "device_command") {
      const ack = commandAckFor(msg);
      if (ack) ws.sendText(ack);
    }
    if (msg.type === "device_command" && msg.command === "wifi_scan") {
      ws.sendText({
        type: "wifi_scan_result",
        ok: true,
        message: "fake-device",
        count: 1,
        networks: [{ ssid: "FakeWiFi", rssi: -42, authmode: 3, secure: true }],
      });
    }
    if (msg.type === "device_command" && msg.command === "wifi_connect") {
      ws.sendText({
        type: "wifi_status",
        phase: "connected",
        ssid: String(msg.ssid || "FakeWiFi"),
        ip: "192.0.2.10",
        ok: true,
        message: "fake-device",
      });
    }
  });

  ws.on("binary", (payload) => {
    console.log(`[fake-device] RX binary bytes=${payload.length}`);
    try {
      const frame = parseNaFrame(payload);
      if (frame.frameBytes !== payload.length) {
        violations.push("Server downlink binary contains more than one NA frame or trailing bytes");
      }
      if (frame.codec !== CODEC_PCM16) {
        violations.push(`Server downlink codec must be PCM16, got ${frame.codec}`);
      }
      if (frame.channels !== 1) {
        violations.push(`Server downlink channels must be 1, got ${frame.channels}`);
      }
      if (frame.sampleRate !== args.outputSampleRate) {
        violations.push(`Server downlink sample_rate=${frame.sampleRate} does not match hello output_sample_rate=${args.outputSampleRate}`);
      }
      if (frame.frameDurationMs !== args.frameMs) {
        violations.push(`Server downlink frame_duration_ms=${frame.frameDurationMs} does not match hello frame_duration_ms=${args.frameMs}`);
      }
      if ((frame.flags & FLAG_END) !== 0) {
        state.gotAudioEnd = true;
      }
    } catch (err) {
      violations.push(`Server downlink binary is not a valid playback NA frame: ${err.message}`);
    }
  });

  ws.on("ping", (payload) => ws.sendPong(payload));
  ws.on("error", (err) => violations.push(err.message));
  ws.on("close", () => {
    closed = true;
  });

  await ws.connect();
  console.log("[fake-device] WebSocket connected");
  ws.sendText(buildHello(args));
  ws.sendText({ type: "ping", now_ms: Date.now() });
  ws.sendText({ type: "listen", state: "start", mode: "wake_word" });
  ws.sendBinary(fixture);
  ws.sendText({ type: "listen", state: "stop", mode: args.stopMode });
  console.log(`[fake-device] Uploaded fixture ${path.relative(process.cwd(), args.fixture)} bytes=${fixture.length}`);

  await new Promise((resolve) => setTimeout(resolve, args.timeoutMs));
  ws.close();

  if (!state.gotPong) {
    violations.push("Did not receive JSON pong for device ping");
  }
  if (args.expectPlayback) {
    if (!state.gotPlaybackBegin) violations.push("Expected playback_begin but did not receive it");
    if (!state.gotAudioEnd) violations.push("Expected downlink NA END but did not receive it");
    if (!state.gotPlaybackEnd) violations.push("Expected playback_end but did not receive it");
  }
  if (closed) {
    console.log("[fake-device] Server closed connection");
  }
  if (violations.length) {
    console.error("[fake-device] FAIL");
    for (const violation of violations) {
      console.error(` - ${violation}`);
    }
    process.exitCode = 1;
    return;
  }
  console.log("[fake-device] PASS");
  console.log(JSON.stringify({
    got_pong: state.gotPong,
    got_text_response: state.gotTextResponse,
    got_playback_begin: state.gotPlaybackBegin,
    got_audio_end: state.gotAudioEnd,
    got_playback_end: state.gotPlaybackEnd,
  }, null, 2));
}

async function main() {
  try {
    const args = parseArgs(process.argv);
    if (args.help) {
      printHelp();
      return;
    }
    await run(args);
  } catch (err) {
    console.error(`[fake-device] ${err.message}`);
    process.exitCode = 1;
  }
}

main();
