# NuwaxAIPartner-v2 迁移审计与实机验证交接

日期：2026-07-29  
工程：`NuwaxAIPartner-v2`  
目标硬件：妙攀 V2.0 / ESP32-S3-WROOM-1-N32R8  
目标实机：新版硬件，历史端口 `COM5`  
禁止操作：`COM8` 属于旧版正式设备，不能用于 V2 验证

## 1. 当前结论

截至本文生成时，`NuwaxAIPartner-v2` 的源码迁移、主机契约回归和
ESP-IDF v6.0 产品构建已经完成。工程不是测试固件的整体复制，而是：

1. 以 `NuwaxAIPartner` 保留正式应用层；
2. 从 `NuwaxAIPartner测试工程` 迁入妙攀 V2.0 板级实现；
3. 在 V2 树中处理主入口、配置、音频链、协议和 OTA 的集成差异。

本轮已经确认：

- 正常对话、会议模式、监控模式、BLE/USB 配网、动态云配置、正式 UI、
  MultiNet 唤醒、软件播放参考、Live AEC、双讲保护和 OTA 源码均已注册
  到 V2 产品构建；
- V2 屏幕、触摸、I2C、ES7210、ES8311、PA、电源、电量、IMU、LED、SD、
  UART 等板级代码已经进入同一正式工程；
- 四组 host checks 全部通过；
- 含空格的 Windows 工程路径下，原始 `esp_audio_codec 1.0.1` managed
  component 能够完成最终链接；
- 固件版本为 `7.0.8`，OTA release sequence 为 `7000008`；
- 最新 App 镜像大小 3,454,176 bytes，A/B 槽仍有 31% 空间。

但不能据此宣布 V2 正式工程已经全部实机通过。当前仍需 COM5 验收的关键项是：

- V2 正式树的开机保持、SW3 点按关机、屏幕、触摸、电量和联网；
- 正常对话、会议、监控和 BLE/USB 配网的 V2 端到端运行；
- 软件参考 AEC 的真人双讲声学效果；
- V2 `7.0.8` 基线之后的 A/B OTA、健康门和回滚；
- BQ27220 profile 与实际 1200 mAh 电芯的一致性。

MIC3/slot2 当前历史实测近似静默，硬件播放参考不可用。V2 必须继续
fail closed，不能因为编译配置包含 slot2 采集能力就宣称硬件 AEC 可用。

本轮没有访问 COM5/COM8，没有串口烧录，也没有发送 OTA 命令。

> **2026-07-30 状态修订**：上述内容是 7.0.8 基线审计记录。随后已通过测试网关
> 将同一新版设备升级到 `7.0.11 / release_sequence 7000011`，并在不使用
> COM5/COM8 的情况下完成网络侧实机回归。最新证据见第 17 节；第 17 节只覆盖
> 测试网关协议、文件完整性和自动化可观测项，不替代屏幕/按键的人工目视确认，
> 也不替代真人双讲声学验收。

## 2. 证据等级

本文使用四种不同等级，不能混为一谈：

| 等级 | 含义 | 本文中的用法 |
| --- | --- | --- |
| 源码证据 | 文件存在、调用和配置路径成立 | 证明功能已迁入和被构建注册 |
| host/build 证据 | Windows 主机检查和 ESP-IDF 编译、链接、镜像检查通过 | 证明契约和可构建性，不证明外设工作 |
| 测试工程历史实机证据 | `NuwaxAIPartner测试工程` 在同一新版样机上的历史结果 | 可支持硬件路线，但不自动等于 V2 正式树通过 |
| V2 正式树实机证据 | V2 镜像在 COM5 上形成日志、UI、协议和音频闭环 | 尚待按第 14 节完成 |

任何后续结论都必须注明属于哪一等级。

## 3. 工程边界和禁止事项

| 目录 | 所有权和用途 |
| --- | --- |
| `NuwaxAIPartner/` | V1.2 正式应用基线。会议、监控、配网、UI、云协议和对话逻辑来源 |
| `NuwaxAIPartner测试工程/` | 妙攀 V2.0 硬件验证来源。板级驱动、电源、电量、OTA 和商家源码参考所在目录 |
| `NuwaxAIPartner-v2/` | 妙攀 V2.0 新正式工程，后续 V2 产品开发唯一目标树 |
| `NuwaxAIPartner_test_build/` | 外部构建产物目录，已由根 `.gitignore` 精确忽略 |

必须遵守：

- 不得用测试工程整体覆盖 `NuwaxAIPartner-v2`；
- 不得把旧正式工程的 V1.2 引脚、INMP441/MAX98357A 原型接线复制回 V2；
- 不得直接编辑 managed component 来维持本机链接；修复必须在工程控制范围内；
- 不得把 Wi-Fi 密码、网关 token、设备密钥或上传凭据写入 defaults、源码、
  文档或日志；
- 不得因测试工程实机通过就跳过 V2 正式树的 COM5 验收；
- 未经用户明确确认，不得烧录 COM5 或下发 OTA；
- 任何 V2 写入动作前必须再次核对端口身份，绝不能触碰 COM8。

## 4. 三树源码归属审计

### 4.1 方法

审计使用三个 `Resolve-Path` 后的绝对根目录，通过根目录长度计算严格相对路径，
再对行为相关源码逐文件计算 SHA-256。范围包含顶层 CMake/分区/defaults、
bootloader component、`main` 顶层、board、conversation、audio_modes、OTA、
provision 和 UI 源码；排除文档、生成目录、managed components、LVGL 库、
二进制资源和构建产物。

这次方法修正了旧 PowerShell 不支持 `Path.GetRelativePath` 导致的无效对比。
此前所有文件被错误归为 `RightOnly` 的结果不得引用。

### 4.2 结果

| 分类 | 文件数 | 含义 |
| --- | ---: | --- |
| 三树相同 | 166 | 两个来源本来就一致的公共逻辑 |
| 与正式工程相同 | 55 | V2 保留正式应用实现 |
| 与测试工程相同 | 45 | V2 采用新版硬件/OTA 实现 |
| V2 集成修改 | 27 | 两个来源之间必须在 V2 内合并或修正的文件 |
| V2 行为源码总数 | 293 | 本次限定审计范围 |

关键归属如下：

| 功能 | V2 当前来源 |
| --- | --- |
| 会议模式主体 | 正式工程 |
| 监控模式主体 | 正式工程 |
| 正常对话 session、preroll、触摸手势 | 正式工程或两树公共逻辑 |
| BLE/USB 配网、动态云配置 | 正式工程 |
| bootloader GPIO7 早期电源保持 | 测试工程 |
| V2 I2C、Codec、电量、IMU、LED、电源、触摸和 UART 驱动 | 测试工程 |
| OTA 状态机、下载、设备控制器 | 测试工程 |
| Live AEC 实现 | 测试工程版本 |
| 产品 CMake、主入口、mic/display、协议、音频上行 | V2 集成修改 |
| OTA release policy | V2 更新为 `7.0.8 / 7000008` |

## 5. 产品功能矩阵

