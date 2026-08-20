# ESP32-S3 BLE Wi-Fi 配网 APP/固件对接文档

> 当前覆盖说明（2026-07-25）：本文保留 Wi-Fi-only 阶段的详细历史和代码依据，不能替代当前云 onboarding 契约。最终镜像已完成设备、本地真实 Provider 模拟器和测试网关的单次 `WIFI_READY + DEVICE_ONLINE` 真机验收。首轮 APP 联调只使用 `docs/ble-provisioning-delivery-package/01-app-firmware-handoff.md`、`05-provisioning-contract.json`、最新验收矩阵和 `10-final-device-acceptance.md`。

## 2026-07-23 设备侧 Wi-Fi 扫描实现

本地 APP 模拟器已经实际接入冻结 capability `wifi_scan` 和官方 `prov-scan` endpoint。目标设备和 Security 2 身份就绪后自动首次扫描，每次完整扫描结束 15 秒后串行刷新，手动刷新仍保留。扫描请求执行二维码/BLE 目标一致性、Security 2 和 `device-info.serialNumber` 校验；返回列表只包含设备可见 SSID、RSSI、信道与认证模式。自动刷新保留已有 SSID、密码和焦点；用户点击 SSID 后才填入名称、清空旧密码并聚焦密码框。

真机测试由 ESP32-S3 返回 14 至 16 个现场 2.4 GHz 网络；`xspaceagi` 位于信道 4、约 -40 dBm、WPA2/WPA3 混合模式。扫描结果上限现已从 16 提高到 32，新镜像已编译并烧录。扫描与配网共用单操作锁：用户填写密码后暂停自动刷新；点击配网后取消后续扫描，若当前扫描已发出则等待它正常完成、断开并释放 BLE，随后自动执行 `prov-config`。配网成功或失败后扫描均保持停止，手动刷新或重置才恢复。官方 `prov-scan` 是 APP 请求/设备响应，Android Provider 应实现相同串行交接，不期待设备主动推送。此实现不增加私有 endpoint，不修改冻结协议契约。

## 2026-07-23 实机回归勘误

- 已通过裁剪未使用 NimBLE 角色和控制器活动数，修复完整应用启动时的 `BLE_ERR_MEM_CAPACITY (519)`；修复版已重新构建并烧录至 `COM8`。
- 二维码定向扫描、冻结 Service UUID、`PROV_1467C4`、Security 2、capabilities 和 `device-info.serialNumber` 身份校验已在真机通过。
- 本地 Provider Bridge 已保留官方 `prov-config` protobuf 的 `wifi_fail_reason`：认证失败映射 `WIFI_AUTH_FAILED`，AP/SSID 不存在映射 `NETWORK_NOT_FOUND`，未知原因才映射 `INTERNAL_ERROR`。
- 官方状态达到 `connected` 后即按 `IP_EVENT_STA_GOT_IP` 判定 `SUCCESS`；成功后的最终 `device-info` 仅用于诊断，不得覆盖成功结果。
- 已定位指定网络早期认证失败的固件原因：BLE/Wi-Fi 共存时 WPA2/WPA3 transition AP 的 compatibility override 导致关联不稳定。配网阶段关闭该 override 后，两次真机完整配网分别在 `14.051 s` 和 `13.232 s` 返回 `SUCCESS`。
- ESP-IDF 6.0 PSA 硬件 SHA/AES 路径在 Security 2 SRP6a-3072 握手期间曾因内部 DMA 内存不足失败。当前改用相同算法的软件 SHA/AES 实现，Security 2、SRP6a、AES-GCM 和 APP 协议均未降级。
- 配网成功后的主凭据及 Wi-Fi 历史均已保存；已配网正式固件重启后使用保存凭据联网，且不会自动广播 provisioning BLE。

## 1. 文档状态与范围

