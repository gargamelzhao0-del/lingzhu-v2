"use strict";

const emotions = [
  { id: "neutral", label: "平静", title: "平静", hint: "轻松自在", category: "平和" },
  { id: "happy", label: "开心", title: "开心", hint: "见到你真好", category: "积极" },
  { id: "surprised", label: "惊讶", title: "惊讶", hint: "原来如此", category: "反应" },
  { id: "wink", label: "眨眼", title: "眨眼", hint: "收到", category: "活泼" },
  { id: "tongue", label: "调皮", title: "调皮", hint: "逗你一下", category: "活泼" },
  { id: "thinking", label: "思考", title: "思考", hint: "请稍候", category: "认知" },
  { id: "focused", label: "专注", title: "专注", hint: "正在专心处理", category: "认知" },
  { id: "pensive", label: "沉思", title: "沉思", hint: "正在整理思路", category: "平和" },
  { id: "analyzing", label: "分析", title: "分析", hint: "正在读取信息", category: "认知" },
  { id: "processing", label: "处理", title: "处理", hint: "正在处理", category: "认知" },
  { id: "received", label: "收到", title: "收到", hint: "消息已收到", category: "反馈" },
  { id: "what", label: "疑问", title: "疑问", hint: "可以再说一遍吗", category: "反应" },
  { id: "understood", label: "明白", title: "明白", hint: "已经理解", category: "反馈" },
  { id: "awesome", label: "惊喜", title: "惊喜", hint: "完成得很好", category: "积极" },
  { id: "like", label: "喜爱", title: "喜爱", hint: "我很喜欢", category: "积极" },
  { id: "sad", label: "难过", title: "难过", hint: "有些不开心", category: "消极" },
  { id: "lost", label: "迷茫", title: "迷茫", hint: "正在寻找方向", category: "消极" },
  { id: "angry", label: "生气", title: "生气", hint: "需要立即处理", category: "警告" },
  { id: "shy", label: "害羞", title: "害羞", hint: "你好呀", category: "活泼" },
  { id: "tired", label: "疲惫", title: "疲惫", hint: "想休息一下", category: "低能量" },
  { id: "booting", label: "启动", title: "启动", hint: "系统正在启动", category: "系统" },
  { id: "listening", label: "聆听", title: "聆听", hint: "我在听", category: "对话" },
  { id: "speaking", label: "播报", title: "播报", hint: "正在回答", category: "对话" },
  { id: "low_battery", label: "低电量", title: "低电量", hint: "请连接电源", category: "警告" },
  { id: "charging", label: "充电", title: "充电", hint: "正在补充能量", category: "系统" }
];

const menuItems = [
  { id: "claw", label: "机械爪", icon: "hand", description: "机械爪控制", status: "已就绪", accent: "#0b8f58", soft: "#dff3e9" },
  { id: "work", label: "工作", icon: "briefcase-business", description: "专注与工作流程", status: "待命", accent: "#3c72be", soft: "#e1ebf8" },
  { id: "issues", label: "问题", icon: "shield-alert", description: "设备检查与异常", status: "0 项", accent: "#d45140", soft: "#f8e5e1" },
  { id: "wifi", label: "WiFi", icon: "wifi", description: "网络连接", status: "已连接", accent: "#16869a", soft: "#dff1f3" },
  { id: "volume", label: "音量", icon: "volume-2", description: "扬声器音量", status: "68%", accent: "#b27a12", soft: "#f7edda" },
  { id: "device", label: "设备", icon: "settings-2", description: "设备设置与服务", status: "正常", accent: "#535b70", soft: "#e7e9ef" }
];

const stateLabels = {
  wifi: {
    connected: ["已连接", "online"],
    connecting: ["连接中", "working"],
    weak: ["信号弱", "warning"],
    disconnected: ["已断开", "error"]
  },
  cloud: {
    connected: ["在线", "online"],
    connecting: ["连接中", "working"],
    disconnected: ["离线", "muted"],
    authfail: ["鉴权失败", "error"]
  },
  dialog: {
    sleep: ["待命", ""],
    listening: ["聆听", "active"],
    recognizing: ["识别", "active"],
    thinking: ["思考", "thinking"],
    speaking: ["播报", "speaking"],
    meeting: ["会议", "meeting"],
    monitoring: ["监控", "monitoring"],
    error: ["错误", "error"]
  },
  power: {
    external: ["外接供电", "online"],
    battery: ["电池供电", "muted"],
    charging: ["充电中", "warning"],
    low: ["低电量", "error"]
  }
};

