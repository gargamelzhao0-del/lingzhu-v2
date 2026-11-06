# SD 离线音频补传协议（云端交付版 v1）

旧版兼容文档：下文描述旧 offline_audio 归档路径，不能直接用于当前 SD 场次固件。当前云端接入以 [SD 会议监控场次上传协议](SD会议监控场次上传协议.md) 为准，设备流程见 [功能逻辑与异常处理](SD会议监控功能逻辑与异常处理.md)。

修订日期：2026-10-13。协议版本仍为 1；本次取消 SD 专用 hello 和能力协商，begin/chunk/commit 消息及归档文件格式不变。
本文用于云端接收端开发与联调；设备行为参见
[当前实施契约](protocol/meeting-monitoring-sd-current-20260910.md) 和
[功能逻辑与数据流](sd-offline-audio-logic-and-dataflow.md)。
本文的补传调度说明已按 2026-09-12 录音优先修复更新；早期材料中的
“录音期间持续并行补传”不再代表当前固件行为。

## 结论与范围

本协议是现有在线会议/监控协议的**增量扩展**。不修改现有
`audio_mode_request`、`device_command(set_audio_mode)`、`listen`、
`monitoring_capture_control`、实时 NA 音频帧或心跳协议。

本次按项目方确认的服务端已支持 SD 上传实施。设备不再等待云端通过
`hello` 声明 SD 能力，直接在已有认证连接上发送上传事务。接收端继续提供：

1. `NWXCH1` 分片与旧 NA 实时音频帧分流；
2. 可持久化、可断点续传的 `begin/chunk/commit` 事务；
3. 会议/监控、心跳和控制链路与 archive-only 接收链路隔离。

**交付结果是 SD 音频归档的保存、校验和索引，不包含离线会议补转写、监控事件
补识别或补告警。** 在线镜像也会补传，不得因此重复执行已有在线业务。
若后续需要离线业务处理，须另行定义业务会话关联、时间映射、去重与处理结果协议，
不能直接把本协议 payload 接入现有实时 ASR。

不再以缺失或关闭能力字段作为停传开关。普通认证失败仍按原连接流程处理；
上传负 ACK、超时或断网时保留本地文件，后续按已有机制重试。

### 当前设备的录音与补传行为

- 会议在线和断网期间均连续保存 PCM；监控在线和断网期间均按人声保存。
- 监控使用连续 60 ms 人声确认、500 ms 前置缓冲及最后人声后的 1000 ms
  句尾。本地录音独立于云端暂停/恢复 ACK，恢复被拒绝时仍保存人声。
- 会议、监控默认离线会话上限均为八小时，运行期归档配额最低 1.5 GiB。
  八小时连续音频约有 959,040,000 字节记录内容，另加分段头尾；成立条件是
  SD 健康可写，实际剩余空间和配额余量充足。历史未确认归档不会为腾空间而删除。
- 归档按采集顺序保存，监控语音段之间允许时间和序号间隔，不应据此判为损坏。
- 当前采用录音优先：归档活跃、离线采集活跃、SD 收尾中、writer 正在写入或
  队列未排空时不启动补传；已运行任务在扫描、校验、读片和提交删除处检查并让出。
  会议持续录音期间延迟补传，停止录音且 SD 收尾后恢复；监控须等当前人声归档
  关闭、存储空闲后才可补传。恢复联网不等于立即上传。
- 设备补传 worker 因上述条件返回 `ESP_ERR_NOT_FINISHED` 时，只结束当前传输
  实例并保留归档和断点，同时发布待重试状态；连接 owner 在录音及 SD writer
  完成后立即重新调度，不应把一次被让出的连接当作本轮补传完成。
- 正式连接补传还受实时发送队列门控。逐片等待 ACK 的协议不保证补传速度
  高于录音产生速度，也不保证分段按录音时间顺序抵达云端。
- 本阶段不开发用户可见的 SD 状态界面。

