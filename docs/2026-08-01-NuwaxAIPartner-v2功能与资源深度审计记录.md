# NuwaxAIPartner-v2 功能与资源深度审计记录

> 日期：2026-08-01  
> 审计对象：`NuwaxAIPartner-v2` 当前工作区源码和 2026-08-01 10:24 最近成功构建产物  
> 当前结论：代码审计完成，尚未修改业务代码，尚未完成最新 7.0.21 实机产品验收

> 构建一致性说明：10:24 构建晚于 `main/` 下全部当前源码，BIN/ELF 与本次读取的业务源码一致。监控 Host Check 已通过；本次续审没有重新运行全部产品 Host Check，也没有烧录或做 D 级实机验证。

## 1. 审计边界与结论等级

本记录分析正常对话、连续对话、AEC、真人打断、会议、监控、UI 状态同步、触摸、BLE 配网、WiFi、WebSocket、设备与云协议、OTA、MCP 以及内存和实时资源。此次只做代码、配置、构建产物和既有日志审计，不烧录、不连接设备、不修改功能实现。

本文使用三类证据，避免把“代码存在”误写成“产品效果通过”：

| 证据等级 | 含义 | 可以得出的结论 |
|---|---|---|
| S：源码确认 | 当前 V2 源码、Kconfig、sdkconfig、分区和 ELF 可直接证明 | 状态机、参数、错误路径、静态资源设计确实存在 |
| H：Host Check | 主机检查脚本通过 | 纯逻辑、协议序列和边界用例在模拟环境成立 |
| D：设备实测 | 与当前源码相同的固件在目标硬件上有完整日志和音频样本 | 才能确认实时性、内存峰值、AEC、打断、弱声、网络和 UI 体验 |

最近记录中监控 Host Check 和 ESP-IDF 6.0 全量构建已经通过。此前的 conversation/product Host Check 可作为历史回归证据，但本次续审未全部重跑；任何 Host Check 都不能替代 D 级验证。

## 2. 基线与总评

### 2.1 最近成功构建基线

| 项目 | 当前值 |
|---|---:|
| 工程版本 | 7.0.21 |
| ESP-IDF | 6.0 |
| 目标芯片 | ESP32-S3 |
| Flash | 16 MiB |
| PSRAM | 8 MiB Octal，80 MHz |
| 固件 BIN | 3,528,032 B，2026-08-01 10:24:33 |
| ESP-IDF image size | 3,527,907 B |
| BIN SHA-256 | `C12514FCC258F881A83D7995789C5D41D479E905E3C770145E45060631772821` |
| 生效分区表 | `partitions.customwake.csv` |
| 单个 OTA 槽 | `0x4C0000` = 4,980,736 B |
| OTA 槽剩余 | 1,452,704 B，约 29.17% |

该构建覆盖当前 `main/` 源码；审计时未发现晚于 BIN 的业务源文件。原 `P0-BUILD-01` 已关闭，但仍需保留“构建通过不等于实机功能验收通过”的边界。

### 2.2 总体判断

工程已经不是原型骨架。正常对话、连续唤醒会话、流式音频、播放、打断事务、会议、监控、配网、重连、OTA 和 UI 状态桥均有真实实现，且多个过去发现的问题已经修复。

当前仍不能按“产品功能全部通过”交付，主要原因如下：

1. 正式云鉴权字段存在文档契约冲突，可能直接造成正式云无法建立应用会话。
2. 最新 7.0.21 没有对应的真人打断、硬件参考、20 ms DSP 和峰值内存实机闭环证据。
3. 最近可用的旧固件实机日志出现内部 RAM 最低仅 35 B 的记录，当前版本必须重新证明资源安全。
4. 监控模式已实现 ARMED/STREAMING、180 秒静音结束和新人声重开轮次，但正式云兼容性和远场弱声阈值仍待实机验收。
5. 会议/监控可靠队列可保留约 10.24 秒陈旧 PCM；WebSocket RX 还有逐帧分配和回调背压风险。
6. FreeRTOS 侧存在唤醒静态任务自删除竞态、OTA 栈余量误报、100 Hz 下 1 ms 等待退化为零 tick，以及缺少业务任务健康监督和系统化运行指标的问题。
7. MCP 对外宣称可用，但工具仅三个，其中舵机工具仍是占位；尚不能语音控制音量、会议、监控等核心设备功能。

## 3. 系统运行拓扑

```text
触摸 / 唤醒词 / UI 菜单 / 云命令
                  |
                  v
          主应用会话所有者
        /         |          \
正常对话       会议模式       监控模式
   |              |             |
实时采集队列     麦克风独占      麦克风独占
   |              |             |
AEC/真人检测      20 ms PCM      20 ms PCM
   |              |             |
预卷/编码/上行  可靠上行队列   可靠上行队列
        \         |          /
          WebSocket 单连接
          |              |
     JSON 控制消息       NA 二进制音频
          |              |
       正式云：ASR -> 智能体 -> TTS
                         |
                     PCM 下行
                         |
                  speaker 队列/I2S

应用状态 -> UI mailbox/state bridge -> LVGL 状态栏、表情、菜单
WiFi/BLE 配网 -> NVS 暂存/提交 -> WiFi -> WebSocket 外层重连
OTA 命令 -> manifest 校验 -> HTTPS 下载 -> OTA 分区 -> 健康门/回滚
```

所有三个音频模式竞争同一麦克风所有权；会议和监控会抢占正常对话。播放、采集、WebSocket RX、上行发送、LVGL、触摸和部分诊断任务并行运行，因此产品稳定性取决于任务优先级、内部 RAM、DMA 连续块和 20 ms DSP 时限，而不是只取决于总 PSRAM 容量。

## 4. 正常对话与连续对话

### 4.1 预期状态机

```text
SLEEPING（月亮，仅本地唤醒检测，不上传）
   | 唤醒词“ni hao ling zhu”或长按屏幕
   v
LISTENING（建立/复用 awake session）
   | 检出真人语音
   v
持续上传 16 kHz / 单声道 / 20 ms 音频
   | 云端断句、ASR、智能体处理
   v
THINKING -> SPEAKING
   | 正常播放结束
   v
LISTENING（同一 awake session，立即等待下一轮）
   | 120 秒没有有效人声/会话活动
   v
SLEEPING
```

### 4.2 源码推演结果

| 环节 | 当前行为 | 结论 |
|---|---|---|
| 休眠 | 启动本地唤醒检测器，休眠态不打开云音频上行 | 正确 |
| 唤醒 | 唤醒词回调或触摸长按投递事件，进入 awake session | 代码闭环存在 |
| 聆听 | 先保留预卷，检出语音后开启正式上行 | 符合减少空白上传和保护句首的目标 |
| 音频参数 | 16,000 Hz、单声道、20 ms、320 samples、PCM16 | 已由 sdkconfig 和静态断言锁定 |
| 一句话结束 | 本地 VAD 只记录句界日志，不逐句发送 `listen stop` | 符合“云端负责断句、设备持续上传”要求 |
| 云端处理 | 接收 ASR partial/final/result、文本、播放 begin/end、TTS result | 协议处理存在 |
| 播放完成 | 恢复同一连续会话的等待/聆听，而不是回到必须重新唤醒的休眠 | 逻辑正确 |
| 无人声超时 | awake session 超时为 120,000 ms | 符合两分钟需求 |
| 云无进展 | ASR 进展 watchdog 到期会终止连接，交给外层重连 | 避免无限假在线 |
| TTS 标记缺失 | 播放阶段和 drain 阶段有有界超时 | 不会无限卡在 speaking |

