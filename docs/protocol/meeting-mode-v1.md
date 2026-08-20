# Nuwax AI Partner 7.0 会议模式设备端/云端通信协议 v1（历史设计稿）

> 注意：本文件包含尚未进入当前固件的 `revision`、`base_revision`、`meeting_id` 等设计，不是当前云端对接依据。当前 7.0 实现请使用同目录下的 `meeting-mode-current-v1.md`。

## 1. 文档信息

| 项目 | 内容 |
| --- | --- |
| 协议名称 | Nuwax Meeting Mode Protocol |
| 协议版本 | 1.0.0 |
| 文档状态 | 待设备端与云端联合确认 |
| 编写日期 | 2026-07-16 |
| 适用工程 | Nuwax AI Partner 7.0 |
| 传输通道 | 设备与云端现有 WebSocket 连接 |
| 音频帧格式 | 现有 NA v1 二进制音频帧 |

本文规定设备屏幕 UI 会议模式开关、云端主动控制、设备实际状态上报和连续音频上传之间的完整通信契约。

文中的“必须”“不得”“应当”属于强制要求；“建议”“可以”属于非强制建议。

## 2. 协议目标

会议模式开启后，设备必须连续采集并上传当前会议期间的全部音频帧，包括：

- 人声；
- 静音；
- 背景声；
- 设备附近的其他非人声音频。

会议模式不得使用本地 VAD 决定是否开始上传、丢弃音频或结束上传。VAD 结果只能作为音频帧的附加标记。

会议模式 v1 的目标是“连续接收全部音频”，不包括以下能力：

- 不定义会议内容总结算法；
- 不定义说话人分离算法；
- 不定义会议 ASR 供应商；
- 不保证网络断开期间的音频不丢失；
- 不上传原始双麦多通道数据；
- 不允许会议音频触发普通智能体自动回复。

会议模式 v1 默认上传设备当前音频处理链输出的 16 kHz、单声道、PCM16 音频。若后续需要原始双麦或其他处理结果，必须通过独立的 `capture_profile` 协议版本扩展，不得改变 v1 的既有语义。

## 3. 核心设计原则

### 3.1 云端持有开启授权

设备 UI 开关只是发起请求。设备收到云端成功响应前，不得进入实际录音状态，也不得发送会议音频。

这样可以确保云端已经完成会议资源、存储目录和音频接收器的创建，不会出现“设备已经录音，但云端没有接收目标”的状态。

### 3.2 本地关闭立即生效

关闭会议模式属于隐私保护操作。用户在设备 UI 点击关闭后，设备必须立即开始停止采集流程，不得等待云端批准。

云端必须接受设备发起的关闭请求。云端不得拒绝本地用户停止录音。

### 3.3 期望状态与实际状态分离

- `audio_mode_request_ack` 和 `device_command_ack` 表示请求或命令已被接受；
- `audio_mode_state` 表示设备已经达到的真实运行状态；
- 云端不得把 ACK 当作设备已经开始或已经停止录音的证明；
- 只有 `audio_mode_state.state == "active"` 才表示会议采集真正开始；
- 只有 `audio_mode_state.state == "disabled"` 才表示设备侧会议采集已经结束。

### 3.4 设备保持单个连续音频会话

一场连续会议在一次 WebSocket 连接内只建立一个逻辑音频会话。设备不得为了生成分段文件而周期性停止并重新初始化麦克风、AEC 或音频上行任务。

云端必须边接收边写入会议存储，并在云端内部滚动切分文件。v1 默认每 60 秒生成一个存储分段，但该分段动作不得改变设备端 `listen` 会话。

### 3.5 与普通对话互斥

会议模式处于 `starting`、`active` 或 `stopping` 时：

- 不得启动普通对话录音；
- 不得启动智能打断录音；
- 不得播放普通智能体 TTS；
- 会议音频不得进入普通 ASR 自动转发链；
- 迟到的普通 ASR、TTS 和状态命令不得改变会议模式状态。

## 4. 参与方与职责

| 参与方 | 职责 |
| --- | --- |
| 设备 UI | 接收用户开关操作，显示 `starting/active/stopping/error/disabled` 状态 |
| 设备会议控制器 | 处理协议状态机、启停采集、上报真实状态 |
| 设备音频上行 | 连续发送 NA v1 二进制音频帧，维护序号与丢帧统计 |
| 云端设备会话 | 认证设备、处理 JSON 控制消息、维护期望状态与 revision |
| 云端会议接收器 | 创建会议存储、连续写入音频、按 60 秒滚动分段 |
| 云端普通对话链 | 会议模式期间必须停止处理该设备的会议音频 |

## 5. 标识符与版本规则

### 5.1 字符集

所有字符串 ID 必须只使用以下 ASCII 字符：

```text
a-z A-Z 0-9 _ - .
```

建议统一使用小写。不得使用中文、空格、斜杠、反斜杠、冒号、URL、控制字符或其他 Unicode 字符。

### 5.2 长度与生命周期

