# Nuwax AI Partner 7.0 全工程 WebSocket 协议

本文依据当前固件源码、协议解析器和发送函数整理。它描述设备已经落地的 WebSocket 消息、二进制帧、状态边界、重试和错误语义，作为正式云端对接总表。

## 1. 连接和通用规则

设备通过 `ws://` 或 `wss://` 连接 `/api/device/ws`。握手请求携带 `X-Bridge-Token: device_secret`。连接成功后设备发送 `hello`，云端必须返回 `hello` 且 `ok:true`；设备以 `device_id`、认证状态和当前 connection epoch 校验响应。

控制消息为 UTF-8 JSON Text Frame，音频为 Binary Frame。JSON 必须是单个完整对象，关键字段重复、类型错误、非法 ID、非空尾随字符和超长固定字段必须拒绝。未知可选字段可以忽略。

`request_id` 用于请求幂等；同一请求重试必须复用原 ID 和相同语义。`audio_session_id` 标识一次音频流，跨 WebSocket 重连不得复用。旧连接的 ACK 不得作用于新 connection epoch。

## 2. Hello 与心跳

设备发送：

```json
{
  "type": "hello",
  "version": 1,
  "device_id": "nuwax_xxx",
  "device_secret": "<secret>",
  "transport": "websocket",
  "features": {
    "audio_upload": true,
    "meeting_mode_v1": true,
    "monitoring_mode_v1": true,
    "monitoring_pause_resume_v1": true,
    "offline_audio_replay_v1": true,
    "audio_playback": true,
    "notify_playback_v1": true,
    "ota_v1": true,
    "mcp": true
  },
  "audio_params": {
    "input_format": "pcm16",
    "output_format": "pcm16",
    "codec": "pcm16",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration_ms": 20
  }
}
```

实际 `hello` 还会携带固件身份、AEC、I2S、扬声器和硬件 profile。`device_secret` 只能用于认证，不得写入业务日志。

云端响应至少包含：

```json
{
  "type": "hello",
  "ok": true,
  "authenticated": true,
  "device_id": "nuwax_xxx",
  "session_id": "cloud_session_xxx"
}
```

SD 补传能力不是通过顶层 `offline_audio_replay_v2` 字段协商。当前设备在 `hello.features` 中发送 `offline_audio_replay_v1`，云端在成功 hello/application-ready 响应中必须返回：

```json
{
  "type": "hello",
  "ok": true,
  "offline_audio": {
    "supported": true,
    "protocol_version": 1,
    "archive_only": true
  }
}
```

设备只有在当前 connection epoch 收到上述三项能力均有效时才启动正式 SD replay；缺失、版本不为 1 或 `archive_only:false` 时保留 `.ready`，不发送补传。

设备周期发送 JSON 心跳：

```json
{ "type": "ping", "now_ms": 123456789 }
```

云端回复 `pong`，可携带 `now_ms` 或 `server_ms`。WebSocket 控制 Ping/Pong 由传输层自动处理。

## 3. 普通对话音频

### 3.1 监听会话

设备发送：

```json
{
  "type": "listen",
  "state": "start",
  "mode": "conversation",
  "audio_session_id": "conversation_audio_001",
  "request_id": "listen_001"
}
```

结束时发送 `state:"stop"`，并先发送最后一个带 END 标志的音频帧。云端应回复 `listen_ack` 或建立等价的接收上下文；设备端的普通对话链路按 connection epoch 保护终止操作。

### 3.2 NA v1 Binary Frame

默认帧为 16 kHz、单声道、PCM16、20 ms、320 samples：

| 偏移 | 长度 | 内容 |
|---:|---:|---|
| 0 | 2 | `NA` |
| 2 | 1 | version=`1` |
| 3 | 1 | codec：`1=PCM16`，`2=Opus` |
| 4 | 1 | flags：`START=0x01`、`END=0x02`、`VAD_SPEECH=0x04` |
| 5 | 1 | channels |
| 6 | 2 | sample_rate，小端 |
| 8 | 2 | frame_duration_ms，小端 |
| 10 | 4 | sequence，小端 |
| 14 | 4 | timestamp_ms，小端 |
| 18 | 2 | payload_bytes，小端 |
| 20 | 2 | 保留，填零 |
| 22 | N | 音频 payload |

云端必须循环解析一个 Binary Frame 中可能包含的多个 NA 帧，校验长度、codec、采样参数、START/END、sequence 和 session 绑定。重复帧按 `device_id + audio_session_id + sequence` 幂等；缺口记录为 `audio_gap`，不得伪造数据。

