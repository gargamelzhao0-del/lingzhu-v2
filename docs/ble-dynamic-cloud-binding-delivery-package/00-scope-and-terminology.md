# 范围与术语

## 目标

设备固件不再编译固定服务端 WebSocket URL、共享 Token 或逐设备 `deviceSecret`。APP 在确认近端设备身份后，从已登录云端领取该设备专用的签名配置包，通过 BLE Security 2 下发；设备验签并保存，取得 Wi-Fi 后使用该配置连接服务端。

最终成功不是“Wi-Fi 图标亮起”，而是同一次 onboarding 同时满足：

```text
WIFI_READY + DEVICE_ONLINE
```

## 六类容易混淆的信息

| 名称 | 谁使用 | 是否发给设备 | 作用 |
| --- | --- | --- | --- |
| `apiBaseUrl` | APP | 否 | APP 调用 claim/status 等 HTTPS API 的环境地址 |
| APP 用户登录 Token | APP -> 云 API | 否 | 证明当前用户身份和绑定权限 |
| PoP | APP -> 近端设备 | 仅用于 Security 2 | 证明 APP 有权配置眼前设备，建立 BLE 加密会话 |
| `cloudUrl` | 设备 | 是 | 设备后续连接的 WebSocket 地址，如 `wss://.../api/device/ws` |
| `credentialId` | APP、设备、云端 | 是 | 标识一版逐设备云凭据，用于匹配提交和在线状态 |
| `deviceSecret` | 设备 -> 云端 | 是 | WebSocket Upgrade 前逐设备鉴权；不是 PoP，也不是用户 Token |

此外还有云配置签名密钥：私钥只在云端，公钥是固件信任锚。`cloudUrl` 和 `deviceSecret` 可以每次 claim 动态变化；签名公钥目前不是动态配置，轮换它需要固件支持多公钥/密钥版本或更新固件。

## 设备身份

业务设备 ID 使用 `device-info.serialNumber`，当前格式为：

```text
NX-3927-2693-9288
```

二维码 `username`、Security 2 username、`device-info.serialNumber`、签名包 `deviceId` 和 WebSocket `X-Device-ID` 必须一致。以下信息不能作为永久业务身份：

- BLE address：可能变化，只用于本次扫描连接。
- 广播名 `PROV_1467C4`：只有 24 bit MAC 后缀，用于附近设备定位。
- PoP：近端配置授权凭据，不是设备 ID。
- `credentialId`：云凭据版本标识，可以轮换。

## 技术基线

| 项目 | 冻结值 |
| --- | --- |
| 芯片 | ESP32-S3 |
| ESP-IDF | `v6.0` / `662a3be354759d9487bf4b1a629fadb766cb1800` |
| network_provisioning | `1.2.4` / `2de4980640bbe3d2d69473d7251640039e185b92` |
| BLE Host | NimBLE |
| 系统 BLE 配对/绑定 | 不需要，不创建 OS bond；应用层由 Security 2 保护 |
| Service UUID | `0000ffff-0000-1000-8000-00805f9b34fb` |
| 广播名 | `PROV_` + STA MAC 后三字节大写十六进制 |
| 安全 | Security 2：SRP6a + AES-GCM，不降级 |
| 配网 | 仅 BLE，不支持 SoftAP fallback |
| `proto-ver.prov.cap` | `wifi_prov`、`wifi_scan` |
| `proto-ver.nuwax.cap` | `wifi_prov`、`wifi_scan`、`cloud_prov` |
| 自定义 endpoint | `device-info`、`cloud-config` |

开发手动配对时，PoP 取机器契约 `security.developmentPoP`（当前测试值 `Nuwax7-Dev-PoP`）或开发二维码中的 `pop`。它是共享开发凭据，只能用于本期测试固件；正式 APP 不得把它作为 fallback。生产 identity-only 屏幕二维码不含 PoP，APP 必须从逐设备产线标签或已鉴权凭据服务取得。

## 当前交付边界

已经完成：设备、本地模拟器、测试网关单次真实端到端成功；动态 URL/密钥验签、pending/active 提交和逐设备 WebSocket 鉴权均有代码和真机证据。这里完成的是“设备与逐设备云 credential 绑定”，不是“正式用户账户所有权绑定”。

尚未完成：Android Provider 联合验收、正式账户绑定 API、20 次稳定性、断电/故障注入、逐设备量产 PoP、Secure Boot、Flash Encryption 和 NVS Encryption。