本文是桌搭本期 Android APP 与 ESP32-S3 固件的 BLE Wi-Fi 配网联调契约。文档结论来自当前固件代码、ESP-IDF 组件源码、`sdkconfig` 和 `dependencies.lock`，不是仅依据 README 推断。

本期范围：

- 芯片：ESP32-S3。
- SDK：ESP-IDF 6.0。
- 配网组件：`espressif/network_provisioning` + `protocomm`。
- BLE Host：NimBLE。
- 传输方式：仅 BLE，不提供 SoftAP fallback。
- 网络：2.4 GHz Wi-Fi STA。
- 安全：protocomm Security 2。
- APP 是 BLE Central，设备是 BLE Peripheral。

当前交付状态为 `FIRMWARE_IMPLEMENTED_FIRST_INTEGRATION`：首次配网和本地物理重新配网入口已进入固件，可以开展 Android Provider 真机联调；逐设备生产密钥和生产二维码链路仍未完成，不能据此直接判定量产就绪。

## 2. 版本冻结

| 项目 | 冻结值 | 代码依据 |
| --- | --- | --- |
| ESP-IDF tag | `v6.0` | 本机构建 SDK Git tag |
| ESP-IDF commit | `662a3be354759d9487bf4b1a629fadb766cb1800` | 本机构建 SDK Git HEAD |
| 配网组件 | `espressif/network_provisioning` | `main/idf_component.yml` |
| 组件版本 | `1.2.4` | `dependencies.lock` |
| 组件仓库 commit | `2de4980640bbe3d2d69473d7251640039e185b92` | 组件 `idf_component.yml.repository_info.commit_sha` |
| 协议层 | ESP-IDF `protocomm` | 固件链接依赖和组件实现 |
| BLE Host | NimBLE | `CONFIG_BT_NIMBLE_ENABLED=y` |
| 密码学动态内存 | 外部 PSRAM | `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`；保障完整 UI/音频固件中的 SRP6a 握手 |
| SHA/AES 实现 | mbedTLS 软件路径 | IDF 6.0 PSA 硬件路径需要内部 DMA 内存；禁用硬件加速不改变 Security 2 算法或报文 |

Android Provider 必须以 ESP-IDF 6.0 / `network_provisioning` 1.2.4 的协议和 protobuf 为兼容基线，不应沿用旧自定义 `FE40` 至 `FE47` GATT 协议。

## 3. BLE 发现契约

| 参数 | 冻结值 |
| --- | --- |
| Primary Service UUID | `0000ffff-0000-1000-8000-00805f9b34fb` |
| 广播名前缀 | `PROV_` |
| 广播名 | `PROV_` + STA MAC 后 3 字节大写十六进制 |
| 示例 | `PROV_A1B2C3` |
| Manufacturer Data | 不使用 |
| 最大连接数 | 1 |
| Preferred ATT MTU | 517，实际值以协商结果为准；Security 2 首包约 415 字节，必须协商到足够 MTU |
| 首次配网总窗口 | 300 秒 |

APP 扫描时同时匹配 Service UUID 和 `PROV_` 前缀。广播名用于发现和人工核对，不作为设备永久身份。

### 3.1 BD 地址策略

当前固件不向 APP 承诺固定的 BLE BD 地址类型或永久地址。APP 不得把 Android 扫描得到的 BLE address 作为账号绑定键、设备序列号或重连后的唯一身份。

稳定业务身份以 Security 2 建立后的 `device-info.serialNumber` 为准；扫描阶段使用 Service UUID、广播名和本次扫描结果定位设备。

### 3.2 GAP 安全边界

本期 provisioning 不要求 BLE GAP pairing 或 bonding，不应触发系统配对弹窗。Wi-Fi 凭据的认证和加密由 protocomm Security 2 提供。该表述不代表 NimBLE 的安全能力从构建中完全移除。

## 4. Security 2、username 与 PoP

Security 2 使用 SRP6a 完成认证和密钥协商，并使用 AES-GCM 保护后续 provisioning 消息。本期不降级到 Security 1，生产版本禁止 Security 0。