## 1. 连接与上传前提

普通云端连接的身份认证、`hello` 和心跳保持原样；SD 上传不再添加
`features.offline_audio_replay_v1`，不发送专用 `hello`，也不读取
`offline_audio.supported/protocol_version/archive_only` 作为启动条件。

认证连接上的应用层 `pong`、有效设备指令或成功 `hello` 均可确认服务就绪，
云端不必须回复 `hello`。存储空闲且有待上传文件时，设备直接发送
`offline_audio_begin`，之后仍必须逐步确认 begin/chunk/commit ACK。

断线重连使用新连接的 epoch，旧连接 ACK 无效；`segment_id` 保持不变，
由新的 begin ACK 返回可靠保存的连续偏移。不改变普通认证拒绝的处理。

独立 LAN 测试连接也直接发送 begin，通过 URL 的 `device_id` 参数区分设备。
此参数仅用于本地测试网关路由，不是正式认证方式，也不修改正式上传字段。

## 2. 传输与消息分流

补传使用设备已经认证的正式 WSS，不建立新的认证通道。云端必须按 WebSocket
应用帧类型和内容前缀分流：

| 入站帧 | 判断 | 处理链路 |
| --- | --- | --- |
| Text | `type=offline_audio_begin` | 离线归档事务开始 |
| Binary | 前 6 字节为 ASCII `NWXCH1` | 离线归档分片 |
| Text | `type=offline_audio_commit` | 离线归档提交 |
| Binary | 前 2 字节为 `NA`、version=1 | 原有实时会议/监控链路 |
| 其他 | 不符合以上格式 | 返回协议错误或丢弃，不能误送 ASR |

单个 `NWXCH1` 必须是一个独立 WebSocket Binary Frame，不得把多个分片拼成
一个应用消息。云端可以在底层 TCP/TLS 层拆分写入，但不能改变应用帧边界。

实时 NA 帧、心跳、控制命令和补传分片可以在同一连接交错到达。补传不得阻塞
心跳、在线会议/监控实时上传或模式控制。设备逐片等待 ACK；同一连接可因
超时或负 ACK 退避后再次尝试，云端不能把补传限制为单次尝试。当前连接中新
封存的 `.ready` 分段也可以在当前健康连接的后续调度周期补传，不保证必须
等到下一次重连。

云端必须使用认证上下文中的 `device_id`，不能从补传 JSON 中接受或覆盖设备
身份。一个设备当前补传连接同一时刻只绑定一个正在接收分片的归档事务，
但可以持久保存多个暂停的 RECEIVING 对象；多个设备之间可以并发。
不能把“一个活动事务”实现为未 commit 的旧对象永久占锁，详见第 4 节。

## 3. 标识、限制与通用规则

- JSON 字符串 ID 使用 ASCII `A-Z a-z 0-9 _ - . :`，不得静默截断。
- `request_id` 最大 39 字节，`segment_id` 最大 47 字节；重连重试时
  `request_id` 可以改变（40/48 字节设备缓冲区均包含结尾空字节）。
- `segment_id` 对同一个本地文件永久不变，云端必须把它当作不透明字符串。
- 云端幂等键为 `authenticated_device_id + segment_id`，不能使用 request ID。
- 传输层 `total_bytes` 接受范围为 `128..4,194,304` 字节，默认上限为
  `1,048,576` 字节；`total_bytes` 包含归档头、记录和尾部。当前设备生成的
  最小有效归档（1 个 20 ms PCM16 帧）约为 `794` 字节；只有 Header/Footer
  的零帧对象不是有效设备归档，Commit 时必须拒绝。
- 接收兼容范围：分片 payload 为 `1..8064` 字节，Binary Frame 最大 `8128` 字节。
  独立测试连接使用最多 `8064` 字节 payload；正式云端共享实时音频连接时使用
  `4096` 字节上限并在块间让出调度，云端不能假设固定分片大小或按 8064 对齐。
