# 会议模式与监控模式设备/正式云端完整通信协议 v1

状态：设备端当前实现的单一交付协议，可直接交付正式云端工程师  
日期：2026-07-23  
协议版本：1.0  
适用设备：Nuwax AI Partner 7.0 ESP32-S3（COM8 正式设备）

## 1. 文档范围、依据与优先级

本文件独立、完整地定义正式云端为联调会议模式和监控模式必须实现的全部设备线通信，包括：WebSocket 接入、鉴权、能力协商、保活、控制 JSON、ACK、`listen` 生命周期、NA v1 二进制音频、长流存储、异常收尾和验收标准。正式云端工程师只需以本文件为对接依据。

普通对话已有的 ASR、智能体回复和 TTS 协议不在本文件中重复定义，因为会议和监控 ACTIVE 期间必须隔离这些链路。现有普通对话通信保持不变。

测试网关的 `POST /api/device/command` 只是本机注入云端命令的管理接口，不属于设备 WebSocket 线协议，正式云端不需要照搬该 REST API。正式云端只需通过已建立的设备 WebSocket 发送本文规定的 JSON。

同目录文件的定位如下：

- `meeting-mode-current-v1.md`：会议模式历史分项说明，保留用于追溯。
- `monitoring-mode-v1.md`：监控模式历史分项说明，保留用于追溯。
- `meeting-mode-v1.md`：包含 `revision`、`base_revision` 和 `meeting_id` 的旧目标设计，不是当前固件协议，不得用于对接。
- `schemas/nuwax-device-protocol.schema.json`：通用协议辅助 Schema，尚未完整覆盖两种音频模式，不能替代本文件。

历史分项文件 SHA-256 为：

```text
meeting-mode-current-v1.md
AD9265A440B187DBFC65F2DEC61D5F5A331A661A80ACBEFA3C3B53E167350AE8

monitoring-mode-v1.md
2508CC010F69A287D1F369080398DEF6FA8D2B0A255B26785B9816D59AE6B76B
```

当前协议不使用 `revision`、`base_revision`、`meeting_id` 或独立的 `audio_mode_state` 消息。通用 Schema 的 `request_id` 上限仍为 39 个字符，而模式代码缓冲区允许 47 个字符；正式云端按本文第 4 节的兼容上限发送。

本文结论以当前设备源码为主、测试网关和实机记录为交叉验证，不以历史设计文档推测代码行为。

## 2. 当前模式边界与历史说明修正

### 2.1 两种模式的业务边界

| 项目 | 会议 `meeting` | 监控 `monitoring` |
| --- | --- | --- |
| 开启来源 | 设备 UI 请求、云端命令 | 仅云端命令 |
| 关闭来源 | 设备 UI、30 分钟连续安静、致命错误、云端命令、断线 | 云端命令、致命错误、断线 |
| 上传内容 | 每个成功读取的音频帧；VAD 不门控上传 | 每个成功读取的音频帧；无 VAD |
| 设备处理 | 慢速 DC 去除、能量判别、默认目标约 +0.53 dB 且硬上限约 +1.58 dB 的受限弱声增益 | 不做模式专用 AEC、VAD、AGC、降噪或增益 |
| 普通 ASR/智能体/TTS | ACTIVE 期间隔离 | ACTIVE 期间隔离 |
| 播报 | 不播报 | 不播报 |
| 长流落盘 | 必须 | 必须 |

普通对话、会议和监控互斥，同一时刻只能有一个麦克风及上行音频会话所有者。

### 2.2 会议模式历史说明修正

| 原文位置 | 原描述或遗漏 | 当前代码实际行为 |
| --- | --- | --- |
| 第 6 节 | 弱声增益最高约 `+1.58 dB`，容易被理解为当前目标增益 | 默认目标是 Q15 `34816`，约 `+0.53 dB`；Q15 `39322`、约 `+1.58 dB` 只是硬上限，默认配置不会把它作为目标 |
| 第 7 节 | 人声连续约 640 ms 或非安静声连续约 2000 ms 才重置不活动计时 | 已过时。任一有效帧只要被判定为疑似人声，或被判定为非安静环境声，就立即把不活动计时清零；无效采集帧既不累计也不清零 |
| 第 5 节 | 最多等待 500 ms 让播放收尾 | 500 ms 后若播放仍未结束，会议模式会记录警告并继续开始采集，首批会议音频仍可能带有残余设备播报 |
| 第 10 节 | ACK 表未列出全部值 | 还可能返回 `audio_mode_busy`，表示监控模式或其他独占音频模式占用切换权 |
| 第 8、10 节 | `reason`、`message` 看起来像封闭枚举 | 两者应按可扩展诊断字符串处理。例如本地关闭原因还可能出现 `uplink_final_enqueue_failed`；云端不能因未知值拒绝整个消息 |
| ID 与幂等说明 | 容易被理解为设备保存了完整请求历史 | 设备只有在途状态和当前状态，没有跨连接、跨会话的持久重放缓存；重试规则必须由云端遵守 |

会议模式能量判别只影响 `VAD_SPEECH` 标志、弱声小增益和 30 分钟连续安静计时，不会删除、置零或停止上传音频帧。
当前会议专用链路不运行软件 AEC、频域降噪、静音门或强杂音抑制。

