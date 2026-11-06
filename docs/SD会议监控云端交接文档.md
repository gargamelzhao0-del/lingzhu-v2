# SD 会议监控云端交接文档

本文保留 7.0.157 及之前固件的逐文件 begin 协议，供兼容旧设备参考。7.0.158 已实现“场次级 begin、文件级 commit”，新接入请使用[SD 会议监控场次上传协议](SD会议监控场次上传协议.md)。

7.0.201 起，有卡会议也改为文件封口后按序上传，录制可继续；整场结束后才发送 session_end。以下“会议结束后才上传”等描述仅对应历史固件，当前时序及场次级 begin 请以上述新协议为准。

## 1. 接入与录音流程

本文描述有卡设备的 SD 文件上传，不需要额外的 SD 能力 `hello` 握手；无卡设备沿用原实时音频流程。

| 场景 | 设备行为 | 文件消息前缀 P | 云端处理 |
| --- | --- | --- | --- |
| 有卡会议 | 在线、离线均先录入 SD；会议结束后上传，离线则保留至恢复连接 | `meeting_audio` | 收齐场次后拼接、转写、总结 |
| 有卡监控在线 | 先写 SD，封存小文件后立即安排上传 | `monitoring_audio_realtime` | 文件提交后及时处理 |
| 有卡监控离线或积压 | 继续写 SD；恢复后优先发送新实时文件，同时补传旧文件 | `monitoring_audio_history` | 补入历史时间线，不触发过期实时告警 |

一次模式开启对应一个 `audio_session_id`；同场断网恢复不变，结束后再次开启使用新 ID。离线多场分别保存、分别上传。监控实时与历史离线上传文件共用原场次和文件编号。

有卡监控开启即连续录音；连续 120 秒未确认有效人声后暂停保存新音频，麦克风继续监听。再次连续检测到约 200 ms 有效人声后本地恢复，并带上最多 500 ms 前置缓存。有效人声会重新计时。暂停、恢复不结束场次，不需要云端额外 ACK；已保存文件仍继续上传。在线、离线使用相同录音策略，无卡监控和现有消息格式不变。

写卡队列积压时，上传暂停读取并保留连接，队列回落后继续；已收到的文件确认仍正常处理。某场次被拒绝只影响该场次，本轮继续尝试其他场次，失败文件保留待重试。设备收到文件成功确认并持久化确认状态后才删除本地文件。

7.0.151 起，异步保存的监控尾段按产生顺序写入。测试网关兼容旧固件已产生的尾段编号乱序：先按原编号核对文件是否齐全，再按归档内采集时间拼接监控音频，不更改文件标识；时间真正重叠仍拒绝完成确认。会议保持原有顺序校验。以上处理不增加协议字段或确认次数。

## 2. 交互顺序

**会议和监控都是逐文件上传：每个文件各有一次 begin 和 commit，整场结束另发 session_end。** `begin` 表示开始上传一个文件，不表示开启一次会议或监控。

同场所有文件共用 `audio_session_id`，用从 0 递增的 `segment_index` 区分；`request_id` 用于匹配本次请求与 ACK，不能作为场次或文件的幂等标识。

### 2.1 每个文件的共同流程

以下 `P` 取第 1 节的三个前缀，例如 `P_begin` 为 `meeting_audio_begin`。同一次文件上传的 begin、commit 及其 ACK 使用同一前缀。

```text
设备                                      云端
P_begin -------------------------------> 校验身份、建立或恢复文件事务
        <------------------ P_begin_ack   返回可靠保存的 next_offset
NWXCH1 Binary × 多片 -------------------> 按 offset 保存；数据阶段不回 ACK
P_commit ------------------------------> 校验完整文件，持久化文件和索引
        <----------------- P_commit_ack   成功后设备可清理该本地文件
```

`begin_ack` 确定本次上传从哪个字节开始；`commit_ack` 才确认整个文件已可靠保存。数据阶段不逐片确认，不使用 `window_bytes`。

