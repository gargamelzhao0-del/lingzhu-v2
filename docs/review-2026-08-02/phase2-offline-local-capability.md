# 阶段 2 A2-06：Wi-Fi/云离线时本地能力 A 级静态审计

审计日期：2026-08-03  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
工作项：`review-work-item-index.csv` 的 `A2-06`  
授权：A 级纯静态源码/配置审计；只写评审证据、索引和执行记录。未修改业务源码、配置、正式协议或资源；未构建、未运行 host/gateway 测试、未访问 COM5、未读写 NVS/eFuse、未 OTA/烧录。

## 1. 结论先行

1. SW3、电源保持任务、LVGL/UI task、touch driver/gesture task 和 battery service 在任务创建顺序上位于网络连接之前。只要它们自身初始化成功，后续 Wi-Fi/云端故障不会因为 `app_main` 的同步重连而停止 FreeRTOS 调度；这是当前实现的正向解耦。
2. “云端死掉不阻塞 UI、电源和本地能力”的产品验收不成立。首次 Wi-Fi init/connect 失败会直接从 `app_main()` 返回；wake model、Wi-Fi 菜单 worker、speaker worker 和长期本地/云控制循环均未创建。屏幕和部分触摸仍可运行，但不是完整的本地可用状态。
3. 首次联网成功后，运行期 WebSocket 断开会显式停止睡眠态 wake detector，再进入同步 Wi-Fi/WS 重连循环。只有重新连接后才调用 `enter_sleeping_conversation_state()` 并重新启动 wake detector。因此离线期间本地唤醒和 conversation sleep/awake 状态机不可用。这是 `OFFLINE-001` P1；它扩充但不替代启动 readiness 的 `START-002`。
4. `s_shell_actions` 只有 WebSocket 已连接的音频主循环会消费。离线重连期间，除 BLE 重配网的直接 callback 外，touch 产生的 listen、volume commit、Wi-Fi、meeting 和状态请求会积压；断线 purge 不清该队列，也没有 timestamp/epoch/generation。连接恢复后可重放陈旧本地动作，队列满后还会丢新动作。会议 enable 的迟到执行会扩大持续采集同意风险。这是 `OFFLINE-002` P1，并交叉引用 `PRIV-002`。
5. Wi-Fi connect 有 30 s wait，主循环失败后固定等待 3 s；WSS connect 配置 15 s timeout。它们不会形成当前代码可见的紧循环，但没有统一的 resolver/TCP/TLS/cleanup 总 deadline、指数退避或 jitter；`ws://` 的同步 DNS/blocking connect 仍沿用 `NET-IO-001`，WebSocket 锁/worker 健康沿用 `NET-CONC-001/LIFE-009/RTOS-001`。本轮没有动态证明所有 stall 都能恢复。
6. transport/auth/service-ready 已拆分，云端 101 后先显示 connecting，收到正向业务活动才显示 connected；这是正确方向。但它不能补偿本地 capability owner 被网络生命周期控制的问题。

因此：A2-06 的 A 级静态审计完成；产品验收未通过；B/D 的断网、云死、动作交错、资源水位和恢复测试仍开放。

## 2. 审计边界与主要证据

| 主题 | 当前证据 |
|---|---|
| 启动顺序 | `main/nuwax_ai_partner_main.c:15537-16170` |
| 长期 Wi-Fi/WS 循环 | `main/nuwax_ai_partner_main.c:16172-16427` |
| sleep/wake owner | `main/nuwax_ai_partner_main.c:2291-2369,5641-5744` |
| shell/UI 与 touch | `main/app_shell.c:174-218,449-461,538-590,699-720,777-916`；`main/conversation/touch_gesture.c:119-136,231-562` |
| shell action 消费 | `main/nuwax_ai_partner_main.c:5336-5368,5641-5771,14592-14602`；`main/conversation/shell_action_dispatch.c:19-89` |
| SW3 | `main/board/nuwax_power_ctrl.c:264-375,401-472` |
| Wi-Fi state/retry | `main/wifi_manager.c:246-291,300-475,477-601` |
| WS deadline/lifecycle | `main/ws_transport.c:43-55,395-445,657-760,1094-1144,1159-1358,1410-1549,1664-1744` |
| cloud 三态 | `main/nuwax_ai_partner_main.c:3568-3693` |
| 当前配置 | `sdkconfig:1160,1172,1320,1325,1336,1399,1443` |

本文件只证明静态可达路径。没有关闭 AP、DNS、TLS、WebSocket 或正式云端，没有观察 task/heap/CPU 水位，也没有触碰设备。

## 3. 启动时的离线能力边界

### 3.1 网络前已创建或启动

