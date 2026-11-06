# 会议/监控 SD 归档补传协议 v2

历史协议记录，不作为当前有卡场次接入契约。当前以 [SD 会议监控场次上传协议](../SD会议监控场次上传协议.md) 和 [功能逻辑与异常处理](../SD会议监控功能逻辑与异常处理.md) 为准；下文的在线镜像、版本和分段流程不可直接套用到当前固件。

## 目的

设备在线时实时上传并异步保存 SD；断网时只写 SD；恢复网络后，历史归档在后台补传。补传数据只进入归档存储，不得再次触发 ASR、会议转写、监控事件或告警。

协议复用设备已认证的 WebSocket，不建立新的认证通道。实时音频和补传任务使用独立队列，实时业务优先。

## 1. 开始分段

设备发送：

```json
{
  "type": "offline_audio_begin",
  "mode": "archive_only",
  "request_id": "offline_begin_xxx",
  "segment_id": "opaque-stable-id",
  "total_bytes": 1048576,
  "crc32": 123456789
}
```

`segment_id` 对同一 SD 分段永久不变。云端使用 `device_id + segment_id` 做幂等键，不能使用 `request_id` 做幂等键。

云端回复：

```json
{
  "type": "offline_audio_begin_ack",
  "mode": "archive_only",
  "request_id": "offline_begin_xxx",
  "ok": true,
  "next_offset": 0
}
```

`next_offset` 是云端已经可靠保存的下一个字节位置。已存在的临时文件必须返回其实际长度。

## 2. 数据分块

设备发送二进制 `NWXCH1` 分块，每块最多 8064 字节，并携带 offset、长度和 request_id。

云端必须先持久化分块，再回复：

```json
{
  "type": "offline_audio_chunk_ack",
  "mode": "archive_only",
  "request_id": "offline_chunk_xxx",
  "ok": true,
  "next_offset": 8064
}
```

offset 不连续、超出总长度或重复内容不匹配时，返回 `ok:false`，设备保留 SD 文件。

## 3. 提交分段

设备发送：

```json
{
  "type": "offline_audio_commit",
  "mode": "archive_only",
  "request_id": "offline_commit_xxx",
  "segment_id": "opaque-stable-id",
  "total_bytes": 1048576,
  "crc32": 123456789
}
```

云端验证完整长度、CRC 和归档文件格式后回复：

```json
{
  "type": "offline_audio_commit_ack",
  "mode": "archive_only",
  "request_id": "offline_commit_xxx",
  "ok": true
}
```

设备只有收到匹配的成功 ACK 后删除本地 `.ready` 文件。超时、断线、CRC 错误、负 ACK 或本地删除失败都保留文件，等待下一次补传。

## 4. 时间与处理要求

归档文件中的每个 PCM 帧包含 sequence 和采集时间戳。云端按采集时间和 sequence 重建顺序，不按网络到达顺序排序。

云端收到 `archive_only` 后只执行：

- 幂等存储；
- 完整性校验；
- 可选转码或归档索引。

不得执行 ASR、会议转写、监控事件检测、告警和重复计费。

## 5. 并发规则

- 实时音频始终优先于历史补传。
- 历史补传不得暂停当前会议或监控采集。
- 补传期间新数据继续实时上传并写入新的 SD 分段。
- 云端按 `device_id + session_id + sequence` 对音频帧幂等去重。

