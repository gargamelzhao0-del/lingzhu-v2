# 正式云端 OTA 协议与开发指南 v1

## 1. 文档范围

本文只规定正式云端需要实现的 OTA 协议、数据、服务和可靠性要求。现有 WebSocket 鉴权与 `hello` 协议保持不变，OTA 版本判断使用独立的 `firmware_report`，不与 `hello` 耦合。

本版协议只保留 6 类消息：

| 方向 | 消息 | 用途 |
| --- | --- | --- |
| 设备到云端 | `firmware_report` | 本次启动的固件身份报告 |
| 云端到设备 | `firmware_report_ack` | 确认版本报告已经持久化 |
| 云端到设备 | `device_command/ota_update` | 下发升级地址和镜像身份 |
| 设备到云端 | `ota_ack` | 告知升级指令是否被接受 |
| 设备到云端 | `ota_result` | 上报升级阶段结果或最终结果 |
| 云端到设备 | `ota_result_ack` | 确认对应结果已经持久化 |

以下协议已取消：

- 不发送 `ota_progress`，下载阶段和百分比只在设备本地 UI 显示。
- 不支持 `device_command/ota_cancel`，设备接受升级后由本地 OTA 状态机完整执行。
- 所有 OTA 消息只使用各自必要字段，不再携带 `protocol_version` 或 `request_id`。

## 2. 启动版本报告

### 2.1 `firmware_report`

设备每次冷启动或重启只建立一个逻辑报告。ACK 丢失或连接波动时可以使用同一个 `report_id` 重发，但不得生成新的报告事务。

```json
{
  "type": "firmware_report",
  "report_id": "fw_0123456789abcdef",
  "device_id": "NX-AABB-CCDD-EEFF",
  "device_secret": "device-runtime-secret",
  "reset_reason": "power_on",
  "firmware": {
    "firmware_type": "NuwaxAIPartner_V2",
    "version": "7.0.48",
    "image_sha256": "6c65363081d4641a725fe2505968dd82c72e71e8bac0db367b065f5128eaa25e"
  }
}
```

云端要求：

1. 只接受已经通过 WebSocket 凭据鉴权的连接。
2. `device_id` 必须与当前认证设备一致。
3. `device_secret` 必须与本次已认证连接使用的设备密钥恒定时间比对一致；不得写入日志、数据库明文字段或审计事件。
4. `report_id` 全局唯一，同一 ID 重复到达必须幂等处理。
5. 必须先提交版本报告和设备状态，再发送 ACK。
6. `firmware_type/version/image_sha256` 用于映射云端不可变发布记录；`firmware_type` 固定为 `NuwaxAIPartner_V2`。
7. WebSocket 重连和重复 `hello` 不得单独触发新的版本判断。

### 2.2 `firmware_report_ack`

```json
{
  "type": "firmware_report_ack",
  "report_id": "fw_0123456789abcdef",
  "persisted": true
}
```

只有数据库提交成功后才能发送 `persisted:true`。失败时不发送成功 ACK，设备使用相同 `report_id` 退避重试。

## 3. 云端版本决策

云端按以下顺序处理：

1. 用 `report_id` 去重并更新设备当前固件身份。
2. 查询该设备是否已经存在未终结 OTA 事务。
3. 从可信设备库存读取板型、芯片、分区布局和构建类型。
4. 用 `firmware_type/version/image_sha256` 映射当前不可变发布。
5. 按渠道、批次、灰度、隔离和版本策略选择目标发布。
6. 目标版本必须严格高于设备当前 SemVer，禁止自动降级或同版本覆盖。
7. 在同一个数据库事务中创建 `ota_transaction` 与命令 Outbox。
8. 事务提交后才允许向当前认证连接投递 `ota_update`。

`release_sequence`、`target_board`、`target_chip`、`partition_schema` 和 `build_flavor` 可以保留在云端发布库与设备库存中用于选包，但不进入设备 WebSocket OTA 消息。

## 4. 固件发布数据

每个发布必须是不可变记录，建议至少保存：

```text
release_id
firmware_type
version
release_sequence
target_board
target_chip
partition_schema
build_flavor
size_bytes
image_sha256
artifact_object_key
publication_state
channel
rollout_policy
created_at
published_at
```

发布后禁止覆盖原对象。任何二进制变化都必须创建新的发布记录、对象路径、版本号和 SHA-256。云端必须自己计算固件大小与 SHA-256，不能信任管理端提交值。

## 5. 下发升级

### 5.1 `device_command/ota_update`