const dialogIcons = {
  sleep: "moon-star",
  listening: "audio-lines",
  recognizing: "scan-line",
  thinking: "sparkles",
  speaking: "volume-2",
  meeting: "users-round",
  monitoring: "scan-eye",
  error: "triangle-alert"
};

const refs = {};
let currentEmotion = 0;
let currentMenu = 0;
let currentView = "face";
let bootTimer = 0;
let provisionStep = 0;
let provisionTimer = 0;
let touchStart = null;
let menuAnimationTimer = 0;
let menuWheelLocked = false;
let menuDrag = null;
let suppressMenuClick = false;
let pendingMenuOpenIndex = null;

const MENU_DRAG_RANGE = 104;
const MENU_DRAG_COMMIT = 46;
const MENU_SLOT_STYLES = {
  "-2": { x: 132, y: 42, size: 24, opacity: 0, emphasis: 0 },
  "-1": { x: 156, y: 94, size: 25, opacity: 0.31, emphasis: 0 },
  "0": { x: 180, y: 130, size: 52, opacity: 1, emphasis: 1 },
  "1": { x: 204, y: 246, size: 25, opacity: 0.31, emphasis: 0 },
  "2": { x: 228, y: 314, size: 24, opacity: 0, emphasis: 0 }
};

function byId(id) {
  return document.getElementById(id);
}

function iconMarkup(name) {
  return `<i data-lucide="${name}" aria-hidden="true"></i>`;
}

function refreshIcons() {
  if (window.lucide) {
    window.lucide.createIcons({ attrs: { "stroke-width": 1.8 } });
  }
}

function cacheRefs() {
  [
    "deviceScreen", "faceStage", "faceTitle", "emotionName", "emotionHint", "emotionSelect",
    "emotionCount", "emotionCategory", "emotionDisplayName", "currentScreenName", "statusDrawer",
    "menuPosition", "menuWheel", "activeMenuLabel", "activeMenuDescription",
    "menuProgress", "detailTitle", "detailStatus", "detailContent", "wifiState", "cloudState",
    "dialogState", "powerState", "wifiValue", "cloudValue", "micValue", "powerValue", "wifiCell",
    "cloudCell", "micCell", "powerCell", "topWifi", "topCloud", "topMic", "topPower", "dialogMark",
    "dialogLabel", "drawerFooter", "volumeRange", "volumeValue", "screenVolume", "screenVolumeValue",
    "drawerClock", "provisionTitle", "provisionCopy", "bleTimer", "qrCanvas", "screenMenuTrigger",
    "dialogStatusIcon"
  ].forEach((id) => {
    refs[id] = byId(id);
  });
}

function setView(view, options = {}) {
  const valid = ["face", "menu", "detail", "qr", "provisioning"];
  const next = valid.includes(view) ? view : "face";
  currentView = next;

  document.querySelectorAll(".screen-view").forEach((element) => element.classList.remove("is-active"));
  const viewMap = {
    face: "faceView",
    menu: "menuView",
    detail: "detailView",
    qr: "qrView",
    provisioning: "provisioningView"
  };
  byId(viewMap[next]).classList.add("is-active");

  if (!options.keepDrawer) {
    closeDrawer(false);
  }

  document.querySelectorAll("#viewControls button").forEach((button) => {
    const selected = button.dataset.view === next || (button.dataset.view === "device" && next === "detail");
    button.classList.toggle("is-selected", selected);
  });

  const screenLabels = {
    face: `表情 / ${emotions[currentEmotion].title}`,
    menu: `菜单 / ${menuItems[currentMenu].label}`,
    detail: `详情 / ${menuItems[currentMenu].label}`,
    qr: "设备 / QR",
    provisioning: "设备 / BLE 配网"
  };
  refs.currentScreenName.textContent = screenLabels[next];
}

function openDrawer() {
  if (refs.deviceScreen.classList.contains("is-booting")) return;
  refs.statusDrawer.classList.add("is-open");
  refs.currentScreenName.textContent = "系统 / 状态";
  document.querySelectorAll("#viewControls button").forEach((button) => {
    button.classList.toggle("is-selected", button.dataset.view === "drawer");
  });
}