- `offset`、`payload_length`、`total_bytes`、`next_offset` 和 CRC 在语义上均为
  无符号整数，云端必须校验整数性及 uint32 范围。设备当前发送十进制整数；
  不要求云端把“科学计数法”作为独立语法错误，但解析后不得接受小数、NaN、
  Infinity、负数或超出范围的值。
- 所有 `NWXCH1` 和归档整数均为 little-endian。

ACK 必须使用与当前请求完全相同的 `request_id`，并以 Text JSON Frame 返回；
迟到、重复或 request ID 不匹配的 ACK 不得推进事务。建议统一使用以下
`error_code`：`unsupported`、`invalid_argument`、`segment_conflict`、
`offset_invalid`、`storage_unavailable`、`archive_invalid`、`crc_mismatch`、
`busy`。

设备对每个 begin/chunk/commit ACK 最多等待 2.5 秒；云端应在收到请求或分片
后 2.5 秒内返回对应 ACK。设备发送控制 JSON 的本地发送预算更短，云端不能
依赖延迟 ACK 来维持事务；若持久化耗时可能超过 2.5 秒，应先写入可靠的
快速临时存储，再返回 ACK。超时不代表设备允许删除本地文件。

当前固件的归档 JSON 和二进制发送采用 25 ms lifecycle 锁准入及 250 ms I/O 预算，
取得准入后释放全局锁，分片 ACK 后让出一个调度片。这是设备发送策略，不改变 2.5 秒
ACK 等待上限，也不是云端持久化必须在 250 ms 内完成的要求。
设备可能因录音开始、连接失效而提前结束 ACK 等待；2.5 秒是上限，不是保证
等待时长。云端已经持久化的数据和迟到 ACK 仍按原事务处理，不得回滚可靠前缀。

## 4. 分段事务状态

云端为每个 `device_id + segment_id` 维护持久状态：

```text
ABSENT → RECEIVING → COMMITTED
             └──────→ FAILED（保留可恢复 .part）
```

连接断开、进程重启或 ACK 丢失不得丢弃 `RECEIVING` 的连续前缀。下次
`offline_audio_begin` 必须返回实际可靠前缀。事务完成后可保留归档元数据，
但重复提交必须返回幂等成功（元数据一致时）。

### 同一连接中的暂停、恢复和事务切换

当前设备没有额外的 `pause`、`abort` 或 `cancel` 消息。录音开始时可能直接
停止后续分片，正式 WSS 保持在线，心跳和原有业务继续运行。云端必须允许：

1. 停止接收分片后保留 RECEIVING 的可靠前缀；可释放内存和连接内临时资源，
   不能因设备录音时间较长而清除已确认数据。
2. 同段重新 begin：校验长度/CRC，重新绑定该段，返回持久化 `next_offset`。
3. 新段 begin：先校验新请求，再串行完成此前已收到分片的处理，将旧段保留为
   可恢复对象，并把当前连接绑定切换到新段。无效 begin 不改变已有绑定。
   不能仅因旧段尚未 commit 而持续拒绝所有新段为 `busy`。
4. `NWXCH1` 不携带 segment_id，必须绑定到同一连接按消息顺序最近一次成功
   begin 的事务；不得异步重排 begin 与分片，也不得把旧异步写操作改绑到新段。
5. 云端允许短暂资源竞争返回 `busy`，但须能在后续 begin 重试中恢复服务。
   连接断开后释放连接绑定，持久对象仍保留。若同一设备有连接接管，应隔离旧
   连接的写入权限，避免两个连接同时修改一个对象。

例如 A 上传到 offset=193536 后录音开始，设备可能在同一 WSS 中先 begin B，
再 begin A。A 的前缀必须保留；重新 begin A 返回 193536，而不是被 B 占锁或
被当成新文件清零。设备也可能在 commit 已获成功 ACK 后、删除本地文件之前
让出；此时后续 begin/commit 必须幂等成功。