```json
{
  "type": "device_command",
  "command": "ota_update",
  "update_id": "ota_7000047_001",
  "url": "https://firmware.example.com/NuwaxAIPartner-7.0.49.bin",
  "version": "7.0.49",
  "size_bytes": 2007552,
  "image_sha256": "a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3"
}
```

字段要求：

| 字段 | 要求 |
| --- | --- |
| `type` | 固定为 `device_command` |
| `command` | 固定为 `ota_update` |
| `update_id` | 云端生成的全局唯一 OTA 事务 ID，最大 63 个 ASCII 字节 |
| `url` | 非空 HTTPS URL，最大 255 字节 |
| `version` | 严格 SemVer，最大 31 字节，必须高于设备当前版本 |
| `size_bytes` | 固件实际字节数，范围 `1..4194304` |
| `image_sha256` | 固件完整文件的 64 位小写十六进制 SHA-256 |

`ota_update` 不再包含以下字段：

```text
protocol_version
request_id
project_name
target_board
target_chip
partition_schema
build_flavor
release_sequence
force
release_notes
```

URL 服务必须满足：

- TLS 证书和域名校验有效。
- 直接返回 HTTP 200，不使用重定向。
- 提供准确 `Content-Length`，不使用 chunked 传输。
- 响应内容必须与 `size_bytes` 和 `image_sha256` 永久一致。
- 签名 URL 有效期应覆盖完整升级窗口，建议至少 60 分钟。
- URL 查询签名、Authorization 和 Cookie 不得写入普通业务日志。

### 5.2 `ota_ack`

接受示例：

```json
{
  "type": "ota_ack",
  "update_id": "ota_7000047_001",
  "accepted": true
}
```

拒绝示例：

```json
{
  "type": "ota_ack",
  "update_id": "ota_7000047_001",
  "accepted": false
}
```

ACK 只包含 `type/update_id/accepted`，不再携带 `ok`、拒绝码、状态文本、当前版本或重复标记。云端收到 `accepted:false` 后记录拒绝并结束本次投递；是否创建新的升级事务由云端策略决定。

同一个 `update_id` 的完全相同命令重复到达时，设备必须幂等处理，不能并行启动第二个下载。相同 `update_id` 但镜像身份不同必须拒绝。

## 6. 本地进度

设备在本地 UI 显示准备、下载百分比、校验、等待重启和失败状态。该进度不通过 WebSocket 上报，云端也不得等待 `ota_progress`。

云端只维护命令已投递、设备已接受、阶段结果已确认和超时状态。无结果超时只表示云端尚未收到结果，不能据此假定设备下载失败或已经回滚。

## 7. 升级结果

### 7.1 `ota_result`

```json
{
  "type": "ota_result",
  "update_id": "ota_7000047_001",
  "ok": false/true,
  "message": "https_read_stalled"
}
```

`ok` 必须是 JSON 布尔值：`true` 表示当前上报的升级阶段成功，`false` 表示升级失败或已经回滚。设备原有 `message` 内容保持不变，云端按以下映射维持内部事务状态：

| `ok` 与 `message` | 云端内部状态 | 是否终结升级事务 |
| --- | --- | --- |
| `true`，`health gate passed` 或 `already confirmed` | `committed` | 是 |
| `true`，其他原始成功说明 | `staged` | 否 |
| `false`，`new image rolled back` | `rolled_back` | 是 |
| `false`，其他原始失败说明 | `failed` | 是 |

`message` 是简短、稳定的诊断原因或状态说明，最大 127 字节。云端应原样保存该字段用于日志、事务状态映射与审计。

`ota_result` 不再包含：

```text
protocol_version
request_id
status
code
version
rebooting
rolled_back
retryable
retry_class
elapsed_ms
```

### 7.2 `ota_result_ack`

```json
{
  "type": "ota_result_ack",
  "update_id": "ota_7000047_001",
  "ok": false/true
}
```

云端必须先在一个数据库事务中写入结果、设备状态和审计事件，提交成功后才能发送 ACK。发送该 ACK 本身就表示结果已经持久化，因此不再携带 `persisted`。

ACK 的 `update_id` 与 `ok` 必须和收到的结果完全一致。设备只接受当前认证连接上匹配的 ACK。ACK 丢失时设备可以重发完全相同的结果，云端必须幂等落库并再次返回相同 ACK。

`ota_result_ack` 不再包含：

```text
protocol_version
request_id
status
persisted
```

## 8. 云端事务状态机

```text
created
  -> dispatch_pending
  -> dispatched
  -> accepted
  -> staged
  -> committed

dispatched -> rejected
accepted   -> failed
staged     -> rolled_back
```

