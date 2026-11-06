# SD 会议监控场次上传协议

音频上传基线：7.0.201 正式云端配置；模式状态同步约定更新至 7.0.209（2026-10-30）。有卡会议改为满文件封口后即可上传，不再等待整场结束；会议和监控分别维护顺序队列，同模式的旧场次完成后再处理新场次。消息名称、字段、ACK、二进制和落卡格式不变，云端与 App 无须增加协议。监控实时／历史属性仍取决于录制来源；无卡流程不变。功能逻辑与异常处理参见 [功能逻辑与异常处理](SD会议监控功能逻辑与异常处理.md)。

SD 会议／监控的 begin、Binary 分片、commit、session_end 及 ACK 均复用设备配网后建立的正式云端 WebSocket，使用现有设备身份认证。设备不再连接 PC 测试网关，也不进行局域网网关发现；不新增 SD 专用 hello。断网后保留未确认数据，恢复连接后继续按本协议上传。

**第 3.4 节保留统一场次 ID，7.0.208 起控制命令可省略 ID，由设备分配并回传实际 ID；7.0.209 起每个云端连接就绪后完整上报两种模式的实际状态。96 字节文件头原保留区增加文件开始时间、持续时长，见第 7.1 节。消息名称、ACK、64 字节传输头、音频帧和 CRC 规则不变。**

## 1. 范围与约定

| 概念 | 定义 |
| --- | --- |
| 场次 | 一次会议或监控从开启到结束。开启方生成、设备保存 audio_session_id；暂停、恢复、断网和上传重试不换 ID；结束后重新开启使用新 ID |
| 文件 | 场次内一段已封存的 SD 归档。segment_index 从 0 递增；封存后内容、编号不变 |
| 上传请求 | 一次上传 JSON 操作。每次请求生成新的 request_id，ACK 原样返回；重试不改变场次或文件身份。模式控制命令的重试规则见第 3.4 节 |
| 场次唯一键 | 已认证的设备身份 + audio_session_id |
| 文件唯一键 | 已认证的设备身份 + audio_session_id + segment_index；不包含实时／历史前缀 |

**正常上传每场只发一次 begin。** ACK 丢失、断线、设备重启或设备回收上传缓存后的恢复阶段允许重发同一场次的 begin，云端恢复原场次，不重复建场。已尝试文件仍通过现有 resume 查询云端持久化位置。begin 不是模式开启命令，也不要求在录制开始时联网发送。

云端必须允许首次 begin 创建音频场次，包括整场离线录制的情况；不依赖此前收到 listen 或模式开启记录，不能把控制请求的 request_id 当作音频场次 ID。begin 只建立音频接收场次，不再次触发模式开启操作。

文件最大 **4 MiB（4,194,304 字节）**，允许不足上限的尾文件和提前封存文件，不补零。本文规定“封存后如何上传”，不强制每个文件必须填满 4 MiB。

有卡会议以单场总时长 8 小时为上限，在线和离线时间都计入，重连不重新计时；到时停止采集并封存尾文件；此前封口文件可在录制期间上传，离线未确认数据保留至恢复连接。有卡会议按 SD 实际剩余容量判断，不再受旧的总计 1.5 GiB 录音配额限制；空间不足时不能覆盖未确认会议。7.0.172 起，有卡监控取消该时长限制：断网期间保持监控，恢复在线继续同一场次，云端关闭后封存收尾。原有本地关闭、关机和硬件故障保护仍有效；静音暂停保存不等于结束监控。

有卡监控按 SD 文件系统实际容量保存，不受原固定录音配额限制。预留容量的 1%（最低 16 MiB、最高 128 MiB），另为下一文件及目录写入留出余量。7.0.193 起，只清理当前绑定身份下已获云端确认、且本地确认状态已持久化的监控文件；未确认、未封存、隔离、正在上传的文件以及会议和无关文件均保留。清理后仍不足以保证预留空间，或卡不可写时，停止新增保存并进入原有存储异常处理，不覆盖待传音频、不虚报保存成功。

空间不足造成录制缺口时，最终 `session_end` 仍按原字段 `recording_complete=false` 报告，不复用 `segment_index` 或更换 `audio_session_id`。旧固件曾覆盖的文件无法恢复，升级后保留其缺失记录及不完整标记，不把旧缺失冒充上传成功。没有新增消息或字段。

现有协议每场最多 1,000,000 个文件的编号边界保持不变；达到边界会安全停止，不能继续产生云端不支持的编号。持续写满文件约需 1,457 天达到该边界，提前封存的小文件会缩短此时间。取消 8 小时限制不等于取消协议字段边界。

## 2. 消息名称

### 2.1 连接与 SD 卡状态：hello

设备仅在建立云端连接时，通过已有的 `hello` 上报顶层布尔字段 `has_sd_card`。开机连接和断网后的重新连接各发送一次；开启会议／监控不额外发送 hello，不新增独立的 SD 上传能力协商。

设备发送示例；能力、音频参数和固件信息按实际运行状态填写，不作为 SD 卡有无的判断依据：

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
    "meeting_mode_v1": true,
    "monitoring_mode_v1": true,
    "monitoring_pause_resume_v1": true,
    "remote_ble_reprovision": true,
    "volume_changed_v1": true,
    "remote_wake_word_config": true,
    "notify_playback_v1": true,
    "ota_v1": true,
    "music_playback_mp3": false,
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
    "version": "7.0.169",
    "release_sequence": 7000169,
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

`firmware` 仅在启用 OTA 时出现，其中 `image_sha256` 仅在运行固件身份已就绪时提供。云端应兼容额外字段。

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| type | string | 固定为 hello |
| version | integer | 当前为 1，与 SD 文件格式版本无关 |
| device_id | string | 设备身份，与当前已认证连接一致 |
| device_secret | string | 设备现有云端认证密钥，每次建立新连接时携带并校验 |
| transport | string | 固定为 websocket |
| has_sd_card | boolean | true：最近一次完成的 SD 检测成功；false：未检测到可识别的卡 |
| features / audio_params / firmware | object | 设备能力、音频参数和固件信息；不用于判断 SD 卡有无 |