| 功能 | 源码/构建 | 历史新版样机证据 | V2 正式树状态 |
| --- | --- | --- | --- |
| 正常对话 | 已迁入，conversation checks 通过 | 测试工程有联网和控制通道历史证据 | 待 COM5 端到端验收 |
| 会议模式 | 已迁入，processor/protocol/mode checks 通过 | 正式旧树有人工作流和录音证据 | 待 V2 COM5 验收 |
| 监控模式 | 已迁入，protocol/mode/capture checks 通过 | 正式旧树有长时测试记录 | 待 V2 COM5 验收 |
| BLE 配网 | Sec2、单连接 peripheral、GATT server 已编译 | 测试树曾确认广播/服务启动，端到端不完整 | 待手机或模拟器全流程验收 |
| USB 配网控制 | 已迁入并注册 | 无 V2 正式树证据 | 待验收 |
| 动态云绑定 | 已迁入，hello 使用运行时配置 | 正式旧树已与云端通信 | 待 V2 敏感配置/绑定验收 |
| MultiNet 唤醒 | `ni hao ling zhu` / `你好灵珠` 已进入产品配置 | 无 V2 正式树唤醒率数据 | 待安静/噪声/远近场验收 |
| LVGL 正式 UI | V28、状态栏、菜单、QR、电量均已构建 | 新板屏幕、触摸和 LED 有历史观察证据 | 待 V2 镜像复核 |
| GPIO7 电源保持 | bootloader 与应用实现已迁入 | 测试树基本实机通过 | 待 V2 冷启动、重启、OTA 后验证 |
| SW3 点按关机 | GPIO16 release-triggered 逻辑已迁入 | 测试树基本通过 | 待 V2 点按循环和 USB 场景验证 |
| BQ27220 电量 UI | 服务、驱动、状态桥均已迁入 | 可显示，但历史 profile 3000/1200 mAh 不符，`trusted=0` | 待 profile/可信度确认 |
| Wi-Fi/测试网关 | 代码已构建 | 测试树曾连接 | 当前审计镜像未注入 Wi-Fi/token，不能直接联网 |
| A/B OTA | 代码、分区、host release policy 通过 | 测试树 b46 一轮完整通过 | V2 `7.0.8` 尚未实机 OTA |
| ES8311/ES7210/I2S | V2 引脚和驱动已构建 | Codec、全双工 smoke、MIC1/2 历史通过 | 待 V2 正式树复核 |
| MIC3/slot2 | 读取和健康门代码存在 | 当前近似静默 | 不可用，必须 fail closed |
| 软件参考 Live AEC | 已迁入并构建 | 控制面/静态检查存在 | 真人双讲尚未验收 |
| 硬参考 AEC | 可编译读取 slot2，运行时受健康门控制 | 健康门失败 | 不可宣布可用 |
| MicroSD | 驱动存在 | 按用户要求暂缓 | 非当前阻塞项 |
| 底座 UART | 驱动存在 | GPIO43/44 与 console 使用需协调 | 非当前阻塞项 |
| 4G | 引脚和探针存在 | 无完整业务链 | 非当前阻塞项 |
| QMI8658A/绿色 LED/GPIO8 触摸 | 底层驱动存在 | 部分底层历史通过 | 产品动作尚未定义或未验收 |

## 6. V2 权威引脚

源码权威：`main/board/esp_vocat_v1_0_pins.h`。文件名为兼容旧接口而保留，
文件内容明确是妙攀 V2.0，不能按文件名推断为旧板。

### 6.1 I2C 和设备地址

| 信号/器件 | 值 |
| --- | --- |
| I2C SCL | GPIO1 |
| I2C SDA | GPIO2 |
| ES8311 | 7-bit `0x18` |
| ES7210 | 7-bit `0x40` |
| QMI8658A | `0x6A`，备选 `0x6B` |
| BQ27220 | `0x55` |
| CST816-family | `0x15`，备选 `0x2A` / `0x2D` |

### 6.2 音频

| 信号 | GPIO/语义 |
| --- | --- |
| MCLK | GPIO42 |
| BCLK | GPIO40 |
| WS | GPIO39 |
| ES8311 DOUT | GPIO41 |
| ES7210 DIN/TDMOUT | GPIO38 |
| NS4150B PA_CTRL | GPIO4 |
| Codec 电源 | `CODEC_3V3`，不是 GPIO 电源开关 |

TDM slot：

- slot0：MIC1，主麦和 ASR/AFE M 通道；
- slot1：MIC2，辅助声学麦克风，不是电参考；
- slot2：MIC3 候选播放参考；
- slot3：MIC4 未连接，NC。

### 6.3 显示、触摸、电源和其他

| 功能 | GPIO |
| --- | --- |
| LCD D0/D1/D2/D3 | 13 / 9 / 12 / 46 |
| LCD SCL/CS/RST/BL/TE | 14 / 10 / 21 / 3 / 17 |
| TP_INT | 11 |
| 独立 TOUCH/wake net | 8 |
| PG2 电源保持 | 7，active-high |
| SW3/PG1 | 16，active-low |
| 绿色 LED | 18 |
| SD CLK/CMD/D0 | 48 / 45 / 47 |
| 底座 UART TX/RX | 43 / 44 |
| 4G RX/TX/EN | 5 / 6 / 15 |
| ESP_BOOT | 0 |

GPIO0、3、45、46 涉及启动绑带或复位时外部电平，改硬件或初始化顺序前必须
再次按 ESP32-S3 数据手册与实板上拉/下拉复核。

## 7. 电源和电量

### 7.1 电源保持与 SW3

- bootloader component 在应用启动前处理 GPIO7/PG2，避免应用尚未初始化时
  松开 SW3 导致掉电；
- 应用层再次配置 GPIO7 保持；
- GPIO16 以 20 ms 轮询、60 ms 去抖；
- 关机动作在一次稳定按下并释放后执行，先关闭 PA/背光，再释放 PG2；
- 关键操作可以临时禁止关机；
- 若 USB 仍给 MCU 供电，释放 PG2 后使用关机标记和深睡路径保持视觉关机状态。

这套逻辑的目标是“关机时点按开机，开机时点按关机”，不是要求用户在正常运行中
持续按住 SW3。测试工程已有基础证据，但 V2 必须重新验证冷启动、软件重启、
OTA 重启、USB 供电和多轮点按。

### 7.2 BQ27220

- 默认设计容量为 1200 mAh；
- UI 读取同一 battery service 快照，包含百分比、电压、电流和电源状态；
- 采样失败使用退避并可重建驱动；
- SOC 只有在样本有效、未陈旧、容量匹配且 profile 状态满足时才标为 trusted；
- OTA 对低电、陈旧样本和未验证 profile 使用保守门槛。

历史样机曾上报 3000 mAh，而软件产品值是 1200 mAh，因此电量“能显示”不等于
“百分比可信”。必须确认 BQ27220 是否写入与实际电芯一致的 CEDV/profile。

## 8. 音频契约和三种模式

### 8.1 云端固定契约

`conversation/audio_uplink.c` 使用编译期断言固定：

- PCM16；
- 16,000 Hz；
- 单声道；
- 20 ms；
- 每帧 320 samples，640 bytes PCM payload。

START/END/VAD 信息位于音频帧头。即使没有普通音频帧，也可以发送零载荷 terminal
marker，保证会话有确定 END。产品、会议和监控共享这套上行契约。

Live AEC 内部 chunk size 从 ESP-SR AEC 接口运行时取得，不要求等于云端 320
samples。输出 FIFO 把内部 AEC chunk 重组为调用方需要的 20 ms 帧。当前实现因此
允许 ESP-SR 常见的 256-sample/16 ms 内部块，同时保持云协议不变。

