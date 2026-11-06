"use strict";
const fs = require("node:fs");
const path = require("node:path");
const os = require("node:os");
const crypto = require("node:crypto");
const root = path.resolve(__dirname, "../..");
const configPath = path.join(root, "runtime-logs/sd-test-gateway/settings.json");
function addresses() {
  return Object.entries(os.networkInterfaces()).filter(([name]) => !/vmware|virtual|tailscale|vethernet|loopback|docker|wsl/i.test(name))
    .flatMap(([name, values]) => values.filter(v => v.family === "IPv4" && !v.internal && !v.address.startsWith("169.254."))
      .map(v => ({ name, address: v.address, netmask: v.netmask })));
}
function loadConfig(file = configPath, create = false) {
  if (!fs.existsSync(file) && create) {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    const value = { schema: 1, gatewayId: crypto.randomBytes(16).toString("hex"),
      discoveryKey: crypto.randomBytes(32).toString("hex"), port: 8810, discoveryPort: 8811,
      storageDir: path.join(root, "runtime-logs/sd-session-test-20261013/archives"),
      runDir: path.join(root, "runtime-logs/sd-session-test-20261013/gateway") };
    // Exclusive creation keeps concurrent build/start commands on one identity.
    try { fs.writeFileSync(file, JSON.stringify(value, null, 2), { flag: "wx", mode: 0o600 }); }
    catch (error) { if (error.code !== "EEXIST") throw error; }
  }
  const value = JSON.parse(fs.readFileSync(file, "utf8"));
  if (value.schema !== 1 || !/^[a-f0-9]{32}$/.test(value.gatewayId) || !/^[a-f0-9]{64}$/.test(value.discoveryKey) ||
      value.port !== 8810 || value.discoveryPort !== 8811 || !path.isAbsolute(value.storageDir) || !path.isAbsolute(value.runDir))
    throw new Error("Invalid SD test gateway settings; preserve the existing identity when repairing.");
  return value;
}
function prepareBuild(file) {
  const config = loadConfig(configPath, true), ip = addresses()[0]?.address;
  let text = fs.existsSync(file) ? fs.readFileSync(file, "utf8") : "";
  const settings = {
    NUWAX_SD_SESSION_ENABLE: "y", NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE: "y",
    // PC audio reception does not require exposing developer controls on-device.
    NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE: "n", NUWAX_TEST_PANEL_ENABLE: "n", NUWAX_TEST_NETWORK_ENABLE: "n",
    NUWAX_SD_GATEWAY_DISCOVERY_ENABLE: "y", NUWAX_SD_GATEWAY_ID: JSON.stringify(config.gatewayId),
    NUWAX_SD_GATEWAY_DISCOVERY_KEY: JSON.stringify(config.discoveryKey),
    NUWAX_SD_GATEWAY_DISCOVERY_PORT: String(config.discoveryPort),
    NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL: JSON.stringify(ip ? `ws://${ip}:${config.port}/api/device/ws` : ""),
  };
  for (const [key, value] of Object.entries(settings)) {
    text = text.replace(new RegExp(`^(?:CONFIG_${key}=.*|# CONFIG_${key} is not set)\\r?\\n?`, "gm"), "");
    text += `\n${value === "n" ? `# CONFIG_${key} is not set` : `CONFIG_${key}=${value}`}\n`;
  }
  fs.mkdirSync(path.dirname(file), { recursive: true }); fs.writeFileSync(file, text);
  console.log(`SD test route: authenticated LAN discovery UDP ${config.discoveryPort}; gateway=${config.gatewayId}; fallback=${ip || "none"}`);
}
if (require.main === module) {
  if (process.argv[2] === "--prepare-build" && process.argv[3]) prepareBuild(path.resolve(process.argv[3]));
  else { const { discoveryKey, ...config } = loadConfig(configPath, true); console.log(JSON.stringify({ ...config, configPath, addresses: addresses() })); }
}
module.exports = { loadConfig, configPath, addresses, prepareBuild };