关键证据包括：

- `main/nuwax_ai_partner_main.c:10989`：本地 VAD 句界被忽略，保持连续上行。
- `main/Kconfig.projbuild:2259`、`sdkconfig:1315`：awake idle timeout 为 120 秒。
- `main/nuwax_ai_partner_main.c:11771`、`:11784`：播放和聆听状态切换。
- `main/nuwax_ai_partner_main.c:7751`：云 ASR 进展 watchdog。

### 4.3 仍需验证的体验风险

1. 连续对话的代码路径很长，包含 capture handoff、播放期 prelisten、异步 speaker owner 事件和云事件；必须用最新固件连续完成至少 100 轮“说话 -> 回复 -> 继续说话”，确认没有偶发丢失恢复事件。
2. 云端若只发部分播放/TTS 事件，设备会在超时后重连，能恢复但用户会感知一次中断；正式服务端必须固定事件顺序。
3. 本地 VAD 仍参与“何时开始正式上传”和无人声窗口，弱声可能无法打开上传门，阈值必须用 0.5 m、1 m、2 m 和噪声场景实测。
4. 当前 PCM16 上行带宽约 32 KB/s（不含协议和 WebSocket 开销）；弱网和长会话必须以队龄而不是只以队列满判断健康。

## 5. 唤醒词与长按唤醒

当前启用 MultiNet，自定义命令为 `ni hao ling zhu`，显示名“你好灵珠”，阈值 30%，命令窗口 3 秒。唤醒任务栈为 8 KB PSRAM，模型工作对象由 ESP-SR 管理，采样工作区分配在内部 RAM。麦克风读取采用最长 100 ms 的有界等待，触摸仍作为唤醒词不可用时的后备入口。

确认项：

- 唤醒检测和正常对话使用显式麦克风 owner，避免两个任务同时读 I2S。
- 检出一次后唤醒任务退出，由主状态机接管麦克风。
- `request_stop` 配合有界读超时，正常情况下可以在有限时间退出。

问题：

- **P2-WAKE-01**：`nuwax_wake_word_detector_init()` 中途失败时没有统一 rollback。模型列表、互斥量或已经创建的 backend 可能一直保留到重启，且初始化失败后无法干净重试。正常启动不受影响，但异常恢复不够完整。
- **验证项**：30% 阈值不能仅凭代码判断识别率。需要安静、远场、设备播放期、电视背景声和多人声的漏唤醒/误唤醒统计。

## 6. AEC、真人打断与 320/256 时间轴

### 6.1 当前闭环

```text
speaker generation 绑定
  -> 同步软件/硬件参考
  -> 256-sample AEC 原子处理
  -> 并行 FIFO 同步输出 320 samples
  -> 真人候选（双麦、残差、相关性、能量）
  -> 连续帧确认
  -> generation-bound 本地停播
  -> 修复并清理 preroll 重叠区
  -> 异步发送 barge_in candidate/confirmed + 新 listen start
  -> 上传新一轮用户语音
```

### 6.2 已修复的时间轴问题

旧实现中 320-sample capture 与 256-sample AEC chunk 不匹配，human-rescue 只保存最后一个 chunk，可能导致复制失败、音频重复和元数据描述错误时间段。当前代码已经建立并行适配器：

- AEC PCM 与 human-rescue PCM 同步写入 FIFO。
- 输出 provenance 和 rescue provenance 随样本同步入队/出队。
- 一个 320-sample 帧支持跨 RAW/SCALAR/FIR 多段来源。
- 首次不足 320 samples 时 `last_output_emitted=false`，调用方不得把原始当前帧冒充上一时间段的 AEC 输出。
- 会话结束提供 `nuwax_live_aec_flush_pending()` 显式清空尾部。
- FIFO 音频计数与 provenance 不一致时 fail closed 并清空适配器。

证据位于 `main/conversation/live_aec.c:2053-2428`、`:2628`、`:3137-3315` 和 `main/conversation/live_aec.h:68-82`。

### 6.3 尚未通过的产品证据

- **P0-BARGE-01**：最新 7.0.21 没有真人打断闭环日志。必须看到：真人起声 -> candidate -> confirmed -> 物理停播 -> 协议事务提交 -> preroll 上传 -> 新 ASR 文本。
- **P0-RT-01**：最新 7.0.21 尚未证明播放期 DSP p99 小于 20 ms。旧日志曾出现 stale capture 和 sequence gap，不能只调检测阈值掩盖调度超时。
- **P1-AEC-01**：旧日志持续出现 `hw_ref_frames=0`，只能证明软件参考路径参与，不能证明 MIC3/硬件参考链真实有效。
- **P1-BARGE-02**：检测器包含大量固定能量、相关性和双麦平衡阈值；代码结构完整，但远场弱人声、近场轻声和不同音量下的召回率未知。

验收时必须同时记录 AEC、真人检测、cleaner、preroll、编码和入队各阶段 p50/p95/p99，不能只记录总 `dsp_us`。

## 7. 会议模式

会议状态机为 `DISABLED -> STARTING -> ACTIVE -> STOPPING -> DISABLED`，错误时进入 ERROR 后复位为可重用状态。UI 请求需要云端授权；设备发送 meeting start，请求 ACK 后才进入采集。ACTIVE 期间使用独立麦克风 owner、20 ms 单声道音频和可靠上行策略。连续 30 分钟静音会结束会议。

正确点：

- START/ACK/ACTIVE/STOP/END 和请求 ID、音频会话 ID 均有明确状态。
- 第一帧和最后一帧带 START/END flag，speech 帧带 VAD flag。
- 云断线、麦克风错误、队列失败和用户关闭均进入统一收尾。
- 与正常对话互斥，不会同时争用麦克风。

问题：

- **P1-MEETING-01**：模式切换只等待播放停止 500 ms；超时后仍带着 speaker pending 开始会议采集（`main/audio_modes/meeting/meeting_mode.c:566-590`）。如果 I2S 停播异常，会议前几帧可能混入设备声音。产品策略应与监控一致：播放未物理停止则拒绝开流，而不是 fail open。
- **P1-UPLINK-01**：PCM16 可靠队列深度 128，每个 slot 最多装 4 帧，约 338,944 B PSRAM，可缓存 10.24 秒。弱网恢复后会上传很旧的会议音频，语义时间线可能落后真实现场。
- **验证项**：30 分钟静音阈值和 speech 判断需要弱声、空调声、远场多人会议实测。

## 8. 监控模式

