# Nuwax AI Partner 设备端与云端完整通信协议 v1

| 项目 | 当前基线 |
| --- | --- |
| 状态 | 当前源码总协议入口 |
| 基准日期 | 2026-08-12 |
| 适用工程 | `NuwaxAIPartner-v2`（ESP32-S3）与当前正式云端/网关实现 |
| 协议版本 | 应用协议 v1、NA 音频容器 v1、OTA 协议 v1 |
| 云端音频 | 16 kHz、单声道、20 ms；上行 PCM16 或构建时协商的 Opus，下行 PCM16 |

## 阅读导航

| 目标 | 建议先读 |
| --- | --- |
| 建立设备连接 | 第 2、5 节 |
| 接收或发送音频 | 第 6、7 节 |
| 实现打断和主动通知 | 第 8、9 节 |
| 控制设备、会议和监控 | 第 10～12 节 |
| 接入 Wi-Fi、MCP 或 OTA | 第 13～15 节 |
| 处理 ACK、重试和兼容性 | 第 16、17 节 |

消息方向统一以设备为参照：**上行**表示设备发往云端，**下行**表示云端发往设备。除特别说明外，JSON 示例中的布尔值和数字均为 JSON 原生类型，不得使用同名字符串代替。

## 1. 范围、依据与优先级

本文总结设备和云端在设备 WebSocket 上发送的全部当前协议，包括：

- WebSocket 接入、认证、保活、重连和帧限制；
- 普通对话的采集、ASR、文本回复、TTS、播放和智能打断；
- 云端主动通知播放及其准备、取消和终态确认；
- 会议模式、监控模式及监控静音暂停/人声恢复；
- Wi-Fi、远程配网、屏幕输入、PC 状态和 MCP；
- 启动固件报告、OTA 命令、本地进度、结果及可靠终态 ACK；
- NA v1 双向二进制音频格式；
- 正式业务命令、兼容消息和诊断命令。

本文不定义手机 App 与设备之间的 BLE 配网 GATT 协议，也不把网关管理 REST API 当成设备线协议。REST API 只负责向网关注入命令，设备实际看到的仍是本文中的 WebSocket JSON 或 Binary 帧。

协议事实按以下优先级解释：

1. 当前设备序列化、解析和状态机源码；
2. 当前正式网关发送、解析和持久化源码；
3. `schemas/nuwax-device-protocol.schema.json` 的正式基础契约；
4. 其他历史设计文档。

本文使用以下标记：

- **正式**：应由正式设备和正式云端共同实现；
- **扩展**：当前设备和网关已实现，但尚未完整写入基础 Schema；
- **兼容**：当前网关可能发送，设备允许忽略或仅记录；
- **诊断**：开发和验收用途，不应作为产品业务接口开放。

实现状态说明：除第 9 节主动通知播放外，本文标记为正式的主路径均已在当前设备和当前工作区网关中实现。主动通知播放已经完成设备解析、状态机、Schema 和 host-check，但当前 `cloud-gateway` 尚未实现完整 BEGIN/Binary/END 编排，正式云端必须补齐后才能端到端联调。

## 2. 传输与连接

### 2.1 WebSocket

设备连接配置的设备端点，当前网关路由为：

```text
/api/device/ws
```

生产环境应使用 `wss://`。若配置了桥接令牌，WebSocket Upgrade 请求携带：

```http
X-Bridge-Token: <bridge-token>
```

Upgrade 层令牌和 `hello.device_secret` 属于两个不同的校验层次；当前集成可以给二者配置同一个值，但云端仍应分别完成 Upgrade 鉴权和应用层设备身份校验。任何令牌、设备密钥、Wi-Fi 密码都不得写入普通日志。

设备接收 WebSocket 帧的约束：

| 项目 | 约束 |
| --- | --- |
| 分片 | 只接受 `FIN=1` 的完整消息，不支持 continuation |
| 压缩 | `RSV1/2/3` 必须为 0，不支持 per-message compression |
| Mask | 云端发往设备的帧不得 mask |
| Opcode | text、binary、close、ping、pong |
| 单帧最大 payload | 16 KiB；超限可使用 close code `1009` 断开 |
| 文本编码 | UTF-8 JSON 对象 |
| Binary | 一个或多个连续 NA v1 帧 |

### 2.2 保活与重连

当前设备存在三层活性检测：

| 层次 | 当前行为 |
| --- | --- |
| WebSocket control Ping | 设备约每 25 秒发送一次，云端必须返回 control Pong |
| 应用层 `ping` | READY 后设备约每 15 秒发送；连接准备阶段可约每 5 秒发送 |
| 接收空闲超时 | 设备连续约 60 秒没有收到任何云端 WebSocket 帧时关闭旧连接并重连 |

设备在正常对话、空闲、会议和监控期间都应保持 WebSocket。业务轮次结束、ASR 无结果、TTS 结束或 `listen stop` 都不是断开 WebSocket 的理由。只有配网切换、OTA 重启、显式关机或不可恢复的传输错误可以主动结束连接。意外断开后设备自动重连，旧连接上的 `request_id`、`audio_session_id` 和命令不得跨连接继续复用。

## 3. 标识符与关联规则

| 字段 | 用途 | 当前通用容量 |
| --- | --- | --- |
| `device_id` | 稳定设备身份，通常为 eFuse 派生 UID | 最多 63 字符 |
| `session_id` | 网关为一次 WebSocket 连接分配的 ID | 云端生成 |
| `request_id` | 请求与 ACK 的幂等关联 | 基础 Schema 最多 39 字符；模式协议兼容上限 47 字符 |
| `audio_session_id` | 一条独立上行音频流 | 最多 47 字符 |
| `turn_id` | 一轮用户输入、文本回复和 TTS | 最多 47 字符 |
| `playback_id` | 一次逻辑 TTS 播放 | 最多 47 字符 |
| `event_id` | 一次 `barge_in` 候选/确认/拒绝事务 | 最多 47 字符 |
| `notify_id` | 一次云端主动通知事务 | 最多 47 字符 |
| `update_id` | 一次 OTA 事务 | 1..63 字符 |

通用建议字符集为 `[A-Za-z0-9_.-]`。OTA ID 额外允许 `+` 和 `:`，格式为 `[A-Za-z0-9][A-Za-z0-9._+:-]{0,62}`。同一 `request_id` 或 `update_id` 重试必须保持业务内容完全一致；相同 ID 携带不同内容属于冲突，不是新请求。

关联关系：

```text
WebSocket session_id
  ├─ request_id -> 请求 / ACK
  ├─ audio_session_id -> listen + 上行 NA 帧 + ASR
  ├─ turn_id -> text_response / device_command / playback_id / TTS NA 帧
  ├─ notify_id -> notify playback / playback_id / 通知 NA 帧
  └─ update_id -> OTA 命令 / 接受 ACK / 可靠结果
```

## 4. 消息总表

### 4.1 设备上行

| 类型 | 等级 | 用途 | 预期云端响应 |
| --- | --- | --- | --- |
| `hello` | 正式 | 身份、能力、音频和固件元数据 | `hello` 或 `error` |
| `firmware_report` | 正式 | 每次启动建立一次精简版本检查事务 | `firmware_report_ack` |
| `ping` | 正式 | 应用层保活 | `pong` |
| `listen` | 正式 | 声明音频会话 start/stop | `listen_ack`（设备不阻塞等待） |
| Binary NA v1 | 正式 | 麦克风音频、START/END/VAD | `audio_stats`、`audio_session`、ASR 系列 |
| `text_request` | 正式 | 请求云端处理一段文本 | `text_response` 及可选 TTS |
| `cancel_turn` | 正式 | 取消一轮对话 | `cancel_turn_ack` |
| `barge_in` | 扩展 | 播放中打断候选、确认或拒绝 | `barge_in_ack` |
| `audio_mode_request` | 扩展 | 设备 UI 开启/关闭会议模式 | `audio_mode_request_ack` |
| `monitoring_capture_control` | 正式 | 监控暂停/恢复上传请求 | 必须返回匹配 ACK |
| `input_event` | 正式 | 触摸输入事件 | `input_event_ack` |
| `wifi_scan_result` | 正式 | Wi-Fi 扫描结果 | `wifi_scan_ack` |
| `wifi_status` | 正式 | Wi-Fi 连接阶段状态 | `wifi_status_ack` |
| `volume_changed` | 正式 | 设备 UI 音量变更及重连音量同步 | 无二次 ACK；云端按 `event_id` 幂等记录 |
| `device_command_ack` | 正式 | 云端命令最终执行结果 | 无二次 ACK |
| `notify_playback_begin_ack` | 正式 | 通知音频已获准进入扬声器链路 | 无二次 ACK |
| `notify_playback_end_ack` | 正式 | 通知逻辑、Binary 和物理播放均已结束 | 无二次 ACK |
| `notify_playback_cancelled` | 正式 | 用户打断、超时或本地恢复导致通知取消 | 无二次 ACK |
| `pc_status_request` | 正式 | 请求 PC Agent 状态 | 转换为 `device_command/show_status` 等 |
| `status_app_request` | 正式 | 请求指定 PC 应用状态 | 转换为 `device_command/show_status` 等 |
| `mcp` | 正式 | MCP JSON-RPC 响应 | 由 JSON-RPC `id` 关联 |
| `ota_ack` | 正式 | OTA 接受或拒绝 | 云端持久化，无直接 ACK |
| `ota_result` | 正式 | OTA 阶段或终态结果 | 必须返回 `ota_result_ack` |