`has_sd_card` 是最近一次完成的检测结果，不保证当前可写或有剩余空间。热插拔不保证即时反映，卡损坏或无法挂载时也可能为 false；短暂重挂载期间保留上次结果。旧固件不带该字段时按“未知”处理，不能当作 false。

发送时机：

1. 开机连接云端成功后，首先发送一次完整 hello，携带当时的 SD 检测结果。
2. 断网后建立新的云端连接时，仍发送该连接的首次 hello，重新读取并上报 SD 检测结果，保留原有认证流程。
3. 同一连接内，会议／监控的开启、结束、暂停和恢复，以及重复开启命令，都不触发 hello；不定时重复上报 SD 状态。
4. 离线开启仍按本地录制规则执行，不为发送 hello 等待联网。建连 hello 发送失败时按原有流程关闭该连接并重连，不在同一连接中循环补发。

**hello 不要求单独 ACK，不以收到 hello 回复作为录制或上传的额外条件。** 连接就绪、模式控制及其 ACK 沿用原有流程，hello 不是开启命令，也不代替场次 begin。

云端如需回复，使用以下成功格式，不另设 hello_ack：

```json
{
  "type": "hello",
  "ok": true,
  "session_id": "cloud_connection_001"
}
```

其中 session_id 是当前云端连接会话标识，不是音频场次 audio_session_id；重连后连接 session_id 可以变化，正在录制或补传的 audio_session_id 保持不变。若回复携带 authenticated，则必须为 true，并同时携带与本机一致的 device_id。

云端在建连 hello 中校验设备身份和密钥，保存 has_sd_card；不应等待开启会议／监控后再收到 hello。当前设备同一连接只发送一次 hello，云端无需为本功能新增“重复 hello 更新状态”的处理。身份或密钥不匹配仍按认证失败处理。以下场次 begin／Binary／commit／session_end 流程不变。

### 2.2 场次上传消息

以下是实际 type 字符串，ACK 名称在对应请求后追加 `_ack`。

| 操作 | 会议 | 监控 |
| --- | --- | --- |
| 建立／恢复场次 | meeting_audio_begin | monitoring_audio_begin |
| 提交文件 | meeting_audio_commit | monitoring_audio_realtime_commit 或 monitoring_audio_history_commit |
| 结束场次 | meeting_audio_session_end | monitoring_audio_session_end |

监控 begin、session_end 不区分实时和历史，**只有文件 commit 区分**。不再发送 monitoring_audio_realtime_begin、monitoring_audio_history_begin，也不增加逐文件开始消息。

## 3. 通信流程

### 3.1 正常上传

```text
设备                                             云端
场次 begin ------------------------------------> 建立／恢复场次，持久化场次身份和模式
           <------------------------- begin_ack  ok=true 后才允许发送本场文件

文件 0 的第一个 Binary 分片 -------------------> 按场次、文件编号、offset 保存
文件 0 的后续 Binary 分片 ---------------------> 继续写入同一个文件，不逐片确认
……直到文件 0 的最后一个 Binary 分片发送完毕……
文件 0 的一次 commit --------------------------> 校验整个文件，持久化文件、索引及处理任务
           <------------------------ commit_ack  ok=true 后设备持久化确认状态、清理本地文件

文件 1 的全部 Binary 分片 → 一次 commit → 一次 commit_ack
……后续每个文件重复上述流程，不再发送 begin……

场次 session_end ------------------------------> 核对文件清单，持久化场次结果
           <------------------- session_end_ack
```

**Binary 分片只是一个文件的传输切块，不是独立文件。正常成功路径下，每个文件的全部分片发送完毕后，只发送一次 commit，并接收一次 commit_ack；不逐片发送 commit，也不逐片 ACK。** 失败重试和断线恢复按第 5 节处理。

收到成功的 commit_ack 并持久化本地确认状态后，才能把文件当作已上传完成。监控在同一场次内按 segment_index 递增上传，旧监控场次完成 session_end 确认后才上传新监控场次；不因实时／历史分类插队。会议采用相同的文件顺序与场次收尾规则，和监控使用独立队列；文件身份由 Binary 头携带。

### 3.2 会议

在线、离线都先录入 SD。每个文件达到容量上限后完成同步、关闭和封口，再按 `segment_index` 从小到大上传；后续音频继续写下一个文件。当前上限约 4 MiB，16 kHz 单声道 PCM 约每 125.94 秒封口一个满文件。正在写入的文件不上传，后续文件不能越过前面的未确认文件。

断网时继续本地录音；恢复连接后，从队头最早未确认文件恢复。多场会议保留各自 `audio_session_id`，旧场次全部文件与 session_end 确认完成后，才上传后一个会议场次，新场录音可继续。临时错误在原队头退避，身份／内容冲突保留原件并暂停该模式队列；监控使用另一条队列。

例如一场生成 4 个满文件和 1 个尾文件：

```text
开始录音文件 0
文件 0 封口 → 开始录文件 1 → meeting_audio_begin → begin_ack → 上传文件 0 → commit(0) → commit_ack
文件 1 封口 → 开始录文件 2 → 上传文件 1 → commit(1) → commit_ack
文件 2 封口 → 开始录文件 3 → 上传文件 2 → commit(2) → commit_ack
文件 3 封口 → 开始录文件 4 → 上传文件 3 → commit(3) → commit_ack
结束会议、封口尾文件 4 → 上传文件 4 → commit(4) → commit_ack
meeting_audio_session_end(segment_count=5) → meeting_audio_session_end_ack
```

上例省略重复前缀，commit 为 `meeting_audio_commit`，序号为 `segment_index`。无重试时仍为 **1 次 begin、5 次 commit、1 次 session_end**。离线积压时，封存与上传不必同步发生，但上传顺序相同。尚在录制时，即使已封口文件全部确认，也不能发送 session_end；开始时间可在 begin 上报，最终场次持续时间在结束后由 session_end 补齐。

### 3.3 监控