### 2.3 监控模式历史说明修正

| 原文位置 | 原描述或遗漏 | 当前代码实际行为 |
| --- | --- | --- |
| 第 5、8 节 | `message` 被称为固定值，表中没有全部分支 | 开启尚在 STARTING 时若被关闭命令替代，原开启请求会收到 `superseded_by_disable` |
| 第 8 节 | `audio_mode_busy` 只解释为其他模式占用 | 它也可能表示监控自身处于 STARTING 或 STOPPING，当前命令不能并行执行 |
| 第 5、8 节 | 错误值像封闭枚举 | `message` 必须按可扩展诊断字符串处理，以 `ok` 为首要成功判据 |
| 验收状态 | 只写到 171.136 秒实机录音 | 2026-07-22 已完成 3600.832 秒 COM8 连续监控，225052 帧，序号无缺口、无重复、无乱序，END、`listen stop` 和 WAV 均完整 |
| 静音行为 | 原实现持续上传所有静音 | 当前实现连续 120000 ms 没有确认人声后先发送 `monitoring_capture_control/pause`，收到云端成功 ACK 后才关闭当前音频流；检测到人声后先发送 `resume`，收到 ACK 后才建立新流并补发预录 |

监控模式仍不因静音退出。暂停只停止云端音频会话和音频上传，监控模式保持 ACTIVE，麦克风继续进行本地轻量 VAD。当前固件将 ESP-SR VAD mode 0 与低阈值自适应能量判断并联以保护弱人声；VAD 分配失败时能量判断仍可独立工作。监控链路仍不启用自动增益、会议增益、AEC 或模式专用降噪。

### 2.4 两种模式共同约束

1. 非法、缺字段、字段过长或不被模式解析器识别的控制 JSON 没有统一错误 ACK；它可能被忽略，也可能落入通用命令分派并返回缺少 `mode/state` 的 `unknown command` ACK。云端必须在发送前校验，并实现 ACK 超时，不能依赖非法消息的具体响应。
2. 单个入站 WebSocket 帧超过 16 KiB 时，设备会以 WebSocket `1009` 关闭连接，而不是返回业务错误 ACK。
3. 设备不会等待 `listen_ack` 才上传二进制音频；云端必须在收到 `listen start` 时立即建立接收器。
4. 两种模式都是长流，正式云端不得沿用普通对话的 VAD 端点、最大话语时长或内存累积策略。
5. 正常关闭需要分别核验 END、`listen stop`、控制 ACK 和存储结果，不能只看到其中一个就判定整场完整。
6. 设备传输层在所有运行模式下每 25 秒主动发送 WebSocket Ping；云端必须回复 Pong。连续 60 秒没有收到任何下行帧时，设备才会断线重连。

## 3. WebSocket 接入与能力协商

### 3.1 连接

设备使用配置的 `/api/device/ws` WebSocket 地址连接云端。若设备配置了令牌，握手携带：

```http
X-Bridge-Token: <由两端安全配置的令牌>
```

Upgrade 令牌不得写入本协议、日志或业务 JSON。正式云端必须在 WebSocket Upgrade 阶段完成这一层鉴权。

设备绑定密钥与 `X-Bridge-Token` 是两个独立凭据。绑定成功后云端返回的逐设备密钥由设备通过首个 `hello.device_secret` 发送；它不得替代 Upgrade 请求头，也不得出现在日志中。

当前 COM8 正式云端地址为：

```text
ws://192.168.31.127:8099/api/device/ws
```

正式云端必须同时通过 Upgrade 令牌鉴权和 `device_id + device_secret` 绑定校验，之后才可接受能力协商及模式控制。

### 3.2 WebSocket 帧与保活限制

当前设备 WebSocket 接收器只接受以下服务端帧：

1. `FIN=1` 的完整帧，不支持 continuation 或分片消息。
2. `RSV1/RSV2/RSV3=0`，不支持 per-message compression。
3. 服务端帧必须不带 mask，符合 RFC 6455 的服务端发送规则。
4. 支持 text、binary、close、ping 和 pong opcode；其他 opcode 会触发协议错误关闭。
5. 单帧 payload 最大 16 KiB。

设备等待任一入站 WebSocket 帧的空闲超时为 60 秒，并在所有模式下每 25 秒主动发送 RFC 6455 Ping。正式云端必须回复 Pong；设备也会自动回复云端主动发送的 Ping。会议或监控音频是设备到云端的上行数据，不能代替云端到设备方向的保活帧。

### 3.3 `hello` 能力

连接后设备首先发送 `hello`。当前 COM8 固件的完整结构如下，`device_id` 取设备 eFuse 派生的稳定 `NX-...` UID，示例中的 UID 只是占位值：

