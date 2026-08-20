# 冻结协议与 APP 流程

## 1. 版本与 BLE 参数

| 项目 | 冻结值 |
| --- | --- |
| ESP-IDF | `v6.0` / `662a3be354759d9487bf4b1a629fadb766cb1800` |
| network_provisioning | `1.2.4` / `2de4980640bbe3d2d69473d7251640039e185b92` |
| Android 参考库 | `com.github.espressif:esp-idf-provisioning-android:lib-2.4.4` |
| BLE Host | NimBLE |
| Service UUID | `0000ffff-0000-1000-8000-00805f9b34fb` |
| 广播名 | `PROV_` + STA MAC 最后三字节大写十六进制 |
| 当前设备 | UID `NX-3927-2693-9288`，广播 `PROV_1467C4` |
| 安全 | protocomm Security 2，SRP6a + AES-GCM |
| SoftAP | 不支持 |

BLE 地址只用于本次扫描定位，不能用作永久设备 ID。永久身份只使用
`device-info.serialNumber` 返回的硬件 UID。

## 2. capability

APP 必须读取原始 `proto-ver` JSON：

```json
{
  "prov": {
    "sec_ver": 2,
    "cap": ["wifi_prov", "wifi_scan"]
  },
  "nuwax": {
    "ver": "7.0.0",
    "cap": ["wifi_prov", "wifi_scan", "cloud_prov"]
  }
}
```

`ESPDevice.getDeviceCapabilities()` 只覆盖 `prov.cap`，不能代替对
`nuwax.cap.cloud_prov` 的检查。

## 3. 自定义 endpoint

### device-info

请求：Security 2 加密的 UTF-8 `{}`。

关键响应字段：

- `serialNumber`：必须等于 QR/手动输入的 username。
- `capabilities`：设备能力。
- `lastProvisioningOutcome`、`lastProvisioningReason`：失败诊断。
- `wifiDisconnectReason`、`wifiDisconnectReasonName`：原始 Wi-Fi 原因。
- `cloudConfigState`：当前是否有动态云配置。

### vox-config

请求：

```json
{
  "deviceId": "NX-3927-2693-9288",
  "gatewayUrl": "wss://testagent.xspaceagi.com/api/device/ws",
  "deviceSecret": "<bind返回的最新设备密钥>"
}
```

成功响应：

```json
{
  "ok": true,
  "state": "active",
  "deviceId": "NX-3927-2693-9288"
}
```

响应不得包含 `deviceSecret`。APP 必须校验 `ok/state/deviceId`，再发送
Wi-Fi 凭据。

`device-info` 和 `vox-config` 的 characteristic UUID 由 provisioning manager
动态分配。Android 必须读取 characteristic 的 `0x2901 User Description`
descriptor，以 endpoint 名称建立映射；不得硬编码 UUID。

### 三个字段的唯一来源

| 字段 | APP 获取方式 |
| --- | --- |
| `deviceId` | Security 2 后读取 `device-info.serialNumber`，必须与 QR/username 一致 |
| `deviceSecret` | 用已登录用户身份调用现有 `bind` 接口，使用返回的最新值 |
| `gatewayUrl` | APP 当前环境配置或后端配置接口；已观察的 `bind` 响应不返回此字段 |

`gatewayUrl` 对固件仍是动态下发值，但不得把它误写成当前 `bind`
响应字段。任何一个来源失败时，APP 必须停止在 `vox-config` 之前，
不得继续发送 Wi-Fi 凭据。

## 4. 正确时序

```text
用户已登录，加载当前环境 gatewayUrl
扫描冻结 Service UUID + PROV_ 名称
-> BLE GATT 连接
-> proto-ver，要求 sec_ver=2 和全部 capability
-> Security 2(username, PoP)
-> device-info，校验 serialNumber
-> 使用已验证的 serialNumber 调用 bind，取得最新 deviceSecret
-> vox-config(deviceId, gatewayUrl, deviceSecret)
-> 可选 prov-scan（与后续配网严格串行）
-> prov-config set(ssid, password)
-> prov-config apply
-> 轮询官方 prov-config status
-> connected => WIFI_READY
-> 断开 BLE
-> 通过正式后端状态接口独立观察云端在线
```

点击开始配网后必须停止自动 Wi-Fi 扫描，等待当前 `prov-scan` 完成并释放
BLE，再建立新的 Security 2 配网会话。

## 5. 成功与错误

`prov-config connected` / `IP_EVENT_STA_GOT_IP` 即 `WIFI_READY`，是 Wi-Fi
配网成功。正式云端暂时不可达不能把该结果改成 Wi-Fi 失败。

| APP 结果 | 含义 |
| --- | --- |
| `SUCCESS` | 官方状态 connected / WIFI_READY |
| `SECURITY_AUTH_FAILED` | Security 2 username/PoP 不匹配 |
| `WIFI_AUTH_FAILED` | Wi-Fi 密码/认证失败 |
| `NETWORK_NOT_FOUND` | SSID/AP 未找到 |
| `TIMEOUT` | 30 秒凭据观察或 APP 超时 |
| `DEVICE_ID_MISMATCH` | QR/输入 UID 与 device-info 不一致 |
| `PROTOCOL_ERROR` | endpoint/capability/响应格式不符合契约 |
| `BUSY` | BLE 扫描或配网操作互斥冲突 |
| `VOX_CONFIG_INVALID` | 动态云配置字段或设备身份非法 |
| `VOX_CONFIG_PERSIST_FAILED` | 动态配置无法写入 NVS |
| `CLOUD_CONNECT_TIMEOUT` | 已 WIFI_READY，但云端观察超时 |
| `CLOUD_CREDENTIAL_REJECTED` | 正式云拒绝动态密钥，Wi-Fi 仍成功 |
| `INTERNAL_ERROR` | 未映射的固件/协议错误 |

## 6. BLE 生命周期

- 未配网首次启动：自动广播。
- 已配网正常启动：不广播。
- 前 300 秒：仅等待 APP 通过 BLE 配网，不尝试旧 Wi-Fi。
- 300 秒后：BLE 持续广播，同时周期性尝试已保存的旧 Wi-Fi，不再设置总超时。
- APP 建立 GATT 连接后：立即暂停旧 Wi-Fi 尝试，并给 Security 2 握手 15 秒完成认证。
- 每次接收 Wi-Fi 凭据后：提供一个完整的 30 秒连接观察周期。
- 成功后：保留 BLE 15 秒供 APP 读取最终状态，再停止。
- APP 断开或会话失败：恢复 BLE 广播；300 秒边界后同时恢复旧 Wi-Fi 周期重试。
- 旧 Wi-Fi 先恢复或新 Wi-Fi 先提交成功：退出配网并进入正常设备流程。
- 不创建系统蓝牙配对/绑定，安全由 protocomm Security 2 提供。