1. 首个文件准备上传时发送 monitoring_audio_begin；成功后，本场后续文件直接发送 Binary 和 commit。
2. 在线时边录边存，已封存文件按 segment_index 从小到大上传，录制可继续产生后续文件。离线时只存 SD；恢复后从最早未确认文件继续，不跳过历史文件去传新文件。例如正在录文件 5、正在传文件 3，则下一文件为 4。
3. 多场监控按创建先后排队：旧场全部文件确认、场次结束确认并保存 done 后，才上传新场；新场录音仍可正常保存。旧场暂时失败就在原位置退避重试，不让后面的监控场次越过；真实断线按第 5 节查询可靠位置。
4. 停止监控后先封存尾文件。本场全部可上传文件均收到 commit_ack 且本地确认状态持久化后，才发送 monitoring_audio_session_end；收到 session_end_ack 并持久化 done 后才完成本场。仍在录制或静音暂停时，即使当前没有待传文件，也不发送 session_end。原有真实缺失文件仍按 recording_complete=false 处理。

**实时／历史只取决于录制来源：**在线录制的文件使用 monitoring_audio_realtime_commit，离线录制的文件使用 monitoring_audio_history_commit。排队时长、断线恢复、设备重启和本场结束都不改变其原有分类；分类不决定上传顺序。在线／离线属性切换时沿用现有提前封存规则，避免同一文件混用来源。

云端以 commit.type 决定处理方式：realtime 在成功提交后进入实时分析，history 仅补历史，不触发过期告警。已提交文件重试时保持首次成功提交的处理结果，不能因前缀变化重复告警。

一次断网恢复的通信示例，始终使用同一个 audio_session_id：

```text
在线：monitoring_audio_begin → begin_ack
      文件 0 → monitoring_audio_realtime_commit → 对应 ACK
离线：文件 1 存入 SD
恢复：monitoring_audio_begin（原场次 ID）→ begin_ack
      旧文件 1 → monitoring_audio_history_commit → 对应 ACK
      新文件 2 → monitoring_audio_realtime_commit → 对应 ACK
停止：如有尾文件，先上传并 commit 确认
      monitoring_audio_session_end → monitoring_audio_session_end_ack
```

有卡监控在线、离线均按 **4 MiB 文件上限**封存：剩余空间不足保存下一条完整音频记录时，立即封存当前文件。在线时安排上传并继续录制下一个文件；离线时保留至联网后上传。不再按 960 ms 或 30 秒定时封存。

按当前音频格式，一个满文件为 4,193,930 字节、6,297 帧，包含 125.94 秒音频，不补零、不拆分帧。停止监控、静音暂停、在线／离线采集属性切换仍会提前封存不足上限的文件。连续 120 秒未确认有效人声后暂停保存、确认人声后恢复的策略不变。因此，持续有人声时通常约两分钟产生一个满文件；云端处理还需等待上传和 commit 成功，不能按旧小文件策略预期秒级告警。

### 3.4 模式开关与录音场次 ID

7.0.208 起，会议／监控开关是状态命令，控制请求的 `audio_session_id` 可省略。设备仍为每场录音保留唯一 ID，成功开启 ACK 和上传消息返回实际录音 ID。报文字段、消息类型、Binary、归档头、文件 commit 和 session_end 格式不变。

| 开启来源 | ID 的生成与同步 |
| --- | --- |
| App／云端开启 | ID 可省略，由设备生成；携带合法未使用 ID 时优先采用。ID 不适合作为 SD 身份或已经使用时生成新 ID，成功 ACK 返回实际录音 ID |
| 设备本地开启（在线或离线） | 设备生成新 ID；录制进入 ACTIVE 且在线时，通过 audio_mode_request 携带该 ID；云端沿用，不另分配 ID |
| 当前录制断线恢复 | 设备发送新 request_id 的 audio_mode_request，audio_session_id 保持不变；只同步状态，不重新开始录音 |
| 整场离线结束／重启后补传 | 通过场次 begin 建立或恢复业务记录，不发送 enabled 请求重新开启已结束录音 |

场次 ID 是 **1～35 个 ASCII 字母、数字、下划线或短横线**，区分大小写，不截断、不改写。云端应生成从未用于其他场次的新 ID，避免仅大小写不同的 ID（SD 文件系统不区分大小写）。设备本地生成 `meeting_` 或 `monitor_` 加 16 位随机十六进制文本。实际录音 ID 一致贯穿：同步请求、成功开启 ACK、SD 场次记录、归档头、Binary 头、上传 begin／commit／session_end；控制请求没有 ID 或需重新分配时，以成功 ACK／上传 begin 的实际 ID 为准。

#### 云端开启

会议示例；监控将 mode 改为 monitoring。每次新操作使用新的 request_id，同一命令重试沿用该 request_id。下面 audio_session_id 是可选字段，旧云端可以完全不发送。控制 request_id 为 1～47 个 ASCII 字母、数字、`_`、`-` 或 `.`，仅用于请求与 ACK 关联，不能代替场次 ID。

```json
{
  "type": "device_command",
  "request_id": "cmd_open_001",
  "command": "set_audio_mode",
  "mode": "meeting",
  "state": "enabled",
  "audio_session_id": "meeting_0123456789abcdef"
}
```

设备确认本地录制已经进入录音状态后回复：

```json
{
  "type": "device_command_ack",
  "request_id": "cmd_open_001",
  "command": "set_audio_mode",
  "mode": "meeting",
  "state": "enabled",
  "ok": true,
  "audio_session_id": "meeting_0123456789abcdef",
  "message": "active"
}
```

#### 本地开启与连接状态同步

7.0.209 起，云端不再下发初始化开关状态命令来覆盖设备。每次连接就绪后，设备用已有 audio_mode_request 上报会议、监控的完整实际状态；正在录制的模式为 enabled，另一模式为 disabled；普通对话时两者均为 disabled。用户主动开关或模式切换仍走原 device_command。

运行中的有卡场次使用原 audio_session_id，reason 沿用 offline_audio_recovered。每次连接生成新的同步 request_id，同一次同步的重试沿用该 request_id；不停止、重启录音，也不重新计算时长。

