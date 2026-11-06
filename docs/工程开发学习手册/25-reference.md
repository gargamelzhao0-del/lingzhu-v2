# 25 术语、辅助模块与源码索引

<!-- technical-edition-20261009 -->

> 技术索引要帮助读者从一个概念跳到真实实现，再回到解释它的场景。知道去哪查，也要知道该追到哪一层才算理解。

**学习目标：** 建立术语、生产入口、配置与验证之间的导航。

```zhizhan-map
概念与术语
源码入口
调用和配置
例题及验证
```

## 25.1 术语速查

| 术语 | 本工程中的含义 |
| --- | --- |
| PCM16 | 每样本 16 位整数的未压缩声音表示 |
| NA 帧 | 带版本、codec、序号和时间的应用音频帧 |
| VAD | 语音活动检测，不能识别人身份 |
| AEC / reference | 回声消除 / 用于预测回声的播放参考 |
| preroll | 识别确认前保留的环形音频，减少首字损失 |
| jitter buffer | 吸收网络到达抖动的播放缓冲 |
| owner / lease | 当前资源所有者 / 有限有效的使用权 |
| epoch / generation | 连接或对象代次，防止迟到事件污染新状态 |
| barrier / drain | 交接屏障 / 等待旧流或旧资源结束 |
| ACK / commit | 应答 / 具有明确持久化语义的事务提交 |
| idempotency | 相同操作重试不产生重复副作用 |
| CRC / SHA | 归档完整性校验 / 更强的摘要和产物身份；都不单独等于授权 |
| NVS / FATFS | 小型键值持久化 / SD 文件系统 |
| PSRAM / DMA | 外部 RAM / 硬件数据搬运机制 |
| GPIO active-low | 低电平对应有效状态 |
| SOC / FC / CHGINH | 估算电量百分比 / 满电标记 / 电量计充电禁止建议状态 |
| OTA / rollback | 远程应用更新 / 新镜像未通过确认时回到旧镜像 |
| PoP / verifier | 配网持有证明材料 / 安全会话验证材料 |
| P95 / high-water mark | 95 分位耗时 / 已观察到的队列或资源高水位，注意栈水位可能表示最小余量 |
| shadow 判定 | 只记录候选策略结果，不影响当前实际准入 |

## 25.2 主链路源码地图

| 关注内容 | 首读入口 | 接着看 |
| --- | --- | --- |
| 主控与启动 | [nuwax_ai_partner_main.c](../../main/nuwax_ai_partner_main.c) | [app_shell.c](../../main/app_shell.c) |
| 板级引脚 | [pins](../../main/board/esp_vocat_v1_0_pins.h) | [nuwax_board.c](../../main/board/nuwax_board.c) |
| 音频板与采集 | [vocat_audio_board.c](../../main/board/vocat_audio_board.c) | [mic_input.c](../../main/mic_input.c) |
| 帧格式 | [audio_frame.c](../../main/audio_frame.c) | [session_protocol.c](../../main/session_protocol.c) |
| 上传 | [audio_uplink.c](../../main/conversation/audio_uplink.c) | [ws_transport.c](../../main/ws_transport.c) |
| 播放 | [speaker_playback.c](../../main/conversation/speaker_playback.c) | [playback_terminal_guard.c](../../main/conversation/playback_terminal_guard.c) |
| 唤醒 | [wake_word_detector.c](../../main/wake_word_detector.c) | [wake_word_detection_policy.h](../../main/conversation/wake_word_detection_policy.h) |
| 会议/监控 | [meeting_mode.c](../../main/audio_modes/meeting/meeting_mode.c) | [monitoring_mode.c](../../main/audio_modes/monitoring/monitoring_mode.c) |
| 归档 | [offline_audio_spool.c](../../main/offline_audio_spool.c) | [offline_audio_voice.c](../../main/offline_audio_voice.c) |
| SD | [sd_card_driver.c](../../main/board/sd_card_driver.c) | [sd_dma_bulk.c](../../main/board/sd_dma_bulk.c) |
| 电池/LED | [battery_service.c](../../main/board/battery_service.c) | [led_status.c](../../main/board/led_status.c) |
| 摇晃/触摸 | [shake_detector.c](../../main/board/shake_detector.c) | [touch_gesture.c](../../main/conversation/touch_gesture.c) |
| 配网 | [nuwax_provision_main.c](../../main/nuwax_provision_new/nuwax_provision_main.c) | [nuwax_cloud_config.c](../../main/nuwax_provision_new/nuwax_cloud_config.c) |
| OTA | [nuwax_ota_device.c](../../main/nuwax_ota_new/nuwax_ota_device.c) | [nuwax_ota_transport.c](../../main/nuwax_ota_new/nuwax_ota_transport.c) |
| UI | [nuwax_state_bridge.c](../../main/nuwax_ui_v28/nuwax_state_bridge.c) | [nuwax_lvgl_port.c](../../main/nuwax_lvgl_port.c) |
| 构建 | [CMakeLists.txt](../../CMakeLists.txt) | [依赖锁](../../dependencies.lock) |