```json
{
  "type": "hello",
  "version": 1,
  "device_id": "NX-XXXX-XXXX-XXXX",
  "device_secret": "<绑定返回的逐设备密钥>",
  "transport": "websocket",
  "features": {
    "audio_upload": true,
    "meeting_mode_v1": true,
    "monitoring_mode_v1": true,
    "monitoring_pause_resume_v1": true,
    "audio_playback": true,
    "mcp": true
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
    "frame_duration_ms": 16,
    "speaker": true,
    "wake_word": true,
    "vad": true,
    "full_duplex": true,
    "shared_i2s": true,
    "hardware_profile": "esp_vocat_v1_0",
    "aec": true,
    "aec_reference": "software",
    "has_ref": true,
    "software_ref": true,
    "hardware_ref": false,
    "ref_slot": -1,
    "afe_input_format": "MR"
  }
}
```

`channels` 和 `mic_channels` 均表示云端收到的媒体声道数，当前固定为单声道 `1`。
CyberVoc 底层 ES7210 仍使用 4-slot TDM 采集，但设备只选择 slot0 生成上行 PCM；
硬件 TDM slot 数不得作为 WAV 或 ASR 的声道数。

云端只有在对应的 `meeting_mode_v1` 或 `monitoring_mode_v1` 为 `true` 时才允许开启该模式。必须以每次连接实际收到的 `hello` 为准，不得仅根据设备型号或历史连接缓存能力。音频解析以 `hello.audio_params` 和每个 NA 头共同校验；同一会话内格式不得无通知变化。

`hello.audio_params.aec` 和 `aec_reference` 描述当前固件的普通对话能力，不表示会议或监控链路会启用 AEC；两种模式的实际处理边界以第 2 节为准。

云端可以返回连接确认；当前设备不等待该消息作为后续动作门禁。建议最小格式为：

```json
{
  "type": "hello",
  "ok": true,
  "session_id": "cloud_ws_session_001"
}
```

### 3.4 消息总表

| 方向 | WebSocket 类型 | `type` 或内容 | 用途 | 对端动作 |
| --- | --- | --- | --- | --- |
| 设备 -> 云端 | text | `hello` | 设备身份、模式能力、音频能力 | 建立本连接的设备上下文 |
| 设备 -> 云端 | text | `ping` | 应用层保活 | 回复 `pong` |
| 设备 -> 云端 | text | `audio_mode_request` | UI 开启/关闭会议，或会议本地退出通知 | 回复匹配的 `audio_mode_request_ack` |
| 云端 -> 设备 | text | `audio_mode_request_ack` | 批准或拒绝设备会议请求 | 设备按 `request_id` 关联 |
| 云端 -> 设备 | text | `device_command/set_audio_mode` | 云端开启/关闭会议或监控 | 等待匹配的 `device_command_ack` |
| 设备 -> 云端 | text | `device_command_ack` | 云端命令最终结果 | 以 `request_id`、`ok` 和目标状态收敛 |
| 设备 -> 云端 | text | `listen start/stop` | 音频会话边界 | start 时立即创建接收器；stop 时记录边界 |
| 云端 -> 设备 | text | `listen_ack` | 可选的会话接收确认 | 设备当前不解析、不等待 |
| 设备 -> 云端 | text | `monitoring_capture_control` | 两分钟无确认人声后请求暂停，或检测到人声后请求恢复 | 必须按 `request_id` 回复 ACK |
| 云端 -> 设备 | text | `monitoring_capture_control_ack` | 批准或拒绝监控音频暂停/恢复 | 设备仅在匹配且 `ok:true` 时改变上传状态 |
| 设备 -> 云端 | binary | 一个或多个 NA v1 帧 | 连续上行音频 | 按第 9 节循环解析并流式落盘 |
| 云端 -> 设备 | WebSocket Ping | RFC 6455 Ping | 长流下行保活 | 设备自动回复 Pong |

当前测试网关还可能发送 `audio_session`、`audio_stats` 等诊断 JSON。设备只记录其中部分信息；它们不是会议/监控正式协议的必需消息，正式云端不得依赖这些测试网关扩展完成状态机。

### 3.5 应用层与 WebSocket 保活

设备在普通空闲阶段约每 25 秒发送一次应用层 ping：

```json
{
  "type": "ping",
  "now_ms": 123456789
}
```

云端收到应用层 ping 后回复以下最小消息即可，设备只要求 `type` 为 `pong`：

```json
{
  "type": "pong",
  "session_id": "cloud_ws_session_001",
  "now": 123456799
}
```

云端还必须按 RFC 6455 对设备发出的 WebSocket Ping 返回携带相同 payload 的 Pong。

RFC 6455 Ping 由 WebSocket 传输层独立维护，每 25 秒发送一次，不依赖普通对话、会议、监控、播放或休眠状态。云端必须回复 Pong；任一入站帧超过约 60 秒没有到达时，设备接收任务会断开并进入重连。

### 3.6 `listen` 音频会话边界

模式真正开始上传前，设备发送：

```json
{
  "type": "listen",
  "state": "start",
  "mode": "meeting",
  "audio_session_id": "meeting_stream_1_a1b2c3d4",
  "request_id": "cloud_meeting_open_001"
}
```

监控模式把 `mode` 改为 `monitoring`，并使用设备生成的 `monitoring_stream_*` ID。`request_id` 关联触发本次会话的开启请求。云端收到 start 时必须同步建立音频接收上下文，随后立即允许 binary 到达。

正常结束时设备发送：