```json
{
  "type": "audio_mode_request",
  "request_id": "meeting_sync_002",
  "mode": "meeting",
  "state": "enabled",
  "reason": "offline_audio_recovered",
  "audio_session_id": "meeting_0123456789abcdef"
}
```

云端按设备身份及该场次 ID 查找或创建业务记录，保存后回复原 ID：

```json
{
  "type": "audio_mode_request_ack",
  "request_id": "meeting_sync_002",
  "mode": "meeting",
  "state": "enabled",
  "ok": true,
  "audio_session_id": "meeting_0123456789abcdef"
}
```

未运行的模式上报 disabled，reason 为 device_state_sync，不带 audio_session_id。普通对话状态会发送下面两条；会议中只发送其中监控关闭这一条，监控中只发送其中会议关闭这一条，并配合上述运行模式的 enabled 上报：

```json
{"type":"audio_mode_request","request_id":"meeting_sync_003","mode":"meeting","state":"disabled","reason":"device_state_sync"}
```

```json
{"type":"audio_mode_request","request_id":"monitoring_sync_004","mode":"monitoring","state":"disabled","reason":"device_state_sync"}
```

云端按 mode 更新设备当前状态，使用 audio_mode_request_ack 回显 request_id、mode、state 和 ok。上报是状态同步，不创建新的录制操作；旧场次上传不得覆盖当前状态。

首次发送失败按 1、2、4、8、10 秒退避，此后每 10 秒重试，直到成功发出或连接／状态变化。成功发出后最多成功发送 3 次，回执等待预算为 5 秒。回执拒绝、丢失或超时均保留本地状态；回执不直接启停录音，也不抢占正常对话。启停过渡完成后再上报稳定状态。无卡运行模式继续使用原开启／重连恢复请求；另一模式的关闭上报在实时采集前处理。

#### 重复请求与兼容边界

- 开启、关闭都允许省略 audio_session_id，不再由 CONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT 决定。缺失、空或不适合 SD 的控制 ID 由设备分配实际录音 ID。非字符串、超过解析器长度、重复字段、转义 NUL 等畸形消息仍拒绝。
- 同一模式已 ACTIVE 时，新的开启命令返回 ok=true、message=already_active 和实际场次 ID，即使其携带其他 ID 也不重新录音。启动中同一请求不重复启动；其他请求仍可能返回 busy；停止收尾期间不能新开场。
- 关闭针对该模式的当前录音执行；未带 ID、旧 ID 或 ID 不匹配都不阻止关闭。重连后也不等待五秒同步。云端须按上述约定停止下发初始化开关状态命令；设备收到明确的 device_command 仍执行。audio_mode_request_ack 只是上报回执，不直接启停。
- 切换到另一模式时先校验实际新录音身份、存储及资源。已使用的云端 ID 改为生成新录音 ID，不覆盖旧文件。存储暂忙时异步等待，未完成有效校验前不停止原模式；通过后才停原模式、封存并启动目标模式。真实失败仍返回原 mode_handoff_failed，不新增字段。

最小关闭命令（会议示例）：

```json
{"type":"device_command","request_id":"cmd_close_001","command":"set_audio_mode","mode":"meeting","state":"disabled"}
```

**切换收尾和命令超时（7.0.170 起）：**

- 必要收尾包括停止采集、释放麦克风、缓冲尾音落盘、文件封存和场次信息保存。**不等待积压音频上传、文件 commit_ack 或场次 session_end_ack。**上传独立在后台执行。
- 云端 `cloud-command-timeout-seconds` 保持 30 秒。设备从收到命令开始计时，排队、校验、旧模式收尾和新模式启动共用 20 秒执行期限；新模式实际进入采集后立即回复成功。
- 20 秒到期仍未完成时，取消本次启动，返回现有 `device_command_ack`，`ok=false`、`message=mode_handoff_failed`。旧录音继续安全封存；原模式如果已经停止，不自动恢复，也不在封存完成后迟到启动新模式。重复请求不重置本次期限。
- ACK 发送及有限重试共用收到命令后 25 秒的截止时间，最多 5 次，每次发送最长 250 ms。截止后停止发送该命令的待发结果；已成功启动的采集不会仅因成功 ACK 发送失败而被停止。
- 时间限制使用设备单调时钟，不依赖 server_ms。25 秒是设备发送截止时间，不是网络断开时的送达保证；云端从发出命令开始计时，通信延迟会占用双方之间的余量。正常切换无需等待 20 秒，实机时延另行测量。
- SD 中仍有该 ID 的场次记录时，不复用它创建新录音，而是生成新的实际 ID。近期已结束开启请求受有限内存去重缓存保护；这不是跨重启、无限历史去重。云端须为每次新操作生成新 request_id，并避免重放历史操作。
- 开启 ACK 发送失败时按现有有限次数重试；仍在录制时，断线保留尚未完成的 ACK 重试，但不延长原 25 秒截止时间。场次已结束或重启后无需补发 enabled ACK，云端可直接通过相同 ID 的 begin 关联原业务记录，不能据补传重新开启录制。
- 7.0.171 起，进入停止流程或实际采集结束时立即作废旧开启成功 ACK，近期重复旧命令不再触发开启。有卡本地停止独立发送现有 `audio_mode_request(state=disabled)`，不因开启 ACK 未送达而省略；发送失败只在原连接内有限重试，下一场开始时取消旧关闭请求。拒绝命令、队列满等控制回复也采用短发送期限，避免阻塞切换超时处理。消息字段与音频上传协议不变。
- 云端开启后的当前连接不再重复发开启同步。设备本地开启或录制中重连会同步；云端成功 ACK 宜回显实际场次 ID。回执携带其他 ID 也不改写录音身份；上报失败或无回执不阻止当前模式开关。
- 上传 begin 使用本场相同的 ID。云端按已认证设备身份 + audio_session_id 关联或创建一次场次，校验模式；后到的模式同步不能复制场次，旧场次补传不能覆盖另一场当前状态。模式冲突不覆盖已有记录。
- 无卡实时链路继续原流程。旧固件和旧 SD 数据仍用原已保存 ID 上传，不改名、不迁移二进制文件；云端须允许仅凭 begin 创建独立场次，旧数据缺少关联信息时不猜测绑定到当前其他业务记录。