| 字段 | 最大长度 | 生成方 | 生命周期和意义 |
| --- | ---: | --- | --- |
| `request_id` | 39 字节 | 请求发起方 | 唯一标识一次控制请求；相同请求重试必须复用原值 |
| `meeting_id` | 32 字节 | 云端 | 唯一标识整场会议；断线重连后仍保持不变 |
| `audio_session_id` | 47 字节 | 云端 | 唯一标识一次连续 WebSocket 音频流；断线重连必须更换 |
| `revision` | uint32 | 云端 | 标识云端权威期望状态版本；必须单调递增，0 保留为未知 |

推荐示例：

```json
{
  "request_id": "ui_mode_20260716_110001",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 1
}
```

### 5.3 幂等规则

- 同一请求超时重试时，必须使用相同 `request_id` 和完全相同的消息内容；
- 云端收到重复 `request_id` 时，必须返回第一次处理得到的相同结果，不得重复创建会议；
- 相同 `revision` 且期望状态相同，属于幂等重放；
- 相同 `revision` 但期望状态不同，必须拒绝并返回 `revision_conflict`；
- 小于设备最后应用 revision 的命令必须拒绝并返回 `stale_revision`；
- 大于当前 revision 的合法命令代表新的期望状态，设备必须以最新 revision 为准。

设备 UI 请求采用 compare-and-set 语义：

- `state:enabled` 的 `base_revision` 必须等于云端当前 revision，否则云端返回 `stale_revision`；
- `state:disabled` 是隐私优先操作，即使 `base_revision` 已过期，云端也必须接受，并分配一个严格大于当前 revision 的新 revision；
- 被后续关闭请求取消的开启 ACK 即使迟到到达，设备也不得应用。

### 5.4 JSON 编码与校验规则

- 控制消息必须使用 UTF-8 JSON Text Frame；
- `type`、`command`、`mode`、`state` 和错误码区分大小写，v1 固定使用小写；
- 布尔字段必须使用 JSON `true/false`，不得使用字符串 `"true"/"false"`；
- `revision`、`base_revision` 和时间字段必须使用 JSON 整数，不得使用数字字符串；
- 必填字段缺失、类型错误或枚举值未知时，接收方必须明确拒绝，不得猜测默认值；
- ID 必须在写入固定缓冲区前完成字符集和长度校验，禁止静默截断后继续执行；
- 接收方应忽略不认识的可选字段，以便后续小版本兼容；
- 接收方不得依据 JSON 字段顺序处理业务。

## 6. 状态模型

设备会议模式实际状态只能取以下值：

| 状态 | 意义 | 是否采集 | 是否上传 |
| --- | --- | --- | --- |
| `disabled` | 会议模式关闭 | 否 | 否 |
| `starting` | 已获得授权，正在停止旧业务并建立会议音频会话 | 尚未正式开始 | 否 |
| `active` | 会议采集和上传均已生效 | 是 | 是 |
| `stopping` | 已停止接收新的会议操作，正在发送尾帧并收尾 | 正在停止 | 正在排空 |
| `error` | 启动、采集、上传或连接发生错误 | 否 | 否 |

合法状态转换：

```text
disabled -> starting -> active -> stopping -> disabled
                  \-> error
active ---------------------------> error
error ----------------------------> disabled
```

禁止直接报告 `disabled -> active`。设备必须经过 `starting`，并在收到成功的 `listen_ack` 后才能报告 `active`。

## 7. 消息总表

| 序号 | 方向 | 消息 | 核心意义 |
| ---: | --- | --- | --- |
| M1 | 设备 -> 云端 | `hello` | 声明设备支持会议模式 v1 |
| M2 | 云端 -> 设备 | `hello` 响应 | 声明当前云端是否允许使用会议模式 |
| M3 | 设备 -> 云端 | `audio_mode_request` | 设备 UI 请求开启或关闭会议模式 |
| M4 | 云端 -> 设备 | `audio_mode_request_ack` | 云端批准或拒绝设备 UI 请求，并分配权威 ID 与 revision |
| M5 | 云端 -> 设备 | `device_command/set_audio_mode` | 云端主动要求设备开启或关闭会议模式 |
| M6 | 设备 -> 云端 | `device_command_ack` | 设备确认已接受或拒绝云端主动命令 |
| M7 | 设备 -> 云端 | `audio_mode_state` | 上报设备会议模式的真实运行状态 |
| M8 | 设备 -> 云端 | `listen start/stop` | 建立或结束会议音频传输会话 |
| M9 | 云端 -> 设备 | `listen_ack` | 确认音频会话是否成功建立或结束 |
| M10 | 设备 -> 云端 | NA v1 二进制帧 | 承载实际连续会议音频 |
| M11 | 云端 -> 设备 | `audio_session` | 提供音频会话开始/结束摘要，仅用于观测 |

## 8. M1：设备 `hello` 能力声明

设备建立 WebSocket 后，在现有 `hello` 中增加会议模式能力：

```json
{
  "type": "hello",
  "version": 1,
  "device_id": "nuwax_esp32s3_001",
  "transport": "websocket",
  "features": {
    "audio_upload": true,
    "audio_playback": true,
    "mcp": true,
    "meeting_mode_v1": true
  },
  "audio_params": {
    "codec": "pcm16",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration_ms": 16
  }
}
```

### 消息意义

