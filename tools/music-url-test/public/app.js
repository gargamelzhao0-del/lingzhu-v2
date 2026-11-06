"use strict";

const fileInput = document.querySelector("#audio-file");
const dropZone = document.querySelector("#drop-zone");
const uploadTitle = document.querySelector("#upload-title");
const message = document.querySelector("#message");
const messageIcon = document.querySelector("#message-icon");
const statusFooter = document.querySelector(".status-footer");
const progressWrap = document.querySelector("#progress-wrap");
const progress = document.querySelector("#progress");
const progressLabel = document.querySelector("#progress-label");
const progressValue = document.querySelector("#progress-value");
const deviceUrl = document.querySelector("#device-url");
const serverState = document.querySelector("#server-state");
const serverStateText = document.querySelector("#server-state-text");
const fileSize = document.querySelector("#file-size");
const duration = document.querySelector("#duration");
const preview = document.querySelector("#preview");
const audioPreview = document.querySelector("#audio-preview");
const sourceName = document.querySelector("#source-name");
const copyButton = document.querySelector("#copy-url");
const targetDevice = document.querySelector("#target-device");
const sendMusic = document.querySelector("#send-music");
const stopMusic = document.querySelector("#stop-music");
let latestStatus = null;
let previewObjectUrl = "";
let dragDepth = 0;
let uploadInProgress = false;

function setProgress(value, label) {
  progressWrap.hidden = false;
  progress.value = value;
  progressValue.textContent = `${Math.round(value)}%`;
  progressLabel.textContent = label;
}

function setMessage(value, kind = "") {
  message.textContent = value;
  statusFooter.className = `status-footer${kind ? ` ${kind}` : ""}`;
  messageIcon.textContent = kind === "success" ? "✓" : kind === "error" ? "!" : "i";
}

function setBusy(busy) {
  uploadInProgress = busy;
  fileInput.disabled = busy;
  dropZone.classList.toggle("busy", busy);
  dropZone.setAttribute("aria-busy", String(busy));
}

function formatBytes(bytes) {
  if (!bytes) return "-";
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
}

function formatDuration(seconds) {
  if (!seconds) return "-";
  const totalSeconds = Math.round(seconds);
  const minutes = Math.floor(totalSeconds / 60);
  const remainder = String(totalSeconds % 60).padStart(2, "0");
  return `${minutes}:${remainder}`;
}

function downmix(buffer, frame) {
  let sum = 0;
  for (let channel = 0; channel < buffer.numberOfChannels; channel += 1) {
    sum += buffer.getChannelData(channel)[frame];
  }
  return sum / buffer.numberOfChannels;
}

function resampleToPcm16(buffer, targetRate = 16000) {
  const outputFrames = Math.max(1, Math.round(buffer.duration * targetRate));
  const output = new ArrayBuffer(outputFrames * 2);
  const view = new DataView(output);
  const ratio = buffer.sampleRate / targetRate;

  for (let frame = 0; frame < outputFrames; frame += 1) {
    const position = frame * ratio;
    const lower = Math.min(buffer.length - 1, Math.floor(position));
    const upper = Math.min(buffer.length - 1, lower + 1);
    const fraction = position - lower;
    const sample =
      downmix(buffer, lower) * (1 - fraction) +
      downmix(buffer, upper) * fraction;
    const clamped = Math.max(-1, Math.min(1, sample));
    const pcm = clamped < 0
      ? Math.round(clamped * 32768)
      : Math.round(clamped * 32767);
    view.setInt16(frame * 2, pcm, true);
  }
  return output;
}

