'use strict';

(function exposeQrCodec(root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  if (root) root.NuwaxQr = api;
}(typeof globalThis !== 'undefined' ? globalThis : this, function createQrCodec() {
  class ProvisioningQrError extends Error {
    constructor(code, message) {
      super(message);
      this.name = 'ProvisioningQrError';
      this.code = code;
    }
  }

  function requireString(payload, field, code) {
    if (typeof payload[field] !== 'string' || !payload[field].trim()) {
      throw new ProvisioningQrError(code, `二维码缺少 ${field}`);
    }
    return payload[field].trim();
  }

  function utf8Length(value) {
    return new TextEncoder().encode(value).length;
  }

  function parseProvisioningQr(rawText, options = {}) {
    if (typeof rawText !== 'string' || !rawText.trim()) {
      throw new ProvisioningQrError('QR_EMPTY', '未识别到二维码内容');
    }
    if (rawText.length > 2048) {
      throw new ProvisioningQrError('QR_TOO_LARGE', '二维码内容超过允许长度');
    }

    let payload;
    try {
      payload = JSON.parse(rawText);
    } catch (_) {
      throw new ProvisioningQrError('QR_INVALID_JSON', '二维码不是有效的 Nuwax JSON');
    }
    if (!payload || Array.isArray(payload) || typeof payload !== 'object') {
      throw new ProvisioningQrError('QR_INVALID_OBJECT', '二维码内容必须是 JSON 对象');
    }
    if (payload.ver !== 'v1') {
      throw new ProvisioningQrError('QR_VERSION_UNSUPPORTED', '仅支持 Nuwax v1 二维码');
    }
    if (payload.transport !== 'ble') {
      throw new ProvisioningQrError('QR_TRANSPORT_UNSUPPORTED', '二维码不是 BLE 配网设备');
    }
    if (payload.network !== 'wifi') {
      throw new ProvisioningQrError('QR_NETWORK_UNSUPPORTED', '二维码不是 Wi-Fi 配网');
    }
    if (payload.security !== 2) {
      throw new ProvisioningQrError('QR_SECURITY_UNSUPPORTED', '设备必须使用 Security 2');
    }

    const name = requireString(payload, 'name', 'QR_DEVICE_NAME_MISSING');
    const prefix = options.serviceNamePrefix || 'PROV_';
    const escapedPrefix = prefix.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    if (!(new RegExp(`^${escapedPrefix}[0-9A-F]{6}$`)).test(name)) {
      throw new ProvisioningQrError(
        'QR_DEVICE_NAME_INVALID',
        `设备名必须为 ${prefix} 加 6 位大写十六进制`,
      );
    }
    const username = requireString(payload, 'username', 'QR_USERNAME_MISSING');
    if (username.includes('\0') || utf8Length(username) > 128) {
      throw new ProvisioningQrError('QR_USERNAME_INVALID', '二维码 username 超过协议限制');
    }
    let pop = '';
    if (payload.pop !== undefined && payload.pop !== null) {
      if (typeof payload.pop !== 'string') {
        throw new ProvisioningQrError('QR_POP_INVALID', '二维码 PoP 格式无效');
      }
      pop = payload.pop;
    }
    if (pop.includes('\0') || utf8Length(pop) > 128) {
      throw new ProvisioningQrError('QR_POP_INVALID', '二维码 PoP 超过协议限制');
    }
    if (options.requirePop && !pop) {
      throw new ProvisioningQrError(
        'QR_POP_REQUIRED',
        '当前开发固件真机配网需要二维码包含 PoP',
      );
    }

    return Object.freeze({
      ver: 'v1',
      name,
      transport: 'ble',
      network: 'wifi',
      security: 2,
      username,
      pop,
    });
  }

  function maskIdentity(value) {
    if (typeof value !== 'string' || !value) return '--';
    if (value.length <= 8) return `${value.slice(0, 2)}***${value.slice(-2)}`;
    return `${value.slice(0, 5)}***${value.slice(-4)}`;
  }

  function selectExactQrDevice(devices, targetName) {
    if (!Array.isArray(devices) || typeof targetName !== 'string') return null;
    return devices.find((device) => (
      device
      && device.name === targetName
      && device.serviceUuidMatched === true
    )) || null;
  }

  return { ProvisioningQrError, parseProvisioningQr, maskIdentity, selectExactQrDevice };
}));