function closeDrawer(updateControls = true) {
  refs.statusDrawer.classList.remove("is-open");
  if (updateControls) setView(currentView, { keepDrawer: true });
}

function applyEmotion(indexOrId) {
  const index = typeof indexOrId === "number"
    ? (indexOrId + emotions.length) % emotions.length
    : emotions.findIndex((emotion) => emotion.id === indexOrId);
  if (index < 0) return;

  currentEmotion = index;
  const emotion = emotions[index];
  refs.faceStage.className = `face-stage emotion-${emotion.id}`;
  refs.emotionName.textContent = emotion.title;
  refs.emotionHint.textContent = emotion.hint;
  if (refs.faceTitle) refs.faceTitle.textContent = `${emotion.label}表情`;
  refs.emotionSelect.value = emotion.id;
  refs.emotionCount.textContent = `${String(index + 1).padStart(2, "0")} / ${emotions.length}`;
  refs.emotionCategory.textContent = emotion.category;
  refs.emotionDisplayName.textContent = emotion.label;

  if (currentView === "face" && !refs.statusDrawer.classList.contains("is-open")) {
    refs.currentScreenName.textContent = `表情 / ${emotion.title}`;
  }
}

function updateMenu() {
  const item = menuItems[currentMenu];

  refs.menuPosition.textContent = `${String(currentMenu + 1).padStart(2, "0")} / ${String(menuItems.length).padStart(2, "0")}`;
  refs.activeMenuLabel.textContent = item.label;
  refs.activeMenuDescription.textContent = item.description;
  refs.currentScreenName.textContent = `菜单 / ${item.label}`;

  refs.menuProgress.innerHTML = menuItems
    .map((_, index) => `<span class="${index === currentMenu ? "is-active" : ""}"></span>`)
    .join("");
  renderMenuWheel();
}

function wrapMenuIndex(index) {
  return (index + menuItems.length) % menuItems.length;
}

function mixMenuValue(from, to, progress) {
  return from + ((to - from) * progress);
}

function menuSlotStyle(role) {
  if (role < -2) return MENU_SLOT_STYLES["-2"];
  if (role > 2) return MENU_SLOT_STYLES["2"];
  return MENU_SLOT_STYLES[String(role)];
}

function mixedMenuSlotStyle(fromRole, toRole, progress) {
  const from = menuSlotStyle(fromRole);
  const to = menuSlotStyle(toRole);
  return {
    x: mixMenuValue(from.x, to.x, progress),
    y: mixMenuValue(from.y, to.y, progress),
    size: mixMenuValue(from.size, to.size, progress),
    opacity: mixMenuValue(from.opacity, to.opacity, progress),
    emphasis: mixMenuValue(from.emphasis, to.emphasis, progress)
  };
}

function menuItemMarkup(index, style) {
  const item = menuItems[index];
  const isCurrent = style.emphasis >= 0.5;
  const labelSize = mixMenuValue(11, 18, style.emphasis);
  const labelGap = mixMenuValue(4, 9, style.emphasis);
  const underlineOpacity = Math.max(0, (style.emphasis - 0.55) / 0.45);
  const issueDot = item.id === "issues" && item.status !== "0 项"
    ? `<span class="menu-issue-dot" aria-hidden="true"></span>`
    : "";

  return `
    <button class="menu-wheel-item${isCurrent ? " is-current" : ""}"
      type="button" data-menu-index="${index}" data-menu-id="${item.id}" aria-label="打开${item.label}"
      ${isCurrent ? 'aria-current="true"' : 'tabindex="-1"'}
      style="left:${style.x.toFixed(2)}px;top:${style.y.toFixed(2)}px;opacity:${style.opacity.toFixed(3)};
      --icon-size:${style.size.toFixed(2)}px;--label-size:${labelSize.toFixed(2)}px;
      --label-gap:${labelGap.toFixed(2)}px;--underline-opacity:${underlineOpacity.toFixed(3)};
      --menu-accent:${item.accent};--menu-soft:${item.soft}">
      <span class="menu-wheel-icon-wrap">
        <span class="menu-wheel-icon">${iconMarkup(item.icon)}</span>
        ${issueDot}
      </span>
      <strong>${item.label}</strong>
    </button>`;
}

