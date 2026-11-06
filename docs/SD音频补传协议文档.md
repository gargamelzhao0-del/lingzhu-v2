当前有卡会议和监控使用场次级 begin、文件级 commit 和整场结束消息，完整格式见 [SD 会议监控场次上传协议](SD会议监控场次上传协议.md)，运行逻辑见 [功能逻辑与异常处理](SD会议监控功能逻辑与异常处理.md)。本文保留旧 `offline_audio_*` 归档兼容入口及历史说明，不能直接作为新业务的接入规范。

修订日期：2026-10-13。SD 上传不发送专用 `hello`，不声明 SD 上传能力，也不要求云端回复能力对象。

沿用普通云端连接的认证、`hello` 和心跳流程。连接已认证且服务就绪后，存储空闲、有待上传文件时直接发送 `offline_audio_begin`。服务就绪可以由应用层 `pong` 或有效设备指令确认，不强制等待 `hello` 回复。下列上传 ACK、断点续传、CRC 校验和提交后删除规则保持不变。

开始
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


云端ack：

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
设备上传数据格式：
逐片发送二进制：NWXCH1 二进制分片

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

云端收到切片后回复ack：

例如收到 offset=0、payload_length=8064，持久化后回复：

```json
{
  "type": "offline_audio_chunk_ack",
  "mode": "archive_only",
  "request_id": "offline_chunk_001",
  "segment_id": "offline_7febb77e_00000001",
  "ok": true,
  "next_offset": 8064
}
```

此处 `request_id` 必须与二进制头一致。设备收到 ACK 后再发下一片，直到 `next_offset == total_bytes`。

补充约束：NWXCH1 头部的 `version`、`offset`、`payload_length` 和保留字段按
little-endian 解析；当前正式固件单片最多 4096 字节，云端应兼容 1～8064 字节，不能
假设固定分片大小或按 8064 字节递增偏移。ACK 必须在本片可靠写入后返回，设备
最多等待 2.5 秒；超时、断线或录音开始时，已确认的连续前缀必须保留，后续 begin
返回实际 `next_offset`，不能清零或丢弃 `.part`。

录音优先：录音开始或 SD writer 尚未排空时，设备可以结束当前补传连接而不发送
新的分片；这不是本段失败。录音任务和 SD 收尾完成后，设备会重新调度补传，使用
同一 `segment_id` 的 begin 获取已持久化的 `next_offset`，再继续 chunk/commit。

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

云端校验长度、归档头尾、NA 帧格式、帧数、payload_bytes 和 CRC，将文件原子转为已提交对象后回复：

```json
{
  "type": "offline_audio_commit_ack",
  "mode": "archive_only",
  "request_id": "offline_commit_23aa0910",
  "segment_id": "offline_7febb77e_00000001",
  "ok": true
}
```

设备只有收到匹配的成功 commit ACK 后才允许删除本地文件；失败时可能稍后重复 begin/commit。


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


CRC32

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

CRC 校验所需的 NWXOFF1 Header、记录长度、NA v1 帧和 NWXEND1 Footer 字段布局，按《sd数据上传协议-云端完整版》第 8 节实现；CRC 仅计算每条完整 NA v1 帧，不包含归档头、记录长度、NWXCH1 头和 Footer。

断网恢复同一会议/监控（2026-09-14 设备端）：

```json
{
  "type": "audio_mode_request",
  "request_id": "meeting_resume_new_request",
  "mode": "meeting",
  "state": "enabled",
  "reason": "offline_audio_recovered",
  "audio_session_id": "original_audio_session"
}
```

监控将 `mode` 换为 `monitoring`。每次恢复申请使用新 `request_id`；云端沿用
请求中的 `audio_session_id`，按原 ACK/开启命令流程恢复，不另建业务会话。
后续 `listen` 与该次模式的 SD 归档 `session_id` 均保持原值。此可选字段是恢复
申请的增补，SD 补传消息和二进制格式不变；首次开启流程不变。

云端按 `device_id + audio_session_id` 关联在线和补传音频，归档仍按
`device_id + segment_id` 幂等落盘。每次新 `listen` 的在线 sequence 从 0 开始，
SD 采集 sequence 在同一会话内延续，两者不能混用。归档帧按开机时间
`timestamp_ms` 排序；在线 NA 时间为流内相对时间，不能直接与归档时间比较。
完整录音优先由覆盖在线/离线的 SD 归档组织，在线用于实时处理；需要用在线
音频填补归档缺口时，云端还需建立各 listen 的时间映射并去除重叠。
相同会话 ID 只保证关联，不保证精确无缝拼接；不能按上传到达顺序拼接。
缺失区间保留缺口或补静音；监控无人声间隔不等于丢帧。
会话延续限同一次开机、未退出当前模式；退出后重新开启为新会话。
