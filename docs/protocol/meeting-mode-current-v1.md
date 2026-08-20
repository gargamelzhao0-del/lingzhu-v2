# 会议模式设备/云端协议 v1（当前固件实现）

状态：7.0 设备端当前实现，可供云端对接  
更新时间：2026-07-21  
模式名称：`meeting`

本文只规定会议模式新增的控制语义和模式行为。设备与云端已经正常使用的 WebSocket、`hello`、`listen`、NA v1 二进制音频和 WAV 落盘协议继续沿用，不在本文重复定义。

## 1. 当前实现结论

1. 设备 UI 和云端都可以开启、关闭会议模式。
2. 设备 UI 控制使用 `audio_mode_request` / `audio_mode_request_ack`。
3. 云端主动控制使用 `device_command` / `device_command_ack`，`command` 固定为 `set_audio_mode`。
4. 会议模式开启后连续上传所有成功读取的音频帧，不因静音、弱声或无人声删除帧。
5. 会议期间普通 ASR、智能体回复、TTS 播报、触摸聆听和自动追问均被设备隔离。
6. 普通对话、会议模式和监控模式互斥，同一时刻只能有一个麦克风及上行会话所有者。
7. 当前协议不使用 `revision`、`base_revision`、`meeting_id` 或 `audio_mode_state`。

设备当前在 `hello.features` 中声明：

```json
{
  "meeting_mode_v1": true
}
```

## 2. ID 约定

- `request_id` 由请求发起方生成，响应方必须原样回显。
- `request_id` 为 1 至 47 个 ASCII 字符，使用 `[A-Za-z0-9_.-]`。
- 同一 WebSocket 连接内，相同 `request_id` 只能表示相同请求内容；重试必须复用原 ID。
- `audio_session_id` 为 1 至 47 个 ASCII 字符，使用相同字符集。
- `audio_session_id` 可由云端提供；未提供时设备生成 `meeting_stream_*`。
- 一次会议的 `listen start` 与 `listen stop` 使用同一 `audio_session_id`；重新开启必须使用新 ID。

## 3. 设备 UI 开启会议

### 3.1 设备发送开启请求

```json
{
  "type": "audio_mode_request",
  "request_id": "meeting_open_1_a1b2c3d4",
  "mode": "meeting",
  "state": "enabled",
  "interrupt_current_turn": true
}
```

字段说明：

- `interrupt_current_turn` 是可选字段。
- 当前设备仅在发送请求时检测到正在播报才携带 `true`。
- 设备在等待云端批准前就会本地请求约 200 ms 淡出并取消当前播放。

### 3.2 云端批准

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "meeting_open_1_a1b2c3d4",
  "ok": true,
  "mode": "meeting",
  "state": "enabled",
  "audio_session_id": "meeting_audio_001",
  "current_turn_cancelled": true
}
```

- `audio_session_id` 可选，但建议云端提供。
- `current_turn_cancelled` 可选，只表示云端是否取消了原对话轮次，不影响设备是否进入会议。
- 设备只接受与待处理请求 `request_id` 完全一致、`ok:true` 且 `state:"enabled"` 的批准。
- 设备等待批准的最长时间为 5000 ms；超时、拒绝或非法 ACK 均回到关闭状态，不开始会议采集。

### 3.3 云端拒绝

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "meeting_open_1_a1b2c3d4",
  "ok": false,
  "mode": "meeting",
  "state": "disabled",
  "reason": "meeting_mode_unavailable"
}
```

`reason` 为简短错误原因，当前设备接收缓冲区最多保存 31 个 ASCII 字符。

## 4. 云端主动开启会议

