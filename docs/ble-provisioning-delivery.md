# BLE 配网对接交付说明

> 当前覆盖说明（2026-07-25）：本文是 Wi-Fi-only 阶段的过程归档，不能作为当前云 onboarding 验收结论。最终镜像 SHA-256 为 `46B85B6769639B7C8B6B5FFC3D66D97B4F5367D2DF0BFF5D3AEA5C6C940CE55A`；设备、本地真实 Provider 模拟器和测试网关已在同一次流程完成 `WIFI_READY + DEVICE_ONLINE`。提交 APP 时只以 `docs/ble-provisioning-delivery-package/` 为权威入口。

## 设备侧 Wi-Fi 扫描交互

本地 APP 模拟器已接入固件现有 `wifi_scan` capability 和官方 `prov-scan` endpoint。用户锁定二维码指定设备后，模拟器在 1.2 秒输入防抖后自动首次扫描，此后每次完整扫描结束 15 秒再串行刷新；手动“扫描设备 Wi-Fi”仍可立即刷新。Bridge 每次都会先完成 Security 2、capability 和 `device-info.serialNumber` 校验，再返回 ESP32-S3 实际扫描到的 SSID、RSSI、信道和认证模式。

列表位于 SSID/密码输入框下方。自动刷新不会改动现有 SSID、密码或焦点；点击网络名称才会填入 SSID、清空可能残留的旧密码并聚焦密码框，密码仍需用户输入。用户开始输入密码后暂停下一次自动刷新；若点击“开始真实配网”时已有扫描请求正在执行，模拟器会立即取消后续扫描，等待当前官方 `prov-scan` 完成并释放 BLE，然后自动开始配网。扫描和配网不会并发，因此不会争用同一 BLE 操作锁产生 `BUSY`。成功或失败后扫描均保持停止，用户可手动刷新或重置后再次扫描。列表不使用电脑 Wi-Fi 扫描结果，不返回 BSSID，不记录 PoP 或 Wi-Fi 密码。

官方 `prov-scan` 是 APP 请求/设备响应，不提供设备主动推送。模拟器和 Android Provider 必须分别发起每次扫描请求；该实现不增加固件私有 endpoint，也不改变冻结契约。固件扫描结果上限已从 16 提高到 32，32 条上限的新镜像已编译并烧录至 `COM8`。

模拟器 Bridge 只绑定 `127.0.0.1`，静态资源只允许访问 `docs/provision_simulator`，契约通过 `/api/contract` 返回；不会把工程根目录的 `sdkconfig`、源代码、构建文件或运行日志作为 HTTP 静态文件暴露。设备发现、设备侧 Wi-Fi 扫描和配网共用一把后端 BLE 操作锁，发现结果不返回 BLE address。

2026-07-23 真机验证中，设备扫描到 14 至 16 个网络，并识别到 `xspaceagi` 位于 2.4 GHz 信道 4、约 -40 dBm、WPA2/WPA3 混合模式。固件随后修正过渡网络关联策略，两次使用正确凭据的完整配网均取得 IP 并返回 `SUCCESS`；此前 `WIFI_AUTH_FAILED` 已确认不是设备看不到该 SSID。

## 2026-07-23 QR 定向配网回归

已烧录内存占用修复版固件。设备可以正常广播 `PROV_1467C4`，不再出现 NimBLE `BLE_ERR_MEM_CAPACITY (519)`；二维码定向扫描、BLE GATT、`proto-ver`、Security 2、capability 和 `device-info` 身份校验均通过，且 Wi-Fi 凭据只在身份一致后发送。

本地 Bridge 已修正官方 Wi-Fi 失败原因解析：认证失败返回 `WIFI_AUTH_FAILED`，网络不存在返回 `NETWORK_NOT_FOUND`，失败后的辅助 endpoint 超时不会覆盖官方结果。官方状态一旦到达 `connected` 即按 `IP_EVENT_STA_GOT_IP` 判定 `SUCCESS`，不依赖后续可选的 `device-info` 读取。