### 4.2 云端下行

| 类型 | 等级 | 用途 | 设备行为 |
| --- | --- | --- | --- |
| `hello` | 正式 | 应用层认证成功 | 标记云端会话可用 |
| `firmware_report_ack` | 正式 | 启动版本报告已经持久化 | 停止本次启动内的报告重试 |
| `error` | 正式 | 认证、JSON 或业务错误 | 记录；认证失败时断开并退避 |
| `pong` | 正式 | 应用层保活响应 | 刷新接收活性 |
| `listen_ack` | 扩展 | 云端已处理 start/stop | 当前设备不等待、不驱动状态机 |
| `audio_session` | 正式 | 云端音频会话开始/结束摘要 | 记录诊断 |
| `audio_stats` | 扩展 | 周期性接收统计 | 更新低优先级音频进度 |
| `asr_partial` | 正式 | 流式 ASR 中间文本 | 更新显示/进度，不结束轮次 |
| `asr_final` | 正式 | 流式 ASR 确定文本 | 更新轮次文本 |
| `asr_result` | 正式 | ASR 最终结果 | 开始或结束对话处理 |
| `text_response` | 正式 | 智能体最终文本与情绪 | 关联 `turn_id`，等待或配合 TTS |
| `playback_begin` | 正式 | 授权一条下行音频播放 | 创建播放上下文，随后接收 Binary |
| Binary NA v1 | 正式 | PCM16 TTS 音频 | 放入扬声器播放队列 |
| `playback_end` | 正式 | 播放流逻辑结束 | 完成/取消对应播放 |
| `tts_result` | 正式 | TTS 汇总或错误 | 记录结果和统计 |
| `tts_skip` | 正式 | 当前轮次禁用语音播报 | 终止 TTS 等待并恢复持续聆听，不建立播放流 |
| `notify_playback_begin` | 正式 | 建立云端主动通知播放事务 | 返回匹配 BEGIN ACK 后接收 Binary |
| `notify_playback_end` | 正式 | 声明通知逻辑结束 | 等待 Binary END 和物理停播后返回 END ACK |
| `device_command` | 正式/扩展 | 状态、显示、声音、模式、OTA 等控制 | 执行并返回 `device_command_ack`，OTA 除外 |
| `cancel_turn_ack` | 正式 | 取消轮次结果 | 记录关联状态 |
| `barge_in_ack` | 扩展 | 打断事务结果 | 按 `event_id` 收敛状态 |
| `audio_mode_request_ack` | 扩展 | 会议 UI 请求结果 | 按 `request_id` 切换或回滚 |
| `monitoring_capture_control_ack` | 正式 | 监控暂停/恢复授权 | 成功 ACK 后才真正改变上传状态 |
| `input_event_ack` | 正式 | 输入事件确认 | 记录统计 |
| `wifi_scan_ack` | 兼容 | 扫描结果确认 | 当前设备可忽略 |
| `wifi_status_ack` | 兼容 | Wi-Fi 状态确认 | 当前设备可忽略 |
| `mcp` | 正式 | MCP JSON-RPC 请求 | 执行并返回 `mcp` |
| `ota_result_ack` | 正式 | OTA 结果已持久化 | 停止对应结果重发/允许后续动作 |
| `ota_error` | 兼容 | 网关拒绝或无法持久化 OTA 报告 | 记录错误，不能当作终态 ACK |
| `ack` | 兼容 | 未知上行类型的通用兜底 | 当前设备可忽略 |
| `sleep_request` | 兼容 | 网关睡眠审计通知 | 真正睡眠由 `device_command/sleep_conversation` 驱动 |

## 5. 接入、认证和能力协商

### 5.1 设备 `hello`

连接建立后设备首先发送：

```json
{
  "type": "hello",
  "version": 1,
  "device_id": "NX-XXXX-XXXX-XXXX",
  "device_secret": "<device-secret>",
  "transport": "websocket",
  "has_sd_card": true,
  "features": {
    "audio_upload": true,
    "audio_playback": true,
    "mcp": true,
    "meeting_mode_v1": true,
    "monitoring_mode_v1": true,
    "monitoring_pause_resume_v1": true,
    "remote_ble_reprovision": true,
    "volume_changed_v1": true,
    "remote_wake_word_config": true,
    "notify_playback_v1": true,
    "ota_v1": true
  },
  "audio_params": {
    "input_format": "pcm16",
    "codec": "pcm16",
    "opus": false,
    "output_format": "pcm16",
    "sample_rate": 16000,
    "output_sample_rate": 16000,
    "channels": 1,
    "mic_channels": 1,
    "frame_duration_ms": 20,
    "speaker": true,
    "wake_word": true,
    "vad": true,
    "full_duplex": true,
    "shared_i2s": true,
    "hardware_profile": "miaopan_v2_0_n32r8",
    "aec": true,
    "aec_reference": "software",
    "has_ref": true,
    "software_ref": true,
    "hardware_ref_candidate": false,
    "hardware_ref_health": "not_tested",
    "hardware_ref": false,
    "ref_slot": -1,
    "afe_input_format": "MR"
  },
  "firmware": {
    "project_name": "NuwaxAIPartner",
    "version": "7.0.0",
    "release_sequence": 7000001,
    "target_board": "miaopan_v2_0_n32r8",
    "target_chip": "esp32s3",
    "partition_schema": 1,
    "build_flavor": "product",
    "running_partition": "ota_0",
    "ota_state": "valid",
    "image_sha256": "<64-lowercase-hex>"
  }
}
```

`firmware` 只在启用 OTA 时出现。`audio_params` 是本次连接的实际能力，云端不得根据设备型号猜测，也不得把 ES7210 的物理 TDM slot 数当作云端音频声道数。

`has_sd_card` 为顶层布尔值。`true` 表示最近一次完成的 SD 检测成功；`false` 表示未检测到可识别的卡。设备使用挂载结果检测 SD，没有独立卡座检测信号：插拔不保证即时更新，卡损坏或无法挂载时也可能为 `false`；短暂重挂载期间保留上次检测结果。此字段不表示剩余容量、当前可写性或当前模式已经成功开启。旧固件不带字段时按未知处理，不等同于 `false`。

每次连接首先发送完整 `hello`；会议／监控真正进入 ACTIVE 后，在控制循环的下一次服务中立即补发同格式 `hello`，重新读取 SD 检测结果。在线正常情况下不等待心跳周期；发送繁忙则后续重试，失败后间隔 1 秒重试。离线开启不要求联网，恢复连接时的首次 `hello` 上报最新状态。重复开启命令、监控静音暂停／恢复不重复触发此上报。

云端须允许同一已认证连接重复收到同身份 `hello`，更新 `has_sd_card` 等设备状态即可，**不能关闭连接、重新创建场次、清空当前音频状态或重新触发 OTA／初始化命令**。首次和后续 `hello` 均不要求单独 ACK；云端若返回 `hello`，保持现有成功响应格式和原连接 `session_id`。后续 `hello` 的身份和密钥仍须校验。

### 5.2 云端 `hello`

正式最小成功响应：

```json
{
  "type": "hello",
  "ok": true,
  "session_id": "cloud_session_001"
}
```

严格响应可额外包含：

```json
{
  "type": "hello",
  "ok": true,
  "authenticated": true,
  "device_id": "NX-XXXX-XXXX-XXXX",
  "session_id": "cloud_session_001",
  "transport": "cloud-websocket",
  "mode": "cloud-pc-relay",
  "conversation_mode": "...",
  "asr_streaming_mode": "...",
  "tts_mode": "...",
  "interrupt_level": "...",
  "barge_in_decision_owner": "device",
  "barge_in_asr_confirm_enabled": false,
  "capability_policy": {}
}
```

设备接受两种成功形式：`ok:true` 且可选 `device_id` 与本机一致；若存在 `authenticated`，必须为 `true`。认证失败示例：

```json
{
  "type": "error",
  "code": "DEVICE_AUTH_FAILED",
  "error": "DEVICE_AUTH_FAILED"
}
```

