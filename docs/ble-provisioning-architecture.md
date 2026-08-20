# ESP32-S3 BLE 配网架构与实施说明

## 1. 目标与边界

本期桌搭 Wi-Fi 配网采用 BLE 单通道：ESP32-S3 作为 BLE Peripheral，Android 作为 BLE Central；SoftAP 不参与本期实现和验收。

技术基线固定为：

- ESP-IDF `v6.0`，commit `662a3be354759d9487bf4b1a629fadb766cb1800`
- `espressif/network_provisioning` `1.2.4`
- ESP-IDF `protocomm`
- NimBLE host
- protocomm Security 2

BLE 只负责安全地把 Wi-Fi 配置交给设备并提供配网状态查询。云端 WebSocket 上线属于配网完成后的应用层动作，不得成为 Wi-Fi 配网成功的前置条件。

## 2. 固件分层

```text
app_main
  |
  +-- nuwax_provision_main       产品生命周期适配层
  |     +-- network_provisioning manager
  |     +-- network_prov_scheme_ble
  |     +-- protocomm Security 2
  |     +-- device-info endpoint
  |
  +-- wifi_manager                现有 STA 状态与应用 Wi-Fi 历史
  |
  +-- session_protocol/ws         配网完成后的云端业务通道
  |
  +-- LVGL/touch                  配网入口、状态展示、重新配网确认
```

### 所有权规则

| 阶段 | Wi-Fi 控制者 | BLE 控制者 | 允许的应用动作 |
| --- | --- | --- | --- |
| 启动检测 | `network_provisioning` 查询标志 | 未启动 | 判断是否进入配网 |
| BLE 配网中 | `network_provisioning` 独占 | `network_prov_scheme_ble` | 扫描、set/apply、get status |
| DHCP 成功 | provisioning manager | 保留短窗口 | 查询最终状态 |
| 配网结束 | `wifi_manager`/应用主循环 | 释放 | WebSocket、普通 Wi-Fi 重连 |
| 已配网运行 | `wifi_manager` | 不广播 | UI 手动切换网络 |

不能在 provisioning manager 运行期间同时调用现有 `nuwax_wifi_connect()`、fallback watcher 或普通 Wi-Fi 重连队列，否则两个状态机都会响应 `WIFI_EVENT_STA_DISCONNECTED`。

## 3. 启动时序

```text
nvs_flash_init
  -> nuwax_wifi_init
  -> nuwax_provision_main_init
  -> network_prov_mgr_is_wifi_provisioned
       |-- true  -> stop/deinit manager -> normal Wi-Fi/WebSocket path
       |-- false -> start BLE provisioning
                    -> wait up to 300 s for provisioning terminal result
                    -> import ESP-IDF STA config to wifi_manager history
                    -> stop/deinit manager
                    -> normal Wi-Fi/WebSocket path
```

当前固件在首次未配网时会阻塞主启动流程，最长 300 秒。这是产品策略，不是 BLE 协议要求；如果产品希望设备在等待配网时仍显示完整 UI，应把等待逻辑迁移到独立 provisioning task，并保留 UI/task 调度。

## 4. BLE 广播与 GATT

### 广播

- Service UUID：`0000ffff-0000-1000-8000-00805f9b34fb`
- Device name：`PROV_` + STA MAC 的第 4、5、6 字节，例如 `PROV_A1B2C3`
- 广播窗口：300 秒
- 单连接：`BT_NIMBLE_MAX_CONNECTIONS=1`
- preferred ATT MTU：517，实际由 Android 与设备协商
- manufacturer data：本期不依赖

Service UUID 在代码中显式调用 `network_prov_scheme_ble_set_service_uuid()` 设置，不能依赖组件内置的另一个默认 UUID。

### 官方 endpoint

| Endpoint | 作用 | 数据格式 |
| --- | --- | --- |
| `proto-ver` | 查询 provisioning manager 版本、安全版本和 capabilities | 官方 JSON |
| `prov-session` | Security 2 SRP6a 握手 | protocomm protobuf |
| `prov-config` | 设置 SSID/passphrase、apply、get status | 官方 protobuf；配网状态权威来源 |
| `prov-ctrl` | reset/reprovision 控制 | 官方 protobuf；普通配网不调用 |
| `prov-scan` | 设备侧 Wi-Fi 扫描 | 官方 protobuf |
| `device-info` | 产品和最近一次配网状态 | UTF-8 JSON |

