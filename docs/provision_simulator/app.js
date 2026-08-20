'use strict';

const CONTRACT_URL = '/api/contract';
const DEFAULT_USERNAME = 'NX-A1B2-C3D4-E5F6';
const QR_SCAN_INTERVAL_MS = 160;
const WIFI_AUTO_START_DELAY_MS = 1200;
const WIFI_AUTO_REFRESH_DELAY_MS = 15000;
const WIFI_COUNTDOWN_TICK_MS = 1000;
const WIFI_SCAN_HANDOFF_TIMEOUT_MS = 60000;

const FLOW = [
  ['scan', '扫描设备', 'UUID + PROV_'],
  ['connect', 'BLE 连接', 'GATT connected'],
  ['proto', '协议发现', 'proto-ver'],
  ['security', 'Security 2', 'prov-session'],
  ['device', '读取设备', 'device-info'],
  ['vox-config', '下发云配置', 'URL + deviceSecret'],
  ['config', '发送凭据', 'prov-config'],
  ['apply', '应用配置', 'prov-config apply'],
  ['status', '等待 Wi-Fi', 'WIFI_READY'],
  ['cloud', '观察云端', '独立于 Wi-Fi 结果'],
];

const SCENARIOS = {
  success: { outcome: 'SUCCESS', failAt: null },
  security_auth_failed: { outcome: 'SECURITY_AUTH_FAILED', failAt: 'security' },
  wifi_auth_failed: { outcome: 'WIFI_AUTH_FAILED', failAt: 'status' },
  network_not_found: { outcome: 'NETWORK_NOT_FOUND', failAt: 'status' },
  timeout: { outcome: 'TIMEOUT', failAt: 'status', virtualDelay: 30000 },
  internal_error: { outcome: 'INTERNAL_ERROR', failAt: 'apply' },
  disconnect_reconnect: { outcome: 'SUCCESS', failAt: null, disconnectAt: 'device' },
  window_expired: { outcome: 'TIMEOUT', failAt: 'status', virtualDelay: 300000, expires: true },
};

const EVENT_TEXT = {
  scan: ['APP', '扫描 Service UUID 与广播名前缀', '发现 PROV_A1B2C3'],
  connect: ['BLE', '连接外围设备并发现 GATT 服务', 'connected'],
  security: ['protocomm', '使用 username + PoP 建立 Security 2', 'session established'],
  proto: ['proto-ver', '读取 sec_ver 与 capabilities', 'sec_ver=2, wifi_prov'],
  device: ['device-info', '读取稳定设备身份与诊断状态', DEFAULT_USERNAME],
  'vox-config': ['vox-config', '下发动态 URL 与设备密钥', 'active configuration stored'],
  config: ['prov-config', '加密发送 SSID/passphrase', 'credentials accepted'],
  apply: ['prov-config', '请求设备应用 Wi-Fi 配置', 'connecting'],
  status: ['prov-config', '查询官方配网状态', 'IP_EVENT_STA_GOT_IP'],
  cloud: ['firmware / Cloud Gateway', '使用 X-Bridge-Token 连接云端', 'DEVICE_ONLINE（附加状态）'],
};

const app = {
  contract: null,
  running: false,
  provisionPending: false,
  cancelled: false,
  virtualMs: 0,
  startedAt: 0,
  events: [],
  bridgeAvailable: false,
  testGatewayConfigured: false,
  testGatewayWsUrl: '',
  reprovisionControlAvailable: false,
  usbSerialControlAvailable: false,
  reprovisionRequestRunning: false,
  reprovisionConfirmUntil: 0,
  realJobId: null,
  realEventCursor: 0,
  wifiScanRunning: false,
  wifiNetworks: [],
  wifiHasScanned: false,
  wifiAutoRefreshStopped: false,
  wifiAutoRefreshTimer: null,
  wifiCountdownTimer: null,
  wifiNextScanAt: 0,
  wifiScanOutcome: '锁定 BLE 设备后可扫描',
  qrTarget: null,
  qrMatched: false,
  qrStream: null,
  qrFrameRequest: null,
  qrDetector: undefined,
  lastQrScanAt: 0,
  qrScanToken: 0,
  volumeControlState: 'TEST_GATEWAY_UNAVAILABLE',
  volumeControlBusy: false,
  volumeRefreshToken: 0,
  gatewayDeviceOnline: false,
  audioMode: null,
  audioModeFeatures: { meeting: false, monitoring: false },
  audioModeControlBusy: false,
};

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function buildFlow() {
  const list = $('flow');
  const template = $('flow-item-template');
  list.replaceChildren();
  FLOW.forEach(([id, title, detail]) => {
    const node = template.content.firstElementChild.cloneNode(true);
    node.dataset.step = id;
    node.querySelector('strong').textContent = title;
    node.querySelector('div span').textContent = detail;
    list.appendChild(node);
  });
}

function setFlowState(id, state) {
  const item = document.querySelector(`[data-step="${id}"]`);
  if (item) item.dataset.state = state;
}

function setStage(label, badge = 'badge-info') {
  $('current-stage').textContent = label;
  $('connection-badge').className = `badge ${badge}`;
}

function updateClock() {
  $('elapsed').textContent = `${(app.virtualMs / 1000).toFixed(1)} s`;
  if (!app.contract) return;
  const total = app.contract.transport.advertisingTimeoutSeconds * 1000;
  $('window-left').textContent = `${Math.max(0, Math.ceil((total - app.virtualMs) / 1000))} s`;
}

function logEvent(source, event, result, level = '') {
  const entry = { timeMs: app.virtualMs, source, event, result, level };
  app.events.push(entry);
  const row = document.createElement('tr');
  if (level) row.className = `log-${level}`;
  [ `${(entry.timeMs / 1000).toFixed(1)} s`, source, event, result ].forEach((value) => {
    const cell = document.createElement('td');
    cell.textContent = value;
    row.appendChild(cell);
  });
  $('event-log').appendChild(row);
  row.scrollIntoView({ block: 'nearest' });
}

async function advance(ms) {
  const speed = Number($('speed').value);
  await sleep(Math.max(120, Math.min(900, ms / speed)));
  if (app.cancelled) throw new Error('cancelled');
  app.virtualMs += ms;
  updateClock();
}

function setResult(value, level) {
  $('final-result').textContent = value;
  $('final-result').className = level === 'success' ? 'result-success' : 'result-error';
}

function realInputsReady() {
  if (!app.bridgeAvailable || !$('real-device').value) return false;
  const values = [
    $('username').value.trim(),
    $('pop').value,
    $('gateway-url').value.trim(),
    $('device-secret').value,
    $('ssid').value.trim(),
    $('wifi-password').value,
  ];
  if (values.some((value) => !value)) return false;
  if (!app.qrTarget) return true;
  return app.qrMatched
    && $('real-device').value === app.qrTarget.name
    && $('username').value.trim() === app.qrTarget.username
    && (!app.qrTarget.pop || $('pop').value === app.qrTarget.pop);
}

function updateRunAvailability() {
  const realMode = $('mode').value === 'real';
  $('run').disabled = app.running || app.provisionPending || !app.contract
    || (realMode && !realInputsReady());
  updateDeviceScanAvailability();
  updateRemoteReprovisionAvailability();
  updateWifiScanAvailability();
  syncWifiAutoRefresh();
  updateVolumeControlAvailability();
}

function websocketTargetKey(value) {
  try {
    const parsed = new URL(value);
    if (!['ws:', 'wss:'].includes(parsed.protocol)
        || parsed.username || parsed.password || parsed.hash) return '';
    const port = parsed.port || (parsed.protocol === 'wss:' ? '443' : '80');
    return [parsed.protocol, parsed.hostname.toLowerCase(), port, parsed.pathname || '/', parsed.search].join('|');
  } catch (_) {
    return '';
  }
}

function targetsConfiguredTestGateway() {
  const configured = websocketTargetKey(app.testGatewayWsUrl);
  return Boolean(configured) && websocketTargetKey($('gateway-url').value.trim()) === configured;
}

