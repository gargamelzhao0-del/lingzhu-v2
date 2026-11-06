"use strict";

const http = require("http");
const fs = require("fs");
const os = require("os");
const path = require("path");

const HOST = process.env.NUWAX_MUSIC_TEST_HOST || "0.0.0.0";
const PORT = Number(process.env.NUWAX_MUSIC_TEST_PORT || 8798);
const MAX_PCM_BYTES = Number(
  process.env.NUWAX_MUSIC_TEST_MAX_PCM_BYTES || 19_200_000,
);
const ROOT = __dirname;
const PUBLIC_DIR = path.join(ROOT, "public");
const UPLOAD_DIR = path.join(ROOT, "uploads");
const PCM_PATH = path.join(UPLOAD_DIR, "music.pcm");
const control = require("./control").createMusicControl({ uploadDir: UPLOAD_DIR });
if (!control.status().source && fs.existsSync(PCM_PATH)) control.selectLegacy(PCM_PATH);
let pcmUploading = false;

function localIpv4Addresses() {
  const addresses = [];
  const networks = Object.entries(os.networkInterfaces()).sort(([a], [b]) =>
    Number(/wlan|wi-fi|wireless/i.test(b)) - Number(/wlan|wi-fi|wireless/i.test(a)));
  for (const [, interfaces] of networks) {
    for (const entry of interfaces || []) {
      if (entry.family === "IPv4" && !entry.internal) {
        addresses.push(entry.address);
      }
    }
  }
  return [...new Set(addresses)];
}

function sendJson(response, status, value) {
  const body = Buffer.from(JSON.stringify(value));
  response.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": body.length,
    "Cache-Control": "no-store",
  });
  response.end(body);
}

function sendText(response, status, value) {
  const body = Buffer.from(value);
  response.writeHead(status, {
    "Content-Type": "text/plain; charset=utf-8",
    "Content-Length": body.length,
    "Cache-Control": "no-store",
  });
  response.end(body);
}

function contentType(filePath) {
  switch (path.extname(filePath)) {
    case ".html":
      return "text/html; charset=utf-8";
    case ".js":
      return "text/javascript; charset=utf-8";
    case ".css":
      return "text/css; charset=utf-8";
    default:
      return "application/octet-stream";
  }
}

function statusPayload(request) {
  let bytes = 0;
  try {
    bytes = fs.statSync(PCM_PATH).size;
  } catch (error) {
    if (error.code !== "ENOENT") {
      throw error;
    }
  }
  const addresses = localIpv4Addresses();
  const requestHost = (request.headers.host || `127.0.0.1:${PORT}`).split(":")[0];
  const isLoopback =
    requestHost === "localhost" ||
    requestHost === "127.0.0.1" ||
    requestHost === "::1";
  const candidates = [
    ...(!isLoopback ? [requestHost] : []),
    ...addresses,
    ...(addresses.length === 0 ? [requestHost] : []),
  ].filter((address, index, all) => all.indexOf(address) === index);
  return {
    ...control.status(),
    ready: bytes > 0 && bytes % 2 === 0,
    bytes,
    duration_seconds: bytes / 2 / 16000,
    format: "PCM signed 16-bit little-endian, 16000 Hz, mono",
    page_urls: candidates.map((address) => `http://${address}:${PORT}/`),
    device_urls: candidates.map(
      (address) => `http://${address}:${PORT}/music.pcm`,
    ),
  };
}

function serveStatic(requestPath, response) {
  const relative = requestPath === "/" ? "index.html" : requestPath.slice(1);
  const resolved = path.resolve(PUBLIC_DIR, relative);
  if (!resolved.startsWith(`${path.resolve(PUBLIC_DIR)}${path.sep}`)) {
    sendText(response, 403, "Forbidden");
    return;
  }
  fs.stat(resolved, (error, stat) => {
    if (error || !stat.isFile()) {
      sendText(response, 404, "Not found");
      return;
    }
    response.writeHead(200, {
      "Content-Type": contentType(resolved),
      "Content-Length": stat.size,
      "Cache-Control": "no-store",
    });
    fs.createReadStream(resolved).pipe(response);
  });
}