### 4.1 开发联调参数

```text
security = 2
username = 设备 UID，例如 NX-A1B2-C3D4-E5F6
password/PoP = Nuwax7-Dev-PoP
```

固件由 eFuse factory MAC、产品域字符串和 SHA-256 派生设备 UID，不直接将原始 MAC 作为 UID。username 必须与生成 SRP6a salt/verifier 时使用的 username 完全一致。

开发 PoP 是共享测试密钥，只允许用于受控联调固件，不能进入生产版本。

### 4.2 生产逐设备方案

生产时每台设备必须：

1. 生成至少 128 bit 随机 PoP。
2. 生成独立随机 salt。
3. 使用该设备 username 和 PoP 生成 SRP6a verifier。
4. 固件仅保存 salt 和 verifier，不保存明文 PoP。
5. 将明文 PoP 仅写入受控产线数据库和设备二维码/标签。
6. 启用 Secure Boot、Flash Encryption，并对 factory NVS 使用 NVS Encryption 或等效保护。
7. 普通 Wi-Fi reset 不删除 Security 2 factory data。

固件预留读取位置：

```text
NVS namespace: nuwax_factory
key: sec2_salt       (blob, current buffer maximum 32 bytes)
key: sec2_verifier   (blob, current buffer maximum 384 bytes)
```

### 4.3 二维码

双方采用以下 Nuwax v1 payload：

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

`security` 是 Nuwax 扩展字段。Android 解析器必须接受该字段；其余字段与 Espressif Security 2 provisioning 语义一致。生产 PoP 不通过 BLE、串口日志或 `device-info` 回传。

APP 扫码后的强制处理顺序为：

1. 校验 `ver=v1`、`transport=ble`、`network=wifi`、`security=2`、`name` 以 `PROV_` 开头且 `username` 非空；开发联调二维码还必须包含非空 `pop`。
2. 仅扫描包含冻结 Primary Service UUID 的广播，并只选择广播名与二维码 `name` 完全一致的设备；不得自动改连附近其他 `PROV_` 设备。
3. 二维码通过后隐藏或锁定 Username/PoP，直接进入 2.4 GHz Wi-Fi SSID/passphrase 输入页；同时定向等待二维码指定设备进入 BLE 配网模式。已配网设备允许用户返回设备菜单点击 `BLE` 并等待重启，等待过程中不得改连其他设备。
4. Security 2 建立后先读取 `device-info`；只有 `serialNumber` 与二维码 `username` 完全一致，才允许发送 Wi-Fi 凭据。不一致统一报 `DEVICE_ID_MISMATCH`。
5. 原始二维码、PoP 和 Wi-Fi passphrase 只能短期保存在操作内存，不得进入日志、埋点、崩溃报告或导出记录。

开发联调固件屏幕二维码包含共享开发 PoP，可执行上述扫码直达流程。生产屏幕二维码不含明文 PoP，必须由产线二维码/受控后端完成凭据交付后，才能实现生产环境的扫码直达；当前状态不得宣称生产屏幕单码配网已完成。

## 5. Endpoint 与 capabilities

### 5.1 Endpoint

| Endpoint | 用途 | APP 要求 |
| --- | --- | --- |
| `proto-ver` | 协议版本、安全版本和能力发现 | 必须调用 |
| `prov-session` | Security 2 会话 | 必须调用 |
| `prov-config` | 写入 SSID/passphrase、apply、状态查询和官方失败原因 | 必须调用，作为配网状态权威来源 |
| `prov-ctrl` | reset/reprovision控制 | 当前APP正常配网流程不调用 |
| `prov-scan` | 设备侧 AP 扫描 | 可选 |
| `device-info` | Nuwax 设备信息及辅助诊断 | Security 2 后调用 |

APP 应使用 Espressif Provider/协议实现发现 endpoint，不自行实现旧 GATT characteristic。官方固定 endpoint 的 characteristic UUID 由组件维护；自定义 `device-info` 是本固件创建的第一个扩展 endpoint。