## 4. JSON 格式

### 4.1 公共字段

本节约束适用于上传消息；模式控制消息按第 3.4 节定义。

| 字段 | 类型与约束 |
| --- | --- |
| type | 第 2 节的请求名称或对应 ACK 名称 |
| request_id | 1～39 个 ASCII 字母、数字、下划线或短横线；同一连接内不重复 |
| audio_session_id | 1～35 个上述字符；本场录制及后续上传期间不变 |
| segment_index | 整数，0～999,999；文件编号 |
| total_bytes | 整数，128～4,194,304；归档总长度，含文件头、记录长度、NA 帧和文件尾 |
| crc32 | uint32 十进制整数；计算方法见第 7 节 |
| ok | 布尔值，表示该次操作是否成功 |

文件 ACK 原样返回 request_id、audio_session_id、segment_index；场次 ACK 原样返回 request_id、audio_session_id。接收端忽略未知字段，但必须校验已定义字段；设备只接受当前连接、当前请求及对应 type 的 ACK。

### 4.2 建立场次

会议示例；监控仅将 type 替换为 monitoring_audio_begin / monitoring_audio_begin_ack。

**时间字段说明：从 7.0.164 起，设备在场次 begin 和 session_end 中发送 `session_time`。云端须兼容不带此字段的旧固件；缺少时间信息不能阻止场次建立、音频上传和确认。**
会议／监控仍在进行时，begin 的 duration_ms 填 null，结束时由 session_end 补齐；已经结束后才上传的场次，可在 begin 中提供最终时长。


设备发送：

```json
{
  "type": "meeting_audio_begin",
  "request_id": "begin_00000001",
  "audio_session_id": "meeting_0123456789abcdef",
  "session_time": {
    "started_at_ms": 1792634400000,
    "duration_ms": 600000
  }
}
```

云端回复：

```json
{
  "type": "meeting_audio_begin_ack",
  "request_id": "begin_00000001",
  "audio_session_id": "meeting_0123456789abcdef",
  "ok": true
}
```

成功 ACK 表示云端已持久化场次身份和模式，并允许在当前连接上传此场次。相同场次重复 begin 必须幂等；同一场次被声明为另一种模式则返回 conflict。连接恢复时每个需要继续上传的场次重新建立关联；新连接接管后，云端拒绝该场次旧连接的迟到写入和提交。

#### 4.2.1 场次时间

`session_time` 只用于场次 begin 和 session_end，表示整场时间。每个文件自己的时间写入 96 字节 SD 文件头，见第 7.1 节；commit 和 64 字节 Binary 传输头不增加时间字段。提供此对象时包含以下两个字段，ACK 格式不变，无须回显时间。

| 字段 | 含义 |
| --- | --- |
| started_at_ms | 本场录制开始的 UTC Unix 毫秒时间戳，正整数；无法确定时为 null。不是开始上传或云端收到文件的时间 |
| duration_ms | 本场从录制开始到结束的总持续毫秒数，非负整数；尚未结束或无法确定结束时刻时为 null |

两个字段使用 JSON 数值或 null；时间值按 64 位整数保存和运算，不得截断为 32 位。

上例表示北京时间 2026-10-22 10:00:00 开始、持续 10 分钟。时间戳不叠加时区偏移；云端展示时再转换时区。持续时间包含断网及场次内静音暂停，不包含结束后的封存、等待联网和上传耗时，也不等于有效音频时长；有效音频时长按第 7 节的帧数计算。

| 发送场景 | 时间填写方式 |
| --- | --- |
| 会议／监控结束后首次上传 | begin 中填写已确定的开始时间和最终持续时间 |
| 会议／监控仍在进行，开始上传第一个封存文件 | begin 中填写已确定的开始时间，duration_ms=null；结束后在 session_end 中补齐最终持续时间 |
| 断线后重新 begin | 使用同一场次已保存的时间；尚未结束时 duration_ms 仍为 null，已结束则可补入最终值 |
| 掉电后恢复未正常结束的场次 | 保留能恢复的开始时间；不能确定实际结束时刻时 duration_ms=null，并按原规则标记 recording_complete=false |

真实时间取自已认证云端连接的**应用层心跳** `pong.server_ms`。云端以生成该回复时的 UTC Unix 毫秒时间填写 server_ms，可回显设备 ping 中的 now_ms：

```json
{ "type": "pong", "now_ms": 123456, "server_ms": 1792634400000 }
```

now_ms 是云端原样回显的设备本机计时，不能当作真实日期。设备将 server_ms 与收到该心跳时的 64 位开机计时建立对应关系，再按场次开始时的计时差换算 started_at_ms，不能直接把上传时收到的 server_ms 当作场次开始时间。当前不做往返时延补偿，真实时间精度受网络延迟和设备时钟漂移影响。server_ms 必须是数值型整数，当前设备接受 2024-01-01 至 9999-12-31 的 UTC 毫秒值；缺少、字符串、秒级值或设备运行毫秒值均不用于校时。

已取得时间基准后，即使离线也可推算开始时间：`started_at_ms = 最近一次有效 server_ms + 场次开始计时 - 该心跳接收计时`。同一次开机先离线录制、后收到有效心跳，也可回算。旧场次重启恢复时使用已保存的开始时间，或该场次所属开机的已保存时间基准；不同开机的计时不能混用。

若断电前从未取得有效时间基准，且开始时间也未保存，旧场次日期为 null。完全断电后、尚未联网校时的新场次同样不能沿用上次开机的计时；本次开机后续收到有效心跳时才能回算。缺少或无效的 server_ms 不阻止录制和上传，不新增 SD hello 或时间协商请求。

开始时间一旦确定、最终持续时间一旦确定，设备均须可靠保存，重试和重启后保持一致；持续时间使用本机单调计时计算，不随云端校时跳变。云端在对应 ACK 前保存提供的时间，允许 null 补为已知值；缺失或 null 不覆盖已有值，重复请求保留已有非空值。时间不参与场次／文件唯一键，也不作为音频接收完整性的判断依据。旧固件与旧归档无法补出时间时按未知处理。