以上是正式接收端的必需兼容行为；独立测试网关的通过记录不替代同一正式 WSS
上的暂停、切换及连接接管验收。

## 5. Begin

设备发送 Text JSON：

```json
{
  "type": "offline_audio_begin",
  "mode": "archive_only",
  "request_id": "offline_begin_7a91c2d0",
  "segment_id": "offline_7febb77e_00000001",
  "total_bytes": 1048412,
  "crc32": 305419896
}
```

字段：

| 字段 | 类型 | 要求 |
| --- | --- | --- |
| `type` | string | 固定 `offline_audio_begin` |
| `mode` | string | 固定 `archive_only` |
| `request_id` | string | 本次请求唯一；可重试改变 |
| `segment_id` | string | 同一文件重试不变 |
| `total_bytes` | uint32 | 完整文件长度，当前不超过 4 MiB |
| `crc32` | uint32 | Footer 中的 CRC32 |

Begin 不携带 `session_id`；云端在收到完整归档并校验通过后，从
`NWXOFF1 Header` 的 `session_id` 和 `segment_index` 写入归档元数据。

成功响应：

```json
{
  "type": "offline_audio_begin_ack",
  "mode": "archive_only",
  "request_id": "offline_begin_7a91c2d0",
  "segment_id": "offline_7febb77e_00000001",
  "ok": true,
  "next_offset": 0
}
```

`next_offset` 必须是云端已可靠保存的连续前缀，范围为 `0..total_bytes`，
可以不按 8064 对齐。`next_offset == total_bytes` 时设备跳过分片并直接发送
commit。

同一 `device_id + segment_id` 已存在时：

- 长度和 CRC 一致：返回已有连续前缀；
- 已提交且元数据一致：返回 `next_offset=total_bytes`；
- 长度或 CRC 不一致：返回 `ok:false,error_code=segment_conflict`，不得覆盖
  原对象。

## 6. NWXCH1 二进制分片

每个 Binary Frame 的应用层格式：

```text
64 字节 NWXCH1 Header + N 字节归档内容（N=1..8064）
```

| 偏移 | 长度 | 内容 |
| ---: | ---: | --- |
| 0 | 6 | ASCII `NWXCH1` |
| 6 | 2 | 保留，发送填 0，接收忽略 |
| 8 | 4 | version=`1` |
| 12 | 4 | 完整归档中的 byte offset |
| 16 | 4 | payload 长度 `1..8064` |
| 20 | 40 | chunk request ID，ASCII，最多 39 字节，NUL 填充 |
| 60 | 4 | 保留，发送填 0，接收忽略 |
| 64 | N | 原始归档字节 |

云端处理顺序：

1. 校验魔数、版本、长度、offset 和当前事务；
2. 新数据必须从当前连续前缀开始；重复覆盖只有在字节完全一致时才幂等接受；
3. 使用 positional write 写入事务对象，不使用 `append`；
4. 完成持久化后再发送 ACK。

成功 ACK：

```json
{
  "type": "offline_audio_chunk_ack",
  "mode": "archive_only",
  "request_id": "offline_chunk_8b12f901",
  "segment_id": "offline_7febb77e_00000001",
  "ok": true,
  "next_offset": 8064
}
```

成功时必须满足：`offset < next_offset <= offset + payload_length`。负 ACK：

```json
{
  "type": "offline_audio_chunk_ack",
  "mode": "archive_only",
  "request_id": "offline_chunk_8b12f901",
  "segment_id": "offline_7febb77e_00000001",
  "ok": false,
  "next_offset": 0,
  "error_code": "offset_invalid"
}
```

`mode` 和 `segment_id` 在当前设备实现中可兼容省略，但正式云端必须返回，
以便日志、审计和后续固件严格绑定事务。

## 7. Commit

设备在最后一个 chunk ACK 的 `next_offset == total_bytes` 后发送：

