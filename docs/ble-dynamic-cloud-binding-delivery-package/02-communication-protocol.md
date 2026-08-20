# 通信协议与报文格式

## 1. 二维码到 APP

开发二维码示例：

```json
{
  "ver": "v1",
  "name": "PROV_1467C4",
  "transport": "ble",
  "network": "wifi",
  "security": 2,
  "username": "NX-3927-2693-9288",
  "pop": "<development-or-per-device-pop>"
}
```

生产 identity-only 屏幕二维码可以不含 `pop`，但 APP 必须从受控产线标签或云端授权接口取得每设备 PoP。没有 PoP 不能建立 Security 2；`deviceSecret` 不能替代 PoP。

## 2. proto-ver 和 endpoint 发现

BLE 服务发现完成后，APP 必须读取原始 `proto-ver` JSON。结构示例：

```json
{
  "prov": {
    "ver": "<network-provisioning-version>",
    "sec_ver": 2,
    "sec_patch_ver": 1,
    "cap": ["wifi_prov", "wifi_scan"]
  },
  "nuwax": {
    "ver": "7.0.0",
    "cap": ["wifi_prov", "wifi_scan", "cloud_prov"]
  }
}
```

APP 验证规则：`prov.sec_ver == 2`、`prov.cap` 含 `wifi_prov/wifi_scan`、`nuwax.cap` 含 `cloud_prov`。Espressif Android `ESPDevice.getDeviceCapabilities()`只解析 `prov.cap`，不能用它检查 `cloud_prov`；必须解析 `ESPDevice.getVersionInfo()` 原始 JSON。

`device-info`、`cloud-config` 的 GATT characteristic UUID 由 provisioning manager 分配，不是冻结常量。Android BLE transport 在服务发现后读取各 characteristic 的 `0x2901 User Description` descriptor，建立 endpoint name -> UUID 映射。APP 只按字符串 endpoint 名调用，不得硬编码 `device-info`/`cloud-config` characteristic UUID。

## 3. APP/Bridge 到测试网关：领取签名包

```http
POST /api/onboarding/claim HTTP/1.1
Host: <apiBaseUrl host>
Authorization: Bearer <APP用户登录Token或本地测试Token>
Content-Type: application/json

{"deviceId":"NX-3927-2693-9288"}
```

成功响应：

```json
{
  "ok": true,
  "bundle": {
    "version": 1,
    "deviceId": "NX-3927-2693-9288",
    "cloudUrl": "wss://testagent.xspaceagi.com/api/device/ws",
    "credentialId": "cred_<random-id>",
    "deviceSecret": "<per-device-random-secret>",
    "bundleVersion": 1784954350746,
    "signature": "<base64-ecdsa-der>"
  }
}
```

当前测试服务生成 16 个随机字节并编码为 32 位十六进制 `deviceSecret`。APP 应把密钥视为不透明字符串，不应限定生产永远是 32 位十六进制；固件上限为 95 个非空白 UTF-8/ASCII 字节。

## 4. 云端签名原文

签名算法：ECDSA P-256 + SHA-256；签名结果为 DER，再 base64。云端按以下固定顺序构造 UTF-8 字节，末尾包含换行：

```text
NUWAX-CLOUD-CONFIG-V1
version=<version>
deviceId=<deviceId>
cloudUrl=<cloudUrl>
credentialId=<credentialId>
deviceSecret=<deviceSecret>
bundleVersion=<bundleVersion>
```

APP 不参与签名。JSON 键顺序不影响设备验签，但所有字段值必须原样透传；APP 不得 trim、URL normalize、替换大小写或重新编码签名。

## 5. APP/Bridge 到设备：cloud-config

前提：Security 2 会话已经建立，`device-info.serialNumber` 已与二维码 username 匹配。

```text
protocomm endpoint: cloud-config
明文编码: UTF-8 JSON
线上保护: Security 2 AES-GCM
```

请求：