指标命名遵守：比例类常用 `x1000/permille`，dB 类使用 `x100`。后续脚本不得把
二者混用。

### 8.2 正常对话

保留正式工程的唤醒 session、自动续听、preroll、云端 ASR/TTS、播放、音频上行、
本地早候选和云端确认路径。产品配置启用软件播放参考和 Live AEC，并包含 near-end
blend、human rescue、动态延迟和双麦人声保护。源码和 host checks 已通过，但
“双讲时消除回声并完整保留用户声音”仍需真人 WAV、协议序号和连续多轮证据。

### 8.3 会议模式

会议模式当前实现不是强降噪器，而是完整连续上传加轻处理：

- 每帧统计能量/峰值特征，并对上传样本做慢速 DC 去除；
- 自适应噪声底和宽松能量/峰值规则给出 speech 与 environment_quiet；
- 弱人声只做缓慢、有限增益，初始目标约 `+0.54 dB`，硬上限约 `+1.58 dB`；
- 增益门由 speech 状态（含 8 帧 hangover）控制；持续环境声不会因自身存在而获得增益；
- 所有有效帧仍连续上传，非人声帧不会被丢弃；
- speech 只增加 `VAD_SPEECH` flag；
- 只有连续 30 分钟既无人声又无非安静环境声才触发 `quiet_timeout`；
- 任意人声或非安静声音都会把 inactivity 清零；
- 进入模式前请求停止/淡出播放，不进行语音播报。

因此“简单环境过滤”的准确含义是 DC 去除、噪声底判别和只对人声做轻增益，
不是频谱降噪，也不是删除环境声。完整会议录音仍由测试/正式云端按完整上行保存。

### 8.4 监控模式

- 只接受云端 `set_audio_mode / monitoring / enabled|disabled`；
- 设备不提供 UI 手动开启语义；
- 开启前停止正常播放；
- 直接从 `nuwax_mic_input_read_pcm16` 读取并连续上传；
- 不调用 meeting processor，不做人声判别、增益或过滤；
- 不自动按安静时长退出；
- 直到云端 disabled、WebSocket 断开或硬错误才停止；
- 不进行语音播报；
- 最后一帧和 terminal marker 仍保证 END 语义。

会议和监控是互斥的独占音频模式，OTA 运行期间新的连续模式命令会以
`ota_busy` 拒绝。

## 9. 唤醒和配网

### 9.1 唤醒

产品选择 MultiNet，不选择 WakeNet：

```text
command: ni hao ling zhu
display: 你好灵珠
language: cn
threshold: 30%
```

`wn9_nihaoxiaozhi_tts` 仍可能作为配置/模型资产名称存在，但当前产品后端是
MultiNet，不能据此判断仍在使用 WakeNet。唤醒率没有可靠实机统计，必须在 V2
正式镜像上做安静、播放、噪声、不同距离和不同说话人测试后再给百分比。

### 9.2 配网和动态云

- BLE 为 one-connection peripheral/broadcaster；
- 使用 GATT server 和 Security 2；
- USB control 已注册；
- 云地址、设备 ID 和设备 secret 通过运行时动态配置进入 hello；
- Upgrade token 与逐设备 secret 是两个不同凭据。

当前构建使用的本地 sdkconfig 中 Wi-Fi SSID/password 和 voice token 为空，
因此本轮镜像只用于源码/链接审计，不是可直接连接测试网关的烧录包。实机前必须
通过本地忽略配置或配网流程注入，不能修改公开 defaults。

## 10. 硬件 AEC、软件 AEC 和商家代码边界

### 10.1 商家实现可以参考什么

商家 `cyberkit-main` 的 V2 board 设置 `AUDIO_INPUT_REFERENCE=true`，
`BoxAudioCodec` 把输入设为双通道，`audio_service` 分离 mic/reference 后送入
ESP-SR `AfeAudioProcessor`。AFE 使用 VC/VOIP high-performance AEC，并可动态
enable/disable。这证明商家设计意图也是“麦克风 + 参考通道送软件 AFE”，不是
由 ES7210/ES8311 芯片自动完成不可见的硬件回声消除。

可参考的是双通道格式、AFE 配置、feed/fetch 生命周期和实时 listening mode；
不能从商家代码推导本样机 MIC3 一定有电信号，也不能整体移植商家应用框架覆盖
正式云协议、会议/监控和 UI。

### 10.2 V2 当前实现

- 编译配置允许读取 MIC1、MIC2 和 slot2；
- `s_hw_ref_health_passed` 默认 false；
- 只有 `CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE` 和运行时健康门同时成立，
  `hw_ref_active` 才可为 true；
- 产品配置不会为了正常开机自动播放侵入式测试音，故未通过健康门时仍保持关闭；
- 软件播放参考由 speaker path 写入环形历史，捕获端按延迟读取；
- Live AEC 接收 mic/reference，内部 AFE 输出经 FIFO 返回；
- near-end blend、human rescue 和双麦提示用于避免把真人声音当作残余回声抹掉；
- 内存或 AEC 初始化失败时有原始单声道 fallback，不能死锁音频链。

### 10.3 当前硬边界

历史实测 MIC3/slot2 近似静默，因此硬参考 AEC 当前不可用。继续追查时需要硬件
测量 ES8311 OUTP/N、ADC_MIC3_P/N 的差分幅度/共模，再判断 RC 网络、焊接、
ES7210 配置或 TDM slot。参数调优不能修复无信号链路。

短期产品验收应使用软件播放参考。只有真人双讲至少连续 20 轮同时满足回声下降、
首尾音节保留、END/START 正确、序号连续和资源稳定，才能宣布软件 AEC 完成。

## 11. OTA 设计、速度和发布约束

### 11.1 状态机

```text
IDLE -> DOWNLOADING -> WRITING -> VERIFYING -> DONE
  |          |             |          |
  +----------+-------------+----------+-> FAILED
```

下载/写入阶段支持协作式 cancel；VERIFYING 是短提交阶段，不接受 cancel。失败会
abort 当前 OTA handle，不能选择坏分区。

### 11.2 manifest 必填项

```json
{
  "update_id": "<全新且唯一>",
  "url": "https://<不可变直链>/NuwaxAIPartner.bin",
  "version": "<高于 7.0.8>",
  "project_name": "NuwaxAIPartner",
  "target_board": "miaopan_v2_0_n32r8",
  "target_chip": "esp32s3",
  "partition_schema": 1,
  "build_flavor": "product",
  "release_sequence": 7000009,
  "size_bytes": 0,
  "image_sha256": "<64 位小写 hex>"
}
```

实际发布时 `size_bytes` 必须填构建镜像的精确非零长度。`size`、`sha256` 旧别名
不接受。`force` 也不能绕过 release sequence 严格递增。

### 11.3 校验和健康门

设备检查 target board/chip/schema/flavor/project、版本、release sequence、HTTPS
证书与 hostname、无重定向、HTTP 200、Content-Length、完整 byte count、SHA-256、
staged app project/version 和 ESP image 格式。通过后才切换 boot partition。

新镜像首次启动的本地健康门包含：speaker manager/output、mic、display、touch
controller/event producer、LVGL、app shell、运行任务和 battery service/新鲜电源样本。
全部通过后标记 valid；失败则保持 pending 或触发 rollback。会议/监控活动和 OTA
互相排斥，关机键在关键 OTA 阶段也被禁止。