云端发送：

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_meeting_open_001",
  "mode": "meeting",
  "state": "enabled",
  "interrupt_current_turn": true,
  "audio_session_id": "meeting_audio_cloud_001"
}
```

- `interrupt_current_turn` 和 `audio_session_id` 均可省略。
- 无论是否携带 `interrupt_current_turn`，当前设备发现正在播报时都会本地淡出并取消播放；该字段主要用于云端同步取消原对话轮次。
- 若省略 `audio_session_id`，设备自行生成。

设备只有在 `listen start` 已成功发送、麦克风已初始化、上行队列已启动并进入 ACTIVE 后才回复成功：

```json
{
  "type": "device_command_ack",
  "request_id": "cloud_meeting_open_001",
  "command": "set_audio_mode",
  "ok": true,
  "mode": "meeting",
  "state": "enabled",
  "message": "active"
}
```

当前开启 ACK 不携带 `audio_session_id`。云端必须从此前收到的 `listen start` 中取得实际 ID。

## 5. 开启后的实际消息顺序

### 5.1 UI 开启

```text
设备本地淡出（仅正在播报时）
-> audio_mode_request enabled
<- audio_mode_request_ack enabled
-> 最多等待 500 ms 让播放收尾
-> listen start, mode=meeting
-> 连续上传既有 NA v1 音频
```

### 5.2 云端主动开启

```text
<- device_command set_audio_mode enabled
设备本地淡出（仅正在播报时）
-> 最多等待 500 ms 让播放收尾
-> listen start, mode=meeting
-> device_command_ack ok=true, state=enabled, message=active
-> 连续上传既有 NA v1 音频
```

`listen start` 包含：

```json
{
  "type": "listen",
  "state": "start",
  "mode": "meeting",
  "audio_session_id": "meeting_audio_001",
  "request_id": "meeting_open_1_a1b2c3d4"
}
```

云端继续按现有音频会话协议创建接收器并返回 `listen_ack`。当前设备不解析或等待 `listen_ack`，因此云端必须在处理 `listen start` 时同步建立接收上下文，不能等后续业务 ACK 才准备接收 binary 音频。

## 6. 会议期间的音频行为

1. 当前固件使用 PCM16、16 kHz、单声道、16 ms 标称帧长，并沿用现有 NA v1 上行。
2. 所有 NA 头部多字节整数及 PCM16 样本均为 little-endian。
3. 当前通常每个 WebSocket binary message 聚合 4 个 NA 帧；云端继续按 `payload_len` 循环解析。
4. I2S 部分读取时，合法帧的 `payload_len` 可能小于完整帧的 512 字节；当前头部 `frame_duration_ms` 和 `timestamp_ms` 仍按 16 ms 递增。云端应以实际 PCM 字节数计算录音样本数和 WAV 时长。
5. 第一帧携带 START，最后一个真实音频帧尽最大努力携带 END。
6. `VAD_SPEECH` 只表示设备能量规则判定出的疑似人声，不决定帧是否上传。

当前会议专用处理为：MIC1/slot0、1.0 倍配置输入增益、慢速 DC 去除、能量型疑似人声标记和最高约 +1.58 dB 的缓慢弱人声增益。当前没有会议专用 AEC、频域降噪、静音门或强杂音抑制。

## 7. 30 分钟不活动退出

当前设备按以下实际规则计算不活动时间：

- 每个成功读取且判定为疑似人声的帧立即将无人声计时清零；
- 其他所有成功读取帧都继续累计，包括安静帧、风噪、音乐和机械背景声；
- 麦克风无效帧既不增加也不清零计时；
- 连续 30 分钟没有判定到疑似人声后结束会议。协议原因继续使用
  `quiet_timeout`，仅用于兼容已经部署的云端协议。

当前疑似人声判定仍是设备端能量规则，不是神经网络人声分类器；特别强且具有人声能量特征的噪声仍可能被误判为人声，但普通非安静环境声不再直接重置计时。

## 8. 设备 UI 或本地关闭

UI 关闭、`quiet_timeout` 或活动采集期间的本地错误触发停止后，设备按以下顺序收尾：

```text
停止继续采集
-> 最后一个真实帧尽最大努力携带 END
-> 排空并结束上行队列
-> 释放麦克风
-> listen stop, mode=meeting
-> audio_mode_request disabled
```

`listen stop`：

```json
{
  "type": "listen",
  "state": "stop",
  "mode": "meeting",
  "audio_session_id": "meeting_audio_001"
}
```

本地关闭通知：

```json
{
  "type": "audio_mode_request",
  "request_id": "meeting_close_2_e5f6a7b8",
  "mode": "meeting",
  "state": "disabled",
  "reason": "ui_close"
}
```

常见 `reason`：

| 值 | 含义 |
| --- | --- |
| `ui_close` | 用户在设备 UI 关闭 |
| `quiet_timeout` | 连续 30 分钟未检测到疑似人声；字段名为兼容保留 |
| `mic_read_timeout` | 连续空读取达到设备上限 |
| `uplink_enqueue_failed` | 上行入队失败 |
| `uplink_finish_failed` | 上行收尾失败 |
| `listen_stop_failed` | `listen stop` 发送失败 |

云端回复：

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "meeting_close_2_e5f6a7b8",
  "ok": true,
  "mode": "meeting",
  "state": "disabled"
}
```