### 4.3 提交文件

文件发送完毕后，设备发送以下请求。监控分别使用 monitoring_audio_realtime_commit 或 monitoring_audio_history_commit，字段相同。

```json
{
  "type": "meeting_audio_commit",
  "request_id": "commit_00000002",
  "audio_session_id": "meeting_0123456789abcdef",
  "segment_index": 0,
  "total_bytes": 4193930,
  "crc32": 2156111519
}
```

云端回复：

```json
{
  "type": "meeting_audio_commit_ack",
  "request_id": "commit_00000002",
  "audio_session_id": "meeting_0123456789abcdef",
  "segment_index": 0,
  "ok": true
}
```

云端须完成：文件长度与末片位置一致、归档结构及音频参数合法、场次／模式／文件编号一致、CRC 正确，然后持久化文件、索引和去重状态，才回复 ok=true。文件无需等转写、总结或告警处理完毕才确认；处理任务须可靠记录并按文件唯一键去重。

commit 成功后文件内容不可修改。重复提交相同长度、CRC 的已提交文件返回成功，不重复处理；长度或 CRC 冲突返回 conflict，保留原文件。正常上传及容量清理均须先持久化成功确认，不删除未确认历史；旧固件已留下的缺失仍按原记录上报，不能当作上传成功。

### 4.4 结束场次

设备在录制结束、所有可上传文件提交成功后发送。以下为会议示例，监控替换为 monitoring_audio_session_end / monitoring_audio_session_end_ack。

```json
{
  "type": "meeting_audio_session_end",
  "request_id": "end_00000003",
  "audio_session_id": "meeting_0123456789abcdef",
  "segment_count": 5,
  "recording_complete": true,
  "session_time": {
    "started_at_ms": 1792634400000,
    "duration_ms": 600000
  }
}
```

```json
{
  "type": "meeting_audio_session_end_ack",
  "request_id": "end_00000003",
  "audio_session_id": "meeting_0123456789abcdef",
  "ok": true
}
```

- segment_count 为整数 0～1,000,000，表示文件编号范围 `[0, segment_count)`，含异常缺失的编号。若已有提交文件编号超出此范围，返回 conflict。没有保存音频的场次为 0，仍按 begin → session_end 确认，不生成空文件。
- session_time 按第 4.2.1 节填写：正常结束时提供最终持续时间，补齐 begin 时尚未知的信息；此前已上报的已知时间保持一致。旧固件可不带。
- recording_complete 为布尔值。为 true 时，范围内文件全部提交，模式一致、各 Footer.complete=1，采集区间不重叠；不满足则返回 incomplete 或 conflict，不能确认成完整场次。
- recording_complete=false：设备已无法恢复部分录音，包括监控循环存储覆盖的未确认历史。云端保存有效文件、缺失编号及不完整标记后确认，不无限等待缺失文件；被覆盖的编号仍计入 segment_count。一般上传失败不等于录音缺失，不能据此提前结束场次。
- 7.0.222 起，单个本地文件经校验、恢复后仍无法使用时，设备持久化该编号的终止缺失记录，再丢弃坏文件并继续后续文件；可重建的索引先恢复，无法重建的已封存编号按同样规则跳过。场次 ID、后续文件编号、原始时间和音频内容均不改变，不补空文件、不伪造文件 ACK。例如编号 2 无法恢复，仍上传 0、1、3、4，最终 segment_count=5、recording_complete=false。云端按本节已有的缺失编号处理，不增加消息或字段。断网、超时、内存不足及尚在写入的文件不属于此丢弃条件。
- ACK 在场次清单、完整性结果及后续处理任务可靠保存后返回，不等待大文件拼接或总结完成。相同清单重复结束返回成功；segment_count 或 recording_complete 与已确认清单冲突则拒绝。
- 不完整场次可接收原编号范围内迟到的有效文件并更新索引，但不自动把 recording_complete=false 改为 true。正常完整场次仅接受幂等重试，不接受新文件。

迟到文件的 commit_ack 不应等待全场次重扫。先持久化文件、索引和待更新任务，再回复文件成功；后台更新场次清单。任务失败或进程重启后继续处理，并确保旧任务不能清除新文件触发的更新。再次收到 session_end 时，须完成尚未处理的清单更新后确认。

### 4.5 失败回复

失败仍使用对应 ACK 和请求身份，ok=false，并携带 error_code；不发送与请求无关的通用 ACK。无法解析 JSON 或缺少可匹配的请求身份时终止连接，不伪造 ACK。例如：

```json
{
  "type": "meeting_audio_commit_ack",
  "request_id": "commit_00000002",
  "audio_session_id": "meeting_0123456789abcdef",
  "segment_index": 0,
  "ok": false,
  "error_code": "storage_unavailable"
}
```

| error_code | 云端与设备处理 |
| --- | --- |
| busy / storage_unavailable | 保留本地文件，退避后重试；云端不得把未可靠保存的数据报为成功 |
| incomplete | 文件或场次尚不完整；查询接收进度，补齐后重试 |
| crc_mismatch | 云端清除本文件未提交的错误接收数据后返回；设备复核本地文件，再从 0 重传；已提交文件不可清除 |
| invalid_argument / conflict | 本次请求非法或身份／内容冲突；保留原对象和本地数据，停止自动重试冲突场次。同模式的后续场次不得越过该队头，另一模式独立调度；核对冲突后恢复，不能将冲突数据当成已成功上传 |

未知 error_code 按暂时失败处理，不因错误响应删除文件。暂时失败按 1、2、4、8、16、30 秒退避，之后最长每 30 秒重试；成功后重置退避。没有“重试几次后删除音频”的规则，容量不足也不删除未确认音频；保留原文件，停止新增保存并按第 1 节处理。

