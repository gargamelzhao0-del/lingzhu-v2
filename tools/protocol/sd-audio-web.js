"use strict";
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const { Worker } = require("node:worker_threads");
const { pipeline } = require("node:stream");
const { addresses } = require("./sd-test-gateway-config");
const hash = value => crypto.createHash("sha256").update(value).digest("hex");
function workerClient(root) {
  let serial = 0, stopped = false, worker = null;
  const pending = new Map();
  function fail(error) { for (const task of pending.values()) task.reject(error); pending.clear(); }
  function ensure() {
    if (stopped) throw new Error("网关已关闭");
    if (worker) return;
    worker = new Worker(path.join(__dirname, "sd-audio-library.js"), { workerData: { sdAudioLibrary: true, root } });
    worker.on("message", message => {
      const task = pending.get(message.id); if (!task) return;
      if (message.progress) { task.progress?.(message.progress); return; }
      pending.delete(message.id);
      if (message.error) task.reject(new Error(message.error)); else task.resolve(message.result);
    });
    worker.on("error", fail);
    worker.on("exit", () => { worker = null; fail(new Error("音频处理线程已退出，请重试")); });
  }
  return {
    call: (message, progress) => new Promise((resolve, reject) => {
      try { ensure(); const id = ++serial; pending.set(id, { resolve, reject, progress }); worker.postMessage({ id, ...message }); }
      catch (error) { reject(error); }
    }),
    close: () => { stopped = true; fail(new Error("网关已关闭")); return worker?.terminate(); },
  };
}
function rangeOf(value, size) {
  if (!value) return { start: 0, end: size - 1, partial: false };
  const match = /^bytes=(\d*)-(\d*)$/.exec(value);
  if (!match || (!match[1] && !match[2])) return null;
  const start = match[1] ? Number(match[1]) : Math.max(0, size - Number(match[2]));
  const end = match[1] && match[2] ? Math.min(size - 1, Number(match[2])) : size - 1;
  if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start > end || start >= size) return null;
  return { start, end, partial: true };
}
function createAudioWeb(root, receiver, connections, options = {}) {
  const scanner = workerClient(root), converter = workerClient(root);
  let snapshot = null, scanning = null, scannedAt = 0, running = 0;
  const jobs = new Map(), cacheDir = path.join(root, "playback");
  const staticFiles = new Map([
    ["/", ["sd-audio-web.html", "text/html; charset=utf-8"]],
    ["/app.js", ["sd-audio-web-client.js", "text/javascript; charset=utf-8"]],
    ["/style.css", ["sd-audio-web.css", "text/css; charset=utf-8"]],
  ].map(([url, [file, mime]]) => [url, { data: fs.readFileSync(path.join(__dirname, file)), mime }]));
  const getLibrary = async () => {
    if (snapshot && Date.now() - scannedAt < 3000) return snapshot;
    if (!scanning) scanning = scanner.call({ operation: "scan" }).then(value => {
      snapshot = value; scannedAt = Date.now(); return value;
    }).finally(() => { scanning = null; });
    return scanning;
  };
  let sampleTime = Date.now(), sampleBytes = 0, speed = 0;
  const meter = setInterval(() => {
    const now = Date.now(), bytes = receiver.stats.receivedBytes || 0;
    speed = (bytes - sampleBytes) * 1000 / Math.max(1, now - sampleTime);
    sampleTime = now; sampleBytes = bytes;
  }, 2000);
  meter.unref();
  const send = (response, code, value) => {
    const data = JSON.stringify(value);
    response.writeHead(code, { "Content-Type": "application/json; charset=utf-8", "Content-Length": Buffer.byteLength(data) }); response.end(data);
  };
  async function handle(request, response) {
    response.setHeader("X-Content-Type-Options", "nosniff");
    response.setHeader("Cache-Control", "no-store");
    response.setHeader("Referrer-Policy", "no-referrer");
    response.setHeader("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; media-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'");
    try {
      const url = new URL(request.url, "http://localhost");
      if (!["GET", "HEAD", "POST"].includes(request.method)) return send(response, 405, { error: "不支持的方法" });
      if (request.method !== "POST" && staticFiles.has(url.pathname)) {
        const file = staticFiles.get(url.pathname); response.writeHead(200, { "Content-Type": file.mime, "Content-Length": file.data.length });
        return response.end(request.method === "HEAD" ? undefined : file.data);
      }
      if (request.method === "GET" && url.pathname === "/api/status") {
        const library = await getLibrary();
        const sessions = library.sessions.map(({ records, ...session }) => session);
        const lan = addresses().map(v => `ws://${v.address}:${options.port}/api/device/ws`);
        return send(response, 200, { sessions, issues: library.issues, now: new Date().toISOString(),
          endpoint: lan[0] || `ws://${request.headers.host}/api/device/ws`, endpoints: lan,
          discovery: options.discoveryStatus?.() || { enabled: false, ready: false },
          devices: [...new Set([...connections].map(c => c.deviceId).filter(Boolean))],
          bytesPerSecond: speed, receivedBytes: receiver.stats.receivedBytes || 0,
          runtime: options.runtimeStatus?.() || null,
          pending: [...receiver.transactions.values()].map(tx => ({ deviceId: tx.deviceId,
            segmentId: tx.segmentId, received: tx.nextOffset, total: tx.totalBytes,
            connected: [...connections].some(c => c.deviceId === tx.deviceId) })) });
      }
      const detail = /^\/api\/sessions\/([a-f0-9]{64})$/.exec(url.pathname);
      if (request.method === "GET" && detail) {
        const item = (await getLibrary()).sessions.find(s => s.id === detail[1]);
        if (!item) return send(response, 404, { error: "场次不存在" });
        const offset = Math.max(0, Math.floor(Number(url.searchParams.get("offset")) || 0));
        return send(response, 200, { ...item, records: item.records.slice(offset, offset + 100).map(({ file, stamp, ...r }) => r), offset });
      }
      const prepare = /^\/api\/sessions\/([a-f0-9]{64})\/prepare$/.exec(url.pathname);
      if (request.method === "POST" && prepare) {
        if (request.headers.origin && request.headers.origin !== `http://${request.headers.host}`) return send(response, 403, { error: "来源不允许" });
        request.resume();
        const item = (await getLibrary()).sessions.find(s => s.id === prepare[1]);
        if (!item || !item.records.length) return send(response, 404, { error: "尚无已确认的音频" });
        // Immutable snapshot: newly arriving files cannot alter a playing stream.
        const key = hash(`pcm-v2:${item.id}:${item.revision}`), target = path.join(cacheDir, `${key}.wav`);
        let job = jobs.get(key);
        if (!job || job.state === "error") {
          const sidecar = `${target}.json`;
          if (fs.existsSync(target) && fs.existsSync(sidecar)) {
            job = { ...JSON.parse(await fs.promises.readFile(sidecar, "utf8")), id: key, state: "ready", url: `/audio/${key}.wav` };
          } else {
            if (running) return send(response, 429, { error: "正在准备另一段音频，请稍后重试" });
            job = { id: key, state: "preparing", completed: 0, total: item.count, sessionId: item.sessionId };
            running++;
            converter.call({ operation: "render", records: item.records, target }, progress => Object.assign(job, progress))
              .then(async result => {
                await fs.promises.writeFile(sidecar, JSON.stringify(result));
                Object.assign(job, result, { state: "ready", url: `/audio/${key}.wav` });
              }).catch(error => Object.assign(job, { state: "error", error: error.message })).finally(() => { running--; });
          }
          jobs.set(key, job);
          // Bound ephemeral jobs; WAV snapshots persist and are reusable after restart.
          if (jobs.size > 128) for (const [id, value] of jobs) { if (id !== key && value.state !== "preparing") { jobs.delete(id); break; } }
        }
        return send(response, job.state === "ready" ? 200 : 202, job);
      }
      const jobMatch = /^\/api\/jobs\/([a-f0-9]{64})$/.exec(url.pathname);
      if (request.method === "GET" && jobMatch) return send(response, jobs.has(jobMatch[1]) ? 200 : 404, jobs.get(jobMatch[1]) || { error: "播放任务不存在，请重新点击播放" });
      const audio = /^\/audio\/([a-f0-9]{64})\.wav$/.exec(url.pathname);
      if (["GET", "HEAD"].includes(request.method) && audio) {
        const file = path.join(cacheDir, `${audio[1]}.wav`);
        let stat;
        try { stat = await fs.promises.stat(file); } catch { return send(response, 404, { error: "音频尚未准备完成" }); }
        const range = rangeOf(request.headers.range, stat.size);
        if (!range) { response.setHeader("Content-Range", `bytes */${stat.size}`); return send(response, 416, { error: "无效的音频范围" }); }
        response.setHeader("Accept-Ranges", "bytes");
        response.setHeader("Content-Type", "audio/wav");
        response.setHeader("Content-Length", range.end - range.start + 1);
        response.setHeader("Content-Disposition", `${url.searchParams.has("download") ? "attachment" : "inline"}; filename="recording-${audio[1].slice(0, 12)}.wav"`);
        if (range.partial) response.setHeader("Content-Range", `bytes ${range.start}-${range.end}/${stat.size}`);
        response.writeHead(range.partial ? 206 : 200);
        if (request.method === "HEAD") return response.end();
        return pipeline(fs.createReadStream(file, { start: range.start, end: range.end }), response, () => {});
      }
      return send(response, 404, { error: "页面不存在" });
    } catch (error) { if (!response.headersSent) send(response, 500, { error: error.message }); else response.destroy(); }
  }
  return { handle, close: () => { clearInterval(meter); scanner.close(); converter.close(); } };
}
module.exports = { createAudioWeb, rangeOf };
