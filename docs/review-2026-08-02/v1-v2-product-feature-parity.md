# B0-16 NuwaxAIPartner V1 -> V2 产品功能等价审计

## 1. 文档目的与结论

本文件对应评审计划 B0-16，检查 `NuwaxAIPartner` 到
`NuwaxAIPartner-v2` 的产品功能迁移是否完整，并按“保留、替换、删除、
待决策”分类正常对话、人声打断、会议、监控、BLE/USB 配网、OTA、UI、
唤醒、电源/电量和云协议。

本轮仅完成 **A 级静态证据**。未执行 host/gateway 测试，未进行干净构建，
未烧录或操作设备。因此本文件证明的是“静态映射已建立并发现了哪些差异”，
不是“V2 已达到 V1 产品等价”或“设备功能已经通过”。

总体结论：

1. V1 的正式源入口在 V2 中没有路径级删除，V2 增加了新版硬件、低延迟对话、
   OTA 和云兼容等正式实现；顶层产品能力没有发现明确的有意删除。
2. 正常对话、AEC/打断、配网、UI 状态桥、唤醒、电源和云传输均发生了实质替换，
   不能因文件仍存在就标记为动态等价。
3. 会议模式的协议和处理器主体保留，但会话事务、停播门禁和连接 epoch 已替换。
4. 监控 pause/resume 扩展已进入设备、hello、schema、fixture 和正式交付文档；设备同时
   声明基础 `monitoring_mode_v1` 与扩展 `monitoring_pause_resume_v1`，原先“扩展能力未协商”
   的疑点已经关闭。但当前唯一权威交付文档仍把实际 20 ms/640-byte PCM 帧写成
   16 ms/512 bytes，并把可靠队列容量从约 10.24 s 误算为 8.192 s；会议停播失败语义和
   COM8/V1.2 设备身份也与 V2 当前代码不一致，因此协议发布仍被阻塞。
5. V1 当前工作区不是冻结发布基线，动态比较前必须先确定唯一 V1 source/config/image
   身份，否则任何 V1/V2 对比结果都不可复现。

## 2. 证据等级与分类口径

### 2.1 A/B/C/D 证据等级

| 等级 | 本审计中的含义 | 本轮状态 |
|---|---|---|
| A | 源码、CMake、Kconfig/sdkconfig、协议和文档的只读静态映射 | 已完成 |
| B | host、gateway、模拟器、协议 fixture、故障注入和同语料 replay | 开放，未执行 |
| C | 干净 configure/build/link、link map、镜像/分区/descriptor 检查 | 开放，未执行 |
| D | 目标设备、真实网络、真实触摸/电源、WAV/声学、OTA 和长稳测试 | 开放，未执行 |

“A 级完成”只表示静态分母和差异已核对。A 级发现不兼容时，结论仍然是“不等价”
或“待决策”，不能用尚未执行的 B/C/D 证据覆盖。

### 2.2 分类定义

| 分类 | 定义 |
|---|---|
| 保留 | 产品外部契约和核心实现语义保持；仍需适当动态回归 |
| 替换 | 产品意图保留，但算法、状态机、硬件入口、事务或资源所有权发生实质变化 |
| 删除 | V1 能力在 V2 产品路径中明确取消，或其子算法被明确停用且有替代路径 |
| 待决策 | V1 与 V2 产品语义冲突，或缺少权威产品/云端决策，不能由代码评审代替决定 |

本审计没有发现顶层产品功能被明确删除。AEC 内部的 adaptive-reference 和
human-rescue FIR 等 V1 活动子路径在 V2 配置中停用，属于“替换中的子项删除”，
不是删除“人声打断/AEC”这一产品能力。

## 3. 比较基线、分母与方法

### 3.1 V1 基线限制

只读 Git 盘点结果：

- Git HEAD：`8b7cf297460dbb4ffc8a8594a27a1143c199818e`；
- 分支：`codex/smart-barge-in-v1`；
- `NuwaxAIPartner` 相对 HEAD 有 49 个 tracked modified 文件；
- 差异规模为 4,608 insertions、525 deletions；
- `NuwaxAIPartner/CMakeLists.txt:5` 仍声明 `PROJECT_VER "7.0.0"`；
- 当前没有专属于该 V1 文件系统快照的 source/config/image manifest。