设备本地关闭立即生效，不等待该 ACK。当前关闭通知不携带 `audio_session_id`，云端应使用此前的 `listen stop` 和当前连接上下文关联。

若会议在麦克风或上行尚未成功启动前失败，当前设备可能只发送已经能够发送的 `listen stop`，不保证再发送本地 `audio_mode_request disabled`。云端必须允许未进入 ACTIVE 的会话按启动失败收尾。

## 9. 云端主动关闭会议

云端发送：

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_meeting_close_001",
  "mode": "meeting",
  "state": "disabled"
}
```

设备先完成 END、上行队列收尾、麦克风释放和 `listen stop`，然后回复：

```json
{
  "type": "device_command_ack",
  "request_id": "cloud_meeting_close_001",
  "command": "set_audio_mode",
  "ok": true,
  "mode": "meeting",
  "state": "disabled",
  "message": "disabled"
}
```

当前设备不等待云端的 `listen_ack stop` 才发送关闭 ACK；同一 WebSocket 上的设备发送顺序仍为 `listen stop` 在前、`device_command_ack` 在后。

## 10. 云端命令 ACK 语义

| `ok` | `state` | `message` | 含义 |
| --- | --- | --- | --- |
| true | enabled | `active` | 新会议已经进入 ACTIVE |
| true | enabled | `already_active` | 收到开启命令时已经 ACTIVE |
| true | disabled | `disabled` | 关闭及资源收尾完成 |
| true | disabled | `already_disabled` | 收到关闭命令时已经关闭 |
| false | disabled | `meeting_busy` | STARTING/STOPPING 等冲突状态 |
| false | disabled | `superseded_by_disable` | 待完成的开启被关闭命令替代 |
| false | disabled | `start_cancelled` | 开启尚未进入 ACTIVE 就被本地操作取消 |
| false | disabled | `capture_failed` | 麦克风、内存或上行启动/运行失败 |
| false | 请求目标值 | `control_queue_full` | 控制事件无法入队 |

`ok:false` 时云端必须以 `ok` 和 `message` 判断结果，不能把 `state` 当作可信的当前设备状态。当前实现没有结构化 `error_code`。

相同 `request_id` 和相同内容的在途重试不会创建第二场会议；不同 ID 的重复开启在 ACTIVE 时返回 `already_active`。

## 11. 断线与互斥

- WebSocket 断开后设备立即清空会议控制队列并回到关闭状态。
- 连接已断开时无法保证发送 END、`listen stop` 或关闭通知。
- 重连后设备不会自动恢复上一场会议，必须重新发起开启并使用新的 `audio_session_id`。
- 会议 STARTING、ACTIVE、STOPPING 期间，设备丢弃迟到的普通对话事件和 TTS binary 音频。
- 云端也必须在会议期间停止继续生成或发送普通对话 TTS。

## 12. 云端最小对接清单

1. 识别 `meeting_mode_v1:true`。
2. 处理设备 UI 的 `audio_mode_request enabled/disabled` 并原样回显 `request_id`。
3. 需要时发送 `set_audio_mode meeting enabled/disabled`，并按 `request_id` 匹配 ACK。
4. 看到 `listen mode:meeting` 后使用已有音频接收链路，但不得把会议音频送入普通 ASR、智能体和 TTS。
5. UI 开启时若 `interrupt_current_turn:true`，取消旧对话轮次并停止继续下发旧 TTS。
6. 正常关闭时等待 END、`listen stop` 和对应控制收尾；断线或序号缺口时把文件标记为异常结束。

## 13. 最简摘要

```text
设备 UI：audio_mode_request <-> audio_mode_request_ack
云端控制：device_command/set_audio_mode <-> device_command_ack
已有音频链路：listen start -> NA binary -> END -> listen stop
```