### 2.2 会议：结束录制后上传

在线、离线都先保存到 SD，会议录制期间不上传本场文件。结束录制后，有连接则上传，没有连接则等待恢复。

以一场会议生成 5 个文件为例（4 个满文件和 1 个尾文件）：

```text
开始会议 → 依次保存文件 0、1、2、3、4 → 结束会议、封存尾文件
有连接后：
文件 0：meeting_audio_begin → begin_ack → Binary → meeting_audio_commit → commit_ack
文件 1～4：分别重复上述完整文件流程
meeting_audio_session_end（segment_count=5）→ meeting_audio_session_end_ack
```

无重试时，共发送 5 次 begin、5 次 commit、1 次 session_end；不是整场只发一次 begin。云端按场次归集文件，在 session_end 时核对清单并完成场次处理。

### 2.3 监控：封存一个文件，上传一个文件

录制和上传可以同时进行：文件 0 封存后安排上传，设备继续录制文件 1，无须等待监控结束。

1. 在线且满足实时条件的文件，使用 `monitoring_audio_realtime_begin`、`monitoring_audio_realtime_commit` 及对应 ACK，按第 2.1 节上传。
2. 离线时继续存 SD；恢复后优先上传新的实时文件，旧文件使用 `monitoring_audio_history_begin`、`monitoring_audio_history_commit` 及对应 ACK 补传。实时、历史文件属于同一场次，不重复创建场次。
3. 停止监控时封存尾文件；本场待上传文件处理完毕后，发送 **`monitoring_audio_session_end`**，云端回复 **`monitoring_audio_session_end_ack`**。场次结束消息不区分 realtime/history。

当前固件在线监控通常按 48 帧／960 ms、离线监控按 1,500 帧／30 秒封存；停止录制、采集属性切换等可提前封存。**4 MiB 是文件上限，当前并非统一等到满 4 MiB 才上传。** 上述时间为已保存音频的时长，静音暂停期间不累计。

### 2.4 中断与续传

中断后重新发送该文件的 begin，场次 ID、文件编号、长度和 CRC 不变；云端返回已可靠保存的 `next_offset`。为 0 时整文件重传，大于 0 时从该偏移续传，等于 `total_bytes` 时直接 commit。

监控每次重新 begin 前重新判定实时性：经历离线恢复或文件过期后，可由 realtime 改为 history；仍满足实时条件的重试可继续用 realtime。云端以 **`设备身份 + audio_session_id + segment_index`** 跨前缀去重、恢复同一文件，ACK 使用本次请求对应的前缀。重试不得重复生成音频记录或重复触发告警；内容一致的已提交文件再次 commit 也应返回成功。

## 3. JSON 消息

### 3.1 开始文件

设备发送：

```json
{
  "type": "meeting_audio_begin",
  "request_id": "offline_begin_00000001",
  "audio_session_id": "meeting_0123456789abcdef",
  "segment_index": 0,
  "total_bytes": 32096,
  "crc32": 3775820668
}
```

云端回复：

```json
{
  "type": "meeting_audio_begin_ack",
  "request_id": "offline_begin_00000001",
  "audio_session_id": "meeting_0123456789abcdef",
  "segment_index": 0,
  "ok": true,
  "next_offset": 0
}
```

| 字段 | 含义与约束 |
| --- | --- |
| `type` | `P_begin` / `P_begin_ack`；监控仅替换对应前缀 |
| `request_id` | 本次操作标识，1～39 个 ASCII 字母、数字、下划线或短横线；ACK 原样返回；重试可变 |
| `audio_session_id` | 场次标识，1～35 个上述字符；同场重连、重试不变 |
| `segment_index` | 场次内文件编号，0～999,999；从 0 递增，重试不变 |
| `total_bytes` | 整个归档长度，含文件头、记录长度、NA 帧、文件尾；128～4,194,304 字节 |
| `crc32` | uint32 十进制数；算法和计算范围见第 4 节 |
| `ok` | 本次操作是否成功 |
| `next_offset` | 云端可靠保存的连续前缀长度；成功时为 0～total_bytes，失败时填 0 |