身份不匹配或错误密钥会被网关拒绝并关闭连接。同身份的重复 `hello` 按第 5.1 节更新状态；旧的本地云端模拟器存在 `duplicate_hello` 拒绝逻辑，接入本行为前需同步修改接收端，不能继续沿用“一条连接只允许一次 hello”的限制。

云端删除设备绑定时必须使用结构化错误码：

```json
{
  "type": "error",
  "code": "DEVICE_NOT_BOUND",
  "error": "device binding is required"
}
```

设备只按唯一、精确匹配的 `code=DEVICE_NOT_BOUND` 进入重新绑定流程，不根据 `error` 或 `message` 的自由文本猜测。该错误会关闭产生它的 WebSocket connection epoch、保留现有 Wi-Fi 配置并请求一次 BLE 重新绑定；旧连接迟到消息不得触发新连接上的配网或状态变化。重新绑定完成前，云端不应期待设备继续快速重连旧身份。

### 5.3 启动固件报告

设备在当前启动第一次连接云端后发送一次逻辑报告；ACK 丢失时使用相同 `report_id` 重试：

```json
{
  "type": "firmware_report",
  "report_id": "fw_0123456789abcdef",
  "device_id": "NX-XXXX-XXXX-XXXX",
  "device_secret": "device-runtime-secret",
  "reset_reason": "power_on",
  "firmware": {
    "firmware_type": "NuwaxAIPartner_V2",
    "version": "7.0.48",
    "image_sha256": "<64-lowercase-hex>"
  }
}
```

`image_sha256` 在首次手工烧录且尚无可信运行镜像身份时可以省略。发布序号、板型、芯片、分区 schema、构建类型、运行分区和 OTA 状态不在本消息重复上报；云端从不可变发布记录、可信设备库存和 `ota_result` 事务取得这些信息。

云端完成持久化后回复：

```json
{
  "type": "firmware_report_ack",
  "report_id": "fw_0123456789abcdef",
  "persisted": true
}
```

`report_id` 必须原样返回。设备只接受当前 WebSocket connection epoch 上收到的匹配 ACK；额外字段可以兼容接收，但云端不得依赖它们。

### 5.4 应用层保活

设备：

```json
{ "type": "ping", "now_ms": 123456 }
```

Schema 推荐云端回显 `now_ms` 并提供 `server_ms`：

```json
{ "type": "pong", "now_ms": 123456, "server_ms": 1780000000000 }
```

当前网关实际兼容格式为：

```json
{ "type": "pong", "session_id": "cloud_session_001", "now": 1780000000000 }
```

设备当前只依赖 `type:"pong"`，因此两种形式均可用。新云端实现建议同时返回 `now_ms` 和 `server_ms`。

## 6. NA v1 二进制音频

### 6.1 固定头

每个 NA 帧使用 22 字节小端头：

| 偏移 | 长度 | 字段 | 说明 |
| ---: | ---: | --- | --- |
| 0 | 2 | `magic` | ASCII `NA` |
| 2 | 1 | `version` | 固定 `1` |
| 3 | 1 | `codec` | `1=PCM16`，`2=Opus` |
| 4 | 1 | `flags` | START/END/VAD 位图 |
| 5 | 1 | `channels` | 当前正式播放和采集为 `1` |
| 6 | 2 | `sample_rate` | 当前正式值 `16000` |
| 8 | 2 | `frame_duration_ms` | 当前正式值通常 `20` |
| 10 | 4 | `sequence` | 每个 `audio_session_id` 从 `0` 开始递增 |
| 14 | 4 | `timestamp_ms` | 音频时间戳 |
| 18 | 2 | `payload_bytes` | 当前 NA 帧负载长度 |
| 20 | 2 | `reserved` | 当前写 `0`，不是 CRC |
| 22 | 可变 | `payload` | PCM16 little-endian 或一个 Opus packet |

Flags：

| 位 | 名称 | 语义 |
| ---: | --- | --- |
| `0x01` | `START` | 本音频会话第一帧 |
| `0x02` | `END` | 本音频会话最后帧/终止标记 |
| `0x04` | `VAD_SPEECH` | 设备本地判断为人声，仅作提示，不得据此丢弃其他帧 |

一个 WebSocket Binary message 可以串联多个完整 NA 帧。接收方必须按每帧 `22 + payload_bytes` 循环解析，不能假定“一条 WebSocket Binary 就是一帧音频”。

### 6.2 上行音频

普通对话、会议和监控共用 NA 容器，但通过 `listen.mode` 和 `audio_session_id` 路由。PCM16 16 kHz 单声道 20 ms 的标准负载是 320 samples，即 640 bytes。实际尾帧可以更短。

顺序约束：

```text
listen start
-> NA START（sequence=0）
-> NA 普通帧（sequence 连续）
-> NA END（可与最后一帧真实 PCM 合并，也可为空负载）
-> listen stop（当前生产默认启用）
```

`CONFIG_NUWAX_PROTOCOL_LISTEN_STOP_ENABLE` 控制是否发送 `listen stop`。当前 `sdkconfig.defaults` 为 `y`，生产必须保持启用；该宏只允许临时协议测试关闭，关闭不代表协议删除。

### 6.3 下行播放音频

下行目前只支持 PCM16、单声道，并应与 `hello.audio_params.output_sample_rate` 一致。标准顺序：

```text
playback_begin
-> NA START（必须先建立 playback_id 上下文）
-> NA 普通 PCM 帧
-> NA END
-> playback_end
-> tts_result（可在播放前提供摘要，也可在完成后提供结果）
```

设备会拒绝没有有效 `playback_begin`/START 上下文的音频、立体声音频、非法帧、过时 `turn_id` 或过时 `playback_id`。云端取消播放时仍应发送 `playback_end.reason="cancelled"` 或 `"canceled"`，并停止发送旧播放的 Binary 帧。

## 7. 普通对话协议

### 7.1 `listen`

设备声明音频会话开始：

```json
{
  "type": "listen",
  "state": "start",
  "mode": "awake_session",
  "audio_session_id": "conversation_00000002_00000004",
  "request_id": "listen_004"
}
```

结束：

```json
{
  "type": "listen",
  "state": "stop",
  "mode": "awake_session",
  "audio_session_id": "conversation_00000002_00000004",
  "request_id": "listen_004"
}
```

字段：

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `type` | 是 | 固定 `listen` |
| `state` | 是 | `start` 或 `stop` |
| `mode` | 是 | 采集来源/业务模式 |
| `audio_session_id` | 强烈要求 | 音频流关联；兼容旧 stop 可省略 |
| `request_id` | 否 | 控制请求关联 |

当前正常对话可能使用的 `mode` 包括：

- `wake_word`、`touch_wake`、`touch_listen`：唤醒入口；
- `awake_session`、`auto_followup`、`cancel_followup`：持续对话和取消后的继续聆听；
- `during_playback`、`barge_in`：播放期间全双工采集/打断；
- `meeting`、`monitoring`：独占长音频模式；
- `acceptance_capture`、`p5_fullduplex_overlap_probe` 等：诊断音频。

云端响应：

```json
{
  "type": "listen_ack",
  "ok": true,
  "session_id": "cloud_session_001",
  "state": "start",
  "mode": "awake_session",
  "audio_session_id": "conversation_00000002_00000004",
  "active_audio_session_id": "conversation_00000002_00000004",
  "duplicate": false,
  "error": ""
}
```

设备不等待 `listen_ack` 才上传音频。云端必须在收到 `listen start` 时立即创建接收器；Binary START 可能紧随其后到达。

### 7.2 ASR 消息

流式中间结果：

```json
{
  "type": "asr_partial",
  "provider": "dashscope",
  "audio_session_id": "conversation_00000002_00000004",
  "turn_id": "turn_001",
  "text": "帮我查一下",
  "definite": false,
  "sequence": 3,
  "at_ms": 1260
}
```

流式确定结果把 `type` 改为 `asr_final`，通常 `definite:true`。最终 ASR 结果：

```json
{
  "type": "asr_result",
  "ok": true,
  "provider": "dashscope",
  "audio_session_id": "conversation_00000002_00000004",
  "turn_id": "turn_001",
  "text": "帮我查一下今天的天气",
  "error": "",
  "no_speech": false
}
```

`asr_partial`/`asr_final` 至少需要 `type,text`；`asr_result` 正式契约需要 `type,ok,turn_id,text`。当前网关还可能附带 `session_id`、`audio`、`latency`、`conversation_endpoint` 和 `no_speech`，设备按向前兼容原则忽略不认识的字段。

### 7.3 `audio_session` 与 `audio_stats`

会话开始：

```json
{
  "type": "audio_session",
  "session_id": "cloud_session_001",
  "state": "start",
  "mode": "awake_session",
  "audio_session_id": "conversation_00000002_00000004"
}
```