function receivePcm(request, response) {
  if (pcmUploading) {
    request.resume(); sendJson(response, 409, { ok: false, error: "Upload already in progress" }); return;
  }
  const declared = Number(request.headers["content-length"] || 0);
  if (!Number.isSafeInteger(declared) || declared <= 0 || declared > MAX_PCM_BYTES) {
    sendJson(response, 400, {
      ok: false,
      error: `PCM payload must be 1..${MAX_PCM_BYTES} bytes`,
    });
    request.resume();
    return;
  }
  if (declared % 2 !== 0) {
    sendJson(response, 400, { ok: false, error: "PCM byte length must be even" });
    request.resume();
    return;
  }

  fs.mkdirSync(UPLOAD_DIR, { recursive: true });
  const tempPath = path.join(UPLOAD_DIR, `music-${require("node:crypto").randomUUID()}.uploading`);
  pcmUploading = true;
  const output = fs.createWriteStream(tempPath, { flags: "wx" });
  let received = 0;
  let settled = false;

  function fail(status, message) {
    if (settled) return;
    settled = true;
    pcmUploading = false;
    output.destroy();
    try {
      fs.rmSync(tempPath, { force: true });
    } catch {
      // Best effort cleanup only.
    }
    sendJson(response, status, { ok: false, error: message });
  }

  request.on("data", (chunk) => {
    received += chunk.length;
    if (received > declared || received > MAX_PCM_BYTES) {
      fail(413, "PCM payload exceeded declared or configured size");
      request.destroy();
    }
  });
  request.on("aborted", () => fail(400, "Upload was aborted"));
  request.on("error", (error) => fail(500, error.message));
  output.on("error", (error) => fail(500, error.message));
  output.on("finish", () => {
    if (settled) return;
    if (received !== declared || received % 2 !== 0) {
      fail(400, "Upload length did not match Content-Length");
      return;
    }
    try {
      fs.renameSync(tempPath, PCM_PATH);
      control.selectLegacy(PCM_PATH);
      settled = true;
      pcmUploading = false;
      sendJson(response, 200, {
        ok: true,
        bytes: received,
        duration_seconds: received / 2 / 16000,
      });
    } catch (error) {
      fail(500, error.message);
    }
  });
  request.pipe(output);
}

const server = http.createServer((request, response) => {
  const url = new URL(request.url, `http://${request.headers.host || "localhost"}`);
  if (control.handle(request, response, url)) return;
  if (request.method === "GET" && url.pathname === "/api/status") {
    sendJson(response, 200, statusPayload(request));
    return;
  }
  if (request.method === "POST" && url.pathname === "/api/music") {
    receivePcm(request, response);
    return;
  }
  if (request.method === "GET" && url.pathname === "/music.pcm") {
    fs.stat(PCM_PATH, (error, stat) => {
      if (error || !stat.isFile() || stat.size <= 0 || stat.size % 2 !== 0) {
        sendText(response, 404, "No valid music.pcm has been uploaded");
        return;
      }
      response.writeHead(200, {
        "Content-Type": "application/octet-stream",
        "Content-Length": stat.size,
        "Cache-Control": "no-store",
        "Accept-Ranges": "none",
        "X-Nuwax-Audio-Format": "pcm_s16le_16000_mono",
      });
      fs.createReadStream(PCM_PATH).pipe(response);
    });
    return;
  }
  if (request.method === "GET") {
    serveStatic(url.pathname, response);
    return;
  }
  sendText(response, 405, "Method not allowed");
});

server.listen(PORT, HOST, () => {
  console.log(`Nuwax music URL test server listening on ${HOST}:${PORT}`);
  for (const address of localIpv4Addresses()) {
    console.log(`Page:   http://${address}:${PORT}/`);
    console.log(`Device: http://${address}:${PORT}/music.pcm`);
  }
});