const VOLUME_STATE_TEXT = {
  READY: ['测试服务可用', '设备在线，可通过测试服务下发音量', 'badge-success'],
  MOCK_MODE: ['真实设备模式未启用', '切换至真实设备模式后检查测试服务', 'badge-warning'],
  TEST_GATEWAY_UNAVAILABLE: ['测试服务不可用', '请先启动包含测试网关的本地联调栈', 'badge-danger'],
  FORMAL_CLOUD_CONTROL_DISABLED: ['正式服务不可控制', '正式服务 URL 下模拟器音量控制已禁用', 'badge-warning'],
  DEVICE_ID_REQUIRED: ['等待设备身份', '扫码或填写 Username 后检查设备状态', 'badge-warning'],
  DEVICE_OFFLINE: ['设备未在线', '目标设备尚未连接当前测试服务', 'badge-warning'],
  AUDIO_MODE_BUSY: ['音频模式占用', '会议或监控结束后可下发音量', 'badge-warning'],
  CHECKING: ['正在检查', '正在读取测试服务设备状态', 'badge-info'],
  VOLUME_ACK_TIMEOUT: ['设备未确认', '音量命令 ACK 超时，请刷新设备状态', 'badge-danger'],
  VOLUME_REJECTED: ['设备拒绝命令', '设备未接受本次音量设置', 'badge-danger'],
  INVALID_REQUEST: ['参数无效', '请检查设备身份与音量参数', 'badge-danger'],
  INTERNAL_ERROR: ['控制失败', '测试服务音量控制调用失败', 'badge-danger'],
};

function setVolumeControlState(state, detail = '') {
  app.volumeControlState = state;
  const [label, defaultDetail, badgeClass] = VOLUME_STATE_TEXT[state] || VOLUME_STATE_TEXT.INTERNAL_ERROR;
  $('volume-control-badge').textContent = label;
  $('volume-control-badge').className = `badge ${badgeClass}`;
  $('volume-control-status').textContent = detail || defaultDetail;
  $('volume-control-status').className = `qr-status ${badgeClass === 'badge-danger' ? 'error-text' : badgeClass === 'badge-success' ? 'success-text' : ''}`.trim();
  updateVolumeControlAvailability();
}

function updateVolumeControlAvailability() {
  const realMode = $('mode').value === 'real';
  const hasIdentity = Boolean($('username').value.trim());
  const testTarget = targetsConfiguredTestGateway();
  const canCheck = realMode && app.bridgeAvailable && app.testGatewayConfigured && testTarget && hasIdentity;
  const canSend = canCheck && app.volumeControlState === 'READY'
    && !app.volumeControlBusy && !app.audioModeControlBusy;
  $('volume-range').disabled = !canSend;
  $('volume-persist').disabled = !canSend;
  $('send-volume').disabled = !canSend;
  $('refresh-volume-status').disabled = !canCheck || app.volumeControlBusy || app.audioModeControlBusy;
  updateAudioModeControlAvailability();
}

function audioControlContextReady() {
  return $('mode').value === 'real'
    && app.bridgeAvailable
    && app.testGatewayConfigured
    && targetsConfiguredTestGateway()
    && Boolean($('username').value.trim());
}

function audioModeDefaultState(mode) {
  if (!audioControlContextReady()) {
    if ($('mode').value !== 'real') return '请先切换至真实设备模式';
    if (!targetsConfiguredTestGateway()) return '正式服务 URL 下不可用';
    return '等待本地测试服务与设备';
  }
  if (!app.gatewayDeviceOnline) return '设备未连接测试服务';
  if (!app.audioModeFeatures[mode]) return '设备未声明支持此模式';
  if (app.audioMode === mode) return '已开启';
  if (app.audioMode) return `${app.audioMode === 'meeting' ? '会议' : '监控'}模式运行中`;
  return '已关闭';
}

function updateAudioModeControlAvailability() {
  const canControl = audioControlContextReady() && app.gatewayDeviceOnline
    && !app.volumeControlBusy && !app.audioModeControlBusy;
  for (const mode of ['meeting', 'monitoring']) {
    const toggle = $(`${mode}-mode-toggle`);
    toggle.checked = app.audioMode === mode;
    toggle.disabled = !canControl || !app.audioModeFeatures[mode]
      || Boolean(app.audioMode && app.audioMode !== mode);
    $(`${mode}-mode-state`).textContent = audioModeDefaultState(mode);
  }
}

const AUDIO_MODE_ERROR_TEXT = {
  FORMAL_CLOUD_CONTROL_DISABLED: '正式服务 URL 下不能使用模拟器控制',
  TEST_GATEWAY_UNAVAILABLE: '本地测试服务不可用',
  DEVICE_OFFLINE: '设备未连接当前测试服务',
  AUDIO_MODE_UNSUPPORTED: '设备未声明支持此模式',
  AUDIO_MODE_BUSY: '请先关闭当前运行的音频模式',
  AUDIO_MODE_ACK_TIMEOUT: '设备确认超时，请刷新状态后重试',
  AUDIO_MODE_REJECTED: '设备拒绝了本次模式切换',
  INVALID_REQUEST: '模式控制参数无效',
  INTERNAL_ERROR: '模式控制调用失败',
};

function setAudioModeFeedback(text, level = '') {
  const status = $('audio-mode-control-status');
  status.textContent = text;
  status.className = `qr-status ${level === 'error' ? 'error-text' : level === 'success' ? 'success-text' : ''}`.trim();
}

function clearAudioModeContext() {
  app.gatewayDeviceOnline = false;
  app.audioMode = null;
  app.audioModeFeatures = { meeting: false, monitoring: false };
  setAudioModeFeedback('仅本地测试服务可用');
  updateAudioModeControlAvailability();
}

function applyImmediateVolumeContext() {
  if ($('mode').value !== 'real') {
    clearAudioModeContext();
    setVolumeControlState('MOCK_MODE');
  } else if (!app.bridgeAvailable || !app.testGatewayConfigured || !app.testGatewayWsUrl) {
    clearAudioModeContext();
    setVolumeControlState('TEST_GATEWAY_UNAVAILABLE');
  } else if (!targetsConfiguredTestGateway()) {
    clearAudioModeContext();
    setVolumeControlState('FORMAL_CLOUD_CONTROL_DISABLED');
  } else if (!$('username').value.trim()) {
    clearAudioModeContext();
    setVolumeControlState('DEVICE_ID_REQUIRED');
  } else {
    return true;
  }
  return false;
}

async function refreshVolumeControlStatus({ quiet = false } = {}) {
  const token = ++app.volumeRefreshToken;
  if (!applyImmediateVolumeContext() || app.volumeControlBusy || app.audioModeControlBusy) return;
  if (!quiet) setVolumeControlState('CHECKING');
  try {
    const response = await fetch('/api/test-gateway/control-status', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        deviceId: $('username').value.trim(),
        gatewayUrl: $('gateway-url').value.trim(),
      }),
    });
    const result = await response.json();
    if (token !== app.volumeRefreshToken) return;
    if (!response.ok || result.ok !== true) {
      clearAudioModeContext();
      const state = result.code || 'INTERNAL_ERROR';
      setVolumeControlState(state, VOLUME_STATE_TEXT[state] ? '' : (result.error || '测试服务状态查询失败'));
      return;
    }
    app.gatewayDeviceOnline = result.reason !== 'DEVICE_OFFLINE';
    app.audioMode = ['meeting', 'monitoring'].includes(result.audioMode) ? result.audioMode : null;
    app.audioModeFeatures = {
      meeting: result.features?.meeting === true,
      monitoring: result.features?.monitoring === true,
    };
    if (result.available) {
      setVolumeControlState('READY');
    } else {
      const modeText = result.audioMode === 'meeting' ? '会议模式' : '监控模式';
      setVolumeControlState(
        result.reason || 'DEVICE_OFFLINE',
        result.reason === 'AUDIO_MODE_BUSY' ? `${modeText}正在使用音频通道` : '',
      );
    }
    setAudioModeFeedback(app.gatewayDeviceOnline ? '状态已与设备同步' : '设备未连接当前测试服务');
    updateAudioModeControlAvailability();
  } catch (_) {
    if (token === app.volumeRefreshToken) {
      clearAudioModeContext();
      setVolumeControlState('TEST_GATEWAY_UNAVAILABLE');
    }
  }
}