因此本文件中的“V1”指 **2026-08-03 审计时的当前文件系统快照**，不等同于
HEAD、7.0.0 发布镜像或任一历史烧录版本。此限制登记为 `PARITY-001`。

### 3.2 V2 基线

V2 使用当前 `NuwaxAIPartner-v2` 文件系统源码。关键源码、根与 main CMake、
`sdkconfig` 和 `sdkconfig.defaults` 在本轮只读复核时与现有
`docs/review-2026-08-02/source-manifest.sha256` 对应项一致。已有 manifest 的完整
范围与排除规则由 B0-07 管理；本文件不复制或重建该 manifest。

### 3.3 源码分母

方法：从两个工程的 `main/` 递归枚举 `.c/.h`，以相对路径配对并对同路径文件计算
SHA-256；另从 `main/CMakeLists.txt` 提取正式列出的 `.c` 源入口。host/check 文件保留
在文件系统分母中，但正式源入口分母以 CMake 为准。

| 统计项 | V1 | V2 | 说明 |
|---|---:|---:|---|
| `main/` 下 `.c/.h` | 260 | 334 | 文件系统静态分母 |
| 同路径 | 260 | 260 | V1 路径均能在 V2 找到 |
| 同路径且字节相同 | 177 | 177 | 只能证明文件相同 |
| 同路径但已改变 | 83 | 83 | 需要逐功能判定 |
| V1-only | 0 | - | 不表示运行行为没有丢失 |
| V2-only | - | 74 | 含产品、host/check 和新增头文件 |
| CMake 正式 `.c` 入口 | 126 | 150 | V2 增加 24 个正式入口；新增 NXFP bounded parser |
| CMake 正式 V1-only | 0 | - | 路径级无正式源删除 |

V2 新增的 23 个正式入口按职责分组：

- 板级/电量/自检 10 个：`battery_insights.c`、`battery_service.c`、
  `board_gpio_inputs.c`、`bq27220_driver.c`、`hardware_selftest.c`、
  `led_driver.c`、`led_status.c`、`qmi8658a_driver.c`、`sd_card_driver.c`、
  `uart_driver.c`；
- 云兼容 1 个：`cloud_protocol_compat.c`；
- 对话 7 个：`audio_uplink_terminal.c`、`barge_in_controller.c`、
  `barge_in_dispatcher.c`、`conversation_audio_health.c`、
  `conversation_metrics.c`、`live_aec_output_provenance.c`、`turn_guard.c`；
- OTA 5 个：`nuwax_ota.c`、`nuwax_ota_device.c`、`nuwax_ota_main.c`、
  `nuwax_ota_manifest.c`、`nuwax_ota_transport.c`。

目录拷贝、同路径存在、相同 SHA 或 CMake 成员统计均不构成功能回归证据。

## 4. 全产品功能矩阵