该消息表示设备固件具备会议模式协议解析、状态机和连续音频采集能力。`meeting_mode_v1:true` 不是会议模式已经开启，只是能力声明。

若设备没有该能力，云端不得发送 `set_audio_mode` 命令，设备 UI 也不得显示可用的会议模式开关。

## 9. M2：云端 `hello` 响应

云端在现有 `hello` 响应中返回会议模式策略：

```json
{
  "type": "hello",
  "ok": true,
  "session_id": "ws_20260716_001",
  "transport": "cloud-websocket",
  "meeting_mode": {
    "supported": true,
    "protocol_version": 1,
    "persist_supported": false,
    "server_segment_duration_ms": 60000
  }
}
```

### 消息意义

该消息表示当前连接到的云端实例已经部署会议模式接收器，可以接受设备 UI 的会议请求。

- `supported:false`：设备必须禁用 UI 开关；
- `protocol_version:1`：双方使用本文定义的 v1 语义；
- `persist_supported:false`：设备重启后不得自行恢复录音；
- `server_segment_duration_ms`：仅表示云端存储分段长度，不要求设备切换音频会话。

## 10. M3：设备 UI 请求 `audio_mode_request`

### 10.1 请求开启

```json
{
  "type": "audio_mode_request",
  "request_id": "ui_mode_20260716_110001",
  "mode": "meeting",
  "state": "enabled",
  "source": "device_ui",
  "base_revision": 0
}
```

### 10.2 请求关闭

```json
{
  "type": "audio_mode_request",
  "request_id": "ui_mode_20260716_113000",
  "mode": "meeting",
  "state": "disabled",
  "source": "device_ui",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "base_revision": 1
}
```

### 10.3 在 starting 阶段取消开启

```json
{
  "type": "audio_mode_request",
  "request_id": "ui_mode_20260716_110002",
  "mode": "meeting",
  "state": "disabled",
  "source": "device_ui",
  "base_revision": 0,
  "supersedes_request_id": "ui_mode_20260716_110001"
}
```

### 字段定义

| 字段 | 类型 | 必填 | 意义 |
| --- | --- | --- | --- |
| `type` | string | 是 | 固定为 `audio_mode_request` |
| `request_id` | string | 是 | 本次 UI 操作的幂等键 |
| `mode` | string | 是 | v1 固定为 `meeting` |
| `state` | string | 是 | `enabled` 或 `disabled` |
| `source` | string | 是 | UI 发起时固定为 `device_ui` |
| `meeting_id` | string | 关闭时是 | 当前会议 ID |
| `audio_session_id` | string | 关闭时是 | 当前音频流 ID |
| `base_revision` | uint32 | 是 | 设备最后已知的云端 revision；首次开启使用 0 |
| `supersedes_request_id` | string | 否 | starting 阶段取消时，指向被取消的开启 request_id |

### 消息意义

该消息表达设备本地用户的操作意图，不直接证明状态已改变。

- 开启请求：设备必须等待 M4 成功响应；
- 关闭请求：设备立即进入 `stopping`，M4 只用于同步云端状态，不得阻止本地停止；
- 云端必须把 `source:device_ui + state:disabled` 视为隐私优先操作，不得拒绝。
- starting 阶段的取消请求必须阻止设备应用被取消开启请求的迟到 ACK；云端必须释放已经为被取消请求预分配但尚未使用的会议资源。

## 11. M4：云端响应 `audio_mode_request_ack`

### 11.1 批准开启

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "ui_mode_20260716_110001",
  "ok": true,
  "mode": "meeting",
  "state": "enabled",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 1,
  "persist": false,
  "message": "meeting resources ready"
}
```

### 11.2 拒绝开启

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "ui_mode_20260716_110001",
  "ok": false,
  "mode": "meeting",
  "state": "disabled",
  "error_code": "meeting_service_unavailable",
  "message": "meeting service unavailable"
}
```

### 11.3 接受关闭

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "ui_mode_20260716_113000",
  "ok": true,
  "mode": "meeting",
  "state": "disabled",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 2,
  "persist": false,
  "message": "stop accepted"
}
```

### 消息意义

对于开启请求，`ok:true` 是云端已经准备好会议接收资源并授权设备启动的证明。设备收到该消息后才能进入 `starting`。

对于关闭请求，设备无需等待该消息才停止录音；该消息只提供新的权威 revision，并确认云端不会再次维持旧的 enabled 期望状态。

云端返回 `ok:true` 后不得立即把设备标记为 active。云端必须继续等待 M7 的 `audio_mode_state.state == "active"`。

## 12. M5：云端主动命令 `device_command/set_audio_mode`

### 12.1 云端主动开启

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_mode_20260716_120001",
  "mode": "meeting",
  "state": "enabled",
  "meeting_id": "mtg_20260716_120001_c3d4",
  "audio_session_id": "mtg_c3d4_stream_01",
  "revision": 3,
  "persist": false
}
```