按当前产品分支，网络之前已经发生：

- `nuwax_power_ctrl_init()` 创建独立 SW3 task；
- speaker manager、live AEC pool、meeting/monitoring controller 和 flash-ops worker 已初始化；
- display、LVGL、app shell、battery service、shell UI task、touch log/gesture task 已启动；
- BLE reprovision 的 touch callback 已直接绑定到 flash-ops worker。

这些 task 不依赖 `nuwax_ws_connect()` 返回才获得 CPU。网络调用阻塞 `app_main` 所在 task 时，scheduler 仍可运行它们。

### 3.2 网络成功后才创建或激活

`nuwax_wifi_connect()` 成功之后才执行：

- `nuwax_wake_word_detector_init()`；
- `wifi_scan_task`、`wifi_connect_task`；
- P3/P5 worker；
- `nuwax_speaker_playback_task`；
- 持久 WebSocket owner 和 shell/session/audio 主循环。

因此以下失败 cut 都不是完整 offline-ready：

| cut | 当前动作 | 仍可能运行 | 明确缺失/失效 |
|---|---|---|---|
| `nuwax_wifi_init()` 失败 | `ERROR` 后 `app_main` return | SW3、成功初始化的 UI/touch/battery、flash worker | wake、speaker worker、Wi-Fi menu worker、主 action consumer、网络恢复 owner |
| BLE 首配网失败且不可回滚 | `ERROR` 后 return | 同上 | 同上；没有持续离线/重试状态 |
| 首次 Wi-Fi connect 超时/失败 | `ERROR` 后 return | UI/touch/SW3 等前置 task | wake/model、speaker worker、长期恢复循环都未建立 |
| 首次 Wi-Fi 成功、cloud config 缺失 | 进入长期循环，每 3 s retry | 全部后置 task 已创建 | 云业务不可用；wake 要等一次 WS 成功进入 sleeping 才启动 |
| DNS/TLS/101 失败 | 同步 connect 返回后固定 3 s retry | UI/touch/SW3 task 可继续调度 | wake 未启动；shell actions 无 consumer |

代码日志 `Wi-Fi init failed before UI` 与真实顺序不符：UI 已在 `:15756-15829` 初始化，Wi-Fi init 在 `:15984-15991`。该文案会误导现场诊断，但本轮把它作为 `START-002/OFFLINE-001` 的可观测性补充，不另建 ID。

## 4. 运行期断网/云死状态机

### 4.1 正常在线睡眠

WebSocket 成功后，主循环调用 `enter_sleeping_conversation_state("boot_ready")`。该函数清理 awake/prelisten、发布 `SLEEPING` UI 并启动 wake detector。wake callback 只向深度 2 的 `s_wake_word_events` 投递；主循环在 `wait_for_mic_start_action()` 中消费并向云端发送 greeting。

因此 wake detection 本身是本地计算，但它的启动/停止 owner 和事件 consumer 都由在线 audio loop 控制。

### 4.2 已连接后断开

断开清理顺序为：

```text
notify meeting/monitoring disconnected
preserve or clear awake session
unbind dispatcher / unpublish ws
owner close / cloud disconnected
stop_sleep_wake_word_detector("ws_disconnect")
speaker runtime reset / connection-bound queue purge
optional Wi-Fi recover
app_state = WS_CONNECTING
fixed 3 s delay -> reconnect
```

`discard_connection_bound_queues()` 只清 session、P3、P5 和 speaker owner queues，不清 `s_shell_actions`。重连失败路径也从不调用 `drain_shell_actions()`。只有新 WebSocket 建立后进入 `run_audio_source()`/`wait_for_cloud_updates()` 才恢复 action 消费。

### 4.3 云静默但 socket 尚未判死

RX task 每 25 s 发 heartbeat，60 s 无 RX 活动后退出；应用层也有 activity timeout。socket 仍被判 connected 的窗口内，wake detector 可以检测并把事件交给主循环，但 greeting/对话依赖云端响应。连接被判死后立即进入上一节的 wake-stop/reconnect 状态。

静态代码不能证明 peer half-open、TLS destroy、DNS resolver 或 TX stall 的实际最坏恢复时间。当前 close/join/lock 已使用 50 ms、5 s 等有限锁等待，这是比无界 lifecycle wait 更好的控制；但 `esp_tls_conn_destroy()` 自身位于锁内，`ws://` 在 socket timeout 前做同步 DNS/blocking connect，仍必须由阶段 5 和 D 级故障注入关闭。

## 5. 本地能力矩阵