只有明确的参数校验或内容冲突可返回 invalid_argument / conflict。磁盘故障、工作线程退出、内部异常应返回 storage_unavailable，资源繁忙返回 busy；不能按异常文本猜测为永久拒绝。设备构造本协议的上传控制消息或有卡模式同步消息时，如任一字段分配失败，整条消息不发送，保留场次供重试。

begin_ack、commit_ack 等待上限为 6 秒，session_end_ack 为 60 秒。超时不等于云端未保存；设备保留数据，恢复后查询或重发请求。session_end ACK 丢失时，同连接直接重发结束请求；若已重连，先 begin 恢复场次再重发结束请求，不重传已确认文件。

## 5. 断线恢复

恢复复用场次 begin，**不恢复逐文件 begin 流程**。设备在 resume 中列出需要核对的文件；正常首次 begin 不带 resume。

例如文件 1 中途断线：

```json
{
  "type": "meeting_audio_begin",
  "request_id": "begin_00000004",
  "audio_session_id": "meeting_0123456789abcdef",
  "session_time": {
    "started_at_ms": 1792634400000,
    "duration_ms": 600000
  },
  "resume": [
    { "segment_index": 1, "total_bytes": 4193930, "crc32": 3462264207 }
  ]
}
```

```json
{
  "type": "meeting_audio_begin_ack",
  "request_id": "begin_00000004",
  "audio_session_id": "meeting_0123456789abcdef",
  "ok": true,
  "resume": [
    { "segment_index": 1, "next_offset": 2097152 }
  ]
}
```

| next_offset | 设备下一步 |
| --- | --- |
| 0 | 从文件头开始发送 |
| 0 < next_offset < total_bytes | 从该字节偏移续传，再发送 commit |
| next_offset = total_bytes | 不发 Binary，直接 commit；覆盖数据已存完或 commit ACK 丢失的情况 |

恢复规则：

- resume 每条请求最多 16 个文件，不重复编号；成功 ACK 按请求顺序返回同样数量的条目，next_offset 必须为整数且在 0～该文件 total_bytes 内。多于 16 个时，仅在恢复阶段分批重发同一场次 begin 查询。
- 已经尝试发送但未在本地持久化确认的文件必须核对，包括旧固件曾被实时文件抢占的历史文件。重启后无法判断是否已尝试的文件也要核对；从未尝试的新文件可以直接从 0 上传，不需要查询。
- 云端只返回已可靠保存的**连续前缀**，不报告内存缓存长度；未知文件返回 0。恢复时丢弃未持久化的尾部，以返回的位置为准。
- 若云端已经保存该文件的长度、CRC，必须与查询一致；尚未收到 commit 的文件可在首次恢复查询时登记这两个值，并在后续 commit 再次校验。
- 查询发生长度、CRC 或模式冲突时，整个 begin 返回 ok=false、error_code=conflict，不返回可用的 resume，不改写冲突对象；设备隔离核对冲突项，其他场次仍可上传。成功回复缺条目或偏移越界时，设备不得猜测位置继续发送。
- 同一场次可以包含多个部分接收文件。查询不改变文件身份；实际再次发送仍按录制来源使用监控类型，不因重试改类。
- 重启后恢复旧场次的存量文件；中断录制的旧场次标记不完整。新的录制使用新场次 ID，不能把新开机时间混进旧场次。

云端进程重启后应能从持久化记录恢复场次、文件进度及提交结果。临时无活动只能标记“暂时结束／待补齐”，不能永久拒收迟到文件；明确的 session_end 按第 4.4 节处理。

## 6. Binary 传输头

每条 WebSocket Binary 消息为 **64 字节传输头 + N 字节归档内容**。WebSocket 自身的分片由接收端先重组；不能按 TCP 包边界解析。

整数均为小端无符号整数，字符串为 ASCII、NUL 填充。

| 偏移 | 字节数 | 字段和值 |
| ---: | ---: | --- |
| 0 | 6 | ASCII `NWXCH1` |
| 6 | 2 | 保留，0 |
| 8 | 4 | version=3，固定值，标识本文的场次／文件传输头 |
| 12 | 4 | offset，本片在该归档文件中的字节偏移 |
| 16 | 4 | N，1～8,064；消息总长必须为 64+N |
| 20 | 36 | audio_session_id，最长 35 字节，NUL 填充 |
| 56 | 4 | segment_index |
| 60 | 4 | flags：中间片为 0，文件最后一片为 1；其余位为 0 |
| 64 | N | 归档原始字节 |

这是固定的二进制布局标识，不增加 JSON 版本字段或协商消息。**此头不再携带 begin.request_id，不能按旧固件的 Binary 头解释。** 已有有效归档可用此传输头上传；新文件头的时间字段见第 7.1 节。

接收要求：

1. 本连接须已成功 begin 该场次，设备身份从已认证连接取得。offset=0 的首片可创建该文件的临时接收对象；无需文件 begin。
2. 同一文件按 offset 连续发送；不同文件可穿插，不要求文件编号按到达顺序递增。不得以“最近一个文件”猜测归属。
3. 全文件不超过 4 MiB。首次解析出归档头后，必须核对场次、模式和文件编号。分片可以跨文件头、记录和文件尾，不能把每片直接当 PCM。
4. flags=1 表示文件数据发送结束，接收端记录末尾位置；commit.total_bytes 必须等于该位置，并且此前没有字节缺口。短片不等于末片。恢复时若全部字节已持久化但末片状态丢失，云端应根据查询长度和完整归档重建末尾状态，再返回 next_offset=total_bytes，不能要求重发全部 Binary 来补末片标记。
5. 重复覆盖已收区域时逐字节校验：一致则忽略重复部分；不一致则拒绝，不覆盖原数据。对已提交文件同样适用。
6. 不合法的 Binary、字节缺口或写入失败时终止本次上传连接，保留可恢复状态；设备通过第 5 节恢复，不把发送成功当作文件确认。旧连接失效后不再接收其数据。

## 7. SD 归档结构

```text
96 字节 NWXOFF1 文件头
重复：4 字节 uint32 记录长度 + 完整 NA 帧
32 字节 NWXEND1 文件尾
```

音频为 **16 kHz、单声道、PCM16 小端、每帧 20 ms**。除 PCM 样本外，整数均为小端无符号整数。