```json
{
  "type": "listen",
  "state": "stop",
  "mode": "meeting",
  "audio_session_id": "meeting_stream_1_a1b2c3d4"
}
```

stop 当前不带 `request_id`。云端必须优先按 `audio_session_id` 关联，并以 start 时记录的模式为准；不得只因 stop 的 `mode` 缺失或不一致而遗留活动会话。若异常设备版本连 ID 也遗漏，则只能在该 WebSocket 连接至多一个活动上行会话的前提下收敛当前会话，并把协议偏差记入会话元数据。

正式云端可以回复：

```json
{
  "type": "listen_ack",
  "ok": true,
  "session_id": "cloud_ws_session_001",
  "state": "start",
  "mode": "meeting",
  "audio_session_id": "meeting_stream_1_a1b2c3d4"
}
```

当前设备不会解析或等待 `listen_ack`，所以它只能作为云端自身的可观测性消息，不能用来反向控制设备何时开始或停止上传。

## 4. JSON 与 ID 的严格规则

1. 所有消息必须是一个完整 JSON object，UTF-8 编码，不允许拼接两个 JSON，也不允许尾随非空白内容。
2. `type`、`command`、`request_id`、`mode` 和 `state` 各只能出现一次，不允许重复键。
3. 云端生成的 `request_id` 建议限制为 1 至 39 个字符，以同时兼容当前通用 Schema；设备模式缓冲区的绝对上限为 47 个字符。
4. `request_id` 字符集固定为 `[A-Za-z0-9_.-]`。
5. ACK 必须原样回显请求的 `request_id`，不能重新生成。
6. 同一连接内，同一 `request_id` 只能表示同一请求内容。重试必须复用原 ID，不能用同一 ID 改变 `mode` 或 `state`。
7. 新请求必须使用新 ID；WebSocket 重连后也应使用新 ID。
8. `audio_session_id` 最长 47 个字符，正式云端同样只使用 `[A-Za-z0-9_.-]`。会议模式可以由云端提供或由设备生成；监控模式始终由设备生成，实际值以 `listen start` 为准。
9. 云端发给设备的会议拒绝 `reason` 最长 31 个 ASCII 字符；`reason` 和 `message` 都是诊断字符串，不是稳定业务枚举。业务判断优先使用 `ok`、`mode`、`state` 和关联状态机。
10. 请求发起方生成 `request_id`：设备生成 UI 会议请求 ID，云端生成 `device_command` ID；响应方不得替换它。设备本地关闭会议是一个新请求，使用新的 ID。

监控解析器会严格拒绝已知字段重复、非法 `request_id` 字符或尾随垃圾数据；会议解析器当前较宽松。云端必须统一按上述更严格规则发送，不能依赖会议解析器的宽松行为。

控制消息字段含义：

| 字段 | 类型 | 必填范围 | 含义 |
| --- | --- | --- | --- |
| `type` | string | 全部 JSON | 消息种类，必须使用本文给出的精确小写值 |
| `command` | string | `device_command`、`device_command_ack` | 固定为 `set_audio_mode` |
| `request_id` | string | 全部控制请求和 ACK | 请求关联及在途幂等键；ACK 原样回显 |
| `mode` | string | 模式控制和 `listen` | 只能是 `meeting` 或 `monitoring`，不得使用 `monitor` 别名 |
| `state` | string | 模式控制和 `listen` | 控制使用 `enabled/disabled`；会话边界使用 `start/stop` |
| `ok` | boolean | ACK | `true` 表示目标动作被接受并达到 ACK 对应阶段 |
| `interrupt_current_turn` | boolean | 会议开启可选 | 请求云端取消旧对话轮次；监控无需发送 |
| `current_turn_cancelled` | boolean | 会议 UI 开启 ACK 可选 | 报告云端是否取消旧轮次，不是设备启动门禁 |
| `audio_session_id` | string | `listen` 必填；会议开启可选 | 一次音频流的唯一关联键，不能跨重连复用 |
| `reason` | string | 设备会议请求/云端会议拒绝可选 | 请求、拒绝或本地退出的诊断原因 |
| `message` | string | `device_command_ack` | 命令结果诊断值；按可扩展字符串处理 |

除本文明确列出的可选字段外，正式云端不要发送测试字段、camelCase 别名或 `null` 占位值。

## 5. 会议模式完整流程

### 5.1 设备 UI 开启

设备发送：

```json
{
  "type": "audio_mode_request",
  "request_id": "meeting_open_1_a1b2c3d4",
  "mode": "meeting",
  "state": "enabled",
  "interrupt_current_turn": true
}
```

只有设备检测到当前正在播报时才携带 `interrupt_current_turn:true`；未播报时省略该字段。设备在等待批准前已经发起本地约 200 ms 淡出，因此云端收到该标志后必须同步取消旧轮次并停止生成、下发旧 TTS。

```text
设备发现正在播报时，本地发起约 200 ms 淡出并取消播放
设备 -> 云端: audio_mode_request meeting enabled
云端取消旧对话轮次和后续旧 TTS（interrupt_current_turn=true 时为必须动作）
云端 -> 设备: audio_mode_request_ack meeting enabled/disabled
设备批准后最多等待播放收尾 500 ms
设备 -> 云端: listen start, mode=meeting, audio_session_id=...
云端立即建立会议音频接收及异步落盘上下文
设备 -> 云端: NA v1 binary 音频流
```