| 能力 | 首次联网前/首次联网失败 | 已联网后云端断开 | 静态判定 |
|---|---|---|---|
| SW3 点按关机 | 独立 priority 4 task 可运行 | 可运行 | 与网络解耦；系统级 quiesce 仍受 `POWER-001` 阻塞 |
| UI/LVGL 刷新 | 自身 init 成功则 task 已启动 | 继续刷新 reconnect/error/icon | task 解耦；不等于产品状态完整 |
| touch 采样/手势/菜单动画 | touch task 已启动，直接 LVGL 导航可工作 | 可继续直接 UI 交互 | driver/UI 部分成立 |
| 电量 UI | battery service 在网络前启动 | 可继续采样/发布 | 与网络解耦；准确性和 UNKNOWN 语义另见 `BAT-*` |
| volume 拖动预览 | callback 可更新 speaker logical volume；release commit 入 shell queue | 同左 | 最终 callback/UI/persist 可能延迟到重连；不能宣称完整可用 |
| Wi-Fi scan/选择 | 首次连接失败时 workers 尚未创建 | workers 存在但 request action 无 consumer | offline 菜单能力不成立 |
| BLE 重配网入口 | touch 对 reprovision 使用直接 callback，flash worker 已创建 | 可绕过 shell queue 请求重启 | 正向例外；仍受 OTA/audio/power gate 和 BLE lifecycle 问题限制 |
| 本地 wake word | model 尚未 init/start | disconnect 明确 stop | 不成立，`OFFLINE-001` |
| conversation sleep/awake | Wi-Fi 失败停在 ERROR；WS 成功前不进 sleeping | disconnect 后变 WS_CONNECTING，wake stop | 不成立，`OFFLINE-001` |
| touch 开始/停止 listen | action 入队，无在线 consumer | action 入队，无在线 consumer | 不会离线对话；可能陈旧重放，`OFFLINE-002` |
| meeting/monitoring enable | action 无 consumer；enable 本来要求 live WS | action积压至重连 | 不能离线启动；迟到 enable 需禁止，`OFFLINE-002/PRIV-002` |
| meeting/monitoring local disable | 控制器尚未 active | WS disconnect notify 会终止 active mode | active 模式的断线停止是正向控制；仍需 D 级确认 END/PA/MIC 终态 |
| 正常云对话/OTA command | 不可用 | 不可用 | 属云依赖能力，不应伪装成本地 ready |

## 6. 已确认问题

### OFFLINE-001（P1，新）：local capability owner 被首次联网和 WebSocket 生命周期门控

证据：

- `nuwax_ai_partner_main.c:15984-16079` 在 wake init 和多个本地 worker 之前对 Wi-Fi init/connect 失败直接 return；
- `:16088-16120` 说明 wake、Wi-Fi workers 和 speaker task 都在首次联网成功后创建；
- `:16313-16317` 只有 WebSocket 成功后才进入 sleeping/start wake；
- `:16412-16426` 断开后 stop wake 并进入重连，没有 offline local owner。

影响：设备可能显示 UI、响应滑动并可 SW3 关机，却不能本地唤醒、进入一致的睡眠态、消费 listen/Wi-Fi/volume actions或表达完整 capability。运维若只看“屏幕能动”会误判产品 ready；云死会把本地语音入口一并关闭。

解决方向：

1. 在网络前建立唯一 `local_runtime` owner，初始化 mandatory local capabilities，并发布带 revision 的完整 capability snapshot；网络只是独立 optional/service capability。
2. wake detector 的 lifecycle 由 local conversation owner 控制。离线检测后显示明确 offline 状态、触发有界 reconnect 或本地提示，再安全重启 detector；不得把 wake task 的存在绑定到 WebSocket OPEN。
3. speaker、Wi-Fi menu workers 和本地 command owner 在进入网络循环前创建；失败进入明确 degraded/safe state，不从 `app_main()` 留下一组孤立 task 后直接 return。
4. 增加 `OFFLINE_READY/DEGRADED` 产品状态，保留本地 sleep/awake 语义；`transport_connected/service_ready` 只作为快照字段，不能覆盖 local state。

### OFFLINE-002（P1，新）：离线 shell action 无 owner、无代次，恢复后可陈旧重放

证据：

- touch gesture 对除 reprovision 外的 action 都 `xQueueSend(s_shell_actions, ..., 0)`，队列深度为 8；
- consumer 只存在于在线 `wait_for_mic_start_action()`、capture poll、cloud wait 和 exclusive mode poll；
- reconnect loop 不 drain/reject action，断线 purge 也不 reset shell queue；
- payload 只有 action/index，没有产生时间、connection epoch、UI revision、press generation 或 expiry。