async function sendTestGatewayAudioMode(mode, enabled) {
  if (!['meeting', 'monitoring'].includes(mode) || app.audioModeControlBusy) return;
  if (!audioControlContextReady() || !app.gatewayDeviceOnline || !app.audioModeFeatures[mode]) {
    updateAudioModeControlAvailability();
    return;
  }
  if (app.audioMode && app.audioMode !== mode) {
    setAudioModeFeedback('请先关闭当前运行的音频模式', 'error');
    updateAudioModeControlAvailability();
    return;
  }

  app.audioModeControlBusy = true;
  updateVolumeControlAvailability();
  setAudioModeFeedback(`正在${enabled ? '开启' : '关闭'}${mode === 'meeting' ? '会议' : '监控'}模式，等待设备确认`);
  let feedback = '';
  let feedbackLevel = '';
  try {
    const response = await fetch('/api/test-gateway/audio-mode', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        deviceId: $('username').value.trim(),
        gatewayUrl: $('gateway-url').value.trim(),
        mode,
        enabled,
      }),
    });
    const result = await response.json();
    if (!response.ok || result.ok !== true) {
      const code = result.code || 'INTERNAL_ERROR';
      feedback = AUDIO_MODE_ERROR_TEXT[code] || '模式控制调用失败';
      feedbackLevel = 'error';
    } else {
      app.audioMode = result.audioMode || null;
      feedback = `设备已确认${enabled ? '开启' : '关闭'}${mode === 'meeting' ? '会议' : '监控'}模式`;
      feedbackLevel = 'success';
      logEvent('测试服务', `${enabled ? '开启' : '关闭'}${mode === 'meeting' ? '会议' : '监控'}模式`, `${result.ack?.message || 'ACK'}`, 'success');
    }
  } catch (_) {
    feedback = AUDIO_MODE_ERROR_TEXT.TEST_GATEWAY_UNAVAILABLE;
    feedbackLevel = 'error';
  } finally {
    app.audioModeControlBusy = false;
    await refreshVolumeControlStatus({ quiet: true });
    setAudioModeFeedback(feedback || '状态已与设备同步', feedbackLevel);
    updateVolumeControlAvailability();
  }
}

async function sendTestGatewayVolume() {
  if (app.volumeControlBusy || app.volumeControlState !== 'READY') return;
  app.volumeControlBusy = true;
  updateVolumeControlAvailability();
  $('volume-control-status').textContent = '正在等待设备确认音量命令';
  try {
    const volumePercent = Number($('volume-range').value);
    const response = await fetch('/api/test-gateway/volume', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        deviceId: $('username').value.trim(),
        gatewayUrl: $('gateway-url').value.trim(),
        volumePercent,
        persist: $('volume-persist').checked,
      }),
    });
    const result = await response.json();
    if (!response.ok || result.ok !== true) {
      const state = result.code || 'INTERNAL_ERROR';
      setVolumeControlState(state, VOLUME_STATE_TEXT[state] ? '' : (result.error || '音量命令下发失败'));
      return;
    }
    setVolumeControlState(
      'READY',
      `设备已确认 ${result.volumePercent}%${result.persist ? '，已保存默认值' : ''}`,
    );
    logEvent('测试服务', '下发设备音量', `${result.volumePercent}% · ACK`, 'success');
  } catch (_) {
    setVolumeControlState('TEST_GATEWAY_UNAVAILABLE');
  } finally {
    app.volumeControlBusy = false;
    updateVolumeControlAvailability();
  }
}

function updateRemoteReprovisionAvailability() {
  const button = $('remote-reprovision');
  if (!button) return;
  button.disabled = app.running || app.provisionPending || app.wifiScanRunning
    || app.reprovisionRequestRunning || $('mode').value !== 'real'
    || !app.bridgeAvailable || !app.reprovisionControlAvailable
    || !$('username').value.trim()
    || !(app.qrTarget?.name || $('real-device').value);
}

function updateDeviceScanAvailability() {
  const button = $('scan-devices');
  if (!button) return;
  button.disabled = app.running || app.provisionPending || app.wifiScanRunning
    || $('mode').value !== 'real';
}

function wifiScanInputsReady() {
  if (!app.bridgeAvailable || !$('real-device').value) return false;
  if (!$('username').value.trim() || !$('pop').value) return false;
  if (!app.qrTarget) return true;
  return app.qrMatched
    && $('real-device').value === app.qrTarget.name
    && $('username').value.trim() === app.qrTarget.username
    && (!app.qrTarget.pop || $('pop').value === app.qrTarget.pop);
}

function updateWifiScanAvailability() {
  const button = $('scan-wifi');
  if (!button) return;
  button.disabled = app.running || app.provisionPending || app.wifiScanRunning
    || $('mode').value !== 'real' || !wifiScanInputsReady();
}

function wifiAutoRefreshEligible() {
  return !app.running && !app.provisionPending && !app.wifiScanRunning
    && !app.wifiAutoRefreshStopped
    && $('mode').value === 'real' && wifiScanInputsReady();
}

function clearWifiAutoRefreshTimers() {
  if (app.wifiAutoRefreshTimer !== null) clearTimeout(app.wifiAutoRefreshTimer);
  if (app.wifiCountdownTimer !== null) clearTimeout(app.wifiCountdownTimer);
  app.wifiAutoRefreshTimer = null;
  app.wifiCountdownTimer = null;
  app.wifiNextScanAt = 0;
}

function renderWifiScanStatus() {
  const status = $('wifi-scan-status');
  if (app.provisionPending) {
    status.textContent = app.wifiScanRunning
      ? '已请求配网 · 正在完成当前扫描并释放 BLE'
      : '设备扫描已停止 · 正在开始配网';
    return;
  }
  if (app.wifiScanRunning) {
    status.textContent = app.wifiNetworks.length
      ? `设备扫描中 · 当前列表 ${app.wifiNetworks.length} 个网络`
      : '设备扫描中 · 正在获取周围 2.4 GHz Wi-Fi';
    return;
  }
  if (app.wifiNextScanAt > 0 && wifiAutoRefreshEligible()) {
    const seconds = Math.max(0, Math.ceil((app.wifiNextScanAt - Date.now()) / 1000));
    status.textContent = `${app.wifiScanOutcome} · 自动刷新 · 下次刷新 ${seconds} 秒`;
    return;
  }
  status.textContent = app.wifiScanOutcome;
}

function updateWifiCountdown() {
  if (app.wifiNextScanAt <= 0 || !wifiAutoRefreshEligible()) return;
  renderWifiScanStatus();
  app.wifiCountdownTimer = setTimeout(updateWifiCountdown, WIFI_COUNTDOWN_TICK_MS);
}

function scheduleWifiAutoRefresh(delayMs = WIFI_AUTO_REFRESH_DELAY_MS) {
  clearWifiAutoRefreshTimers();
  if (!wifiAutoRefreshEligible()) {
    renderWifiScanStatus();
    return;
  }
  app.wifiNextScanAt = Date.now() + Math.max(0, delayMs);
  app.wifiAutoRefreshTimer = setTimeout(() => {
    app.wifiAutoRefreshTimer = null;
    app.wifiNextScanAt = 0;
    if (app.wifiCountdownTimer !== null) clearTimeout(app.wifiCountdownTimer);
    app.wifiCountdownTimer = null;
    if (wifiAutoRefreshEligible()) scanDeviceWifi({ automatic: true });
    else renderWifiScanStatus();
  }, Math.max(0, delayMs));
  updateWifiCountdown();
}

function syncWifiAutoRefresh() {
  if (!wifiAutoRefreshEligible()) {
    clearWifiAutoRefreshTimers();
    renderWifiScanStatus();
    return;
  }
  if (app.wifiAutoRefreshTimer !== null) {
    renderWifiScanStatus();
    return;
  }
  scheduleWifiAutoRefresh(app.wifiHasScanned
    ? WIFI_AUTO_REFRESH_DELAY_MS : WIFI_AUTO_START_DELAY_MS);
}

function clearWifiNetworks(message = '尚未扫描') {
  clearWifiAutoRefreshTimers();
  app.wifiNetworks = [];
  app.wifiHasScanned = false;
  app.wifiAutoRefreshStopped = false;
  app.wifiScanOutcome = message;
  const list = $('wifi-network-list');
  list.replaceChildren();
  const empty = document.createElement('p');
  empty.className = 'wifi-network-empty';
  empty.textContent = message;
  list.appendChild(empty);
  renderWifiScanStatus();
}

function syncWifiNetworkSelection() {
  const selectedSsid = $('ssid').value;
  $('wifi-network-list').querySelectorAll('.wifi-network-row').forEach((button) => {
    button.setAttribute('aria-pressed', String(button.dataset.ssid === selectedSsid));
  });
}

function selectWifiNetwork(ssid) {
  $('ssid').value = ssid;
  $('wifi-password').value = '';
  $('ssid').dispatchEvent(new Event('input', { bubbles: true }));
  $('wifi-password').dispatchEvent(new Event('input', { bubbles: true }));
  app.wifiScanOutcome = `已选择 ${ssid}，请输入 Wi-Fi 密码`;
  renderWifiScanStatus();
  syncWifiNetworkSelection();
  $('wifi-password').focus({ preventScroll: true });
}

