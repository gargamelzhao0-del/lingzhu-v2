"use strict";
const fs = require("node:fs");
const path = require("node:path");
const { randomUUID } = require("node:crypto");

function createMusicControl({ uploadDir, now = Date.now }) {
  const sources = path.join(uploadDir, "sources");
  fs.mkdirSync(sources, { recursive: true });
  const metadata = path.join(uploadDir, "selected.json");
  let source = null;
  try { source = JSON.parse(fs.readFileSync(metadata, "utf8")); } catch {}
  const devices = new Map();
  const online = (device) => now() - device.seen < 5000;
  function reply(response, code, value) {
    const body = JSON.stringify(value);
    response.writeHead(code, { "Content-Type": "application/json; charset=utf-8", "Content-Length": Buffer.byteLength(body), "Cache-Control": "no-store" });
    response.end(body);
  }
  function select(file, format, bytes, name = "") {
    source = { path: `/sources/${file}`, format, bytes, name };
    const temp = metadata + ".tmp";
    fs.writeFileSync(temp, JSON.stringify(source));
    fs.renameSync(temp, metadata);
    return source;
  }
  function selectLegacy(pcm) {
    const file = `${randomUUID()}.pcm`;
    fs.copyFileSync(pcm, path.join(sources, file));
    return select(file, "pcm", fs.statSync(pcm).size);
  }
  function status() {
    return {
      source,
      devices: [...devices.values()].map((d) => ({
        device_id: d.id, online: online(d), state: d.state, request_id: d.request_id,
        sample_rate: d.sample_rate, reason: d.reason, playback_reason: d.playback_reason,
        accepted: d.accepted, ack: d.ack, pending: d.pending?.request_id || "",
      })),
    };
  }
  async function jsonBody(request) {
    if (!(request.headers["content-type"] || "").startsWith("application/json"))
      throw new Error("Content-Type must be application/json");
    const chunks = []; let bytes = 0;
    for await (const chunk of request) {
      bytes += chunk.length;
      if (bytes > 4096) throw new Error("Request too large");
      chunks.push(chunk);
    }
    const value = JSON.parse(Buffer.concat(chunks).toString("utf8"));
    if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error("Invalid request");
    return value;
  }
  const text = (value, max) => typeof value === "string" ? value.slice(0, max) : "";
  async function handleAsync(request, response, url) {
    if (request.method === "POST" && request.headers.origin &&
        request.headers.origin !== `http://${request.headers.host}`) {
      reply(response, 403, { ok: false, error: "Origin rejected" }); return;
    }
    if (url.pathname === "/api/device/poll") {
      const body = await jsonBody(request);
      if (!/^[a-f0-9]{12}$/.test(body.device_id || "") || !/^[a-f0-9]{8}$/.test(body.boot_id || ""))
        throw new Error("Invalid device identity");
      let device = devices.get(body.device_id);
      if (!device || device.boot !== body.boot_id) {
        // A reboot must never replay a command accepted by the preceding boot.
        device = { id: body.device_id, boot: body.boot_id, pending: null };
        devices.set(body.device_id, device);
      }
      device.seen = now();
      device.state = text(body.state, 16);
      device.request_id = text(body.request_id, 80);
      device.sample_rate = Number(body.sample_rate) || 0;
      device.ack = text(body.ack, 80);
      device.accepted = body.accepted === true;
      device.reason = text(body.reason, 48);
      device.playback_reason = text(body.playback_reason, 48);
      if (device.pending && (device.pending.request_id === device.ack ||
                            now() - device.pending.created > 30000)) device.pending = null;
      reply(response, 200, device.pending || {});
      return;
    }
    if (url.pathname === "/api/play" || url.pathname === "/api/stop") {
      const body = await jsonBody(request);
      const device = devices.get(body.device_id);
      if (!device || !online(device)) {
        reply(response, 409, { ok: false, error: "设备未连接，请先烧录音乐测试固件并连接同一局域网。" }); return;
      }
      const stop = url.pathname === "/api/stop";
      if (!stop && !source) { reply(response, 409, { ok: false, error: "请先上传音频" }); return; }
      device.pending = {
        command: stop ? "stop_music" : "play_music",
        request_id: `music_${randomUUID()}`,
        ...(stop ? {} : { path: source.path, format: source.format }),
        created: now(),
      };
      reply(response, 202, { ok: true, request_id: device.pending.request_id, state: "queued" });
      return;
    }
    if (url.pathname === "/api/music/mp3") {
      const length = Number(request.headers["content-length"]);
      if (!Number.isSafeInteger(length) || length < 4 || length > 32000000) {
        request.resume(); reply(response, 400, { ok: false, error: "MP3 文件须小于 32 MB" }); return;
      }
      const file = `${randomUUID()}.mp3`;
      const target = path.join(sources, file);
      const temporary = target + ".uploading";
      const output = fs.createWriteStream(temporary, { flags: "wx" });
      let bytes = 0; let prefix = Buffer.alloc(0);
      const { pipeline } = require("node:stream/promises");
      const { Transform } = require("node:stream");
      const check = new Transform({ transform(chunk, encoding, callback) {
        bytes += chunk.length;
        if (prefix.length < 4096) prefix = Buffer.concat([prefix, chunk.subarray(0, 4096 - prefix.length)]);
        callback(bytes > length ? new Error("Upload length exceeded") : null, chunk);
      }});
      try {
        await pipeline(request, check, output);
        const looksMp3 = prefix.subarray(0, 3).toString() === "ID3" ||
          prefix.some((value, index) => value === 255 && index + 1 < prefix.length &&
                      (prefix[index + 1] & 0xe0) === 0xe0);
        if (bytes !== length || !looksMp3) throw new Error("文件长度或 MP3 格式不正确");
        fs.renameSync(temporary, target);
        const selected = select(file, "mp3", bytes, text(request.headers["x-file-name"], 120));
        reply(response, 200, { ok: true, ...selected });
      } catch (error) {
        fs.rmSync(temporary, { force: true });
        if (!response.destroyed) reply(response, 400, { ok: false, error: error.message });
      }
      return;
    }
    if (url.pathname.startsWith("/sources/")) {
      const file = url.pathname.slice(9);
      if (!/^[a-f0-9-]{36}\.(mp3|pcm)$/.test(file)) { reply(response, 404, {}); return; }
      let stat;
      try { stat = fs.statSync(path.join(sources, file)); } catch { reply(response, 404, {}); return; }
      response.writeHead(200, {
        "Content-Type": file.endsWith("mp3") ? "audio/mpeg" : "application/octet-stream",
        "Content-Length": stat.size, "Cache-Control": "no-store",
      });
      const input = fs.createReadStream(path.join(sources, file));
      response.on("close", () => input.destroy());
      input.on("error", () => response.destroy());
      input.pipe(response);
    }
  }
  function handle(request, response, url) {
    const post = ["/api/device/poll", "/api/play", "/api/stop", "/api/music/mp3"].includes(url.pathname);
    const get = url.pathname.startsWith("/sources/");
    if (!post && !get) return false;
    if (request.method !== (post ? "POST" : "GET")) { reply(response, 405, {}); return true; }
    handleAsync(request, response, url).catch((error) => {
      if (!response.headersSent && !response.destroyed) reply(response, 400, { ok: false, error: error.message });
      else response.destroy();
    });
    return true;
  }
  return { handle, status, selectLegacy };
}
module.exports = { createMusicControl };