云端对 UI 开启请求必须在 5 秒内回复，建议服务端目标响应时间不超过 1 秒。批准消息至少包含：

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "meeting_open_1_a1b2c3d4",
  "ok": true,
  "mode": "meeting",
  "state": "enabled",
  "audio_session_id": "meeting_audio_20260723_001",
  "current_turn_cancelled": true
}
```

`audio_session_id` 和 `current_turn_cancelled` 对设备均为可选字段，但正式云端建议提供。`current_turn_cancelled` 只报告云端动作结果，不是设备开始会议的门禁。

若拒绝或无法准备存储，返回 `ok:false, state:"disabled"`。设备在发送开启请求前可能已本地取消播放，因此即使拒绝，云端也不得继续下发被取消轮次的旧 TTS。

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

### 5.2 云端主动开启

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

实际成功顺序是：

```text
云端先取消旧对话轮次并停止继续产生旧 TTS
云端 -> 设备: set_audio_mode enabled
设备本地淡出并取消正在播放的音频
设备 -> 云端: listen start meeting
设备初始化麦克风和上行队列并进入 ACTIVE
设备 -> 云端: device_command_ack ok=true, state=enabled, message=active
设备 -> 云端: NA v1 binary 音频流
```

`listen start` 先于成功 ACK。云端不能等待 `device_command_ack active` 后才创建音频接收器。

设备真正进入 ACTIVE 后返回：

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

该 ACK 当前不携带 `audio_session_id`，实际 ID 必须取此前的 `listen start`。

### 5.3 设备 UI、本地超时或本地错误关闭

正常顺序是：

```text
设备停止继续采集
设备发送带 END 的最后一个真实 NA 帧并排空上行
设备 -> 云端: listen stop meeting
设备 -> 云端: audio_mode_request meeting disabled, reason=...
云端 -> 设备: audio_mode_request_ack meeting disabled
```

设备本地关闭不等待最后这个 ACK。若启动在进入 ACTIVE 前失败，设备可能只有 `listen stop`，不保证再发送 `audio_mode_request disabled`。云端必须能清理这种半启动会话。

设备本地关闭通知示例：

```json
{
  "type": "audio_mode_request",
  "request_id": "meeting_close_2_e5f6a7b8",
  "mode": "meeting",
  "state": "disabled",
  "reason": "ui_close"
}
```

云端原样回显 ID：

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "meeting_close_2_e5f6a7b8",
  "ok": true,
  "mode": "meeting",
  "state": "disabled"
}
```

当前常见本地 `reason` 包括但不限于：

```text
ui_close
quiet_timeout
mic_read_timeout
uplink_enqueue_failed
uplink_final_enqueue_failed
uplink_finish_failed
listen_stop_failed
```

### 5.4 云端主动关闭

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_meeting_close_001",
  "mode": "meeting",
  "state": "disabled"
}
```

云端在发送关闭后仍必须继续接收音频，直到收到 END 和 `listen stop`。设备完成收尾后返回：

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

## 6. 监控模式完整流程

### 6.1 云端开启

监控只能由云端开启。云端先取消旧对话轮次、停止继续生成旧 TTS，再发送：

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_monitoring_open_001",
  "mode": "monitoring",
  "state": "enabled"
}
```

成功顺序为：

```text
云端 -> 设备: set_audio_mode monitoring enabled
设备停止普通采集、自动追问和唤醒词采集
设备约 200 ms 淡出并取消当前播放，最多等待 500 ms
设备 -> 云端: listen start monitoring, audio_session_id=设备生成值
云端立即建立监控音频接收及异步落盘上下文
设备初始化麦克风和上行队列并进入 ACTIVE
设备 -> 云端: device_command_ack ok=true, state=enabled, message=active
设备 -> 云端: 连续 NA v1 binary 音频；本地 VAD 只用于两分钟暂停/恢复判断
```

其中 start 和成功 ACK 分别为：

```json
{
  "type": "listen",
  "state": "start",
  "mode": "monitoring",
  "audio_session_id": "monitoring_stream_1_a1b2c3d4",
  "request_id": "cloud_monitoring_open_001"
}
```

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

成功 ACK 不携带 `audio_session_id`；云端必须使用 `listen start` 中的设备生成值。

与会议模式不同，500 ms 后若播放仍未停止，监控模式不会带着残余播放继续采集，而是返回 `ok:false, message:"playback_stop_failed"`。

### 6.2 两分钟无确认人声后的暂停与恢复

监控刚进入 ACTIVE 时立即建立并上传第一条音频流。连续 120000 ms 没有确认人声后，设备先发送：

```json
{
  "type": "monitoring_capture_control",
  "request_id": "mon_pause_1_a1b2c3d4",
  "mode": "monitoring",
  "state": "pause",
  "audio_session_id": "monitoring_stream_1_a1b2c3d4",
  "reason": "no_speech",
  "silence_ms": 120000
}
```