function updateMenuItemElement(button, index, style) {
  const item = menuItems[index];
  const isCurrent = style.emphasis >= 0.5;
  const labelSize = mixMenuValue(11, 18, style.emphasis);
  const labelGap = mixMenuValue(4, 9, style.emphasis);
  const underlineOpacity = Math.max(0, (style.emphasis - 0.55) / 0.45);

  button.classList.toggle("is-current", isCurrent);
  button.setAttribute("aria-label", `打开${item.label}`);
  button.tabIndex = isCurrent ? 0 : -1;
  if (isCurrent) button.setAttribute("aria-current", "true");
  else button.removeAttribute("aria-current");
  button.style.cssText = `left:${style.x.toFixed(2)}px;top:${style.y.toFixed(2)}px;opacity:${style.opacity.toFixed(3)};
    --icon-size:${style.size.toFixed(2)}px;--label-size:${labelSize.toFixed(2)}px;
    --label-gap:${labelGap.toFixed(2)}px;--underline-opacity:${underlineOpacity.toFixed(3)};
    --menu-accent:${item.accent};--menu-soft:${item.soft}`;
}

function renderMenuWheel(animation = null) {
  if (!refs.menuWheel) return;
  const roles = animation
    ? (animation.direction > 0 ? [-1, 0, 1, 2] : [-2, -1, 0, 1])
    : [-1, 0, 1];

  const desiredIndexes = new Set();
  let iconsAdded = false;

  roles.forEach((role) => {
    const index = wrapMenuIndex(currentMenu + role);
    const style = animation
      ? mixedMenuSlotStyle(role, role - animation.direction, animation.progress)
      : menuSlotStyle(role);
    desiredIndexes.add(index);

    let button = refs.menuWheel.querySelector(`[data-menu-index="${index}"]`);
    if (!button) {
      refs.menuWheel.insertAdjacentHTML("beforeend", menuItemMarkup(index, style));
      button = refs.menuWheel.querySelector(`[data-menu-index="${index}"]`);
      iconsAdded = true;
    }
    updateMenuItemElement(button, index, style);
  });

  refs.menuWheel.querySelectorAll("[data-menu-index]").forEach((button) => {
    if (!desiredIndexes.has(Number(button.dataset.menuIndex))) button.remove();
  });

  const describedIndex = animation && animation.progress >= 0.5
    ? wrapMenuIndex(currentMenu + animation.direction)
    : currentMenu;
  refs.activeMenuDescription.textContent = menuItems[describedIndex].description;
  if (iconsAdded) refreshIcons();
}

function easeOutCubic(progress) {
  return 1 - Math.pow(1 - progress, 3);
}

function animateMenu(direction, fromProgress, toProgress, commit) {
  window.cancelAnimationFrame(menuAnimationTimer);
  menuWheelLocked = true;
  const startedAt = performance.now();
  const duration = commit ? 190 : 150;
  refs.menuWheel.classList.add("is-settling");

  function frame(now) {
    const elapsed = Math.min(1, (now - startedAt) / duration);
    const progress = mixMenuValue(fromProgress, toProgress, easeOutCubic(elapsed));
    renderMenuWheel({ direction, progress });
    if (elapsed < 1) {
      menuAnimationTimer = window.requestAnimationFrame(frame);
      return;
    }
    refs.menuWheel.classList.remove("is-settling");
    if (commit) currentMenu = wrapMenuIndex(currentMenu + direction);
    updateMenu();
    const shouldOpen = pendingMenuOpenIndex === currentMenu;
    pendingMenuOpenIndex = null;
    menuWheelLocked = false;
    if (shouldOpen) openDetail();
  }

  menuAnimationTimer = window.requestAnimationFrame(frame);
}

function moveMenu(direction) {
  if (menuWheelLocked) return;
  menuWheelLocked = true;
  animateMenu(direction > 0 ? 1 : -1, 0, 1, true);
}

function handleMenuWheel(event) {
  if (currentView !== "menu" || refs.statusDrawer.classList.contains("is-open")) return;
  event.preventDefault();
  if (menuWheelLocked || menuDrag) return;
  const delta = event.deltaY;
  if (Math.abs(delta) < 6) return;
  moveMenu(delta > 0 ? 1 : -1);
}