```json
{
  "type": "offline_audio_commit",
  "mode": "archive_only",
  "request_id": "offline_commit_23aa0910",
  "segment_id": "offline_7febb77e_00000001",
  "total_bytes": 1048412,
  "crc32": 305419896
}
```

云端在返回成功前必须完成：

```text
文件长度 == total_bytes
NWXOFF1/NWXEND1 格式正确
每条记录长度和 NA v1 帧格式正确
Footer.frame_count 正确
Footer.payload_bytes 正确
实际 CRC32 == Footer.crc32 == begin.crc32 == commit.crc32
```

成功响应：

```json
{
  "type": "offline_audio_commit_ack",
  "mode": "archive_only",
  "request_id": "offline_commit_23aa0910",
  "segment_id": "offline_7febb77e_00000001",
  "ok": true
}
```

云端应先把 `.part` 原子提升为已提交对象，再发送 `ok:true`。设备只有收到
匹配的成功 ACK 后才删除本地 `.ready`。重复 commit 在长度和 CRC 一致时必须
返回幂等成功。若设备本地删除失败，云端对象不回滚；设备会保留文件并在后续
补传中重复执行幂等 begin/commit，云端必须继续返回成功。

## 8. 设备归档文件格式

SD 文件状态：

```text
*.part  正在写入或未完成，禁止上传
*.ready 已封存，可补传
```

完整文件：

```text
96 字节 NWXOFF1 Header
→ 多个 [4 字节 little-endian 长度 + NA v1 PCM16 帧]
→ 32 字节 NWXEND1 Footer
```

`total_bytes = 96 + Σ(4 + 每条 NA 帧长度) + 32`。

### NWXOFF1 Header（96 字节）

| 偏移 | 长度 | 内容 |
| ---: | ---: | --- |
| 0 | 8 | ASCII `NWXOFF1`（7 字节文本加 0） |
| 8 | 2 | version=`1` |
| 10 | 2 | header_bytes=`96` |
| 12 | 1 | mode：`0=meeting`，`1=monitoring` |
| 13 | 1 | channels=`1` |
| 14 | 2 | codec=`1`（PCM16） |
| 16 | 4 | sample_rate=`16000` |
| 20 | 2 | frame_duration_ms=`20` |
| 22 | 2 | source_flags：bit0 `0=断网采集`、`1=在线镜像`，其他位保留 |
| 24 | 4 | segment_index |
| 28 | 8 | start_uptime_ms，设备开机毫秒数，非 Unix 时间 |
| 36 | 48 | session_id，NUL 填充 |
| 84 | 12 | 保留 |

### NA v1 PCM16 Frame

| 偏移 | 长度 | 内容 |
| ---: | ---: | --- |
| 0 | 2 | ASCII `NA` |
| 2 | 1 | version=`1` |
| 3 | 1 | codec=`1`（PCM16） |
| 4 | 1 | flags |
| 5 | 1 | channels=`1` |
| 6 | 2 | sample_rate=`16000` |
| 8 | 2 | frame_duration_ms=`20` |
| 10 | 4 | sequence |
| 14 | 4 | timestamp_ms，设备开机毫秒数，非 Unix 时间 |
| 18 | 2 | PCM payload bytes，当前为 `640` |
| 20 | 2 | 保留 |
| 22 | N | little-endian PCM16，当前为 320 samples |

flags：`START=0x01`、`END=0x02`、`VAD_SPEECH=0x04`。

归档内帧不得使用 `START` 或 `END`。`VAD_SPEECH` 仅是设备本地判断结果，
云端必须保留但不得把它当作归档完整性条件。当前 v1 固定要求 PCM16、16 kHz、
单声道、20 ms；其他媒体参数须升级协议版本。