APP 不应依赖固定 characteristic UUID，也不应向 `FE40~FE47` 自定义特征写入 Wi-Fi 密码。

## 5. APP 推荐时序

```text
initialize
  -> startScan(serviceUuid = FFFF, namePrefix = PROV_)
  -> connect
  -> negotiate MTU
  -> get proto-ver and verify sec_ver/capabilities
  -> establish Security 2 session
  -> get device-info
  -> optional prov-scan
  -> prov-config(set SSID/passphrase)
  -> prov-config(apply)
  -> poll prov-config(get status)
  -> success/failure
  -> read device-info once more
  -> disconnect/dispose
```

`device-info` 应在发送 Wi-Fi 配置之前读取，便于 APP 保存设备 UID、型号和协议版本。Security 2 未建立前，不能发送 Wi-Fi 凭据，也不能把 Wi-Fi 密码放入日志或错误上报。

## 6. Security 2 设计

### 6.1 开发模式

当前开发配置：

```text
CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE=y
CONFIG_NUWAX_PROVISION_SEC2_DEV_POP="Nuwax7-Dev-PoP"
```

设备用 UID 作为 username，使用固定开发 PoP 调用 `esp_srp_gen_salt_verifier()` 生成本次会话使用的 salt/verifier。该模式只用于 APP Provider 联调，不能发布生产固件。

### 6.2 生产模式

产线为每台设备生成：

1. `username`：设备 UID 或序列号。
2. 随机 PoP：至少 128 bit 熵，使用 Base32/二维码友好字符集。
3. 随机 salt：建议 16 字节或更高。
4. SRP6a verifier：使用与 APP 相同的 username/PoP 算法生成。

设备只保存：

- `nuwax_factory/sec2_salt`
- `nuwax_factory/sec2_verifier`

设备不得保存 PoP 明文。二维码/标签保存 username 和 PoP。普通 Wi-Fi 重置不得擦除 `nuwax_factory`，只有受控 factory reset 才能更换整套身份材料。

### 6.3 二维码

开发联调 payload：

```json
{"ver":"v1","name":"PROV_A1B2C3","transport":"ble","network":"wifi","security":2,"username":"NX-A1B2-C3D4E5","pop":"Nuwax7-Dev-PoP"}
```

开发 payload 已通过 `Device > Device QR` 渲染到 LCD，模拟器支持摄像头或图片解析、定向锁定和 `device-info.serialNumber` 二次校验。生产 payload 必须由产线系统生成；生产固件屏幕二维码不含明文 PoP，因此产线标签/受控后端的生产凭据交付仍是量产阻塞项。

完整 UI/音频固件在 IDF 6.0 PSA 硬件 SHA/AES 路径中曾因 SRP6a 所需内部 DMA context 分配失败。当前关闭硬件 SHA/AES 并使用 mbedTLS 软件实现，SRP6a、AES-GCM、Security 2 版本和线协议不变。

## 7. Wi-Fi 状态机

```text
IDLE
  -> CREDENTIALS_RECEIVED
  -> CONNECTING
  -> GOT_IP (success)
  -> AUTH_FAILED
  -> AP_NOT_FOUND
  -> STATUS_TIMEOUT
```

官方 manager 原生错误只有：

- `AuthError=0`
- `WifiNetworkNotFound=1`

成功以 `IP_EVENT_STA_GOT_IP` 判定。DHCP 没有独立的官方错误码；30 秒内未 GOT_IP 时当前固件返回 timeout。若产品必须单独显示 DHCP failed，应增加自定义 last-status 字段或 endpoint，不能将其伪装成 AP_NOT_FOUND。

BLE/Wi-Fi 共存配网期间，固件对 WPA2/WPA3 transition AP 关闭 IDF WPA3 compatibility override，使关联走 WPA2 路径；`xspaceagi` 信道 4 已连续两次成功。正常已配网启动不运行 provisioning BLE，保存凭据仍由普通 STA 路径连接。纯 WPA3-only AP 尚未纳入首轮验收。

## 8. 成功、失败和断连策略

### 成功