function beginMenuDrag(event) {
  if (currentView !== "menu" || refs.statusDrawer.classList.contains("is-open")) return;
  if (menuWheelLocked || refs.menuWheel.classList.contains("is-settling")) {
    const button = event.target.closest("[data-menu-index]");
    if (button?.classList.contains("is-current")) {
      pendingMenuOpenIndex = Number(button.dataset.menuIndex);
    }
    event.preventDefault();
    event.stopPropagation();
    return;
  }
  menuDrag = { pointerId: event.pointerId, startY: event.clientY, deltaY: 0, moved: false };
  refs.menuWheel.classList.add("is-dragging");
  refs.menuWheel.setPointerCapture?.(event.pointerId);
  event.stopPropagation();
}

function updateMenuDrag(event) {
  if (!menuDrag || event.pointerId !== menuDrag.pointerId) return;
  const rawDelta = event.clientY - menuDrag.startY;
  const deltaY = Math.max(-MENU_DRAG_RANGE, Math.min(MENU_DRAG_RANGE, rawDelta));
  menuDrag.deltaY = deltaY;
  menuDrag.moved = menuDrag.moved || Math.abs(rawDelta) > 7;
  const direction = deltaY <= 0 ? 1 : -1;
  renderMenuWheel({ direction, progress: Math.abs(deltaY) / MENU_DRAG_RANGE });
  event.preventDefault();
  event.stopPropagation();
}

function finishMenuDrag(event) {
  if (!menuDrag || event.pointerId !== menuDrag.pointerId) return;
  const { deltaY, moved } = menuDrag;
  const direction = deltaY <= 0 ? 1 : -1;
  const progress = Math.abs(deltaY) / MENU_DRAG_RANGE;
  const commit = Math.abs(deltaY) >= MENU_DRAG_COMMIT;
  menuDrag = null;
  refs.menuWheel.classList.remove("is-dragging");
  suppressMenuClick = moved;
  if (moved) window.setTimeout(() => { suppressMenuClick = false; }, 80);
  if (progress > 0) animateMenu(direction, progress, commit ? 1 : 0, commit);
  event.stopPropagation();
}

function handleMenuItemClick(event) {
  const button = event.target.closest("[data-menu-index]");
  if (!button || suppressMenuClick) return;
  const index = Number(button.dataset.menuIndex);
  if (menuWheelLocked || refs.menuWheel.classList.contains("is-settling")) {
    if (button.classList.contains("is-current")) pendingMenuOpenIndex = index;
    return;
  }
  if (index === currentMenu) openDetail();
}

function renderDetail() {
  const item = menuItems[currentMenu];
  refs.detailTitle.textContent = item.label;
  refs.detailStatus.textContent = item.status;

  if (item.id === "device") {
    refs.detailContent.className = "detail-content detail-content-device";
    refs.detailContent.innerHTML = `
      <div class="device-overview">
        <div class="device-avatar">
          ${iconMarkup("settings-2")}
          <span class="device-online-dot" aria-label="设备在线"></span>
        </div>
        <div class="device-identity">
          <strong>我的设备</strong>
          <span>设备 ID · A1C3E7</span>
        </div>
        <div class="device-meta" aria-label="设备状态">
          <span><i></i> 在线</span>
          <span>固件 7.0</span>
        </div>
      </div>
      <div class="device-actions" aria-label="设备功能">
        <button class="device-action" type="button" data-device-action="meeting">
          <span class="device-action-icon">${iconMarkup("video")}</span>
          <span class="device-action-copy"><strong>见面</strong><small>开启面对面交流</small></span>
          <span class="device-action-state" id="meetingState">已关闭</span>
        </button>
        <button class="device-action" type="button" data-device-action="qr">
          <span class="device-action-icon">${iconMarkup("qr-code")}</span>
          <span class="device-action-copy"><strong>设备二维码</strong><small>扫码识别与绑定</small></span>
          <span class="device-action-next">${iconMarkup("chevron-right")}</span>
        </button>
        <button class="device-action" type="button" data-device-action="provision">
          <span class="device-action-icon">${iconMarkup("bluetooth")}</span>
          <span class="device-action-copy"><strong>BLE 配网</strong><small>连接新的 WiFi</small></span>
          <span class="device-action-next">${iconMarkup("chevron-right")}</span>
        </button>
      </div>`;
  } else {
    refs.detailContent.className = "detail-content";
    const detailData = {
      claw: { title: "机械爪待命", copy: "当前处于安全收拢位置", facts: [["位置", "收拢"], ["驱动", "正常"]] },
      work: { title: "暂无进行中的任务", copy: "等待新的工作指令", facts: [["当前任务", "无"], ["模式", "待命"]] },
      issues: { title: "设备运行正常", copy: "未发现需要处理的问题", facts: [["严重问题", "0"], ["一般问题", "0"]] },
      wifi: {
        title: refs.wifiState.value === "connected" ? "已连接 xspaceagi" : stateLabels.wifi[refs.wifiState.value][0],
        copy: refs.wifiState.value === "connected" ? "网络连接稳定" : "请检查当前网络状态",
        facts: [["频段", "2.4 GHz"], ["信号", refs.wifiState.value === "connected" ? "良好" : "--"]]
      },
      volume: { title: `音量 ${refs.volumeRange.value}%`, copy: "扬声器输出", facts: [["输出", "扬声器"], ["模式", "标准"]] }
    };
    const data = detailData[item.id];
    refs.detailContent.innerHTML = `
      <div class="app-detail" style="--app-accent:${item.accent};--app-soft:${item.soft}">
        <div class="app-detail-emblem">
          <span></span>
          ${iconMarkup(item.icon)}
        </div>
        <strong class="app-detail-title">${data.title}</strong>
        <p class="app-detail-copy">${data.copy}</p>
        ${item.id === "volume" ? `<div class="generic-meter"><span style="width:${refs.volumeRange.value}%"></span></div>` : ""}
        <div class="app-detail-facts">
          ${data.facts.map(([label, value]) => `<span><small>${label}</small><b>${value}</b></span>`).join("")}
        </div>
      </div>`;
  }

  refs.detailContent.querySelectorAll("[data-device-action]").forEach((button) => {
    button.addEventListener("click", handleDeviceAction);
  });
  refreshIcons();
}

