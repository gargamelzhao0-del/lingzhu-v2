# SD 卡离线音频上传协议

旧版兼容文档：当前有卡场次接入请使用 [SD 会议监控场次上传协议](SD会议监控场次上传协议.md)，设备行为见 [功能逻辑与异常处理](SD会议监控功能逻辑与异常处理.md)。下文保留旧离线补传格式，不代表当前有卡录制和上传流程。

会议模式和监控模式在正式云端 WebSocket 断开期间写入 SD 卡的音频归档，待网络恢复后上传。

##  正式连接前提

设备先执行现有正式云端连接流程：

```text
WSS 握手（X-Bridge-Token 为 device_secret）
→ 标准 hello
← 应用层 pong 或有效设备指令确认服务就绪（也兼容成功 hello）
→ 开始离线归档上传
```

2026-10-13 起不再发送 SD 专用 `hello` 或 SD 能力声明，也不等待能力回复。普通连接认证及心跳保持不变，随后直接发送 `offline_audio_begin`。

## 上传时序

```text
设备发现 .ready 文件
→ offline_audio_begin
← offline_audio_begin_ack
→ NWXCH1 Binary Frame
← offline_audio_chunk_ack
→ 重复发送剩余分片并逐片等待 ACK
→ offline_audio_commit
← offline_audio_commit_ack
→ 删除对应 .ready 文件
```

`begin_ack` 返回的 `next_offset` 可能大于零，表示云端已经持久化了同一
`device_id + segment_id` 的连续前缀，设备必须从该 offset 继续。

## Begin

设备发送 Text Frame：

```json
{
  "type": "offline_audio_begin",
  "request_id": "offline_begin_7a91c2d0",
  "segment_id": "offline_7febb77e_00000001",
  "total_bytes": 1048412,
  "crc32": 305419896
}
```

字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `type` | string | 固定为 `offline_audio_begin` |
| `request_id` | string | 本次请求 ID；重连重试时可以改变 |
| `segment_id` | string | 归档分段稳定 ID；同一个本地文件重试时必须不变 |
| `total_bytes` | unsigned integer | 完整归档文件长度，包含头、记录和尾 |
| `crc32` | unsigned integer | 所有 NA 音频帧字节的 CRC32，详见第 8 节 |

云端成功响应：

```json
{
  "type": "offline_audio_begin_ack",
  "request_id": "offline_begin_7a91c2d0",
  "ok": true,
  "next_offset": 0
}
```

`next_offset` 是云端已经可靠保存的连续字节数，取值范围必须是
`0..total_bytes`。拒绝 begin 时也建议返回 `next_offset: 0`，便于设备统一处理。

## NWXCH1 二进制分片

每个 WebSocket Binary Frame 的应用层内容为：

```text
64 字节 NWXCH1 头
+ 最多 8064 字节原始归档内容
```

因此应用层分片最大长度为 `8128` 字节。`8064` 是协议的应用层上限，不是
TCP、TLS、SD 卡或 PCM 音频帧大小限制。WebSocket/TLS 内部可以继续把它拆成
多个底层写操作。

所有整数均为小端序：

| 偏移 | 长度 | 内容 |
| ---: | ---: | --- |
| 0 | 6 | ASCII `NWXCH1` |
| 6 | 2 | 保留字段；接收端必须忽略，发送端建议填零 |
| 8 | 4 | version，固定为 `1` |
| 12 | 4 | 当前分片在完整归档中的字节 offset |
| 16 | 4 | 本分片 payload 长度，`1..8064` |
| 20 | 40 | chunk `request_id`，ASCII，零填充 |
| 60 | 4 | 保留字段；接收端必须忽略，发送端填零 |
| 64 | N | 归档内容，N 等于 payload 长度 |


成功 ACK：

```json
{
  "type": "offline_audio_chunk_ack",
  "request_id": "offline_chunk_8b12f901",
  "ok": true,
  "next_offset": 8064
}
```

`next_offset` 必须是当前云端持久化后第一个尚未保存的字节位置。设备只有在
`offset < next_offset <= offset + payload_length` 时才推进到下一分片。

## 6. Commit

设备确认 `next_offset == total_bytes` 后发送：

```json
{
  "type": "offline_audio_commit",
  "request_id": "offline_commit_23aa0910",
  "segment_id": "offline_7febb77e_00000001",
  "total_bytes": 1048412,
  "crc32": 305419896
}
```

云端校验通过后返回：

```json
{
  "type": "offline_audio_commit_ack",
  "request_id": "offline_commit_23aa0910",
  "ok": true
}
```

云端必须在返回 `ok:true` 前完成完整性验证。重复收到同一分段的 commit 时，应对已经提交的对象返回幂等成功 ACK，前提是长度和 CRC 一致。

## SD 归档文件格式

`total_bytes` 表示整个归档文件的长度，不是纯 PCM 数据长度。归档格式为：

```text
96 字节 NWXOFF1 Header
→ 多个 [4 字节记录长度 + NA v1 PCM16 音频帧]
→ 32 字节 NWXEND1 Footer
```

### NWXOFF1 Header（96 字节）

| 偏移 | 长度 | 内容 |
| ---: | ---: | --- |
| 0 | 8 | ASCII `NWXOFF1` |
| 8 | 2 | version=`1` |
| 10 | 2 | header_bytes=`96` |
| 12 | 1 | mode：`0=meeting`，`1=monitoring` |
| 13 | 1 | channels，当前为 `1` |
| 14 | 2 | codec：`1=PCM16` |
| 16 | 4 | sample_rate，当前为 `16000` |
| 20 | 2 | frame_duration_ms，当前为 `20` |
| 22 | 2 | 保留 |
| 24 | 4 | segment_index |
| 28 | 8 | start_uptime_ms |
| 36 | 48 | session_id |
| 84 | 12 | 保留 |