1. 收到 GOT_IP。
2. 状态查询返回 connected/IP。
3. BLE 保留 15 秒供 APP 读取最终状态。
4. 固件停止 provisioning manager，并在约 1 秒内完成 transport cleanup。
5. 将 ESP-IDF STA 配置同步到 `wifi_manager` 的历史网络。
6. 进入正常 WebSocket 启动流程。

### Wi-Fi 失败

1. 保留 BLE session 和 provisioning service。
2. `get status` 返回最近错误。
3. 调用官方 reset Wi-Fi state，允许 APP 在同一连接重新提交凭据。
4. 300 秒主窗口到期且没有正在进行的已接纳尝试时停止服务。

若一组凭据已在 300 秒截止前到达，固件只允许该次正在进行的尝试补足自身 30 秒观察期；失败事件或观察超时会清除 active-attempt 标记，不能通过重复失败无限延长 BLE 窗口。

### BLE 意外断开

失败状态下应继续广播，APP 重连后先执行 `device-info`/`get status`。成功状态从 `GOT_IP` 起保留 15 秒；APP 收到官方 success 后应立即完成状态收敛，不能依赖该窗口执行必需的二次读取。

### 已配网设备重新配网

正常启动不广播 BLE。重新配网必须由本地物理操作触发，并且要有 UI 明确反馈。需要二维码定向时先打开 `Device QR` 供 APP 扫描，返回后点击 `Meeting` 下方实际显示为 Wi-Fi 图标加 `BLE` 的按钮；固件显示重启提示、调用 `esp_wifi_restore()` 只清除 ESP-IDF Wi-Fi 配置并重启，随后进入 300 秒 BLE 配网窗口。按钮触摸、清除 Wi-Fi、重启和恢复广播已于 2026-07-23 完成真机验证。

## 9. `device-info` 契约

请求：

```json
{}
```

响应：

```json
{
  "protocolVersion": "1.0",
  "serialNumber": "NX-A1B2-C3D4E5",
  "productModel": "Nuwax AI Partner",
  "hardwareVersion": "CyberVoc V1.2",
  "firmwareVersion": "7.0.0",
  "capabilities": ["wifi_prov", "wifi_scan"],
  "lastProvisioningOutcome": "success",
  "lastProvisioningReason": "dhcp_success"
}
```

字段不能包含 Wi-Fi 密码、PoP、salt、verifier 或原始 MAC。

## 10. 测试与验收矩阵

| 场景 | 预期结果 |
| --- | --- |
| 新设备启动 | 广播 `PROV_XXXXXX`，APP 可发现 |
| 正确 Security 2 | session 建立成功 |
| 错误 PoP | Security session 失败，不能进入配置 |
| 正确 2.4 GHz Wi-Fi | GOT_IP，返回 success |
| 错误密码 | `AuthError=0` |
| AP 不存在 | `WifiNetworkNotFound=1` |
| DHCP 超时 | timeout，不伪装成 AP_NOT_FOUND |
| Wi-Fi 配置后 BLE 断开 | 重连后可查询最近状态 |
| 配网成功后重启 | 不自动广播，使用已保存 STA |
| 失败后重新提交 | 同一 BLE 会话可 retry |
| 广播超时 | 服务停止，正常应用不启动云端连接 |
| 连续 20 次配网 | 统计成功率、耗时、失败阶段 |

## 11. 生产前检查清单

- [ ] 关闭 `CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE`。
- [ ] 完成每设备 salt/verifier 产线注入。
- [ ] 完成每设备二维码/标签生成和扫描验证。
- [ ] 启用 Secure Boot、Flash Encryption、NVS Encryption 或等效 factory data 保护。
- [x] 真机验收 `Device` 页 `BLE` 按钮、LCD 反馈、清除 Wi-Fi 和重启广播全链路。
- [x] 成功后的 BLE 查询窗口冻结为 15 秒。
- [ ] 决定是否需要独立 `DHCP_FAILED` 自定义状态。
- [ ] Android Provider 完成真实 Security 2、MTU、断连和错误注入测试。
- [ ] 产出三台可区分设备的串口日志、ELF/MAP 和完整 sdkconfig。
> **归档说明（2026-07-24）**：本文描述旧 Wi-Fi-only 架构。当前架构见 `docs/ble-provisioning-delivery-package/03-architecture.md` 和 `09-cloud-onboarding.md`。