```json
{
  "op": "set",
  "bundle": {
    "version": 1,
    "deviceId": "NX-3927-2693-9288",
    "cloudUrl": "wss://testagent.xspaceagi.com/api/device/ws",
    "credentialId": "cred_<random-id>",
    "deviceSecret": "<per-device-random-secret>",
    "bundleVersion": 1784954350746,
    "signature": "<base64-ecdsa-der>"
  }
}
```

成功响应不会回显密钥：

```json
{
  "ok": true,
  "state": "pending",
  "credentialId": "cred_<random-id>",
  "bundleVersion": 1784954350746
}
```

错误响应：

```json
{"ok":false,"error":"CLOUD_SIGNATURE_INVALID"}
```

错误值：`CLOUD_CONFIG_INVALID`、`CLOUD_SIGNATURE_INVALID`、`CLOUD_CONFIG_ROLLBACK`。

字段限制：`version` 必须是 JSON 整数 `1`；`deviceId` 最多 39 字节，`cloudUrl` 最多 191 字节，`credentialId` 最多 63 字节，`deviceSecret` 最多 95 字节，`signature` 最多 111 字节；`bundleVersion` 为 `1..9007199254740991` 的 JSON 整数。七个字段全部必填。生产只允许有效证书校验的 `wss://`，`ws://` 仅由测试 Kconfig 开启。

## 6. APP/Bridge 到设备：Wi-Fi

Wi-Fi 不使用上面的 JSON，而继续使用 `network_provisioning` 官方 protobuf：

1. `prov-config` SetConfig：SSID 和 passphrase。
2. `prov-config` ApplyConfig。
3. `prov-config` GetStatus：`connecting`、`failed`、`connected`。

必须先收到 `cloud-config ok=true,state=pending`，再发送 Wi-Fi。设备扫描使用官方 `prov-scan`，扫描和配网必须串行。

## 7. 设备到云端：WebSocket Upgrade

设备从 NVS pending/active 读取动态配置，连接 bundle 中的 `cloudUrl`：

```http
GET /api/device/ws HTTP/1.1
Upgrade: websocket
Connection: Upgrade
X-Device-ID: NX-3927-2693-9288
X-Credential-ID: cred_<random-id>
Authorization: Bearer <deviceSecret>
```

网关必须在返回 `101 Switching Protocols` 前校验三个字段。服务端只持久化 `deviceSecret` 的 SHA-256 摘要，并使用常量时间比较。

设备建立 WebSocket 后发送 hello：

```json
{
  "type": "hello",
  "version": 2,
  "device_id": "NX-3927-2693-9288",
  "credential_id": "cred_<random-id>",
  "features": {"cloud_auth_v2": true}
}
```

网关返回 authenticated hello；设备完成 NVS active 提交后发送：

```json
{
  "type": "cloud_config_committed",
  "version": 1,
  "device_id": "NX-3927-2693-9288",
  "credential_id": "cred_<random-id>"
}
```

网关成功提交注册表后回复：

```json
{
  "type": "cloud_config_committed_ack",
  "ok": true,
  "credential_id": "cred_<random-id>"
}
```

APP 不直接接收这条 WebSocket 回复，最终仍以 status API 为准。当前测试固件在本地 active 提交后发送一次 `cloud_config_committed`，但尚未持久化或重试服务端 `_ack`；若该提交消息丢失，status 会停留在 pending 并最终映射 `CLOUD_CONNECT_TIMEOUT`。生产固件必须把提交设计为可幂等补发，云端必须允许同一 `(deviceId, credentialId)` 重复确认。

## 8. APP 到云端：查询最终状态

```http
GET /api/onboarding/status?device_id=NX-3927-2693-9288 HTTP/1.1
Authorization: Bearer <APP用户登录Token或本地测试Token>
```

```json
{
  "ok": true,
  "deviceId": "NX-3927-2693-9288",
  "state": "DEVICE_ONLINE",
  "credentialId": "cred_<random-id>"
}
```

状态可为 `WAITING_FOR_DEVICE`、`CLOUD_CONFIG_COMMIT_PENDING`、`DEVICE_ONLINE`、`CLOUD_CREDENTIAL_REJECTED`。APP 必须同时核对 `credentialId`，防止把旧连接误判为本次成功。