| 功能组 | V1 外部意图/实现 | V2 当前实现 | 分类 | A 级结论 | B/C/D 状态 |
|---|---|---|---|---|---|
| 正常对话 | 唤醒后采集、START/PCM/END、等待云端并播放回复；固定 4 帧上行批量 | 线上音频基本参数保留；首包/稳态 1 帧、实时年龄门禁、terminal END、turn guard、connection epoch、健康和指标链路 | 替换/增强 | 产品意图保留，实现和延迟链已替换 | 全部开放 |
| 人声打断/AEC | live AEC、adaptive reference、human-rescue FIR、near-end raw mix 参与打断 | ESP-SR AFE low-cost backend；独立 controller/dispatcher；adaptive ref 和 human-rescue FIR 停用，raw mix 为 0 | 替换；子项删除/停用 | 不能静态证明不同音量下零自打断、首音节保留和双讲质量 | B/D 声学开放，C 构建开放 |
| 会议模式 | `meeting_mode_v1`，会议 START/PCM/END 和处理器 | 协议/processor 保留；mode 增加 fail-closed 停播、epoch、START/END 提交事务 | 保留协议，替换事务实现 | 协议主体静态保留，失败语义更严格 | B/C/D 开放 |
| 监控模式 | 启用后连续上传所有成功读取的 MIC 音频，不以 VAD/静音结束 | 初始 STREAMING；120 s 静默向云请求 pause 并等待 ACK，之后 ARMED 检测人声、请求 resume ACK、开新 stream、回放 500 ms preroll，并设置 `VAD_SPEECH` | 待决策 | 与冻结 `monitoring_mode_v1` 明确冲突；还新增云端控制/ACK契约 | 产品/云决策前不可进入等价回归 |
| BLE/USB 配网 | Wi-Fi、device UID、动态 cloud URL/secret、启动期配网 | 保留身份目标；增加 cloud active/pending、Wi-Fi verified/commit marker、rollback、低内存 NimBLE profile | 替换/增强 | 状态事务和生命周期变化 | B/C/D 开放 |
| OTA | V1 有原型/文件但未进入正式 CMake，当前配置未启用 | 条件链接 5 个正式源，`CONFIG_NUWAX_OTA_ENABLE=y`，hello 声明 `ota_v1`，A/B app-only | 新增/正式启用 | 不是 V1 等价能力，而是新增发布功能 | B/C/D 开放 |
| UI | V28 视觉、表情、菜单、Wi-Fi/云/音量等主体 | 大部分视觉文件保留；新增 READY/monitoring/可信电量、revision mailbox，并替换新版屏幕/触摸入口 | 保留视觉主体，替换状态和硬件入口 | 不能由视觉资源相同推导交互等价 | B/C/D 开放 |
| 唤醒 | MultiNet，命令 `ni hao ling zhu`，阈值 30%，编译期为主 | MultiNet 和命令保留；阈值 20%，新增远端 `set_wake_word` 和 NVS 持久化 | 替换/扩展 | 阈值及运行期配置是产品行为变化 | B/C/D 开放 |
| 电源/电量/板级驱动 | V1.2 GPIO9 active-low hold；旧 LCD/触摸/音频引脚 | Miaopan 2.0 GPIO7 PG2 active-high、GPIO16 SW3、bootloader early hold、BQ27220、电量可信度与新版引脚 | 硬件替换 | 不能要求寄存器/引脚“等同”，应按新硬件需求验收 | C/D 开放 |
| 云协议/传输 | hello 核心身份、secret、transport、audio_params、会议/监控能力 | 核心字段保留；新增 firmware、OTA、remote wake、hardware-ref health、正式云兼容分类和 WS epoch/lifecycle | 保留核心 v1，扩展/重构 | schema/云端兼容和断线恢复仍需动态证明 | B/C/D 开放 |

## 5. 分功能源码证据与回归要求

### 5.1 正常对话

静态证据：

- V1 固定批量：`NuwaxAIPartner/main/conversation/audio_uplink.h:14` 为 4 帧；
- V2：`NuwaxAIPartner-v2/main/conversation/audio_uplink.h:17-20` 将首包和稳态设为
  1 帧，并按压力退化到 2/4 帧；`:27-42` 增加实时/可靠队列年龄和 admission deadline；
- V2 新增 `conversation/audio_uplink_terminal.c`、`turn_guard.c`、
  `conversation_audio_health.c` 和 `conversation_metrics.c`；
- 核心 hello 身份和 `audio_params` 分别仍在
  `NuwaxAIPartner/main/session_protocol.c:176-236` 与
  `NuwaxAIPartner-v2/main/session_protocol.c:246-356`。

结论：对话产品意图保留，但上行调度、终止事务、旧连接隔离、播放/状态控制和延迟
预算均已替换。应登记 `PARITY-003`，不能引用 V1 历史日志证明 V2 当前代码等价。

动态回归入口：

- B：分别绑定 V1/V2 manifest 运行同一套 conversation host checks、协议 fixture、
  gateway 正常/断线/迟到帧用例；比较 START/END、序号、首包时间和 terminal reason；
- C：两个冻结基线分别干净构建，记录 config、ELF/map、app descriptor 和镜像 hash；
- D：同设备条件和同语料录制 V1/V2 上行 WAV，量化唤醒到首包、尾静音到 END、
  END 到首个 TTS、heap/stack 和连续多轮稳定性。

### 5.2 人声打断与 AEC

静态证据：

- V1 `sdkconfig:956-996` 未启用 AFE backend，但启用 adaptive reference、
  human-rescue 和 human-rescue FIR；
- V2 `sdkconfig:1069` 启用 AFE backend，`:1087` 停用 adaptive reference，
  `:1130` 停用 human-rescue FIR；
- V2 新增独立 `barge_in_controller.c`、`barge_in_dispatcher.c` 和
  `live_aec_output_provenance.c`；