同一分段内 `sequence` 必须严格递增。不同分段或不同 `session_id` 不得用单一
全局序号去重；分段归属以 `device_id + segment_id` 为准。严格递增不等于每帧
必须加一：合法缺口应记录为质量信息，不得仅因跳号拒绝已通过格式/CRC 校验的
有效归档。会议连续采集的跳号通常表示缺帧；监控按人声保存时的时间/序号间隔
不能直接当作丢帧。首尾序号、跳号次数和模式应一并保留供分析。
`start_uptime_ms` 和 `timestamp_ms` 只可在同一次设备启动/会话内比较，跨重启
不得当作绝对时间；云端应保存原值，并使用 `session_id` 与 `segment_index`
组织跨分段顺序。
归档可能晚到或乱序到达，不能按上传时间拼接音频。2026-09-14 设备端在同次
开机、未退出当前模式时，让在线、离线及恢复后的归档 `session_id` 沿用原
`audio_session_id`；恢复申请使用新 `request_id` 并携带原 `audio_session_id`。
云端必须接受该恢复字段并沿用原会话，具体见
[会议/监控会话恢复说明](protocol/audio-mode-session-resume-20260914.md)。
旧固件仍可能生成独立离线 ID；首次在线音频尚未建立时也可由设备生成会话 ID。
字段不提供跨重启 UTC 映射。云端应按设备及音频会话关联、按归档帧时间排序。
在线 sequence 每次 listen 从 0 开始，SD 采集序号跨恢复延续，不能互相等同。
在线 NA 时间为流内相对时间，归档时间为开机时间，也不能直接比较。完整音频
优先由 SD 归档组织；在线数据填补归档缺口需额外时间映射或音频对齐并消除重叠。

### NWXEND1 Footer（32 字节）

| 偏移 | 长度 | 内容 |
| ---: | ---: | --- |
| 0 | 8 | ASCII `NWXEND1`（7 字节文本加 0） |
| 8 | 2 | version=`1` |
| 10 | 2 | footer_bytes=`32` |
| 12 | 4 | frame_count |
| 16 | 8 | payload_bytes，所有 NA 帧总长度 |
| 24 | 4 | crc32 |
| 28 | 1 | complete：`1=正常封存（含容量分段）`，`0=切换/中断/降级边界` |
| 29 | 3 | 保留 |

`complete` 描述设备录音分段的封存状态，不是上传状态，也不是整场会议结束标志。
`complete=0` 可出现在正常断网/恢复联网交接处；文件仍可长度正确、帧连续、CRC
正确并完整提交。云端必须保存原值，不能仅因此拒绝分段或认定 PCM 已损坏。

云端应分别记录事务状态（是否 COMMITTED）、格式/CRC 校验结果、设备 complete
及帧序号质量信息。`complete=1` 不替代连续性检查，CRC 正确也不证明采集期间
绝无缺帧。不同采集会话之间的切换空窗不能仅靠段内序号连续性排除。

## 9. CRC32

CRC 输入为每条完整 NA 帧的连续字节（NA 头加 PCM payload），不包含归档头、
记录长度、NWXCH1 头或 Footer。每条帧使用上一条帧的 CRC 累计值。

算法为 CRC-32/ISO-HDLC：多项式 `0x04C11DB7`（反射实现 `0xEDB88320`）、
初值 `0xFFFFFFFF`、RefIn/RefOut=true、XorOut=`0xFFFFFFFF`。标准向量：
`123456789 -> 0xCBF43926`。

等价伪代码：

```c
uint32_t crc = 0;
for (each complete NA frame in file order) {
    crc = crc32_update(crc, frame_bytes, frame_length);
}
```

JSON 的 `crc32` 使用十进制 uint32；Footer 使用 little-endian uint32。不得使用
CRC32C、CRC16 或其他变体。CRC 只用于完整性校验，不提供加密或身份认证。

## 10. 云端持久化要求

每个事务至少持久化以下元数据：

```text
device_id
segment_id
mode=archive_only
total_bytes
crc32
next_offset
state（RECEIVING/COMMITTED）
archive session_id、mode、segment_index（解析归档后写入）
frame_count、payload_bytes、complete
created_at、updated_at
```

