# 协议目录

> 同步说明:本目录与 `docs/2026-07-02-设备端服务端通信协议规范.md` **v2.1** 严格一致。
> v2.1 已删除 §4.10 `pc_status_request` / `status_app_request`(设备端未实现)。

## 目录结构

```
protocol/
├── README.md                                       # 本文件
├── schemas/
│   └── nuwax-device-protocol.schema.json           # 完整协议 JSON Schema(Draft 2020-12)
└── fixtures/na/                                    # NA 二进制音频帧测试集
    ├── manifest.json                               # 7 个 fixture 索引
    ├── pcm16-mono-16k-20ms-start.bin               # 设备首帧(START | VAD_SPEECH)
    ├── pcm16-batch-3frames-16k-20ms.bin            # 连续 3 帧(seq 0/1/2)
    ├── opus-packet-16k-60ms.bin                    # Opus 编码首帧(START | END | VAD_SPEECH)
    ├── invalid-bad-magic.bin                       # 负向:错误 magic
    ├── invalid-truncated-payload.bin               # 负向:payload 被截断
    ├── invalid-pcm16-odd-payload.bin               # 负向:payload_len 奇数
    └── downlink-stereo-reject.bin                  # 负向:下行使用立体声(服务端不应发送)

tools/
├── protocol/
│   ├── validate-protocol-assets.js                 # 验证 Schema / fixture 是否合规
│   └── generate-na-fixtures.js                     # 重新生成 7 个 NA fixture
└── fake-device/
    └── fake-device-conformance.js                  # 假设备客户端(WS + JSON + NA)
```

## 运行工具

```bash
# 1. 验证所有 JSON / fixture 文件
node tools/protocol/validate-protocol-assets.js

# 2. 重新生成 NA fixture
node tools/protocol/generate-na-fixtures.js

# 3. 启动假设备客户端(默认连 ws://127.0.0.1:8799)
node tools/fake-device/fake-device-conformance.js

# 4. 启动假设备客户端(自定义服务端)
FAKE_WS_URL=ws://192.168.1.10:8799 FAKE_TOKEN=nuwax_demo node tools/fake-device/fake-device-conformance.js
```

## 协议消息类型(覆盖)

### 设备 → 服务端(9 类)

| type | 何时使用 |
|---|---|
| `hello` | WS 连上后第一条,声明设备能力 |
| `ping` | 应用层心跳(每 25 秒) |
| `listen` | 唤醒后开启新一轮录音 |
| `text_request` | 设备端直接发文字给智能体 |
| `cancel_turn` | 用户取消当前轮 |
| `input_event` | 触摸 / 按键 / 物理输入 |
| `wifi_scan_result` | 设备扫描附近 WiFi |
| `wifi_status` | WiFi 连接状态变化 |
| `device_command_ack` | 应答服务端的设备命令 |

### 服务端 → 设备(11 类)

| type | 何时使用 |
|---|---|
| `pong` | 设备端 ping 的应答 |
| `device_command` | 8 类命令:set_emotion / set_volume / stop_playback / set_state / sleep_conversation / show_status / wifi_scan / wifi_connect |
| `asr_partial` | 流式 ASR 增量识别(中间结果) |
| `asr_final` | 流式 ASR 最终识别 |
| `asr_result` | 一次性 ASR 结果 |
| `audio_session` | 服务端开/关 audio session |
| `text_response` | 流式文本应答 |
| `playback_begin` | 播放开始(NA 二进制帧前置) |
| `playback_end` | 播放结束 |
| `tts_result` | 一次性 TTS 结果 |
| `mcp` | MCP 协议消息(扩展通道) |

### NA 二进制音频帧(22 字节 header)

```
┌────────────────────────────────────────┐
│ magic 'N','A' (2B) │ version=1 (1B)    │
│ codec 1=PCM16/2=Opus (1B)              │
│ flags 0x01=START | 0x02=END | 0x04=VAD (1B) │
│ channels 1=mono (1B)                   │
│ sample_rate u16 LE (2B)                │
│ frame_duration_ms u16 LE (2B)          │
│ sequence u32 LE (4B)                   │
│ timestamp_ms u32 LE (4B)               │
│ payload_len u16 LE (2B)                │
│ reserved 0x0000 (2B)                   │
├────────────────────────────────────────┤
│ payload (PCM16 mono / Opus bytes)      │
└────────────────────────────────────────┘
```

## 与设备端 ESP32 代码的对应关系

| 交付包文件 | 对应设备端源文件 |
|---|---|
| `schemas/nuwax-device-protocol.schema.json` | `main/session_protocol.c / .h` |
| `fixtures/na/*.bin` | `main/audio_frame.c / .h` |
| `tools/fake-device/*.js` | `main/nuwax_ai_partner_main.c:4128 app_main` |
| `tools/protocol/validate-protocol-assets.js` | 单元测试(替代品) |

> 服务端工程师**不需要**阅读 ESP32 源码即可基于本目录实现服务端。
> 本目录是设备端对外行为的**唯一事实来源**(single source of truth)。
> 任何对协议字段、ID 模型、心跳间隔、NA header 的修改,**必须先**更新本目录,**再**更新 ESP32 代码。