产品配置为 35 分钟总超时、4 KiB internal I/O buffer，内部内存不足时回退到
2 KiB；HTTPS/transport 可从 0 开始重试，总计最多 3 次。TLS 证书和 SHA 校验不能
为速度而关闭。

### 11.4 历史速度证据边界

测试工程 b46 `7.0.6` 使用稳定 Nuwax S3 不可变直链完成一轮：

- 镜像 3,435,680 bytes；
- dispatch 到 staged 32.761 秒；
- dispatch 到 committed 51.550 秒；
- A/B 切换、重连、健康门和 committed 完成。

这证明同类代码和硬件在稳定源下可以低于三分钟，但不等于 V2 `7.0.8` 已通过。
临时 Quick Tunnel 的慢速/停顿也不能简单归因于本地 Wi-Fi。

### 11.5 OTA 覆盖范围

当前 OTA 只写 inactive App slot。它不更新：

- bootloader；
- partition table；
- `srmodels.bin` / model partition；
- NVS；
- storage。

因此首次 V2 基线、分区变化、bootloader 电源保持变化和唤醒模型变化必须走完整
基线发布流程，不能只发 App OTA。

## 12. 分区和镜像

### 12.1 当前分区

| 名称 | Offset | Size | 用途 |
| --- | ---: | ---: | --- |
| nvs | `0x9000` | 24 KiB | 配置和 OTA 持久化 |
| otadata | `0xf000` | 8 KiB | A/B 状态 |
| phy_init | `0x11000` | 4 KiB | PHY |
| model | `0x20000` | 4 MiB | ESP-SR models |
| ota_0 | `0x420000` | 4864 KiB | App slot A |
| ota_1 | `0x8e0000` | 4864 KiB | App slot B |
| storage | `0xda0000` | 2432 KiB | SPIFFS/storage |

### 12.2 最新审计构建

构建目录：

`NuwaxAIPartner_test_build/b-v2-product-ota-708-repro-s3`

实际配置源：

```text
SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.product-ota.defaults
SDKCONFIG=C:\tmp\nuwax7-v2-repro\sdkconfig
target=esp32s3
ESP-IDF=v6.0
```

该 sdkconfig 的 Wi-Fi SSID/password 和 voice token 为空，不能直接作为联网验收包。

| 文件 | Bytes | SHA-256 |
| --- | ---: | --- |
| `NuwaxAIPartner.bin` | 3,454,176 | `127ea5153dc1cbaa54c57a6d3ef94e66a74b6fa46867e4e38f46e5cbbf06d6b8` |
| `bootloader/bootloader.bin` | 21,392 | `b0775444f0b7b9e2b3f0d8e2ba2d184a1f056335dab577397c6bc663f271e511` |
| `partition_table/partition-table.bin` | 3,072 | `aad81c7e6e4686758d711bf7336ff59e4c3e816af34cf0bb795300b5e3bf8cdf` |
| `ota_data_initial.bin` | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `srmodels/srmodels.bin` | 2,681,351 | `dec3196aaadcf4e6305434462f5735e1148e5ddfe1f6a0696776d666f9066d34` |

App image-info：

- project：`NuwaxAIPartner`；
- version：`7.0.8`；
- chip：ESP32-S3；
- flash：16 MB / DIO / 80 MHz；
- validation hash：`9339921b2fae3c5882ac845bc3525c7f3ef2411ba96454c11b53ac128ad80e89`，valid；
- ELF SHA-256：`f5d6db12444220fbd51e0e92a0b6495039045226a6e43ed313d4aed3c99d4d93`；
- App 大小：`0x34b4e0`；
- 最小 App 槽：`0x4c0000`；
- 余量：`0x174b20`，31%。

## 13. 构建和回归结果

### 13.1 四组 host checks

2026-07-29 最新源码重复执行全部通过：

```powershell
.\tools\run-v2-product-host-checks.ps1
.\tools\run-conversation-host-checks.ps1
.\tools\run-meeting-host-checks.ps1
.\tools\run-monitoring-host-checks.ps1
```

覆盖：

- 0、1、3、4、5、8 帧的 END/terminal marker；
- OTA sequence floor、pending identity 和 anti-rollback；
- awake session 超时/刷新/回绕；
- preroll 窗口、缺口处理和 barge-in onset；
- meeting processor/protocol/mode；
- monitoring protocol/mode/短读拼帧。

Host checks 不覆盖 FreeRTOS 实时调度、真实 I2C/I2S、慢网、声学效果和实机重启。

### 13.2 ESP-IDF fresh build

2026-07-29 使用 ESP-IDF v6.0、外部 sdkconfig 和全新短构建路径完成：

```text
C:\tmp\nuwax-v2-com5-baseline-708-20260729
```

配置顺序为 `sdkconfig.defaults;sdkconfig.product-ota.defaults`。第一次高并发 Ninja
运行在 1549/2211 处出现一次没有编译器诊断的 Windows 进程启动失败；将并发限制为
4 后，同一 `lv_libwebp.c` 对象和剩余 650 个目标全部通过。随后再次执行无源码变化
构建也通过。这是构建主机资源抖动，不是 LVGL 或产品源码错误。

最终链接没有裸 `-lesp_audio_codec`，直接链接完整 archive；V2 与测试工程的
`esp_audio_codec/CMakeLists.txt` SHA-256 一致。镜像信息为：

| 文件 | 大小（bytes） | SHA-256 |
| --- | ---: | --- |
| `NuwaxAIPartner.bin` | 3,454,176 | `fd7900efb60a0932d479d9d635152f4711ca272ff9c73e7c38f99583bdcf97b4` |
| `bootloader/bootloader.bin` | 21,392 | `298a81c294319f47cd84a15193057e36aa5965385c15dcdf8782b6bc09996bc5` |
| `partition_table/partition-table.bin` | 3,072 | `aad81c7e6e4686758d711bf7336ff59e4c3e816af34cf0bb795300b5e3bf8cdf` |
| `ota_data_initial.bin` | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `srmodels/srmodels.bin` | 2,681,351 | `dec3196aaadcf4e6305434462f5735e1148e5ddfe1f6a0696776d666f9066d34` |

App image-info 复核为 `NuwaxAIPartner / 7.0.8 / ESP32-S3 / 16 MB / DIO /
80 MHz`，镜像校验和与 validation hash 均有效。App 为 `0x34b4e0`，小于
`0x4c0000` 的 A/B 槽，剩余 `0x174b20`（31%）。

### 13.3 COM5 持久化配置边界

这不是把凭据固化进 App 的工厂恢复镜像，而是给当前已配网、已绑定测试网关的
COM5 做升级基线：

- 运行时 Wi-Fi 优先读取 ESP-IDF 保存在 NVS 的 STA 配置；
- 运行时 cloud URL、硬件派生 device ID 和 device secret 只读取 `nuwax_cloud`
  NVS 动态记录；
- `CONFIG_NUWAX_VOICE_WS_URL/TOKEN` 已不是正式运行时回退入口；
- 本构建的编译期 Wi-Fi SSID/password/token 为空，敏感字面量扫描命中数为 0；
- 烧录清单不写 `0x9000-0xefff` 的 NVS，因此当前 COM5 配网和动态绑定会保留；
- 禁止执行 `erase-flash`、`erase_flash` 或单独覆盖 NVS。若 NVS 已丢失，必须走
  BLE/USB 配网与动态绑定，不能声称此 App 单独即可连接测试网关。