### 12.2 云端主动关闭

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_mode_20260716_123000",
  "mode": "meeting",
  "state": "disabled",
  "meeting_id": "mtg_20260716_120001_c3d4",
  "audio_session_id": "mtg_c3d4_stream_01",
  "revision": 4,
  "persist": false
}
```

### 消息意义

该消息用于云端管理页面、云端业务逻辑或远程管理员主动控制设备会议模式。

它与设备 UI 请求的区别只在于发起方不同。设备必须把 M4 的成功开启响应和 M5 的开启命令归一化到同一个会议模式状态机，不能维护两套录音逻辑。

v1 中 `persist` 必须为 `false`。收到 `persist:true` 时设备必须拒绝，返回 `unsupported_persist`。

## 13. M6：设备命令确认 `device_command_ack`

### 13.1 命令已接受

```json
{
  "type": "device_command_ack",
  "request_id": "cloud_mode_20260716_120001",
  "command": "set_audio_mode",
  "ok": true,
  "mode": "meeting",
  "state": "starting",
  "meeting_id": "mtg_20260716_120001_c3d4",
  "revision": 3,
  "message": "accepted"
}
```

### 13.2 命令被拒绝

```json
{
  "type": "device_command_ack",
  "request_id": "cloud_mode_20260716_120001",
  "command": "set_audio_mode",
  "ok": false,
  "mode": "meeting",
  "state": "disabled",
  "revision": 3,
  "error_code": "audio_session_active",
  "message": "another audio session is active"
}
```

### 消息意义

该消息只回答“设备是否接受这条云端命令”。`ok:true` 不表示麦克风已开始上传，也不表示会议模式已 active。

云端收到 `ok:true` 后必须等待 M7。设备收到开启命令后，应尽快回复 ACK，再异步执行播放淡出、旧会话清理和会议采集初始化。

## 14. M7：设备真实状态 `audio_mode_state`

### 14.1 正在开启

```json
{
  "type": "audio_mode_state",
  "mode": "meeting",
  "state": "starting",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 1,
  "reason": "device_ui"
}
```

### 14.2 已正式开启

```json
{
  "type": "audio_mode_state",
  "mode": "meeting",
  "state": "active",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 1,
  "reason": "listen_ack_received"
}
```

### 14.3 正在关闭

```json
{
  "type": "audio_mode_state",
  "mode": "meeting",
  "state": "stopping",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 1,
  "pending_request_id": "ui_mode_20260716_113000",
  "reason": "device_ui"
}
```

### 14.4 已关闭

```json
{
  "type": "audio_mode_state",
  "mode": "meeting",
  "state": "disabled",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 2,
  "reason": "listen_stop_sent"
}
```

### 14.5 错误状态

```json
{
  "type": "audio_mode_state",
  "mode": "meeting",
  "state": "error",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 1,
  "reason": "capture_start_failed",
  "error_code": "capture_start_failed",
  "message": "microphone initialization failed",
  "dropped_frames": 0
}
```

### 消息意义

这是云端判断设备真实状态的唯一权威上行消息。

- `starting`：设备已经开始执行启动流程；
- `active`：云端音频会话已确认，设备开始连续上传；
- `stopping`：设备不再接受新的会议启动动作，正在完成尾帧和队列收尾；
- `disabled`：设备侧录音和上行资源均已释放；
- `error`：设备未能维持会议模式，云端必须停止把该设备标记为录音中。

`revision` 必须表示设备最后已经从云端确认的 revision。设备 UI 本地关闭时，物理停止动作不能等待云端，但新的 revision 尚未返回。因此：

- `stopping` 可以携带关闭前最后确认的 revision，并用 `pending_request_id` 关联正在等待的关闭 ACK；
- 收到关闭 ACK 后，最终 `disabled` 必须携带 ACK 返回的新 revision；
- 若关闭 ACK 超时或关闭期间断线，设备仍必须上报或在重连后上报 disabled；此时使用最后确认的 revision、`pending_request_id` 和 `reason:local_stop_unconfirmed`，云端必须把该状态视为本地隐私覆盖并生成新的 disabled revision。

设备每次 WebSocket 重连并完成 hello 后，必须主动发送一次当前 `audio_mode_state` 快照。

## 15. M8：音频会话控制 `listen`

### 15.1 建立会议音频会话

```json
{
  "type": "listen",
  "state": "start",
  "mode": "meeting",
  "request_id": "ui_mode_20260716_110001",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 1
}
```

### 15.2 结束会议音频会话

```json
{
  "type": "listen",
  "state": "stop",
  "mode": "meeting",
  "request_id": "ui_mode_20260716_113000",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 1
}
```

### 消息意义

`listen` 只负责音频传输会话的建立和结束，不负责决定用户是否允许录音。

- `listen start` 必须发生在云端批准开启之后；
- `listen start` 成功前不得发送正式会议音频；
- `listen stop` 必须使用与 start 相同的 `meeting_id` 和 `audio_session_id`；
- 设备 UI 本地关闭时，`listen stop.revision` 使用最后已确认的 revision；关闭请求的目标版本通过相同 `request_id` 关联，最终以 M4 返回的新 revision 为准；
- 一次 WebSocket 连接上不得同时存在两个活动音频会话；
- 云端内部滚动文件不得要求设备重复发送 `listen stop/start`。

## 16. M9：音频会话确认 `listen_ack`

### 16.1 建立成功

```json
{
  "type": "listen_ack",
  "ok": true,
  "state": "start",
  "mode": "meeting",
  "request_id": "ui_mode_20260716_110001",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 1,
  "duplicate": false,
  "error": ""
}
```

### 16.2 建立失败

```json
{
  "type": "listen_ack",
  "ok": false,
  "state": "start",
  "mode": "meeting",
  "request_id": "ui_mode_20260716_110001",
  "meeting_id": "mtg_20260716_110001_a1b2",
  "audio_session_id": "mtg_a1b2_stream_01",
  "revision": 1,
  "duplicate": false,
  "error": "audio_session_active",
  "active_audio_session_id": "audio_other_01"
}
```

### 消息意义

该消息证明云端是否已经为二进制音频帧建立正确的接收上下文。

设备只有收到 `state:start + ok:true` 后才能：

1. 把第一帧标记为 `START`；
2. 开始连续发送会议音频；
3. 上报 `audio_mode_state active`。

`duplicate:true + ok:true` 表示云端识别到幂等重试，设备可以继续使用原会话，不得创建第二个音频流。

`request_id`、`meeting_id`、`audio_session_id` 和 `revision` 必须回显对应的 listen 请求，使设备可以拒绝迟到或属于旧会话的 ACK。

## 17. M10：NA v1 二进制会议音频帧

会议模式复用当前 NA v1 帧格式：

| 偏移 | 长度 | 字段 | 会议模式语义 |
| ---: | ---: | --- | --- |
| 0 | 2 | Magic `NA` | 标识 Nuwax 音频帧 |
| 2 | 1 | Version | 固定为 1 |
| 3 | 1 | Codec | v1 当前使用 PCM16 |
| 4 | 1 | Flags | `START/END/VAD_SPEECH` |
| 5 | 1 | Channels | 当前固定为 1 |
| 6 | 2 | Sample rate | 当前固定为 16000 |
| 8 | 2 | Frame duration | 当前配置为 16 ms |
| 10 | 4 | Sequence | 整个音频流单调递增的帧序号 |
| 14 | 4 | Timestamp | 设备侧单调时间戳，单位 ms |
| 18 | 2 | Payload bytes | 音频负载字节数 |
| 20 | 2 | Reserved | 固定为 0 |
| 22 | N | Payload | PCM16 little-endian 音频 |

Flags 定义：

| 标志 | 值 | 意义 |
| --- | ---: | --- |
| `START` | `0x01` | 该帧是本次 `audio_session_id` 的第一帧 |
| `END` | `0x02` | 该帧是本次 `audio_session_id` 的最后一帧 |
| `VAD_SPEECH` | `0x04` | VAD 认为该帧可能包含语音，仅作为元数据 |

### 消息意义

二进制帧承载真实会议音频。JSON 消息只管理状态，不承载音频内容。

一个 WebSocket Binary Message 可以包含一个或多个首尾相接的完整 NA v1 音频帧。云端必须根据每个 NA 帧头中的 payload 长度循环解析，不能假设一个 WebSocket Message 只对应一个音频帧。

NA v1 帧本身不包含 `meeting_id` 或 `audio_session_id`。云端必须按照同一 WebSocket 上当前已由 `listen_ack ok:true` 确认的唯一活动音频会话归属这些帧。因此，任何时刻只能有一个活动上行音频会话。

会议模式必须满足：

- 第一帧包含 `START`；
- 正常关闭时最后一帧包含 `END`；
- `sequence` 在同一 `audio_session_id` 内连续递增；
- 第一帧 sequence 可以是任意 uint32 值，云端不得强制从 0 或 1 开始；
- 同一音频会话内 codec、采样率、声道数和帧时长不得改变；
- 静音帧也必须发送；
- `VAD_SPEECH` 未设置时也必须发送；
- 云端检测到序号缺口时必须记录丢帧数量；
- 云端不得因为音量低、VAD false 或连续静音而自动结束会议。

## 18. M11：云端观测消息 `audio_session`

云端可以继续发送现有音频会话事件：

```json
{
  "type": "audio_session",
  "state": "stop",
  "mode": "meeting",
  "audio_session_id": "mtg_a1b2_stream_01",
  "frames": 22500,
  "duration_ms": 360000,
  "rms": 824,
  "end_frame_received": true,
  "listen_stop_seen": true
}
```

### 消息意义

该消息用于日志、诊断和 UI 统计，不是会议模式控制命令，也不能替代 `audio_mode_state`。

设备可以记录该消息，但不得因为收到 `audio_session state:stop` 自动恢复普通对话；设备必须以本地会议状态机和权威 revision 为准。

## 19. 设备 UI 开启完整时序

| 步骤 | 方向 | 消息/动作 | 必须达到的结果 |
| ---: | --- | --- | --- |
| 1 | 本地 | 用户点击开关 ON | UI 进入 `starting`，暂不录音 |
| 2 | 设备 -> 云端 | M3 `audio_mode_request enabled` | 请求云端准备会议资源 |
| 3 | 云端 | 创建 `meeting_id`、音频接收器和存储目录 | 资源成功后才能批准 |
| 4 | 云端 -> 设备 | M4 `audio_mode_request_ack ok:true` | 返回 ID 和 revision |
| 5 | 设备 -> 云端 | M7 `audio_mode_state starting` | 报告正在执行启动流程 |
| 6 | 设备 | 200 ms 左右淡出并停止当前播放，结束普通对话采集 | 与会议模式互斥 |
| 7 | 设备 -> 云端 | M8 `listen start mode:meeting` | 请求建立音频上下文 |
| 8 | 云端 -> 设备 | M9 `listen_ack ok:true` | 云端接收器就绪 |
| 9 | 设备 -> 云端 | M10 第一帧，携带 `START` | 正式开始会议音频 |
| 10 | 设备 -> 云端 | M7 `audio_mode_state active` | 云端和 UI 均标记为 active |
| 11 | 设备 -> 云端 | 连续 M10 音频帧 | 包括静音在内的所有帧持续上传 |

若步骤 4 被拒绝或超时，设备必须回到 `disabled`，不得执行步骤 5 以后动作。

## 20. 设备 UI 关闭完整时序

| 步骤 | 方向 | 消息/动作 | 必须达到的结果 |
| ---: | --- | --- | --- |
| 1 | 本地 | 用户点击开关 OFF | 设备立即进入 `stopping` |
| 2 | 设备 -> 云端 | M3 `audio_mode_request disabled` | 通知云端本地停止意图 |
| 3 | 设备 -> 云端 | M7 `audio_mode_state stopping` | 使用最后已确认 revision，并携带 pending_request_id |
| 4 | 设备 | 停止接收新的会议音频，完成当前帧 | 不等待云端批准 |
| 5 | 设备 -> 云端 | M10 最后一帧，携带 `END` | 标识音频负载结束 |
| 6 | 设备 | 排空已经进入上行队列的音频 | 防止尾部音频丢失 |
| 7 | 设备 -> 云端 | M8 `listen stop` | 结束云端音频上下文 |
| 8 | 云端 -> 设备 | M9 stop ACK | 确认音频会话收尾 |
| 9 | 设备 -> 云端 | M7 `audio_mode_state disabled` | 实际停止完成；优先使用 M4 返回的新 revision |

云端收到步骤 2 后只能把会议标记为“正在停止”，不得立即关闭音频写入器。音频写入器必须至少保持到收到 `END`、`listen stop` 或 WebSocket 断开。

M4 关闭 ACK 可以在步骤 2 之后的任意时刻到达，但不得作为步骤 4 至步骤 7 的执行前提。设备收到 M4 后保存其新 revision；若在步骤 9 前尚未收到 M4，则按 `local_stop_unconfirmed` 规则上报旧 revision 和 pending_request_id。

即使步骤 8 未收到，设备仍必须完成本地关闭。重新连接后，设备必须报告 `disabled` 和 `reason:local_stop_unconfirmed`，云端必须接受该本地停止结果。

## 21. 云端主动开启/关闭时序

云端主动控制使用 M5/M6，后续音频建立和状态上报与 UI 流程相同：

```text
云端 M5 set_audio_mode
  -> 设备 M6 device_command_ack
  -> 设备 M7 starting/stopping
  -> 设备 M8 listen start/stop
  -> 云端 M9 listen_ack
  -> 设备 M7 active/disabled
