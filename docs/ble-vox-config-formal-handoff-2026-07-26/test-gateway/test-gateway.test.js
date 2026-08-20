"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const http = require("node:http");
const net = require("node:net");
const { after, before, test } = require("node:test");

process.env.CLOUD_GATEWAY_HOST = "127.0.0.1";
process.env.CLOUD_GATEWAY_PORT = "18800";
process.env.CLOUD_GATEWAY_TOKEN = "unit-test-secret";
const { server } = require("./cloud-gateway");

function request(path, options = {}, body = null) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: "127.0.0.1", port: 18800, path, ...options }, (res) => {
      const chunks = [];
      res.on("data", (chunk) => chunks.push(chunk));
      res.on("end", () => resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks)) }));
    });
    req.on("error", reject);
    if (body) req.end(JSON.stringify(body));
    else req.end();
  });
}

function maskedTextFrame(value) {
  const payload = Buffer.from(JSON.stringify(value));
  assert.ok(payload.length <= 0xffff);
  const mask = crypto.randomBytes(4);
  const headerLength = payload.length < 126 ? 6 : 8;
  const frame = Buffer.alloc(headerLength + payload.length);
  frame[0] = 0x81;
  if (payload.length < 126) {
    frame[1] = 0x80 | payload.length;
    mask.copy(frame, 2);
  } else {
    frame[1] = 0x80 | 126;
    frame.writeUInt16BE(payload.length, 2);
    mask.copy(frame, 4);
  }
  for (let index = 0; index < payload.length; index += 1) {
    frame[headerLength + index] = payload[index] ^ mask[index & 3];
  }
  return frame;
}

function connectDevice(token = "unit-test-secret") {
  return new Promise((resolve, reject) => {
    const socket = net.connect(18800, "127.0.0.1");
    const key = crypto.randomBytes(16).toString("base64");
    let response = Buffer.alloc(0);
    socket.on("connect", () => socket.write([
      "GET /api/device/ws HTTP/1.1",
      "Host: 127.0.0.1:18800",
      "Upgrade: websocket",
      "Connection: Upgrade",
      `Sec-WebSocket-Key: ${key}`,
      "Sec-WebSocket-Version: 13",
      `X-Bridge-Token: ${token}`,
      "\r\n",
    ].join("\r\n")));
    socket.on("data", function handshake(chunk) {
      response = Buffer.concat([response, chunk]);
      const end = response.indexOf("\r\n\r\n");
      if (end < 0) return;
      socket.removeListener("data", handshake);
      resolve({ socket, statusLine: response.subarray(0, response.indexOf("\r\n")).toString() });
    });
    socket.on("error", reject);
  });
}

before(async () => {
  if (!server.listening) await new Promise((resolve) => server.once("listening", resolve));
});

after(async () => {
  await new Promise((resolve) => server.close(resolve));
});

test("status endpoint is healthy and does not expose the token", async () => {
  const response = await request("/api/status");
  assert.equal(response.status, 200);
  assert.equal(response.body.ok, true);
  assert.equal(response.body.gateway.device_auth_protocol,
    "x-bridge-token+hello.device_secret");
  assert.equal(response.body.gateway.cloud_config_protocol,
    "vox-config-url-device-secret-v1");
  assert.equal(response.body.gateway.signed_bundle_required, false);
  assert.doesNotMatch(JSON.stringify(response.body), /unit-test-secret/);
});

test("websocket upgrade rejects the wrong runtime secret", async () => {
  const connection = await connectDevice("wrong-secret");
  assert.match(connection.statusLine, /401 Unauthorized/);
  connection.socket.destroy();
});

test("device hello becomes online and accepts a remote provisioning command", async () => {
  const connection = await connectDevice();
  assert.match(connection.statusLine, /101 Switching Protocols/);
  connection.socket.write(maskedTextFrame({
    type: "hello",
    device_id: "NX-UNIT-DEVICE",
    device_secret: "unit-test-secret",
    features: { remote_ble_reprovision: true },
    padding: "x".repeat(300),
  }));
  await new Promise((resolve) => setTimeout(resolve, 25));
  const status = await request("/api/status");
  assert.equal(status.body.devices[0].device_id, "NX-UNIT-DEVICE");
  const command = await request("/api/device/command", {
    method: "POST",
    headers: { Authorization: "Bearer unit-test-secret", "Content-Type": "application/json" },
  }, {
    device_id: "NX-UNIT-DEVICE",
    request_id: "test-request",
    command: "start_ble_provisioning",
  });
  assert.equal(command.status, 200);
  assert.equal(command.body.command.command, "start_ble_provisioning");
  connection.socket.destroy();
});

test("command API requires bearer authentication", async () => {
  const response = await request("/api/device/command", { method: "POST" }, {});
  assert.equal(response.status, 401);
});