会话结束摘要：

```json
{
  "type": "audio_session",
  "session_id": "cloud_session_001",
  "state": "stop",
  "audio_session_id": "conversation_00000002_00000004",
  "frames": 120,
  "duration_ms": 2400,
  "rms": 318,
  "reason": "audio_end_flag"
}
```

网关当前每累计约 50 帧可发送：

```json
{
  "type": "audio_stats",
  "session_id": "cloud_session_001",
  "frames_received": 150,
  "audio_messages_received": 38,
  "bytes_received": 99300,
  "last_rms": 318,
  "audio_session": {
    "audio_session_id": "conversation_00000002_00000004"
  }
}
```

`audio_stats` 是低优先级诊断进度，不能作为 END、ASR 完成或存储完成的依据。

### 7.4 `text_request` 与 `text_response`

设备请求：

```json
{
  "type": "text_request",
  "request_id": "asr_001",
  "text": "帮我查一下今天的天气"
}
```

云端响应：

```json
{
  "type": "text_response",
  "ok": true,
  "request_id": "asr_001",
  "turn_id": "turn_001",
  "user_text": "帮我查一下今天的天气",
  "assistant_text": "今天白天多云，气温……",
  "emotion": "happy",
  "tts_pending": true,
  "error": ""
}
```

正式必填字段为 `type,ok,turn_id,tts_pending`。情绪兼容 `emotion`、`assistant_emotion`、`assistantEmotion`，新实现统一使用 `emotion`。若 `ok:false`，云端填写 `error`，不得继续发送属于失败轮次的旧 TTS。

### 7.5 播放控制与 TTS 结果

开始：

```json
{
  "type": "playback_begin",
  "session_id": "cloud_session_001",
  "request_id": "asr_001",
  "turn_id": "turn_001",
  "playback_id": "pb_001",
  "codec": "pcm16",
  "sample_rate": 16000,
  "channels": 1,
  "frame_duration_ms": 20,
  "frames": 300,
  "bytes": 192000,
  "provider": "volcengine"
}
```

结束：

```json
{
  "type": "playback_end",
  "session_id": "cloud_session_001",
  "request_id": "asr_001",
  "turn_id": "turn_001",
  "playback_id": "pb_001",
  "codec": "pcm16",
  "sample_rate": 16000,
  "channels": 1,
  "frame_duration_ms": 20,
  "frames": 300,
  "bytes": 192000,
  "provider": "volcengine",
  "reason": "complete"
}
```

`reason` 常见值为 `complete`、`cancelled`/`canceled`、`error`。可选 `segment_index` 表示流式分段，但同一次逻辑播放必须保持一致的 `playback_id` 和有序控制边界。

TTS 汇总：

```json
{
  "type": "tts_result",
  "ok": true,
  "request_id": "asr_001",
  "turn_id": "turn_001",
  "playback_id": "pb_001",
  "provider": "volcengine",
  "sample_rate": 16000,
  "channels": 1,
  "frames": 300,
  "duration_ms": 6000,
  "playback_start": true,
  "playback_end": true,
  "error": ""
}
```

`tts_result` 只报告合成结果和统计元数据，`playback_start/playback_end` 是兼容观测字段，不驱动设备播放状态机。逻辑播放边界由 `playback_begin/playback_end` 管理，物理 PCM 边界由 Binary NA v1 的 START/END 标志管理。

关闭语音播报时，云端在成功的 `text_response` 之后发送：

```json
{
  "type": "tts_skip",
  "ok": true,
  "request_id": "",
  "turn_id": "",
  "reason": "voice_broadcast_disabled"
}
```

`tts_skip` 是当前轮次的无播放终止边界。它必须在 `playback_begin` 之前发送，并且与该轮的 `playback_begin`、下行 Binary NA、`playback_end` 和 `tts_result` 互斥。推荐填写与 `text_response` 一致的 `request_id`、`turn_id`；空字符串仅用于兼容当前已经处于 `WAIT_TTS` 的唯一活动轮次，不能创建或关联一个新轮次。设备收到合法消息后释放 TTS 预热状态、保留 WebSocket 并恢复持续聆听。已经进入物理播放的轮次不会被 `tts_skip` 中断。

### 7.6 `cancel_turn`

设备取消：

```json
{
  "type": "cancel_turn",
  "request_id": "cancel_001",
  "turn_id": "turn_001",
  "reason": "device_cancel"
}
```

云端确认：

```json
{
  "type": "cancel_turn_ack",
  "ok": true,
  "turn_id": "turn_001",
  "new_turn_id": "turn_002",
  "reason": "device_cancel"
}
```

取消后云端必须停止该 `turn_id` 的模型生成和 TTS 下发。`new_turn_id` 可选，用于显式声明后续轮次。

### 7.7 正常连续对话时序

```text
设备唤醒
-> listen start(audio_session_id=A, mode=wake_word/awake_session)
-> 上行 NA START..END
-> listen stop(A)
<- audio_session / asr_partial / asr_final / asr_result(turn_id=T)
<- device_command set_state(thinking, turn_id=T)
<- text_response(turn_id=T, tts_pending=true)
<- playback_begin(turn_id=T, playback_id=P)
<- 下行 NA START..END
<- playback_end(T, P)
<- tts_result(T, P)
设备进入后续聆听
-> 新 listen start(audio_session_id=B, mode=awake_session/auto_followup)
```

长回答期间设备的 120 秒对话空闲计时必须暂停；只有设备处于聆听等待且持续没有用户语音时才进入可唤醒睡眠。播放、思考、ASR 等待或活跃采集期间不得因该计时器睡眠。

## 8. 智能打断 `barge_in`

### 8.1 设备上报

```json
{
  "type": "barge_in",
  "state": "candidate",
  "event_id": "barge_001",
  "parent_turn_id": "turn_001",
  "playback_id": "pb_001",
  "audio_session_id": "barge_audio_001",
  "onset_sequence": 220,
  "current_sequence": 224,
  "confidence_q15": 28672
}
```

`state` 为：

- `candidate`：本地发现可能为近端人声，尚未最终打断；
- `confirmed`（兼容输入 `confirm`）：确认是真人声，应取消旧播放；
- `rejected`：候选被本地或云端判定为回声/噪声。

确认消息额外携带 `confirmed_sequence`，设备当前将其设为 `current_sequence`。`confidence_q15` 范围 `0..32768`。

### 8.2 云端 ACK

```json
{
  "type": "barge_in_ack",
  "ok": true,
  "session_id": "cloud_session_001",
  "event_id": "barge_001",
  "state": "confirmed",
  "duplicate": false,
  "interrupt_enabled": true,
  "parent_turn_id": "turn_001",
  "playback_id": "pb_001",
  "audio_session_id": "barge_audio_001",
  "new_turn_id": "turn_002",
  "action": "playback_cancelled",
  "error": "",
  "rejection_reason": ""
}
```

同一 `event_id` 的上下文必须保持一致。常见失败：`event_id_required`、`invalid_barge_in_state`、`event_context_mismatch`。重复确认应返回 `duplicate:true`，不能再次取消新轮次。

云端确认打断时通常还发送带旧 `turn_id/playback_id` 的 `device_command/stop_playback`。设备按 ID 隔离过时 stop，不能让上一轮迟到命令取消新播放。

## 9. 云端主动通知播放

### 9.1 能力、边界与互斥

只有 `hello.features.notify_playback_v1=true` 时，云端才可启动通知播放。该事务复用正常扬声器和 NA v1 下行音频链路，但使用独立的 `notify_id`，不属于普通对话的 `turn_id`。

通知可从正常对话或可唤醒睡眠状态启动。会议、监控、OTA 和运行时 BLE 配网与通知事务互斥；通知未收敛时不得启动这些独占业务。每次通知绑定到接收 BEGIN 的 WebSocket connection epoch，断线后旧事务失效，旧连接的 ACK、Binary 或 END 不得跨到新连接继续使用。

### 9.2 启动前准备

正常对话中，云端先停止当前对话播放并等待 ACK：

```text
device_command/stop_playback
-> device_command_ack
-> notify_playback_begin
```

设备处于可唤醒睡眠时，云端先发送：

```json
{
  "type": "device_command",
  "request_id": "notify_prepare_001",
  "command": "prepare_notify_playback",
  "notify_id": "ntf_001",
  "text": "可选通知摘要"
}
```

设备只有在释放旧扬声器 generation、麦克风采集和播放期唤醒检测所有权后，才返回 `ok:true`。云端必须等待该 `device_command_ack`，不能把控制帧已经写入 WebSocket 当作设备已准备完成。

### 9.3 BEGIN、Binary 与 END

云端建立播放事务：

