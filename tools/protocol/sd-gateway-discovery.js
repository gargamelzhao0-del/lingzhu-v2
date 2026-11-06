"use strict";
const dgram = require("node:dgram");
const crypto = require("node:crypto");
const { loadConfig, addresses } = require("./sd-test-gateway-config");
const mac = (data, key) => crypto.createHmac("sha256", Buffer.from(key, "hex")).update(data).digest();
function answer(query, config) {
  if (query.length !== 72 || query.toString("ascii", 0, 8) !== "NWXSDQ1\0" ||
      !query.subarray(24, 40).equals(Buffer.from(config.gatewayId, "hex")) ||
      !crypto.timingSafeEqual(query.subarray(40), mac(query.subarray(0, 40), config.discoveryKey))) return null;
  const reply = Buffer.alloc(76); reply.write("NWXSDR1\0", 0, "ascii");
  query.copy(reply, 8, 8, 40); reply.writeUInt16BE(config.port, 40);
  mac(reply.subarray(0, 44), config.discoveryKey).copy(reply, 44); return reply;
}
function startDiscovery(configFile, log = () => {}) {
  const config = loadConfig(configFile), socket = dgram.createSocket("udp4"), peers = new Map();
  const status = { enabled: true, ready: false, port: config.discoveryPort, gatewayId: config.gatewayId, error: null };
  socket.on("message", (query, peer) => {
    const now = Date.now(), previous = peers.get(peer.address) || 0;
    if (now - previous < 150) return;
    if (peers.size > 128) peers.clear();
    peers.set(peer.address, now);
    const reply = answer(query, config);
    if (reply) socket.send(reply, peer.port, peer.address, error => {
      if (error) log("SD gateway discovery reply failed", { error: error.message });
    });
  });
  socket.on("error", error => { status.ready = false; status.error = error.message; log("SD gateway discovery error", { error: error.message }); });
  socket.bind(config.discoveryPort, "0.0.0.0", () => {
    status.ready = true; status.error = null;
    log("SD gateway discovery listening", { port: config.discoveryPort, gatewayId: config.gatewayId });
  });
  return { status: () => ({ ...status, addresses: addresses().map(v => v.address) }), close: () => { try { socket.close(); } catch {} } };
}
module.exports = { answer, mac, startDiscovery };