历史 Kconfig 符号仍名为 `NUWAX_BOARD_ESP_VOCAT_V1_0`，但其可见名称和实现均为
Miaopan 2.0。生成配置已复核音频 GPIO40/39/38/41、PA GPIO4、电源保持 GPIO7、
SW3 GPIO16、屏幕和触摸引脚均为 V2 配置。

### 13.4 COM5 首次 V2 正式树启动证据

2026-07-29 用户手动完成五镜像烧录并启动设备。本轮没有再次烧录或发送 OTA。
完整启动日志：

```text
runtime-logs/nuwax7-v2-708-user-poweron-20260729-211608.log
```

已落实的实机证据：

- bootloader 从 `ota_0 @ 0x420000` 启动，App 为 `7.0.8`、ESP-IDF v6.0；
- `POWERON` 启动后 PG2/GPIO7 拉高，SW3/GPIO16 单击关机逻辑已 armed；
- 运行时打印的板型为 Miaopan 2.0 / N32R8，V2 音频、屏幕、触摸和电源引脚与
  第 13.3 节一致；
- ST77916 QSPI 360x360 初始化成功并打开背光；
- I2C 扫描确认 touch `0x15`、ES8311 `0x18`、ES7210 `0x40`、BQ27220 `0x55`
  全部 ACK；CST816-family 触摸驱动初始化成功；
- ES7210 四槽 16 kHz TDM、ES8311、共享 I2S0 全双工和软件参考环初始化成功；
- MultiNet7 加载 `mn7_cn`，自定义命令 `ni hao ling zhu` / “你好灵珠”初始化成功；
- 保留的 NVS 生效：Wi-Fi 使用已配网的 `xspaceagi`，设备取得
  `192.168.31.53`；动态 cloud config `active=1`；
- 测试网关 hello/认证成功，设备 ID `NX-FCBB-6906-0438`，OTA floor
  `7000008`；心跳 ping/pong 和 application keepalive 持续增长；
- hello 明确上报 meeting v1、monitoring v1、OTA v1、remote BLE reprovision、
  MultiNet、shared-I2S/full-duplex 和 `aec_reference=software`。不能把
  slot2/MIC3 candidate 日志误写成硬件 AEC 已可用；
- 启动和联网周期没有 panic、abort、boot loop 或驱动初始化失败。

本轮发现的边界：

1. 电脑 WLAN 当时已断开，设备访问保存的 `192.168.31.180:8799` 失败；电脑重新
   接入同一 `xspaceagi` 后设备自动恢复。这不是密钥或固件问题，网关认证拒绝为 0。
2. BQ27220 报告 design capacity `3000 mAh`，软件期望 `1200 mAh`，因此
   `trusted=0`、`profile_verified=0`。当前 100% 只能算 best-effort，不能作为可信
   电量验收结论。
3. 启动后 internal free 约 22 KiB，历史最低约 1.5 KiB，largest block 约
   15 KiB，触发 heap budget warning。当前运行稳定，但 OTA TLS、BLE Security 2、
   播放和双讲压力阶段必须继续看 internal/DMA low-water mark。
4. 不能在无人按住 SW3 时通过 USB Serial/JTAG 的 DTR/RTS 脉冲 EN 做普通复位：
   MCU 复位会让 GPIO7 电源保持瞬间释放，设备直接掉电，COM5 和网关同时消失。
   抓冷启动日志应先以 DTR/RTS low 等待端口，再由用户按 SW3 物理开机。
5. 日志只能证明显示/触摸控制器和驱动初始化；屏幕实际画面、点击/滑动、SW3
   点按关机和松开后的持续供电仍需要用户目视/手动确认。

## 14. COM5 实机验收顺序

只有得到用户明确同意后才执行。每一步失败都先保存日志和状态，不跳到后续步骤。

1. **身份核对**：确认端口、VID/PID；必要时只读芯片 MAC。确认不是 COM8。
2. **确认持久化状态**：当前 COM5 必须仍保有已验证的 Wi-Fi STA 配置和
   `nuwax_cloud` 动态绑定。本轮烧录保留 NVS；不得执行 erase-flash。若绑定已丢失，
   先走正式 BLE/USB 配网流程，不能用已废弃的编译期 token 代替。
3. **完整基线烧录**：用户手动执行，包含 bootloader、partition table、otadata、
   srmodels 和 App；不得只写 `0x420000` 作为首次 V2 基线。
4. **电源**：SW3 启动后松开仍运行；点按关机；再点按开机；重复多轮并覆盖 USB。
5. **UI/触摸**：屏幕、背光、点击、滑动、菜单、Wi-Fi/云图标、无重启。
6. **电量**：读取 SOC、电压、电流、design capacity、profile/trusted；不匹配时不得
   伪装百分比可信。
7. **Wi-Fi/测试网关**：hello/auth、ping/pong、重连和动态云配置。
8. **正常对话**：MultiNet 唤醒、ASR/TTS、START/END、序号和自动续听。
9. **会议**：云端/允许的 UI 启停、完整落盘、人声 flag/轻增益、环境声不断流、
   手动关闭和短化测试配置下的 quiet timeout。
10. **监控**：只能云端启停、持续原始上传、无播报、完整落盘、END 正确。
11. **软件 AEC 真人双讲**：far-end only、near-end only、固定句双讲、不同音量、
    首尾音节、cancel 后下一轮、至少 20 轮稳定性和 WAV 定量分析。
12. **OTA**：构建 `>7.0.8 / >7000008` 的新 App，使用全新 ID 和不可变 HTTPS
    URL；验证 accepted/progress/staged/reboot/pending_verify/committed/fresh valid。
13. **OTA 故障**：错误 SHA、错误长度、断网、低电、会议/监控 busy、健康门失败
    回滚。串口救援始终保留。

SD、底座 UART 和 4G 按当前用户要求不是本轮阻塞项，但不能写成已验收。

## 15. 后续发布检查表

- [ ] V2 源码没有被测试工程整体覆盖；
- [ ] 版本高于 `7.0.8`；
- [ ] release sequence 高于 `7000008`；
- [ ] request_id/update_id 从未使用；
- [ ] URL 为直接 200、无重定向、不可变 HTTPS 对象；
- [ ] size 和 SHA 来自最终 App，不是旧构建；
- [ ] 四组 host checks 通过；
- [ ] ESP-IDF v6.0 fresh/增量构建通过；
- [ ] App 小于 4864 KiB；
- [ ] 敏感配置未进入 Git、日志或文档；
- [ ] 当前设备不在会议、监控、播放或另一 OTA；
- [ ] 电源健康门满足；
- [ ] 已得到用户对烧录或 OTA 的明确授权；
- [ ] 目标是 COM5/V2，绝不是 COM8；
- [ ] 保留完整串口救援包和回滚方案。

## 16. 下一步唯一建议

迁移和 COM5 基线构建阶段已完成。下一步由用户手动将第 13.2 节的五个镜像写入
COM5，且绝不擦除 NVS；随后严格按第 14 节从电源保持、UI、触摸、电量、Wi-Fi 和
测试网关开始验收。只有 V2 正式树形成实机证据后，再开始声音参数调优或发布
下一版 OTA。

## 17. 2026-07-30：7.0.11 OTA 后网络侧实机回归

### 17.1 发布与 OTA

本次最终 App 镜像来自构建目录
`C:\tmp\nuwax-v2-711-r1-s3`，大小 `3,457,600` bytes，SHA-256：

