# 阶段 1 A1-05：Callback、Timer、Event、ISR 与 Cache-safe 只读审计

审计日期：2026-08-02  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
权限：A 级只读；未修改业务源码、未构建、未运行 host test、未操作 COM5、未写 NVS、未 OTA。  
对应计划：阶段 1.6、1.9、1.14、1.19；执行项 `A1-05`。

> 2026-08-03 后续状态：`TIMER-002` 的两处直接 NULL 解引用已修复，并沿 UI create 链传播失败；host contract 和 ESP-IDF 6.0 构建通过。allocator 故障注入和实机低内存验证仍开放。详见 `implementation-verification-2026-08-03.md`。下文保留原始发现快照。

## 1. 范围、方法和结论边界

本轮以 `main/CMakeLists.txt` 的产品候选链接路径为边界，反向枚举以下入口：

1. `esp_timer`、LVGL timer/animation/object event；
2. `esp_event`、network provisioning manager callback；
3. GPIO ISR、I2C/I2S/SPI 驱动中断和 DMA buffer；
4. WebSocket handshake/text/binary/activity callback；
5. wake、touch、battery、speaker、board input 和 OTA 应用 callback；
6. Flash cache-disabled 期间可能仍然活动的 ISR、DMA、PSRAM stack/buffer 和 NVS/OTA 写入路径。

每个入口核对：执行任务/中断上下文、允许的阻塞范围、注册 owner、解绑/停止顺序、`user_ctx` 与 payload 生命周期、epoch/generation、重复初始化、cache/IRAM/DRAM/DMA 能力。

静态审计可以确认 C 数据竞争、缺少回滚、空指针调用和契约缺口；不能仅凭代码证明 NVS 最坏关缓存时间、I2S underrun 是否可闻、短触摸是否会被控制器锁存、真实 event callback drain 时延。因此后文严格区分“已确认代码问题”和“需 B/D 级实测量化的物理后果”。

## 2. 当前配置真值

| 项目 | 当前产品配置/代码 | 静态含义 |
|---|---|---|
| CPU/RTOS | ESP32-S3；`CONFIG_FREERTOS_HZ=100`（`sdkconfig:3276`） | 32 位双核；普通 `volatile` 不提供跨核同步，64 位普通/volatile 访问不能假设原子 |
| `esp_timer` | `CONFIG_ESP_TIMER_IN_IRAM=y`；task affinity CPU0；ISR dispatch 未启用（`sdkconfig:3013,3021,3027-3033`） | 当前 LVGL tick callback 在 `esp_timer` task 执行，不是 ISR callback；cache-disabled 时可能延后执行 |
| I2C | `CONFIG_I2C_ISR_IRAM_SAFE` 未启用；`CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM=y`（`sdkconfig:2337-2340`） | 仅 handler 位于 IRAM 不等价于完整 driver cache-safe；cache-disabled 窗口不保证 I2C 事务继续推进 |
| I2S | `CONFIG_I2S_ISR_IRAM_SAFE`、`CONFIG_I2S_CTRL_FUNC_IN_IRAM` 均未启用（`sdkconfig:2347-2349`） | Flash 写入时 I2S 中断可被推迟；DMA 只能提供有限缓冲，不能静态证明无 underrun/overrun |
| SPI | `CONFIG_SPI_MASTER_ISR_IN_IRAM=y`（`sdkconfig:2372`） | 与内部 DMA staging buffer 配合，显示路径的 cache-safe 基础优于 I2C/I2S；仍需实测 Flash/UI 并发 |
| LVGL | `CONFIG_LV_USE_ASSERT_NULL=y`、`CONFIG_LV_USE_ASSERT_MALLOC=y`（`sdkconfig:4669-4671`） | LVGL 分配失败是可达的致命/断言边界；创建结果后继续直接调用必须显式检查 |
| shared I2S DMA | 6 descriptors × 240 frames（`sdkconfig:1048-1050`；`board/vocat_audio_board.c:333-352`） | 16 kHz 下约 90 ms 的单方向帧容量；这是名义缓冲，不是 NVS/Flash 最坏时延证明 |

## 3. Callback 与 ISR 执行上下文矩阵