## 4. 会议模式

会议和普通对话、监控、播报互斥，同一时间只有一个音频 owner。状态为：

```text
disabled → starting → active → stopping → disabled
                  └────→ error → disabled
```

### 4.1 设备 UI 请求

```json
{
  "type": "audio_mode_request",
  "request_id": "meeting_open_001",
  "mode": "meeting",
  "state": "enabled",
  "interrupt_current_turn": true,
  "reason": "device_ui"
}
```

云端以 `audio_mode_request_ack` 回复：

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "meeting_open_001",
  "ok": true,
  "mode": "meeting",
  "state": "enabled",
  "audio_session_id": "meeting_audio_001",
  "current_turn_cancelled": true
}
```

开启 ACK 只授权设备继续启动，不代表音频已经 active。拒绝时 `ok:false` 并给出 `reason` 或 `error_code`。

### 4.2 云端主动控制

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "meeting_cloud_001",
  "mode": "meeting",
  "state": "enabled",
  "interrupt_current_turn": true,
  "audio_session_id": "meeting_audio_001"
}
```

设备完成资源准备后回复：

```json
{
  "type": "device_command_ack",
  "command": "set_audio_mode",
  "request_id": "meeting_cloud_001",
  "ok": true,
  "mode": "meeting",
  "state": "enabled",
  "message": "active"
}
```

### 4.3 会议音频时序

```text
set_audio_mode enabled
→ 停止普通对话和播报
→ listen start(mode=meeting)
→ 初始化麦克风和可靠上行队列
→ 连续发送 NA 帧（包括静音）
→ 关闭时发送 END、排空队列、listen stop
```

会议不使用 VAD 丢弃音频。云端不得将会议音频送入普通对话自动回复链路。

## 5. 监控模式