```text
27f506d9bd9110b6ea512147f344d0ca20556c2285f2361f19cd8bb0838ca51c
```

不可变 HTTPS 对象为：

```text
https://s3.nuwax.com:9443/nuwax-packages/firmware/nuwax-ai-partner/7.0.11/v2-711-27f506d9/NuwaxAIPartner.bin
```

实机事务 `ota-v2-711-20260730-1214-27f506d9` 已完成：

- `7.0.10 ota_0 -> 7.0.11 ota_1`；
- 下载/写入 `3457600/3457600`，SHA 校验通过；
- `committed`，detail 为 `health gate passed`，`rolled_back=false`；
- `correlation_errors=0`，release floor 为 `7000011`；
- 新 hello 上报 `ota_state=valid`；
- dispatch 到 staged 约 32.5 秒，staged 到 committed 约 19.4 秒，完整事务约 51.9 秒；
- `elapsed_ms=31896` 是设备 OTA 结果携带的耗时字段，不将其误写成
  staged 到 committed 的墙钟时间。

按 OTA 实现和本轮操作清单，该事务只更新 inactive App slot，没有擦除 NVS，也没有
触碰 COM5 或 COM8。OTA 重连后的现场状态查询曾观察到旧扫描结果仍在，但当时没有
单独保存状态快照；可持久复核的跨 WebSocket 缓存证据来自
`cloud-gateway/test-wifi-state-reconnect.js`，不能把后续三次扫描本身当作重连证据。

### 17.2 Wi-Fi 扫描回归

在 7.0.11 上连续发送三次 `POST /api/device/wifi/scan`，每次等待设备真正上报新
结果，而不是只检查 HTTP 202：

| 轮次 | HTTP 接受 | 结果 | 网络数 | 端到端观测耗时 | WebSocket 会话 | 音频活动 |
| ---: | --- | --- | ---: | ---: | --- | --- |
| 1 | true | true | 8 | 2725 ms | 未改变 | 无 |
| 2 | true | true | 8 | 2446 ms | 未改变 | 无 |
| 3 | true | true | 8 | 2706 ms | 未改变 | 无 |

三轮均低于 8 秒扫描硬超时；扫描结果时间戳均更新，且同一设备 WebSocket 会话保持
不变。表中耗时是现场脚本包含 HTTP 轮询延迟的观测值，现已写入
`runtime-logs/v2-711-live-regression-20260730.json`。该结果验证 7.0.11 的异步扫描，
不单独证明跨重连缓存；网关进程重启后的持久化也不是本轮验收范围。

### 17.3 会议模式短时闭环

通过测试网关发送一次 `set_audio_mode / meeting / enabled`，保持约 6 秒后发送
`disabled`。结果：

```text
audio_session_id       = meeting_stream_0_70ac8446
duration_ms             = 6140
frames                  = 307 (sequence 0..306)
START/END               = present / present
sequence gaps/duplicates/out_of_order = 0 / 0 / 0
listen_stop_seen        = true
protocol/capture/storage = true / true / true
capture_failed          = false
WAV                     = meeting_stream_0_70ac8446.wav (196524 bytes)
PCM                     = 196480 bytes, PCM16, 16000 Hz, mono
RIFF/data length        = consistent with actual file and bytes_written
TTS frame delta         = 0
```

现场测试 runner 观察到开启/关闭 ACK 均为 `ok=true`，且启停实际生效；逐条原始 ACK
状态快照当时未单独落盘，结果已补记到结构化回归记录。`listen_stop_seen=true` 也是
关闭后轮询观察值：完成日志中的 `false` 只是 END 先于 `listen stop` 到达的正常时序。
设备 WebSocket 会话未改变。此项证明 7.0.11 的
会议控制、连续上行和网关完整落盘链路可用；它不证明人声判别、轻增益或声学效果的
主观质量，后者仍需真人输入和 WAV 定量分析。

### 17.4 监控模式短时闭环

通过测试网关发送一次 `set_audio_mode / monitoring / enabled`，保持约 6 秒后发送
`disabled`。结果：

```text
audio_session_id       = monitoring_stream_1_a4917c86
duration_ms             = 6220
frames                  = 311 (sequence 0..310)
START/END               = present / present
sequence gaps/duplicates/out_of_order = 0 / 0 / 0
listen_stop_seen        = true
protocol/capture/storage = true / true / true
capture_failed          = false
WAV                     = monitoring_stream_1_a4917c86.wav (199084 bytes)
PCM                     = 199040 bytes, PCM16, 16000 Hz, mono
RIFF/data length        = consistent with actual file and bytes_written
TTS frame delta         = 0
```

监控模式没有进入普通 ASR/TTS 链路，关闭后 `currentUplinkAudioSession=null`。该项
证明云端启停、连续上行和完整文件封装正常；“样本逐点不变”由 monitoring host check
覆盖，本次网络短测本身不能从云端 WAV 反证设备是否做过处理。没有覆盖长时间耐久、
断线恢复或正式
云端权限策略。

### 17.5 正常对话和 TTS 传输闭环

为避免真人声学变量，通过测试网关提交纯 ASCII 文本请求。`/api/device/text` 使用
网关当前配置的 Nuwax API，得到严格的 `test passed.` 回复，再由真实火山 TTS 生成
并下发播放数据。结果：

```text
request_id       = live711-real-chat-1785386990189
chat provider    = nuwax-api
chat result      = test passed.
chat_ms          = 1363
tts              = ok=true, provider=volcengine
provider output  = 73374 bytes at 24000 Hz, provider_streaming=false
device PCM sent  = 77 frames / 48916 bytes at 16000 Hz, mono
gateway downlink = same playback_id, 77 frames, 48916 bytes
gateway end      = reason=complete
turn status      = completed
uplink active    = false
playback active  = false after completion
Wi-Fi            = connected
```

这证明正常对话的文本请求、真实 Nuwax API、火山完整合成、网关重采样/按帧实时下发
77 帧和网关
下行会话收尾均可闭环。当前协议没有设备侧“扬声器实际发声完成”ACK，因此真实出声
仍需用户听感或串口/硬件测量确认。本次没有麦克风输入，因此不等价于真实 ASR 或真人唤醒验收；
MultiNet 唤醒率、真实 ASR 和真人双讲仍须另行测试。此前一条中文 PowerShell 测试因
调用端编码成为乱码，不计入语义验收，但对应网关下行会话正常收尾。

另以工作区现有 `sample.wav` 独立调用当前配置的火山双向流式 ASR，结果保存到
`runtime-logs/volc-probe-v2-711-20260730.json`：连接 155 ms，106 个输入 chunk，
收到 58 个 partial 和 1 个 final，最终 `ok=true`、`error=""`。这只证明当前 ASR
服务凭据、WebSocket 和 final 结果链可用；不能替代“设备普通采集 -> 网关 ASR”的
真人端到端测试。

### 17.6 当前裁决和剩余边界

截至本节记录：

- **已通过（网络实机）**：7.0.11 OTA 提交与健康门、Wi-Fi 扫描、会议模式短时协议和
  WAV 闭环、监控模式短时协议和 WAV 闭环、文本到真实 TTS 生成及网关下发闭环；
- **已通过（云服务独立 probe）**：火山双向流式 ASR 连接、partial 和 final；这不是
  设备普通采集端到端证据；