| 注册/入口 | 实际执行上下文 | payload / `user_ctx` 生命周期 | 停止、解绑和静态结论 |
|---|---|---|---|
| LVGL tick `lvgl_tick_cb`（`nuwax_lvgl_port.c:349-352,505-511`） | CPU0 `esp_timer` task；默认 task dispatch | 无外部 payload；访问 LVGL tick counter | 产品常驻，无 deinit；成功路径成立，部分初始化失败见 `TIMER-001/LIFE-004` |
| LVGL timer/animation/object event（`nuwax_ui_v28/`） | `lvgl_task` 调用 `lv_timer_handler()` 的单线程上下文 | LVGL object/timer 生命周期；大多数为静态 UI singleton | 未发现非 LVGL task 直接执行这些 callback；创建失败处理不一致，见 `TIMER-002` |
| Wi-Fi/IP handler（`wifi_manager.c:244-296,342-351`） | ESP-IDF default event-loop task | `event_data` 仅在 callback 内使用；共享模块全局状态 | handler instance 句柄为局部变量且无 rollback/unregister；共享状态无同步，见 `EVENT-001/002` |
| provisioning `esp_event` handler（`nuwax_provision_new/nuwax_provision_main.c:325-381,517-620`） | ESP-IDF default event-loop task | `event_data` 不跨 callback 保存；`s_state` 主要由 `s_state_lock` 保护 | partial-register rollback 完整；stop 会 deinit manager、unregister，再释放资源。`s_started` 不在同一同步域，见 `EVENT-003` |
| provisioning manager app callback（`nuwax_provision_new/nuwax_provision_main.c:453-502,1117-1124`） | provisioning manager 所属上下文，不假设为 app owner task | 当场读取/校验 `wifi_config_t`，未保存借用指针 | manager deinit 在 security buffer/queue 释放前；未发现当前 `event_data` UAF |
| touch GPIO ISR（`touch_input.c:76-86,709-755`） | GPIO ISR service | 只读取静态 task handle，调用 `vTaskNotifyGiveFromISR` 和 `portYIELD_FROM_ISR`；touch task stack/TCB 强制 internal | task 先创建再注册 ISR；产品无 stop/deinit，故当前没有 task-handle UAF。ISR service flags 为 `0`，见 `ISR-001` |
| touch event callback（`touch_input.c:509-520,684-690`） | `nuwax_touch` task | callback/context 在 critical section 内取快照；point 只在调用期间有效 | callback 不持 I2C mutex 执行；当前注册对象是 app 生命周期静态对象，未发现迟到 UAF |
| WebSocket handshake callback（`ws_transport.c:1502-1515`） | `nuwax_ws_connect()` owner/app task，同步发生在 RX task 创建前 | `user_ctx=&ws` 指向不返回的 `app_main` 栈帧内 owner 对象 | 在 RX 启动前用于发布 transport-auth；没有借用 frame payload |
| WebSocket activity/text/binary callback（`ws_transport.c:1277-1303`） | 单一 `nuwax_ws_rx` task，同步调用 | payload 借用 connection RX buffer，仅在 callback 返回前有效；`user_ctx` 同上 | owner 先 join RX，再释放 buffer并清空 callback/context（`:1658-1690`），静态 UAF 边界正确；共享状态和同步 ACK 问题见 `WS-CB-001/002` 及既有 `NET-CONC-003/AUDIO-CONC-001` |
| normal TTS binary ingress（`nuwax_ai_partner_main.c:4084-4248`） | WebSocket RX task | speaker 在入队前把 PCM `memcpy` 到值对象（`conversation/speaker_playback.c:1385-1456`），不保留 RX buffer | payload ownership 正确；但 ingress 仍可能等待 speaker mutex/I2S lazy init，已在 `phase1-ws-audio-concurrency-audit.md` 登记 |
| meeting/monitoring protocol callback（`audio_modes/meeting/meeting_mode.c:1055-1120`；`audio_modes/monitoring/monitoring_mode.c:1161-1195`） | WebSocket RX task | 解析结果复制为 queue value；队列 payload ownership 正确 | owner task 消费事件；RX 侧在入队前读取 owner 状态并调用 `can_start()`，见 `WS-CB-002` |
| wake callback（`wake_word_detector.c:540-558`；`nuwax_ai_partner_main.c:2290-2299`） | `nuwax_wake` worker | detector 锁内复制 callback/context，锁外调用；app callback 把 wake word复制进 queue value | stop/deinit 拒绝从 worker callback 内同步释放；当前静态 callback/context 为 app lifetime，未发现迟到 UAF |
| speaker callbacks（`conversation/speaker_playback.c:1366-1381,3674-3788`） | 主要由 `nuwax_speaker` task执行；音量 callback 也可由 app/UI command caller 执行 | callback table 在 boot 初始化后保持不变；owner event 使用值队列 | 多数状态变更投递 owner queue；回调上下文并非统一，公开 API 可重入性继续由 A1-07 审计 |
| battery listener（`board/battery_service.c:129-139,313-328`） | `battery_service` task | 配置在 mutex 内取快照；snapshot 只在同步 callback 内有效 | app listener只发布 UI state；stop/join 后才删除 worker。未发现当前 listener UAF |
| board GPIO/head-touch callback（`board/board_gpio_inputs.c:50-64,114-121,179-255`） | `board_inputs` polling task | callback/context 在 critical section 内取快照 | header 明确要求 bounded/non-blocking；deinit先停止/join worker再清 callback，静态顺序合理 |
| OTA callback table（`nuwax_ota_new/nuwax_ota_device.h:13-27`；`nuwax_ai_partner_main.c:15557-15567`） | OTA controller worker；boot-health callback也由 controller 流程调用 | callback/context 为 app lifetime static owner | 当前无 controller deinit，初始化 partial-failure 已由 `LIFE-010` 登记；OTA audio quiesce见第 6 节 |
| I2C/I2S application callbacks | 产品路径未调用 `i2c_master_register_event_callbacks` / `i2s_channel_register_event_callback` | 无应用层异步借用 buffer | 中断由 ESP-IDF driver内部处理；cache-safe 仍由配置、DMA 内存和同步 I/O 生命周期决定 |