云端必须校验当前活动监控会话 ID，并回复：

```json
{
  "type": "monitoring_capture_control_ack",
  "request_id": "mon_pause_1_a1b2c3d4",
  "mode": "monitoring",
  "state": "pause",
  "audio_session_id": "monitoring_stream_1_a1b2c3d4",
  "ok": true,
  "reason": "acknowledged"
}
```

暂停成功顺序固定为：

```text
设备继续上传并等待 pause ACK
-> 收到匹配的 ok:true ACK
-> 最后一个真实 NA 帧带 END
-> listen stop monitoring
-> 停止上传，保留本地麦克风 VAD
```

暂停期间检测到包括弱人声在内的连续人声后，设备为下一条流预生成新 `audio_session_id`，发送 `state:"resume"`。云端只有在上一条流已暂停且当前没有活动音频会话时才返回 `ok:true`。设备收到匹配 ACK 后发送新 `listen start`，先上传滚动预录，再继续实时音频。

相同请求重试必须复用同一 `request_id`，云端必须幂等返回相同结果。同一 ID 携带不同 state 或 session ID 时返回 `ok:false, reason:"request_id_conflict"`。设备最多发送三次；仍无 ACK 时停止本次监控，不能在双方状态未知时自行暂停或恢复。

### 6.3 云端关闭

```json
{
  "type": "device_command",
  "command": "set_audio_mode",
  "request_id": "cloud_monitoring_close_001",
  "mode": "monitoring",
  "state": "disabled"
}
```

关闭顺序为：

```text
设备停止继续采集
设备发送带 END 的最后一个真实 NA 帧并排空上行
设备 -> 云端: listen stop monitoring
设备 -> 云端: device_command_ack ok=true, state=disabled, message=disabled
```

其中 stop 和关闭 ACK 分别为：

```json
{
  "type": "listen",
  "state": "stop",
  "mode": "monitoring",
  "audio_session_id": "monitoring_stream_1_a1b2c3d4"
}
```

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

监控模式没有 UI 关闭消息，也没有 30 分钟静音自动退出；两分钟逻辑只暂停上传，不退出监控模式。

## 7. 完整 ACK 语义

### 7.1 会议模式 `device_command_ack.message`

| `ok` | `state` | `message` | 云端处理 |
| --- | --- | --- | --- |
| true | enabled | `active` | 新会议已进入 ACTIVE |
| true | enabled | `already_active` | 已在会议中；作为开启成功处理 |
| true | disabled | `disabled` | 关闭收尾成功 |
| true | disabled | `already_disabled` | 已关闭；作为关闭成功处理 |
| false | disabled | `audio_mode_busy` | 其他独占模式占用，保持原状态 |
| false | disabled | `meeting_busy` | 会议自身处于冲突转换状态 |
| false | disabled | `superseded_by_disable` | 在途开启被关闭替代；等待关闭请求自身的 ACK |
| false | disabled | `start_cancelled` | 开启在进入 ACTIVE 前被本地操作取消 |
| false | disabled | `capture_failed` | 麦克风、内存、上行、传输或收尾失败 |
| false | 请求目标值 | `control_queue_full` | 设备控制队列已满，命令未受理 |

### 7.2 监控模式 `device_command_ack.message`

| `ok` | `state` | `message` | 云端处理 |
| --- | --- | --- | --- |
| true | enabled | `active` | 新监控已进入 ACTIVE |
| true | enabled | `already_active` | 已在监控中；作为开启成功处理 |
| true | disabled | `disabled` | 关闭收尾成功 |
| true | disabled | `already_disabled` | 已关闭；作为关闭成功处理 |
| false | disabled | `audio_mode_busy` | 其他模式占用，或监控自身正在 STARTING/STOPPING |
| false | disabled | `superseded_by_disable` | 在途开启被关闭替代；等待关闭请求自身的 ACK |
| false | disabled | `capture_failed` | 麦克风、内存、上行、传输或收尾失败 |
| false | disabled | `playback_stop_failed` | 500 ms 内没有完成播放取消，监控未开始 |
| false | 请求目标值 | `control_queue_full` | 设备控制队列已满，命令未受理 |

云端必须以 `ok` 判断成功或失败。`ok:false` 时的 `state` 是请求相关字段，不保证能表达另一个正在运行模式的真实状态。

## 8. ACK 超时、重试和竞态

以下是正式云端的推荐实现策略，不改变设备协议字段：

1. 设备 UI 的 `audio_mode_request enabled` 必须在 5 秒内得到云端 ACK。
2. 云端发送 enabled 后，建议等待设备 ACK 最多 10 秒。
3. 云端发送 disabled 后，设备可能等待最终入队和上行排空；建议等待最多 20 秒，同时持续接收音频和 `listen stop`。
4. 超时后不得直接假定模式已关闭。先检查当前连接是否仍存在、是否已经收到 `listen start/stop` 或对应 ACK。
5. 需要重试同一在途命令时，复用相同 `request_id` 和完全相同的 JSON 语义；不要并行使用新 ID 重发。
6. STARTING 或 STOPPING 中的同 ID 重试可能不立即得到第二个 ACK，原命令完成后才返回最终 ACK。
7. ACK 丢失后重试可能得到 `already_active` 或 `already_disabled`，两者分别按目标状态已达成处理。
8. 连接断开后旧请求全部失效；重连后使用新 `request_id` 和新音频会话。