function renderWifiNetworks(networks) {
  app.wifiNetworks = networks;
  const list = $('wifi-network-list');
  list.replaceChildren();
  if (!networks.length) {
    const empty = document.createElement('p');
    empty.className = 'wifi-network-empty';
    empty.textContent = '设备未扫描到可用的 2.4 GHz Wi-Fi';
    list.appendChild(empty);
    return;
  }
  networks.forEach((network) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'wifi-network-row';
    button.dataset.ssid = network.ssid;
    button.setAttribute('aria-pressed', String(network.ssid === $('ssid').value));

    const name = document.createElement('span');
    name.className = 'wifi-network-name';
    name.textContent = network.ssid;
    const meta = document.createElement('span');
    meta.className = 'wifi-network-meta';
    meta.textContent = `${network.rssi} dBm · CH ${network.channel} · ${network.auth}`;
    button.append(name, meta);
    button.addEventListener('click', () => selectWifiNetwork(network.ssid));
    list.appendChild(button);
  });
}

async function scanDeviceWifi({ automatic = false } = {}) {
  if (!wifiScanInputsReady() || app.running || app.provisionPending || app.wifiScanRunning) return;
  if (!automatic) app.wifiAutoRefreshStopped = false;
  const request = {
    serviceName: $('real-device').value,
    username: $('username').value.trim(),
    pop: $('pop').value,
  };
  clearWifiAutoRefreshTimers();
  app.wifiScanRunning = true;
  app.wifiHasScanned = true;
  updateRunAvailability();
  $('scan-wifi').textContent = automatic ? '自动扫描中' : '设备扫描中';
  if (!app.wifiNetworks.length) {
    const list = $('wifi-network-list');
    list.replaceChildren();
    const empty = document.createElement('p');
    empty.className = 'wifi-network-empty';
    empty.textContent = '正在通过 BLE 请求设备扫描 2.4 GHz Wi-Fi';
    list.appendChild(empty);
  }
  renderWifiScanStatus();
  let requestBody = '';
  try {
    requestBody = JSON.stringify(request);
    request.pop = '';
    const response = await fetch('/api/wifi-scan', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: requestBody,
    });
    requestBody = '';
    const body = await response.json();
    if (!response.ok) throw new Error(body.result || body.error || `HTTP ${response.status}`);
    if (body.deviceInfo?.serialNumber !== request.username) {
      throw new Error('DEVICE_ID_MISMATCH');
    }
    if (body.protocolInfo?.prov?.cap?.includes('wifi_scan') !== true) {
      throw new Error('PROTOCOL_ERROR');
    }
    renderWifiNetworks(body.networks || []);
    app.wifiScanOutcome = body.networks?.length
      ? `已发现 ${body.networks.length} 个网络`
      : '未发现可用的 2.4 GHz Wi-Fi';
  } catch (error) {
    app.wifiScanOutcome = `扫描失败：${error.message}`;
    if (!app.wifiNetworks.length) {
      const list = $('wifi-network-list');
      list.replaceChildren();
      const empty = document.createElement('p');
      empty.className = 'wifi-network-empty';
      empty.textContent = '本次扫描失败，将自动重试';
      list.appendChild(empty);
    }
  } finally {
    requestBody = '';
    request.pop = '';
    app.wifiScanRunning = false;
    if ($('wifi-password').value) app.wifiAutoRefreshStopped = true;
    $('scan-wifi').textContent = '扫描设备 Wi-Fi';
    updateRunAvailability();
  }
}

function setAssertions(items) {
  const list = $('assertions');
  list.replaceChildren();
  items.forEach(({ label, pass }) => {
    const item = document.createElement('li');
    item.className = pass ? 'pass' : 'fail';
    const mark = document.createElement('span');
    mark.className = 'mark';
    mark.textContent = pass ? 'OK' : '!';
    const text = document.createElement('span');
    text.textContent = label;
    item.append(mark, text);
    list.appendChild(item);
  });
}

function renderContract() {
  const c = app.contract;
  const deliveryStatus = ({
    FIRMWARE_AND_TEST_PROVIDER_IMPLEMENTED: '首轮联调代码就绪',
    FIRMWARE_LOCAL_E2E_VERIFIED: '固件本地三端已验证',
    FIRMWARE_AND_SIMULATOR_IMPLEMENTED_VALIDATION_PENDING: '固件与模拟器已实现，真机验收待执行',
  })[c.status] || c.status;
  const rows = [
    ['交付状态', deliveryStatus],
    ['SDK / 组件', `${c.firmware.espIdfVersion} / ${c.firmware.componentVersion}`],
    ['BLE Host', c.firmware.bleHost],
    ['Service UUID', c.transport.primaryServiceUuid],
    ['广播名', `${c.transport.serviceNamePrefix}A1B2C3`],
    ['Security', `Security ${c.security.version}`],
    [
      'Capabilities',
      `prov: ${c.officialEndpoints.capabilityLocations['prov.cap'].join(', ')} · ` +
        `nuwax: ${c.officialEndpoints.capabilityLocations['nuwax.cap'].join(', ')}`,
    ],
    ['Wi-Fi 里程碑', c.successCriteria.firmwareEvent],
    ['配网成功', 'WIFI_READY（云端状态单独显示）'],
    ['凭据超时', `${c.successCriteria.statusTimeoutMs / 1000} s`],
    ['截止前尝试宽限', `${c.transport.activeAttemptGraceMs / 1000} s`],
    ['成功保留', `${c.lifecycle.postSuccessBleWindowMs / 1000} s`],
    ['SoftAP', c.releaseScope.softApSupported ? '支持' : '不支持'],
  ];
  const list = $('contract-summary');
  list.replaceChildren();
  rows.forEach(([term, value]) => {
    const wrap = document.createElement('div');
    const dt = document.createElement('dt');
    const dd = document.createElement('dd');
    dt.textContent = term;
    dd.textContent = value;
    wrap.append(dt, dd);
    list.appendChild(wrap);
  });
  if (!app.qrTarget) {
    $('pop').value = c.security.developmentPoP || '';
    $('username').value = DEFAULT_USERNAME;
  }
  $('contract-status').textContent = `契约 ${c.contractVersion}`;
  $('contract-status').className = 'badge badge-success';
  $('load-error').hidden = true;
  $('run').disabled = false;
  resetSimulation();
}

function validateContract(c) {
  const required = [
    c?.firmware?.espIdfVersion,
    c?.firmware?.componentVersion,
    c?.transport?.primaryServiceUuid,
    c?.security?.version,
    c?.officialEndpoints?.authoritativeStatusEndpoint,
    c?.successCriteria?.firmwareEvent,
    c?.lifecycle?.postSuccessBleWindowMs,
    c?.cloudProvisioning?.endpoint,
  ];
  if (required.some((value) => value === undefined || value === null || value === '')) {
    throw new Error('契约缺少模拟器所需字段');
  }
  if (c.security.version !== 2 || c.releaseScope.softApSupported !== false) {
    throw new Error('契约不是本期 Security 2 / BLE-only 基线');
  }
  return c;
}