后续由 ESP32-S3 自身执行的 `prov-scan` 已确认目标 2.4 GHz AP 可见。二维码解析/定向锁定、Security 2、身份校验、设备扫描、凭据下发和官方状态读取均已走通；最终两次正确凭据配网分别在 `14.051 s` 和 `13.232 s` 返回 `SUCCESS`。早期“电脑未发现 2.4 GHz”只代表电脑侧扫描状态，不能用来判断设备侧可见性。

## 兼容性和启动延迟修复

- `main/nuwax_provision_new/nuwax_provision_main.c` 在 BLE 与 Wi-Fi 共存的配网阶段对 WPA2/WPA3 transition AP 关闭 ESP-IDF WPA3 compatibility override，使本次关联稳定走 WPA2；不改变 Security 2，也不表示支持 5 GHz。纯 WPA3-only AP 尚未列为首轮联调验收项。
- ESP-IDF 6.0 的 PSA 硬件 SHA/AES 路径在 SRP6a-3072 握手时需要内部 DMA 内存，完整 UI/音频固件中曾分配失败。当前关闭 `CONFIG_MBEDTLS_HARDWARE_SHA` 和 `CONFIG_MBEDTLS_HARDWARE_AES`，使用相同 SHA/AES-GCM 算法的软件实现；协议、安全版本和 APP 报文均不变。
- Wi-Fi 历史密码 NVS 键后缀由 `_password` 改为 `_pwd`，避免索引键超过 ESP-IDF NVS 的 15 字符限制；配网成功后已实测同时保存主凭据和历史记录。
- `GOT_IP` 后仍保留 15 秒官方状态查询窗口；随后 provisioning transport 的资源清理延时为 1 秒。30 秒仅用于单次凭据结果观察，不再被误用为停止清理延时，因此 APP 收到 success 后约 16 秒固件即可继续完整业务启动。

完整的模块边界、时序、状态机、Security 2 生命周期和测试矩阵见
[`ble-provisioning-architecture.md`](ble-provisioning-architecture.md)。本文保留为 APP Provider 真机联调的快速入口。

## 交付范围

本次交付将 `main/nuwax_provision_new` 从自定义 GATT 配网改为官方
`espressif/network_provisioning 1.2.4`。固件使用 ESP-IDF `v6.0`
(`662a3be354759d9487bf4b1a629fadb766cb1800`)、NimBLE 和 protocomm
Security 2，不实现 SoftAP fallback。

当前契约状态为 `FIRMWARE_IMPLEMENTED_FIRST_INTEGRATION`。固件侧真机预验收已完成，Android Provider 联合验收和量产密钥链路尚未完成。

## APP 对接关键值

- Service UUID：`0000ffff-0000-1000-8000-00805f9b34fb`
- 广播名：`PROV_` + STA MAC 后三字节大写十六进制
- 能力：`wifi_prov`、`wifi_scan`
- Security：2；开发 PoP 为 `Nuwax7-Dev-PoP`
- 自定义 endpoint：`device-info`
- 成功：收到 `IP_EVENT_STA_GOT_IP`
- 失败：`AuthError=0`、`WifiNetworkNotFound=1`
- 单次凭据观察：30 秒；APP 建议状态等待上限：35 秒
- BLE 总窗口：300 秒；成功后查询窗口：15 秒
- 截止前已接收的凭据：允许当前尝试补足最多 30 秒观察期，失败完成后不再延长

APP 不应查找或写入旧的 `FE40~FE47` 特征，也不应假设固定的
`prov-config` 特征 UUID；应通过官方 `proto-ver` 和服务描述完成端点发现。
扫描时必须同时匹配冻结 Service UUID 和 `PROV_` 广播名前缀，BLE address 只用于本次扫描定位，不能作为永久设备 ID。

## 真机联调顺序

