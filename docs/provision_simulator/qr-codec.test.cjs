'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { parseProvisioningQr, selectExactQrDevice } = require('./qr-codec.js');

const VALID = {
  ver: 'v1',
  name: 'PROV_A1B2C3',
  transport: 'ble',
  network: 'wifi',
  security: 2,
  username: 'NX-A1B2-C3D4-E5F6',
  pop: 'development-secret',
};

function expectCode(payload, code, options = { requirePop: true }) {
  assert.throws(
    () => parseProvisioningQr(typeof payload === 'string' ? payload : JSON.stringify(payload), options),
    (error) => error.code === code,
  );
}

test('accepts the frozen Nuwax v1 development QR fields', () => {
  const parsed = parseProvisioningQr(JSON.stringify(VALID), { requirePop: true });
  assert.equal(parsed.name, VALID.name);
  assert.equal(parsed.username, VALID.username);
  assert.equal(parsed.security, 2);
});

test('rejects malformed JSON', () => expectCode('{broken', 'QR_INVALID_JSON'));
test('rejects an oversized payload', () => expectCode('x'.repeat(2049), 'QR_TOO_LARGE'));
test('rejects unsupported version', () => expectCode({ ...VALID, ver: 'v2' }, 'QR_VERSION_UNSUPPORTED'));
test('rejects Security 0 or 1', () => expectCode({ ...VALID, security: 1 }, 'QR_SECURITY_UNSUPPORTED'));
test('rejects SoftAP transport', () => expectCode({ ...VALID, transport: 'softap' }, 'QR_TRANSPORT_UNSUPPORTED'));
test('rejects non-Wi-Fi provisioning', () => expectCode({ ...VALID, network: 'thread' }, 'QR_NETWORK_UNSUPPORTED'));
test('rejects a name outside the frozen prefix', () => expectCode({ ...VALID, name: 'ESP_A1B2C3' }, 'QR_DEVICE_NAME_INVALID'));
test('rejects a malformed name inside the frozen prefix', () => expectCode({ ...VALID, name: 'PROV_a1b2c3-extra' }, 'QR_DEVICE_NAME_INVALID'));
test('rejects a missing username', () => expectCode({ ...VALID, username: '' }, 'QR_USERNAME_MISSING'));
test('rejects an oversized username', () => expectCode({ ...VALID, username: 'N'.repeat(129) }, 'QR_USERNAME_INVALID'));
test('rejects an oversized PoP', () => expectCode({ ...VALID, pop: 'P'.repeat(129) }, 'QR_POP_INVALID'));
test('rejects a missing PoP in development real mode', () => expectCode({ ...VALID, pop: undefined }, 'QR_POP_REQUIRED'));

test('parses a production identity QR without exposing a PoP', () => {
  const payload = { ...VALID };
  delete payload.pop;
  const parsed = parseProvisioningQr(JSON.stringify(payload), { requirePop: false });
  assert.equal(parsed.pop, '');
});

test('selects only the QR-named device when multiple PROV devices are nearby', () => {
  const devices = [
    { name: 'PROV_999999', serviceUuidMatched: true, rssi: -20 },
    { name: VALID.name, serviceUuidMatched: true, rssi: -70 },
  ];
  assert.equal(selectExactQrDevice(devices, VALID.name)?.name, VALID.name);
});

test('rejects a name match without the frozen Service UUID', () => {
  const devices = [{ name: VALID.name, serviceUuidMatched: false, rssi: -20 }];
  assert.equal(selectExactQrDevice(devices, VALID.name), null);
});