async function loadContract() {
  try {
    const response = await fetch(CONTRACT_URL, { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    app.contract = validateContract(await response.json());
    renderContract();
  } catch (error) {
    $('load-error').hidden = false;
    $('contract-status').textContent = '契约未加载';
    $('contract-status').className = 'badge badge-danger';
    $('run').disabled = true;
    console.error('Contract load failed:', error);
  }
}

function resetSimulation() {
  app.cancelled = true;
  app.running = false;
  app.virtualMs = 0;
  app.events = [];
  buildFlow();
  $('event-log').replaceChildren();
  $('current-stage').textContent = '待机';
  $('connection-badge').textContent = '未连接';
  $('connection-badge').className = 'badge';
  $('final-result').textContent = '--';
  $('final-result').className = '';
  const realMode = $('mode').value === 'real';
  updateRunAvailability();
  $('scenario').disabled = realMode;
  $('speed').disabled = realMode;
  updateClock();
  setAssertions([
    { label: '尚未执行场景', pass: true },
    { label: realMode ? '等待真实设备配网' : '结果仅代表 Mock 协议流程', pass: true },
  ]);
}

function failInfo(scenario) {
  const map = {
    security_auth_failed: ['protocomm', 'PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH'],
    wifi_auth_failed: ['prov-config', 'NETWORK_PROV_WIFI_STA_AUTH_ERROR'],
    network_not_found: ['prov-config', 'NETWORK_PROV_WIFI_STA_AP_NOT_FOUND'],
    timeout: ['firmware', 'FIRMWARE_CREDENTIAL_OBSERVATION_TIMEOUT'],
    internal_error: ['provider', 'LOCAL_ESP_ERR'],
    window_expired: ['firmware', 'PROVISIONING_WINDOW_EXPIRED'],
  };
  return map[scenario];
}

async function simulateDisconnect() {
  $('connection-badge').textContent = '已断开';
  $('connection-badge').className = 'badge badge-danger';
  logEvent('BLE', 'GATT连接意外断开', '继续广播 PROV_A1B2C3', 'error');
  await advance(1600);
  logEvent('APP', '重新扫描并连接', 'session locator refreshed');
  await advance(900);
  $('connection-badge').textContent = '已重连';
  $('connection-badge').className = 'badge badge-success';
  logEvent('BLE', 'GATT重新连接', '恢复配网流程', 'success');
}

async function runSimulation() {
  if (!app.contract || app.running) return;
  resetSimulation();
  app.cancelled = false;
  app.running = true;
  $('run').disabled = true;
  $('scenario').disabled = true;
  $('speed').disabled = true;

  const scenarioName = $('scenario').value;
  const scenario = SCENARIOS[scenarioName];
  const completed = [];

  try {
    for (const [id, title] of FLOW) {
      setFlowState(id, 'active');
      setStage(title);
      $('connection-badge').textContent = id === 'scan' ? '正在扫描' : '已连接';
      $('connection-badge').className = `badge ${id === 'scan' ? 'badge-info' : 'badge-success'}`;
      let stepDurationMs = 700;
      if (id === 'status' && scenario.expires) {
        stepDurationMs = Math.max(0,
          app.contract.transport.advertisingTimeoutSeconds * 1000 - app.virtualMs);
      } else if (id === 'status' && scenario.virtualDelay) {
        stepDurationMs = scenario.virtualDelay;
      }
      await advance(stepDurationMs);

      if (scenario.disconnectAt === id) await simulateDisconnect();

      if (scenario.failAt === id) {
        const [source, reason] = failInfo(scenarioName);
        setFlowState(id, 'failed');
        logEvent(source, EVENT_TEXT[id][1], reason, 'error');
        FLOW.slice(FLOW.findIndex(([step]) => step === id) + 1).forEach(([step]) => setFlowState(step, 'skipped'));
        setStage(scenario.expires ? '配网窗口结束' : '配网失败', 'badge-danger');
        setResult(scenario.outcome, 'error');
        if (scenario.expires) {
          $('connection-badge').textContent = 'BLE已停止';
          $('connection-badge').className = 'badge badge-danger';
        } else {
          $('connection-badge').textContent = '窗口内可重试';
          $('connection-badge').className = 'badge badge-warning';
        }
        setAssertions([
          { label: `错误映射为 ${scenario.outcome}`, pass: true },
          { label: scenario.expires ? '300秒后停止BLE' : '失败后保留BLE重试窗口', pass: true },
          { label: '未错误判定为配网成功', pass: true },
        ]);
        return;
      }

      const [source, event, result] = EVENT_TEXT[id];
      logEvent(source, event, result, id === 'status' ? 'success' : '');
      setFlowState(id, 'done');
      completed.push(id);
    }

    setStage('Wi-Fi 配网成功', 'badge-success');
    setResult('SUCCESS', 'success');
    const holdMs = app.contract.lifecycle.postSuccessBleWindowMs;
    $('connection-badge').textContent = `保留 ${holdMs / 1000} 秒`;
    $('connection-badge').className = 'badge badge-warning';
    logEvent('firmware', 'IP_EVENT_STA_GOT_IP', `BLE保留${holdMs / 1000}秒`, 'success');
    await advance(holdMs);
    $('connection-badge').textContent = 'BLE已停止';
    $('connection-badge').className = 'badge';
    logEvent('firmware', '成功查询窗口结束', '停止 provisioning BLE');
    setAssertions([
      { label: 'Security 2 会话先于凭据发送', pass: completed.indexOf('security') < completed.indexOf('config') },
      { label: 'proto-ver 先于 Security 2 会话', pass: completed.indexOf('proto') < completed.indexOf('security') },
      { label: 'prov-config 是状态权威来源', pass: app.contract.officialEndpoints.authoritativeStatusEndpoint === 'prov-config' },
      { label: 'GOT_IP 即完成 WIFI_READY', pass: app.contract.successCriteria.firmwareEvent === 'IP_EVENT_STA_GOT_IP' },
      { label: 'vox-config 在 Wi-Fi 凭据前完成', pass: completed.indexOf('vox-config') < completed.indexOf('config') },
      { label: '云端状态不改变 Wi-Fi 结果', pass: app.contract.successCriteria.cloudOnlineRequired === false },
      { label: `成功后保留BLE ${holdMs / 1000}秒`, pass: holdMs === 15000 },
    ]);
  } catch (error) {
    if (error.message !== 'cancelled') console.error(error);
  } finally {
    app.running = false;
    updateRunAvailability();
    $('scenario').disabled = false;
    $('speed').disabled = false;
  }
}

async function loadBridgeHealth() {
  try {
    const response = await fetch('/api/health', { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const health = await response.json();
    app.testGatewayConfigured = health.testGatewayConfigured === true;
    app.testGatewayWsUrl = health.testGatewayWsUrl || '';
    app.reprovisionControlAvailable = health.reprovisionControlAvailable === true;
    app.usbSerialControlAvailable = health.usbSerialControlAvailable === true;
    app.bridgeAvailable = health.ok && health.security === 2 &&
      health.transport === 'ble';
    if (health.testGatewayWsUrl) $('gateway-url').value = health.testGatewayWsUrl;
    $('bridge-status').textContent = app.bridgeAvailable ? 'BLE Bridge 就绪' : 'BLE Bridge 不可用';
    $('bridge-status').className = `badge ${app.bridgeAvailable ? 'badge-success' : 'badge-danger'}`;
    $('test-gateway-status').textContent = health.testGateway
      ? `测试网关 ${health.testGateway} · 可附加验证 DEVICE_ONLINE`
      : '未配置测试网关；仍可验证真实 Wi-Fi 配网和正式服务配置下发';
  } catch (_) {
    app.bridgeAvailable = false;
    app.reprovisionControlAvailable = false;
    app.usbSerialControlAvailable = false;
    app.testGatewayWsUrl = '';
    $('bridge-status').textContent = '仅Mock服务器';
    $('bridge-status').className = 'badge badge-warning';
  }
  updateMode();
  refreshVolumeControlStatus({ quiet: true });
}

function updateMode() {
  const realMode = $('mode').value === 'real';
  $('real-controls').hidden = !realMode;
  if (realMode && !app.qrTarget && $('username').value === DEFAULT_USERNAME) {
    $('username').value = '';
  }
  if (realMode) {
    $('wifi-password').value = '';
    $('wifi-password').type = 'password';
    $('toggle-wifi-password').textContent = '显示';
    $('toggle-wifi-password').setAttribute('aria-label', '显示 Wi-Fi 密码');
    $('toggle-wifi-password').setAttribute('aria-pressed', 'false');
    $('device-secret').value = '';
    $('device-secret').type = 'password';
    $('toggle-device-secret').textContent = '显示';
    $('toggle-device-secret').setAttribute('aria-label', '显示设备密钥');
    $('toggle-device-secret').setAttribute('aria-pressed', 'false');
  }
  $('execution-mode').textContent = realMode ? 'REAL DEVICE MODE' : 'MOCK ONLY';
  $('execution-mode').className = `badge ${realMode ? 'badge-info' : 'badge-warning'}`;
  $('scenario').disabled = realMode;
  $('speed').disabled = realMode;
  $('run').textContent = realMode ? '开始真实配网' : '开始模拟';
  clearWifiNetworks(realMode ? '锁定 BLE 设备后可扫描' : '切换到真实设备模式后可扫描');
  resetSimulation();
  refreshVolumeControlStatus({ quiet: true });
}

function setQrStatus(message, state = '') {
  $('qr-status').textContent = message;
  $('qr-status').className = `qr-status ${state ? `${state}-text` : ''}`.trim();
}

function stopQrCamera() {
  if (app.qrFrameRequest !== null) cancelAnimationFrame(app.qrFrameRequest);
  app.qrFrameRequest = null;
  if (app.qrStream) app.qrStream.getTracks().forEach((track) => track.stop());
  app.qrStream = null;
  $('qr-video').srcObject = null;
  if ($('qr-scanner').open) $('qr-scanner').close();
}

async function getQrDetector() {
  if (app.qrDetector !== undefined) return app.qrDetector;
  app.qrDetector = null;
  if (!('BarcodeDetector' in window)) return null;
  try {
    const formats = await BarcodeDetector.getSupportedFormats();
    if (formats.includes('qr_code')) app.qrDetector = new BarcodeDetector({ formats: ['qr_code'] });
  } catch (_) {
    app.qrDetector = null;
  }
  return app.qrDetector;
}

async function decodeQrSource(source, width, height) {
  const detector = await getQrDetector();
  if (detector) {
    const detected = await detector.detect(source);
    if (detected.length && detected[0].rawValue) return detected[0].rawValue;
  }

  if (typeof window.jsQR !== 'function' || !width || !height) return null;
  const maxSide = 1280;
  const scale = Math.min(1, maxSide / Math.max(width, height));
  const canvas = $('qr-canvas');
  canvas.width = Math.max(1, Math.round(width * scale));
  canvas.height = Math.max(1, Math.round(height * scale));
  const context = canvas.getContext('2d', { willReadFrequently: true });
  context.drawImage(source, 0, 0, canvas.width, canvas.height);
  const pixels = context.getImageData(0, 0, canvas.width, canvas.height);
  const result = window.jsQR(pixels.data, pixels.width, pixels.height, { inversionAttempts: 'attemptBoth' });
  return result?.data || null;
}

async function scanCameraFrame(timestamp) {
  if (!app.qrStream) return;
  const video = $('qr-video');
  if (video.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA
      && timestamp - app.lastQrScanAt >= QR_SCAN_INTERVAL_MS) {
    app.lastQrScanAt = timestamp;
    try {
      const rawText = await decodeQrSource(video, video.videoWidth, video.videoHeight);
      if (rawText) {
        await acceptQrPayload(rawText);
        return;
      }
    } catch (_) {
      $('camera-status').textContent = '当前画面无法解码，请保持二维码完整清晰';
    }
  }
  app.qrFrameRequest = requestAnimationFrame(scanCameraFrame);
}

async function startQrCamera() {
  $('mode').value = 'real';
  updateMode();
  const dialog = $('qr-scanner');
  $('camera-status').textContent = '正在打开摄像头';
  if (!dialog.open) dialog.showModal();
  stopQrCamera();
  if (!dialog.open) dialog.showModal();
  try {
    app.qrStream = await navigator.mediaDevices.getUserMedia({
      video: { facingMode: { ideal: 'environment' } },
      audio: false,
    });
    const video = $('qr-video');
    video.srcObject = app.qrStream;
    await video.play();
    $('camera-status').textContent = '将设备屏幕二维码完整放入取景框';
    app.lastQrScanAt = 0;
    app.qrFrameRequest = requestAnimationFrame(scanCameraFrame);
  } catch (_) {
    app.qrStream = null;
    $('camera-status').textContent = '无法使用摄像头，请允许权限或选择二维码图片';
  }
}

function showQrValidationError(error) {
  if (app.qrTarget) clearQrTarget();
  const code = error?.code || 'QR_INVALID';
  setQrStatus(`${code}：${error?.message || '二维码无法识别'}`, 'error');
  setResult(code, 'error');
  setAssertions([
    { label: '二维码必须符合 Nuwax v1 / BLE / Wi-Fi / Security 2', pass: false },
    { label: '未向任何BLE设备发送配网凭据', pass: true },
  ]);
}

async function acceptQrPayload(rawText) {
  let target;
  try {
    target = window.NuwaxQr.parseProvisioningQr(rawText, {
      // Production screen QR carries identity only. The PoP can be entered
      // from the controlled factory label/credential handoff below.
      requirePop: false,
      serviceNamePrefix: app.contract?.transport?.serviceNamePrefix || 'PROV_',
    });
  } catch (error) {
    stopQrCamera();
    showQrValidationError(error);
    return;
  }

  stopQrCamera();
  app.qrTarget = target;
  app.qrMatched = false;
  clearWifiNetworks('锁定 BLE 设备后可扫描');
  $('mode').value = 'real';
  $('username').value = target.username;
  $('pop').value = target.pop;
  $('username').readOnly = true;
  $('pop').readOnly = Boolean(target.pop);
  $('manual-identity').open = false;
  $('qr-device-name').textContent = target.name;
  $('qr-device-id').textContent = window.NuwaxQr.maskIdentity(target.username);
  $('qr-target').hidden = false;
  $('clear-qr').hidden = false;
  $('qr-match-status').textContent = '正在查找指定设备';
  $('qr-match-status').className = 'badge badge-info';
  $('wifi-fields').classList.add('is-ready');
  $('wifi-step-state').textContent = target.pop
    ? '二维码已验证，请输入 Wi-Fi 信息'
    : '二维码已验证，请补录产线 PoP 和 Wi-Fi 信息';
  setQrStatus(target.pop
    ? `二维码有效，正在按设备名 ${target.name} 和冻结 Service UUID 定向扫描。`
    : `二维码已验证，${target.name} 还需要从受控产线凭据补录 PoP。`, 'success');
  updateMode();
  const scanToken = ++app.qrScanToken;
  const directedScan = scanRealDevices({
    targetName: target.name,
    waitForMs: 30000,
    scanToken,
  });
  $('wifi-fields').scrollIntoView({ behavior: 'smooth', block: 'center' });
  $('ssid').focus({ preventScroll: true });
  await directedScan;
}

function clearQrTarget() {
  stopQrCamera();
  app.qrScanToken += 1;
  if (app.qrTarget) app.qrTarget = null;
  app.qrMatched = false;
  clearWifiNetworks('锁定 BLE 设备后可扫描');
  $('qr-text').value = '';
  $('qr-file').value = '';
  $('username').readOnly = false;
  $('pop').readOnly = false;
  $('username').value = '';
  $('pop').value = app.contract?.security?.developmentPoP || '';
  $('manual-identity').open = true;
  $('qr-target').hidden = true;
  $('clear-qr').hidden = true;
  $('qr-device-name').textContent = '--';
  $('qr-device-id').textContent = '--';
  $('real-device').replaceChildren(new Option('请手动扫描BLE设备', ''));
  $('wifi-fields').classList.remove('is-ready');
  $('wifi-step-state').textContent = '输入 2.4 GHz Wi-Fi 信息';
  setQrStatus('已切换为手动模式。设备名、Username 和 PoP 需要人工确认。');
  resetSimulation();
}

async function scanQrFile(file) {
  if (!file) return;
  $('mode').value = 'real';
  updateMode();
  setQrStatus('正在解析二维码图片。');
  try {
    const bitmap = await createImageBitmap(file);
    const rawText = await decodeQrSource(bitmap, bitmap.width, bitmap.height);
    bitmap.close();
    if (!rawText) {
      const error = new Error('图片中未检测到二维码');
      error.code = 'QR_NOT_DETECTED';
      throw error;
    }
    await acceptQrPayload(rawText);
  } catch (error) {
    showQrValidationError(error);
  } finally {
    $('qr-file').value = '';
  }
}

async function scanRealDevices(options = {}) {
  if (!app.bridgeAvailable || app.running || app.provisionPending || app.wifiScanRunning) return;
  const button = $('scan-devices');
  const select = $('real-device');
  const targetName = options.targetName || app.qrTarget?.name || '';
  const scanToken = options.scanToken || ++app.qrScanToken;
  const deadline = Date.now() + (options.waitForMs || 0);
  button.disabled = true;
  button.textContent = '扫描中';
  select.replaceChildren(new Option(targetName ? `正在查找 ${targetName}` : '正在扫描PROV_设备', ''));
  try {
    while (true) {
      const response = await fetch('/api/devices', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: '{}',
        cache: 'no-store',
      });
      const body = await response.json();
      if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`);
      if (scanToken !== app.qrScanToken) return;
      select.replaceChildren();
      if (targetName) {
        const exactTarget = window.NuwaxQr.selectExactQrDevice(body.devices, targetName);
        if (!exactTarget && Date.now() < deadline) {
          app.qrMatched = false;
          select.appendChild(new Option(`等待 ${targetName} 进入BLE配网模式`, ''));
          $('qr-match-status').textContent = '等待设备BLE广播';
          $('qr-match-status').className = 'badge badge-info';
          setQrStatus('二维码已验证。若设备已连接Wi-Fi，请返回设备菜单点击 BLE；模拟器正在定向等待设备重启。');
          await sleep(700);
          continue;
        }
        if (!exactTarget) {
          app.qrMatched = false;
          select.appendChild(new Option(`${targetName} 未进入BLE配网模式`, ''));
          $('qr-match-status').textContent = '未发现指定设备';
          $('qr-match-status').className = 'badge badge-danger';
          setQrStatus('QR_DEVICE_NOT_FOUND：30秒内未发现二维码指定设备。请在设备菜单点击 BLE，等待重启后重新扫描。', 'error');
          setResult('QR_DEVICE_NOT_FOUND', 'error');
          setAssertions([
            { label: `仅允许连接二维码指定的 ${targetName}`, pass: true },
            { label: `广播必须包含 ${app.contract.transport.primaryServiceUuid}`, pass: true },
            { label: '当前扫描未发现完全匹配设备', pass: false },
          ]);
          return;
        }
        select.appendChild(new Option(`${exactTarget.name}  RSSI ${exactTarget.rssi} dBm`, exactTarget.name));
        select.value = exactTarget.name;
        app.qrMatched = true;
        $('qr-match-status').textContent = '已锁定指定设备';
        $('qr-match-status').className = 'badge badge-success';
        setQrStatus(`已锁定 ${exactTarget.name}，不会连接附近其他 PROV_ 设备。`, 'success');
        setResult('--', '');
        setAssertions([
          { label: 'Nuwax v1二维码字段校验通过', pass: true },
          { label: '广播名与二维码设备名完全一致', pass: true },
          { label: '广播包含冻结Service UUID', pass: true },
          { label: 'Security 2后仍将校验device-info身份', pass: true },
        ]);
        return;
      }
      if (!body.devices.length) {
        select.appendChild(new Option('未发现配网设备', ''));
        return;
      }
      body.devices.forEach((device) => {
        const label = `${device.name}  RSSI ${device.rssi} dBm`;
        select.appendChild(new Option(label, device.name));
      });
      return;
    }
  } catch (error) {
    select.replaceChildren(new Option(`扫描失败: ${error.message}`, ''));
    if (app.qrTarget) {
      app.qrMatched = false;
      $('qr-match-status').textContent = 'BLE扫描失败';
      $('qr-match-status').className = 'badge badge-danger';
    }
  } finally {
    button.disabled = false;
    button.textContent = '扫描BLE设备';
    updateRunAvailability();
    refreshVolumeControlStatus({ quiet: true });
  }
}

async function requestRemoteBleReprovision() {
  const deviceId = $('username').value.trim();
  const serviceName = app.qrTarget?.name || $('real-device').value;
  if (!deviceId || !serviceName || !app.bridgeAvailable ||
      !app.reprovisionControlAvailable || app.reprovisionRequestRunning) return;

  const button = $('remote-reprovision');
  const status = $('remote-reprovision-status');
  if (Date.now() > app.reprovisionConfirmUntil) {
    app.reprovisionConfirmUntil = Date.now() + 8000;
    button.textContent = '再次点击确认重启';
    status.textContent = `将通过本机 USB 或测试网关让 ${deviceId} 重启进入 BLE 配网模式`;
    window.setTimeout(() => {
      if (Date.now() >= app.reprovisionConfirmUntil && !app.reprovisionRequestRunning) {
        app.reprovisionConfirmUntil = 0;
        button.textContent = '让设备进入配网模式';
      }
    }, 8100);
    return;
  }
  app.reprovisionConfirmUntil = 0;
  app.reprovisionRequestRunning = true;
  updateRunAvailability();
  button.textContent = '正在发送命令';
  status.textContent = '正通过本机 USB（优先）或本地测试网关请求设备保存回滚信息并重启';
  try {
    const response = await fetch('/api/reprovision', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ deviceId, serviceName }),
      cache: 'no-store',
    });
    const body = await response.json();
    if (!response.ok || body.ok !== true) throw new Error(body.error || `HTTP ${response.status}`);
    if (body.state !== 'ble_advertisement_confirmed') {
      throw new Error('设备控制未确认 BLE 广播');
    }
    status.textContent = `已确认 ${body.serviceName} BLE 广播，可以开始配网`;
    logEvent('device', '设备进入 BLE 配网模式',
      `${body.deviceId} ${body.method} ble_advertisement_confirmed`);
    await scanRealDevices({ targetName: serviceName, waitForMs: 10000 });
  } catch (error) {
    status.textContent = `请求失败：${error.message}`;
    logEvent('gateway', '远程进入 BLE 配网模式失败', error.message, 'error');
  } finally {
    app.reprovisionRequestRunning = false;
    button.textContent = '让设备进入配网模式';
    updateRunAvailability();
  }
}

function applyRealEvent(event) {
  app.virtualMs = event.timeMs;
  updateClock();
  const reprovisionStatus = $('remote-reprovision-status');
  const knownStep = FLOW.some(([id]) => id === event.stage);
  if (knownStep) {
    FLOW.forEach(([id]) => {
      if (id === event.stage) {
        setFlowState(id, ['status', 'cloud'].includes(event.stage) ? 'active' : 'done');
      }
    });
    const flowEntry = FLOW.find(([id]) => id === event.stage);
    setStage(flowEntry ? flowEntry[1] : event.stage);
  }
  if (event.stage === 'failed') setStage('配网失败', 'badge-danger');
  if (event.stage === 'connect' && reprovisionStatus) {
    reprovisionStatus.textContent = '已发现设备并建立 BLE 连接，正在下发配网参数';
  }
  if (event.stage === 'wifi-ready') {
    setFlowState('status', 'done');
    setFlowState('cloud', 'active');
    setStage('Wi-Fi 已就绪，云端状态单独观察');
  }
  if (event.stage === 'cloud-warning') {
    setFlowState('cloud', 'skipped');
    setStage('Wi-Fi 已成功，云端未观测', 'badge-warning');
  }
  if (event.stage === 'success') {
    setFlowState('status', 'done');
    if (event.result.includes('DEVICE_ONLINE')) setFlowState('cloud', 'done');
    setStage(event.result.includes('DEVICE_ONLINE') ? 'Wi-Fi 成功且设备在线' : 'Wi-Fi 配网成功', 'badge-success');
    if (reprovisionStatus) {
      reprovisionStatus.textContent = '设备已完成 BLE 配网；云端在线状态按目标服务单独确认';
    }
  }
  logEvent(event.stage, event.message, event.result,
    event.stage === 'failed' ? 'error' : event.stage === 'success' ? 'success' : '');
}

async function waitForWifiScanHandoff() {
  const deadline = Date.now() + WIFI_SCAN_HANDOFF_TIMEOUT_MS;
  while (app.wifiScanRunning && Date.now() < deadline) await sleep(100);
  return !app.wifiScanRunning;
}

async function runRealProvisioning() {
  if (!app.contract || !app.bridgeAvailable || app.running || app.provisionPending) return;
  const serviceName = $('real-device').value;
  const request = {
    serviceName,
    username: $('username').value.trim(),
    pop: $('pop').value,
    gatewayUrl: $('gateway-url').value.trim(),
    deviceSecret: $('device-secret').value,
    ssid: $('ssid').value,
    password: $('wifi-password').value,
  };
  if (app.qrTarget && (
    !app.qrMatched
    || serviceName !== app.qrTarget.name
    || request.username !== app.qrTarget.username
    || (app.qrTarget.pop && request.pop !== app.qrTarget.pop)
  )) {
    setResult('QR_TARGET_MISMATCH', 'error');
    setAssertions([{ label: '二维码目标、BLE广播和Security 2身份必须保持一致', pass: false }]);
    updateRunAvailability();
    return;
  }
  if (Object.values(request).some((value) => !value)) {
    setResult('INPUT_REQUIRED', 'error');
    setAssertions([{ label: '设备、Security 2、Gateway URL、Device Secret 和 Wi-Fi 信息均不能为空', pass: false }]);
    return;
  }

  app.provisionPending = true;
  app.wifiAutoRefreshStopped = true;
  clearWifiAutoRefreshTimers();
  app.wifiScanOutcome = '已请求配网，自动扫描已停止';
  updateRunAvailability();
  if (!await waitForWifiScanHandoff()) {
    request.pop = '';
    request.password = '';
    request.deviceSecret = '';
    app.provisionPending = false;
    app.wifiScanOutcome = '当前扫描未能及时结束，请手动刷新后重试';
    setResult('SCAN_STOP_TIMEOUT', 'error');
    setAssertions([{ label: '必须在设备扫描释放 BLE 后才允许开始配网', pass: false }]);
    updateRunAvailability();
    return;
  }
  app.wifiScanOutcome = '设备扫描已停止，正在开始配网';
  renderWifiScanStatus();
  app.provisionPending = false;
  resetSimulation();
  app.cancelled = false;
  app.running = true;
  clearWifiAutoRefreshTimers();
  app.realEventCursor = 0;
  $('run').disabled = true;
  $('scan-devices').disabled = true;
  $('scan-wifi').disabled = true;
  $('connection-badge').textContent = 'Bridge处理中';
  $('connection-badge').className = 'badge badge-info';

  try {
    const response = await fetch('/api/provision', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(request),
    });
    request.pop = '';
    request.password = '';
    request.deviceSecret = '';
    const created = await response.json();
    if (!response.ok) throw new Error(created.error || `HTTP ${response.status}`);
    app.realJobId = created.jobId;

    while (true) {
      await sleep(500);
      const jobResponse = await fetch(`/api/jobs/${encodeURIComponent(app.realJobId)}`, { cache: 'no-store' });
      const job = await jobResponse.json();
      if (!jobResponse.ok) throw new Error(job.error || `HTTP ${jobResponse.status}`);
      job.events.slice(app.realEventCursor).forEach(applyRealEvent);
      app.realEventCursor = job.events.length;
      if (job.state === 'queued' || job.state === 'running') continue;

      const succeeded = job.state === 'success' && job.result === 'SUCCESS';
      const cloudOnline = job.cloudState === 'DEVICE_ONLINE';
      app.wifiAutoRefreshStopped = true;
      app.wifiScanOutcome = succeeded
        ? '配网成功，设备 Wi-Fi 扫描已停止'
        : '本次配网结束，设备 Wi-Fi 扫描保持停止';
      const identityMatched = job.deviceInfo?.serialNumber === request.username;
      if (Number.isInteger(job.wifiStatus?.rawDisconnectReason)) {
        logEvent(
          'wifi-raw',
          '设备原始 Wi-Fi 断线原因',
          `${job.wifiStatus.rawDisconnectReason} / ${job.wifiStatus.rawDisconnectReasonName || 'unmapped'}`,
          'error',
        );
      }
      setResult(job.result || 'INTERNAL_ERROR', succeeded ? 'success' : 'error');
      $('connection-badge').textContent = succeeded
        ? (cloudOnline ? 'DEVICE_ONLINE' : 'WIFI_READY')
        : '需要检查失败阶段';
      $('connection-badge').className = `badge ${succeeded ? 'badge-success' : 'badge-warning'}`;
      setAssertions([
        { label: '使用真实BLE GATT传输', pass: true },
        { label: 'Security 2且未降级', pass: job.protocolInfo?.prov?.sec_ver === 2 },
        { label: '包含wifi_prov capability', pass: job.protocolInfo?.prov?.cap?.includes('wifi_prov') === true },
        { label: '包含cloud_prov capability', pass: job.protocolInfo?.nuwax?.cap?.includes('cloud_prov') === true },
        { label: 'device-info身份与二维码/Username一致', pass: identityMatched },
        { label: '动态URL和设备密钥已通过vox-config暂存', pass: job.cloudConfig?.state === 'active' },
        { label: succeeded ? '官方prov-config已达到WIFI_READY' : `错误映射为${job.result}`, pass: true },
        { label: cloudOnline ? '测试网关已观测DEVICE_ONLINE' : `云端附加状态：${job.cloudState || 'NOT_OBSERVED'}`, pass: true },
      ]);
      break;
    }
  } catch (error) {
    app.wifiAutoRefreshStopped = true;
    app.wifiScanOutcome = '配网调用结束，设备 Wi-Fi 扫描保持停止';
    setStage('Bridge调用失败', 'badge-danger');
    setResult('INTERNAL_ERROR', 'error');
    logEvent('bridge', '本地Bridge调用失败', error.message, 'error');
    setAssertions([{ label: error.message, pass: false }]);
  } finally {
    app.provisionPending = false;
    app.running = false;
    $('scan-devices').disabled = false;
    updateRunAvailability();
  }
}

function runSelectedMode() {
  return $('mode').value === 'real' ? runRealProvisioning() : runSimulation();
}

function toggleWifiPasswordVisibility() {
  const password = $('wifi-password');
  const toggle = $('toggle-wifi-password');
  const visible = password.type === 'text';
  password.type = visible ? 'password' : 'text';
  toggle.textContent = visible ? '显示' : '隐藏';
  toggle.setAttribute('aria-label', visible ? '显示 Wi-Fi 密码' : '隐藏 Wi-Fi 密码');
  toggle.setAttribute('aria-pressed', String(!visible));
  password.focus({ preventScroll: true });
}

function toggleDeviceSecretVisibility() {
  const secret = $('device-secret');
  const toggle = $('toggle-device-secret');
  const visible = secret.type === 'text';
  secret.type = visible ? 'password' : 'text';
  toggle.textContent = visible ? '显示' : '隐藏';
  toggle.setAttribute('aria-label', visible ? '显示设备密钥' : '隐藏设备密钥');
  toggle.setAttribute('aria-pressed', String(!visible));
  secret.focus({ preventScroll: true });
}

function exportLog() {
  if (!app.events.length) return;
  const payload = {
    generatedAt: new Date().toISOString(),
    contractVersion: app.contract?.contractVersion,
    scenario: $('scenario').value,
    mockOnly: $('mode').value !== 'real',
    input: {
      deviceName: $('real-device').value || null,
      usernameMasked: window.NuwaxQr.maskIdentity($('username').value),
      ssid: $('ssid').value,
    },
    events: app.events,
    result: $('final-result').textContent,
  };
  const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = `ble-provision-${$('scenario').value}.json`;
  link.click();
  URL.revokeObjectURL(link.href);
}

$('run').addEventListener('click', runSelectedMode);
$('toggle-wifi-password').addEventListener('click', toggleWifiPasswordVisibility);
$('toggle-device-secret').addEventListener('click', toggleDeviceSecretVisibility);
$('volume-range').addEventListener('input', () => {
  $('volume-value').textContent = `${$('volume-range').value}%`;
});
$('refresh-volume-status').addEventListener('click', () => refreshVolumeControlStatus());
$('send-volume').addEventListener('click', sendTestGatewayVolume);
$('meeting-mode-toggle').addEventListener('change', (event) => {
  sendTestGatewayAudioMode('meeting', event.target.checked);
});
$('monitoring-mode-toggle').addEventListener('change', (event) => {
  sendTestGatewayAudioMode('monitoring', event.target.checked);
});
$('reset').addEventListener('click', () => {
  app.wifiAutoRefreshStopped = false;
  app.wifiScanOutcome = '已重置，准备扫描设备 Wi-Fi';
  resetSimulation();
});
$('export-log').addEventListener('click', exportLog);
$('mode').addEventListener('change', updateMode);
$('scan-devices').addEventListener('click', scanRealDevices);
$('remote-reprovision').addEventListener('click', requestRemoteBleReprovision);
$('scan-wifi').addEventListener('click', scanDeviceWifi);
$('scan-qr').addEventListener('click', startQrCamera);
$('close-qr-scanner').addEventListener('click', stopQrCamera);
$('qr-scanner').addEventListener('cancel', (event) => {
  event.preventDefault();
  stopQrCamera();
});
$('qr-file').addEventListener('change', (event) => scanQrFile(event.target.files?.[0]));
$('clear-qr').addEventListener('click', clearQrTarget);
$('parse-qr-text').addEventListener('click', async () => {
  const rawText = $('qr-text').value;
  $('qr-text').value = '';
  await acceptQrPayload(rawText);
});
$('real-device').addEventListener('change', () => {
  clearWifiNetworks('设备已变更，请重新扫描 Wi-Fi');
  if (app.qrTarget) {
    app.qrMatched = $('real-device').value === app.qrTarget.name;
  }
  updateRunAvailability();
});
['username', 'pop', 'gateway-url', 'device-secret', 'ssid', 'wifi-password'].forEach((id) => {
  $(id).addEventListener('input', () => {
    if (id === 'username' || id === 'pop') {
      clearWifiNetworks('设备身份已变更，等待重新扫描 Wi-Fi');
    }
    if (id === 'wifi-password' && $('mode').value === 'real') {
      if ($(id).value) {
        app.wifiAutoRefreshStopped = true;
        clearWifiAutoRefreshTimers();
        app.wifiScanOutcome = app.wifiScanRunning
          ? 'Wi-Fi 信息已填写，当前扫描结束后停止自动刷新'
          : 'Wi-Fi 信息已填写，自动扫描已暂停';
      } else if (!app.running && !app.provisionPending) {
        app.wifiAutoRefreshStopped = false;
      }
    }
    updateRunAvailability();
    if (id === 'username' || id === 'gateway-url') {
      app.volumeRefreshToken += 1;
      applyImmediateVolumeContext();
      window.clearTimeout(app.volumeInputTimer);
      app.volumeInputTimer = window.setTimeout(
        () => refreshVolumeControlStatus({ quiet: true }),
        350,
      );
    }
    if (id === 'ssid') syncWifiNetworkSelection();
  });
});
$('contract-file').addEventListener('change', async (event) => {
  const [file] = event.target.files;
  if (!file) return;
  try {
    app.contract = validateContract(JSON.parse(await file.text()));
    renderContract();
  } catch (error) {
    $('contract-status').textContent = '契约格式错误';
    console.error(error);
  }
});

buildFlow();
loadContract();
loadBridgeHealth();
window.setInterval(() => {
  if (document.visibilityState === 'visible' && !app.volumeControlBusy && !app.audioModeControlBusy) {
    refreshVolumeControlStatus({ quiet: true });
  }
}, 5000);