要求：

- `.part` 写入必须支持按 offset 覆盖和断点恢复，不得依赖内存保存全部归档；
- ACK 前必须完成可靠持久化；
- 进程重启后根据连续前缀恢复 `next_offset`，不能用稀疏文件大小冒充连续长度；
- 元数据冲突不得覆盖已有对象；
- `commit` 成功后原子转为完成对象；
- 单个归档失败不得主动关闭健康的正式 WebSocket；
- 云端文件系统路径不得直接拼接未校验的 `segment_id`，应使用安全编码或对象键。

## 11. 异常与安全语义

| 情况 | 云端响应/处理 | 设备结果 |
| --- | --- | --- |
| 无 hello/能力回复 | 正常处理 begin/chunk/commit；不需要补发能力声明 | 满足原调度条件后直接上传 |
| begin 参数非法 | `offline_audio_begin_ack(ok:false,error_code=...)` | 本段稍后重试 |
| offset 不连续 | chunk 负 ACK，返回当前合法 `next_offset` | 保留本地文件 |
| ACK 超过 2.5 秒 | 不关闭 WebSocket；保持事务可恢复 | 当前批次停止，后续重试 |
| 网络断开 | 保留 `.part` 和事务前缀 | 重连后 begin 续传 |
| 录音开始，补传停止但 WSS 在线 | 保留前缀，接受同段恢复或新段 begin 切换 | 存储空闲后再调度 |
| CRC/格式错误 | commit 负 ACK；可删除云端损坏 `.part` | 本地 `.ready` 不删除 |
| 重复 begin/chunk/commit | 按 `device_id + segment_id` 幂等处理 | 不重复上传或丢段 |
| SD 拔出/异常结束 | 接受 `complete=0` 合法分段 | 仍可补传有效前缀 |

所有 ACK 必须是 JSON 对象，`type`、`request_id`、`ok` 必填；begin/chunk ACK
必须有 `0..total_bytes` 范围内的整数 `next_offset`（负 ACK 返回当前可靠前缀，
无法保留时返回 0）。正式云端应同时返回 `mode=archive_only`、
`segment_id` 和可机器处理的 `error_code`。

`archive_only` 数据只能进入归档存储、校验和索引，禁止进入 ASR、会议转写、
监控事件、告警、普通对话、TTS 或计费流程。正式传输继续使用设备认证和 WSS；
CRC 不替代传输认证或数据加密。

## 12. 与现有在线协议的边界

以下行为保持不变：

- `audio_mode_request` / `device_command set_audio_mode` 的开启、关闭和 ACK；
- `listen`、`monitoring_capture_control` / `monitoring_capture_control_ack`；
- 在线会议/监控的 NA v1 实时帧和心跳 ping/pong；
- 云端对实时帧的 ASR、会议转写、监控门控及现有状态机。

补传分片不得被当作 NA 实时帧，也不得改变在线会议/监控状态。补传 worker
失败时，实时上传和控制面仍应继续工作。

当前固件不实现 V2 的 `revision`、`meeting_id`、`audio_mode_state` 和
`listen_ack` 启动门禁；不得把这些后续设计作为本次补传接入的前置要求。

本地测试固件可使用独立 LAN WebSocket（例如 `offline-audio-replay-gateway.js`）
验证补传；该测试连接不是正式云端协议，不能替代正式 WSS，也不应写入生产配置。
正式云端联调必须使用启用 `CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE` 的
固件，同时关闭 `CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE`；启用 test replay
的固件走独立测试连接，不代表正式连接已开启本协议。正式固件连接原认证 WSS，
不能把 LAN 地址或测试 token 作为正式接入配置。

## 13. 云端验收标准

云端实现交付前必须通过：