## 9. NA v1 二进制音频

当前 COM8 上行配置为 PCM16、16000 Hz、单声道、16 ms 标称帧长。NA v1 头固定 22 字节：

| 偏移 | 长度 | 字段 | 当前值或说明 |
| --- | --- | --- | --- |
| 0 | 2 | magic | ASCII `NA` |
| 2 | 1 | version | `1` |
| 3 | 1 | codec | `1=PCM16`，`2=Opus` |
| 4 | 1 | flags | bit0 START，bit1 END，bit2 VAD_SPEECH |
| 5 | 1 | channels | 当前为 `1` |
| 6 | 2 | sample_rate | 当前为 `16000` |
| 8 | 2 | frame_duration_ms | 当前为 `16` |
| 10 | 4 | sequence | 首帧为 0，逐帧加 1 |
| 14 | 4 | timestamp_ms | 当前为 `sequence * 16` |
| 18 | 2 | payload_len | 本 NA 帧的音频负载字节数 |
| 20 | 2 | reserved | 当前为 0；不是 CRC |
| 22 | 可变 | payload | 当前为 little-endian PCM16 |

头部所有多字节整数均为 little-endian。一个 WebSocket binary message 通常包含连续 4 个完整 NA 帧，最后一批可能少于 4 帧。云端必须按每帧 `payload_len` 循环解析整个 binary message，不能把一个 WebSocket message 当成一个音频帧。

I2S 部分读取时，`payload_len` 可能小于标准的 512 字节，但 `frame_duration_ms` 与 `timestamp_ms` 仍按 16 ms 标称值填写。因此：

```text
PCM 样本数 = payload_len / 2 / channels
真实录音时长 = 累计 PCM 样本数 / sample_rate
```

不得仅使用 `最后 timestamp + frame_duration_ms` 计算 WAV 时长。

会议模式和监控模式的第一帧带 START，最后一个真实帧尽最大努力带 END；本地判定为人声的帧还可能带 VAD_SPEECH。监控云端不得使用该标志丢弃帧，它只用于诊断和暂停计时说明。若会话在读到第一帧前就失败，可能只有 `listen start` 和 `listen stop`，没有 START/END binary 帧。

## 10. 长流、背压和存储要求

PCM16 16 kHz 单声道的音频负载速率约为：

```text
32000 bytes/s
115200000 bytes/hour（不含 NA 头和 WebSocket 开销）
```

设备 PCM 上行队列当前最多容纳 128 个批次，每批 4 帧、每帧 16 ms，理论缓存约 8.192 秒。这只是短时抖动缓冲，不是云端可以暂停读取的额度。队列已满后，普通批次只等待约 80 ms，持续背压会让设备以 `uplink_enqueue_failed`/`capture_failed` 结束模式。

正式云端必须满足：

1. WebSocket 读循环持续实时消费 binary 数据，不在读循环内同步执行 ASR、模型调用或慢速对象存储。
2. 使用有界队列把音频交给独立的异步磁盘/对象存储任务；队列异常必须显式标记，不得静默丢帧。
3. 不得把完整会议或监控音频只保存在内存中。
4. 不得应用普通对话的 VAD 端点、无声终止或最大话语时长。
5. 监控可能持续数小时，应支持文件分段，避免单个 RIFF/WAV 接近 4 GiB 上限。
6. 每段 WAV 必须是 PCM16、对应采样率和通道数，并正确回写 RIFF/data 长度。
7. 进程异常退出时保留 `.partial` 或等价恢复文件，并记录已写字节、会话 ID、序号状态和不完整原因。
8. 正常完成至少同时满足：START 已见、序号连续、END 已见、`listen stop` 已见、存储成功、WAV 长度正确。

模式音频必须按 `listen.mode` 路由到独立存储链路：

```text
meeting    -> 会议录音存储，不进入普通 ASR/智能体/TTS
monitoring -> 监控录音存储，不进入普通 ASR/智能体/TTS
```

## 11. 断线与异常收尾

1. WebSocket 断开时设备立即退出会议或监控并释放麦克风，但无法保证 END、`listen stop` 或控制 ACK 已送达。
2. 云端必须把断线前已收到的 PCM 落盘为可读取文件，同时标记为异常结束，不得伪造 END 或协议完整。
3. 缺 START、缺 END、首序号不为 0、序号缺口、重复、乱序、格式变化和落盘失败都必须单独记录。
4. 重连后设备不会自动恢复旧模式。云端需要继续监控时，应在新连接完成 `hello` 后重新发送 enabled；会议模式也必须重新发起。
5. 重连后的会话必须使用新的 `request_id` 和 `audio_session_id`，不得拼接到旧 WAV。
6. 设备返回 `capture_failed` 时，即使收到过 `listen stop`，云端仍要检查 END、序号和存储结果，不能直接标记完整。

## 12. 模式互斥与切换

普通对话、会议和监控不能并行拥有麦克风及上行。正式云端切换模式时必须串行执行：

