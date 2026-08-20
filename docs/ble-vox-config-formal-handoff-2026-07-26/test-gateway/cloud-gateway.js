"use strict";

const crypto = require("node:crypto");
const http = require("node:http");

const HOST = process.env.CLOUD_GATEWAY_HOST || "127.0.0.1";
const PORT = parsePort(process.env.CLOUD_GATEWAY_PORT || "8800");
const TOKEN = String(process.env.CLOUD_GATEWAY_TOKEN || "");
const DEVICE_PATH = "/api/device/ws";
const MAX_HTTP_BODY = 16 * 1024;
const MAX_FRAME_PAYLOAD = 1024 * 1024;
const DEVICE_ID_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$/;
const devices = new Map();

if (!TOKEN || TOKEN.length > 95 || !/^[\x21-\x7e]+$/.test(TOKEN)) {
  throw new Error("CLOUD_GATEWAY_TOKEN must be 1..95 printable ASCII characters");
}

function parsePort(value) {
  const port = Number(value);
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error("CLOUD_GATEWAY_PORT must be a valid TCP port");
  }
  return port;
}

function safeEqual(left, right) {
  const a = Buffer.from(String(left || ""), "utf8");
  const b = Buffer.from(String(right || ""), "utf8");
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

function sendJson(res, statusCode, value) {
  const body = Buffer.from(JSON.stringify(value), "utf8");
  res.writeHead(statusCode, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": body.length,
    "Cache-Control": "no-store",
    "Access-Control-Allow-Origin": "http://127.0.0.1:8765",
    "Access-Control-Allow-Headers": "Authorization, Content-Type",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  });
  res.end(body);
}

function isAdminAuthorized(req) {
  const authorization = String(req.headers.authorization || "");
  return authorization.startsWith("Bearer ") && safeEqual(authorization.slice(7), TOKEN);
}

async function readJsonBody(req) {
  const chunks = [];
  let total = 0;
  for await (const chunk of req) {
    total += chunk.length;
    if (total > MAX_HTTP_BODY) {
      throw new Error("Request body is too large");
    }
    chunks.push(chunk);
  }
  const text = Buffer.concat(chunks).toString("utf8");
  return text ? JSON.parse(text) : {};
}

function statusSnapshot() {
  const now = Date.now();
  return {
    ok: true,
    service: "nuwax-vox-config-test-gateway",
    device_path: DEVICE_PATH,
    auth: "X-Bridge-Token",
    gateway: {
      device_auth_protocol: "x-bridge-token+hello.device_secret",
      cloud_config_protocol: "vox-config-url-device-secret-v1",
      signed_bundle_required: false,
    },
    devices: [...devices.values()]
      .filter((peer) => !peer.closed && peer.deviceId)
      .map((peer) => ({
        device_id: peer.deviceId,
        connected_at: peer.connectedAt,
        last_seen_at: new Date(peer.lastSeenAt).toISOString(),
        age_ms: Math.max(0, now - peer.lastSeenAt),
        remote_ble_reprovision: peer.remoteBleReprovision,
        last_command_ack: peer.lastCommandAck,
      })),
  };
}