## 4. 注册、解绑、重复初始化和迟到访问结论

| 子系统 | 阻止新事件 | 等待已进入 callback | 释放对象 | 结论 |
|---|---|---|---|---|
| WebSocket | `connected=false` + transport interrupt | `rx_task_running=false` join predicate | join 后释放 RX buffer、callback/context、transport | 主关闭顺序可阻止 frame callback UAF；RX 立即退出覆盖 OPEN 的竞态另见 `LIFE-009` |
| provisioning | stop provisioning + manager deinit + event unregister | 依赖 manager deinit / event unregister 语义；无显式 callback in-flight counter | 之后释放 security buffer、queue/gate | 当前 callback 不引用随后释放的 buffer；整体合理，但 lifecycle flag 同步有 P2 缺口 |
| touch ISR | 产品运行期不解绑 | 不适用，任务常驻 | 不释放 task/queue/ISR | singleton 假设下无 UAF；不支持同 boot deinit/reinit，且 flags 不是 IRAM-safe |
| LVGL timer | 产品运行期不停止 | 不适用，LVGL task常驻 | 无统一 port deinit | 成功启动路径是 singleton；初始化失败和重复调用不闭合 |
| Wi-Fi handlers | 没有 unregister API | 没有 | 没有 | 只在完整成功的一次性 boot 路径成立；任何中途失败重试都不闭合 |
| wake/battery/board input | stop request | worker退出/停止信号 + owner join | join 后删除 task/资源 | 主路径顺序合理；真实 I/O 卡住仍需 D 级故障注入 |

## 5. 已确认问题

### EVENT-001 / RTOS-009（P1，同一根因不重复计数）：Wi-Fi event handler instance 丢失，初始化失败没有反向回滚

证据：`wifi_init_once()` 在 `wifi_manager.c:298-371` 依次创建 event group、netif、Wi-Fi driver并注册两个 handler，但 `instance_any_id`、`instance_got_ip` 仅为局部变量（`:339-351`）。任一后续步骤失败都直接 return；没有 unregister、`esp_wifi_deinit()`、netif delete 或 event group delete。

影响：`s_wifi_initialized` 仍为 false，调用者重试时会覆盖 `s_wifi_event_group` 并再次注册 handler。旧 handler可继续写新/旧状态，形成 event group泄漏、重复 reconnect、重复 fallback 和 retry counter异常。

关闭条件：保存 netif 和两个 handler instance；以分阶段 init state + 单一 `goto fail` 逆序停止 Wi-Fi、unregister、destroy netif/event group；明确 idempotent init/deinit；对每个步骤做故障注入并证明 callback次数始终为 1。

### EVENT-002（P1）：Wi-Fi event loop、fallback worker 和 app caller 对普通全局状态存在 C 数据竞争