### 5.2 `proto-ver` 结构

实际返回是分层对象，不是单独的 capabilities 数组。核心结构如下：

```json
{
  "nuwax": {
    "ver": "7.0.0",
    "cap": ["wifi_prov", "wifi_scan"]
  },
  "prov": {
    "ver": "v1.1",
    "sec_ver": 2,
    "cap": ["wifi_prov", "wifi_scan"]
  }
}
```

`prov.ver` 的精确字符串以设备实际响应为准。APP 必须验证 `prov.sec_ver == 2`，并从 `prov.cap` 判断 `wifi_scan`。正常 Security 2 构建不得返回 `no_sec` 或 `no_pop`。

### 5.3 `device-info`

请求 payload 约定为 UTF-8 `{}`。当前固件 handler 会忽略请求内容，APP 仍应发送 `{}`，为后续严格校验保留兼容性。

响应：

```json
{
  "protocolVersion": "1.0",
  "serialNumber": "NX-A1B2-C3D4-E5F6",
  "productModel": "Nuwax AI Partner",
  "hardwareVersion": "CyberVoc V1.2",
  "firmwareVersion": "7.0.0",
  "capabilities": ["wifi_prov", "wifi_scan"],
  "lastProvisioningOutcome": "none",
  "lastProvisioningReason": "-"
}
```

`device-info` 只能在 Security 2 会话建立后访问。不得包含 Wi-Fi 密码、PoP、salt、verifier、会话密钥或原始 MAC。

状态约束：

- `prov-config` 是 APP 判定配网成功或失败的权威来源。
- `device-info.lastProvisioningOutcome` 当前只可靠表达 `none`、`success`、`auth_failed`、`network_not_found`、`timeout`。
- `device-info` 可返回 `internal_error`；APP 仍应以 `prov-config` 和 Provider 调用结果作为配网状态权威来源。

## 6. APP 调用流程

```text
initialize provider
  -> scan(Service UUID + PROV_ prefix)
  -> connect selected peripheral
  -> negotiate MTU (optional optimization)
  -> read proto-ver and validate sec_ver/capabilities
  -> establish Security 2(username + PoP)
  -> read device-info
  -> optional prov-scan
  -> send SSID/passphrase through prov-config
  -> apply through prov-config
  -> poll/wait for official provisioning status
  -> success or classified failure
  -> optionally read device-info while BLE is still available
  -> disconnect and dispose provider
```

建议 APP 页面状态：

```text
scanning
-> connecting_ble
-> establishing_security
-> ready
-> sending_credentials
-> connecting_wifi
-> success | security_auth_failed | wifi_auth_failed
           | network_not_found | timeout | internal_error
```

Security 2 凭据错误发生在 `prov-config` 之前；Wi-Fi 密码错误发生在 apply 之后。APP 应按失败阶段区分二者。

## 7. 成功标准、超时与状态权威

唯一 Wi-Fi 配网成功标准是：

```text
IP_EVENT_STA_GOT_IP
```

这表示 Wi-Fi 认证、关联和 DHCP 已完成。云端登录、时间同步和 WebSocket 在线不属于 Wi-Fi 配网成功条件。

| 时序项 | 当前值 | 行为 |
| --- | --- | --- |
| 首次配网总窗口 | 300 秒 | 从固件启动 provisioning 开始计算 |
| 单次凭据观察超时 | 30 秒 | 从收到本次凭据开始计算 |
| Wi-Fi 连接尝试 | 3 次 | 组件内部尝试次数 |
| 成功后查询窗口 | 15 秒 | 从 `GOT_IP` 开始，供 APP 读取最终状态 |

APP 的状态等待上限建议为 35 秒，以覆盖固件 30 秒观察窗口和 BLE 调度余量。APP 超时不等价于设备永久停止：只要仍在 300 秒主窗口或已接纳尝试的 grace 内，APP 可查询状态；只有主窗口内可以开始新的凭据尝试。