function websocketAccept(key) {
  return crypto.createHash("sha1")
    .update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`, "ascii")
    .digest("base64");
}

function websocketFrame(opcode, payload = Buffer.alloc(0)) {
  const body = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  let header;
  if (body.length < 126) {
    header = Buffer.from([0x80 | opcode, body.length]);
  } else if (body.length <= 0xffff) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode;
    header[1] = 126;
    header.writeUInt16BE(body.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(body.length), 2);
  }
  return Buffer.concat([header, body]);
}

class DevicePeer {
  constructor(socket) {
    this.socket = socket;
    this.buffer = Buffer.alloc(0);
    this.fragmentOpcode = null;
    this.fragments = [];
    this.fragmentLength = 0;
    this.closed = false;
    this.deviceId = "";
    this.remoteBleReprovision = false;
    this.connectedAt = new Date().toISOString();
    this.lastSeenAt = Date.now();
    this.lastCommandAck = null;
    socket.setNoDelay(true);
    socket.setKeepAlive(true, 20000);
    socket.on("data", (chunk) => this.onData(chunk));
    socket.on("close", () => this.close(false));
    socket.on("error", () => this.close(false));
  }

  sendJson(value) {
    if (!this.closed && this.socket.writable) {
      this.socket.write(websocketFrame(0x1, Buffer.from(JSON.stringify(value), "utf8")));
    }
  }

  protocolError(message) {
    const reason = Buffer.from(String(message).slice(0, 100), "utf8");
    const payload = Buffer.alloc(2 + reason.length);
    payload.writeUInt16BE(1002, 0);
    reason.copy(payload, 2);
    if (!this.closed && this.socket.writable) {
      this.socket.write(websocketFrame(0x8, payload));
    }
    this.close();
  }

  close(destroy = true) {
    if (this.closed) return;
    this.closed = true;
    if (this.deviceId && devices.get(this.deviceId) === this) {
      devices.delete(this.deviceId);
    }
    if (destroy && !this.socket.destroyed) this.socket.destroy();
  }

  onData(chunk) {
    if (this.closed) return;
    this.lastSeenAt = Date.now();
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.decodeOneFrame()) {
      // Drain all complete frames already received on the TCP stream.
    }
  }

  decodeOneFrame() {
    if (this.buffer.length < 2) return false;
    const first = this.buffer[0];
    const second = this.buffer[1];
    const fin = (first & 0x80) !== 0;
    const opcode = first & 0x0f;
    const masked = (second & 0x80) !== 0;
    let length = second & 0x7f;
    let offset = 2;
    if ((first & 0x70) !== 0 || !masked) {
      this.protocolError("Invalid client frame");
      return false;
    }
    if (length === 126) {
      if (this.buffer.length < 4) return false;
      length = this.buffer.readUInt16BE(2);
      offset = 4;
    } else if (length === 127) {
      if (this.buffer.length < 10) return false;
      const wide = this.buffer.readBigUInt64BE(2);
      if (wide > BigInt(MAX_FRAME_PAYLOAD)) {
        this.protocolError("Frame is too large");
        return false;
      }
      length = Number(wide);
      offset = 10;
    }
    if (length > MAX_FRAME_PAYLOAD) {
      this.protocolError("Frame is too large");
      return false;
    }
    if (opcode >= 0x8 && (!fin || length > 125)) {
      this.protocolError("Invalid control frame");
      return false;
    }
    const frameLength = offset + 4 + length;
    if (this.buffer.length < frameLength) return false;
    const mask = this.buffer.subarray(offset, offset + 4);
    const payload = Buffer.from(this.buffer.subarray(offset + 4, frameLength));
    this.buffer = this.buffer.subarray(frameLength);
    for (let index = 0; index < payload.length; index += 1) {
      payload[index] ^= mask[index & 3];
    }
    this.handleFrame(opcode, fin, payload);
    return !this.closed && this.buffer.length >= 2;
  }

  handleFrame(opcode, fin, payload) {
    if (opcode === 0x8) {
      if (this.socket.writable) this.socket.write(websocketFrame(0x8, payload));
      this.close();
      return;
    }
    if (opcode === 0x9) {
      this.socket.write(websocketFrame(0xA, payload));
      return;
    }
    if (opcode === 0xA) return;

    if (opcode === 0x0) {
      if (this.fragmentOpcode === null) return this.protocolError("Unexpected continuation");
      this.fragments.push(payload);
      this.fragmentLength += payload.length;
      if (this.fragmentLength > MAX_FRAME_PAYLOAD) return this.protocolError("Message is too large");
      if (fin) {
        const complete = Buffer.concat(this.fragments, this.fragmentLength);
        const originalOpcode = this.fragmentOpcode;
        this.fragmentOpcode = null;
        this.fragments = [];
        this.fragmentLength = 0;
        this.handleMessage(originalOpcode, complete);
      }
      return;
    }
    if (opcode !== 0x1 && opcode !== 0x2) return this.protocolError("Unsupported opcode");
    if (this.fragmentOpcode !== null) return this.protocolError("Interleaved fragments");
    if (!fin) {
      this.fragmentOpcode = opcode;
      this.fragments = [payload];
      this.fragmentLength = payload.length;
      return;
    }
    this.handleMessage(opcode, payload);
  }

  handleMessage(opcode, payload) {
    this.lastSeenAt = Date.now();
    if (opcode === 0x2) return;
    let message;
    try {
      message = JSON.parse(payload.toString("utf8"));
    } catch {
      this.sendJson({ type: "error", error: "INVALID_JSON" });
      return;
    }
    if (message.type === "hello") {
      const deviceId = String(message.device_id || message.deviceId || "").trim();
      if (!DEVICE_ID_PATTERN.test(deviceId) || !safeEqual(message.device_secret, TOKEN)) {
        this.sendJson({ type: "error", error: "DEVICE_AUTH_FAILED" });
        this.close();
        return;
      }
      const previous = devices.get(deviceId);
      if (previous && previous !== this) previous.close();
      this.deviceId = deviceId;
      this.remoteBleReprovision = Boolean(message.features?.remote_ble_reprovision);
      devices.set(deviceId, this);
      this.sendJson({
        type: "hello",
        ok: true,
        authenticated: true,
        device_id: deviceId,
        session_id: `test_${crypto.randomBytes(8).toString("hex")}`,
        transport: "cloud-websocket",
      });
      return;
    }
    if (!this.deviceId) {
      this.sendJson({ type: "error", error: "HELLO_REQUIRED" });
      return;
    }
    if (message.type === "ping") {
      this.sendJson({ type: "pong", now: Date.now() });
      return;
    }
    if (message.type === "device_command_ack") {
      this.lastCommandAck = {
        request_id: String(message.request_id || ""),
        command: String(message.command || ""),
        ok: message.ok === true,
        message: String(message.message || ""),
        received_at: new Date().toISOString(),
      };
      return;
    }
    if (message.type === "wifi_status") {
      this.sendJson({ type: "wifi_status_ack", ok: true });
      return;
    }
    this.sendJson({ type: "ack", received_type: String(message.type || "unknown") });
  }
}

const server = http.createServer(async (req, res) => {
  if (req.method === "OPTIONS") {
    sendJson(res, 204, {});
    return;
  }
  const requestUrl = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
  if (req.method === "GET" && requestUrl.pathname === "/api/status") {
    sendJson(res, 200, statusSnapshot());
    return;
  }
  if (req.method === "POST" && requestUrl.pathname === "/api/device/command") {
    if (!isAdminAuthorized(req)) {
      sendJson(res, 401, { ok: false, error: "UNAUTHORIZED" });
      return;
    }
    try {
      const body = await readJsonBody(req);
      const deviceId = String(body.device_id || body.deviceId || "").trim();
      const command = String(body.command || "").trim();
      const requestId = String(body.request_id || body.requestId || "").trim();
      const peer = devices.get(deviceId);
      if (!peer || peer.closed) {
        sendJson(res, 404, { ok: false, error: "DEVICE_NOT_ONLINE" });
        return;
      }
      if (command !== "start_ble_provisioning") {
        sendJson(res, 400, { ok: false, error: "UNSUPPORTED_COMMAND" });
        return;
      }
      if (!peer.remoteBleReprovision) {
        sendJson(res, 409, { ok: false, error: "CAPABILITY_NOT_ADVERTISED" });
        return;
      }
      if (!requestId || requestId.length > 96) {
        sendJson(res, 400, { ok: false, error: "INVALID_REQUEST_ID" });
        return;
      }
      const forwarded = { type: "device_command", request_id: requestId, command };
      peer.sendJson(forwarded);
      sendJson(res, 200, { ok: true, device_id: deviceId, command: forwarded });
    } catch (error) {
      sendJson(res, 400, { ok: false, error: error instanceof SyntaxError ? "INVALID_JSON" : error.message });
    }
    return;
  }
  sendJson(res, 404, { ok: false, error: "NOT_FOUND" });
});

server.on("upgrade", (req, socket) => {
  const requestUrl = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
  const key = String(req.headers["sec-websocket-key"] || "");
  const upgrade = String(req.headers.upgrade || "").toLowerCase();
  const version = String(req.headers["sec-websocket-version"] || "");
  if (requestUrl.pathname !== DEVICE_PATH) {
    socket.end("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
    return;
  }
  if (!safeEqual(req.headers["x-bridge-token"], TOKEN)) {
    socket.end("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
    return;
  }
  if (!key || upgrade !== "websocket" || version !== "13") {
    socket.end("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
    return;
  }
  socket.write([
    "HTTP/1.1 101 Switching Protocols",
    "Upgrade: websocket",
    "Connection: Upgrade",
    `Sec-WebSocket-Accept: ${websocketAccept(key)}`,
    "\r\n",
  ].join("\r\n"));
  new DevicePeer(socket);
});

const reapTimer = setInterval(() => {
  const deadline = Date.now() - 90000;
  for (const peer of devices.values()) {
    if (peer.lastSeenAt < deadline) peer.close();
  }
}, 15000);
reapTimer.unref();

server.listen(PORT, HOST, () => {
  console.log(JSON.stringify({
    event: "ready",
    http: `http://${HOST}:${PORT}/api/status`,
    websocket: `ws://${HOST}:${PORT}${DEVICE_PATH}`,
  }));
});

module.exports = { server, devices, safeEqual, websocketFrame };