function pcmWaveBlob(pcm, sampleRate = 16000) {
  const header = new ArrayBuffer(44);
  const view = new DataView(header);
  const write = (offset, value) => {
    [...value].forEach((character, index) => {
      view.setUint8(offset + index, character.charCodeAt(0));
    });
  };
  write(0, "RIFF");
  view.setUint32(4, 36 + pcm.byteLength, true);
  write(8, "WAVE");
  write(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  write(36, "data");
  view.setUint32(40, pcm.byteLength, true);
  return new Blob([header, pcm], { type: "audio/wav" });
}

async function refreshStatus(options = {}) {
  const response = await fetch("/api/status", { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`状态接口返回 ${response.status}`);
  }
  const status = await response.json();
  latestStatus = status;
  const source = status.source;
  const ready = Boolean(source) || status.ready;
  const deviceOrigin = status.device_urls?.[0] ? new URL(status.device_urls[0]).origin : location.origin;
  deviceUrl.textContent = source ? `${deviceOrigin}${source.path}` : status.device_urls[0] || `${location.origin}/music.pcm`;
  serverStateText.textContent = ready ? "音频已就绪" : "等待音频";
  serverState.classList.toggle("ready", ready);
  fileSize.textContent = formatBytes(source?.bytes || status.bytes);
  duration.textContent = source?.format === "mp3" ? "以原文件为准" : formatDuration((source?.bytes || status.bytes) / 32000);
  document.querySelector("#source-format").textContent = source?.format === "mp3" ? "MP3 原文件 / 单声道输出" : "PCM16 / 单声道";
  const devices = status.devices || [];
  const selected = targetDevice.value;
  const choices = devices.map(d => [d.device_id, `${d.device_id} · ${d.online ? "在线" : "离线"}`]);
  if (JSON.stringify(choices) !== targetDevice.dataset.choices) {
    targetDevice.replaceChildren();
    if (!choices.length) targetDevice.add(new Option("等待设备连接", ""));
    for (const [value, label] of choices) targetDevice.add(new Option(label, value));
    if (devices.some(d => d.device_id === selected)) targetDevice.value = selected;
    targetDevice.dataset.choices = JSON.stringify(choices);
  }
  refreshDeviceControls();
  document.body.dataset.audioReady = String(ready);
  if (!options.silent && !uploadInProgress) {
    setMessage(
      ready
        ? "音频已就绪，点击发送播放。"
        : "请选择一个音频文件。",
      ready ? "success" : "",
    );
  }
}

async function upload(file) {
  setBusy(true);
  uploadTitle.textContent = file.name;
  setMessage("正在准备音频，请保持页面打开。", "");
  setProgress(8, "读取文件");
  const encoded = await file.arrayBuffer();
  if (/\.mp3$/i.test(file.name) || file.type === "audio/mpeg") {
    try {
      if (encoded.byteLength > 32000000) throw new Error("MP3 文件超过 32 MB。");
      setProgress(45, "上传 MP3 原文件");
      const response = await fetch("/api/music/mp3", {
        method: "POST", headers: { "Content-Type": "audio/mpeg" }, body: encoded,
      });
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error(result.error || "上传失败");
      if (previewObjectUrl) URL.revokeObjectURL(previewObjectUrl);
      previewObjectUrl = URL.createObjectURL(file);
      audioPreview.src = previewObjectUrl;
      sourceName.textContent = file.name;
      preview.hidden = false;
      setProgress(100, "原文件已上传");
      await refreshStatus({ silent: true });
      setMessage("MP3 已就绪，点击发送播放。设备按原采样率播放。", "success");
    } finally { setBusy(false); refreshDeviceControls(); }
    return;
  }
  setProgress(24, "解码音频");
  const context = new AudioContext();
  try {
    const decoded = await context.decodeAudioData(encoded.slice(0));
    setProgress(48, "混合并重采样");
    const pcm = resampleToPcm16(decoded);
    if (pcm.byteLength > 19_200_000) {
      throw new Error("转换结果超过 19.2 MB，请使用十分钟以内的测试音频。");
    }
    setProgress(72, "上传设备播放源");
    const response = await fetch("/api/music", {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: pcm,
    });
    const result = await response.json();
    if (!response.ok || !result.ok) {
      throw new Error(result.error || `上传失败：${response.status}`);
    }
    setProgress(100, "处理完成");
    if (previewObjectUrl) URL.revokeObjectURL(previewObjectUrl);
    previewObjectUrl = URL.createObjectURL(pcmWaveBlob(pcm));
    audioPreview.src = previewObjectUrl;
    sourceName.textContent = file.name;
    preview.hidden = false;
    await refreshStatus({ silent: true });
    setMessage("PCM 已就绪，点击发送播放；旧 /music.pcm 接口也可继续使用。", "success");
  } finally {
    await context.close();
    setBusy(false);
  }
}

async function handleFile(file) {
  if (!file || uploadInProgress) return;
  try {
    await upload(file);
  } catch (error) {
    setMessage(error.message || String(error), "error");
    progressLabel.textContent = "处理失败";
    setBusy(false);
  } finally {
    fileInput.value = "";
  }
}

async function copyText(value) {
  if (navigator.clipboard && window.isSecureContext) {
    await navigator.clipboard.writeText(value);
    return;
  }
  const textArea = document.createElement("textarea");
  textArea.value = value;
  textArea.setAttribute("readonly", "");
  textArea.style.position = "fixed";
  textArea.style.opacity = "0";
  document.body.appendChild(textArea);
  textArea.select();
  const copied = document.execCommand("copy");
  textArea.remove();
  if (!copied) throw new Error("浏览器未允许复制，请手动选择 URL。");
}

fileInput.addEventListener("change", () => handleFile(fileInput.files[0]));

dropZone.addEventListener("dragenter", (event) => {
  event.preventDefault();
  dragDepth += 1;
  dropZone.classList.add("drag");
});

dropZone.addEventListener("dragover", (event) => {
  event.preventDefault();
});

dropZone.addEventListener("dragleave", (event) => {
  event.preventDefault();
  dragDepth = Math.max(0, dragDepth - 1);
  if (dragDepth === 0) dropZone.classList.remove("drag");
});

dropZone.addEventListener("drop", (event) => {
  event.preventDefault();
  dragDepth = 0;
  dropZone.classList.remove("drag");
  handleFile(event.dataTransfer.files[0]);
});

copyButton.addEventListener("click", async () => {
  try {
    await copyText(deviceUrl.textContent);
    copyButton.classList.add("copied");
    copyButton.title = "已复制";
    setMessage("设备播放 URL 已复制。", "success");
    window.setTimeout(() => {
      copyButton.classList.remove("copied");
      copyButton.title = "复制设备播放 URL";
    }, 1600);
  } catch (error) {
    setMessage(error.message || String(error), "error");
  }
});

refreshStatus().catch((error) => {
  serverStateText.textContent = "服务异常";
  setMessage(error.message, "error");
});

window.setInterval(() => {
  if (!document.hidden && !uploadInProgress) {
    refreshStatus({ silent: true }).catch(() => {});
  }
}, 1000);

function refreshDeviceControls() {
  const device = latestStatus?.devices?.find(d => d.device_id === targetDevice.value);
  sendMusic.disabled = uploadInProgress || !latestStatus?.source || !device?.online;
  stopMusic.disabled = !device?.online;
  const states = { idle: "待机", buffering: "正在缓冲", playing: "正在播放", stopping: "正在停止", completed: "播放完成", cancelled: "已中止", failed: "播放失败" };
  let label = device?.online ? (states[device.state] || "已连接") : "等待设备连接";
  if (device?.pending) label = "指令已发送，等待设备处理";
  else if (device?.ack && !device.accepted) label = `设备未接受：${device.reason}`;
  else if (device?.playback_reason && ["failed", "cancelled"].includes(device.state)) label += ` · ${device.playback_reason}`;
  document.querySelector("#device-playback-state").textContent = label;
  document.querySelector("#sample-rate").textContent = device?.sample_rate ? `${device.sample_rate.toLocaleString()} Hz` : latestStatus?.source?.format === "pcm" ? "16,000 Hz" : "跟随 MP3 原采样率";
}
async function sendControl(kind) {
  try {
    const response = await fetch(`/api/${kind}`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ device_id: targetDevice.value }) });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || "发送失败");
    setMessage(kind === "play" ? "播放请求已发送，等待设备确认。" : "中止请求已发送。", "success");
    await refreshStatus({ silent: true });
  } catch (error) { setMessage(error.message, "error"); }
}
targetDevice.addEventListener("change", refreshDeviceControls);
sendMusic.addEventListener("click", () => sendControl("play"));
stopMusic.addEventListener("click", () => sendControl("stop"));