约束：

- 同一设备最多只能有一笔活跃 OTA 事务。
- `update_id` 全局唯一，重复消息只更新同一事务。
- `committed` 必须发生在 `staged` 之后。
- `failed` 和 `rolled_back` 后不得自动重发同一个 `update_id`。
- 要重试升级必须由云端按策略创建新的 `update_id`。
- 新的 `firmware_report` 可用于对账当前版本，但不能替代 `ota_result` 的阶段闭环。

## 9. 数据库与可靠投递

### 9.1 `device_firmware_state`

保存当前 `firmware_type/version/image_sha256`、最后 `report_id`、目标发布、最后 OTA 状态、最后失败说明和最后在线时间。板型、芯片、分区布局和构建类型来自可信设备库存。

### 9.2 `ota_transaction`

保存 `update_id`、设备、目标发布、下发 payload 快照、投递连接、ACK 状态、`status`、`message`、各阶段时间和结果 ACK 状态。不再保存设备上报的下载百分比。

建议唯一约束：

```text
UNIQUE(report_id)
UNIQUE(update_id)
UNIQUE(device_id) WHERE transaction_state is active
```

升级命令使用事务 Outbox。WebSocket 写入成功不代表设备已接受，必须收到匹配 `ota_ack`；云端发送匹配 `ota_result_ack` 前必须已经提交结果事务。

## 10. 超时与重试

| 事项 | 建议值 |
| --- | --- |
| `firmware_report_ack` | 10 秒，使用同一 `report_id` 重试 |
| `ota_ack` | 30 秒，确认连接后重投相同 payload |
| 已接受但无 `ota_result` | 20 分钟，标记待对账 |
| `staged` 后无 `committed/rolled_back` | 180 秒，标记待对账 |
| 整体生命周期 | 40 分钟 |

超时不等于失败。云端不得在结果不明确时立即再次下发相同版本，避免设备重复升级循环。

## 11. 管理 API 建议

```text
POST /api/ota/releases
POST /api/ota/releases/{release_id}/publish
POST /api/ota/rollouts
GET  /api/ota/devices/{device_id}
GET  /api/ota/transactions/{update_id}
POST /api/ota/quarantine/{device_id}/{release_id}/clear
```

取消 API 只能取消尚未投递或尚未被设备接受的云端任务。设备返回 `accepted:true` 后，不再发送 `ota_cancel`。

## 12. 安全要求

- OTA 命令只能发送到当前认证设备的当前 WebSocket 会话。
- 固件存储桶禁止公开写入，已发布对象不可覆盖。
- 正式发布建议增加发布者签名；SHA-256 只能证明下载内容与清单一致。
- OTA 管理接口必须有独立权限、审批和审计。
- 设备密钥、签名 URL、Authorization 与 Cookie 必须脱敏。

## 13. 云端开发顺序

1. 建立发布、设备固件状态、OTA 事务和 Outbox 数据表。
2. 实现 `firmware_report` 幂等落库和 `firmware_report_ack`。
3. 实现固件上传、服务端 size/SHA 校验和不可变发布。
4. 实现设备库存匹配、SemVer 决策、灰度和隔离。
5. 实现精简 `ota_update` 投递与 `ota_ack` 收敛。
6. 实现 `ota_result` 幂等落库和精简 `ota_result_ack`。
7. 实现断线重连、超时对账、监控、告警和审计。
8. 通过模拟器与真机断网、断电、错误 SHA、回滚和 ACK 丢失验收。

## 14. 云端验收清单

- 同一 `firmware_report` 重复到达只执行一次版本判断。
- WebSocket 重连或重复 `hello` 不产生新的启动版本检查。
- `ota_update` 只包含本文定义的 7 个字段。
- 相同 `update_id` 重投不会产生第二笔事务。
- 云端不发送 `ota_cancel`，也不等待 `ota_progress`。
- `firmware_report` 必须包含与当前认证凭据一致的 `device_secret`，且云端不记录密钥明文。
- `ota_ack` 只包含 `type/update_id/accepted`。
- `ota_result` 只包含 `type/update_id/ok/message`。
- `ota_result_ack` 只包含 `type/update_id/ok`。
- 结果落库失败时不发送 `ota_result_ack`。
- `staged` 后断线可在新会话继续收敛为 `committed` 或 `rolled_back`。
- 旧固件回滚后不会立即再次下发同一发布。
- 签名 URL 和设备凭据不会进入普通日志。

机器可读协议以 `schemas/nuwax-device-protocol.schema.json` 为准。