监控状态机和云命令 ACK 已实现；启动时会先确认播放停止并初始化麦克风，然后进入 `ACTIVE/ARMED`，此时只在本地检测人声，不创建上行队列，也不发送 `listen start`。

**P1-MONITOR-01（2026-08-01 已修复，待实机验收）**：监控采集已拆为内部 `ARMED` 和 `STREAMING` 两个阶段，保持现有公开状态和线协议不变：

```text
ARMED（本地监听，不上传）
  -> 自适应噪声底检测到连续 3 个 20 ms 人声帧
  -> 新 audio_session_id + 现有 listen start + 500 ms 预卷 + 持续音频
  -> 180 秒无“连续 3 帧”确认的人声
  -> 最后一帧 END + 现有 listen stop，结束云端这一轮
  -> 回到 ARMED
```

实现位于 `main/audio_modes/monitoring/monitoring_mode.c`，参数位于 `main/Kconfig.projbuild`。默认阈值为 `avg_abs >= max(280, noise_floor + 150)` 且 `peak >= 900`，并保留强峰值弱声路径；单帧噪声不会开启或延长云端轮次。500 ms 预卷约占 16 KB PSRAM；可靠上行队列和 Opus 编码器只在 `STREAMING` 阶段创建，停流后释放。

验证证据：`tools/run-monitoring-host-checks.ps1` 已覆盖“开头静音不上传、预卷、两轮独立 session/sequence、静音 END/stop、ARMED 期间保持模式、云 disable 清理”并全部通过；ESP-IDF 6.0 全量构建通过。仍需实机用安静环境、空调持续噪声、远场弱声和短促冲击声校准阈值，并用正式云确认 180 秒 stop 后的新一轮 start 可正确建流。可靠队列的 10.24 秒陈旧音频风险仍独立存在。

## 9. UI、状态栏、菜单与状态同步

### 9.1 当前状态映射

| 应用状态 | 下标图标 |
|---|---|
| BOOTING / WIFI_CONNECTING / WS_CONNECTING | recognizing |
| IDLE / READY | recognizing 资源（无动画） |
| LISTENING | listening |
| THINKING | thinking |
| SPEAKING | speaking |
| ERROR | error |
| SLEEPING | sleep/月亮 |
| 会议 STARTING/ACTIVE/STOPPING | 强制 listening |
| 监控 STARTING/ACTIVE/STOPPING | 强制 listening |
| 会议/监控 ERROR | 强制 error |

结论：只有 `NUWAX_DIALOG_SLEEP` 会映射月亮。`IDLE` 已映射 `READY`，虽然 READY 暂时复用 recognizing 资源，但不会显示月亮。会议和监控状态通过共享 UI snapshot/mailbox 覆盖普通对话图标，不会因为底层 app state 暂时变成 idle 而显示月亮。

关键证据：

- `main/nuwax_ui_v28/nuwax_state_bridge.c:272-325`
- `main/nuwax_ui_v28/nuwax_dialog_policy.h:5-18`
- `main/nuwax_ui_v28/nuwax_status_bar.c:812-829`

### 9.2 UI 问题与资源行为

- **P2-UI-01**：READY 没有独立图标，复用 recognizing，用户不易区分“已连接待命”和“正在识别”。
- **P2-UI-02**：设备菜单只有会议开关，没有监控开关；监控只能依赖云端命令或外部测试入口。
- 表情缓存为 4 个 360x360 RGB565 全屏槽，常驻约 1,036,800 B PSRAM。
- LVGL 使用 24 行局部 draw buffer，每个 17,280 B；优先单缓冲内部 RAM，资源不足时退到两个 PSRAM buffer。注释中的实测显示 PSRAM draw buffer 可把渲染时间放大到约 88 ms/帧。
- 菜单和抽屉使用 snapshot，打开复杂界面时会额外产生大块 PSRAM 分配和碎片压力。
- 显示驱动固定占用约 15,184 B 内部 DMA RAM（720 B line + 14,400 B TX + 64 B command）。

## 10. 触摸、菜单交互与配网入口

触摸原始输入、手势解析和 LVGL/UI 消费分层存在。I2C 读取失败会产生 cancelled 事件，清理长按/拖拽状态以及可能的 STOP_LISTEN，不再把一次失败遗留成下一次长按重启或错误唤醒。

仍需区分两类故障：

- I2C probe timeout、无 ACK、RST/INT/FPC/供电异常属于硬件或底层时序问题，手势层只能避免状态残留，不能软件修复物理通信。
- **P1-RTOS-05**：`touch_reset()` 计划将 RST 拉低 5 ms，但 `pdMS_TO_TICKS(5)` 在 100 Hz 下等于 0，低脉宽可能只剩连续 GPIO 写之间的极短时间。它不能解释断线、排线和供电等所有 I2C 故障，但足以造成触摸芯片未可靠复位并放大 probe timeout。ES7210 在 I2S ready 后的 stop/start 也使用同样的 5 ms 零 tick 间隔，可能影响麦克风 codec 重启稳定性。硬件时序应使用器件要求对应的微秒延时或向上取整的至少一 tick，并分别验证最小/最大脉宽。
- UI 菜单进入 BLE 配网依赖主应用释放音频和网络相关资源；需实测在线、对话中、播放中、会议中四种入口的互斥提示和退出恢复。

## 11. BLE 配网、WiFi 与云配置提交

### 11.1 已实现的安全提交顺序

```text
BLE/USB 收到候选 WiFi + 云配置
  -> stage pending config
  -> 尝试连接候选 WiFi
  -> GOT_IP
  -> commit_pending
  -> 后续使用新网络和云配置

任一阶段失败
  -> discard_pending
  -> 保留并恢复之前已提交的网络/云配置
```

该设计避免“手机写入了错误 WiFi 后永久覆盖旧配置”。WiFi manager 还包含已保存网络列表和 fallback watcher；WiFi 断线时先恢复 WiFi，WebSocket 外层循环随后重连云端。

### 11.2 风险

- **P1-SEC-01**：当前启用 Security 2 开发模式，PoP 固定为 `Nuwax7-Dev-PoP`。量产设备共享固定 PoP 会显著降低配网鉴权强度，必须改为每设备唯一凭据或安全扫码交付。
- 在线重配网会主动断开现有云会话，产品 UI 应明确显示“配网中”，避免用户误认为设备死机。
- 必须做断电注入：stage 后、GOT_IP 前、commit 前、commit 后分别断电，确认 NVS 不出现半提交。

## 12. WebSocket、心跳、断线与重连

### 12.1 生命周期

当前实现已改为单一关闭 owner：

```text
owner request_close
  -> connected=false
  -> shutdown/interrupt socket
  -> 等待 RX task 自行退出
  -> owner 唯一一次销毁 TLS/socket
  -> IDLE

等待或取锁失败 -> FAULTED，保留句柄，不强删可能持锁的任务
```

RX 任务不再自行销毁 transport，也没有“主任务强删 RX 后再次抢同一锁”的旧死锁路径。网络 I/O、锁等待和 join 均有超时，失败会进入受控重连。