```json
{
  "type": "notify_playback_begin",
  "request_id": "notify_begin_001",
  "notify_id": "ntf_001",
  "playback_id": "notify_pb_001",
  "codec": "pcm16",
  "sample_rate": 16000,
  "channels": 1,
  "frame_duration_ms": 20,
  "frames": 123,
  "bytes": 78720
}
```

`request_id` 最大 39 字节，`notify_id` 和 `playback_id` 最大 47 字节。媒体字段可省略；出现时只接受当前产品支持的 PCM16、16 kHz、单声道、20 ms。设备回复：

```json
{
  "type": "notify_playback_begin_ack",
  "request_id": "notify_begin_001",
  "notify_id": "ntf_001",
  "playback_id": "notify_pb_001",
  "ok": true,
  "message": "ready"
}
```

云端应等待 BEGIN ACK 后再发送 Binary NA START、PCM 数据和 END。设备仍对兼容发送方提供接收背压，使紧随 BEGIN 的 Binary 不会越过排队中的控制事件，但云端不能依赖该兼容行为替代 ACK。

逻辑结束消息：

```json
{
  "type": "notify_playback_end",
  "request_id": "notify_end_001",
  "notify_id": "ntf_001",
  "playback_id": "notify_pb_001",
  "frames": 123,
  "bytes": 78720,
  "reason": "completed"
}
```

云端先发送 Binary END，再发送 JSON END。设备允许二者的应用回调顺序交错，但只有以下三项全部成立后才返回 `notify_playback_end_ack(ok:true)`：Binary END 已接收、JSON END 已接收、绑定的物理扬声器 generation 已停止。成功后设备创建或继续醒着的会话，并进入聆听状态。

同一连接内重试必须复用相同 ID 和相同媒体总量；相同 ID 携带不同 `frames`、`bytes` 或 `reason` 必须视为冲突。

### 9.4 取消、超时与断线

头部触摸和唤醒词可以取消通知。设备停止绑定的扬声器 generation、释放音频所有权，并发送：

```json
{
  "type": "notify_playback_cancelled",
  "request_id": "notify_cancel_001",
  "notify_id": "ntf_001",
  "playback_id": "notify_pb_001",
  "reason": "wake_word_interrupt"
}
```

通知媒体使用 10 秒滑动接收空闲超时；每次有效音频进度都会刷新计时。超时后设备取消事务并恢复到有效本地状态。WebSocket 断线时设备丢弃通知事务并重置扬声器运行态，不在新连接补发旧 ACK。完整通知专项契约见 `notification-playback-v1.md`。

## 10. 云端 `device_command`

### 10.1 通用包络与 ACK

命令包络：

```json
{
  "type": "device_command",
  "request_id": "cmd_001",
  "command": "set_state",
  "turn_id": "turn_001",
  "audio_session_id": "conversation_001",
  "state": "thinking"
}
```

`type,request_id,command` 为正式必填字段。`turn_id`、`audio_session_id`、`playback_id`、`barge_event_id` 根据命令使用。唯一兼容例外是历史 `set_wake_word` 可只发送 `command,wake_word,persist`；设备仍会执行完整替换，但由于没有关联 ID，ACK 省略 `request_id` 且重复下发不能走幂等账本。其他命令不放宽。

普通命令统一回复：

```json
{
  "type": "device_command_ack",
  "request_id": "cmd_001",
  "command": "set_state",
  "ok": true,
  "message": "thinking"
}
```

模式命令 ACK 额外包含 `mode,state`。OTA 使用独立的 `ota_ack/result`，不使用普通 `device_command_ack` 作为正式结果；OTA 下载进度只在设备本地 UI 显示。

### 10.2 正式业务命令

| `command` | 主要字段 | 语义 |
| --- | --- | --- |
| `set_state` | `state=sleeping|wake_only` | 兼容休眠入口；统一走完整睡眠协调器 |
| `set_emotion` | `emotion` | 更新表情 |
| `set_volume` | `volume_percent`, `persist?` | 设置 0..100 音量 |
| `set_wake_word` | 非空 `wake_word[]`, `persist?` | 完整替换 MultiNet 拼音命令组；最多保留前 10 组 |
| `reset_wake_word` | 无业务字段 | 恢复固件默认唤醒词并删除唤醒词 NVS 覆盖 |
| `stop_playback` | `turn_id`, `playback_id`, `title?`, `text?` | 取消严格匹配的播放；正常收敛不主动断开 WebSocket |
| `sleep_conversation` | `turn_id?`, `text?` | 进入仍可被唤醒词唤醒的对话睡眠 |
| `prepare_notify_playback` | `notify_id`, `text?` | 从睡眠/普通对话安全释放音频所有权，为通知播放做准备 |
| `show_status` | `title?`, `lines?`, `text?` | 显示状态内容 |
| `wifi_scan` | 无业务字段 | 请求扫描附近 Wi-Fi |
| `wifi_connect` | `ssid`, `password?` | 请求连接 Wi-Fi |
| `start_ble_provisioning` | 无业务字段 | 请求进入运行时 BLE 配网 |
| `set_audio_mode` | `mode`, `state`, `interrupt_current_turn?` | 控制会议或监控模式 |
| `ota_update` | OTA v1 清单 | 下载、校验、切换和确认固件 |

### 10.3 状态与表情枚举

`set_state.state`：

```text
sleeping
wake_only
```

`sleeping` 和 `wake_only` 不是纯 UI 标签，会停止当前对话所有权、取消必要的播放并启动睡眠唤醒词检测。`booting/wifi_connecting/ws_connecting/idle/listening/thinking/speaking/error` 由设备本地生命周期独占，云端下发会被严格校验拒绝。正式云端优先使用 `sleep_conversation` 表达业务睡眠。

`set_emotion.emotion` 正式兼容值：

```text
neutral normal idle happy smile surprised wink tongue
thinking think focused pensive analyzing analysis processing received
what confused understood ok awesome great like sad sorry error lost
angry shy tired
dizziness dizzy excited thought_complete thinking_complete sleeping sleep
provisioning pairing disconnected offline meeting monitoring monitor
touch touched playful question
```

新表情 UI 不增加新的消息类型。云端继续使用现有
`device_command/command=set_emotion`，只需要把上述新名称加入发送端白名单；
推荐直接使用 `dizziness`、`excited`、`thought_complete`、`sleeping`、
`provisioning`、`disconnected`、`meeting`、`monitoring`、`touch`、
`playful` 和 `question`，旧别名仅用于兼容。

`set_emotion` 只设置语义表情，不会启动会议、监控、配网或休眠事务。
这些业务状态仍使用各自的正式命令；真实配网、会议和监控模式保留最终显示优先级。
云端语义表情可以覆盖普通对话、断联和休眠的默认表情，但不会唤醒设备、启动音频、
退出休眠或改变任何业务状态。设备在休眠事务中也必须接收并 ACK 受支持的表情值。

字段别名 `assistant_emotion`、`assistantEmotion` 只为兼容旧云端；新消息统一使用 `emotion`。

### 10.4 音量

```json
{
  "type": "device_command",
  "request_id": "volume_001",
  "command": "set_volume",
  "volume_percent": 60,
  "persist": true
}
```

`volume_percent` 必须为整数 `0..100`。`persist` 默认 `false`。兼容字段 `volumePercent`、`volume_persist`、`volumePersist` 可解析；`volume_db` 只用于诊断命令，不能替代正式音量百分比。

设备端 UI 完成一次音量提交后，上报：

```json
{
  "type": "volume_changed",
  "event_id": "volume_a1b2c3d4_00000001",
  "volume_percent": 65,
  "source": "device_ui"
}
```

`hello.features.volume_changed_v1=true` 表示支持此消息。`source=device_ui` 表示用户在设备屏幕上确认了修改；`source=device_sync` 是 WebSocket 重连并确认云服务可用后的当前值快照。设备对快速连续调节采用 latest-wins，只保留最终值；发送拥塞或断线时保留待上报值，并在当前连接可用或下次重连后补发。云端下发 `set_volume` 时设备只返回原有 `device_command_ack`，不得再生成 `volume_changed`，以避免命令回显环路。

`event_id` 在一次启动内单调变化，云端必须将重复 ID 视为同一事件。首版不定义 `volume_changed_ack`；设备在每次云服务重新就绪时发送当前快照，因此云端可在重连后收敛到设备实际值。

### 10.5 唤醒词

```json
{
  "type": "device_command",
  "request_id": "wakeword_001",
  "command": "set_wake_word",
  "wake_word": [
    "xiao zhu xiao zhu",
    "xiao zu xiao zu",
    "xiao zhu xiao zu",
    "xiao zu xiao zhu",
    "ni hao ling zhu",
    "ni hao lin zhu",
    "ni hao ling zu",
    "ni hao lin zu",
    "ni hao xiao zu",
    "ni hao xiao zhu"
  ],
  "persist": true
}
```