- 因 backend、reference、混音和 owner 均变化，host 状态检查只能证明局部状态机，
  不能证明回声消除或真人声保真。

结论：这是完整算法替换；V1 活动的 adaptive-reference 与 FIR 子算法在 V2 产品配置
中被删除/停用并由 AFE low-cost 路径替代。`PARITY-003` 在以下 D 级指标冻结并通过前
保持开放：

- 0%~100% 播放音量的设备自声误打断率；
- 近/中/远场、不同方位和噪声条件的真人打断检出率与停止时延；
- 从用户第一音节开始的 preroll 连续性；
- 重叠阶段残余 TTS、用户语音 STOI/PESQ 或已冻结的等价指标；
- 无 reference、reference 失真、I2S stall、WS 断线后的 fail-closed/recovery。

### 5.3 会议模式

静态证据：

- 两工程 `meeting_protocol.c` SHA-256 均为
  `0CBB3E74D888BA145ABED842E32E19194B7B911FC1820BEF7988623799B2AA61`；
- 两工程 `meeting_processor.c` SHA-256 均为
  `D699FE62D2C8AF99B506C510D453772C3AC587FD5B9F36F253F70EDE3A89E6B5`；
- V2 `meeting_mode.c:464-465,602-609` 将停播失败显式变为
  `playback_stop_failed`，不再 fail-open 建流；
- V2 `meeting_mode.c:669-925` 增加连接 epoch、带 deadline 的 listen START/STOP、
  END commit 门禁和失败关闭。

结论：云协议/音频处理主体保留，模式事务实现替换。建议回归入口为
`tools/run-meeting-host-checks.ps1`、网关 meeting 用例和目标机启停/重复/播放中开启/
断线矩阵。D 级必须核对 START、连续序号、END totals、listen stop、WAV 头、时长和
文件大小，并验证停播失败时不创建 session。

### 5.4 监控模式

扩展协议决策已经落地：

- `docs/protocol/README.md:7-15` 将
  `meeting-monitoring-formal-cloud-complete-v1.md` 指定为会议/监控唯一权威交付，并把
  `monitoring-mode-v1.md` 降为历史参考；
- `session_protocol.c:255-257` 同时声明 `monitoring_mode_v1=true` 和
  `monitoring_pause_resume_v1=true`；云端只有在扩展能力为 true 时才应使用 pause/resume；
- schema `:238-239,277-305` 已声明扩展 feature、control 和 ACK；protocol validator 与
  fake-device 也发送扩展 feature 和控制消息；
- `monitoring_mode.c:94-98,1330-1392` 实现
  `ARMED/STREAMING/PAUSE_PENDING/RESUME_PENDING`。初始直接 STREAMING；连续 120 s 没有
  3 帧确认人声后请求 pause；成功 ACK 后关闭旧 stream；ARMED 检出确认人声后请求
  resume；成功 ACK 后建立新 stream 并补发 500 ms preroll；
- `monitoring_mode.c:581-657` 以自适应能量、强峰值和可用时 ESP VAD 的 OR 结果标记候选
  人声，连续 3 帧才成为 confirmed speech；候选帧可带 `VAD_SPEECH`，但静默计时只由
  confirmed speech 清零；
- `monitoring_protocol.c` SHA-256 为
  `d29d36618a487397c76c537d529e86036fdf8c4718dfa40e5c5a8bf459874085`，严格匹配
  request/session/state；拒绝、ACK 超时或重试发送失败会停止监控，避免双方状态分叉。

但权威交付仍与当前代码存在静态冲突：

1. `sdkconfig:1198` 和 `audio_uplink.c:56-57` 把正式上行固定为 20 ms；16 kHz、mono、
   PCM16 因而每完整帧为 320 samples/640 bytes。权威文档 `:150,680-699` 却固定写
   16 ms/512 bytes，且 `:719` 把 128 批次 x 4 帧缓冲算成 8.192 s；当前 PCM 产品应为
   10.24 s。README、schema fixture、validator 和 fake-device 均已使用 20 ms，进一步
   证明权威文字是陈旧值。
2. 权威文档 `:59` 仍写会议停播等待 500 ms 后 fail-open 采集，但当前
   `meeting_mode.c:602-609` 已 fail-closed 返回 `playback_stop_failed`。