1. 未配网设备首次启动会自动进入 300 秒 BLE 配网窗口。已配网设备先在本机 `Device` 详情页按需打开 `Device QR` 供 APP 扫描，返回后点击 `Meeting` 下方实际显示为 Wi-Fi 图标加 `BLE` 的配网按钮。固件调用 `esp_wifi_restore()` 后重启；不要擦除整个 NVS。
2. APP 同时匹配 Service UUID 和 `PROV_` 广播名前缀，选择设备并连接。
3. 读取 `proto-ver`，确认 `prov.sec_ver=2` 且 capabilities 包含 `wifi_prov`。
4. 使用 Security 2 用户名（设备 UID）和开发 PoP 建立 protocomm session；生产身份二维码若不含 PoP，则从受控产线凭据中补录后再建会话。
5. 在安全会话内读取 `device-info`，可选执行 `prov-scan`。
6. 通过 `prov-config` 执行 set SSID/passphrase、apply 和 status 查询；`prov-ctrl` 仅用于 reset/reprovision 控制。
7. 收到官方 connected/GOT_IP 后立即进入成功状态，可选在 15 秒窗口内读取最终 `device-info`，随后断开并释放 Provider。

错误映射：

- Security 2 凭据不匹配：`SECURITY_AUTH_FAILED`。
- `NETWORK_PROV_WIFI_STA_AUTH_ERROR`：`WIFI_AUTH_FAILED`。
- `NETWORK_PROV_WIFI_STA_AP_NOT_FOUND`：`NETWORK_NOT_FOUND`。
- 30 秒凭据观察或 APP 35 秒保护超时：`TIMEOUT`。
- 未知 Wi-Fi reason、protocomm 或本地 `ESP_ERR`：`INTERNAL_ERROR`。

## 生产切换前置条件

当前代码的开发模式会现场生成 salt/verifier，方便 APP 真机联调，但不能
作为生产凭据方案。生产必须：

- 关闭 `CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE`。
- 向受保护的 `nuwax_factory` NVS 写入每设备 `sec2_salt` 和 `sec2_verifier`。
- 由产线系统生成每设备 username/PoP/二维码；固件不保存 PoP 明文。
- 启用 Secure Boot、Flash Encryption 和 NVS Encryption/受保护 factory data。
- 将芯片 revision、物料号和三台联调设备编号补入正式契约。

## 已验证内容

2026-07-23 已完成 ESP-IDF 6.0 完整编译、`COM8` 烧录和镜像哈希校验，生成
`build/NuwaxAIPartner.bin`；应用大小 `0x322d30`，应用分区剩余约 34%。

本地模拟器作为 Provider 与真机已验证：冻结 Service UUID、`PROV_1467C4` 广播、ATT MTU 517、`sec_patch_ver=1`、Security 2、capabilities、`device-info`、错误 PoP、错误 Wi-Fi 密码、不存在 SSID、失败后重试、正确 Wi-Fi `SUCCESS`、本地物理重新配网入口和成功后 15 秒 BLE 窗口。正确凭据两次用时分别为 `14.051 s`、`13.232 s`，设备取得 `192.168.31.3`；主凭据和 Wi-Fi 历史保存成功。正式自动判断固件重启后可用保存凭据联网，且 `/api/devices` 扫描不到 `PROV_1467C4`，证明已配网正常启动不会开放 provisioning BLE。网页 Mock 模式的 8 个状态机场景均通过。

证据日志：`runtime-logs/nuwax7-ble-final-credential-test-20260723-172305.log`、`runtime-logs/nuwax7-ble-history-persist-retry-20260723-173653.log`。

仍需现场验收：Android Provider、真实 BLE 断连恢复、300 秒边界 active-attempt grace 和连续 20 次稳定性测试。完成前不得把契约状态改为 `INTEGRATION_VERIFIED`。
> **归档说明（2026-07-24）**：本文保存旧交付过程，不再是当前对接依据。请只提交 `docs/ble-provisioning-delivery-package/`；其 1.1 契约新增 `cloud-config` 和逐设备云鉴权。