```

云端主动开启前必须已经创建会议资源。云端主动关闭时，设备仍必须发送 `END` 和 `listen stop`，不得直接销毁上行队列。

## 22. 超时与重试

| 场景 | 超时 | 处理 |
| --- | ---: | --- |
| 等待 `audio_mode_request_ack` | 3000 ms | 使用相同 request_id 重试最多 2 次，仍失败则回到 disabled |
| 等待 `device_command_ack` | 3000 ms | 云端可使用相同 request_id 重发命令 |
| 等待 `listen_ack start` | 1000 ms | 使用相同 audio_session_id 重试最多 2 次 |
| 等待设备上报 `active` | 3000 ms | 云端标记启动失败并发送 disabled 命令 |
| 等待 `listen_ack stop` | 2000 ms | 设备保持本地 disabled，云端按连接状态收尾 |
| active 状态下 WebSocket 断开 | 立即 | 设备停止采集并进入 error/disabled，不离线录音 |

任何重试不得生成新的 `request_id`、`meeting_id` 或 `audio_session_id`。只有确定旧连接已经失效并重新建立 WebSocket 时，才为同一 `meeting_id` 分配新的 `audio_session_id`。

## 23. 断线与重连

### 23.1 active 期间断线

设备必须：

1. 立即停止继续录制会议音频；
2. 停止向已失效连接发送数据；
3. 记录本地错误原因 `ws_disconnected`；
4. UI 显示错误或重连状态；
5. 不得在后台离线持续录音。

云端必须关闭当前 `audio_session_id` 的写入器，并把缺少 END 的会话标记为非正常结束。

### 23.2 重连

设备重新发送 M1 并收到云端 M2 后，必须发送当前 M7 状态快照。设备不得仅依据重启前状态自行恢复会议录音。

若云端确认原会议仍然有效并决定恢复，应当：

- 保留原 `meeting_id`；
- 生成新的 `audio_session_id`；
- 使用更大的 `revision` 下发 M5 enabled；
- 在云端存储中记录两段音频之间存在网络中断。

若设备在离线期间被用户本地关闭，设备的本地关闭具有优先权，云端不得自动重新开启。

### 23.3 设备内部错误收尾

如果错误发生在 `listen start` 之前，设备必须上报 `audio_mode_state error`，随后释放已经初始化的本地资源并上报 `disabled`；云端据此释放预分配的会议资源。

如果错误发生在 active 期间：

1. 设备立即停止接收新的会议音频；
2. WebSocket 仍可用时，尽最大努力发送带 END 的最后一帧、排空已排队音频并发送 `listen stop`；
3. 设备上报 `audio_mode_state error`，其中包含 `error_code` 和 `dropped_frames`；
4. 本地资源释放完成后上报 `disabled`；
5. WebSocket 已失效时不得阻塞等待发送，重连后补报错误摘要和 disabled 状态。

发生错误后不得自动恢复录音。恢复必须重新经过云端授权并使用新的 `audio_session_id`。

## 24. 云端音频处理要求

### 24.1 独立路由

云端识别到 `listen.mode == "meeting"` 后，必须进入专用会议处理分支。会议音频不得进入：

- 普通 `createStreamingAsrSession()`；
- 普通批量 ASR；
- `asr_result -> handleDeviceTextRequest()` 自动转发；
- `show_status` 普通语音识别 UI；
- 普通智能体自动 TTS 回复。

如果云端在没有活动 meeting 音频上下文时收到二进制帧，必须拒绝并记录协议错误，不得使用现有 implicit audio session 兜底把该数据送入普通对话链。

若需要会议实时转写，必须产生独立的会议转写事件，不得复用会触发普通对话的 `asr_result` 语义。

### 24.2 流式落盘

当前 16 kHz、单声道 PCM16 每秒产生 32000 字节，60 秒产生 1920000 字节 PCM。现有 2 MiB 内存上限只能容纳约 65.536 秒，不能用于长期会议缓存。

会议接收器必须：

1. 收到音频帧后立即顺序写入流式存储；
2. 不把整场会议保存在 `session.chunks`；
3. 默认每 60 秒在云端关闭当前 WAV 文件并打开下一文件；
4. 文件滚动时不得要求设备停止或重新建立 `listen`；
5. 最后一段文件在 END、listen stop 或连接关闭时正确补齐 WAV 头；
6. 保存 `meeting_id/audio_session_id/segment_index/first_sequence/last_sequence` 元数据。

建议云端内部文件命名：

```text
<meeting_id>/<audio_session_id>_seg_000001.wav
<meeting_id>/<audio_session_id>_seg_000002.wav
```

该文件名属于云端内部实现，不进入设备协议，也不受设备 ID 缓冲区长度限制。

## 25. 设备音频处理要求

设备会议采集路径必须满足：

- 使用独立会议模式标志，不得伪装成普通 `prelisten`；
- 不受 `CONFIG_NUWAX_VOICE_MIC_MAX_RECORD_MS` 限制；
- 不使用本地 VAD endpoint 停止会议；
- 不使用无语音超时停止会议；
- 不因触摸释放普通聆听操作而停止会议；
- 每个采集帧都进入上行路径；
- 网络背压导致队列满时必须上报 error 和 dropped_frames；
- 关闭时必须先发送 END、排空上行队列，再销毁麦克风/AEC/编码器资源；
- 会议 active 期间不得进入普通 THINKING、SPEAKING 或 SLEEPING 对话状态。

设备当前可以继续对音频执行既有的硬件采集和音频处理，但不得依据处理结果删帧。VAD 标志只用于云端后续分析。

## 26. 错误码

| 错误码 | 发送方 | 意义 | 推荐处理 |
| --- | --- | --- | --- |
| `unsupported_protocol` | 任一方 | 不支持会议模式 v1 | 禁用 UI 开关 |
| `meeting_service_unavailable` | 云端 | 会议存储或服务不可用 | 拒绝开启，设备回到 disabled |
| `invalid_request` | 云端 | 请求字段缺失或非法 | 不重试原内容 |
| `invalid_id` | 任一方 | ID 字符或长度非法 | 重新生成合法 ID |
| `unsupported_persist` | 设备 | v1 不支持 persist=true | 云端改为 false 后重试 |
| `stale_revision` | 设备 | 命令 revision 早于已应用状态 | 云端读取实际状态后重新决策 |
| `revision_conflict` | 任一方 | 相同 revision 对应不同状态 | 停止自动重试并告警 |
| `audio_session_active` | 云端 | 已存在其他活动音频会话 | 先结束旧会话 |
| `audio_session_mismatch` | 云端 | stop ID 与活动会话不一致 | 不结束错误会话，进行状态校准 |
| `listen_start_failed` | 设备 | 云端未能建立音频接收上下文 | 进入 error/disabled |
| `capture_start_failed` | 设备 | 麦克风或音频处理链启动失败 | 进入 error/disabled |
| `send_failed` | 设备 | WebSocket 音频发送失败 | 停止录音并断线恢复 |
| `queue_overflow` | 设备 | 音频上行队列溢出并发生丢帧 | 进入 error，报告 dropped_frames |
| `ws_disconnected` | 设备 | 会议 active 期间连接断开 | 立即停止，不离线录音 |

错误响应必须同时提供机器可读 `error_code` 和面向日志的 `message`。业务逻辑只能依据 `error_code` 分支，不得解析自由文本 `message`。

## 27. UI 行为要求

UI 开关视觉状态应映射如下：

| 协议状态 | UI 表现 | 用户是否可以再次操作 |
| --- | --- | --- |
| `disabled` | 开关关闭 | 可以开启 |
| `starting` | 开关加载中 | 只允许关闭以取消本次开启，不允许重复开启 |
| `active` | 开关开启，显示持续录音指示 | 可以关闭 |
| `stopping` | 开关关闭中 | 禁止重复点击 |
| `error` | 开关关闭并显示错误状态 | 可以重新尝试 |

UI 不得在收到 M4 `ok:true` 时立即显示 active。必须等待 M7 `active`。

会议 active 期间必须有持续可见的录音指示。设备不得在没有视觉提示的情况下后台录音。

## 28. 安全与隐私要求

- v1 固定 `persist:false`；
- 设备重启后默认 `disabled`；
- 云端必须记录会议开启者、开启时间、关闭者、关闭时间和异常结束原因；
- 本地用户关闭优先于云端期望状态；
- 云端不得通过反复发送旧 revision 绕过本地关闭；
- 未完成 WebSocket hello 和能力协商前不得开启会议模式；
- 设备不得在云端拒绝或超时后继续录音；
- 断线后不得进行未声明的离线录音。

## 29. 与当前代码的兼容性结论

### 29.1 可以复用

- `session_protocol.c` 中现有 `listen` 消息外壳；
- `device_command` JSON 外壳；
- 基础 `device_command_ack` 机制；
- NA v1 二进制音频帧和 START/END/VAD flags；
- WebSocket 单连接有序发送；
- 音频上行队列及结束时排空机制；
- 播放淡出停止能力。

### 29.2 必须新增或修改

- hello 增加 `meeting_mode_v1` 能力；
- 解析和发送 `audio_mode_request`；
- 解析和发送 `audio_mode_request_ack`；
- `device_command` 增加通用 `meeting_id/revision/persist` 字段；
- `device_command_ack` 增加结构化 `mode/state/revision/error_code`；
- 设备解析 `listen_ack`，不能只记录日志；
- 新增 `audio_mode_state` 发送函数；
- 新增独立会议模式状态机；
- 新增不受 VAD/最大时长限制的连续采集路径；
- 会议控制使用独立的“最新期望状态 + revision”存储或高优先级控制通道，不得因普通命令队列满而静默丢失开启/关闭命令；
- 云端增加会议专用流式写入与滚动分段；
- 云端会议模式绕过普通 ASR 自动回复链；
- 云端和设备都增加 request_id 幂等与 revision 校验。

## 30. 验收标准

协议和实现至少通过以下测试后才能进入产品使用：

1. UI 开启成功：收到云端授权后进入 active，授权前无音频上传；
2. UI 开启拒绝：云端拒绝后设备保持 disabled；
3. UI 开启超时：重试次数耗尽后设备保持 disabled；
4. UI 立即关闭：无论云端是否响应，设备都能停止录音；
5. 云端主动开启和关闭：M5/M6/M7 状态一致；
6. 重复 request_id：不会创建第二场会议或第二个音频会话；
7. stale revision：设备拒绝旧命令，不改变当前状态；
8. 快速重复点击：不会出现两个并发采集任务；
9. 连续采集 10 分钟：不会被 VAD、静音或普通最大录音时长停止；
10. 连续静音 3 分钟：所有音频帧仍上传，会议保持 active；
11. 云端滚动分段：每 60 秒生成文件，设备端音频序号连续无边界缺口；
12. 正常关闭：最后一帧包含 END，云端收到 listen stop；
13. 网络断开：设备立即停止，不进行离线录音；
14. 重连恢复：保持 meeting_id、更换 audio_session_id，并记录中断；
15. 普通对话隔离：会议音频不触发普通 ASR 自动回复和 TTS；
16. 播放互斥：进入会议前设备播报完成淡出并停止；
17. 音频队列背压：发生丢帧时状态和 dropped_frames 可被观测；
18. ID 边界：非法字符和超长 ID 被明确拒绝，不允许静默截断后继续运行。

## 31. 最终约定摘要

1. 设备 UI 开启通过 `audio_mode_request enabled` 请求云端授权；
2. 云端通过 `audio_mode_request_ack ok:true` 返回 meeting_id、audio_session_id 和 revision；
3. 设备通过 `listen start mode:meeting` 建立音频会话；
4. 收到成功 `listen_ack` 后设备才开始上传，并上报 `audio_mode_state active`；
5. 会议期间所有音频帧连续上传，VAD 不参与启停和删帧；
6. 设备保持一个连续音频会话，云端每 60 秒滚动存储文件；
7. 会议音频完全隔离于普通 ASR、智能体自动回复和 TTS；
8. 本地关闭立即生效，先 END、排空队列、listen stop，再报告 disabled；
9. 云端主动控制使用 `device_command/set_audio_mode`，与 UI 请求共用同一设备状态机；
10. v1 不持久化、不离线录音，断线后停止，由云端重新授权恢复。
