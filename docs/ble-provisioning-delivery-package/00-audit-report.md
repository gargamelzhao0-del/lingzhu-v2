# BLE 配网交付前审计报告

审计日期：2026-07-25。修复后结论：契约 1.2 源码和模拟器满足首轮 Android Provider 真机联调要求；设备 NVS 提交回执、BLE endpoint 关闭边界、损坏槽位隔离、失败 pending 清理、原始 Wi-Fi reason、状态同步、重新配网回滚，以及 PSRAM 任务触发 Flash/NVS cache assertion 的问题均已修复。最终镜像已烧录，并以 ESP32-S3 + 本地真实 Provider 模拟器 + 测试网关完成一次 `WIFI_READY + DEVICE_ONLINE` 三端真机验收。由于 Android Provider、连续稳定性和量产安全项仍待验证，契约状态仍不能标记为 `INTEGRATION_VERIFIED` 或生产就绪。

## 审计范围

- 固件：`main/nuwax_provision_new`、Wi-Fi 管理、启动集成、Device 菜单、设备二维码、稳定设备 ID、签名云配置 NVS 双槽和逐设备云鉴权。
- 配置：ESP-IDF/组件锁定、NimBLE、protocomm Security 2、网络扫描、内存配置。
- 模拟器：二维码、BLE 发现、Security 2、`device-info`、云 claim/`cloud-config`、`prov-scan`、`prov-config`、`DEVICE_ONLINE`、状态机、错误映射、静态文件和本机 HTTP 边界。
- 文档：APP 原始需求、APP 仓库对接入口、机器契约与实际代码逐项比对。

## 本轮已修复

| 问题 | 修复 |
| --- | --- |
| 生产 identity-only QR 无 PoP 时，即使用户补录 PoP，前端仍错误禁用扫描/配网 | 只有二维码实际携带 PoP 时才要求完全相等；不含 PoP 时允许受控补录 |
| 契约把已成为必需流程的设备 Wi-Fi 扫描标成非必需 | `networkScanRequired` 改为 `true` |
| Bridge 只绑定 loopback，但未阻止恶意网页对本机端口发起简单 POST | 增加 Host、Origin、`application/json` 校验和 CSP 等安全响应头；设备发现改为 POST |
| BLE 操作可能被并发任务或长时间异常占用 | BLE 发现、Wi-Fi 扫描、配网共用操作锁，任务数有上限，单操作设置 90 秒保护超时 |
| eFuse MAC 读取失败后忽略 fallback 返回值，极端情况下可能产生重复 UID | 两种 MAC 读取都失败或 SHA-256 失败时直接返回错误，不生成弱身份 |
| 网关鉴权成功早于设备 NVS active 提交，模拟器可能误报完整成功 | 增加 `cloud_config_committed`；网关在回执前返回 `CLOUD_CONFIG_COMMIT_PENDING` |
| `cloud-config` 在窗口结束或成功保留期可能无限等待 | endpoint 关闭与入队使用互斥门，结束前排空任务，15秒保留期持续处理队列 |
| 损坏 pending 阻断有效 active 和 BLE 恢复 | pending/active 独立校验并隔离损坏槽位 |
| 失败 pending 每次重启重复尝试 | 明确拒绝或有 active 时连续三次失败后删除 pending 并回退 active |
| 官方两值 Wi-Fi 错误掩盖 raw reason | `device-info` 增加原始 reason 数值和名称，Bridge 在失败后读取并展示 |
| 本地重配网提前擦除旧 Wi-Fi | 先持久化 STA 回滚记录，新网络失败或超时恢复旧配置 |
| 固件二维码使用字符串拼接，配置字符可能破坏 JSON | 使用 cJSON 结构化生成并检查长度/内存；`device-info` 同时校验请求是 JSON 对象 |
| QR 前端仅检查 `PROV_` 前缀 | 改为严格匹配 `PROV_[0-9A-F]{6}`，并校验 username/PoP UTF-8 长度 |
| Bridge 曾以仓库根目录作为静态目录 | 静态目录限制为模拟器目录，`sdkconfig`、源码、构建和日志不能经 HTTP 读取 |
| NimBLE Host 使用 8 KB 栈时无法在整机内存布局中启动，4 KB 栈内直接做 ECDSA/NVS 又会溢出 | Host 栈固定为 4 KB；`cloud-config` 回调只排队并等待，由内部 RAM 的 16 KB `main_task` 完成 JSON、ECDSA 和 NVS pending 处理 |
| 模拟器把 `cloud_prov` 错从 `proto-ver.prov.cap` 读取，真机已通过仍显示警告 | 两份模拟器统一从 `proto-ver.nuwax.cap` 读取并完成回归 |
| 触摸任务栈位于 PSRAM，重新配网动作直接写 NVS 时触发 Flash cache assertion | Device 菜单回调只投递 `NUWAX_FLASH_OP_REPROVISION`，由单独 4 KB 内部 RAM `nuwax_flash_ops` 任务持久化回滚记录 |
| WebSocket RX 任务栈位于 PSRAM，鉴权成功后直接把云配置从 pending 提升为 active 时触发同类 assertion | RX 任务只投递 `NUWAX_FLASH_OP_CLOUD_COMMIT`；内部 RAM 任务完成 NVS 提交后，主任务再发送 `cloud_config_committed`，重复请求保持幂等 |