证据：`s_wifi_retry_count`、`s_wifi_connect_requested`、`s_fallback_in_progress`、`s_active_ssid_index`、`s_current_ssid`、`s_current_ip`、`s_wifi_scan_succeeded` 等定义于 `wifi_manager.c:40-53`。event handler在 `:244-295` 读写；连接/fallback路径在 `:431-475,552-601` 读写；status getter在 `:608-613,781-788` 无锁读取字符串。

`EventGroup` 只同步 bits，不为这些普通对象建立统一 happens-before；`volatile` 也没有被使用且即使使用也不能修复跨核数据竞争。字符串读取可观察到 SSID/IP 的中间状态，bool/counter可产生错误重连判断。

关闭条件：优先将 Wi-Fi 状态变为单 owner task + command/event queue；若保留现结构，至少用一把短持有 state lock 保护完整 snapshot/revision，禁止持锁调用 `esp_wifi_*`、NVS、日志或等待。getter一次复制一致快照。并发 scan/connect/disconnect/fallback stress下 revision单调且每个 disconnect仅触发一次策略。

### EVENT-003（P2）：provisioning `s_started` 跨 event task 和 owner task未进入同一同步域

证据：`s_started` 定义于 `nuwax_provision_new/nuwax_provision_main.c:47`；owner在 `:1288-1344,1492-1507` 普通读写；Wi-Fi event callback却在持有 `s_state_lock` 时读取它（`:578-597`）。写方没有取得该锁，因此读方的 critical section不能建立同步。

影响：start/stop边界的 disconnect事件可能依据陈旧 started状态更新或遗漏 `wifi_disconnected_before_commit`。当前影响限于启动期配网结果/诊断状态，未发现由此直接释放 callback资源的 UAF，故定为 P2。

关闭条件：把 started/stopping 与 `s_state` 一并置于 `s_state_lock` 或使用 C11 atomic；stop先发布 STOPPING，再停止新事件，manager deinit/unregister后发布 STOPPED。用 start/stop/disconnect交错 host/property test验证。

### WS-CB-001（P1）：WebSocket activity 64 位时间戳用 `volatile` 跨核共享

证据：`s_ws_last_rx_activity_ms` 是 `volatile int64_t`（`nuwax_ai_partner_main.c:527`）；RX activity callback写入（`:3686-3704`），app owner读写（`:3690-3697,16194`）。ESP32-S3 为 32 位平台，`volatile` 不保证 64 位原子性，也不形成 C memory ordering。

影响：撕裂/陈旧读可产生虚假超时或掩盖真实 activity timeout，继而误重连或长期不重连。问题不要求极端网络条件，只要求两个核心在临界时间交错。

关闭条件：改用 `atomic_uint_fast64_t`（若内部实现/性能可接受）或同一 portMUX下的 64 位 snapshot；也可使用 wrap-safe 32 位毫秒计数。增加 RX activity与owner timeout高频交错测试。

### WS-CB-002（P1）：meeting/monitoring 的 RX busy预判读取 owner状态，队列没有消除共享状态竞争

证据：两类 mode 的 `state` 仅声明为 `volatile`，`initialized` 为普通 bool（`audio_modes/meeting/meeting_mode.h:52-58`；`audio_modes/monitoring/monitoring_mode.h:44-49`）。owner task通过 `set_state()` 和 run/request API修改；RX callback在 `audio_modes/meeting/meeting_mode.c:1055-1069`、`audio_modes/monitoring/monitoring_mode.c:1161-1175` 直接读取并调用 `can_start()`。`can_start()` 又读取对方 mode 的 `initialized/state/queue`（`nuwax_ai_partner_main.c:4994-5004`）。

影响：两个 enable在边界交错时，RX可能基于陈旧状态发出错误 busy ACK，或把本应拒绝的事件入队。最终状态修改虽在 owner task，但协议可见结果已在 RX task提前决定。

关闭条件：RX只解析、验证结构并把 command value入队；所有模式互斥和 busy决策由单一 audio-mode owner完成。若协议要求立即 ACK，使用独立有界 control-TX queue回传 owner决策，不在 RX callback同步 send。`volatile` 字段改为 owner-only或原子快照。同步 meeting/monitoring enable交错测试必须恰有一个成功。

### TIMER-001 / LIFE-004（P1）：LVGL port在周期 timer启动后失败时不停止、不删除、不回滚

