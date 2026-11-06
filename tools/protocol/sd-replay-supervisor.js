"use strict";

// A session-local supervisor, not an installed service. Keep the archive
// directory across restarts so the receiver can resume its durable prefix.
const fs = require("fs");
const path = require("path");
const { spawn } = require("child_process");
const [runDir, storageDir, listenAddress, port, advertisedAddress, discoveryConfig] = process.argv.slice(2);
const host = listenAddress || "0.0.0.0";
const advertised = advertisedAddress || host;
if (!runDir || !storageDir || !host || !/^\d+$/.test(port || "")) {
  throw new Error("Usage: sd-replay-supervisor.js RUN_DIR STORAGE_DIR LISTEN_ADDRESS PORT [ADVERTISED_ADDRESS]");
}
fs.mkdirSync(runDir, { recursive: true });
const stopFile = path.join(runDir, "stop.request");
const statusFile = path.join(runDir, "supervisor.json");
const startedAt = new Date().toISOString();
let child = null;
let stopping = false;
let attempt = 0;
let restartTimer = null;
function status(extra = {}) {
  const payload = JSON.stringify({ supervisorPid: process.pid,
    childPid: child ? child.pid : null, startedAt, updatedAt: new Date().toISOString(),
    attempt, host, listenAddress: host, advertisedAddress: advertised,
    port: Number(port), storageDir, stopping, ...extra }, null, 2);
  const temp = `${statusFile}.tmp`;
  fs.writeFileSync(temp, payload);
  fs.renameSync(temp, statusFile);
}
function stop() {
  if (stopping) return;
  stopping = true;
  clearTimeout(restartTimer);
  clearInterval(stopTimer);
  if (child) child.kill();
  else status({ state: "stopped" });
}
function launch() {
  if (stopping) return;
  attempt++;
  const prefix = `gateway-${Date.now()}-${attempt}`;
  let out, err, finished = false;
  function finish(code, signal, error) {
    if (finished) return; // Failed spawn can emit both error and close.
    finished = true;
    console.log(new Date().toISOString(), "gateway exited", JSON.stringify({ code, signal, attempt, error }));
    child = null;
    status({ state: stopping ? "stopped" : "restart_pending", exitCode: code, signal, error, logPrefix: prefix });
    if (!stopping) restartTimer = setTimeout(launch, 2000);
  }
  try {
    out = fs.openSync(path.join(runDir, `${prefix}.log`), "a");
    err = fs.openSync(path.join(runDir, `${prefix}-error.log`), "a");
    // Windows spawn may throw synchronously (e.g. UNKNOWN), before any child
    // error listener exists. Keep the supervisor alive and close both FDs.
    child = spawn(process.execPath, [path.join(__dirname, "offline-audio-replay-gateway.js"),
      "--host", host, "--advertised-host", advertised, "--port", port, "--storage-dir", storageDir,
      ...(discoveryConfig ? ["--discovery-config", discoveryConfig] : []),
      "--report", path.join(runDir, `${prefix}-report.json`)],
    { windowsHide: true, stdio: ["ignore", out, err] });
    status({ state: "starting", logPrefix: prefix });
    child.on("spawn", () => status({ state: "running", logPrefix: prefix }));
    child.on("error", error => finish(null, null, `${error.code || "ERROR"}: ${error.message}`));
    child.on("close", (code, signal) => finish(code, signal));
  } catch (error) {
    finish(null, null, `${error.code || "ERROR"}: ${error.message}`);
  } finally {
    if (out !== undefined) fs.closeSync(out);
    if (err !== undefined) fs.closeSync(err);
  }
}
const stopTimer = setInterval(() => { if (fs.existsSync(stopFile)) stop(); }, 1000);
process.on("SIGINT", stop);
process.on("SIGTERM", stop);
launch();