## 技术纠错

APP 仓库当前入口文档把 `prov-ctrl` 描述为配网终态权威来源，这是错误的。ESP-IDF `network_provisioning` 的 Wi-Fi set/apply/status 都在 `prov-config`；`prov-ctrl` 只用于 reset/reprovision 控制。本交付包和机器契约已统一为：

- 状态权威来源：`prov-config`。
- 控制 endpoint：`prov-ctrl`，本期 APP 不通过它远程清除设备凭据。
- Wi-Fi 子流程标准：`IP_EVENT_STA_GOT_IP` 即 `WIFI_READY`；完整 onboarding 必须继续等待同一凭据的 `DEVICE_ONLINE`。屏幕 Wi-Fi 图标仍不是协议条件。

## 遗留风险

### P0：量产阻塞

- 当前 `CONFIG_NUWAX_PROVISION_SEC2_DEV_MODE=y`，固定开发 PoP 会被所有开发设备共用，禁止量产。
- `nuwax_factory` 逐设备 salt/verifier 注入、PoP 管理、Secure Boot、Flash Encryption、NVS Encryption 尚未交付。
- 生产固件只保存 salt/verifier，设备屏幕二维码不含明文 PoP。若 APP 只扫描屏幕二维码而没有产线标签或云端受控凭据来源，Security 2 无法完成。
- 测试网关已实现设备 claim、签名配置、逐设备摘要鉴权和在线查询；正式用户账户绑定、解绑、所有权转移、吊销和审计仍需生产云实现。云端身份必须使用 `device-info.serialNumber`，不能使用 BLE 地址或广播名。
- 产线不得克隆已经包含 `nuwax_uid` 缓存的整片 NVS；每台设备必须由本机 eFuse 派生或写入经唯一性校验的工厂序列号。
- 本地 `sdkconfig` 含机器专用开发配置，不能提交给 APP 或进入交付包。

### P1：发布前联调

- Android Provider 的真实 Security 2、蓝牙权限、断连恢复和后台/前台切换尚未联合验收。
- 300 秒窗口末端的 30 秒 active-attempt grace 需要故障注入验证。
- 需要至少连续 20 次配网、失败后重试和重配网稳定性测试。
- 纯 WPA3-only AP 尚未验证；本次通过的 `xspaceagi` 不能替代该兼容性用例。
- 广播名只有 24 bit MAC 后缀，极低概率出现附近设备同名；Security 2 和 `device-info` 会阻止错误下发，但 APP 应明确提示重试，量产可评估增加非敏感 identity hint。

### P2：明确边界

- 当前 UID 展示格式只有 48 bit 哈希截断。现阶段可用于联调，量产应由产线唯一性注册表校验，或切换为工厂写入的更长不可变序列号。
- 模拟器和首轮契约要求非空 Wi-Fi 密码，未冻结开放网络配网。若产品需要开放网络，应单独扩展并验收。

## 已验证证据

- ESP-IDF 6.0 完整构建通过；最终应用镜像 `3,302,976` bytes，SHA-256 `46B85B6769639B7C8B6B5FFC3D66D97B4F5367D2DF0BFF5D3AEA5C6C940CE55A`，已仅烧录应用分区 `0x420000`。
- 两份 Bridge 单元测试各 14 项通过；两份二维码测试各 16 项通过；Python/JavaScript 语法检查通过。
- 模拟器 Mock 流程已扩展到 `WIFI_READY + DEVICE_ONLINE`；Real 页面已完成桌面真机流程检查，并修复 `cloud_prov` 断言误报。
- Bridge 静态隔离、同源限制、JSON 限制和契约接口已做 HTTP 验证。
- 最终镜像真机已通过：`PROV_1467C4`/Service UUID 广播、GATT、`proto-ver`、Security 2、`device-info` UID 一致性、签名 `cloud-config` pending 暂存、`prov-config` set/apply、`IP_EVENT_STA_GOT_IP`、逐设备 WebSocket 鉴权、pending 提升 active、`cloud_config_committed` 回执和在线查询。
- 模拟器 Real 模式在同一次流程中记录：GATT 7.1 s、Security 2 7.7 s、云配置暂存 8.5 s、Wi-Fi connected 14.3 s、`DEVICE_ONLINE` 33.6 s，最终 `SUCCESS`。
- 测试网关注册表确认设备 `NX-3927-2693-9288` 的 credential 为 active、`pending: null`；设备后续 WebSocket ping/pong 持续正常，串口未出现新的 assertion 或 panic。
- 详细证据和验收边界见 `10-final-device-acceptance.md`。下一步是 Android Provider 联合验收、20 次稳定性、300 秒边界故障注入及 P0 量产安全项，不应重复把本地三端通过表述为 Android 或生产通过。