证据：`nuwax_lvgl_port_init()` 创建 mutex、LVGL、draw buffers、display/indev，再创建并启动 `s_tick_timer`（`nuwax_lvgl_port.c:410-511`）。之后 TCB/stack allocation或 task create失败会直接 return（`:518-548`）。timer start失败也没有 `esp_timer_delete()`。

影响：timer可在没有 LVGL owner task时永久运行；mutex/display/buffer/indev/TCB/stack泄漏。再次 init会覆盖全局 handle、重复 `lv_init()`、创建第二个 tick source或重复 display对象。

关闭条件：该项沿用 `phase1-worker-lifecycle-audit.md` 的 `LIFE-004`，不重复计数。实现分阶段初始化和逆序 `goto fail`；timer先 stop后 delete，task资源、indev/display、draw buffer和mutex有唯一 owner；增加逐点 allocation/timer/task-create故障注入和重复 init测试。

### TIMER-002（P1）：两个 LVGL timer创建结果被立即解引用

证据：status bar在 `nuwax_ui_v28/nuwax_status_bar.c:866-867` 创建 `s_quiet_timer` 后无检查调用 `lv_timer_set_repeat_count()`；focus menu在 `nuwax_ui_v28/nuwax_focus_menu.c:1312-1313` 创建 `s_main_surface_timer` 后无检查调用 `lv_timer_pause()`。产品启用了 null/malloc assert。

影响：内部内存压力下 UI初始化可能断言/崩溃，或在 assert策略改变后直接 NULL访问；上层 void create API无法区分“UI完成”和“关键 timer缺失”。

关闭条件：检查创建结果并把 UI create链改为可传播错误的 init API；若选择启动期致命策略，必须统一记录阶段/reset reason并安全进入可恢复模式，不能由偶然 NULL解引用决定。对 LVGL allocator逐点失败验证。

补充：`nuwax_ui_v28/nuwax_app.c:51` 丢弃 engine tick handle，失败时只会静默失去 face engine周期更新；face/drawer的其余 `lv_timer_create()` 多数在后续使用前检查 NULL。前者应保留 handle和初始化结果，列为 P2 teardown/可观测性改进，不与 `TIMER-002` 重复计数。

### CACHE-001（P1，物理后果待实测）：运行期 NVS/Flash写入没有统一 audio/cache-disabled 仲裁

证据：I2C/I2S完整 ISR IRAM-safe未启用。音量持久化在 `conversation/speaker_playback.c:809-823,3103-3127` 直接 `nvs_commit()`，可由 UI/cloud command在播放期间调用（`nuwax_ai_partner_main.c:5156-5160,14241-14244`）。远端 `set_wake_word` 和 L3 persistent command同样可在业务会话中提交 NVS（`wake_word_detector.c:154-166`；`p3_afe_l3.c:75-101`）。Wi-Fi保存也有多处直接提交。它们没有统一进入 `s_flash_ops_task`，也没有检查 capture/playback idle。

确认边界：已确认的是“代码缺少 Flash/audio 仲裁且 driver并非完整 cache-safe”；不能仅凭 6×240 DMA名义容量宣称已经发生可闻 underrun、采集丢样或 I2C timeout。真实后果取决于 NVS擦写、GC、Flash芯片和调度的最坏时延，必须 B/D 级测量。

关闭条件：把运行期持久化集中到 flash-op owner，coalesce revision，并只在 capture/playback停止或受控 guard窗口提交；OTA继续使用独立 quarantine。若选择启用 I2C/I2S IRAM-safe，必须证明 driver object、DMA和ISR可达数据全部在 internal DRAM并重新核算内部 RAM。实机同时记录 NVS commit最大时延、I2S underrun/overrun、采集序号、speaker gap和I2C timeout。

### ISR-001（P2）：touch handler标记 IRAM，但 GPIO ISR service不是 IRAM-safe contract

证据：`touch_int_isr` 有 `IRAM_ATTR`且只做ISR-safe通知（`touch_input.c:76-86`）；但 `gpio_install_isr_service(0)` 使用 flags `0`（`:732`），已有全局 service时还会直接复用其未知 flags。`IRAM_ATTR` 只保证函数位置，不能保证整条 dispatcher/service和数据访问在 cache-disabled时可执行。

影响：Flash/NVS窗口内 touch IRQ可能延迟；100 ms fallback poll（`:40,491-495`）限制了永久失效，但短触摸是否丢失取决于触摸控制器是否锁存中断/触点，当前代码不能证明。