300 秒是新凭据尝试的主窗口。若固件已在截止前收到一组凭据，该次正在进行的尝试最多延长到自身 30 秒观察期结束；已完成失败不会延长窗口，也不能通过连续失败无限续期。APP 不应在窗口最后阶段主动拖延提交，但无需担心已接受的正确凭据被截止时刻提前中止。

## 8. 错误映射

| APP 错误 | 来源/条件 | 当前可靠性 |
| --- | --- | --- |
| `SECURITY_AUTH_FAILED` | Security 2 username/PoP 不匹配 | 可区分，发生在安全会话阶段 |
| `WIFI_AUTH_FAILED` | 官方 `NETWORK_PROV_WIFI_STA_AUTH_ERROR` | 可区分 |
| `NETWORK_NOT_FOUND` | 官方 `NETWORK_PROV_WIFI_STA_AP_NOT_FOUND` | 可区分 |
| `TIMEOUT` | 30 秒未得到终态，或 APP 35 秒保护超时 | 可使用 |
| `INTERNAL_ERROR` | Provider/protobuf/内存/NVS/ESP-IDF 本地错误 | APP 以调用失败及日志判断 |
| `DHCP_FAILED` | 尚无独立官方状态 | 当前不支持，暂映射为 `TIMEOUT` |

不要把所有未知 Wi-Fi disconnect reason 映射为 `NETWORK_NOT_FOUND`。无法可靠分类时应使用 `INTERNAL_ERROR` 或 `TIMEOUT`，并保留原始数值 reason 供日志诊断。

## 9. BLE 生命周期与重试

### 9.1 未配网设备首次启动

- ESP-IDF Wi-Fi provisioned 标志不存在时自动开启 BLE provisioning。
- BLE 配网总窗口为 300 秒。
- 窗口内 BLE 意外断开后设备继续广播，APP 可以重新连接。
- 窗口内认证失败或 AP 不存在后，固件重置官方 Wi-Fi provisioning 状态机，允许再次提交凭据。

### 9.2 300 秒总窗口结束

若截止时没有正在进行的已接纳尝试，固件停止 provisioning 和 BLE，并进入应用错误态；已有尝试先补足最多 30 秒观察期再停止。当前不会自动开启下一轮广播；恢复方式是重启未配网设备或使用本地 `Device > BLE` 配网按钮。APP 必须向用户显示“设备配网窗口已结束，请在设备上重新进入配网模式后重试”。

### 9.3 成功

- `GOT_IP` 后官方状态变为 success。
- 固件等待 15 秒供 APP 查询最终状态。
- 随后显式停止 provisioning，transport 最多再使用约 1 秒完成资源清理，导入 Wi-Fi 凭据并继续正常业务启动。30 秒凭据观察期不参与成功后的清理延时。
- APP 应先以官方 success 更新页面，不得把成功后读取 `device-info` 作为成功的额外必要条件。

### 9.4 已配网设备重新配网

- 正常启动时不广播 provisioning BLE。
- 已实现本地物理入口：打开设备 `Device` 详情页，点击 `Meeting` 下方实际显示为 Wi-Fi 图标加 `BLE` 的按钮。
- 固件显示“BLE provisioning restart”反馈，清除 ESP-IDF Wi-Fi 配置并重启；重启后因未配网标志缺失自动开启 BLE provisioning。
- 不提供无物理确认的远程清除 Wi-Fi 命令。

## 10. 日志与隐私

串口波特率：`115200`。

允许记录：广播名、设备 UID、SSID、阶段、数值 Wi-Fi reason、耗时和重试次数。

禁止记录：Wi-Fi passphrase、PoP、salt、verifier、Security 2 session key 和未脱敏 protobuf payload。

## 11. Android 首轮真机联调输入