### 7.1 文件头

| 偏移 | 字节数 | 字段和值 |
| ---: | ---: | --- |
| 0 | 8 | `NWXOFF1` + NUL |
| 8 | 2 | version=1 |
| 10 | 2 | header_bytes=96 |
| 12 | 1 | mode：0 会议，1 监控；与场次 begin.type 一致 |
| 13 | 1 | channels=1 |
| 14 | 2 | codec=1，PCM16 |
| 16 | 4 | sample_rate=16000 |
| 20 | 2 | frame_duration_ms=20 |
| 22 | 2 | source_flags=2，表示 SD 场次文件，不区分实时／历史 |
| 24 | 4 | segment_index，与传输头及 commit 一致 |
| 28 | 8 | start_uptime_ms，首帧开机相对毫秒时间，非 UTC |
| 36 | 48 | session_id，与 audio_session_id 一致，NUL 填充 |
| 84 | 8 | file_started_at_ms：首帧采集开始的 UTC Unix 毫秒，uint64；0 表示未知 |
| 92 | 4 | file_duration_ms：首帧开始至末帧结束的实际时间跨度，毫秒，uint32；0 表示未知 |

文件头**总长仍为 96 字节**，使用原来的 12 字节保留区，不再叠加一个文件头。两个字段按小端读取，文件内容的前 96 字节会随 Binary 数据上传；传输头自身仍为 64 字节。旧文件这 12 字节全为 0，云端按时间未知接收，不得把 0 当作 1970 年或拒收旧数据。

- `file_started_at_ms` 是该文件首帧的时间，不是整场开始时间、文件封存时间或接收时间。设备基于已保存的场次 UTC 与单调计时映射计算：`场次 started_at_ms + 文件 start_uptime_ms - 场次开始开机计时`。离线继续使用同一映射，后续心跳校时不移动同场文件的时间线。
- `file_duration_ms = 末帧采集开始计时 + 20 - 首帧采集开始计时`，包含文件内部的采集间隔，不含首帧前等待、末帧后静音、封存及上传耗时。有效音频时长仍为 `frame_count × 20`，不要混用。
- 文件封存时写定时间，上传失败、重试、断线和重启补传不修改已封存内容。掉电留下的未封存文件按保留下来的完整帧恢复时长，日期只使用该次开机已保存的场次时间／时间基准。没有可信日期时开始时间为 0，时长仍可有效。
- 封存后才获得时间基准的旧文件不原地回填，避免改变已开始上传的字节。文件日期未知时，云端可展示已知的场次日期，但不能据此声称知道每个文件的精确日期，也不能用上传到达时间或有效音频累加值冒充采集时间。文件日期为尽力提供，精度同第 4.2.1 节。
- `file_duration_ms` 最大为 4,294,967,295 ms；不能可靠确定或溢出时为 0，不截断回绕。云端对非零时长可按文件内 NA 时间戳展开后校验。帧时间戳低 32 位可能回绕，需要按相邻帧无符号差值展开。

例：文件首帧 UTC 为 `1792634400000`，保存 2 帧、两帧开始相隔 120,020 ms，则头内填写 `file_started_at_ms=1792634400000`、`file_duration_ms=120040`，文件覆盖到 `1792634520040`，有效音频仅 40 ms。不能用有效音频时长累加出后续文件的采集日期。

### 7.2 NA 音频帧

每条记录为 4 字节长度值 662，加 662 字节 NA 帧，共 666 字节。NA 帧结构：

| 偏移 | 字节数 | 字段和值 |
| ---: | ---: | --- |
| 0 | 2 | ASCII `NA` |
| 2 | 1 | version=1 |
| 3 | 1 | codec=1 |
| 4 | 1 | flags：START=0x01、END=0x02、VAD_SPEECH=0x04，可组合或为 0 |
| 5 | 1 | channels=1 |
| 6 | 2 | sample_rate=16000 |
| 8 | 2 | frame_duration_ms=20 |
| 10 | 4 | sequence，场次内采集帧序号 |
| 14 | 4 | timestamp_ms，采集开机毫秒时间低 32 位 |
| 18 | 2 | payload_bytes=640 |
| 20 | 2 | 保留，0 |
| 22 | 640 | 320 个有符号 int16 小端 PCM 样本 |

不要求每个文件出现 START/END；文件边界由归档头尾定义，上传成功由 commit_ack 定义。

### 7.3 文件尾与 CRC

| 偏移 | 字节数 | 字段和值 |
| ---: | ---: | --- |
| 0 | 8 | `NWXEND1` + NUL |
| 8 | 2 | version=1 |
| 10 | 2 | footer_bytes=32 |
| 12 | 4 | frame_count，NA 记录数 |
| 16 | 8 | payload_bytes，完整 NA 帧总字节数，含 NA 头 |
| 24 | 4 | crc32 |
| 28 | 1 | complete：1 正常封存，0 录音不完整；不是整场结束标记 |
| 29 | 3 | 保留，0 |

```text
payload_bytes = frame_count × 662
total_bytes = 96 + frame_count × 666 + 32
纯 PCM 字节数 = frame_count × 640
有效音频时长毫秒 = frame_count × 20
```

正常短文件 complete=1。complete=0 的文件在结构、CRC 正确时仍可成功提交，最终场次必须标为不完整。

CRC 使用 CRC-32/ISO-HDLC：反射多项式 0xEDB88320，初始寄存器和末尾异或均为 0xFFFFFFFF；检查值 `123456789 → 0xCBF43926`。只计算按顺序拼接的完整 NA 帧（头+PCM），不包括记录长度、归档头尾或传输头。必须满足 **计算值 = Footer.crc32 = commit.crc32**。Python 可从 crc=0 开始逐帧执行 `crc=zlib.crc32(na_frame, crc) & 0xffffffff`。

4 MiB 上限可容纳 6,297 帧，总长 4,193,930 字节，有效音频 125.94 秒；余量不足一帧时封存，不拆分 NA 记录。JSON 中长度、CRC 仅为示例，实际必须按本文件计算。