### 3.2 提交文件

设备发完该文件后发送：

```json
{
  "type": "meeting_audio_commit",
  "request_id": "offline_commit_00000001",
  "audio_session_id": "meeting_0123456789abcdef",
  "segment_index": 0
}
```

云端校验并可靠保存后回复：

```json
{
  "type": "meeting_audio_commit_ack",
  "request_id": "offline_commit_00000001",
  "audio_session_id": "meeting_0123456789abcdef",
  "segment_index": 0,
  "ok": true
}
```

**云端校验文件长度、结构和 CRC，并持久化文件及索引后，才能回复成功 commit_ack；设备持久化该确认状态后才删除本地文件。**

### 3.3 结束场次

正常场次在录制结束且全部文件提交成功后发送；异常缺失按下方 `recording_complete=false` 规则处理。以下以会议为例，监控的 type 分别替换为 `monitoring_audio_session_end` 和 `monitoring_audio_session_end_ack`，其余字段相同。

设备发送：

```json
{
  "type": "meeting_audio_session_end",
  "request_id": "sd_end_00000001",
  "audio_session_id": "meeting_0123456789abcdef",
  "segment_count": 1,
  "recording_complete": true
}
```

云端回复：

```json
{
  "type": "meeting_audio_session_end_ack",
  "request_id": "sd_end_00000001",
  "audio_session_id": "meeting_0123456789abcdef",
  "ok": true
}
```

- `segment_count`：0～1,000,000，表示文件编号范围 `[0, segment_count)`，包含异常缺失的编号；0 表示无音频场次，正常确认。
- `recording_complete=true`：范围内文件必须全部提交、模式一致、Footer.complete=1、采集区间不重叠。
- `recording_complete=false`：保存有效文件、缺失编号及不完整状态后确认，不无限等待缺失文件；即使文件全部收到，也不能改判为完整录音。

### 3.4 回复与失败规则

文件 ACK 必须返回对应的 `type`，并原样返回该请求的 `request_id`、`audio_session_id`、`segment_index`；场次结束 ACK 不含 `segment_index`。字段类型及长度遵循第 3.1 节，`segment_index` 为整数。

失败仍用对应 ACK，`ok:false`；失败的 begin_ack 也必须携带 `next_offset:0`。重复请求按同一文件或场次幂等处理。

当前设备等待 begin_ack、commit_ack 各 **6 秒**，session_end_ack **60 秒**。文件上传超时、断线或被抢占时保留未确认数据，后续重新 begin；session_end 未确认则后续重发结束消息，已持久化确认的文件不因此重传。

## 4. 二进制数据结构

以下整数全部为**小端无符号整数**，PCM 样本除外；定长字符串为 ASCII、NUL 填充。

### 4.1 NWXCH1 传输分片

每条 Binary 消息为 `64 字节传输头 + N 字节归档内容`：

| 偏移 | 字节数 | 内容 |
| ---: | ---: | --- |
| 0 | 6 | ASCII `NWXCH1` |
| 6 | 2 | 保留，0 |
| 8 | 4 | version=2 |
| 12 | 4 | offset，本片在完整归档中的字节偏移 |
| 16 | 4 | N，1～8,064 |
| 20 | 40 | 本次 begin.request_id，最长 39 字节，NUL 填充 |
| 60 | 4 | flags：中间片 0，文件最后一片 1 |
| 64 | N | 原始归档字节 |

消息总长必须为 `64+N`；当前正式云端发送路径每片最多 4,096 字节内容。**云端按 N 和 offset 接收，不能写死分片大小。** 二进制 version=2 仍保留，不需要 JSON 版本协商。

同一次文件上传共用 begin.request_id，offset 从 next_offset 连续递增。flags=1 必须满足 `offset+N=total_bytes`，短片本身不代表结束；若 next_offset 已等于 total_bytes，则直接 commit，无 Binary。数据不重复携带场次和编号，由当前 begin 绑定。

