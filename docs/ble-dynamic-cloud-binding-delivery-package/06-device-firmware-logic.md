# 设备固件逻辑

## 代码边界

| 代码位置 | 职责 |
| --- | --- |
| `main/nuwax_provision_new/nuwax_provision_main.c` | provisioning 生命周期、endpoint、cloud-config JSON、状态和错误 |
| `main/nuwax_provision_new/nuwax_cloud_config.c` | ECDSA 验签、防回滚、NVS pending/active |
| `main/nuwax_provision_new/nuwax_device_uid.c` | 稳定设备 ID |
| `main/wifi_manager.c` | 导入官方配网凭据、历史网络和回滚 |
| `main/ws_transport.c` | 动态 URL 解析、TLS/TCP、101 前鉴权 headers |
| `main/session_protocol.c` | hello、authenticated hello、commit/ack 消息 |
| `main/nuwax_ai_partner_main.c` | 启动集成、云配置选取、内部 RAM 提交任务、连接重试 |

## 进入配网

- 首次启动：ESP-IDF 未标记 Wi-Fi provisioned 时自动进入 BLE。
- 升级/恢复：已有 Wi-Fi、但 pending/active 云配置均缺失时，自动保存原 STA 并进入可回滚 BLE recovery。
- 已配网设备：正常启动不广播；用户在 `Device` 菜单点击 Meeting 下方 BLE 按钮。
- 本地重配网先保存当前 STA 配置作为回滚记录，然后重启进入 300 秒窗口，不提前删除旧 Wi-Fi 和 active 云配置。
- 不提供 APP 远程清除凭据 endpoint，避免远程误操作使设备离线。

## BLE 阶段

设备使用 NimBLE Peripheral 和官方 `network_provisioning`：

1. `proto-ver.prov` 暴露 Security 2 和 `wifi_prov/wifi_scan`；`proto-ver.nuwax` 暴露产品 capability `cloud_prov`。
2. `prov-session` 建立 SRP6a + AES-GCM。
3. `device-info` 返回稳定 UID 和配网诊断。
4. `cloud-config` 接收 UTF-8 JSON；GATT 回调只入队，主任务执行 JSON、ECDSA 和 NVS。
5. `prov-scan`/`prov-config` 使用官方 protobuf。

没有成功 cloud-config 且也没有可用 active 时，固件不允许把本次流程判定为完整配网成功，诊断为 `cloud_config_required`。

## 配置验签

固件逐项检查：

- `version == 1`。
- `bundleVersion` 为正且不超过 JSON 安全整数上限。
- `deviceId` 等于本机硬件派生 UID。
- `cloudUrl` 是无空白、无 userinfo 的 `wss://`；测试构建可允许 `ws://`。
- credential、secret 和 signature 非空且不含控制/空白字符。
- ECDSA P-256/SHA-256 签名有效。
- 版本高于现有最新版本；相同版本只允许内容完全相同的幂等重放。

APP 修改 URL、secret、版本或任一字符都会导致签名失败。

## NVS 事务

namespace：`nuwax_cloud`；键：`pending`、`active`。

- BLE 验签成功只写 `pending`。
- 启动连接时优先使用 pending；没有 pending 才使用 active。
- WebSocket authenticated hello 的 credential 必须等于当前 pending。
- 内部 RAM 任务完成 `pending -> active` NVS 原子提交。
- 主任务确认 active 已持久化后才发 `cloud_config_committed`。
- 当前固件只发送一次 commit，尚未持久化服务端 `_ack`；丢包后的幂等补发是生产阻塞项。
- 损坏 pending/active 独立隔离，不让一个坏槽位阻断另一个有效槽位。
- 新配置明确 401，或有旧 active 且新 pending 连续失败三次时，丢弃 pending 并回退 active。

## 为什么使用内部 RAM Flash 任务

触摸任务和 WebSocket RX 任务的栈可能位于 PSRAM。ESP-IDF 写 Flash/NVS 时会临时关闭 cache，直接从 PSRAM 栈写 NVS可能触发 assertion。最终实现使用一个 4 KB 内部 RAM 栈任务 `nuwax_flash_ops` 串行处理：

- `NUWAX_FLASH_OP_REPROVISION`：保存 Wi-Fi 回滚记录。
- `NUWAX_FLASH_OP_CLOUD_COMMIT`：提升云配置 active。

UI/RX 回调只排队，避免在外部 RAM 栈进入 Flash 操作。

## Wi-Fi 和 BLE 生命周期

- 配网总窗口 300 秒。
- 截止前已接受一次有效凭据，允许该 active attempt 额外观察 30 秒。
- Wi-Fi set/apply 后扫描停止，避免 STA scan 与连接互相影响。
- `IP_EVENT_STA_GOT_IP` 即 `WIFI_READY`，新 Wi-Fi 到达该里程碑后清除 STA 回滚记录。
- BLE 成功后保留 15 秒状态查询窗口，再停止广播并释放 BLE 内存。
- BLE 释放后设备继续动态连接 cloudUrl；云端在线不要求 BLE 长期保持。

## 动态配置的实际使用

应用主循环不读取编译期 URL/密钥。它调用 `nuwax_cloud_config_get_effective()` 得到 pending/active，然后构造：

```text
url = cloud_config.cloud_url
device_id = cloud_config.device_id
credential_id = cloud_config.credential_id
device_secret = cloud_config.device_secret
```

WebSocket Upgrade 发送三项鉴权 header。配置保存为 active 后，设备重启无需 APP 再下发，直到云端轮换 credential 或用户重新绑定。