function openDetail() {
  renderDetail();
  setView("detail");
}

function handleDeviceAction(event) {
  const action = event.currentTarget.dataset.deviceAction;
  if (action === "meeting") {
    const state = byId("meetingState");
    const active = state.textContent === "已开启";
    state.textContent = active ? "已关闭" : "已开启";
    event.currentTarget.classList.toggle("is-active", !active);
  }
  if (action === "qr") setView("qr");
  if (action === "provision") startProvisioning();
}

function startProvisioning() {
  provisionStep = 0;
  window.clearInterval(provisionTimer);
  setView("provisioning");
  updateProvisioning();
  provisionTimer = window.setInterval(() => {
    provisionStep = (provisionStep + 1) % 5;
    updateProvisioning();
    if (provisionStep === 4) window.clearInterval(provisionTimer);
  }, 1800);
}

function updateProvisioning() {
  const steps = [
    ["等待连接", "NUWAX-A1C3E7"],
    ["安全连接已建立", "Security 2 · SRP6a"],
    ["正在接收 WiFi 信息", "连接信息已加密"],
    ["正在连接", "WiFi 与云端服务"],
    ["配网完成", "设备已在线"]
  ];
  const [title, copy] = steps[provisionStep];
  refs.provisionTitle.textContent = title;
  refs.provisionCopy.textContent = copy;
  document.querySelectorAll(".provision-steps span").forEach((dot, index) => {
    dot.classList.toggle("is-complete", index < provisionStep);
    dot.classList.toggle("is-current", index === Math.min(provisionStep, 3));
  });
}

function replayBoot() {
  window.clearTimeout(bootTimer);
  closeDrawer(false);
  setView("face", { keepDrawer: true });
  refs.deviceScreen.classList.remove("is-booting");
  void refs.deviceScreen.offsetWidth;
  refs.deviceScreen.classList.add("is-booting");
  refs.currentScreenName.textContent = "系统 / 启动中";
  bootTimer = window.setTimeout(() => {
    refs.deviceScreen.classList.remove("is-booting");
    applyEmotion("neutral");
    setView("face");
  }, window.matchMedia("(prefers-reduced-motion: reduce)").matches ? 120 : 4250);
}

function setStateVisual(element, stateClass) {
  element.classList.remove("is-online", "is-warning", "is-error", "is-working", "is-muted");
  if (stateClass) element.classList.add(`is-${stateClass}`);
}

