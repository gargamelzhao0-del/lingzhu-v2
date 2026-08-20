# ESP32-S3 BLE Wi-Fi 配网 APP/固件对接契约

## 1. 范围

本期只支持 ESP32-S3 通过 BLE 配置 2.4 GHz Wi-Fi STA，并在同一个 Security 2 会话内下发云端连接配置。APP 是 BLE Central，设备是 BLE Peripheral。协议基于 ESP-IDF 6.0、`espressif/network_provisioning` 1.2.4 和 protocomm，不支持 SoftAP fallback，不沿用旧 `FE40` 到 `FE47` 自定义 GATT 协议。

## 2. 版本和发现

| 参数 | 冻结值 |
| --- | --- |
| ESP-IDF | `v6.0` / `662a3be354759d9487bf4b1a629fadb766cb1800` |
| network_provisioning | `1.2.4` / `2de4980640bbe3d2d69473d7251640039e185b92` |
| BLE Host | NimBLE，Peripheral + Broadcaster + GATT Server |
| Primary Service UUID | `0000ffff-0000-1000-8000-00805f9b34fb` |
| 广播名 | `PROV_` + STA MAC 后三字节大写十六进制 |
| 最大连接数 | 1 |
| Preferred ATT MTU | 517，以实际协商值为准 |
| GAP pairing/bonding | 不要求；凭据安全由 protocomm Security 2 提供 |

扫描必须同时匹配 Service UUID 和 `PROV_` 名称格式。BLE address 只能作为本次扫描会话的定位信息，不能保存为永久设备 ID。

## 3. Security 2 和二维码

- 固定使用 Security 2：SRP6a 身份认证和密钥协商，AES-GCM 保护后续报文。
- 不允许降级到 Security 1 或 Security 0。
- username 是设备稳定 UID；Security 2 成功后必须再次检查 `device-info.serialNumber == username`，一致后才允许发送 Wi-Fi 凭据。
- 当前开发版使用机器契约中的开发 PoP；仅限首轮联调。
- 生产必须每设备随机 PoP。设备只保存 salt/verifier，不保存明文 PoP。

二维码 v1：

```json
{
  "ver": "v1",
  "name": "PROV_A1B2C3",
  "transport": "ble",
  "network": "wifi",
  "security": 2,
  "username": "NX-A1B2-C3D4-E5F6",
  "pop": "PER_DEVICE_RANDOM_POP"
}
```

开发屏幕二维码包含开发 PoP。生产屏幕二维码不含 `pop` 时，APP 必须从受控产线标签或云端领用服务取得 PoP 后才能建立 Security 2；单独扫描 identity-only 屏幕二维码不能完成配网。

## 4. endpoints 和 capabilities

`proto-ver` 必须返回 `sec_ver=2`，capabilities 必须包含：

```json
 ["wifi_prov", "wifi_scan", "cloud_prov"]
```

官方 endpoints：

- `prov-session`：Security 2 会话。
- `proto-ver`：协议、安全版本和 capabilities。
- `prov-scan`：设备侧 Wi-Fi 扫描。
- `prov-config`：set config、apply config、get status；它是 Wi-Fi 配网状态权威来源。
- `prov-ctrl`：reset/reprovision 控制，不是状态查询 endpoint；本期不提供 APP 远程清除凭据入口。

自定义 `device-info`：仅在 Security 2 会话内访问，请求为 UTF-8 JSON `{}`，响应字段如下：

```json
{
  "protocolVersion": "1.0",
  "serialNumber": "NX-A1B2-C3D4-E5F6",
  "productModel": "Nuwax AI Partner",
  "hardwareVersion": "CyberVoc V1.2",
  "firmwareVersion": "7.0.0",
  "capabilities": ["wifi_prov", "wifi_scan", "cloud_prov"],
  "lastProvisioningOutcome": "none",
  "lastProvisioningReason": "started",
  "cloudConfigState": "missing"
}
```

`lastProvisioningOutcome` 可为 `none`、`success`、`auth_failed`、`network_not_found`、`timeout`、`internal_error`、`stopped`。该字段用于诊断，不能覆盖 `prov-config` 已返回的明确成功或失败。

自定义 `cloud-config`：仅在 Security 2 会话内访问，必须在 `prov-config set/apply` 前调用。APP 不允许用户手工编辑生产 URL 或密钥，而应从已登录云端 claim API 获取完整签名包：

```json
{
  "op": "set",
  "bundle": {
    "version": 1,
    "deviceId": "NX-A1B2-C3D4-E5F6",
    "cloudUrl": "wss://device.example.com/api/device/ws",
    "credentialId": "cred_xxx",
    "deviceSecret": "per-device-random-secret",
    "bundleVersion": 1,
    "signature": "base64-ecdsa-der"
  }
}
```

固件校验设备 ID、URL、ECDSA P-256/SHA-256 签名和 `bundleVersion` 防回滚后写入 NVS `pending`。`bundleVersion` 必须是 `1..9007199254740991` 范围内的 JSON 安全整数。响应不得回显 `deviceSecret`。

## 5. APP 调用顺序

