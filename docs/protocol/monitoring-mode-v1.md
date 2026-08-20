# 监控模式设备/云端协议 v1

状态：设备/云端协议冻结；7.0 设备端及 COM8 核心实机验收已通过，待正式云端联调  
更新时间：2026-07-22  
模式名称：`monitoring`

本文只规定监控模式新增的控制语义和模式行为。设备与云端已经正常使用的 WebSocket、`listen`、NA v1 二进制音频和音频落盘协议继续沿用，不在本文重复定义。

## 1. 当前实际边界

截至本文更新时间：

1. 设备端已实现独立的 `audio_modes/monitoring` 协议解析、状态机、麦克风所有权和连续采集上传链路。
2. 设备已在 `hello.features` 声明 `monitoring_mode_v1:true`；云端只能在收到该能力后发送监控开启命令。
3. 设备端协议、控制器和真实采集分支宿主机检查均已通过，ESP-IDF 6.0 的 ESP32-S3 完整固件构建已通过。
4. 测试网关能够通过通用 `/api/device/command` 发送控制命令，并接收 `listen mode:monitoring`、NA v1 音频及持续保存 WAV；监控音频不会进入普通 ASR、智能体和 TTS。
5. COM8 已完成云端启停、反复和幂等命令、播放中开启、171.136 秒连续录音、设备复位断线、重连不自动恢复及重连后再次开启测试；正常会话的 START/END、序号连续性、`listen stop` 和 WAV 均完整。
6. 测试网关已对缺 END、序号缺口、重复和乱序分别标记异常；正式云端仍需按本文完成权限、存储和故障策略联调。

设备能力声明为：

```json
{
  "monitoring_mode_v1": true
}
```

## 2. 固定业务规则

1. 标准模式名称固定为 `monitoring`；正式设备和云端不得发送 `monitor` 别名。
2. 监控模式只能由云端开启和关闭，设备 UI 不提供监控开关。
3. 控制消息只使用现有 `device_command` / `device_command_ack`，`command` 固定为 `set_audio_mode`。
4. 开启监控隐含停止普通对话、自动追问和当前播报，不需要增加 `interrupt_current_turn`。
5. 监控期间不运行普通 ASR，不请求智能体，不生成或播放 TTS。
6. 监控模式连续上传全部成功读取的麦克风音频，包括静音、弱声、人声和环境声。
7. 监控模式不使用静音门、VAD 门控、自动增益、会议模式增益、AEC 或模式专用降噪；只保留硬件编解码器和公共麦克风驱动本身不可绕过的配置。
8. 监控模式不因静音或无人声自动退出，只能由云端关闭、WebSocket 断开或设备致命采集故障结束。
9. 普通对话、会议和监控模式互斥，同一时刻只能有一个麦克风及上行音频会话所有者。
10. 不使用 `revision`、`base_revision`、`meeting_id` 或 `audio_mode_state`。

## 3. ID 约定

- `request_id` 由云端生成，设备在 ACK 中原样回显。
- `request_id` 为 1 至 47 个 ASCII 字符，使用 `[A-Za-z0-9_.-]`。
- 同一 WebSocket 连接内，相同 `request_id` 只能对应相同命令内容；重试必须复用原 ID。
- `audio_session_id` 由设备为每次成功开启生成，格式为 1 至 47 个 ASCII 字符。
- 一次监控的 `listen start` 与 `listen stop` 使用同一 `audio_session_id`；重新开启必须生成新 ID。
- 控制 ACK 不重复携带 `audio_session_id`，云端从已有 `listen start` 取得实际值。

## 4. 云端开启监控

云端发送：

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_monitoring_open_001",
  "mode": "monitoring",
  "state": "enabled"
}
```

设备端依次执行：

1. 校验当前没有会议或其他独占音频模式；若有，拒绝开启，不自动切换。
2. 停止普通麦克风会话、自动追问和唤醒词采集。
3. 若正在播报，对当前音频执行约 200 ms 淡出并取消后续旧 TTS；最多等待 500 ms 收尾。
4. 创建新的 `audio_session_id`。
5. 使用已有音频会话协议发送 `listen start, mode:monitoring`。
6. 初始化麦克风和音频上行，进入 ACTIVE。
7. 回复开启成功 ACK，然后连续上传音频。

`listen start`：

```json
{
  "type": "listen",
  "state": "start",
  "mode": "monitoring",
  "audio_session_id": "monitoring_stream_001",
  "request_id": "cloud_monitoring_open_001"
}
```

开启成功 ACK：

```json
{
  "type": "device_command_ack",
  "request_id": "cloud_monitoring_open_001",
  "command": "set_audio_mode",
  "ok": true,
  "mode": "monitoring",
  "state": "enabled",
  "message": "active"
}
```

实际顺序固定为：

```text
cloud command enabled
-> 设备停止普通对话并淡出播放
-> listen start monitoring
-> device_command_ack active
-> 已有 NA v1 binary 音频持续上传
```

云端按现有音频会话协议处理 `listen start` 并返回 `listen_ack`。为了与当前会议链路保持一致，监控设备实现不等待 `listen_ack` 才开始上传；云端必须在处理 `listen start` 时同步建立接收上下文。

## 5. 开启拒绝或失败

统一错误 ACK：

```json
{
  "type": "device_command_ack",
  "request_id": "cloud_monitoring_open_001",
  "command": "set_audio_mode",
  "ok": false,
  "mode": "monitoring",
  "state": "disabled",
  "message": "audio_mode_busy"
}
```

固定 `message`：

| 值 | 含义 |
| --- | --- |
| `audio_mode_busy` | 会议或其他独占音频模式正在运行 |
| `capture_failed` | 麦克风、内存或音频上行启动失败 |
| `playback_stop_failed` | 当前播报无法完成本地取消 |
| `control_queue_full` | 控制事件无法入队 |

若 `listen start` 已发送后才启动失败，设备应尽最大努力再发送匹配的 `listen stop`，然后返回 `ok:false, state:"disabled", message:"capture_failed"`。云端必须关闭该未进入 ACTIVE 的音频上下文。

## 6. 监控期间行为

- 所有成功读取的 PCM 帧均上传，不因内容类型丢弃。
- 设备不设置 `VAD_SPEECH`，也不使用其控制上传。
- 云端使用现有 NA v1 解析和流式存储链路，不将音频送入普通 ASR、智能体或 TTS。
- 设备丢弃监控期间迟到的普通对话事件和 TTS binary 音频。
- 设备不执行 30 分钟静音退出或其他业务超时。
- 云端未发送关闭命令前，设备保持监控；但网络断开或致命硬件错误仍必须本地停止。

## 7. 云端关闭监控

云端发送：

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_monitoring_close_001",
  "mode": "monitoring",
  "state": "disabled"
}
```