完整的本轮本地源文件索引见 [源码清单](evidence/source-inventory.json)，它是导航清单，不表示逐行审核过每一个第三方或测试文件。

## 25.3 容易忽视的辅助模块

**时间同步**：[time_sync.c](../../main/time_sync.c) 启动 SNTP 并判断系统时间是否基本有效。TLS 验证和日志日期依赖墙钟，而音频排序/期限应使用适合的单调时间。SNTP 更新不能使等待 deadline 突然跳跃；“时间值晚于固定基准”也不是精密时钟准确度证明。

**设备工具调用**：[mcp_server.c](../../main/mcp_server.c) 处理设备侧 JSON-RPC/MCP 消息，和开发者使用的 IDE 工具不是同一个服务。解析需限制资源、检查重复字段及尾部字符；工具暴露范围与配置能力一致，不能因 JSON 解析成功就允许任意硬件动作。

**通知**：[notification_playback.c](../../main/conversation/notification_playback.c) 与 RX barrier 分开处理通知生命周期和流归属。通知、普通回复和用户交互可能争用扬声器，需要明确优先级与取消，不是另开一个播放线程就能解决。

**USB 控制**：[nuwax_provision_usb_control.c](../../main/nuwax_provision_new/nuwax_provision_usb_control.c) 是诊断/配网控制入口之一。它应汇入同一个配网 owner，而不是建立第二套绑定状态。调试入口也要检查参数、权限与生命周期。

**错误与固件报告**：[device_error_report.c](../../main/device_error_report.c) 和 [nuwax_firmware_report.c](../../main/nuwax_ota_new/nuwax_firmware_report.c) 把故障与运行身份送到云端。上报队列、重试、NVS 写入时机与脱敏会影响稳定性；上报失败不能妨碍主要业务恢复。

**音乐等保留模块**：`main/music_playback`、`main/music_url_test` 等是可选实现，当前正式配置未启用。阅读时以编译开关和生产调用为边界，不把仓库中所有目录都列成产品已开放功能。

## 25.4 官方资料的正确用法

官方文档用来理解 SDK/API 的支持范围；本地源码决定本产品的实际选择。查版本时优先使用与项目匹配的 6.0 和 9.5，不直接用 latest 替代。

| 资料 | 学习用途 |
| --- | --- |
| [ESP-IDF 6.0 Kconfig](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/kconfig-reference.html) | 查配置语义和依赖关系 |
| [ESP-IDF 6.0 外部 RAM](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-guides/external-ram.html) | 理解能力、缓存和约束 |
| [ESP-IDF 6.0 OTA](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/system/ota.html) | 理解分区与回滚状态 |
| [LVGL 9.5 集成](https://lvgl.io/docs/open/9.5/integration/overview) | 理解移植、线程与刷新 |

上述资料已在编写时检查对应页面；本书关于工程行为主要来源于本地实现和已有测试记录，而非把通用文档中的所有能力当作本工程已启用能力。

## 25.5 继续学习的顺序

先用第 23 章的计算和帧解析理解数据，再运行一项生产策略测试，再追一个完整请求，最后做一次受控断网实验。能解释“这个 ACK 属于谁、这个指针活到什么时候、这个等待在哪里结束”，比记住所有配置常量更接近独立维护本项目的能力。



## 技术要点与自测

**设计要点：** 同名符号可能存在历史或测试版本。阅读时确认参与构建的是哪一份。

**自测：** 仓库中找到 servo 工具名称，就能断言设备有舵机吗？

**参考答案：** 不能。要继续检查硬件、配置、能力声明和实际绑定的回调。