### NA v1 PCM16 音频帧

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
| 14 | 4 | timestamp_ms |
| 18 | 2 | PCM payload bytes |
| 20 | 2 | 保留 |
| 22 | N | PCM16 little-endian payload |

当前标准帧为 `320` 个采样点、`640` 字节 PCM 数据，因此 NA 帧为
`22 + 640 = 662` 字节；归档记录还要加 4 字节记录长度，共 `666` 字节。

### NWXEND1 Footer（32 字节）

| 偏移 | 长度 | 内容 |
| ---: | ---: | --- |
| 0 | 8 | ASCII `NWXEND1` |
| 8 | 2 | version=`1` |
| 10 | 2 | footer_bytes=`32` |
| 12 | 4 | frame_count |
| 16 | 8 | payload_bytes，所有 NA 帧总长度 |
| 24 | 4 | crc32 |
| 28 | 1 | complete；`0` 表示断网/异常边界结束，仍可上传 |
| 29 | 3 | 保留 |

## CRC32 规则

CRC32 用于确认云端保存的内容与设备归档内容一致。

计算范围是每条 NA 音频帧的完整字节：

```text
NA 头 + PCM payload
```

不包含：

- `NWXOFF1` 归档头；
- 每条记录前的 4 字节记录长度；
- `NWXEND1` 归档尾。

设备在 begin 和 commit 中发送同一个 `crc32`。云端必须重新计算并同时核对：

```text
实际文件长度 == total_bytes
实际 frame_count == Footer.frame_count
实际 payload_bytes == Footer.payload_bytes
实际 CRC32 == Footer.crc32 == begin/commit.crc32
```

### 固定算法参数

双方必须使用同一种 CRC32 变体：

| 参数 | 值 |
| --- | --- |
| 算法名称 | `CRC-32/ISO-HDLC`（常用 IEEE CRC32） |
| 多项式 | `0x04C11DB7`；按反射实现使用 `0xEDB88320` |
| 初始寄存器值 | `0xFFFFFFFF` |
| 输入反射（RefIn） | `true` |
| 输出反射（RefOut） | `true` |
| 最终异或（XorOut） | `0xFFFFFFFF` |
| 结果范围 | 无符号 32 位整数 `0..4294967295` |

设备代码对外将累计变量初始化为 `0`，在更新函数内部先取反，因此与上表的
标准参数完全等价。云端不要使用 CRC32C、CRC-16 或其他 CRC32 变体。

### 设备等价伪代码

```c
uint32_t crc = 0;

for (每一条 NA 音频帧，按文件顺序) {
    crc = crc32_update(crc, na_frame_bytes, na_frame_length);
}

uint32_t crc32_update(uint32_t crc, byte[] data, size_t length) {
    uint32_t value = ~crc;

    for (size_t i = 0; i < length; ++i) {
        value ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if ((value & 1) != 0) {
                value = (value >> 1) ^ 0xEDB88320;
            } else {
                value >>= 1;
            }
        }
    }

    return ~value;
}
```

设备必须把上一条帧的结果作为下一条帧的输入，不能每一帧重新初始化 CRC。
云端可以直接使用标准库；例如 Java 使用 `java.util.zip.CRC32`，按顺序对
每个完整 NA 帧调用 `update()` 即可。

标准自测向量：

```text
输入 ASCII：123456789
结果：0xCBF43926（十进制 3421780262）
```

### 存储和传输表示

- JSON 中的 `crc32` 使用十进制无符号整数表示；
- `NWXEND1` Footer 中的 `crc32` 使用小端序存储；
- CRC 输入仍然是所有完整 NA 帧的原始字节，不包含 JSON、`NWXCH1` 头、归档头、记录长度或归档尾。

CRC32 不是加密或身份认证机制，传输安全仍由正式 WSS 和设备认证保证。

## 断点续传


示例：云端已经保存 `16128` 字节，设备重连后再次发送 begin，云端返回：

```json
{
  "type": "offline_audio_begin_ack",
  "request_id": "offline_begin_aabbccdd",
  "ok": true,
  "next_offset": 16128
}
```

设备从 offset `16128` 继续发送，不重传已经确认的前缀。

## 异常响应建议

负 ACK 至少包含当前请求的 `request_id` 和 `ok:false`。对于 begin/chunk，建议
同时返回合法的 `next_offset`：

```json
{
  "type": "offline_audio_chunk_ack",
  "request_id": "offline_chunk_8b12f901",
  "ok": false,
  "next_offset": 0
}
```

云端不应因单个分段失败关闭整个正式 WebSocket；可以拒绝当前上传事务，设备会
保留文件并在下一次连接重试。若事务对象损坏，云端可以删除自己的 `.part`，
下一次 begin 返回 `next_offset: 0`，但不能要求设备删除本地 `.ready`。

## 20 KiB 分片示例

假设完整归档文件为 `20 KiB = 20480` 字节，且 begin 返回
`next_offset=0`，设备会发送三个 Binary Frame：

| 分片 | offset | payload | 应用层长度（64 字节头 + payload） |
| ---: | ---: | ---: | ---: |
| 1 | 0 | 8064 | 8128 |
| 2 | 8064 | 8064 | 8128 |
| 3 | 16128 | 4352 | 4416 |

每个分片都必须等待对应 `offline_audio_chunk_ack` 后再发送下一个。最后一个
ACK 的 `next_offset` 必须为 `20480`，然后才能发送 commit。