```text
关闭当前模式
-> 等待 END
-> 等待 listen stop
-> 等待当前模式 disabled ACK 或本地关闭通知
-> 再发送目标模式 enabled
```

不要通过同时发送两个 enabled 命令实现切换。冲突命令会得到 `audio_mode_busy`，设备不会自动把会议切为监控，也不会自动把监控切为会议。

会议或监控期间，云端不得继续向设备发送旧轮次 TTS binary。设备会丢弃迟到音频，但云端仍应在模式开启前取消旧轮次，从源头停止生成和下发。

## 13. 正式云端验收矩阵

| 编号 | 场景 | 必须验证 |
| --- | --- | --- |
| C01 | 能力协商 | 未声明对应 feature 时拒绝发命令；声明后允许 |
| C02 | 设备 UI 开启会议 | 5 秒内批准/拒绝；批准后建立 meeting 接收器 |
| C03 | 设备 UI 关闭会议 | END、`listen stop`、disabled 请求及 ACK 关联正确 |
| C04 | 云端开启/关闭会议 | `listen start` 先于 active ACK；关闭后完整收尾 |
| C05 | 云端开启/关闭监控 | 设备生成 session ID；连续上传阶段完整收尾 |
| C06 | 播放中开启 | 旧轮次被云端取消；不再下发旧 TTS；设备完成淡出 |
| C07 | 模式互斥 | 冲突 enabled 返回 `audio_mode_busy`，原模式不受破坏 |
| C08 | 重复 enabled/disabled | 正确接受 `already_active`/`already_disabled` |
| C09 | STARTING 中 disable | 开启 ACK 可为 `superseded_by_disable`，关闭 ACK 最终成功 |
| C10 | 非法 JSON/非法 ID | 无 ACK 时超时可控，不泄漏资源，不无限重试 |
| C11 | 序号缺口/重复/乱序 | WAV 可保存但必须标为协议不完整 |
| C16 | 监控静音暂停/人声恢复 | 120000 ms 后 pause 请求先于 END/stop；resume ACK 先于新 start/音频；弱人声进入预录且不丢首字 |
| C12 | 缺 END 或断线 | 保存部分音频，标记异常，不自动拼接到重连会话 |
| C13 | 慢存储/背压 | WebSocket 读取不被存储阻塞；无设备上行队列溢出 |
| C14 | 会议至少 10 分钟 | 全帧持续保存，不进入普通 ASR/智能体/TTS |
| C15 | 监控至少 1 小时 | WAV 长度、序号、END、stop、ACK 和存储全部完整 |
| C16 | 正式 URL 与鉴权 | `X-Bridge-Token` 验证、重连及错误码处理正确 |
| C17 | 长流 WebSocket 保活 | 设备每 25 秒发送 Ping且云端回复 Pong；连续运行超过 60 秒不重连 |
| C18 | WebSocket 帧约束 | 不分片、不压缩、不掩码，单帧不超过 16 KiB |

正式联调通过标准不是“能收到音频”，而是控制状态、协议完整性、音频完整性和持久化完整性四项同时通过。

## 14. 当前实机证据与尚未完成项

当前测试网关已获得以下实机结果：

1. 会议模式：`meeting_audio_mrvsj3io_87283c1f`，747.088 秒，46693 帧，序号 `0..46692`，无缺口、重复或乱序，START/END、`listen stop`、WAV 和存储均完整。
2. 监控模式：`monitoring_stream_10_6fd44abb`，3600.832 秒，225052 帧，序号 `0..225051`，无缺口、重复或乱序，START/END、`listen stop`、WAV 和存储均完整；PCM data 为 115226624 bytes。

这些结果证明设备端和测试网关闭环成立，不代表正式云端已经完成对接。正式云端仍需按第 13 节独立验收。

当前设备源码工作树还没有用正式发布提交或固件 build hash 绑定本协议。正式交付前，设备端应补充实际烧录固件的版本号、构建时间和二进制 SHA-256，云端验收记录也应保存这些值。

## 15. 设备端代码依据

本完整协议的关键结论来自以下当前实现：

```text
main/audio_modes/meeting/meeting_protocol.c
main/audio_modes/meeting/meeting_mode.c
main/audio_modes/meeting/meeting_processor.c
main/audio_modes/monitoring/monitoring_protocol.c
main/audio_modes/monitoring/monitoring_mode.c
main/conversation/audio_uplink.c
main/audio_frame.c
main/session_protocol.c
main/ws_transport.c
main/nuwax_ai_partner_main.c
sdkconfig
```

其中：

- `meeting_processor.c` 定义会议能量判别、弱声增益和连续安静计时。
- 两个 `*_mode.c` 定义状态机、淡出、麦克风所有权、ACK 和关闭顺序。
- `audio_uplink.c` 定义 4 帧聚合、队列深度、背压等待和收尾超时。
- `audio_frame.c` 定义 22 字节 NA v1 头、字节序和 START/END/VAD 标志。
- `session_protocol.c` 定义 `hello.features` 与 `listen` 消息。
- `ws_transport.c` 定义 `X-Bridge-Token` 和 16 KiB 入站帧限制。