1. 扫描并同时匹配 Service UUID、广播名前缀和二维码中的完整设备名。
2. 连接 GATT，读取 `proto-ver`，确认 Security 2、`wifi_prov` 和 `cloud_prov`。
3. 用 username + PoP 建立 `prov-session` Security 2。
4. 读取 `device-info`，校验稳定设备 ID。
5. APP 使用登录态和设备 ID 调云端 claim API，取得不可编辑的签名云配置包。
6. 调用 `cloud-config set`；只有 `ok=true,state=pending` 才能继续。
7. 可选但本产品要求支持 `prov-scan`，显示设备实际可见的 2.4 GHz SSID。
8. 用户选 SSID、输入密码后停止周期扫描，等待当前 `prov-scan` 完成并释放 BLE 操作。
9. 通过 `prov-config` 依次 set SSID/passphrase、apply、轮询 status。
10. `connected` 即 `WIFI_READY`。APP 结束 BLE 后通过云端 HTTPS 状态接口等待同一 `credentialId` 的 `DEVICE_ONLINE`，推荐上限 60 到 90 秒。`CLOUD_CONFIG_COMMIT_PENDING` 只是中间状态。
11. 只有 `WIFI_READY + DEVICE_ONLINE` 才向用户报告设备绑定和配网全部成功。

`prov-scan` 是请求/响应，不是设备主动推送。APP 的周期刷新建议在一次扫描完成后等待约 15 秒；开始配网后不得与扫描并发。

## 6. 成功和错误

| 条件 | APP 结果 |
| --- | --- |
| `prov-config=connected` / `IP_EVENT_STA_GOT_IP` | 进入 `WIFI_READY`，尚非最终成功 |
| 云端确认同一 `deviceId + credentialId` 在线 | `SUCCESS` / `DEVICE_ONLINE` |
| 云鉴权成功但设备尚未确认 NVS active 提交 | 继续等待 / `CLOUD_CONFIG_COMMIT_PENDING` |
| Security 2 凭据不匹配 | `SECURITY_AUTH_FAILED` |
| `NETWORK_PROV_WIFI_STA_AUTH_ERROR` | `WIFI_AUTH_FAILED` |
| `NETWORK_PROV_WIFI_STA_AP_NOT_FOUND` | `NETWORK_NOT_FOUND` |
| 30 秒固件观察窗口或 35 秒 APP 保护超时 | `TIMEOUT` |
| 未知 Wi-Fi reason、protocomm、本地 ESP_ERR | `INTERNAL_ERROR` |
| 未先成功下发签名云配置 | `CLOUD_CONFIG_REQUIRED` |
| `device-info.serialNumber` 不匹配 | `DEVICE_ID_MISMATCH`，不得发送 Wi-Fi 凭据 |
| 未发现二维码指定设备 | `DEVICE_NOT_FOUND` |
| 安全版本/capability/endpoint 不符 | `PROTOCOL_ERROR` |
| 另一 BLE 操作占用 | `BUSY`，串行重试 |
| 云配置字段、设备 ID 或 URL 非法 | `CLOUD_CONFIG_INVALID` |
| 云配置签名错误 | `CLOUD_SIGNATURE_INVALID` |
| `bundleVersion` 回滚或同版本内容冲突 | `CLOUD_CONFIG_ROLLBACK` |
| WebSocket 101 前逐设备密钥被拒绝 | `CLOUD_CREDENTIAL_REJECTED` |
| Wi-Fi 已就绪但 60 到 90 秒仍未上线 | `CLOUD_CONNECT_TIMEOUT` |

屏幕 Wi-Fi 图标不是协议成功条件。Wi-Fi 配置子流程以 `GOT_IP` 为准，完整 onboarding 以云端 `DEVICE_ONLINE` 为准。

## 7. BLE 生命周期和重配网

- 未配网首次启动：自动广播，窗口 300 秒。
- 已配网正常启动：不广播 provisioning BLE。
- 失败：在 300 秒窗口内保持服务，调用官方 Wi-Fi 状态机 reset 后允许重试。
- 截止前已接收凭据：允许当前尝试最多再观察 30 秒；已完成失败不会无限延长窗口。
- 成功：`GOT_IP` 后保留 15 秒查询窗口，随后约 1 秒完成 transport cleanup。
- 已配网设备切换 Wi-Fi：本机 `Device > BLE` 按钮先把当前 ESP-IDF STA 配置写入回滚记录，再重启进入 BLE 配网，不会提前擦除旧网络。新网络到达 GOT_IP 后清除回滚记录；失败或超时则恢复旧网络。已有 `active` 云配置始终保留。

## 8. 设备绑定边界

APP 和云端使用 `device-info.serialNumber` 作为设备业务身份。广播名和 BLE address 都不是永久身份。BLE PoP 只证明 APP 有权向附近设备下发敏感配置；`deviceSecret` 只用于设备到云端的逐设备认证；用户登录 token/claim 权限用于账户所有权，三者不得互换。当前测试网关实现首轮 claim 和上线查询；生产云仍需实现账户绑定、解绑、所有权转移、凭据吊销和审计。