传输片可以跨文件头、音频记录和文件尾，先按 offset 还原归档字节，不能按网络片边界提取 PCM。

### 4.2 归档结构与文件头

```text
96 字节 NWXOFF1 文件头
重复：4 字节 uint32 记录长度 + 完整 NA 帧
32 字节 NWXEND1 文件尾
```

当前音频固定为 **16 kHz、单声道、PCM16 小端、每帧 20 ms**。文件头：

| 偏移 | 字节数 | 字段和值 |
| ---: | ---: | --- |
| 0 | 8 | `NWXOFF1` + NUL |
| 8 | 2 | version=1 |
| 10 | 2 | header_bytes=96 |
| 12 | 1 | mode：0 会议，1 监控；须与 begin.type 对应 |
| 13 | 1 | channels=1 |
| 14 | 2 | codec=1，PCM16 |
| 16 | 4 | sample_rate=16000 |
| 20 | 2 | frame_duration_ms=20 |
| 22 | 2 | source_flags=2 |
| 24 | 4 | segment_index，与 begin 一致 |
| 28 | 8 | start_uptime_ms，首帧开机相对毫秒时间，非 UTC |
| 36 | 48 | session_id，与 begin.audio_session_id 一致 |
| 84 | 12 | 保留，0 |

### 4.3 NA 音频帧

记录长度为 662，不包含前面的 4 字节长度字段，每条记录共 666 字节。

| NA 内偏移 | 字节数 | 字段和值 |
| ---: | ---: | --- |
| 0 | 2 | ASCII `NA` |
| 2 | 1 | version=1 |
| 3 | 1 | codec=1 |
| 4 | 1 | flags：START=0x01、END=0x02、VAD_SPEECH=0x04；可组合，也可为 0 |
| 5 | 1 | channels=1 |
| 6 | 2 | sample_rate=16000 |
| 8 | 2 | frame_duration_ms=20 |
| 10 | 4 | sequence，场次内采集帧序号 |
| 14 | 4 | timestamp_ms，采集开机毫秒时间低 32 位 |
| 18 | 2 | payload_bytes=640 |
| 20 | 2 | 保留，0 |
| 22 | 640 | PCM：320 个有符号 int16 小端样本 |

不要求每个文件出现 START/END，文件结束以 Footer 和 commit 为准。

### 4.4 文件尾与校验

| Footer 内偏移 | 字节数 | 字段和值 |
| ---: | ---: | --- |
| 0 | 8 | `NWXEND1` + NUL |
| 8 | 2 | version=1 |
| 10 | 2 | footer_bytes=32 |
| 12 | 4 | frame_count，记录数 |
| 16 | 8 | payload_bytes，完整 NA 帧总字节数，含 NA 头 |
| 24 | 4 | crc32 |
| 28 | 1 | complete：1 正常封存，0 不完整；不是整场结束标记 |
| 29 | 3 | 保留，0 |

```text
payload_bytes = frame_count × 662
total_bytes = 96 + frame_count × 666 + 32
纯 PCM 字节数 = frame_count × 640
有效音频时长毫秒 = frame_count × 20
```

文件无须填满 4 MiB，也不补零；正常短文件 complete=1。complete=0 的文件在结构、CRC 正确时仍可保存并成功确认，场次标记为不完整。

CRC 为 **CRC-32/ISO-HDLC**，反射多项式 `0xEDB88320`，初始寄存器和末尾异或均为 `0xFFFFFFFF`，检查值 `123456789 → 0xCBF43926`。输入只包含顺序拼接的**完整 NA 帧（头+PCM）**，不包含记录长度、归档头尾或传输头。必须满足 `计算值 = Footer.crc32 = begin.crc32`。Python 可从 `crc=0` 开始逐帧执行 `crc=zlib.crc32(na_frame, crc) & 0xffffffff`。