心跳在所有模式下由 WebSocket RX/连接层独立执行；会议、监控和正常对话不会关闭心跳。既支持 WebSocket ping/pong，也处理应用层 JSON ping/pong/正向云活动。

### 12.2 风险

- **P1-WS-01**：每个下行帧都执行 `malloc(payload_size + 1)` / `free`，最大帧约 16 KB。长时间 TTS 和控制消息混合时存在堆碎片风险。
- **P1-WS-02**：RX 任务直接调用文本/二进制回调；speaker 队列背压可在 RX 上阻塞最多约 80 ms，期间后续控制帧和 pong 处理被延迟。
- **P1-CLOUD-UI-01**：transport 连接后 `app_shell_set_bridge_status(true)`，而正式云允许不返回 hello ACK。代码另有 positive cloud activity 和 authenticated 标记，但 UI 的“云已连接”语义仍可能比应用鉴权更早。应明确区分 TCP/TLS/WS 在线与云业务可用。

## 13. 设备与服务端通信内容总表

### 13.1 设备上行

| 消息 | 作用 | 主要参数 |
|---|---|---|
| WebSocket Upgrade | 建立传输 | `X-Bridge-Token` |
| `hello` | 声明设备、功能、固件、音频能力 | `device_id`、`device_secret`、features、16 kHz/mono/20 ms、codec、AEC/ref |
| `ping` / WS ping | 应用和传输心跳 | 时间戳或固定 ping payload |
| `listen start/stop` | 开始/结束一个音频会话 | `mode`、`audio_session_id`、`request_id` |
| NA binary audio | 上传 PCM16/Opus 音频 | magic/version/flags/sequence/sample rate/channels/frame samples/payload |
| `barge_in` | 报告打断候选和确认 | state、event ID、父 turn、playback ID、onset/current sequence、confidence |
| `cancel_turn` | 显式取消当前云轮次 | request ID、turn ID、reason |
| `text_request` | 无麦克风场景的文本请求/诊断 | request ID、text |
| touch/input event | 上报触摸 | x/y/action/source |
| WiFi scan result/status | 配网和网络状态回报 | phase、SSID、RSSI、authmode、IP、ok、message |
| command ACK | 回应设备命令 | request ID、command、ok、message |
| meeting request/status | 开关会议及状态同步 | enabled、request/audio session ID、state/reason |
| monitoring status/ACK | 回应监控开关及状态 | enabled、request/audio session ID、state/reason |
| MCP response | JSON-RPC initialize/list/call 结果 | id、result 或 error |
| OTA status/progress/result | OTA 接受、下载、提交和重启状态 | update ID、版本、release sequence、bytes、hash、reason |

### 13.2 云下行

| 消息 | 设备行为 |
|---|---|
| `hello`/hello OK/error | 确认或拒绝应用 hello；正向消息可建立业务可用标记 |
| `pong` / WS ping | 更新心跳活动，WS ping 会立即回 pong |
| `device_command` | 状态、音量、表情、会议/监控、WiFi、诊断、OTA 等命令分派 |
| `mcp` | 交给 MCP JSON-RPC 处理器 |
| `asr_partial` / `asr_final` / `asr_result` | 更新文本、turn 和对话进度 watchdog |
| `audio_session` | 回报云端收到的帧数、时长和 RMS |
| `text_response` | 文本回复、语义表情和 TTS pending |
| `playback_begin` / `playback_end` | 绑定 turn/playback generation，驱动 speaking/listening |
| `tts_result` | TTS 成功/失败、帧数、采样率和播放边界 |
| binary PCM | 进入 speaker 队列并由 I2S 播放 |
| `cancel_turn_ack` | 确认取消旧轮次 |
| `input_event_ack` | 确认触摸等输入事件 |
| meeting/monitoring 控制与 ACK | 驱动对应模式状态机 |
| OTA command | manifest 获取、校验、下载和切换 |

### 13.3 已确认协议风险

- **P0-PROTO-01**：当前代码把同一个 `device_secret` 同时用于 WebSocket `X-Bridge-Token`（`main/nuwax_ai_partner_main.c:14288`、`main/ws_transport.c:538`）和 `hello.device_secret`（`main/nuwax_ai_partner_main.c:14325`、`main/session_protocol.c:216`）。`docs/protocol/README.md` 认可同值，但 `docs/protocol/meeting-monitoring-formal-cloud-complete-v1.md` 要求两者独立。正式服务端必须冻结唯一契约，否则设备端无法可靠判断 401/hello reject 的根因。
- **P1-DOC-01**：部分旧文档仍写 16 ms/512 B；当前代码唯一事实为 20 ms/320 samples/640 B PCM payload，必须清理文档漂移。
- 当前 NA 音频头为 22 B，所以 PCM16 单帧为 662 B；四帧 batch 最大 2,648 B。

## 14. 音频播放

speaker 使用独立任务、generation、播放队列、I2S owner、fade cancel 和结束回调。最近并发修改还加入了 JSON 播放元数据 start gate：PCM 首帧先按 generation 排队，只有匹配 `turn_id/playback_id` 的 `playback_begin` 通过 owner 后才允许 I2S 播放；打断只停止匹配 generation 的播放，避免迟到的旧事件停止新一轮 TTS。队列写入有 80 ms 背压预算，缺少 END 时上层有有界恢复。

新增 gate 的条件风险：

- **P1-SPEAKER-01**：start gate 默认等待约 2 秒；若正式云先下发二进制 TTS、丢失 `playback_begin`、或 turn/playback ID 不一致，设备会拒绝该 generation、清理对话并请求关闭 WebSocket。该策略可以防止“无归属音频直接播放”，但正式云必须冻结并保证 `playback_begin -> PCM -> playback_end` 的顺序和 ID 一致性；否则单个控制帧丢失会扩大为整条云连接重建。
- 10:24 构建已包含当前 speaker gate 源码，但仍必须做“控制帧延迟/丢失/乱序”实机和正式云测试后才能判定体验。
- **P1-RTOS-04**：speaker 的启动授权、I/O claim、停播 join、cancel claim、fade 和 runtime reset 共 9 处可能使用小于一个 tick 的延时。当前 100 Hz 下 1 ms 参数为 0，实际只是 yield，不会阻塞 1 ms；故障并发时会高频轮询，并可能让同核较低优先级的资源 owner 无法运行直至超时。应改为任务通知/信号量驱动；临时轮询至少要显式使用一 tick，并重新评估 10 ms 粒度对打断延迟的影响。

资源问题：

- speaker 队列深度 128。
- `speaker_playback_packet_t` 当前 ELF 中为 1,952 B，因为每个 slot 固定保留 `VOICE_MAX_SAMPLES_PER_FRAME=960`，即使正式云每帧只有 320 samples。
- 队列因此占约 249,856 B PSRAM，其中约三分之二的 sample 容量在 20 ms 产品配置下不会使用。
- **P2-MEM-01**：可以在后续优化中改为 320-sample 产品 packet 或分离 metadata + PCM pool，在保持 128 帧时释放约 160 KB PSRAM；本轮不改代码。

## 15. OTA