function syncSystemState() {
  const wifi = stateLabels.wifi[refs.wifiState.value];
  const cloud = stateLabels.cloud[refs.cloudState.value];
  const dialog = stateLabels.dialog[refs.dialogState.value];
  const power = stateLabels.power[refs.powerState.value];

  refs.wifiValue.textContent = wifi[0];
  refs.cloudValue.textContent = cloud[0];
  refs.powerValue.textContent = power[0];
  refs.dialogLabel.textContent = dialog[0];

  setStateVisual(refs.wifiCell, wifi[1]);
  setStateVisual(refs.cloudCell, cloud[1]);
  setStateVisual(refs.powerCell, power[1]);
  setStateVisual(refs.topWifi, wifi[1]);
  setStateVisual(refs.topCloud, cloud[1]);
  setStateVisual(refs.topPower, power[1]);

  refs.dialogMark.className = `mode-mark${dialog[1] ? ` is-${dialog[1]}` : ""}`;
  refs.dialogStatusIcon.dataset.state = refs.dialogState.value;
  refs.dialogStatusIcon.innerHTML = `<i data-lucide="${dialogIcons[refs.dialogState.value]}"></i>`;
  refs.screenMenuTrigger.setAttribute("aria-label", `${dialog[0]}，打开菜单`);
  document.querySelectorAll(".status-icon-option").forEach((button) => {
    button.classList.toggle("is-selected", button.dataset.dialogState === refs.dialogState.value);
  });
  refreshIcons();
  refs.drawerFooter.textContent = wifi[1] === "error" || cloud[1] === "error"
    ? "连接异常，请检查"
    : (wifi[1] === "working" || cloud[1] === "working" ? "正在建立连接" : "所有系统可用");

  const dialogEmotion = {
    sleep: "neutral",
    listening: "listening",
    recognizing: "analyzing",
    thinking: "thinking",
    speaking: "speaking",
    meeting: "focused",
    monitoring: "analyzing",
    error: "lost"
  };
  if (dialogEmotion[refs.dialogState.value]) {
    applyEmotion(dialogEmotion[refs.dialogState.value]);
    setView("face", { keepDrawer: refs.statusDrawer.classList.contains("is-open") });
  }
}

function setVolume(value) {
  const normalized = Math.min(100, Math.max(0, Number(value)));
  refs.volumeRange.value = normalized;
  refs.screenVolume.value = normalized;
  refs.volumeValue.textContent = `${normalized}%`;
  refs.screenVolumeValue.textContent = normalized;
  menuItems.find((item) => item.id === "volume").status = `${normalized}%`;
  if (currentView === "detail" && menuItems[currentMenu].id === "volume") renderDetail();
}

function drawDeviceQr() {
  const canvas = refs.qrCanvas;
  const ctx = canvas.getContext("2d");
  const size = 29;
  const cell = canvas.width / size;
  const seedText = "NUWAX-A1C3E7|ble|security2|pop";
  let seed = 2166136261;
  for (const character of seedText) {
    seed ^= character.charCodeAt(0);
    seed = Math.imul(seed, 16777619);
  }

  const matrix = Array.from({ length: size }, () => Array(size).fill(false));
  const reserved = Array.from({ length: size }, () => Array(size).fill(false));

  function finder(startX, startY) {
    for (let y = -1; y <= 7; y += 1) {
      for (let x = -1; x <= 7; x += 1) {
        const xx = startX + x;
        const yy = startY + y;
        if (xx < 0 || yy < 0 || xx >= size || yy >= size) continue;
        reserved[yy][xx] = true;
        matrix[yy][xx] = x >= 0 && x <= 6 && y >= 0 && y <= 6 &&
          (x === 0 || x === 6 || y === 0 || y === 6 || (x >= 2 && x <= 4 && y >= 2 && y <= 4));
      }
    }
  }

  finder(0, 0);
  finder(size - 7, 0);
  finder(0, size - 7);

  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      if (reserved[y][x]) continue;
      seed ^= seed << 13;
      seed ^= seed >>> 17;
      seed ^= seed << 5;
      matrix[y][x] = ((seed >>> 0) & 3) < 2;
    }
  }

  ctx.fillStyle = "#f4f7f2";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = "#101514";
  matrix.forEach((row, y) => row.forEach((active, x) => {
    if (active) ctx.fillRect(Math.floor(x * cell), Math.floor(y * cell), Math.ceil(cell), Math.ceil(cell));
  }));
}