设备按以下顺序完成关闭：

```text
停止继续采集
-> 最后一个真实音频帧尽最大努力携带 END
-> 排空并结束上行队列
-> 释放麦克风
-> listen stop monitoring
-> device_command_ack disabled
```

`listen stop`：

```json
{
  "type": "listen",
  "state": "stop",
  "mode": "monitoring",
  "audio_session_id": "monitoring_stream_001"
}
```

关闭成功 ACK：

```json
{
  "type": "device_command_ack",
  "request_id": "cloud_monitoring_close_001",
  "command": "set_audio_mode",
  "ok": true,
  "mode": "monitoring",
  "state": "disabled",
  "message": "disabled"
}
```

设备不等待 `listen_ack stop` 才发送关闭 ACK；同一 WebSocket 上的设备发送顺序必须保持 `listen stop` 在前、`device_command_ack` 在后。

## 8. 幂等与命令 ACK

| `ok` | `state` | `message` | 含义 |
| --- | --- | --- | --- |
| true | enabled | `active` | 新监控已经进入 ACTIVE |
| true | enabled | `already_active` | 收到开启命令时已经监控中 |
| true | disabled | `disabled` | 关闭及资源收尾完成 |
| true | disabled | `already_disabled` | 收到关闭命令时已经关闭 |
| false | disabled | `audio_mode_busy` | 其他独占模式占用音频资源 |
| false | disabled | `capture_failed` | 启动或运行失败 |
| false | disabled | `playback_stop_failed` | 播放无法完成取消 |
| false | 请求目标值 | `control_queue_full` | 控制事件无法入队 |

- 相同 `request_id`、相同内容的重试不得创建第二个监控会话。
- 已 ACTIVE 时收到不同 ID 的 enabled，返回 `already_active`。
- 已关闭时收到 disabled，返回 `already_disabled`。
- `ok:false` 时云端以 `ok` 和 `message` 判断，不依赖 `state` 推断其他模式的实际状态。

## 9. 断线和异常

- WebSocket 断开时设备立即停止采集、释放麦克风并回到普通模式。
- 断线时不能保证发送 END、`listen stop` 或控制 ACK。
- 云端封装已收到的 PCM，并依据 END、序号连续性和存储结果标记完整或不完整。
- 重连后设备不自动恢复监控；云端仍需监控时必须重新发送 enabled，并建立新的 `audio_session_id`。
- 连接仍可用但设备发生致命采集故障时，设备尽最大努力发送 END 和 `listen stop`，随后回到普通模式；不存在无请求来源的伪造 `device_command_ack`。

## 10. 模式互斥

- 会议 ACTIVE/STARTING/STOPPING 时，监控 enabled 返回 `audio_mode_busy`。
- 监控 ACTIVE/STARTING/STOPPING 时，会议开启请求也必须被拒绝或保持原模式，不能并发占用麦克风。
- 云端需要从会议切换到监控时，必须先关闭会议并收到其关闭收尾，再发送监控 enabled。
- 云端需要从监控切换到会议时，必须先关闭监控并收到其关闭收尾，再开启会议。

## 11. 云端最小对接清单

1. 只有设备声明 `monitoring_mode_v1:true` 后才发送监控命令。
2. 发送 `set_audio_mode monitoring enabled/disabled`，按 `request_id` 匹配 ACK。
3. 发送 enabled 前取消原智能体轮次并停止继续下发旧 TTS。
4. 看到 `listen mode:monitoring` 后使用已有音频接收和流式落盘链路。
5. 监控音频不得进入普通 ASR、智能体或 TTS。
6. 发送 disabled 后继续接收，直到 END、`listen stop` 和关闭 ACK 完成收尾。
7. 序号缺口、缺少 END、断线或存储失败时将录音标记为异常结束。

## 12. 最简摘要

云端主动发送的业务控制只有两条：

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_monitoring_open_001",
  "mode": "monitoring",
  "state": "enabled"
}
```

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_monitoring_close_001",
  "mode": "monitoring",
  "state": "disabled"
}
```

设备分别在真正进入 ACTIVE、真正完成关闭收尾后返回对应 `device_command_ack`。音频继续使用已有 `listen start -> NA binary -> END -> listen stop` 链路。