```text
Service UUID: 0000ffff-0000-1000-8000-00805f9b34fb
Name prefix: PROV_
Transport: ble
Security: 2
Development PoP: Nuwax7-Dev-PoP
Username: device UID from serial log
Required capability: wifi_prov
Optional capability: wifi_scan
Custom endpoint: device-info
APP status wait: 35 seconds
Firmware total window: 300 seconds
```

首轮测试矩阵：

1. 正确 username、PoP 和 2.4 GHz Wi-Fi，确认以 GOT_IP 成功。
2. 错误 PoP，确认在 Security 2 阶段失败。
3. 错误 Wi-Fi 密码，确认 `WIFI_AUTH_FAILED`。
4. 不存在的 SSID，确认 `NETWORK_NOT_FOUND`。
5. 单次超时后在同一 300 秒窗口重新提交凭据。
6. BLE 断开后重新扫描和连接，并查询当前状态。
7. 验证成功后15秒窗口内 APP 能稳定收到官方 success。
8. 连续执行 20 次配网，记录成功率、阶段耗时和断连原因。
9. 验证 APP 不依赖 BLE address 作为设备永久 ID。

## 12. 交付判定

### 12.1 已具备联调条件

- ESP-IDF 6.0、组件 1.2.4、NimBLE 和 Security 2 已进入构建。
- BLE UUID、名称规则、endpoint 和首次配网流程已实现。
- 官方 success、Wi-Fi 认证失败、AP 未找到及窗口内重试路径已实现。
- ESP-IDF 6.0 工程已完成构建并生成固件镜像。

### 12.2 联调前操作

- 使用未配网测试设备；已配网设备通过本机 `Device > BLE` 清除 ESP-IDF Wi-Fi 配置。需要二维码定向时，应先打开并扫描 `Device QR`，返回设备详情页后再点击 `BLE`。不要为进入配网模式而擦除整个 NVS。
- 烧录当前开发固件并从串口获取设备 UID。
- Android 使用设备 UID 和开发 PoP 建立 Security 2。
- 双方保存固件 commit、APP commit、设备 UID、芯片 revision 和测试日志。

### 12.3 量产前阻塞项

- 实现逐设备 PoP/salt/verifier 的生成、注入、追溯和吊销流程。
- 完成生产二维码/标签及 APP 扫码解析。
- 启用并验证 Secure Boot、Flash Encryption 和 factory NVS 保护。
- 将硬编码 firmware/hardware version 改为构建信息和 factory data。
- 决定是否需要独立 `DHCP_FAILED` 扩展；当前 APP 必须按 timeout 处理。

## 13. 双方冻结确认

| 项目 | 固件状态 | APP 动作 | 验收状态 |
| --- | --- | --- | --- |
| IDF/组件版本 | 已确认并完成构建/烧录 | 按 6.0/1.2.4 对接 | 固件真机已验证，Android 待联调 |
| UUID/广播名 | 已实现 | 双条件扫描 | 固件真机已验证，Android 待联调 |
| Security 2 开发凭据 | 已实现 | UID + 开发 PoP | 本地模拟器 + 固件真机已验证，Android 待联调 |
| `proto-ver`/capabilities | 已实现 | 按分层 JSON 读取 | 本地模拟器 + 固件真机已验证，Android 待联调 |
| `device-info` | 已实现基础字段 | 仅作身份和辅助诊断 | 本地模拟器 + 固件真机已验证，Android 待联调 |
| 官方成功/失败状态 | 主要路径已实现 | `prov-config` 为权威 | success、`NETWORK_NOT_FOUND`、`WIFI_AUTH_FAILED` 固件真机已验证；Android 待联调 |
| 300 秒失败重试窗口 | 已实现 | 超时提示重启 | Wi-Fi 失败后重试和 active-attempt grace 已实现；Android 待联调 |
| 成功后15秒窗口 | 已实现 | 不强制二次读取 | 固件真机已验证，Android 待联调 |
| 已配网设备重新配网 | Device 页 `BLE` 按钮已实现 | 点击本地物理按钮触发 | 固件真机已验证 |
| 二维码定向配网 | 设备屏幕二维码已实现 | 严格校验并锁定同名+UUID设备 | 本地模拟器解析、图片解码和拒绝路径已验证；设备屏幕实扫及Android待联调 |
| 生产逐设备密钥/二维码 | 未完成 | 待产线协议 | 阻塞量产 |