监控只能由云端 `device_command/set_audio_mode` 开启和关闭：

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "monitor_open_001",
  "mode": "monitoring",
  "state": "enabled"
}
```

设备启动新的 `audio_session_id`，发送 `listen start(mode=monitoring)`，随后回复 `device_command_ack`。监控期间不运行普通 ASR、智能体回复或 TTS。

监控静音时可请求暂停：

```json
{
  "type": "monitoring_capture_control",
  "request_id": "monitor_pause_001",
  "mode": "monitoring",
  "state": "pause",
  "audio_session_id": "monitor_audio_001",
  "silence_ms": 120000,
  "reason": "no_speech"
}
```

云端回复：

```json
{
  "type": "monitoring_capture_control_ack",
  "request_id": "monitor_pause_001",
  "mode": "monitoring",
  "state": "pause",
  "audio_session_id": "monitor_audio_001",
  "ok": true
}
```

只有匹配 ACK 成功后设备才结束当前流并暂停上传。检测到人声后使用 `state:"resume"` 和新的 `audio_session_id`，云端 ACK 后再发送新的 `listen start`。控制 ACK 必须匹配 request_id、mode、state 和 audio_session_id。

## 6. 会议/监控状态与冲突

当前代码主要通过 `device_command_ack`、模式控制器状态和 `listen` 生命周期表达实际状态；当前源码没有统一的 `audio_mode_state` 发送函数，云端不能假设该消息一定存在。云端应以设备命令 ACK、`listen` 生命周期和实时音频帧共同判断是否正在采集，不得只依据命令 ACK。

冲突时设备拒绝新独占模式并返回：

```json
{
  "type": "device_command_ack",
  "request_id": "monitor_open_001",
  "command": "set_audio_mode",
  "ok": false,
  "mode": "monitoring",
  "state": "disabled",
  "message": "audio_mode_busy"
}
```

常见原因：`audio_mode_busy`、`capture_failed`、`playback_stop_failed`、`control_queue_full`、`stale_request`。

## 7. SD 在线镜像与离线归档

当 SD 可用时，会议实时帧和监控有效流式帧会异步写入 SD；SD writer 使用有界队列，不能阻塞实时采集。无 SD、SD 满、拔卡或写入失败时，实时上传继续，设备记录 `dropped_frames` 或 `storage_gap`。

归档文件：

```text
96 字节 NWXOFF1 Header
→ 多个 [4 字节长度 + NA v1 帧]
→ 32 字节 NWXEND1 Footer
```

文件状态：`.part` 表示写入中，`.ready` 表示已封存等待补传。断网或异常结束使用 `complete=0`；正常结束且 writer 无降级使用 `complete=1`。

## 8. SD 补传协议

设备在正式连接上发送：

```json
{
  "type": "offline_audio_begin",
  "mode": "archive_only",
  "request_id": "offline_begin_001",
  "segment_id": "stable_segment_id",
  "total_bytes": 1048576,
  "crc32": 123456789
}
```

云端回复 `offline_audio_begin_ack`，其中 `next_offset` 为已经可靠保存的连续前缀。随后设备发送 `NWXCH1` Binary Frame：64 字节头加最多 8064 字节归档内容。云端先持久化再回复 `offline_audio_chunk_ack` 和新的 `next_offset`。

设备最后发送：

```json
{
  "type": "offline_audio_commit",
  "mode": "archive_only",
  "request_id": "offline_commit_001",
  "segment_id": "stable_segment_id",
  "total_bytes": 1048576,
  "crc32": 123456789
}
```

云端验证长度、Footer、frame_count、payload_bytes 和 CRC 后回复 `offline_audio_commit_ack(ok:true)`。设备只有收到匹配成功 ACK 才删除 `.ready`；任何失败都保留文件。云端幂等键为 `device_id + segment_id`。

`archive_only` 只允许归档存储、校验和索引，禁止进入 ASR、会议转写、监控事件、告警和计费流水线。

## 9. 普通对话控制协议

设备发送：

| 消息 | 主要字段 | 用途 |
|---|---|---|
| `text_request` | `request_id,text` | 文本对话请求 |
| `cancel_turn` | `request_id,turn_id,reason` | 取消当前对话轮 |
| `barge_in` | `state,event_id,parent_turn_id,playback_id,audio_session_id,onset_sequence,current_sequence,confidence_q15` | 打断确认与序列关联 |
| `input_event` | `source,action,touch` | 触摸输入 |
| `pc_status_request` | `request_id` | 查询 PC 状态 |
| `status_app_request` | `request_id,app` | 查询指定应用状态 |
| `volume_changed` | `event_id,volume_percent,source` | 上报音量改变 |
| `wifi_scan_result` | `request_id,ok,count,networks` | 上报 Wi-Fi 扫描结果 |
| `wifi_status` | `request_id,phase,ssid,ip,ok,message` | 上报 Wi-Fi 状态 |
| `device_error_report` | 事件、错误码、上下文 | 上报设备错误 |

云端返回并由设备解析的事件包括：`asr_partial`、`asr_final`、`asr_result`、`text_response`、`audio_stats`、`audio_session`、`playback_begin`、`playback_end`、`tts_result`、`tts_skip`、`cancel_turn_ack`、`input_event_ack`、`error` 和 `pong`。

其他专用 ACK 由对应模块直接消费，不进入普通会话事件队列：

| ACK | 关联字段 | 消费模块 |
|---|---|---|
| `audio_mode_request_ack` | `request_id,mode,state,ok,audio_session_id?` | 会议/监控控制器 |
| `monitoring_capture_control_ack` | `request_id,mode,state,audio_session_id,ok` | 监控控制器 |
| `offline_audio_begin_ack` | `request_id,segment_id?,ok,next_offset` | SD spool |
| `offline_audio_chunk_ack` | `request_id,segment_id?,ok,next_offset` | SD spool |
| `offline_audio_commit_ack` | `request_id,segment_id?,ok` | SD spool |
| `firmware_report_ack` | `report_id,persisted` | 固件报告 |
| `device_error_report_ack` | `event_id,accepted` | 错误报告 |
| `ota_result_ack` | `update_id,ok` | OTA |
| `notify_playback_begin_ack` / `notify_playback_end_ack` | `request_id,notify_id,playback_id,ok` | 通知播放 |

## 10. 云端设备命令

所有命令外层格式为：

```json
{
  "type": "device_command",
  "command": "<command>",
  "request_id": "cmd_001"
}
```

当前设备分发器支持：

| command | 关键字段 | 设备行为 |
|---|---|---|
| `set_audio_mode` | `mode,state,audio_session_id` | 开关会议或监控 |
| `set_volume` | `volume_percent` 或 `volume_db,persist` | 设置并可持久化音量 |
| `set_wake_word` | `wake_word` 或字符串数组，`persist` | 更新唤醒词 |
| `reset_wake_word` | 无 | 恢复默认唤醒词 |
| `wifi_scan` | 无 | 扫描并发送 `wifi_scan_result` |
| `wifi_connect` | `ssid,password` | 连接 Wi-Fi；密码不得回显 |
| `set_state` | `state` | 设置 `sleeping`、`wake_only` 等设备状态 |
| `play_music` | `url,format,request_id` | 启动网络音乐播放 |
| `stop_playback` | `turn_id,playback_id` | 停止或幂等结束播放 |
| `ota_update` | `update_id,url,version,size_bytes,image_sha256` | 进入 OTA 事务 |
| `start_ble_provisioning` | 无 | 启动 BLE 配网 |
| `sleep_conversation` | `turn_id` 可选 | 启动可确认的休眠事务 |
| `set_emotion` | `emotion,duration_ms` | 设置设备表情 |
| `stop_music` | `request_id` | 停止音乐播放 |
| `show_status` | `title,lines[]` | 显示状态页 |
| `set_audio_cleaner` | `enabled,persist` | 设置音频清理 |
| `set_l3_interrupt` | `enabled,persist` | 设置 L3 打断策略 |
| `p3_ref_health` | 诊断参数 | 执行 P3 参考健康检查 |
| `p3_softref_ab` | 诊断参数 | 执行软件参考 A/B 测试 |
| `p3_hardref_ab` | 诊断参数 | 执行硬件参考 A/B 测试 |
| `p3_nearend_candidate` | 诊断参数 | 执行近端候选测试 |
| `p3_interrupt_timing` | 诊断参数 | 执行打断时序测试 |
| `p5_fullduplex_overlap_probe` | `audio_session_id,duration_ms,delay_samples` | 执行全双工探针 |
| `p5_during_playback_candidate` | `audio_session_id,duration_ms` | 执行播放中采集探针 |
| `set_acceptance_capture` | `mode,duration_ms,delay_samples,audio_session_id` | 仅验收构建启用，用于原始音频/参考信号采集 |
| `prepare_notify_playback` | `notify_id` | 准备主动通知播放事务 |

不支持或参数非法的命令必须返回 `device_command_ack(ok:false)`，不得执行部分副作用。

其中 `set_acceptance_capture`、`prepare_notify_playback` 和 P3/P5 命令属于验收或诊断接口，不是普通 App 业务能力；生产云端只有在设备能力和测试窗口明确开启时才可发送。

## 11. 播放和通知

设备上报普通播放生命周期：

```json
{
  "type": "playback_begin",
  "request_id": "play_001",
  "turn_id": "turn_001",
  "playback_id": "playback_001",
  "provider": "cloud_tts",
  "sample_rate": 16000,
  "channels": 1,
  "frame_duration_ms": 20
}
```

结束使用 `playback_end`，携带 frames、bytes 和 reason。主动通知使用 `notify_playback_begin` / `notify_playback_end`，云端必须处理对应 ACK、取消和 BEGIN/END 音频边界。通知 Binary 音频不能与普通对话或会议音频串流。

音乐播放状态还会使用：

```json
{
  "type": "music_playback_status",
  "request_id": "music_001",
  "state": "playing",
  "reason": "started"
}
```

`state` 为 `playing`、`completed`、`failed` 或 `cancelled`。主动通知取消时设备发送 `notify_playback_cancelled`，携带 `request_id`、`notify_id`、`playback_id` 和 `reason`。

## 12. MCP

MCP 使用：

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

设备实现 `initialize`、`tools/list`、`tools/call`。工具清单按运行时 capability 发布，当前包括 `self.get_device_status`、`self.ui.set_emotion`，以及硬件 capability 同时满足时的 `self.servo.head_shake`。会议、监控、OTA 和配网仍使用 `device_command`，不通过 MCP 猜测调用。

## 13. 固件报告与 OTA

固件报告：

```json
{
  "type": "firmware_report",
  "report_id": "report_001",
  "project_name": "NuwaxAIPartner",
  "version": "7.0.x",
  "release_sequence": 7000098
}
```

云端持久化后回复 `firmware_report_ack`。重复报告必须幂等。

`firmware_report_ack` 必须至少包含 `report_id` 和 `persisted`；设备只接受当前连接中匹配 report_id 且确认已持久化的 ACK。

`firmware_report` 的实际上行还包含 `device_id`、`device_secret`、`reset_reason` 和 `firmware{firmware_type,version,image_sha256}`；这些字段属于启动身份报告，云端必须按 report_id 幂等保存，日志不得记录 device_secret。

OTA 命令使用 `device_command/ota_update`。设备先回复：

```json
{ "type": "ota_ack", "update_id": "ota_001", "accepted": true }
```

完成后发送：

```json
{ "type": "ota_result", "update_id": "ota_001", "ok": true, "message": "image_staged" }
```

云端持久化后回复 `ota_result_ack`。OTA 期间不得开启会议、监控或普通长音频采集。

## 14. 重连、错误和安全

- WebSocket 断开时，当前普通音频、会议和监控流在该 epoch 结束；新连接不得接收旧流迟到 ACK。
- 实时发送失败按 transport 状态和 heartbeat 判断是否重连；已产生不确定字节进度的帧不能盲目重试。
- 正常终止发送 `listen stop` 时最多执行有限次安全重试。
- SD 补传超时保留 `.ready`，不得删除；正式云端未部署补传协议时，设备应暂停本轮 replay，不阻塞实时模式恢复。
- `X-Bridge-Token`、`device_secret`、Wi-Fi password、OTA 签名 URL 不进入日志。
- `DEVICE_NOT_BOUND`、认证失败和无效 hello 必须进入对应配网/重绑定流程，不得将旧连接继续视为有效。

设备错误上报使用 `device_error_report`，包含 `event_id`、`severity`、`component`、`code`、可选 `result/state/generation/turn_id` 和 `uptime_ms`。云端持久化后返回 `device_error_report_ack`，其中使用 `event_id` 和 `accepted` 关联；错误上报失败不能阻塞实时音频。`wifi_scan_ack`、`wifi_status_ack`、`input_event_ack` 和 `audio_stats` 只用于确认或诊断，不改变音频状态机。

## 15. 当前实现边界

以下内容在设备端已有基础实现，但正式云端仍需对接确认：

1. SD `offline_audio_begin/chunk/commit` 及 `archive_only` 语义；
2. 会议/监控的云端权限、存储和重复事件隔离；
3. 跨重启的 `boot_id`、Unix 时间和累计离线会议时长；
4. `audio_mode_state`、离线会议 8 小时结束事件和存储缺口上报；
5. 云端对多个 NA 帧、END、sequence gap 和 replay 幂等的严格处理。

另外，源码当前 `hello.features` 使用的字段名仍是 `offline_audio_replay_v1`，而补传控制消息已经带有 `mode:"archive_only"`。在正式云端统一前，不要把 feature 名直接改成 v2；建议云端同时兼容该 v1 能力名，并以 begin/ACK 中的 `archive_only` 作为实际处理模式。

正式联调必须先完成 hello 能力协商，再验证会议/监控控制、实时音频、SD 补传、CRC、重连和异常闭环。

## 16. 源码逐项核对表

| 协议区域 | 主要实现文件 | 当前核对结果 |
|---|---|---|
| WebSocket 握手、epoch、Ping/Pong | `main/ws_transport.c`, `main/session_protocol.c` | 已实现 |
| Hello 能力和音频参数 | `main/session_protocol.c` | 已实现；能力按构建宏变化 |
| 普通对话、ASR、文本、播放事件 | `main/session_protocol.c`, `main/conversation/session_dispatch.c` | 已实现 |
| 会议控制和连续音频 | `main/audio_modes/meeting/*` | 已实现 |
| 监控控制和 pause/resume | `main/audio_modes/monitoring/*` | 已实现 |
| NA v1 Binary | `main/audio_frame.c`, `main/conversation/audio_uplink.c` | 已实现 |
| SD 镜像、归档、补传 | `main/offline_audio_spool.c` | 设备端已实现，正式云端待接入 |
| 错误上报 ACK | `main/device_error_report.c` | 已实现；ACK 使用 `accepted` |
| 固件报告 ACK | `main/nuwax_ota_new/nuwax_firmware_report.c` | 已实现；ACK 必须 `persisted:true` |
| OTA ACK、结果 ACK | `main/nuwax_ota_new/nuwax_ota_device.c` | 已实现 |
| MCP | `main/mcp_server.c` | 已实现 |
| 统一 `audio_mode_state` 上报 | 全工程 | 未发现统一发送函数，不能作为当前必需消息 |
| 跨重启 boot_id/绝对时间 | 全工程 | 未实现 |
| 离线会议累计时长 NVS 恢复 | 全工程 | 未实现，仅有运行时 8 小时计时 |
