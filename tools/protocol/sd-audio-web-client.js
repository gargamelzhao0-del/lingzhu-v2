"use strict";
const $ = id => document.getElementById(id);
const states = { complete: "整场接收完成", receiving: "场次结束待确认", incomplete: "场次不完整", legacy: "旧协议 · 结束未知" };
const types = { meeting: "会议", realtime: "实时", history: "历史补传", legacy: "旧协议补传" };
let sessions = [], selected = null, mode = "all", offset = 0, detail = null, busy = false, playbackToken = 0, polling = false;
let listSignature = "";
function duration(ms) { const seconds = Math.floor(ms / 1000), h = Math.floor(seconds / 3600), m = Math.floor(seconds % 3600 / 60), s = seconds % 60; return h ? `${h}:${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}` : `${m}:${String(s).padStart(2, "0")}`; }
function bytes(n) { return n >= 1048576 ? `${(n / 1048576).toFixed(1)} MiB` : `${(n / 1024).toFixed(1)} KiB`; }
function date(value) { return value ? new Date(value).toLocaleString("zh-CN", { month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit" }) : "暂无时间"; }
function el(tag, text, className) { const e = document.createElement(tag); if (text !== undefined) e.textContent = text; if (className) e.className = className; return e; }
async function api(url, options) { const r = await fetch(url, { cache: "no-store", ...options }); const data = await r.json(); if (!r.ok) throw new Error(data.error || `请求失败 ${r.status}`); return data; }
function notice(text) { $("notice").textContent = text; $("notice").hidden = !text; }
function renderList() {
  const query = $("search").value.trim().toLowerCase();
  const visible = sessions.filter(s => (mode === "all" || s.mode === mode) && `${s.deviceId} ${s.sessionId}`.toLowerCase().includes(query));
  const signature = JSON.stringify([selected, mode, query, visible.map(s => [s.id, s.revision, s.state, s.sessionTime])]);
  if (signature === listSignature) return;
  listSignature = signature;
  const nodes = visible.map(s => {
    const button = el("button", undefined, `recording${s.id === selected ? " selected" : ""}`);
    button.setAttribute("aria-pressed", s.id === selected ? "true" : "false");
    const top = el("div", undefined, "recording-top");
    top.append(el("span", s.mode === "meeting" ? "会议录音" : "监控录音", "recording-title"), el("span", states[s.state], `badge ${s.state}`));
    const bottom = el("div", undefined, "recording-bottom");
    bottom.append(el("b", duration(s.durationMs)), el("span", `${s.count} 个分片 · ${bytes(s.bytes)}`),
      el("span", s.sessionTime?.started_at_ms ? `开始 ${date(s.sessionTime.started_at_ms)}` : `接收 ${date(s.updatedAt)}`));
    button.append(top, el("div", `${s.deviceId} / ${s.sessionId}`, "id"), bottom);
    button.onclick = () => select(s.id); return button;
  });
  $("recordings").replaceChildren(...(nodes.length ? nodes : [el("div", query || mode !== "all" ? "没有符合条件的录音" : "等待设备上传第一场录音。会议需结束后才开始上传。", "empty")]));
}
function renderDetail(item) {
  detail = item; $("detail-empty").hidden = true; $("detail-content").hidden = false;
  $("detail-title").textContent = item.mode === "meeting" ? "会议录音" : "监控录音";
  $("detail-state").textContent = states[item.state]; $("detail-state").className = `badge ${item.state}`;
  $("detail-id").textContent = item.sessionId; $("detail-device").textContent = `设备 ${item.deviceId}`;
  $("detail-duration").textContent = duration(item.durationMs);
  $("detail-started").textContent = item.sessionTime?.started_at_ms ?
    new Date(item.sessionTime.started_at_ms).toLocaleString("zh-CN", { hour12: false }) : "未上报";
  $("detail-session-duration").textContent = item.sessionTime?.duration_ms == null ? "未上报" : duration(item.sessionTime.duration_ms);
  $("detail-count").textContent = item.expectedSegments === null ? `${item.count} 个` : `${item.count} / ${item.expectedSegments}`;
  $("detail-types").textContent = Object.entries(item.types).filter(([, n]) => n).map(([t, n]) => `${types[t] || t} ${n}`).join(" · ");
  $("play").disabled = busy || !item.count;
  $("segment-page").textContent = item.count ? `${offset + 1}–${Math.min(offset + 100, item.count)} / ${item.count}` : "暂无分片";
  $("segments").replaceChildren(...item.records.map(r => {
    const row = el("div", undefined, "segment");
    const started = r.fileStartedAtMs == null ? "采集时间未知" : new Date(r.fileStartedAtMs).toLocaleString("zh-CN", { hour12: false });
    const span = r.fileDurationMs == null ? "跨度未知" : `跨度 ${(r.fileDurationMs / 1000).toFixed(2)} 秒`;
    row.title = `${r.segmentId}\n${started}\n${span}\n有效音频 ${(r.durationMs / 1000).toFixed(2)} 秒`;
    const timing = el("span");
    timing.append(el("span", duration(r.durationMs)), el("br"), el("small", r.fileStartedAtMs == null ? "采集时间未知" : date(r.fileStartedAtMs)), el("br"), el("small", span));
    row.append(el("b", `#${String(r.index).padStart(4, "0")}`), el("span", types[r.type]), timing, el("span", bytes(r.bytes)), el("span", r.complete ? "已确认" : "录音不完整")); return row;
  }));
  $("previous").disabled = offset === 0; $("next").disabled = offset + 100 >= item.count;
}
async function loadDetail() {
  if (!selected) return;
  const id = selected, page = offset;
  const item = await api(`/api/sessions/${id}?offset=${page}`);
  if (id === selected && page === offset) renderDetail(item);
}
async function select(id) {
  if (selected !== id) {
    playbackToken++; busy = false; $("audio").pause(); $("audio").removeAttribute("src"); $("audio").load();
    $("audio").hidden = true; $("download").hidden = true; $("player-status").textContent = "";
    $("play").textContent = "准备并播放录音";
  }
  selected = id; offset = 0; renderList();
  try { await loadDetail(); } catch (error) { notice(error.message); }
}
async function refresh() {
  if (polling) return; polling = true;
  try {
    const data = await api("/api/status"); sessions = data.sessions;
    $("endpoint").textContent = `${data.endpoint}${data.discovery?.ready ? " · 自动发现已开启" : ""}`;
    $("dot").className = "dot ok"; $("connection").textContent = data.devices.length ? `${data.devices.length} 台设备已连接` : "网关运行中 · 等待设备";
    $("session-count").textContent = sessions.length;
    $("complete-count").textContent = `${sessions.filter(s => s.state === "complete").length} 场已确认完整`;
    $("duration").textContent = duration(sessions.reduce((n, s) => n + s.durationMs, 0));
    $("size").textContent = bytes(sessions.reduce((n, s) => n + s.bytes, 0));
    $("file-count").textContent = `${sessions.reduce((n, s) => n + s.count, 0)} 份已确认文件`;
    $("speed").textContent = (data.bytesPerSecond / 1024).toFixed(1);
    $("updated").textContent = `${new Date(data.now).toLocaleTimeString("zh-CN")} 更新`;
    $("transfers").hidden = !data.pending.length;
    $("transfer-list").replaceChildren(...data.pending.map(t => {
      const row = el("div", undefined, "transfer"), bar = el("progress"); bar.max = t.total; bar.value = t.received;
      row.append(el("span", `${t.deviceId} · ${t.connected ? "上传中" : "等待设备续传"}`), bar, el("span", `${bytes(t.received)} / ${bytes(t.total)}`)); return row;
    }));
    notice(data.discovery?.enabled && !data.discovery.ready ? `网关自动发现不可用：${data.discovery.error || "正在启动"}` :
      data.issues.length ? `${data.issues.length} 份录音无法读取：${data.issues[0].error}` : "");
    renderList(); if (selected) await loadDetail();
  } catch (error) { $("dot").className = "dot"; $("connection").textContent = "网关连接中断"; notice(`无法读取网关：${error.message}。请确认电脑上的网关仍在运行。`); }
  finally { polling = false; }
}
$("play").onclick = async () => {
  if (!selected || busy) return;
  const token = ++playbackToken, id = selected; busy = true; $("play").disabled = true;
  $("audio").pause(); $("player-status").textContent = "正在校验并准备音频…";
  try {
    let job = await api(`/api/sessions/${id}/prepare`, { method: "POST" });
    while (job.state === "preparing" && token === playbackToken) {
      $("player-status").textContent = `正在准备 ${job.completed} / ${job.total} 个分片，设备上传可继续。`;
      await new Promise(resolve => setTimeout(resolve, 600));
      if (token !== playbackToken) return;
      job = await api(`/api/jobs/${job.id}`);
    }
    if (token !== playbackToken) return;
    if (job.state !== "ready") throw new Error(job.error || "音频准备失败");
    $("audio").src = job.url; $("audio").hidden = false;
    $("download").href = `${job.url}?download=1`; $("download").hidden = false;
    $("player-status").textContent = `校验通过 · ${duration(job.durationMs)}${job.discontinuities ? ` · ${job.discontinuities} 处录音不连续点` : ""}`;
    $("play").textContent = "重新加载并播放";
    try { await $("audio").play(); } catch { $("player-status").textContent += " · 请点击播放器的播放键"; }
  } catch (error) { if (token === playbackToken) $("player-status").textContent = error.message; }
  finally { if (token === playbackToken) { busy = false; $("play").disabled = false; } }
};
$("audio").addEventListener("error", () => { if ($("audio").getAttribute("src")) $("player-status").textContent = "音频加载失败，请重新加载录音。"; });
document.querySelectorAll("[data-mode]").forEach(button => { button.onclick = () => { mode = button.dataset.mode; document.querySelectorAll("[data-mode]").forEach(b => b.classList.toggle("active", b === button)); renderList(); }; });
$("search").oninput = renderList; $("refresh").onclick = refresh;
$("previous").onclick = () => { offset = Math.max(0, offset - 100); loadDetail().catch(e => notice(e.message)); };
$("next").onclick = () => { offset += 100; loadDetail().catch(e => notice(e.message)); };
$("endpoint").textContent = `ws://${location.host}/api/device/ws`;
refresh(); setInterval(() => { if (!document.hidden) refresh(); }, 3000);