双方完成真机测试并填写 APP commit、固件 commit、测试设备和签字人后，本契约状态方可从 `FIRMWARE_IMPLEMENTED_FIRST_INTEGRATION` 更新为 `INTEGRATION_VERIFIED`。

### 13.1 固件侧真机预验收记录（2026-07-23）

- 开发固件已烧录至 ESP32-S3，广播名为 `PROV_1467C4`，Primary Service UUID 与冻结契约一致。
- 实际协商 ATT MTU 为 517，Security 2 SRP6a 会话建立成功。
- `proto-ver` 返回 `sec_ver=2`、`sec_patch_ver=1` 和 `wifi_prov,wifi_scan`；`device-info` 可加密读写。
- 不存在 SSID 完成 `scan -> connect -> Security 2 -> set/apply -> status`，结果为 `NETWORK_NOT_FOUND`，BLE 在 300 秒窗口内可继续重试。
- 正确 2.4 GHz Wi-Fi 完成两次全流程，分别在 `14.051 s` 和 `13.232 s` 返回官方 `SUCCESS`；固件日志确认 `STA Got IP` 和地址 `192.168.31.3`。
- `GOT_IP` 后 15 秒内仍可扫描 BLE，窗口结束后 `PROV_1467C4` 停止广播。
- 错误 PoP 在 Security 2 阶段返回 `SECURITY_AUTH_FAILED`，未进入 Wi-Fi 配置。
- 错误 Wi-Fi 密码返回 `WIFI_AUTH_FAILED`，失败后设备继续广播并允许同窗口重试。
- 已配网设备点击 `Device > BLE` 后清除 Wi-Fi、重启并恢复 `PROV_1467C4` 广播，全链路已验证。
- 成功后已保存 ESP-IDF STA 配置、应用主凭据和 Wi-Fi 历史；NVS 历史密码键已改为不超过 15 字符的 `_pwd` 后缀。
- 正式自动判断固件重启后可由保存凭据联网，`192.168.31.3` 可达，同时模拟器 BLE 扫描为空，证明正常已配网启动不开放 provisioning BLE。
- `GOT_IP` 后的 15 秒 APP 查询窗口与 1 秒 transport 清理延时已分离，避免此前额外 30 秒阻塞业务/UI 启动。
- 本地模拟器已实现摄像头扫码、二维码图片离线解码、严格字段校验、指定设备定向选择和 `device-info.serialNumber` 二次身份校验；当前设备屏幕到手机摄像头的实扫仍待 Android/真机验收。
- 发现并修复 300 秒截止瞬间提前中止已接收凭据的问题；active-attempt grace 已完成编译和烧录，仍需专项边界计时测试。

证据日志：`runtime-logs/nuwax7-ble-final-credential-test-20260723-172305.log`、`runtime-logs/nuwax7-ble-history-persist-retry-20260723-173653.log`。

以上是本地模拟器作为 Provider 对固件执行的预验收，不等同于 Android Provider 联合验收。真实 BLE 断连恢复、300 秒边界 grace、纯 WPA3-only AP、20 次稳定性测试和 Android Provider 联调仍待完成。
> **归档说明（2026-07-24）**：本文记录 Wi-Fi-only 阶段的历史实现和真机结果，已被 `docs/ble-provisioning-delivery-package/` 中契约 1.1 取代。当前 APP 对接必须包含 `cloud_prov`、`cloud-config`、签名云配置、逐设备 WebSocket 鉴权以及 `WIFI_READY + DEVICE_ONLINE`，不得继续用本文的“GOT_IP 即完整成功”结论。