关闭条件：由单一 board owner用 `ESP_INTR_FLAG_IRAM`安装 GPIO ISR service，并证明 ISR链、task handle和所有可达数据在 internal DRAM；或明确接受非 cache-safe中断，结合控制器规格/逻辑分析仪证明短触摸被锁存且延迟SLO满足。进行 NVS/OTA窗口内点按/滑动测试。

## 6. DMA 与 cache-disabled 矩阵

| 路径 | buffer/代码能力 | Flash/NVS并发结论 |
|---|---|---|
| touch/BQ27220/QMI8658A I2C | 应用 scratch buffer均为 `DRAM_ATTR`（`touch_input.c:70-74`、`board/bq27220_driver.c:76-78`、`board/qmi8658a_driver.c:50-51`）；各驱动有 I/O mutex | 避免 PSRAM stack buffer被 driver ISR迟到访问；但 I2C ISR完整 cache-safe未启用，事务延迟/timeout仍需测量 |
| shared audio I2S TX/RX | DMA由 driver分配；6×240 frames；同步 read/write调用使用的 task stack/buffer可能在 PSRAM，但 driver DMA copy由内部对象负责 | 普通运行期 NVS提交可能推迟 ISR；OTA前高层音频被quiesce。不能静态宣称无 underrun/overrun |
| display QSPI | `display.c:934-940` 分配 internal DMA staging buffers；SPI master ISR in IRAM；PSRAM/LVGL source先复制到 staging再发送 | 静态能力匹配较好；仍需 Flash写+动画压力测试证明无卡屏/flush timeout |
| WebSocket RX | connection RX buffer优先 PSRAM（`ws_transport.c:110-142`）；仅 task context读取 | cache-disabled期间 task执行会暂停/延迟，不由ISR访问；未发现PSRAM buffer被ISR持有 |
| touch GPIO ISR | handler IRAM、只访问静态 task handle；touch task stack/TCB internal | handler本体合格，但service flags不保证cache-safe，见 `ISR-001` |
| LVGL `esp_timer` tick | callback很短，仅 `lv_tick_inc()`；task dispatch | Flash窗口只会形成tick延迟，不是ISR访问PSRAM；初始化生命周期问题独立存在 |
| OTA transport | internal DRAM IO buffer（`nuwax_ota_new/nuwax_ota_transport.c:380-399`）；顺序 `esp_ota_begin/write/end`（`:496-548,689`） | controller在启动transport前两次检查 audio idle（`nuwax_ota_new/nuwax_ota_device.c:927-976`；app quiesce `nuwax_ai_partner_main.c:4935-4990`）。本轮未发现 `esp_ota_write` 绕过该入口；abort cleanup在 `nuwax_ota_new/nuwax_ota_transport.c:745-756` |

## 7. 已验证为合理、不得误报的路径

1. 产品代码只有 touch一个显式 GPIO ISR；I2C/I2S未注册应用层 event callback，不能把同步 driver API误写成应用 ISR callback。
2. touch ISR先创建 internal task再注册；当前没有公开 touch stop/deinit，所以静态路径不存在释放 task后 ISR仍通知的 UAF。缺点是不可重入/不可回收，不是当前 UAF。
3. WebSocket callback借用 RX buffer，但 speaker PCM、session command、meeting/monitoring event都在 callback返回前复制到自有 queue/value；未发现保存 payload指针。
4. WebSocket owner在清 callback/context和释放 RX buffer前等待 RX task退出；`user_ctx=&ws` 的 owner对象贯穿 `app_main`生命周期。
5. provisioning四类 `esp_event` handler的 partial registration失败路径会逐个 unregister；manager deinit发生在 security buffer和job queue释放之前。
6. touch、BQ27220、QMI8658A 的 I2C scratch buffer位于 internal DRAM；BQ27220/touch在 timeout后避免立即 remove/re-add device handle，降低 driver ISR retirement竞态。
7. display使用 internal DMA staging，不把任意 PSRAM pixel pointer直接交给 QSPI DMA。
8. OTA在所有产品 `esp_ota_write` 前有 audio quarantine/quiesce；因此 `CACHE-001` 指向普通运行期 NVS提交，不能误写成“OTA正在一边播放一边写固件”。

## 8. 既有问题交叉引用

以下问题与 callback上下文相关但已在其他阶段 1 证据文件登记，本文件不重复创建ID：

