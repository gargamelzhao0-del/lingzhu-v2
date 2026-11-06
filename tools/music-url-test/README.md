# 音乐播放测试网关

网页入口：`http://127.0.0.1:8798/`。设备和电脑应处于可互通的局域网。
当前音乐测试固件使用 `http://192.168.31.10:8798`。电脑换网后需重新指定地址构建。
本服务使用 8798 端口，不占用 SD 音频网关的 8810 端口。

```powershell
node tools/music-url-test/server.js
```

页面上传 MP3 后，点击“发送播放”才会开始播放；点击“中止”停止当前音乐和待播放请求。
上传另一个文件本身不会换歌，点击“发送播放”才会中断旧歌并播放新歌。
设备名称为 MAC 地址，网页分别显示等待设备、缓冲、播放、停止、完成和失败。
设备未上线时按钮不可用。HTTP 202 仅表示指令已排队，页面随后显示设备实际返回的状态。

MP3 原文件不转码，设备按原采样率解码，当前单喇叭输出左右声道的平均值。
其他浏览器可解码的格式仍走旧的 PCM16 / 16 kHz / mono 转换方式。
页面显示的采样率来自设备；本地试听不等同于设备喇叭实测。

## 固件构建

在工程根目录运行：

```powershell
.\tools\build-music-test.ps1 -GatewayOrigin 'http://192.168.31.10:8798'
```

默认版本 7.0.182，输出 `C:/tmp/nuwax-music-20261027-r182/NuwaxAIPartner.bin`。
音乐功能与正式云端 `play_music` / `stop_music` 同时启用。SD 上传仍保留当前正式云端路由。
LAN 轮询仅在 `CONFIG_NUWAX_MUSIC_CONTROL_TEST_ENABLE=y` 时编译。
正式构建使用 `tools/build-sd-replay-formal.ps1`，保留云端音乐功能，关闭 LAN 控制。

旧的开机后一次性 PCM 测试模块 `main/music_url_test` 及其 Kconfig 开关保留。
新版按钮测试固件关闭该自动播放开关，避免启动后无操作自动响起。

## 网关接口

| 接口 | 用途 |
| --- | --- |
| `POST /api/music/mp3` | 上传 MP3 原文件，最大 32,000,000 字节，需要 Content-Length |
| `POST /api/music` | 原 PCM 上传接口，PCM16 / 16 kHz / mono，保留原格式和大小限制 |
| `GET /music.pcm` | 原固定 PCM 地址，继续兼容旧固件 |
| `GET /sources/{uuid}.mp3` 或 `.pcm` | 本次选择的不可变文件，防止播放中上传另一首造成内容拼接 |
| `GET /api/status` | 当前文件及设备状态 |
| `POST /api/play` | JSON `{"device_id":"94a990f1b4c4"}`，播放当前选择的文件 |
| `POST /api/stop` | 相同 JSON，停止当前音乐及待播请求 |
| `POST /api/device/poll` | 测试固件每约 500 ms 取一次指令并报告状态 |

设备轮询请求示例：

```json
{"device_id":"94a990f1b4c4","boot_id":"a12b34c5","ack":"music_xxx","accepted":true,"reason":"accepted","request_id":"music_xxx","state":"playing","playback_reason":"","sample_rate":44100}
```

网关返回 `{}`，或者：

```json
{"command":"play_music","request_id":"music_uuid","path":"/sources/uuid.mp3","format":"mp3","created":1793000000000}
```

中止返回 `command: "stop_music"`，无需 path 和 format。设备报告 ack 后网关移除指令；
重复取到同一请求不会重复播放。连续点击只保留最新请求；未取走的请求 30 秒失效；
设备重启更换 boot_id 后清空旧请求，不自动续播。云端原协议没有增加这些 LAN 字段。

音乐保存在 `tools/music-url-test/uploads/`，原 `music.pcm` 保留，
`sources/` 内为每次上传生成的独立文件，`selected.json` 记录当前选择。
不要在设备播放中删除源文件。测试页仅用于受信任的局域网，不应映射公网。

## 验证

```powershell
node --test tools/music-url-test/control.test.js
python tools/test-music-core.py
python tools/test-music-handoff.py
python tools/test-music-failures.py
python tools/audit-audio-teardown.py
python tools/test-speaker-pa-sequence.py
```

主机测试使用真实 Helix 解码器，覆盖 44.1/48 kHz、分片输入、ID3、截断文件、
切歌和取消竞态、恢复语音时钟失败时阻止麦克风进入，以及网关重启/重发边界。
主机测试不能验证实板的 I2S 时钟、音质、爆音和运行时堆余量。

实板依次检查：MP3 正常播放 → 网页中止 → 摸头中止 → 摇晃中止 → 连续切歌 →
会议/监控接管 → OTA 接管 → 低电量关机 → 恢复普通唤醒和对话。
播放期间唤醒词和普通 TTS 不应生效，停止音乐后应恢复唤醒，不自动恢复旧歌。