- **已有用户目视/手动证据**：屏幕显示、触摸点击/滑动、Wi-Fi/云端图标、电量图标和
  SW3 点按关机/再开机的基本行为（需继续保留串口日志做正式归档）；
- **不可宣称已完成**：BQ27220 `trusted/profile_verified` 与真实电芯容量一致性、
  BLE/USB 配网全流程、MultiNet 唤醒率、真实 ASR、真人双讲的软件 AEC 声学效果、
  MIC3/slot2 硬件参考 AEC；当前网关没有 battery 状态接口，电量可信度只能从 UI 或
  串口复核；
- **驱动存在但产品路径未启用**：绿色 LED 和 QMI8658A IMU 仅在硬件验收自检路径中
  使用，7.0.11 产品运行路径没有可远程观察的动作，不能把“驱动已构建”等同于实机通过；
- **需要破坏性边界确认后再测**：真正 BLE 重配会写入可回滚 NVS 记录、重启并断开
  WebSocket，最长进入 300 秒配网窗口；本轮没有发送 `start_ble_provisioning`；
- **按用户要求未验收**：SD、底座 UART、4G；
- **端口边界**：本节所有实时验证均走已在线设备和测试网关 API，未操作 COM5 或
  COM8；任何后续串口烧录/OTA 前仍需重新确认目标设备和授权。

因此，V2 目前可以进入测试网关/正式云端协议联调和受控功能测试，但不能把“所有
硬件功能正常”或“硬件 AEC/真人双讲已通过”作为最终发布结论。

## 18. 2026-07-30 正式云端 hello 兼容修复与 7.0.12 待烧录基线

### 18.1 实际故障与根因

目标正式云端为：

```text
wss://testagent.xspaceagi.com/api/device/ws
```

目标 V2 设备 ID 为 `NX-FCBB-6906-0438`。现场失败日志已经证明 TLS 证书校验和
WebSocket Upgrade 均成功：

```text
WebSocket handshake ok
Secure WebSocket connected to wss://testagent.xspaceagi.com/api/device/ws
```

失败发生在应用层。旧 V2 代码把“收到严格扩展 hello ACK”同时当成普通业务放行条件
和 OTA 启动健康条件；正式云端没有先发该扩展 ACK，而是直接下发
`device_command/set_volume`。设备因此丢弃首条有效命令，等待 5 秒后主动关闭连接，
形成稳定重连循环。V1 正式工程的历史实机日志
`runtime-logs/nuwax7-formal-align-20ms-20260729-153253.log` 已确认正式云端首条消息是带
`request_id` 的 `set_volume`，且 V1 不等待 hello ACK 即可正常处理。

### 18.2 设备端兼容规则

7.0.12 已完成以下调整：

- hello 发送成功后立即允许普通业务，不再设置 5 秒 ACK 门禁，也不因缺少 ACK 主动断线；
- 同时接受严格 hello ACK、正式云端简式 `{type:"hello",ok:true,...}`、无 ACK 后直接
  到达的有效 `device_command`，以及应用层 `pong`；
- 首条有效 `device_command` 先建立“正向云端活动”证据，再继续进入原业务处理链，
  不会被兼容层吞掉；
- hello 后立即发送一次应用层 `ping`，在没有启动命令时可由 `pong` 尽快建立正向活动；
- 显式 hello 拒绝采用非阻塞关闭请求；`type:error`、错误设备 ID、
  `authenticated:false`、重复关键字段、尾随垃圾、畸形命令均不能通过 OTA 健康门；
- 严格 ACK 只在 `authenticated:true` 且 `device_id` 必填并与当前设备精确匹配时通过；
  正式云端简式 ACK 才允许省略这两个扩展字段；
- hello 发送失败或云端拒绝后的快速重连路径由连接 owner 完整关闭并回收接收任务，
  不会因反复拒绝累积挂起任务；
- OTA pending 镜像仍必须实际观察到 hello 后的正向入站活动才能提交；仅连接成功或
  仅发送 hello 不会绕过回滚保护；
- hello 中 `audio_params.mic_channels` 改为云端实际收到的单声道数 `1`，不再把 ES7210
  的四个 TDM 硬件槽误报为四声道上行。

测试网关新增 `CLOUD_DEVICE_HELLO_ACK_MODE=formal|strict|none`，默认 `formal`，用来
持续覆盖正式云端简式 ACK，同时保留严格 ACK 和无 ACK 两条兼容回归。该变量只影响
测试网关，不改变正式云端。

### 18.3 验证与构建产物

以下回归均已通过：

- V2 产品迁移/OTA/正式云端兼容 host checks；
- 普通对话 host checks；
- 会议模式 host checks；
- 监控模式 host checks；
- 测试网关 `formal`、`strict`、`none` 三种 hello 行为集成测试；
- 测试网关 Node.js 语法检查。

全新 ESP-IDF v6.0 / ESP32-S3 构建目录：

```text
C:\tmp\nuwax-v2-712-formal-r1-s3
```

构建配置为 16 MB flash、DIO、80 MHz。App 信息：

```text
version          = 7.0.12
release_sequence = 7000012
size             = 3,458,704 bytes (0x34c690)
ota slot free    = 0x173970 bytes (31%)
SHA-256          = 0121a0e2579f7408828fb24576f4ecf16536b8f3bcee9d4c9367a9db791feb53
```

完整镜像哈希：

| 文件 | SHA-256 |
| --- | --- |
| `bootloader/bootloader.bin` | `5631fe41af5c02a11893f29ebadf16b26db6b3fe0266bedcc5e8a0071a553d7e` |
| `partition_table/partition-table.bin` | `aad81c7e6e4686758d711bf7336ff59e4c3e816af34cf0bb795300b5e3bf8cdf` |
| `ota_data_initial.bin` | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `srmodels/srmodels.bin` | `dec3196aaadcf4e6305434462f5735e1148e5ddfe1f6a0696776d666f9066d34` |
| `NuwaxAIPartner.bin` | `0121a0e2579f7408828fb24576f4ecf16536b8f3bcee9d4c9367a9db791feb53` |

### 18.4 烧录边界与正式云端验收

该基线尚未烧录到 COM5，也尚未形成 7.0.12 正式云端实机通过证据。首次安装此基线
必须由用户手动完整写入以下五项：

```text
0x0      bootloader/bootloader.bin
0x8000   partition_table/partition-table.bin
0xf000   ota_data_initial.bin
0x20000  srmodels/srmodels.bin
0x420000 NuwaxAIPartner.bin
```

不得只写 `0x420000`：设备当前可能从另一个 OTA slot 启动。清单不包含 `0x9000`，因此
不会主动覆盖保存 Wi-Fi 和动态云绑定的 NVS；禁止执行 `erase-flash`。烧录后至少核对：

```text
WebSocket handshake ok
Secure WebSocket connected to wss://testagent.xspaceagi.com/api/device/ws
positive cloud activity established kind=device_command
```

最后一行也可能是 `kind=pong` 或 `kind=hello_ok`。不得再出现：

```text
ignoring cloud payload before authenticated hello ACK
authenticated cloud hello failed
```

还必须确认正式云端首条 `set_volume` 被处理、云端图标持续在线且连接不再每约 5 秒
重建，再依次验证普通对话、会议模式和监控模式。只有这些实机证据完成后，才能把
“V2 已连接正式云端”标记为通过。

## 19. 2026-07-31：I2C/OTA 启动修复与 7.0.19 实机闭环