1. 无 hello/能力回复时可直接上传，普通认证及心跳继续生效；
2. `NWXCH1`/NA 二进制分流；
3. 首次上传、任意字节 offset 断点续传；
4. ACK 丢失、连接断开、进程重启后的幂等恢复；
5. 重复 chunk 不产生重复字节；
6. 长度、帧格式、序号、Footer 和 CRC 全部校验；
7. `complete=0` 分段可成功归档；
8. 负 ACK、超时和存储故障不关闭健康 WebSocket；
9. archive-only 数据不进入任何在线业务处理链；
10. 多设备并发、4 MiB 上限、权限和路径安全测试。
11. 正式固件 4096 B 分片、兼容上限 8064 B、1024 B 短片及尾部非对齐分片均可
    完成事务；1024 B 是兼容测试尺寸，不是当前正式发送上限。
12. 补传过程中开始录音，补传让出且在线功能正常；停止录音及 SD 收尾后，在
    原健康连接恢复 begin/chunk/commit，无需人为断网。
13. 同一 WSS 上 A 部分上传后 begin B，再 begin A，A 的可靠前缀仍可恢复；
    迟到 ACK、异步落盘与连接接管不得写错对象或形成永久 busy。
14. 归档乱序抵达、在线镜像重复覆盖已处理音频时仍只归档，不重复转写/告警。

验收成功的最低结果是：设备端收到匹配的
`offline_audio_commit_ack(ok:true)` 后删除本地 `.ready`，云端完成对象可按
`device_id + session_id + segment_index` 查询，且重试不会产生重复或 CRC 错误。

### 验证记录与待验收项

截至 2026-09-12，设备侧会议、监控/离线音频主机检查和整机测试固件构建通过；
测试网关自测覆盖断点恢复、幂等及 CRC，另有活动连接报告更新、缺帧识别和
异常退出重启检查。入口为
[`offline-audio-replay-gateway.js`](../tools/protocol/offline-audio-replay-gateway.js)、
[`run-offline-audio-host-checks.ps1`](../tools/run-offline-audio-host-checks.ps1)
以及 [`offline-audio-gateway-report-test.js`](../tools/protocol/offline-audio-gateway-report-test.js)。

2026-09-12 设备日志与独立 LAN 网关实物归档核对结果：

| 本次采集阶段 | 设备帧数 | 网关帧数 | 结果 |
| --- | ---: | ---: | --- |
| 断网前在线会议 | 956 | 956 | 已提交；CRC 正确、段内序号连续 |
| 离线会议 | 1873 | 1574 + 299 | 两段均提交，分段连接处 1573→1574 连续 |
| 恢复联网后会议 | 1003 | 1003 | 已提交；CRC 正确、段内序号连续 |
| 合计 | 3832 | 3832 | 4 个文件全部收到 commit ACK 后删除 |

本次离线采集约 37.46 秒。旧段 `offline_46d59523_00000019` 从 `next_offset=193536`
恢复到总长 772022 字节，网关校验 1159 帧连续并提交成功。网关报告共 5 次提交
包含该旧段，不能当作本次新生成 5 个文件。

记录来源：设备截取日志中的会话 `meeting_offline_74911`，以及
`runtime-logs/sd-replay-supervised-20260912/gateway-1789189448669-1-report.json`
和原 archives 目录。交付联调包应另附相应日志和脱敏样例，以上项目内路径不要求
云端开发环境存在。测试网关是参考接收器，不代表生产鉴权、集群持久化或全部验收
条件已经实现。

这些记录不证明正式云端已接入，也不等于八小时实机写卡验收。跨采集模式仍观察到
约 320/510 ms 的麦克风交接间隔，不能把段内零丢帧表述为全程无缝录音。

交付联调仍需记录正式服务版本、固件版本、设备/SD 型号和结果，完成正式 WSS
事务验收，以及八小时持续录音、暂停期间人声保存、慢卡/满卡/热拔插、掉电恢复、
真正 Wi-Fi 断网恢复、模式切换空窗及录音抢占补传/空闲恢复测试。
本轮文档修订整理已有验证证据，没有执行刷机或新增实机测试。