OTA 不是占位：已实现 HTTPS 下载、SHA-256、项目/芯片/板型/分区 schema、严格 SemVer、release sequence、双 OTA 分区、pending verify、启动健康门和 rollback。

安全缺口：

- `CONFIG_SECURE_BOOT` 未启用。
- `CONFIG_FLASH_ENCRYPTION_ENABLED` 未启用。
- manifest 当前没有设备端验证的数字签名字段，仅靠 HTTPS 和 SHA-256 不能覆盖发布服务器或凭据泄漏后的恶意合法下载。
- 因此当前是“具备可靠升级和回滚”，不是“完整量产供应链安全”。

## 16. MCP

当前 MCP 支持 JSON-RPC `initialize`、`tools/list` 和 `tools/call`，仅注册：

1. `self.get_device_status`
2. `self.ui.set_emotion`
3. `self.servo.head_shake`

其中舵机回调尚未配置，实际返回失败。没有音量、会议、监控、WiFi、OTA 等设备控制工具。

- **P1-MCP-01**：`hello.features.mcp=true` 的能力声明过宽，云端可能认为设备核心控制已经 MCP 化。
- 产品实现应新增具备参数 schema、幂等 request ID、权限、危险操作确认、状态回读和 ACK 的工具，而不是让模型直接拼接内部 `device_command`。

## 17. 静态内存与 Flash 资源

### 17.1 链接结果

| 区域 | 已用 | 总量 | 剩余 | 说明 |
|---|---:|---:|---:|---|
| Flash Code | 1,784,636 B | - | - | `.text` |
| Flash Data | 1,592,556 B | - | - | 主要为 `.rodata` 和 UI 资源 |
| DIRAM | 230,047 B | 341,760 B | 111,713 B | 链接期静态占用 67.31% |
| IRAM | 16,384 B | 16,384 B | 0 B | 当前链接区域满，不等于运行必崩，但不可随意新增 IRAM_ATTR/ISR 代码 |
| RTC FAST | 80 B | 8,192 B | 8,112 B | 余量充足 |
| RTC SLOW | 44 B | 8,192 B | 8,148 B | 余量充足 |
| OTA app slot | 3,528,032 B BIN | 4,980,736 B | 1,452,704 B | 固件槽余量约 29.17% |

主要 Flash 消耗包括三张约 259 KB 的全屏 RGB565 资源、face diff path、主应用和协议/网络组件。新的整套 UI 可以替换旧资源，但如果只叠加而不删除旧资源，Flash 余量会快速收窄。

### 17.2 主要运行期动态资源

| 资源 | 位置 | 当前规模 | 生命周期/风险 |
|---|---|---:|---|
| 表情缓存 | PSRAM | 4 x 259,200 = 1,036,800 B | 常驻，最大单项之一 |
| 连续对话预卷 | PSRAM | 256 x 2,772 = 709,632 B | 常驻，保存 5.12 秒和四路 PCM/元数据 |
| speaker 队列 | PSRAM | 128 x 1,952 = 249,856 B | 常驻，slot 按 960 samples 过度预留 |
| 会议/监控可靠上行 pool | PSRAM | 128 x 2,648 = 338,944 B | 模式活动时，最多 10.24 秒 |
| 正常对话实时上行 pool | PSRAM | 12 x 2,648 = 31,776 B | 会话活动时，最大包龄 120 ms |
| 正常对话实时采集 pool | PSRAM | 约 64 KB | 16 slots；metadata、5 路 PCM、5 个 drop buffer、6 KB 栈及队列/context |
| 软件播放参考环形区 | PSRAM | 32,000 x int16，约 64 KB | 播放期 AEC software reference |
| WebSocket RX payload | 默认 heap/PSRAM | 每帧 0~约 16 KB | 每帧 malloc/free，碎片风险 |
| LVGL draw buffer | internal 或 PSRAM | 17,280 B 单 internal；fallback 为 34,560 B PSRAM | internal 快，PSRAM 明显降低动画帧率 |
| display DMA buffers | internal DMA | 15,184 B | 常驻，必须保留连续 DMA 块 |
| wake task stack | PSRAM | 8 KB | 休眠监听时 |
| WebSocket RX stack | PSRAM | 12 KB | 连接期间 |
| touch gesture stack | PSRAM | 16 KB | 常驻 |
| P3 softref A/B stack | PSRAM | 8 KB | 当前产品配置关闭相关 prototype，但任务仍常驻阻塞 |
| P5 candidate stack | PSRAM | 24 KB | 常驻诊断/候选任务 |
| uplink sender stack | PSRAM | 10 KB | 音频会话期间，CPU1 |
| RT capture stack | PSRAM | 6 KB | 正常对话采集期间，CPU0 |
| OTA controller stack | 默认内部任务栈 | 16 KB | OTA controller 常驻；需要 high-water 证明 |

注意：预卷 709,632 B 是 10:24 的 7.0.21 ELF 由 GDB `sizeof` 得到。旧 256-sample 日志中的 542,720 B 已不适用于当前布局。

### 17.3 当前内存安全判断

仅凭 8 MiB PSRAM 和链接期 111 KB DIRAM 余量，不能证明运行安全。最近可用的旧 7.0.20 实机日志 `runtime-logs/nuwax7-v2-720-live-aec-barge-sleep-20260731-195231.log` 在播放期记录：

```text
internal free       约 11~12 KB
internal minimum    35 B
internal largest    6,400 B
DMA free            约 6.8~7.8 KB
DMA largest         6,400 B
PSRAM free          约 1.84~1.86 MB
PSRAM minimum       约 1.44 MB
```

该日志属于旧 7.0.20，且当时正常上行队列为异常的 96 帧；不能直接宣判 7.0.21 仍然相同。但内部 minimum=35 B 是明确的历史近 OOM 证据，当前版本没有实机反证前必须作为发布阻断项。

- **P0-MEM-01**：7.0.21 必须在最坏并发场景证明 internal/DMA/PSRAM minimum、largest block 和任务栈余量。任何单次启动“能工作”都不构成内存验收。
- **P1-MEM-02**：四个全屏 face cache、5.12 秒四路预卷、speaker 过宽 slot 和可靠上行同时存在，虽总量可容纳，但大块 PSRAM 的分配顺序会影响最大连续块和菜单 snapshot 成功率。
- **P1-MEM-03**：WebSocket payload、LVGL snapshot、配网 Security 2、WiFi 和 OTA TLS 都有短时峰值，必须覆盖叠加场景。
- **P2-MEM-TASK-01**：当前产品配置关闭 P3 AFE prototype 和 P5 overlap probe，但启动仍无条件创建 `p3_softref_ab` 8 KB、`p5_candidate` 24 KB 两个永久 PSRAM 栈及请求队列。约 32 KB 栈加 TCB/队列开销长期闲置，应按生产 Kconfig 裁剪或改为按需 worker。

## 18. 实时调度与任务资源

### 18.1 内核配置