3. 权威文档把适用设备写成 COM8，并在 hello 示例固定 `esp_vocat_v1_0` 与具体 AEC 值；
   当前 V2 必须以实际 hello/runtime caps 为准。未绑定当前 source/config/image identity 的
   COM8 一小时历史结果不能关闭 V2 的 B/C/D 证据。

结论：`PARITY-002` 不再是“是否采用 pause/resume”的产品待决策，而是 **扩展已采用、
协议资产尚未一致冻结**。保持 P1 发布阻塞，并与 `VER-002` 交叉引用。关闭条件是：

1. 权威文档、schema、fixture、validator、fake-device、正式云实现和固件统一 20 ms
   当前产品 profile，或全部明确以 hello/NA header 为准且删除相互矛盾的固定值；
2. 修正文档中的会议 fail-closed 语义和 V2 设备适用边界；
3. 执行当前 manifest 绑定的 B 级协议/gateway 回归、C 级构建/descriptor 绑定和 D 级
   监控 pause/resume/END/序号/WAV/一小时长稳验证。

### 5.5 BLE/USB 配网

静态证据：

- 两工程均正式链接 `nuwax_provision_new` 主体，设备 UID/云配置目标得以保留；
- V2 `nuwax_cloud_config.c:13-14,127-276` 将云配置分为 active/pending 并提供
  commit/discard；
- V2 `nuwax_provision_main.c:88-90` 增加 Wi-Fi verified/commit marker，
  `:1530-1639` 增加本地重配网 rollback；
- V2 `sdkconfig:1746` 将 NimBLE 内存分配到 external memory。

结论：配网外部目标保留，但事务、NVS schema、掉电恢复和 BLE 生命周期均是替换。
B 级应覆盖 parser/simulator、错误 secret/UID/URL、重复请求和掉电状态转换；D 级应覆盖
首次配网、已有配置重配网、失败 rollback、连续重启、Wi-Fi/云验证和 BLE 资源释放。
现有历史交接文档中的设备结果只有在绑定当前 source/config/image hash 后才能使用。

### 5.6 OTA

静态证据：

- V1 `main/CMakeLists.txt` 不链接 OTA 源，当前 V1 配置也不形成正式 OTA 产品能力；
- V2 `main/CMakeLists.txt:78-97` 在 `CONFIG_NUWAX_OTA_ENABLE` 下正式链接 5 个源；
- V2 `sdkconfig:998` 启用 OTA；
- V2 `session_protocol.c:264,275-310` 声明 `ota_v1` 并发布 firmware/partition/image
  identity。

结论：OTA 是 V2 新增并正式启用的能力，不是 V1 等价项。B 级应验证 manifest、命令
幂等、下载中断、hash/签名/版本拒绝；C 级应绑定分区、app 大小和镜像 identity；D 级
应执行 A/B 正常升级、断电、损坏包、pending-verify rollback、跨局域网下载和长稳。
分区/model/app-only 兼容问题继续引用 `PART-001`~`PART-005`，不重复建立 parity issue。

### 5.7 UI、显示与触摸

静态证据：

- V2 UI 目录视觉主体大部分与 V1 相同；变化集中在 state、status bar、focus menu、
  app shell 和新版硬件入口；
- V2 `nuwax_device_state.h:56,78-82,98` 增加 READY、monitoring UI 状态和
  `battery_soc_trusted`；
- V2 `app_shell.c:76-94,358-412` 使用 revision mailbox 发布完整状态；
- V2 `nuwax_status_bar.c:920` 以 `%`/`~%` 区分可信和估算电量；
- LCD、触摸、背光引脚从 V1.2 到 Miaopan 2.0 全面替换，不能由 UI 资源 SHA 证明驱动等价。

结论：视觉产品主体保留，状态同步和硬件驱动替换。B 级可覆盖 state policy/mailbox；
C 级验证资源链接和内存布局；D 级必须覆盖开机首帧、Wi-Fi/云、READY/对话/会议/监控、
可信电量、触摸点击/滑动、休眠/唤醒和长时间刷新。

### 5.8 唤醒

静态证据：

- V1/V2 均配置 MultiNet 命令 `ni hao ling zhu`：V1 `sdkconfig:1141`，
  V2 `sdkconfig:1340`；
