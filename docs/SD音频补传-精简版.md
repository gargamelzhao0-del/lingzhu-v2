# SD 音频补传（云端接入精简版）

旧版兼容文档：当前有卡场次接入以 [SD 会议监控场次上传协议](SD会议监控场次上传协议.md) 为准，设备逻辑见 [功能逻辑与异常处理](SD会议监控功能逻辑与异常处理.md)。下文的旧归档入口及确认流程不用于当前新场次对接。

修订日期：2026-10-13，协议 v1。现有在线会议/监控协议不变，本次取消 SD 上传能力协商，归档消息和文件格式不变。
完整字段、文件格式和验收要求见 [云端完整版](sd数据上传协议-云端完整版.md)。下列长度及 CRC 为结构示例，实际发送必须使用文件的真实值。

上传前提：

设备沿用正常认证连接，不发送 SD 专用 `hello`，不声明 SD 能力，也不等待能力回复。普通连接 `hello` 保留；应用层 `pong` 或有效设备指令同样可确认服务就绪。存储空闲且有待上传文件时，直接开始以下事务。失败或未收到成功提交 ACK 时保留本地文件。

设备开始上传一个归档文件：

```json
{
  "type": "offline_audio_begin",
  "mode": "archive_only",
  "request_id": "offline_begin_001",
  "segment_id": "offline_7febb77e_00000001",
  "total_bytes": 1048412,
  "crc32": 305419896
}
```

云端创建或恢复事务，返回已可靠保存的连续字节数：

```json
{
  "type": "offline_audio_begin_ack",
  "mode": "archive_only",
  "request_id": "offline_begin_001",
  "segment_id": "offline_7febb77e_00000001",
  "ok": true,
  "next_offset": 0
}
```

首次为 0；断点恢复返回真实偏移；已提交且长度/CRC 一致时返回 `total_bytes`，设备直接进入 commit。同一文件的 `segment_id` 不变，重试的 `request_id` 可以改变。

设备逐片发送二进制：

```text
64 字节 NWXCH1 头 + 本片原始归档字节

偏移 0：  6 字节 ASCII "NWXCH1"
偏移 6：  2 字节保留，发送为 0
偏移 8：  uint32 version = 1
偏移 12： uint32 offset，文件内字节偏移
偏移 16： uint32 payload_length
偏移 20： 40 字节 request_id，ASCII，最多 39 字节，NUL 填充
偏移 60： 4 字节保留，发送为 0
偏移 64： payload_length 字节归档内容
```

整数均为 little-endian。正式固件每片最多 4096 字节内容，独立测试固件最多 8064；云端兼容 1～8064 字节，不能要求固定大小或对齐。每片为独立 WebSocket Binary Frame。

例如收到 offset=0、payload_length=4096，持久化后回复：

```json
{
  "type": "offline_audio_chunk_ack",
  "mode": "archive_only",
  "request_id": "offline_chunk_001",
  "segment_id": "offline_7febb77e_00000001",
  "ok": true,
  "next_offset": 4096
}
```

此处 `request_id` 必须与二进制头一致。设备收到 ACK 后再发下一片，直到 `next_offset == total_bytes`。云端按 offset 写入，不可直接追加；重复片只有字节一致时才幂等接受。

设备提交完整文件：

```json
{
  "type": "offline_audio_commit",
  "mode": "archive_only",
  "request_id": "offline_commit_001",
  "segment_id": "offline_7febb77e_00000001",
  "total_bytes": 1048412,
  "crc32": 305419896
}
```

云端校验长度、归档头尾、NA 帧格式、帧数、payload_bytes 和 CRC，将文件原子转为已提交对象后回复：

```json
{
  "type": "offline_audio_commit_ack",
  "mode": "archive_only",
  "request_id": "offline_commit_001",
  "segment_id": "offline_7febb77e_00000001",
  "ok": true
}
```

设备只有收到匹配的成功 commit ACK 后才允许删除本地文件；录音抢占或删除失败时可能稍后重复 begin/commit，云端必须幂等成功。

失败时使用对应请求的 ACK 类型，例如分片偏移错误：

```json
{
  "type": "offline_audio_chunk_ack",
  "mode": "archive_only",
  "request_id": "offline_chunk_001",
  "segment_id": "offline_7febb77e_00000001",
  "ok": false,
  "next_offset": 0,
  "error_code": "offset_invalid"
}
```

begin/chunk 负 ACK 的 `next_offset` 返回当前可靠前缀，无法保留时为 0；commit ACK 不要求 offset。常见错误为 `segment_conflict`、`storage_unavailable`、`archive_invalid`、`crc_mismatch`、`busy`。失败不得关闭健康的在线业务连接。

云端必须遵守的规则：

- 正式补传复用原认证 WSS。Text 按 `type` 分流；Binary 的 `NWXCH1` 进入归档，原 `NA` 实时帧继续走原业务。归档内容中的 NA 不得再次进入实时 ASR。
- 使用认证上下文的 `device_id`，以 `device_id + segment_id` 幂等；相同 ID 的长度/CRC 冲突必须拒绝，不能覆盖。`request_id` 最多 39 字节，`segment_id` 最多 47 字节。
- ACK 必须在可靠持久化之后返回，设备最多等待 2.5 秒，也可能提前让出。ACK 丢失、重连或服务重启不能丢掉已确认前缀。
- 录音及 SD 收尾优先，空闲后自动补传。设备可能在 WSS 不断开时停止发片，没有额外 pause/abort 消息。新 begin 可切换当前绑定并保留旧段断点，不能被未完成旧段永久锁为 busy。
- NWXCH1 不带 segment_id，必须绑定当前连接最近一次成功 begin；按消息顺序处理，不能让异步写入串段。云端允许保存多个暂停对象，但同一连接只接收一个当前事务。
- 归档结构为 `96 字节 NWXOFF1 + 多条 [4 字节长度 + NA 帧] + 32 字节 NWXEND1`。当前媒体为 PCM16、16 kHz、单声道、20 ms。接收上限 4 MiB，当前默认分段上限 1 MiB；零帧文件不能提交。
- CRC 使用 CRC-32/ISO-HDLC，累计所有完整 NA 帧；不包含归档头尾、记录长度或 NWXCH1 头。算法向量 `123456789 → 0xCBF43926`。
- Footer 的 `complete=0` 可表示断网/恢复等边界，仍可合法完整上传；它不是上传失败标志。帧序号缺口应按会议/监控模式记录质量信息，不能与 CRC 错误混为一谈。
- 文件可能乱序抵达。从归档头提取 mode、session_id、segment_index 建索引；设备时间是开机毫秒，离线 session_id 不保证等于在线会议 ID。

最终效果：会议连续保存、监控按人声保存的 SD 归档，在存储空闲时可靠上传；云端只保存、校验和索引，设备确认提交后删除。原在线会议/监控、心跳和控制流程保持兼容。

正式联调需启用 `CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE` 并关闭 `CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE`。LAN 测试通过不替代正式云端联调；正式上线前至少验证无 hello 回复时直接上传、首次上传、断点恢复、重复提交、录音抢占后恢复以及归档不触发在线业务。