- `NET-CONC-001/002/003`、`AUDIO-CONC-001/002`：WS lifecycle/I/O锁、connect-close竞态、RX同步ACK和speaker ingress背压，见 `phase1-ws-audio-concurrency-audit.md`；
- `LIFE-004`：LVGL timer/task部分初始化回滚，本文件以 `TIMER-001`别名补充 timer证据；
- `LIFE-009`：WebSocket RX task可能先退出，创建者随后把 lifecycle覆盖回 OPEN，见 `phase1-worker-lifecycle-audit.md`；
- `LIFE-010`：OTA controller callback/queue/interlock partial-init没有完整反向回滚；
- `RTOS-007`：BLE job queue和handler shutdown join；`QUEUE-001` 是Wi-Fi状态报告overwrite问题，`LIFE-003` 是孤立touch对象，三者不得混用，见 `phase1-queue-and-lifecycle-audit.md`。

## 9. 后续动态验证门槛

| 验证项 | 等级 | 注入/场景 | 必须记录的通过条件 |
|---|---|---|---|
| Wi-Fi init rollback | B/D | 在 netif/Wi-Fi init、两个handler register、set mode、start逐点失败并重试 | event group/handler/netif无增长；单个disconnect只回调一次；fallback/retry计数唯一 |
| mode callback并发 | B | meeting和monitoring enable在相同WS epoch交错到达，同时叠加UI open/close | 仅一个owner成功；ACK与最终状态一致；无RX同步长阻塞 |
| WS activity可见性 | B | RX activity与owner timeout在双核高频交错 | 无撕裂、无虚假timeout；disconnect后在SLO内重连 |
| LVGL failure rollback | B | timer create/start、TCB/stack、task create、quiet/menu timer逐点分配失败 | 无遗留timer/task/display/buffer；UI失败有明确状态；可重复init或明确拒绝 |
| NVS/Flash与音频 | D | 播放、采集、双讲时触发volume/wake/L3/Wi-Fi持久化，并触发NVS GC最坏路径 | commit max/p95/p99；I2S错误/gap=0或在冻结阈值内；上行序号连续；无I2C timeout |
| touch cache窗口 | D | NVS commit和OTA写入期间执行短点按、长按、全方向滑动 | 事件不丢或明确符合冻结SLO；IRQ/fallback latency有逻辑分析仪/日志证据 |
| driver cache-safe profile | B/D | 若启用 I2C/I2S IRAM-safe，构建内存预算并实机Flash并发 | internal/DMA reserve满足门槛；ISR链不触及PSRAM/flash；音频/触摸稳定 |

## 10. A1-05 计划反向校验

| 计划要求 | 静态证据 | 状态 |
|---|---|---|
| 1.6 callback注册、解绑、epoch/generation、阻塞API | 第 3、4、5、8 节 | A级完成；WS背压动态验证待 B/D |
| 1.9 ISR、DMA、buffer生命周期、cache/PSRAM能力 | 第 2、3、6、7 节 | A级完成；时序/物理后果待 D |
| 1.14 timer/event/driver callback上下文、重复注册、迟到回调 | 第 3~5 节 | A级完成；逐点失败注入待 B/D |
| 1.19 ISR/IRAM/DMA callback独立检查 | 第 2、6、7 节 | A级完成；IRAM-safe候选profile待 B/D |
| 产品路径反向枚举 | `phase1-callback-api-inventory.txt`、`phase1-callback-timer-rg-current.txt`及本文件矩阵 | 已完成；host/stub不计为产品能力 |

## 11. 阶段结论

- 未确认新的直接 P0。
- 新确认并独立登记：`EVENT-002` P1、`EVENT-003` P2、`WS-CB-001` P1、`WS-CB-002` P1、`TIMER-002` P1、`CACHE-001` P1、`ISR-001` P2。`EVENT-001` 是既有 `RTOS-009` 的 callback 侧补充证据，不重复计数。
- `TIMER-001` 与既有 `LIFE-004` 是同一根因，不重复计数；其 timer生命周期证据已补齐。
- callback payload/user context方面，WebSocket、touch、wake、battery、board input主路径未发现当前UAF；这不等于阻塞时延、stop竞态或实机长稳已经通过。
- OTA transport的 `esp_ota_write` 有高层 audio quiesce门；普通NVS持久化没有统一门，是当前cache-disabled审计的主要产品风险。

**A1-05 状态：A级静态审计完成；B/D级故障注入、cache-disabled时序和实机验收仍开放，不得标记为产品已通过。**