- 阈值从 V1 `sdkconfig:1145` 的 30% 改为 V2 `sdkconfig:1344` 的 20%；
- V2 `wake_word_detector.c:142-165,889-1001` 增加 NVS load/persist、运行期切换和失败恢复；
- V2 `session_protocol.c:259` 声明 `remote_wake_word_config`，协议见
  `docs/protocol/set-wake-word-v1.md`。

结论：登记 `PARITY-004`，分类“替换/扩展”。20% 不能仅以代码可配置为理由放行；
D 级必须冻结语料、距离、方位、噪声、多人、设备播放期间和音量矩阵，量化漏唤醒与
误唤醒，并验证远端修改、重启持久化、无效命令和 NVS 写失败恢复。

### 5.9 电源、电量和新版硬件驱动

静态证据：

- V1 `esp_vocat_v1_0_pins.h:39`：GPIO9、运行时 active-low hold；
- V2 同文件 `:39-42,69`：GPIO7 PG2 active-high hold、GPIO16 SW3 active-low；
- V2 `nuwax_power_ctrl.c:224-441` 增加 intentional-off marker、按键释放确认、
  critical-operation gate、关背光/PA、PG2 release 和 deep-sleep fallback；
- V2 bootloader 组件在 app 前执行 early power hold；
- V2 新增 BQ27220、battery service/insights、QMI8658A、LED、GPIO、SD/UART 和自检源；
- V2 `battery_service.c:205-211` 只在有效且 profile verified 时信任 SOC，UI 可显示估算值。

结论：这是硬件平台替换，不应以 V1 驱动行为作为新板“等价”标准。C 级要证明正确
board profile 和源进入镜像；D 级按原理图/规格书验证 GPIO、时序、电压、I2C/I2S、
显示、触摸、SW3、电量和音频。SD/底座 UART/4G 若当前产品不验收，必须明确标记
“非验收/静态隔离”，不能误写为硬件通过。电量可信度沿用 `BAT-001`。

### 5.10 云协议与 WebSocket

静态证据：

- 核心 hello 字段在两工程保留：`device_id`、`device_secret`、`transport`、
  `audio_params`，以及 `meeting_mode_v1`/`monitoring_mode_v1`；
- V2 `session_protocol.c:259-310` 增加 remote wake、OTA 和 firmware identity；
- V2 `:231-352` 将 hardware-ref candidate/health/enabled 分开；
- V2 新增 `cloud_protocol_compat.c`，WebSocket lifecycle、connection epoch 和业务认证
  状态也发生重构。

结论：核心 v1 外部身份保留，但 capability、认证、旧连接隔离和断线恢复属于扩展/
替换。B 级需运行 schema/fixture/gateway 畸形帧、401/403、hello ACK、迟到旧 epoch、
TTS 背压和断线重连；D 级需验证正式云兼容、云端完全停止时本地 UI/电源可用、重连
不混用 turn/session。协议字段未正式冻结继续引用 `VER-002`。

## 6. Canonical 问题清单

| ID | 级别 | 问题 | 影响 | 关闭条件 |
|---|---|---|---|---|
| PARITY-001 | P1 | V1 当前有 49 个 tracked 修改且无独立 source/config/image manifest | V1 身份不唯一，对比不可复现 | 指定唯一 commit/发布镜像，或冻结当前文件系统并生成 source/config/image hash tuple |
| PARITY-002 | P1 发布阻塞 | pause/resume 扩展已进入 hello/schema/code，但唯一权威交付仍把实际 20 ms/640-byte 帧写成 16 ms/512 bytes、误算队列时长，并保留旧会议停播与 COM8/V1.2 身份描述 | 云端可能按错误帧长、时间戳、容量和失败语义实现；历史 COM8 证据可能被误归因给 V2 | 统一固件/权威文档/schema/fixture/fake-device/正式云的媒体与失败契约，并完成绑定当前 identity 的 B/C/D 回归 |
| PARITY-003 | P1 产品验收阻塞 | 正常对话/AEC/打断的 backend、上行、owner、END 和 guard 均替换，当前无绑定基线的成对回归 | 不能证明低延迟、零自打断、首音节保留或双讲质量 | 完成冻结基线的 B/C，并通过同语料/WAV/真人声学 D 级 SLO |
| PARITY-004 | P1 产品验收阻塞 | 唤醒阈值 30% -> 20%，且新增运行期远端/NVS 配置，缺当前镜像定量验收 | 可能改变漏唤醒、误唤醒和播放期触发率 | 冻结阈值与语料矩阵，完成误/漏唤醒统计及远端配置/持久化恢复测试 |