当前是 ESP32-S3 双核、ESP-IDF 非 SMP FreeRTOS，`CONFIG_FREERTOS_HZ=100`，即一个 tick 为 10 ms。栈 canary 已启用，但编译器栈保护关闭；任务 WDT 为 10 秒并监视两个 idle task，`CONFIG_ESP_TASK_WDT_PANIC` 未启用。Queue registry 为 0，trace facility 和 runtime stats 均关闭。

这套配置能发现部分栈越界和 idle starvation，但不能自动证明业务任务健康，也无法直接给出各核占用、任务运行时间和队列峰值。20 ms 音频帧只有两个 OS tick，音频截止时间必须继续使用 I2S 超时和 `esp_timer_get_time()` 等微秒时钟，不能用 tick 粒度推导 DSP 余量。

### 18.2 主要任务拓扑

| 任务 | 优先级 | 栈 | 运行位置/生命周期 | 评估 |
|---|---:|---:|---|---|
| `nuwax_mic_rx` 实时采集 | 12 | 6,144 B PSRAM | CPU0；正常对话活动期 | 最高应用优先级，合理保护采集 |
| `app_main` / 主 DSP | 11 | 16,384 B internal | CPU0；常驻 | 承担状态机和 DSP，职责过重 |
| WebSocket RX | 10 | 12,288 B PSRAM | unpinned；连接期 | 直接执行回调，可能被 speaker 背压拖慢 |
| speaker playback | 10 | 6,144 B PSRAM | unpinned；常驻 | 与 WS RX 同优先级，core 落点不确定 |
| audio uplink TX | 9 | 10,240 B PSRAM | CPU1；音频流活动期 | 网络发送与 CPU0 采集隔离，方向正确 |
| barge dispatcher | 8 | 6,144 B PSRAM | unpinned；常驻 | 异步提交打断协议，避免阻塞 DSP |
| flash operations | 6 | 4,096 B internal | 常驻 | internal 栈符合 cache/flash 操作要求 |
| OTA controller | 6 | 16,384 B internal | 常驻 | 栈较大，当前 watermark 计算错误 |
| LVGL | 5 | 8,192 B | CPU1；常驻 | internal 不足时退 PSRAM，帧率受内存位置影响 |
| touch raw | 5 | 4,096 B PSRAM | 常驻 | 与 UI 分层 |
| touch gesture | 5 | 16,384 B PSRAM | 常驻 | 栈明显偏大，需 high-water 证据 |
| `p3_softref_ab` | 5 | 8,192 B PSRAM | unpinned；常驻阻塞 | 产品关闭诊断特性后仍创建 |
| `p5_candidate` | 5 | 24,576 B PSRAM | unpinned；常驻阻塞 | 产品关闭诊断特性后仍创建 |
| WiFi fallback watcher | 5 | 2,048 B dynamic | 恢复期 | 与 WiFi manager 协作 |
| WiFi scan/connect | 4 | 8,192/4,096 B PSRAM | 请求驱动 | 不应进入 20 ms 热路径 |
| wake word | 4 | 8,192 B PSRAM | 休眠监听期 | 静态 TCB/栈自删除生命周期有竞态 |
| battery/board/power | 4 | 4,096/2,560/3,072 B | 常驻 | 低速外设任务 |
| shell/UI sync | 3 | 4,096 B PSRAM | CPU1；常驻 | mailbox 同步 |
| USB provisioning | 3 | 3,072 B PSRAM | 常驻/控制期 | 与重配网互斥 |
| face preload | 2 | 4,096 B PSRAM | core-pinned；按需 | 低优先级后台加载 |

采集 12 高于主 DSP 11，且 uplink 固定 CPU1，是当前最合理的实时隔离。风险在于 CPU0 同时承载 WiFi 和主状态机/DSP；speaker、WS RX 又是 unpinned 的同优先级任务，调度落点会随阻塞状态变化。最终必须用设备端分核利用率和每阶段 p95/p99 证明，而不能仅凭优先级表判断实时性。

### 18.3 任务生命周期

实时采集和 audio uplink 使用“任务完成后自挂起，owner 外部 suspend、确认非 running、再 `vTaskDelete()`”的模式。ESP-IDF 6.0 对非当前运行任务的删除会同步释放 TCB；因此这两处在当前调用顺序下可以随后复用自有静态 TCB/栈，不应继续误报为静态 TCB use-after-free。

- **P1-RTOS-01**：wake word 路径不同。任务在 `wake_word_detector.c:344-351` 先发布 `running=false`、`task=NULL`，解锁后才 `vTaskDelete(NULL)` 自删除；`stop()` 只等待 `running=false`，`deinit()` 随即可以释放静态 TCB/栈。另一个 start/deinit 调用者因此可能在 idle 清理旧任务前复用或释放同一存储。通常较长的模式切换间隔会掩盖此竞态，但 API 生命周期本身不安全。应由 owner 持有有效 handle，采用退出事件/信号量完成所有权移交，再由 owner 删除和释放；任意固定 delay 不能作为同步契约。
- WebSocket RX 已改为只报告退出并挂起，连接 owner 等待、删除任务且唯一销毁 transport；旧版“RX 自毁 TLS、owner 强删后再次销毁”的死锁问题当前不存在。
- 对主要 critical section 的扫描未发现持 spinlock 执行分配、网络 I/O、日志或阻塞等待；OTA 在 spinlock 内调用的 shutdown inhibit 只是原子 CAS，没有确认锁反转。

### 18.4 tick、等待和优先级风险

- **P1-RTOS-04**：`speaker_playback.c` 有 8 个固定 1 ms 等待和 1 个可退化到 1 ms 的 fade step，100 Hz 下均可能折算为 0 tick，覆盖 packet I/O claim、START admission、joined stop、cancel、fade、runtime reset 和 START metadata gate。零 tick 只让出当前执行机会，不提供定时阻塞；并发异常时会高频轮询，且可能让同核较低优先级 owner 饥饿至超时。产品修复应使用 task notification/event/semaphore 表达所有权变化；若临时改为一 tick，需接受并验证每次最多约 10 ms 的唤醒粒度。
- **P1-RTOS-05**：触摸 RST 低脉宽和 ES7210 clocked stop/start 间隔都写为 `pdMS_TO_TICKS(5)`，当前同样为 0 tick。前者可能导致触摸 IC 未完成硬复位，后者可能降低 codec 重启可靠性。这是硬件时序错误，不应以普通 `vTaskDelay()` 轮询策略修复；应按器件规格使用确定的最小延时并做示波器/逻辑分析仪验证。
- 20 ms frame budget 是硬截止期。旧日志的 stale capture 和 sequence gap 说明平均值不足以验收；日志格式化、状态卡刷新、命令分派和统计汇总必须移出采集/DSP 热路径。
- PSRAM task stack 节约 internal RAM，但 cache 不可用路径不能依赖 PSRAM；Flash/OTA 相关任务使用 internal 栈的方向正确。

### 18.5 看门狗与可观测性