要求：数组不能为空；设备最多保留原数组前 10 项并忽略后续项。保留项必须为 2..63 ASCII bytes，且仅使用拼音所需的字母、数字和空白；每项内部的拼音音节数量不固定，同一数组中的各项也可以长短不同。设备会转换小写、合并首尾及连续空白，并按规范化后的值去重。处理后的数组是完整配置快照：先删除旧命令组，再将结果以同一个命令 ID 交给 MultiNet。例如当前有 10 组、云端下发 8 组时，最终只保留这 8 组；旧版四词数组也保持四组，不再自动扩展。成功 ACK 的 `message` 返回实际 `wake_words` 数量和 `truncated=0|1`。`persist` 默认 `true`。旧版单字符串格式仍兼容，其中任一内置唤醒词字符串会展开为当前十组内置拼音。只有 `hello.features.remote_wake_word_config=true` 时云端才可下发。

恢复固件默认十组唤醒词：

```json
{
  "type": "device_command",
  "request_id": "wake_reset_001",
  "command": "reset_wake_word"
}
```

成功后立即启用当前固件默认十组，并只删除 `wake_word/config_v2` 与旧版 `wake_word/command` 两个 NVS 键。Wi-Fi、设备密钥、云端绑定和其他持久化数据不受影响；任一步失败则恢复复位前的运行配置和 NVS 快照。

### 10.6 停止播放与睡眠

停止指定播放：

```json
{
  "type": "device_command",
  "request_id": "stop_001",
  "command": "stop_playback",
  "turn_id": "turn_001",
  "playback_id": "pb_001",
  "title": "VOICE",
  "text": "playback stopped"
}
```

`turn_id` 和 `playback_id` 均为必填，命令只作用于同时匹配的当前播放；过时命令返回隔离/忽略 ACK，不应破坏当前新轮次。正常路径只停止扬声器和相关旧轮次；若物理停止或终态收敛失败，设备可关闭绑定 epoch，避免在协议状态不明的连接上开始新轮次。

云端睡眠：

```json
{
  "type": "device_command",
  "request_id": "sleep_001",
  "command": "sleep_conversation",
  "turn_id": "turn_002",
  "text": "进入休眠"
}
```

设备成功 ACK 后进入 `sleeping`，但必须保留本地唤醒能力；再次唤醒后重新开始正常对话循环。

### 10.7 显示、Wi-Fi 与 BLE 配网

显示：

```json
{
  "type": "device_command",
  "request_id": "status_001",
  "command": "show_status",
  "title": "VOICE TEXT",
  "lines": ["第一行", "第二行", "第三行"],
  "text": "可选长文本"
}
```

最多 6 行，设备内部每行最多保存 47 bytes，`title` 最多 39 bytes，`text` 最多保存 511 bytes。

Wi-Fi 和 BLE 示例见第 13 节。

### 10.8 诊断命令

以下命令已存在于当前固件，但只用于研发、声学验收或灰度测试。正式业务云端默认不得开放：

| 命令 | 字段 | 默认/说明 |
| --- | --- | --- |
| `set_audio_cleaner` | `enabled` | 开关音频清理器 |
| `set_l3_interrupt` | `interrupt_enabled?`, `persist?` | 默认 enabled=true、persist=true |
| `p3_ref_health` | `volume_db?`, `duration_ms?`, `run_ref_probe?` | 默认 -18 dB、2000 ms |
| `p3_softref_ab` | `duration_ms?`, `delay_samples?`/`delay_ms?` | 默认 1000 ms、240 samples |
| `p3_hardref_ab` | `duration_ms?` | 默认 1000 ms，硬件 ref slot 2 |
| `p3_nearend_candidate` | `duration_ms?`, `delay_samples?`/`delay_ms?` | 默认 1200 ms、record-only |
| `p3_interrupt_timing` | `mode?`, `duration_ms?`, `delay_samples?`/`delay_ms?` | 默认 mode=record_only |
| `p5_fullduplex_overlap_probe` | `audio_session_id`, `duration_ms?` | 默认 1200 ms |
| `p5_during_playback_candidate` | `mode?`, `audio_session_id?`, `duration_ms?`, `delay_samples?` | 默认 mode=record_only、5000 ms；也用于 gray_confirm/gray_cancel |
| `set_acceptance_capture` | `mode`, `audio_session_id?`, `duration_ms?`, `delay_samples?` | `off/disable/disabled` 取消；`raw/raw_ref/raw_hw_ref` 选择信号 |

诊断命令仍返回 `device_command_ack`，但 ACK 的 `message` 是可扩展诊断文本，不应被业务端按固定字符串解析。

## 11. 会议模式

### 11.1 边界

- 设备 UI 按钮只可以请求会议模式，不能请求监控模式；
- 云端可以用 `set_audio_mode` 开启或关闭会议；
- 会议与普通对话、监控互斥；
- 会议期间上传连续长音频，不进入普通 ASR/智能体/TTS；
- 连续约 30 分钟没有符合活动判定的人声/环境活动时，设备以 `quiet_timeout` 本地关闭会议；
- UI、错误、云端命令或断线也可以关闭会议。

### 11.2 设备 UI 请求

开启：

```json
{
  "type": "audio_mode_request",
  "request_id": "meeting_ui_001",
  "mode": "meeting",
  "state": "enabled",
  "interrupt_current_turn": true,
  "reason": "ui"
}
```

关闭或本地超时通知：

```json
{
  "type": "audio_mode_request",
  "request_id": "meeting_close_001",
  "mode": "meeting",
  "state": "disabled",
  "reason": "quiet_timeout"
}
```