### 19.1 现场故障与根因

7.0.13 的现场日志在运行约 363 秒后先出现 CST816 触摸读取超时，旧恢复路径随后依次
探测 `0x15`、`0x2a`、`0x2d`，并在失败过程中删除、重建设备句柄。一次触摸超时因此
被放大为连续 probe timeout，最终进入 cache error/崩溃链。该故障包含两个独立问题：

1. V2 实物触摸地址已经确定为 `0x15`，运行期扫描另外两个地址没有恢复价值；
2. I2C 中断可在 cache-off 窗口运行时，旧代码可能把 PSRAM 任务栈中的收发缓冲交给
   I2C 驱动，且在超时后立即销毁、重建设备句柄，内存所有权和 ISR 生命周期均不安全。

7.0.14 至 7.0.18 的 OTA 镜像虽然完成下载、校验和重启，但均没有在 120 秒健康窗口内
完成云端正向活动并提交。根因不是 OTA 下载失败，而是启动早期在 Wi-Fi/TLS 之前常驻
创建 AFE AEC 池，额外占用约 23 KiB internal RAM 并降低最大连续块；pending 镜像无法
及时完成联网健康门，因此被 bootloader 正确回滚到 7.0.13。

### 19.2 7.0.19 设备端修复

I2C 路径的修复如下：

- `main/touch_input.c` 固定使用 `0x15`，整个启动周期保留一个设备句柄；读取失败只在
  同一句柄上做两次有限重试，随后进入 2 秒冷却，不再运行期扫描、删除或重建设备；
- 触摸 I2C TX/RX 缓冲改为 `DRAM_ATTR` 内部静态 scratch，事务完成后才复制到调用方；
- 真正执行 I2C 的 touch worker 栈固定在 internal RAM；只消费队列和识别手势的大栈
  仍可放在 PSRAM；
- `touch_gesture` 在读取错误时主动延时，避免错误路径形成忙循环；
- BQ27220、QMI8658A 的正常运行事务使用持久句柄和内部缓冲；音频板初始化不再额外
  probe codec；
- 关闭 `CONFIG_I2C_ISR_IRAM_SAFE`，保留
  `CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM=y`。ISR handler 仍位于 IRAM，但 I2C 中断不再
  以 cache-off 期间可执行的方式注册，避免在 flash/PSRAM 不可访问时继续事务；
- speaker/WebSocket 空闲和重连路径保留 codec/I2S 初始化，只静音输出，避免反复拆装
  共享音频 I2C 句柄。

OTA 启动路径的修复如下：

- speaker 先预分配并锚定共享音频板，再执行 pending 镜像的 mic 自检；
- AFE AEC 不再位于 Wi-Fi/TLS/WebSocket 之前的启动关键路径；
- pending-verify 镜像完全跳过 AEC 池分配，先完成云端健康门并提交；提交触发重连后，
  已变为 valid 的镜像再延迟创建可复用 AEC 池；
- AEC 分配前恢复 `internal free >= 49152`、`largest block >= 16384` 的预检查，并保留
  分配后的 internal/DMA 水位门；资源不足时 fail closed，不启用打断 AEC；
- OTA mic 自检不再先初始化后立即拆除 audio board，避免 codec 句柄被健康门反复重建。

host contract 已覆盖 I2C cache 配置、内部 scratch、touch worker/gesture 栈所有权、OTA
发布策略、正式/测试云端协议、音频上行终止帧、低延迟上行和 Wi-Fi 保存网络列表。
ESP-IDF v6.0 / ESP32-S3 产品构建通过，最终镜像晚于全部相关源码和构建配置。

### 19.3 唯一发布产物

本次正确构建目录是：

```text
NuwaxAIPartner-v2/build-v2-i2cfix-719-s3
```

不得引用旧的 `build-v2-ota-audit-s3`；该目录中的 image descriptor 仍可能是 7.0.8。
7.0.19 App 的不可变身份如下：

```text
version          = 7.0.19
release_sequence = 7000019
size             = 3,501,408 bytes
SHA-256          = 60f35c7f5a9671fd732794a3f0c1aaf5fe37a3c56a4da201c7556bd85e664998
ota slot free    = 0x1692a0 bytes (30%)
```

发布对象：

```text
https://s3.nuwax.com:9443/nuwax-packages/firmware/nuwax-ai-partner/7.0.19/v2-719-60f35c7f/NuwaxAIPartner.bin
```

上传后重新下载的对象与本地镜像大小、SHA-256 完全一致。

### 19.4 OTA 实机结果

目标设备为 `NX-FCBB-6906-0438`。本轮只下发一次 OTA，未操作 COM5：

```text
request_id/update_id = ota_v2_719_i2c_bootfix_20260731_1706_60f35c7f
accepted             = 2026-07-31 17:06:42.711 +08:00
staged               = 2026-07-31 17:07:16.597 +08:00
committed            = 2026-07-31 17:07:35.557 +08:00
downloaded           = 3,501,408 / 3,501,408 bytes
device elapsed       = 33,467 ms
dispatch to commit   = about 52.9 s
result               = committed / health gate passed / code=ok
rolled_back          = false
```

提交后的权威状态为：

```text
firmware.version     = 7.0.19
release_sequence     = 7000019
ota_state            = valid
running_partition    = ota_0
release floor        = 7000019
online/authenticated = true/true
```

运行镜像上报 SHA 与发布镜像一致，OTA 持久化状态健康，
`consistency_uncertain=false`、`rollback_visible=false`。升级后同一稳定会话连续观察
444 秒，跨过原错误约 363 秒的触发时间；22 次协议 ping 均收到 pong，没有离线、重启、
重新鉴权或回滚。

### 19.5 升级后基础功能证据

`runtime-logs/cloud-gateway.out.log` 记录了 7.0.19 重连后的真实 `touch_wake` 对话：

- 设备上行 331 帧、序号 `0..330`，`sequence_complete=true`，缺口、重复、乱序均为 0；
- START/END 完整，`protocol_complete=true`，PCM16/16 kHz/单声道；
- ASR 识别、正式 Nuwax API 文本响应和三段流式 TTS 下行均完成；
- TTS 共 669 帧，播放收口为 `status=completed`；
- 随后的 awake-session 上行持续 53.1 秒、2655 帧，仍无序号缺口、重复或乱序。

这证明升级后触摸唤醒、麦克风上行、云端对话、扬声器下行和共享音频板基本闭环没有
因本轮 I2C/OTA 改动回归。

### 19.6 结论边界

本轮可以判定：I2C 超时被旧恢复代码放大为 probe storm/cache crash 的软件故障链已
消除，7.0.19 OTA 启动内存修复已通过实机健康门且没有回滚。仍需保留以下边界：

- 本轮按约束未占用 COM5，因此 444 秒网络侧稳定观察不能证明运行期绝对没有出现过
  单次、可恢复的 I2C timeout；若要证明日志完全无 timeout，仍需经用户同意后做串口
  长时采集，并反复操作触摸、电量读取和音频播放；
- 网络稳定和一次 touch-wake 对话不能替代触摸全区域、连续滑动及 I2C 并发长稳验收；
- `secure_version=0`，当前防回滚依赖应用 release floor 和 A/B 健康门，尚未启用 eFuse
  硬件 secure-version 防回滚；
- 本节不改变真人双讲、不同扬声器音量下误打断率和首音节保留仍需专项声学验收的结论。
