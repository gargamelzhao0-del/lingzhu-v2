# 会议/监控模式完整通信协议 v2

> 后续协议设计，尚未完整部署。当前固件与正式云端的实施契约见
> [2026-09-10 当前契约](meeting-monitoring-sd-current-20260910.md)。
> 不应仅依据本文要求当前固件提供 revision、audio_mode_state 或 listen_ack 门禁。

## 1. 范围与传输

协议运行在设备已认证的 WebSocket 上，控制消息使用 UTF-8 JSON 文本帧，音频使用 NA v1 二进制帧。设备只有一个音频采集 owner，实时上传、SD 镜像和历史补传是三个相互隔离的下游。

会议模式连续上传完整 PCM 音频；监控模式由本地人声门控决定进入或退出流式上传，并保留必要的前置帧。

## 2. 标识与通用规则

所有 ID 使用 ASCII `A-Z a-z 0-9 _ - .`，不得静默截断。`request_id` 用于请求幂等；`audio_session_id` 标识一次音频流；`meeting_id` 标识整场会议；`revision` 是云端期望状态版本。

重试必须复用相同 `request_id`。云端重复收到相同请求时返回相同结果，不得重复创建会话。设备拒绝过期 revision；本地关闭请求具有隐私优先级。

## 3. 状态

会议和监控均使用：

```text
disabled → starting → active → stopping → disabled
                         └────→ error → disabled
```

`active` 只表示设备已经完成监听建立并开始音频传输。ACK 不等于 active；云端必须等待设备的状态上报。

离线会议增加：

```text
offline_capturing → offline_finalizing → sleeping
```

离线采集最长 8 小时，达到上限后自动封存并进入正常休眠策略。

## 4. 开关控制消息

设备请求开启或关闭：

```json
{
  "type": "audio_mode_request",
  "request_id": "req_001",
  "mode": "meeting",
  "state": "enabled",
  "reason": "device_ui"
}
```

`mode` 为 `meeting` 或 `monitoring`，`state` 为 `enabled` 或 `disabled`。

云端回复：

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "req_001",
  "ok": true,
  "mode": "meeting",
  "state": "enabled",
  "meeting_id": "mtg_001",
  "audio_session_id": "aud_001",
  "revision": 12
}
```

失败时 `ok:false` 并返回 `error_code`。关闭请求必须接受，不能因 revision 过期阻止本地停止。

## 5. 云端主动控制

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cmd_001",
  "mode": "monitoring",
  "state": "disabled",
  "revision": 13
}
```

设备确认：

```json
{
  "type": "device_command_ack",
  "command": "set_audio_mode",
  "request_id": "cmd_001",
  "mode": "monitoring",
  "state": "disabled",
  "ok": true,
  "message": "accepted"
}
```

ACK 仅表示命令已接受，真实结果通过 `audio_mode_state` 上报。

## 6. 真实状态上报

```json
{
  "type": "audio_mode_state",
  "mode": "meeting",
  "state": "active",
  "meeting_id": "mtg_001",
  "audio_session_id": "aud_001",
  "revision": 12,
  "reason": "listen_ack_received",
  "dropped_frames": 0
}
```

设备重连完成 hello 后必须主动发送一次状态快照。`error` 状态必须包含 `error_code`；云端收到后停止显示录音中。

## 7. 音频会话控制

设备建立音频流：

```json
{
  "type": "listen",
  "state": "start",
  "mode": "meeting",
  "audio_session_id": "aud_001",
  "request_id": "listen_001"
}
```

云端返回 `listen_ack`，确认后设备才进入 `active`。结束时设备先发送所有尾帧，再发送：

```json
{
  "type": "listen",
  "state": "stop",
  "mode": "meeting",
  "audio_session_id": "aud_001",
  "request_id": "listen_stop_001"
}
```

云端不得把会议或监控音频转发到普通对话链路。

## 8. 实时音频帧

音频为 NA v1 PCM16，默认 16 kHz、单声道、20 ms。每帧包含 sequence、时间戳、flags 和采样数据。会议模式 sequence 连续；监控模式每个流独立计数。

云端必须校验帧长度、编码、sequence 和 session。重复帧按 `device_id + audio_session_id + sequence` 幂等处理。实时帧丢失必须记录 gap。

监控专用控制：

```json
{
  "type": "monitoring_capture_control",
  "request_id": "mc_001",
  "mode": "monitoring",
  "state": "pause",
  "audio_session_id": "aud_002",
  "silence_ms": 120000,
  "reason": "silence_timeout"
}
```

状态为 `pause` 或 `resume`，云端以 `monitoring_capture_control_ack` 回复。

## 9. SD 归档与补传

在线时每个实时帧同时异步写入 SD；断网时只写 SD；恢复网络后新帧继续实时上传，历史分段后台补传。补传使用独立的 `offline_audio_begin/chunk/commit` 事务，并且必须携带 `mode:"archive_only"`。

`archive_only` 数据只做归档、校验和幂等存储，不得触发 ASR、会议转写、监控事件、告警或重复计费。

设备只有收到 `offline_audio_commit_ack(ok:true)` 后删除 `.ready` 文件。断线、超时、CRC 错误或负 ACK 均保留文件。

云端使用 `device_id + session_id + segment_id` 幂等，按归档帧采集时间和 sequence 排序，不按网络到达顺序排序。

## 10. 异常处理

| 情况 | 设备处理 | 云端处理 |
|---|---|---|
| 无 SD | 继续实时上传，统计丢帧 | 记录 gap |
| SD 满/拔出 | 停止镜像，实时上传继续 | 接收 storage_gap |
| 网络断开 | 保持采集，写 SD | 等待后续补传 |
| 网络恢复 | 新帧实时上传，旧段后台补传 | 按时间排序、幂等入库 |
| 设备断电 | `.part` 恢复为不完整 `.ready` | 接受 incomplete 标记 |
| CRC 错误 | 保留本地文件 | 删除临时对象并要求重传 |
| 8 小时到期 | 封存会议并进入休眠 | 记录 offline_limit_reached |

## 11. 时间字段

当前归档帧至少包含设备单调时间和 sequence。正式版本应增加 `boot_id`、`capture_unix_ms` 和 `time_quality`，云端按采集时间重建跨重启时间线；未同步时间不得伪装成绝对时间。