function tickClock() {
  const now = new Date();
  refs.drawerClock.textContent = `${String(now.getHours()).padStart(2, "0")}:${String(now.getMinutes()).padStart(2, "0")}`;
}

function handleGestureStart(event) {
  if (event.target.closest(".menu-wheel")) return;
  const point = event.touches ? event.touches[0] : event;
  touchStart = { x: point.clientX, y: point.clientY };
}

function handleGestureEnd(event) {
  if (event.target.closest(".menu-wheel")) return;
  if (!touchStart) return;
  const point = event.changedTouches ? event.changedTouches[0] : event;
  const dx = point.clientX - touchStart.x;
  const dy = point.clientY - touchStart.y;
  touchStart = null;
  if (Math.max(Math.abs(dx), Math.abs(dy)) < 42) return;

  if (Math.abs(dy) > Math.abs(dx)) {
    if (dy > 0) openDrawer();
    else closeDrawer();
    return;
  }
  if (refs.statusDrawer.classList.contains("is-open")) return;
  if (currentView === "face" && dx < 0) setView("menu");
  else if (currentView === "menu" && dx > 0) setView("face");
  else if (dx > 0 && ["menu", "detail", "qr", "provisioning"].includes(currentView)) {
    setView(currentView === "menu" ? "face" : (currentView === "detail" ? "menu" : "detail"));
  }
}

function bindEvents() {
  byId("replayBootTop").addEventListener("click", replayBoot);
  byId("replayBoot").addEventListener("click", replayBoot);
  byId("screenMenuTrigger").addEventListener("click", () => setView("menu"));
  byId("menuBack").addEventListener("click", () => setView("face"));
  refs.menuWheel.addEventListener("pointerdown", beginMenuDrag);
  refs.menuWheel.addEventListener("pointermove", updateMenuDrag);
  refs.menuWheel.addEventListener("pointerup", finishMenuDrag);
  refs.menuWheel.addEventListener("pointercancel", finishMenuDrag);
  refs.menuWheel.addEventListener("click", handleMenuItemClick);
  byId("detailBack").addEventListener("click", () => setView("menu"));
  byId("qrBack").addEventListener("click", () => setView("detail"));
  byId("provisionBack").addEventListener("click", () => setView("detail"));
  byId("drawerClose").addEventListener("click", () => closeDrawer());

  refs.emotionSelect.addEventListener("change", (event) => {
    applyEmotion(event.target.value);
    setView("face");
  });
  byId("prevEmotion").addEventListener("click", () => { applyEmotion(currentEmotion - 1); setView("face"); });
  byId("nextEmotion").addEventListener("click", () => { applyEmotion(currentEmotion + 1); setView("face"); });

  document.querySelectorAll("#viewControls button").forEach((button) => {
    button.addEventListener("click", () => {
      if (button.dataset.view === "drawer") {
        openDrawer();
      } else if (button.dataset.view === "device") {
        currentMenu = menuItems.findIndex((item) => item.id === "device");
        updateMenu();
        renderDetail();
        setView("detail");
      } else {
        setView(button.dataset.view);
      }
    });
  });

  [refs.wifiState, refs.cloudState, refs.dialogState, refs.powerState].forEach((select) => {
    select.addEventListener("change", syncSystemState);
  });
  document.querySelectorAll(".status-icon-option").forEach((button) => {
    button.addEventListener("click", () => {
      refs.dialogState.value = button.dataset.dialogState;
      syncSystemState();
    });
  });
  refs.volumeRange.addEventListener("input", (event) => setVolume(event.target.value));
  refs.screenVolume.addEventListener("input", (event) => setVolume(event.target.value));

  refs.deviceScreen.addEventListener("pointerdown", handleGestureStart);
  refs.deviceScreen.addEventListener("pointerup", handleGestureEnd);
  refs.deviceScreen.addEventListener("touchstart", handleGestureStart, { passive: true });
  refs.deviceScreen.addEventListener("touchend", handleGestureEnd, { passive: true });
  refs.deviceScreen.addEventListener("wheel", handleMenuWheel, { passive: false });
}

function init() {
  cacheRefs();
  refreshIcons();
  drawDeviceQr();
  updateMenu();
  applyEmotion(0);
  syncSystemState();
  setVolume(68);
  tickClock();
  bindEvents();
  window.setInterval(tickClock, 30000);
  replayBoot();
}

document.addEventListener("DOMContentLoaded", init);