影响：断网期间的 START/STOP、volume、Wi-Fi、meeting 等动作可能排满队列或在重连后执行。尤其 meeting enable 若在恢复后迟到执行，用户看到的动作时间和持续采集真正开始的时间分离；这与 `PRIV-002` 的本地同意缺口叠加。即使产品决定“离线操作延迟执行”，也必须有可见 pending/cancel/expiry 合约，当前没有。

解决方向：建立常驻 local command owner；每个 action 带 monotonic timestamp、generation、UI revision 和 policy。纯本地 action 立即执行；需要云端的 action 离线时立即明确拒绝或进入可取消、可见且有 deadline 的 pending 状态；listen press/release 只能在同一 gesture generation 内配对；持续采集 enable 不得跨断线重放。断线和重连边界按 policy 清理，而不是把所有 action 留给未来 socket。

## 7. 既有问题交叉引用

| ID | A2-06 中的证据 | 处理 |
|---|---|---|
| `START-002` P1 | 首次 Wi-Fi 失败留下部分本地 task，wake/worker未建立，无统一 capability snapshot | 扩充既有问题 |
| `RTOS-001` P1 | local/network workers无统一 heartbeat；A 级不能证明 I/O stall 时仍健康 | 复用 |
| `RTOS-005` P1 | wake stop 无 deadline，断线清理可被卡住 | 复用 |
| `NET-IO-001` P2 | `ws://` DNS/blocking connect 无本地总 deadline | 复用 |
| `NET-CONC-001/002` P1 | connect/close/TX stall 与 cancel 生命周期仍需故障注入 | 复用 |
| `LIFE-009` P1 | RX task立即退出/OPEN发布交错影响重连恢复可信度 | 复用 |
| `EVENT-002` P1 | Wi-Fi event/fallback/app普通共享状态影响 offline/recovery snapshot | 复用 |
| `ERR-002` P2 | fallback watcher两次启动均忽略结果，备网能力可静默缺失 | 复用 |
| `POWER-001` P1 | SW3能响应不代表关机已安全 quiesce 全系统 | 复用 |
| `PRIV-002` P1 | 持续采集 local consent/hard-stop不足；迟到 meeting action扩大风险 | 复用 |

## 8. B/D 关闭矩阵

| 等级 | 用例 | 必须断言 |
|---|---|---|
| B | 提取 local/network capability 状态机，覆盖 Wi-Fi init失败、无凭据、AP消失、cloud config缺失、DNS/TLS/101失败 | local mandatory capability不因网络失败消失；状态/revision唯一；无直接 return 留下伪 ready |
| B | shell action policy，覆盖断线前/中/后 START/STOP、volume、scan、saved Wi-Fi、meeting enable/disable、reprovision | 无陈旧跨代次动作；持续采集 enable不可迟到重放；queue满有明确结果 |
| B | wake owner fault model | offline wake有唯一终态，MIC lease释放，detector有界restart；stop卡死触发受控恢复 |
| D | 冷启动时 AP不存在/密码错误/DHCP失败各至少3次 | UI/touch/SW3/电量持续工作；wake/offline提示符合批准语义；task/heap不增长 |
| D | 在线 sleeping 时关闭云、DNS黑洞、TCP拒绝、TLS超时、101拒绝、云静默/半开 | 本地能力保持；重连有总deadline、指数退避+jitter；无死锁/重启风暴 |
| D | 断线期间重复触摸、调音量、切 meeting、触发 Wi-Fi 菜单和 BLE reprovision | 每个动作立即有确定反馈；无迟到采集、无queue溢出、无错误NVS commit |
| D | 断线时点按 SW3，分别处于 Wi-Fi wait、DNS/TLS、WS close、音频停止 | power task有界响应；系统级关机仍按 `POWER-001` 的波形/owner矩阵验收 |
| D | 100次断开/恢复和至少8小时云死 soak | task/heap/socket/handler/queue水位回基线；CPU/功耗不持续恶化；状态revision单调 |

所有 D 级记录必须绑定 source/config/image hash、V2设备ID、run_id、故障注入方法、完整串口日志和资源前后值。

## 9. A2-06 判定

- A 级静态审计：**完成**。
- 独立 UI/touch/SW3 task：**静态存在**，但只证明调度解耦，不证明完整本地产品能力。
- 云端死掉不阻塞本地 UI、电源、唤醒和休眠：**未满足**。UI/SW3部分成立，wake/sleep/action owner明确不成立。
- 新增 canonical 问题：`OFFLINE-001` P1、`OFFLINE-002` P1。
- B/D：**开放**。本文件不能作为断网、云死、Wi-Fi恢复、资源稳定、SW3安全关机或离线隐私行为的真机通过证明。