云端 ACK：

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "meeting_ui_001",
  "ok": true,
  "mode": "meeting",
  "state": "enabled",
  "audio_session_id": "meeting_audio_001",
  "current_turn_cancelled": true
}
```

拒绝时 `ok:false` 并提供 `reason`，例如 `missing_request_id`、`invalid_request_id`、`invalid_state`、`meeting_mode_unavailable`。

### 11.3 云端主动控制

```json
{
  "type": "device_command",
  "request_id": "meeting_cloud_001",
  "command": "set_audio_mode",
  "mode": "meeting",
  "state": "enabled",
  "interrupt_current_turn": true
}
```

设备最终 ACK：

```json
{
  "type": "device_command_ack",
  "request_id": "meeting_cloud_001",
  "command": "set_audio_mode",
  "ok": true,
  "mode": "meeting",
  "state": "enabled",
  "message": "active"
}
```

### 11.4 生命周期

```text
设备/云端发起 enabled
-> 控制请求被批准
-> 停止旧普通对话/播放并取得麦克风所有权
-> listen start(mode=meeting, audio_session_id=M)
-> 上行 NA START..连续音频
-> 设备进入 ACTIVE 并返回 active ACK
...
-> UI/quiet_timeout/云端 disabled/错误/断线
-> NA END
-> listen stop(M)
-> disabled ACK 或 audio_mode_request(disabled)
```

常见模式 ACK `message`：`active`、`already_active`、`disabled`、`already_disabled`、`audio_mode_busy`、`meeting_busy`、`superseded_by_disable`、`start_cancelled`、`capture_failed`、`control_queue_full`。云端必须以 `ok` 为主判断，`message` 仅用于可扩展诊断。

## 12. 监控模式

### 12.1 边界

- 监控只能由云端 `device_command/set_audio_mode` 开启或关闭；
- 设备 UI 会议按钮在监控活跃时失效，不能切换或关闭监控；
- 监控与普通对话、会议互斥；
- 监控静音两分钟不会退出模式，只暂停云端音频上传；
- 暂停期间本地麦克风轻量 VAD 保持工作，检测弱人声后请求恢复；
- 暂停和恢复都必须先得到云端成功 ACK，设备才改变流状态。

### 12.2 云端开启/关闭

```json
{
  "type": "device_command",
  "request_id": "monitoring_001",
  "command": "set_audio_mode",
  "mode": "monitoring",
  "state": "enabled"
}
```

设备 ACK 与会议格式相同，只把 `mode` 改为 `monitoring`。关闭使用 `state:"disabled"`。

### 12.3 两分钟无声暂停

设备保持上传并先请求：

```json
{
  "type": "monitoring_capture_control",
  "request_id": "mon_pause_001",
  "mode": "monitoring",
  "state": "pause",
  "audio_session_id": "monitoring_stream_001",
  "reason": "no_speech",
  "silence_ms": 120000
}
```

云端必须回复完全匹配的关联字段：

```json
{
  "type": "monitoring_capture_control_ack",
  "request_id": "mon_pause_001",
  "mode": "monitoring",
  "state": "pause",
  "audio_session_id": "monitoring_stream_001",
  "ok": true,
  "reason": "acknowledged",
  "duplicate": false
}
```

只有收到匹配 `ok:true` 后，设备才发送当前流的 NA END、排空上行队列、发送 `listen stop`，然后进入暂停上传状态。ACK 超时按相同 ID、相同 payload 重试，当前默认 1500 ms、最多 3 次。pause 被拒绝或持续无 ACK 时设备回到持续上传状态并重新累计静音时间，不退出监控模式；云端仍可使用 `set_audio_mode disabled` 正常关闭监控。

### 12.4 人声恢复

设备为下一条流预生成新的 `audio_session_id`：

```json
{
  "type": "monitoring_capture_control",
  "request_id": "mon_resume_001",
  "mode": "monitoring",
  "state": "resume",
  "audio_session_id": "monitoring_stream_002",
  "reason": "speech_detected"
}
```

云端成功 ACK 后，设备才发送 `listen start(monitoring_stream_002)`，先上传预录窗口，再继续实时音频。resume 被拒绝或持续无 ACK 时设备保持暂停上传，丢弃已经过期的预录窗口，并重新等待安静到人声的有效边沿；不会因此退出监控模式。云端在旧流仍活跃时应拒绝 resume。

ACK `reason` 可能为：`acknowledged`、`request_id_conflict`、`invalid_request`、`feature_not_advertised`、`monitoring_session_not_active`、`audio_session_mismatch`、`audio_session_active`、`monitoring_not_paused`。

### 12.5 幂等规则

- 同一 `request_id` 和相同 `mode/state/audio_session_id` 重试，云端返回原 ACK 并设 `duplicate:true`；
- 相同 ID 携带不同 state 或 session ID，返回 `ok:false, reason:"request_id_conflict"`；
- `pause` ACK 先于 END/stop；`resume` ACK 先于新 start/audio；
- 监控暂停不是 `set_audio_mode disabled`，云端不得把模式标记为退出。

## 13. Wi-Fi、远程配网与输入事件

### 13.1 Wi-Fi 扫描

云端命令：

```json
{
  "type": "device_command",
  "request_id": "wifi_scan_001",
  "command": "wifi_scan"
}
```

设备先返回 `device_command_ack`（`message:"scanning"`），扫描完成后发送：

```json
{
  "type": "wifi_scan_result",
  "ok": true,
  "message": "",
  "count": 2,
  "networks": [
    { "ssid": "Office", "rssi": -45, "authmode": 3, "secure": true },
    { "ssid": "Guest", "rssi": -67, "authmode": 0, "secure": false }
  ]
}
```

当前设备最多上报 16 个网络。网关可返回：

```json
{
  "type": "wifi_scan_ack",
  "session_id": "cloud_session_001",
  "ok": true,
  "count": 2
}
```

### 13.2 Wi-Fi 连接

```json
{
  "type": "device_command",
  "request_id": "wifi_connect_001",
  "command": "wifi_connect",
  "ssid": "Office",
  "password": "<secret>"
}
```

`ssid` 必填，最长 32 bytes；`password` 最长 64 bytes。密码不得写入日志、ACK 或状态 API。设备命令 ACK 只表示连接任务已进入队列，不表示已经获得 IP。

设备随后上报阶段：

```json
{
  "type": "wifi_status",
  "phase": "connected",
  "ssid": "Office",
  "ip": "192.168.1.20",
  "ok": true,
  "message": "got_ip"
}
```

`phase` 为 `connecting`、`connected`、`failed`、`disconnected`。网关可返回 `wifi_status_ack`：

```json
{
  "type": "wifi_status_ack",
  "session_id": "cloud_session_001",
  "ok": true,
  "phase": "connected"
}
```

Wi-Fi 切换会自然中断原 WebSocket。设备在新网络获得 IP 后建立新连接、重新发送 `hello`；云端不得要求旧连接继续发送完成 ACK。

### 13.3 BLE 配网

```json
{
  "type": "device_command",
  "request_id": "ble_provision_001",
  "command": "start_ble_provisioning"
}
```

设备返回 `device_command_ack`，`ok:true` 表示请求已被接受，`ok:false` 表示电源/OTA/其他关键操作占用或配网服务不可用。设备在不重启的情况下暂停非必要高内存业务、保留 UI 并启动 BLE 配网。前 300 秒由 APP 独占配网机会；其后 BLE 广播与已保存 Wi-Fi 恢复持续竞争且没有总超时。旧 Wi-Fi 先成功则退出配网并恢复正常服务；APP 先连接则暂停旧 Wi-Fi 尝试，APP 失败或断开后回到并行竞争。实际 SSID、密码和云配置通过独立 BLE 协议传输，不经过本 WebSocket 文档。

进入配网是允许主动停止云端连接的例外。配网完成后必须重新连接并重新认证。

### 13.4 屏幕输入

设备：

```json
{
  "type": "input_event",
  "source": "touch",
  "action": "press",
  "touch": {
    "x": 120,
    "y": 88,
    "gesture": 0,
    "points": 1,
    "event": 1
  }
}
```

云端：

```json
{
  "type": "input_event_ack",
  "session_id": "cloud_session_001",
  "source": "touch",
  "action": "press",
  "events_received": 15
}
```

## 14. PC 状态转发与 MCP

### 14.1 PC 状态请求

设备请求默认 PC 状态：

```json
{
  "type": "pc_status_request",
  "request_id": "pc_status_001"
}
```

请求指定应用：

```json
{
  "type": "status_app_request",
  "request_id": "app_status_001",
  "app": "browser"
}
```

网关把请求转发给 PC Agent。设备侧没有独立 `pc_status_response` 解析器，当前结果通常转成 `device_command/show_status`、`set_state` 或 `set_emotion` 返回。云端必须保留原 `request_id` 或创建可追踪的新命令 ID。

### 14.2 MCP 包络

云端请求和设备响应都使用：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/list",
    "params": {}
  }
}
```

设备实现的 MCP 方法：

| 方法 | 说明 |
| --- | --- |
| `initialize` | 返回协议版本 `2024-11-05`、serverInfo 和 tools capability |
| `tools/list` | 返回当前真正可用的工具清单 |
| `tools/call` | 调用设备工具 |

当前工具：

| 工具 | 参数 | 条件 |
| --- | --- | --- |
| `self.get_device_status` | 无 | 有设备状态快照时可用 |
| `self.ui.set_emotion` | `{ "emotion": "happy" }` | 有表情回调时可用 |
| `self.servo.head_shake` | `{ "times": 1..5 }` | 只有真实舵机驱动和 capability bit 同时存在时发布 |

当前 MCP **不提供**设置音量、开启/关闭会议、开启/关闭监控、配网或 OTA 工具。这些能力仍必须使用第 10～15 节定义的 `device_command` 和专用状态机。云端不得根据产品功能反向猜测 MCP 工具；只能调用本次 `tools/list` 实际返回的名称。