- **P1-RTOS-02**：OTA `log_controller_stack_watermark()` 把 `uxTaskGetStackHighWaterMark()` 再乘 `sizeof(StackType_t)`。ESP-IDF 6.0 的 API 已返回字节，当前日志和 2,048 B 告警阈值因此被放大 4 倍，可能漏报 OTA controller 栈临界。
- **P1-RTOS-03**：TWDT 只监视两个 idle task，10 秒超时仅记录而不 panic/reset；没有音频、网络、UI、OTA 等应用任务心跳 supervisor。当前能发现严重 CPU 饥饿，却不能保证单个 worker 卡死后受控恢复。量产策略需在故障注入后决定“记录、局部重建还是重启”，不能直接打开 panic 后交付。
- **P1-RTOS-OBS-01**：queue registry、trace 和 runtime stats 全关闭，主要任务也没有统一 stack/CPU/queue 采样。现有 conversation metrics 有价值但覆盖不全。验收至少需要所有主要任务 stack minimum、分核 CPU、队列 high-water、speaker/WS 回调最大耗时及 DSP 分阶段 p50/p95/p99。

### 18.6 代码组织风险

- **P2-CODE-01**：`nuwax_ai_partner_main.c` 约 15,000 行、约 696 KB，同时持有会话 owner、模式仲裁、网络回调、诊断、采集/DSP、配网和恢复逻辑。它不是当前某个已确认崩溃的直接原因，但显著扩大并发修改的回归面。后续应按 conversation owner、mode arbiter、network session、diagnostics 和 recovery supervisor 分模块，先冻结接口和事件所有权，再逐步迁移，避免一次性重构。

## 19. 已修复、不应重复列为现存 Bug 的项目

以下问题在当前源码已经处理，后续评审不得沿用旧报告直接判定仍存在：

- 监控已有独立 UI 状态和 mailbox。
- 会议、监控强制显示 listening，错误显示 error。
- `IDLE -> READY`，月亮只对应 `SLEEPING`。
- WiFi/云配置使用 stage、GOT_IP、commit/discard。
- WebSocket 由连接 owner 唯一关闭，RX 不销毁 TLS。
- 触摸 I2C 失败会投递 cancelled 并清理手势。
- TTS END 缺失有播放/drain 超时恢复。
- AEC 320/256 已使用并行 FIFO、metadata/provenance 和 flush。
- 正常对话上行默认已从临时 96 帧恢复为 12 帧、120 ms age budget。
- 监控已改为 ARMED 本地检测、确认人声后 STREAMING、180 秒静音 stop、下一次人声新建轮次。
- 10:24 构建覆盖当前 `main/` 源码，旧 `P0-BUILD-01` 已关闭。
- 实时采集和 audio uplink 的外部删除在 ESP-IDF 6.0 下同步回收非运行任务，不是当前静态 TCB 竞态；真正需要修复的是 wake word 自删除路径。

这些修复仍需要回归测试，但不能再作为“代码尚未实现”描述。

## 20. 问题清单与优先级

| ID | 等级 | 问题 | 影响 | 当前证据 |
|---|---|---|---|---|
| P0-PROTO-01 | P0 | Upgrade token 与 hello secret 契约冲突 | 正式云鉴权失败/行为不一致 | 源码 + 两份文档冲突 |
| P0-MEM-01 | P0 | 7.0.21 无峰值内存实机证明；旧版 internal min=35 B | 随机分配失败、重启、TLS/I2S 异常 | 旧实机日志，当前待反证 |
| P0-BARGE-01 | P0 | 最新固件真人打断未闭环验收 | 无法打断或上传设备声 | 代码存在，D 级证据缺失 |
| P0-RT-01 | P0 | 最新固件 20 ms DSP p99 未证明 | stale/gap、句首丢失、AEC 失配 | 旧实机风险，当前待测 |
| P1-MONITOR-01 | P1 | 已修复：ARMED 本地检测，确认人声开流，180 秒静音结束单轮 | 实机阈值或云端重开流兼容性仍待验收 | Host Check + ESP-IDF 6.0 构建通过 |
| P1-UPLINK-01 | P1 | 可靠队列可积压 10.24 秒 | 上传陈旧会议/监控音频 | 当前配置计算 |
| P1-MEETING-01 | P1 | 停播 500 ms 超时后会议仍开流 | 首帧混入设备 TTS | 当前源码确认 |
| P1-AEC-01 | P1 | 硬件参考未有当前实机证据 | 只能依赖软件参考 | 旧日志 `hw_ref_frames=0` |
| P1-BARGE-02 | P1 | 真人检测阈值只有代码配置，无多距离/噪声召回率 | 弱声打断失败或设备声误触发 | 当前算法 + D 级证据缺失 |
| P1-SPEAKER-01 | P1 | TTS start gate 依赖 playback_begin 顺序，超时会关云 | 控制帧丢失被放大为连接重建 | 10:24 构建已包含，正式云故障注入待验收 |
| P1-WS-01 | P1 | WebSocket RX 每帧动态分配 | 长时堆碎片 | 当前源码确认 |
| P1-WS-02 | P1 | RX 回调承担 speaker 背压 | 控制帧/pong 延迟 | 当前源码确认 |
| P1-RTOS-01 | P1 | wake word 在自删除前先发布 stopped/null handle | 静态 TCB/栈可能在 idle cleanup 前被复用或释放 | 源码 + ESP-IDF 6.0 删除语义 |
| P1-RTOS-02 | P1 | OTA stack watermark 已是字节却再次乘 4 | 栈余量虚高、告警失效 | 源码 + ESP-IDF 6.0 API 文档 |
| P1-RTOS-03 | P1 | TWDT 不 panic，且没有应用任务 health supervisor | worker 卡死可能只留日志、不自动恢复 | sdkconfig + 任务注册检查 |
| P1-RTOS-04 | P1 | speaker 小于 10 ms 的等待在 100 Hz 下退化为零 tick | 高频自旋、同核 owner 饥饿、错误超时 | 9 个调用点 + tick 配置 |
| P1-RTOS-05 | P1 | touch/ES7210 的 5 ms 硬件间隔退化为零 tick | 触摸复位或 codec 重启不可靠 | 两个调用点 + tick 宏定义 |
| P1-RTOS-OBS-01 | P1 | 缺少统一 CPU/stack/queue/lock/DSP 观测 | 无法证明实时预算和栈安全 | sdkconfig + 日志接口检查 |
| P1-MCP-01 | P1 | MCP 声明大于实际工具能力 | 云模型误判可控能力 | 当前源码确认 |
| P1-SEC-01 | P1 | 固定开发 PoP；Secure Boot/Flash Encryption/manifest 签名缺失 | 量产安全不足 | sdkconfig + OTA schema |
| P1-MEM-02 | P1 | 多个大块 PSRAM 常驻/并发 | 最大连续块下降 | ELF + 源码计算 |
| P1-MEM-03 | P1 | TLS、snapshot、配网、OTA 峰值可能叠加 | internal/DMA 瞬时分配失败 | 生命周期与分配路径 |
| P1-CLOUD-UI-01 | P1 | 传输在线和云业务在线语义可能提前合并 | UI 假在线 | 当前连接流程 |
| P1-DOC-01 | P1 | 16 ms 旧文档与 20 ms 当前事实源漂移 | 云端按错误帧长接入 | 文档/源码对比 |
| P2-UI-01 | P2 | READY 复用 recognizing 图标 | 状态辨识弱 | 当前 UI 映射 |
| P2-UI-02 | P2 | 菜单缺少监控开关 | 本机不可直接控制监控 | 当前菜单源码 |
| P2-WAKE-01 | P2 | 唤醒初始化失败不完整 rollback | 异常重试和资源释放不确定 | 当前源码确认 |
| P2-MEM-01 | P2 | speaker slot 按 960 而非 320 samples | 约 160 KB PSRAM 浪费 | ELF `sizeof` + 配置 |
| P2-MEM-TASK-01 | P2 | 已关闭诊断功能仍常驻 32 KB 任务栈和队列 | PSRAM 常驻浪费、任务拓扑膨胀 | 启动路径 + Kconfig |
| P2-CODE-01 | P2 | 主文件约 15,000 行并拥有多个并发域 | 修改回归面大、所有权难审计 | 文件规模与职责扫描 |
| P2-BOARD-01 | P2 | V2 工程仍使用 `ESP_VOCAT_V1_0` 兼容配置名 | 维护和交付易误解 | sdkconfig |
| P2-DEAD-01 | P2 | `audio_reliable_playback.c` 未进入主 CMake | 实验/死代码漂移 | 构建清单检查 |