同根因问题不在此重复编号：发布镜像绑定使用 `BUILD-001`，分区/OTA 边界使用
`PART-001`~`PART-005`，电量使用 `BAT-001`，协议版本冻结使用 `VER-002`，配网掉电
一致性使用 `NVS-005`，BLE/WS/音频并发使用既有 `RTOS`、`NET-CONC`、
`AUDIO-CONC`、`API` 问题。

## 7. 分层回归入口与证据缺口

| 功能 | B 级入口 | C 级入口 | D 级入口 | 当前结论 |
|---|---|---|---|---|
| 正常对话/AEC/打断 | conversation host checks、协议/gateway、同语料 replay | V1/V2 干净构建、map/ELF/image hash | 延迟、WAV、音量/距离/噪声双讲、长稳 | 开放 |
| 会议 | meeting host checks、gateway command/ACK/epoch | 产品 profile 构建和协议资产验证 | 启停/播放中开启/断网、END/序号/WAV | 开放 |
| 监控 | 统一 20 ms oracle 后运行 schema/fixture/fake-device/gateway pause/resume/ACK/epoch 用例 | 产品 profile 构建、descriptor 与协议资产绑定 | pause/resume、END/序号/WAV、断线和 1 h 长稳 | 协议资产一致性关闭前阻塞 |
| 配网 | simulator/parser/gateway、NVS 状态模型 | NimBLE/profile/link map | BLE/USB、重配网、掉电、rollback、连续重启 | 开放 |
| OTA | manifest/gateway、拒绝/中断/幂等 | 分区、descriptor、镜像大小/hash/signature | A/B、断电、rollback、WAN、长稳 | 开放 |
| UI | state/mailbox host checks | UI 资源和内存布局 | 屏幕/触摸/状态/电量/休眠视觉回归 | 开放 |
| 唤醒 | parser/state/NVS host 检查 | srmodels/config/image 绑定 | 误/漏唤醒、播放期、噪声、运行期修改 | 开放 |
| 电源/硬件 | 静态 driver/state fault model | board profile/bootloader/app 构建 | 波形、SW3、掉电、电量、显示/触摸/音频 | 开放 |
| 云协议/WS | schema/fixture/gateway/fault injection | TLS/profile/link map/image | 正式云、云死亡、断网重连、资源长稳 | 开放 |

现有 `tools/run-conversation-host-checks.ps1`、`tools/run-meeting-host-checks.ps1`、
`tools/run-monitoring-host-checks.ps1` 以及工作区 `cloud-gateway` 测试只视为“候选入口”。
脚本存在不等于当前批次已执行，也不证明脚本覆盖的 oracle 与当前产品决策一致。

历史 7.0.11、7.0.19、COM8 或其他设备日志/录音若没有同时绑定 source manifest、
最终 sdkconfig、app/srmodels/partition/bootloader hash 和唯一 run_id，不得用作当前
V1/V2 parity 的 B/C/D 关闭证据。

## 8. B0-16 完成度与放行门

A 级静态范围已完成：

- 已建立 V1/V2 源码和正式链接入口分母；
- 已覆盖计划要求的全部十类产品功能；
- 每类均有保留/替换/删除/待决策分类；
- 已记录关键源码/config/协议证据；
- 已建立 canonical parity issue 和后续回归入口；
- 已明确顶层功能无路径级删除，但不能据此宣称行为等价。

仍开放：

- `PARITY-001` 的唯一 V1 基线；
- `PARITY-002` 的监控扩展协议资产一致性与绑定回归；
- 所有未执行的 B/C/D 证据；
- `PARITY-003` 的对话/打断/AEC 动态等价；
- `PARITY-004` 的唤醒定量验收；
- 与 BUILD、PART、VER、NVS、BAT、并发和安全评审交叉引用的发布门禁。

因此 B0-16 可标记为“**completed-A; B/C/D-open; release-blocked**”，但整个产品功能
等价结论仍为“**未证明**”。在 `PARITY-001` 和 `PARITY-002` 关闭前，不得把 V2
声明为 V1 的可复现、协议兼容发布替代物。
