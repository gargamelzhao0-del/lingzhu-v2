"use strict";
const { parentPort, workerData } = require("node:worker_threads");
const { AudioSessions } = require("./sd-audio-sessions");
const { serializeError } = require("./sd-protocol-error");
const sessions = new AudioSessions(workerData.storageDir);
parentPort.on("message", ({ id, device, message, refresh }) => {
  const started = process.hrtime.bigint();
  try {
    const ack = sessions.end(device, message, refresh === true); // Durable before ACK.
    parentPort.postMessage({ id, ack, workMs: Number(process.hrtime.bigint() - started) / 1e6 });
  } catch (error) { parentPort.postMessage({ id, error: serializeError(error) }); }
});
parentPort.postMessage({ ready: true });