成功响应：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 1,
    "result": {
      "content": [
        { "type": "text", "text": "true" }
      ],
      "isError": false
    }
  }
}
```

JSON-RPC 错误码：`-32700 Parse error`、`-32600 Invalid request`、`-32601 Method not found`、`-32602 Invalid tool parameters`。工具名合法但设备未发布或当前不可用时，`tools/call` 返回 `result.isError=true`，不使用 JSON-RPC 传输错误表示业务失败。

## 15. OTA v1

> 本节是当前设备已经实现并通过设备端 host-check 的精简 OTA 线协议。更完整的云端持久化、发布库和事务状态机说明见 `正式云端OTA协议与开发指南-v1.md`。

### 15.1 前置条件

云端只有在以下条件成立时才可发送 OTA：

- 当前 WebSocket 已通过 `hello` 认证；
- `hello.features.ota_v1=true`；
- 没有会议或监控长音频占用；
- 设备电量/供电满足安全策略；
- 没有其他 OTA 或待确认终态；
- 云端已通过可信设备库存选择正确板型、芯片、分区布局和构建类型的固件；
- 目标 SemVer 严格高于设备当前版本。

OTA 包络额外要求：

- `update_id` 是唯一 OTA 事务键；
- JSON 必须是单个完整对象；
- `image_sha256` 必须是 64 个小写十六进制字符；
- URL 必须为非空 HTTPS URL；
- `size_bytes` 为 `1..4194304`。

### 15.2 `ota_update`

```json
{
  "type": "device_command",
  "command": "ota_update",
  "update_id": "ota_20260805_001",
  "url": "https://firmware.example.com/NuwaxAIPartner.bin",
  "version": "7.0.1",
  "size_bytes": 3456789,
  "image_sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
}
```

正式消息只包含上例 7 个字段。`protocol_version`、`request_id`、`project_name`、`target_board`、`target_chip`、`partition_schema`、`build_flavor`、`release_sequence`、`force` 和 `release_notes` 均不发送。硬件目标与发布序号只保留在云端发布库和可信设备库存中。

### 15.3 `ota_ack`

```json
{
  "type": "ota_ack",
  "update_id": "ota_20260805_001",
  "accepted": true
}
```

`ota_ack` 只包含 `type/update_id/accepted`，不包含 `ok`、拒绝码、状态文本或重复标记。拒绝时 `accepted=false`。相同 `update_id` 和相同镜像身份的重复命令必须幂等处理；相同 `update_id` 但镜像身份不同必须拒绝。

### 15.4 本地进度

设备继续计算并在本地 UI 显示准备、下载百分比、校验、等待重启和失败状态，但不发送 `ota_progress`。云端不得把缺少进度消息判定为下载失败。

### 15.5 `ota_result`

```json
{
  "type": "ota_result",
  "update_id": "ota_20260805_001",
  "ok": true,
  "message": "image_staged"
}
```

`ok` 必须是 JSON 布尔值：

```text
true  当前上报的升级阶段成功
false 升级失败或已经回滚
```

`message` 是最大 127 字节的稳定诊断说明，其内容保持设备原有定义不变。云端内部仍维护 `staged/committed/failed/rolled_back`：`health gate passed`、`already confirmed` 映射为 `committed`；其余 `ok:true` 映射为 `staged`；`new image rolled back` 映射为 `rolled_back`；其余 `ok:false` 映射为 `failed`。

### 15.6 云端可靠终态 ACK

云端必须先持久化 `ota_result`，然后回复：

```json
{
  "type": "ota_result_ack",
  "update_id": "ota_20260805_001",
  "ok": true
}
```

ACK 的发送本身表示云端已经持久化结果。设备校验当前认证连接、`update_id` 和 `ok`。ACK 丢失时设备重发完全相同的结果，云端必须幂等落库并再次回复相同 ACK。

### 15.7 取消能力

正式协议不包含 `device_command/ota_cancel`。云端只可取消尚未投递或尚未被设备接受的本地任务；设备返回 `accepted:true` 后，等待 `ota_result` 收敛。

## 16. ACK、错误、重试与安全规则

### 16.1 ACK 判断

- 业务成功以布尔字段 `ok` 为准，不以 `message` 文本为准；
- `device_command_ack` 必须同时匹配 `request_id` 和 `command`；
- 模式命令还必须匹配 `mode,state`；
- 监控控制必须匹配 `request_id,mode,state,audio_session_id`；
- 打断必须匹配 `event_id` 及原播放上下文；
- 通知 BEGIN/END ACK 必须匹配 `request_id,notify_id,playback_id`；
- `ota_ack` 必须匹配 `update_id`，且只使用 `accepted` 表示接受或拒绝；
- `ota_result_ack` 必须匹配 `update_id,ok`。

### 16.2 重试

- 未收到 ACK 时先确认 WebSocket 仍为同一连接 generation；
- 同一连接上的安全重试复用同一 ID 和完全相同 payload；
- 不得并行发送多个新 ID 来实现同一状态切换；
- 重连后旧在途普通命令失效，使用新 ID 重新获取目标状态；
- OTA 终态是例外，它有持久化上下文和明确的跨重连重放协议。

### 16.3 错误与未知消息

无效 JSON：

```json
{ "type": "error", "error": "Invalid JSON: ..." }
```

当前网关对未知上行 JSON 类型发送：

```json
{
  "type": "ack",
  "session_id": "cloud_session_001",
  "received_type": "unknown_type"
}
```

这只是兼容兜底，不表示业务成功。正式消息必须使用其专用 ACK。

设备 JSON 解析保持 `additionalProperties:true` 的向前兼容策略，但关键 ID、枚举、布尔值和数字范围必须严格校验。安全敏感协议（监控 ACK、模式协议、OTA）还会拒绝重复关键字段、尾随非空白数据或过长字段。

### 16.4 敏感字段

以下字段必须脱敏：

```text
hello.device_secret
X-Bridge-Token
device_command.password
ota_update.url（若 URL 含临时签名查询参数）
```

## 17. 已知兼容差异

| 项目 | 基础 Schema | 当前实现 | 对接要求 |
| --- | --- | --- | --- |
| `pong` 时间字段 | `now_ms`, `server_ms` | 当前网关发送 `now` | 设备两者均兼容；新云端建议遵循 Schema |
| `request_id` 长度 | 最多 39 字符 | 模式协议缓冲区支持 47 字符 | 正式通用消息控制在 39 字符内 |
| 模式、barge-in | Schema 未完整覆盖 | 设备和网关已实现 | 以本文及对应源码为准 |
| 主动通知播放 | Schema、设备状态机和 host-check 已覆盖 | 当前 `cloud-gateway` 尚无通知 BEGIN/Binary/END 编排 | 云端实现第 9 节完整事务并通过专项验收后再启用 |
| `listen_ack` | Schema 未列出 | 网关始终可发送 | 设备不等待 |
| `wifi_*_ack` | Schema 未列出 | 网关可发送 | 设备可忽略 |
| `audio_stats` | Schema 未列出 | 网关周期发送 | 仅诊断进度 |
| `listen stop` | 协议正式存在 | 受构建宏控制，当前默认启用 | 生产必须启用 |
| `cloud_config_committed` | 旧 onboarding 兼容路径 | 当前动态 URL/device-secret 设备不发送 | 不属于当前正式设备协议 |
| camelCase 字段 | 部分旧网关可接受 | 设备解析少量别名 | 新协议统一 snake_case |

## 18. 云端实现验收清单

1. Upgrade、`hello.device_id + device_secret` 和能力协商均通过。
2. 首次认证连接接收 `firmware_report`，持久化后原样返回 `report_id`；重复报告幂等。
3. 同时正确处理 WebSocket control Ping/Pong 和 JSON `ping/pong`。
4. 按单条 Binary 中多个 NA 帧循环解析，校验 START、END、sequence、格式和长度。
5. 普通对话的 `audio_session_id -> turn_id -> playback_id` 关联无串轮。
6. 取消/迟到 `stop_playback` 不能影响新轮次。
7. 长回答播放期间不触发 120 秒聆听空闲睡眠；回复后继续正常对话循环。
8. 主动通知严格执行准备 ACK、BEGIN ACK、NA 音频、三重 END 和取消/断线隔离。
9. 会议 30 分钟静音退出，完整接收 END、stop 和关闭 ACK。
10. 监控 120 秒静音先 ACK pause 再收尾，弱人声恢复时先 ACK resume 再收新流。
11. 通知、会议、监控、OTA 和普通对话按本文规则互斥，冲突命令可预测地返回失败 ACK。
12. Wi-Fi 密码、设备密钥、桥接令牌和签名 URL 不进入日志。
13. OTA 严格校验 SemVer、size、SHA-256 和镜像身份，持久化结果后才发送精简 `ota_result_ack`；不发送取消命令，也不等待云端进度上报。
14. `DEVICE_NOT_BOUND` 使用唯一结构化 `code`；设备保留 Wi-Fi、进入单次重绑定，旧 connection epoch 不产生副作用。
15. WebSocket 意外断线后保存已收数据、标记异常结束；新连接不拼接旧普通音频或通知会话。
16. 未知字段可忽略，非法关键字段不会导致内存越界、状态机错乱或误判成功。

## 19. 源码与机器可读契约

本文件的关键依据：

```text
docs/protocol/schemas/nuwax-device-protocol.schema.json
docs/protocol/notification-playback-v1.md
docs/protocol/正式云端OTA协议与开发指南-v1.md
main/session_protocol.c
main/session_protocol.h
main/audio_frame.c
main/audio_frame.h
main/cloud_protocol_compat.c
main/cloud_connection_epoch_policy.h
main/ws_transport.c
main/conversation/session_dispatch.c
main/conversation/barge_in_dispatcher.c
main/conversation/notification_playback.c
main/conversation/notification_rx_barrier.c
main/audio_modes/meeting/meeting_protocol.c
main/audio_modes/meeting/meeting_mode.c
main/audio_modes/monitoring/monitoring_protocol.c
main/audio_modes/monitoring/monitoring_mode.c
main/nuwax_ota_new/nuwax_ota_device.c
main/nuwax_ota_new/nuwax_ota_manifest.c
main/nuwax_ota_new/nuwax_firmware_report.c
main/mcp_server.c
../cloud-gateway/cloud-gateway.js
```

基础 Schema 仍用于自动校验基础消息；本文补足 Schema 尚未覆盖的当前扩展。任何新增消息类型或 `device_command` 都应同时更新本文、Schema（适用时）、设备主机测试和网关契约测试。