## 21. 产品级验收矩阵

| 场景 | 必须采集的证据 | 通过标准 |
|---|---|---|
| 当前源码构建 | 10:24 BIN/ELF、size、SHA；补跑全部 Host Check | 构建已通过且源码时间一致；全部逻辑检查需补齐后冻结回归基线 |
| 冷启动 | 完整 boot、heap、stack、UI 状态 | 无 ERROR；最终状态与网络一致 |
| 唤醒词 | 距离/噪声/播放期 1000 次统计 | 达到产品定义漏唤醒和误唤醒率 |
| 长按唤醒 | 按下、保持、释放、I2C 故障注入 | 不重启；进入 listening；两分钟策略正确 |
| 连续对话 | 100 轮不打断对话 | 每次播放结束立即恢复聆听，无需重新唤醒 |
| 真人打断 | 多音量、多距离、不同 TTS 内容 | candidate/confirmed/停播/新 ASR 全闭环，不上传设备声 |
| 无人播放基线 | 设备连续播放但无真人 | 0 误打断、0 设备自说自话循环 |
| 音频实时性 | 每阶段 p50/p95/p99、stale、gap | p99 < 20 ms；stale/gap=0 |
| 硬件参考 | 主麦/MIC3/软件参考原始 PCM、相关性、ERLE | `hw_ref_frames>0` 且映射、延迟、ERLE 达标 |
| 会议 | 30 分钟/2 小时、弱网、静音、切模式 | 参数正确、无旧音频回放、状态 ACK 一致 |
| 监控 | ARMED/人声/2~3 分钟静音/再说话 | ARMED 不上传；人声带预卷开流；180 秒 stop；新人声新 session；正式云 ACK 正确 |
| 云断线 | 正常/播放/会议/监控中断云 | 不死锁、不假在线、自动重连或明确错误 |
| WiFi 断线 | AP 重启、漫游、密码错误 | 恢复旧配置并重连云，不永久卡住 |
| BLE 重配网 | 成功、失败、断电注入、重复操作 | 原配置不被半提交，新配置只在 GOT_IP 后提交 |
| OTA | 正常、断电、错 hash、降级、错误板型、首启失败 | 拒绝非法包；可回滚；状态可观测 |
| 内存压力 | 对话+TTS+菜单、配网、OTA、模式切换各 2 小时 | 无分配失败；minimum/largest/stack 满足预算 |
| RTOS 故障恢复 | 卡住音频/WS/UI worker、延迟 owner、反复启停 wake | 无自旋超时和 TCB 复用；故障被 supervisor 识别并按策略恢复 |
| 硬件短时序 | 示波器测 touch RST 和 ES7210 stop/start | 脉宽/间隔满足器件规格，连续 1000 次初始化无 probe/codec 失败 |
| UI | 所有状态和模式录像 | 月亮仅休眠；listening/speaking/meeting/monitoring 同步 |

## 22. 运行期必须记录的资源指标

当前已有部分 heap checkpoint 和 conversation metrics，最终产品验收至少应每 30~60 秒及每次模式切换记录：

```text
internal free / minimum / largest
DMA free / minimum / largest
PSRAM free / minimum / largest
所有主要任务 stack high-water
capture/uplink/speaker/control 队列当前值和 high-water
DSP 总体和 AEC/human/cleaner/preroll/encode/enqueue p50/p95/p99
stale capture / sequence gaps / speaker underrun
AEC fallback / reference kind / ref health / ERLE
WebSocket reconnect 原因、RX frame 分配失败和 callback 最大耗时
LVGL render/flush p95、snapshot 分配失败
```

建议的量产门槛必须由团队按功能峰值冻结。就当前历史风险而言，至少要确保最坏场景仍有可分配的内部/DMA 连续块，且 minimum 不能接近 0；只设“总 free 大于某值”不够。

## 23. 后续处理顺序

1. 以 10:24 构建为当前编译基线，补跑 conversation/product 全部 Host Check，冻结 BIN SHA 和 size。
2. 正式云端冻结 Upgrade token、hello secret、20 ms 音频头和事件顺序，消除 P0-PROTO-01。
3. 修复 wake task 生命周期、OTA stack watermark、speaker 零 tick 轮询及 touch/ES7210 短时序；补充最小可用的任务健康和资源观测。
4. 用当前固件烧录后先做资源和分阶段实时基线，不先调打断阈值；确认 P0-MEM-01 和 P0-RT-01。
5. 通过无人播放基线，再做真人打断闭环和硬件参考原始 PCM 采样。
6. 对已实现的监控 ARMED/STREAMING 做正式云和远场弱声验收，并给可靠队列增加包龄策略。
7. 解耦 WebSocket RX 与 speaker 入队，改用可复用 payload buffer/pool。
8. 裁剪生产配置下 P3/P5 诊断 worker，并按 high-water 收缩过宽任务栈。
9. 补齐 MCP 工具和真实能力声明，完成量产安全方案。
10. 最后处理 READY 独立图标、设备监控开关和主文件模块化。

## 24. 本轮结论

当前源码的主架构方向正确，正常对话循环、状态图标规则、WebSocket owner、配网原子提交、监控静音分轮和 AEC 320/256 适配已经形成代码闭环。10:24 构建也已覆盖当前业务源码。现阶段最危险的不是“完全没有实现”，而是若干关键能力只有代码/Host 证据，没有与当前固件相同的实机证据；同时旧版日志暴露过极低内部 RAM 水位，RTOS 仍有任务生命周期、零 tick 轮询、错误栈观测和故障监督缺口。

因此当前合理结论是：**功能实现度高且当前源码可构建，但正式云契约、内存/实时预算、真人打断、硬件参考、监控实机效果和 RTOS 故障恢复仍是发布前必须关闭的问题。**
