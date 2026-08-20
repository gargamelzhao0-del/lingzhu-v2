# 阶段 1：公开 API 可重入性、静态工作区与单 owner 契约只读审计

审计日期：2026-08-02  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
对应工作项：A1-07、评审计划阶段 1.17  
权限：A 级只读；未修改业务代码、未构建、未运行测试、未访问 COM5、未写 NVS、未执行 OTA

## 1. 范围、方法与产品路径

本轮从公开头文件和产品调用点反向检查：重复 init/deinit、同一对象并发调用、静态 workspace/global scratch、singleton/owner token、同步 callback 重入、callback user context 寿命，以及 API 是否明确限制到单 task/owner。重点覆盖 `mic_input_init_with_owner`、`p3_afe_l3`、face cache、LED 和 battery，并对正式 CMake 产品路径中的其他高风险 singleton 做交叉抽查。

当前 `main/CMakeLists.txt` 静态展开结果为：基础 `SRCS_COMMON` 74 项；因 `CONFIG_NUWAX_OTA_ENABLE=y` 追加 OTA 5 项；`SRCS_LVGL_UI` 70 项；正式产品候选共 149 个源文件，且当前快照下均存在。`*_host_check.c`、host stub 和未被该 CMake 路径引用的原型只作为测试/边界证据，不用于宣称正式产品存在或不存在某项运行时行为。

当前配置真值如下：

| 配置事实 | 证据 | 对本审计的影响 |
|---|---|---|
| ESP-VOCAT V1.0 board | `sdkconfig:979` | MIC 使用 audio-board shared RX lease 路径 |
| battery service 启用 | `sdkconfig:984-992` | battery worker/listener 属于正式产品范围 |
| OTA 启用 | `sdkconfig:998` | CMake 追加 5 个 OTA 源文件 |
| P3 software reference 启用；P3 AFE MR prototype 关闭 | `sdkconfig:1060-1066` | `p3_afe_l3.c` 仍在正式源列表；prototype 条件路径不计为已启用功能 |
| face test mode 关闭 | `sdkconfig:1469`；`face_cache.c:23-36` | cache 为 4 slots，每 slot 259200 bytes |
| 上行 PCM16；Opus 关闭 | `sdkconfig:1193-1210` | audio uplink 的全局 batch workspace 生效；条件 Opus scratch 不进入当前产品二进制 |
| ESP32-S3 双核、non-SMP、FreeRTOS 100 Hz | `sdkconfig:3272-3276` | 普通 C 全局变量不能依赖 `volatile` 或单核假设建立跨核同步 |

## 2. API ownership / reentrancy 矩阵

| 模块/API | 静态或共享状态 | 当前产品调用约束 | 公开契约结论 |
|---|---|---|---|
| `nuwax_mic_input_*` | 全局 owner；资源句柄放在公开 caller object | 已发现调用点使用局部零初始化对象并顺序 cleanup | 重复 init 会先毁掉 live object；owner token 不能区分实例，见 `API-001` |
| `nuwax_p3_l3_*` | AFE handle、MR/OUT buffer、flags/stats 全为模块全局 | 正常 app 路径在 owner task 内处理 capture 和 command | 头文件未声明单 owner；生命周期、process、stats 无同步，见 `API-002` |
| face cache | 4 个全局 slot、LRU、queue、worker | 当前由 `face_engine_init()` 初始化一次 | 重复 init 已登记 `LIFE-008`；跨核 slot race 已登记 `RTOS-006`，本轮不重复编号 |
| `nuwax_led_status_*` | policy singleton + 自有 mutex | product policy wrapper 串行访问 raw driver | wrapper 契约合理；raw API 并发 deinit 风险扩充既有 `LIFE-006` |
| battery service | singleton task、lock、一个 binary stopped signal、listener | 当前 listener 仅更新 UI 状态 | getter callback 重入可行；callback self-stop 和 concurrent stop 未定义，见 `API-003` |
| audio uplink | 每 context 队列/池，但全局 staging batch 和静态 TCB | 当前成功路径保持单 capture/uplink owner | singleton 只在 start 时实施，producer/callback 契约未封闭，见 `API-004` |
| cloud config | active/pending config 和 flags 全为模块全局 | stage/commit 多数经 provisioning owner queue | endpoint getter 可与 owner 写入交错；init/get/set 无同步，见 `API-005` |
| Wi-Fi manager | 多个普通全局状态和 handler singleton | event loop、fallback worker、app caller 并存 | 沿用 `EVENT-001/RTOS-009`、`EVENT-002`，不重复登记 |
| P3 software ref | ring/stats/scratch metadata | 多生产/消费路径 | steady-state copy 受锁保护，但首次建锁非原子，API 还在取锁前读取可被重配置改写的普通全局，见 `API-007` |
| speaker playback / audio board | singleton context、callback table；board refcount/leases | product owner + worker | audio-board lease 受锁保护；speaker init 无 guard/deinit，会替换 live queue/mutex，见 `API-006` |
| meeting / monitoring mode | caller object 内 queue、callbacks、state/capture flags | app 启动期各 init 一次，运行期由 mode owner 使用 | init 先清零 live object，重复/并发 init 会丢 queue 且无生命周期屏障，见 `API-008` |
| wake detector init | singleton、模型/backend、lock、worker、callback pair | app 启动期单 owner；callback self-stop 有延后清理 | 首次并发 init/deinit 无稳定锁，见 `API-009`；无 deadline stop 仍沿用 `RTOS-005` |
| LVGL pre-handler registration | process-global callback/context pair | app 当前启动期注册一次且 context 为 NULL | 两字段无同步发布，运行期替换可形成错配，见 `API-010` |
| touch/BQ27220/QMI8658A scratch | 各 driver 静态 I/O scratch | 各自 I/O API | scratch 访问受各自 I/O mutex 保护，未发现新增问题 |

## 3. 新确认问题

### API-001（P1）：`mic_input` 重复 init 先清空 live object，永久丢失 owner/resource 回收能力

证据：全局 owner 位于 `main/mic_input.c:33-36`；`acquire_owner()` 在 `:71-96` 发现任意 active owner 即返回 `ESP_ERR_INVALID_STATE`。但 `nuwax_mic_input_init_with_owner()` 在尝试获取 owner 前先执行 `memset(mic, 0, sizeof(*mic))`（`:205-213`）。当前 Vocat 路径在 `:220-248` 取得 audio-board refcount/RX lease并只把 raw buffer、RX handle 和 shared 标记存入 caller object；deinit 依赖这些字段及 `owner/owner_acquired` 才能释放 shared RX、refcount、buffer 和全局 owner（`:799-824`）。这些字段通过公开 struct 暴露（`main/mic_input.h:22-46`）。

确定的失败序列是：同一 `nuwax_mic_input_t` 已成功持有 MIC；调用者误重试 init；init 先把唯一的 live handle/owner metadata 清零；全局 owner 检查随后失败；再调用 deinit 也已无法释放资源或把 `s_active_owner` 复位。结果是 shared RX/refcount/buffer 泄漏，且后续所有 MIC owner 可持续被拒绝，通常只能靠整机重启恢复。

同一根因还有两个契约缺口：同一对象的 init/read/deinit 没有 per-object lifecycle lock；`release_owner()` 在 `:99-117` 只比较 owner enum，不比较对象地址或 generation。复制/伪造一个仍带相同 owner enum 的公开 struct，可错误释放当前全局 owner，甚至触发重复释放其公开资源字段。

当前产品调用点在 conversation、meeting、monitoring、wake 和诊断路径中均使用局部零初始化对象并按成功标志顺序 deinit；未发现当前主路径对同一 live object 重试 init。因此本项是可直接由 API 调用触发的公共契约/恢复性 P1，不宣称正常产品流程已发生并发竞态。

关闭条件：init 在任何写入 caller object 前拒绝 live object，或把资源移入模块私有 opaque handle；owner claim 使用 instance/generation token；明确同一对象只允许一个 lifecycle owner；失败回滚必须保持旧 live object 可 deinit。增加“同对象成功 init 后再次 init”“两个 owner 竞争”“复制/陈旧 handle deinit”“read 与 deinit 交错”的故障注入/压力测试，要求 owner、audio-board refcount、RX lease 和 heap 全部回到基线。

### API-002（P1）：`p3_afe_l3` 生命周期与 process/stats 无同步，重复 init 还能把 live AFE 变成不可达资源

证据：`s_l3_enabled`、`s_afe_inited`、AEC handle、chunksize、MR/OUT buffer 和 stats 均为普通模块全局（`main/p3_afe_l3.c:10-20`）；lazy create/allocate 位于 `:45-64`，destroy/free 位于 `:67-73`，enable/disable 位于 `:75-103`，process 在 `:112-151` 使用同一 handle/buffer，stats getter/reset 在 `:154-155`。全模块没有 mutex、atomic lifecycle state 或 in-flight process 计数，`main/p3_afe_l3.h:7-13` 也没有单 owner/task 限制。

若 `nuwax_p3_l3_process_chunk()` 与 `nuwax_p3_l3_set_enabled(false, ...)` 并发，disable 可在 process 读写期间 destroy `s_aec_handle` 并 free `s_mr_buf/s_out_buf`，形成 UAF/heap corruption；enable、stats getter/reset 和 `s_l3_enabled` 也存在普通 C 数据竞争。

重复 init 另有确定的顺序缺陷：先以 `persist=false` 在运行时 enable，使 AFE 已创建但 NVS 仍为 disabled；随后调用 `nuwax_p3_l3_init()`，它在 `:30-40` 无 lifecycle guard 地把 `s_l3_enabled` 重读为 false，却保留 `s_afe_inited=true` 和所有资源；之后 `set_enabled(false)` 会在 `:76` 直接返回，永不执行 teardown。AFE 和两块 buffer 因而被 stranded。

正式 app 当前在 `main/nuwax_ai_partner_main.c:16020` 启动期 init；capture 在 `:10937` process；remote L3 command 通过 `:14283-14289` 执行，而 command queue 在 capture checkpoint（例如 `:8811-8814`）由同一 application owner task drain。该当前 normal path 提供了事实上的串行化，但没有成为模块 ABI，其他 task 仍可直接调用公开 runtime API。

关闭条件：优先把 init/enable/disable/process/stats 收敛到一个 audio owner/control queue；若保留跨 task API，则用 lifecycle mutex + in-flight/refcount，使 teardown 等待所有 process 退出，并用 atomic/锁保护 stats snapshot。init 必须幂等且不能让 enable flag 与资源状态分裂。增加 process/disable/enable/reset 并发压力和 runtime-enable -> repeat-init -> disable 序列测试，要求无 UAF、无泄漏且状态单调可解释。

### API-003（P2）：battery listener 可重入普通 getter，但 self-stop 与并发 stop 没有可线性化语义

证据：battery singleton 的 task、lock、binary `stopped`、`running/stop_requested` 位于 `main/board/battery_service.c:24-55`。`notify_listener()` 在 `:129-140` 锁内复制 callback/context，解锁后调用，worker 在成功/失败采样路径 `:313-328` 同步进入 listener；因此 listener 调用 snapshot getter 不会递归持有 service lock。头文件只说明 callback 运行于 battery-service task 和借用 snapshot 寿命（`main/board/battery_service.h:50-55`），没有禁止 stop 重入。

若 listener 同步调用 `nuwax_battery_service_stop()`，stop 在 `main/board/battery_service.c:413-439` 会等待由当前 worker 自己在 `:353` 发出的 `stopped` 信号；worker 尚未退出 callback，不可能及时发信号，所以该调用必然 timeout。callback 返回后 worker 才能退出并 suspend（`:345-357`），但没有 owner 自动 delete/reap；`running` 也只由成功 stop caller 在 `:433-437` 清除，因此状态可长期停留在“running=true、task 已 suspend”，直到另一个 caller 再次 stop。

两个并发 stop caller 也共享一个 binary completion signal。一个 caller 可消费信号、delete task 并清状态，另一个 caller 仍等待并最终返回 timeout，即便服务事实上已经停止；模块没有 join waiter 数量或 stop generation。

当前 app listener 位于 `main/nuwax_ai_partner_main.c:650-688`，只更新 UI/power snapshot，未发现 self-stop 调用。因此本项是 callback/并发 stop 契约 P2，不是当前 listener 的已证实故障。

关闭条件：检测 `xTaskGetCurrentTaskHandle()==s_service.task` 并拒绝同步 stop，或提供 callback-safe async stop request；由唯一 owner/join 对象负责最终 delete 和状态发布；定义多个 stop caller 是共同等待同一 generation 还是除 owner 外立即返回。测试 listener self-stop、两到三个 stop waiter、stop/start 交错，要求无自等待、无伪 timeout、无悬挂 task。

### API-004（P2）：audio uplink 用 per-context ABI 暴露 process-global staging，sender callback 重入约束也未闭合

证据：PCM/packet staging `s_audio_upload_batch` 和静态 task TCB/claim flag 位于 `main/conversation/audio_uplink.c:67-70`；条件 Opus scratch 在 `:124-200`。start 在 `:483-623` 拒绝 live context，并以 `:406-420` 的全局 claim 保证只有一个 sender TCB 成功创建。但 `nuwax_audio_uplink_enqueue_frame()` 从 `:805` 开始只检查 `ctx/samples` 和 frame state，没有先要求 `ctx->task/free_slots/ready_packets/pool` 有效，随后在 `:846-1016` 读写全局 batch。未 start 的第二个 context 因而可在第一个 live context 期间改写同一 workspace；同一 context 的两个 producer 也会并发修改 batch metadata 和全局 buffer。

sender 在 final packet 后先调用 `after_end`（`:378-395`），之后才发布 `finished=true`（`:398-403`）。注册 API 在 `:1069-1089` 保存 callback/user context；头文件只说明 callback 在 sender task 执行（`main/conversation/audio_uplink.h:85-88`），没有声明 user context 必须活到 finished，也没有禁止 callback 调用同步 `finish()`/`destroy()`。这种重入会让 sender task等待自己发布 finished；`finish/destroy` 在 `main/conversation/audio_uplink.c:1149-1223` 随后可进入 timeout、连接关闭乃至重启恢复路径。

当前产品调用点通过 MIC/exclusive-mode owner 保持一个成功 uplink context，且按 start -> 单 producer enqueue -> finish/destroy 顺序使用；当前配置还关闭了 Opus scratch。因此本项登记为 ABI/owner/lifetime P2，不宣称正常产品路径已有两个 producer。

关闭条件：把 staging 移入 context，或在每个 producer API 入口校验 active generation 并强制唯一 producer task；singleton claim 应覆盖所有会触碰 process-global workspace 的入口。callback 必须仅投递 owner event，或头文件明确禁止 finish/destroy/self-wait并提供 sender-context 检测；user context 由 owner 在 finished/join 后释放。测试第二个未 start context enqueue、双 producer、after_end 调用 finish/destroy 和 callback context 提前失效。

### API-005（P2）：cloud config 的 active/pending snapshot 在 endpoint callback 与 provisioning owner 间无同步

证据：`s_initialized`、`s_has_config`、`s_has_pending`、`s_config` 和 `s_pending` 是普通静态全局（`main/nuwax_provision_new/nuwax_cloud_config.c:16-20`）。init 在 `:115-180` 读取 NVS并 reset/发布这些全局；set/stage/commit/discard 在 `:183-288` 写入 active/pending struct 和 flags；`has_pending/get/has_config` 在 `:291-309` 无锁读取。并发首次 init 也可能同时打开 NVS并改写同一内存。

`vox_config` 经 job queue 交给 provisioning owner，最终在 `main/nuwax_provision_new/nuwax_provision_main.c:795` stage，并在 owner loop `:1350-1407` 附近处理/commit；但 `device_info_handler()` 是 provisioning-manager endpoint callback（`:623-690`），会直接调用 `has_pending/has_config`，模块内没有证据保证它不与 owner 的 stage/commit/discard 重叠。该当前 endpoint 直接竞争的是 `s_has_pending/s_has_config` 两个普通 flag；完整 `s_config/s_pending` struct 的 torn copy 风险来自其他 task 并发调用公开 `get()` 与 set/stage/commit/discard。两类入口都可能观察到 flag 与 struct 属于不同 revision。

关闭条件：由单 provisioning owner 持有所有 cloud-config 读写，并向 endpoint 返回 immutable revisioned snapshot；或以同一 mutex 保护完整 active/pending snapshot 和 flags，NVS I/O 可在预留 state/revision 后解锁执行并在提交时复核 generation。增加 endpoint read 与 stage/commit/discard/首次 init 交错测试，要求每个 snapshot 自洽、revision 单调且 NVS 与 RAM 最终一致。

### API-006（P1）：speaker playback 重复 init 会替换 live worker 使用的 queue/mutex，并泄漏旧对象

证据：`nuwax_speaker_playback_init()` 在 `main/conversation/speaker_playback.c:1518-1565` 没有 initialized guard、lifecycle lock 或配套 deinit。它先直接覆盖 callback table 和全部运行状态，再无条件创建新 `packets` queue 与 `mutex` 并覆盖 process-global handle。若 singleton 已初始化，旧 queue/mutex 永久失去引用；若 speaker worker 或 WebSocket ingress 正在运行，其他入口可能已经在等待/使用旧 mutex 或 queue，而后续入口开始使用新对象，形成两个不同同步域、数据竞争、旧 queue 中帧不可达以及潜在 use-after-delete/永久泄漏。第二次 queue 分配成功但 mutex 分配失败时，代码只删除新 queue，并把 `packets=NULL`，旧 live queue/mutex 同样无法恢复。

正式 app 目前只在 `main/nuwax_ai_partner_main.c:15605` 启动调用一次，未发现正常流程主动重复 init；但该函数是公开 API，重复调用的后果会直接破坏正在运行的核心播放链路，因此登记 P1 API/lifecycle 缺陷。

关闭条件：由唯一 application owner 实施一次性、并发安全的 init state machine；live 时重复 init 必须幂等或返回 `ESP_ERR_INVALID_STATE`，且不得在判定前改写任何状态。若产品需要重新初始化，必须提供 stop -> worker join -> callback drain -> queue/mutex delete -> reinit 的完整 deinit。测试启动后重复/并发 init、worker 正在取 queue、RX 正在 ingress、第二次 mutex 分配失败等交错，要求对象数量守恒、旧 worker 不访问失效同步对象且 callback/generation 不倒退。

### API-007（P2）：P3 software reference 首次建锁与重配置前置读不具可重入性

证据：`main/p3_software_ref.c:172-179` 在没有稳定初始化锁的情况下执行 `if (!s_ref.lock) -> xSemaphoreCreateMutex() -> s_ref.lock=...`。两个首次并发 init 可以各创建一把 mutex、后写覆盖 handle，并让两个 caller 分别持不同 mutex 修改同一个 `s_ref` singleton。`write_samples()` 在 `:105-115`、read APIs 在 `:323-337,365-388` 还会在取得 mutex 前普通读取 `initialized/buffer/capacity_samples`；init/reconfiguration 在 mutex 内改写并释放这些字段，因此前置读与写方没有 C happens-before，属于数据竞争。取得当前 lock 后的第二次检查能减少实际陈旧指针解引用，但不能消除首次双锁或 C 内存模型缺陷。

当前正式启动首次 init 串行，steady-state copy/memset/stats 主体也在 mutex 内；因此这是公共 API/重配置 P2，不宣称当前正常对话已复现内存破坏。

关闭条件：用静态 portMUX/一次性 init primitive 串行发布唯一 mutex；所有 initialized/buffer/capacity metadata 的读写进入同一同步域，或发布 immutable generation snapshot；若允许重配置，先阻止新读写并等待 in-flight=0 再交换/free buffer。测试两个并发首次 init、feed/read 与不同容量 re-init 交错及 allocation failure，要求只有一把 live lock、无 torn metadata/UAF、旧 buffer 仅在无 reader 后释放。

### API-008（P2）：meeting/monitoring init 先清零 live caller object，重复调用永久丢失 queue

证据：meeting init 在 `main/audio_modes/meeting/meeting_mode.c:950-974` 先 `memset(mode, 0, sizeof(*mode))`，再创建 event queue 和 request-ACK queue；monitoring init 在 `main/audio_modes/monitoring/monitoring_mode.c:1113-1130` 采用相同顺序并创建 event queue。两者都没有 initialized guard、对象锁或 run/RX/deinit 的 in-flight 屏障。对 live object 重复 init 会在拒绝机会出现前丢失旧 queue handle；meeting 每次丢 events 与 request_acks，monitoring 丢 events。与 run/RX callback/deinit 并发时，还可能让 producer/consumer分别访问新旧 queue或让 deinit删除刚被其他路径取得的 handle。

正式 app 当前仅在 boot 对两个静态对象各初始化一次；运行期切换模式并不重复 init。因此登记为 P2 公共对象生命周期缺口，并与既有 `WS-CB-002` 的跨 task state 读取区分：前者是 init/deinit 契约，后者是 live mode 状态可见性。

关闭条件：先在稳定 lifecycle lock/atomic state 下检查 UNINITIALIZED，成功创建全部对象后一次性发布 READY；失败只清理本次临时对象，不覆盖 live object。deinit 先拒绝新事件、等待 run/RX/ACK waiter 退出，再删除 queue。测试重复/并发 init、init allocation failure、run/RX/deinit 交错，要求 queue 创建/删除守恒且旧 session 不污染新 generation。

### API-009（P2）：wake detector 首次 init/deinit 在稳定锁建立前可并发覆盖 singleton

证据：`main/wake_word_detector.c:577-599` 先无锁读取 `s_detector.initialized`；首次路径随后 `memset` 整个 singleton、初始化 atomics并创建 lock。两个并发首次 init 可分别通过检查、交错清零 singleton并创建/覆盖不同 lock，随后重复初始化模型/backend资源。deinit 会删除 lock并清零结构，公开 init/set callback/start/stop 与 deinit 之间也没有独立于可删除对象的 lifecycle gate。当前 callback worker-context stop/deinit 会延后 owner cleanup（`:255-263,831-884`），这是正向控制，但不覆盖首次初始化竞争。

正式 app 在 boot 由单 owner init，未发现产品路径并发首次调用；因此本项为 P2 API 契约缺口。无总 deadline stop/reap 继续沿用 `RTOS-005`，不重复计数。

关闭条件：使用永久存在的 lifecycle lock/atomic state 实现 UNINITIALIZED -> INITIALIZING -> READY -> STOPPING -> STOPPED；只有 owner 在 worker EXITED 后释放模型和运行锁。并发 init 只能共同观察同一结果或被明确拒绝。测试双 init、init/deinit/start交错及逐点模型分配失败，要求资源守恒、callback pair自洽且状态不倒退。

### API-010（P2）：LVGL pre-handler callback/context 不是一个原子发布单元

证据：`nuwax_lvgl_port_set_pre_handler_cb()` 在 `main/nuwax_lvgl_port.c:568-570` 依次普通写 `s_pre_handler_cb` 和 `s_pre_handler_user_data`；LVGL task 在 `:364-366` 普通读取 callback 后，再直接从全局读取 context。运行期替换时可观察到新 callback + 旧 context 或旧 callback + 新 context，且 callback/context 普通跨任务读写本身没有同步。当前 app 仅在启动期注册一次且 user data 为 NULL，所以没有证据表明正常路径已触发。

关闭条件：在 LVGL mutex/critical section内发布并读取完整 `{cb, ctx, revision}` snapshot，或只允许在 LVGL task尚未启动时注册并由 API 明确拒绝 live replacement；解绑需等待已进入 callback退出。测试运行期快速替换/解绑和 callback长执行，要求每次调用的 callback/context来自同一 revision。

## 4. 既有问题交叉引用与本轮补证

### Face cache：沿用 `LIFE-008` 和 `RTOS-006`

`face_cache_init()` 在 `main/nuwax_ui_v28/face/face_cache.c:459-494` 每次直接覆盖 `s_slots[i]`，没有 initialized guard，也不释放旧 slot。当前 4-slot 配置下，每次重复 init 会丢失 4 × 259200 = 1036800 bytes（约 0.99 MiB）旧 PSRAM 指针。queue/task 在 `:470-489` 有存在性 guard，会被保留而不是重复创建，因此重复 init 还会让 live worker/UI 与被重置的 slot metadata 交错。当前唯一直接初始化入口为 `face_engine_init()`（`main/nuwax_ui_v28/face/face_engine.c:1264-1265`）。

该 repeat-init 泄漏已由 `phase1-worker-lifecycle-audit.md` 的 `LIFE-008`（P2）登记；worker CPU0 与 LVGL/UI CPU1 对 slot metadata/buffer 无锁共享已由 `phase1-architecture-rtos-audit.md`/`phase1-queue-and-lifecycle-audit.md` 的 `RTOS-006`（P1）登记。本轮不创建新的 face ID。关闭仍需幂等 init 或显式拒绝重复调用、完整 deinit/join，以及 versioned publish/单 owner cache 更新压力测试。

### LED：沿用并扩充 `LIFE-006`

policy wrapper 在 `main/board/led_status.c:78-111,114-173,182-203` 用自己的 mutex 串行 init/set/deinit；头文件 `main/board/led_status.h:26-29` 明确它是产品唯一 LED owner。该正式 wrapper 路径的 owner 契约成立。

raw driver 的 setter 在 `main/board/led_driver.c:197-220,233-280` 先无锁检查 `s_led.initialized/s_led.lock`，再 take mutex；解锁后使用 task handle 快照 notify。deinit 在 `:288-315` 等 worker、delete task、delete lock/stopped。除既有 `LIFE-006` 所述 stale task notify 外，setter 可在 precheck 后被抢占，deinit 删除 mutex，setter 再对旧 mutex handle执行 `xSemaphoreTake()`；两个 concurrent deinit caller 还会争用同一个 binary stopped 信号。以上是同一 raw lifecycle 没有串行化的扩充证据，不另建 ID。直接 raw API 当前主要由 hardware self-test/诊断使用；产品 policy caller 不应绕过 wrapper。

### Wi-Fi：沿用 `EVENT-001/RTOS-009` 和 `EVENT-002`

`wifi_manager.c:298-371` 的 partial-init handler/netif/event-group rollback 缺口已登记 `EVENT-001/RTOS-009`；event-loop callback、fallback worker、app caller 对 `wifi_manager.c:40-53,244-295,431-475,552-601,608-613,781-788` 普通全局状态的竞争已登记 `EVENT-002`。这些均属于 singleton/callback reentrancy 范围，但本轮不重复计数。

### Barge dispatcher：沿用并扩充 `LIFE-001`

`main/conversation/barge_in_dispatcher.c:332-365` 的存在性检查和首次 lock/queue/task 创建没有永久 init gate。两个并发 `bind()` caller都可进入 `nuwax_barge_dispatcher_init()`，分别创建并覆盖lock/queue/task，形成多worker、不同同步域和旧对象泄漏；部分初始化失败的反向回滚缺口已由 `LIFE-001`登记。本轮扩充并发首次init证据，不另建API编号。

## 5. 已核对的正向控制

1. `main/p3_software_ref.c:115-155,337-355,388-412,453-588,667-717`：获得稳定 mutex 后的共享 ring、copy/memset 和 stats 主体由同一 mutex 保护，且未见锁内 callback或反向锁序；首次建锁和取锁前普通读的例外已登记 `API-007`，不能再概括为全面并发安全。
2. `main/conversation/speaker_playback.c:516-565,3675-3718,3741-3788`：已初始化后的若干状态路径会在 mutex 下 snapshot，并把 callback/I/O移到解锁后；这不能覆盖 init 本身，重复 init 的独立 P1 已登记 `API-006`。
3. `main/board/audio_board.c:18-23,59-83,114-201`：singleton refcount 和 RX/TX lease 在统一同步域；`main/board/audio_board.h:73-100` 给出 shared channel ownership 契约。
4. touch、BQ27220、QMI8658A 的静态 scratch 分别在 `main/touch_input.c:70-74,363,658`、`main/board/bq27220_driver.c:76-78,454-572`、`main/board/qmi8658a_driver.c:50-51,225-333`，访问受各自 I/O mutex 保护。
5. wake callback lifecycle 会识别 worker-context stop/deinit并延后 owner cleanup（`main/wake_word_detector.c:255-263,831-884`）；独立的无总 deadline stop 风险仍沿用 `RTOS-005`，不在本轮重复登记。

## 6. 后续动态关闭门槛

| 验证项 | 建议等级 | 最小场景 | 通过条件 |
|---|---|---|---|
| MIC owner/retry | B/D | live object重复 init；两个 owner竞争；陈旧/复制 object deinit；read/deinit交错 | 无 lease/refcount/heap 泄漏；陈旧对象不能释放新 generation；后续 owner 可正常获取 |
| P3 L3 lifecycle | B/D | process 与 enable/disable/reset 高频交错；runtime-only enable -> repeat init -> disable | 无 UAF/内存破坏；AFE/buffer分配释放守恒；stats snapshot 自洽 |
| battery callback/stop | B | listener self-stop；2~3个 stop waiter；stop/start交错 | 无 self-wait、伪 timeout、悬挂 task；所有 waiter观察同一 generation结果 |
| audio uplink owner/reentry | B | live context期间第二 context enqueue；双 producer；after_end重入 finish/destroy | 第二 owner在写 workspace前被拒绝；无 batch corruption、自等待、提前释放 user context |
| cloud config snapshot | B | endpoint getter 与首次 init/stage/commit/discard交错 | snapshot属于完整 revision；RAM/NVS最终一致；无 torn struct/flag |
| speaker init lifecycle | B/D | live worker/RX ingress期间重复/并发 init；第二次分配逐点失败 | live init被无副作用拒绝或幂等；queue/mutex数量守恒；无新旧同步域并存 |
| P3 software ref init/reconfig | B | 双首次 init；feed/read 与不同容量 re-init交错 | 唯一 lock；无 metadata race/UAF；旧 buffer在 reader退出后释放 |
| meeting/monitoring object lifecycle | B | 重复 init；run/RX/deinit交错；queue allocation failure | queue守恒；旧 session不能访问/删除新 generation对象 |
| wake/LVGL callback contract | B | wake双 init/deinit/start交错；LVGL callback/context快速替换/解绑 | 状态单调；资源守恒；callback/context同 revision且解绑后无迟到调用 |
| face/LED既有项 | B/D | face重复 init+快速表情切换；raw LED setter/deinit/concurrent deinit | 关闭 `LIFE-008/RTOS-006/LIFE-006` 的原有验收条件，不因本轮另起重复用例 |

本轮没有执行上述测试；表中是后续 B/D 级关闭条件，不得解释为产品已通过。

## 7. A1-07 / 阶段 1.17 反向校验

| 计划 1.17 要求 | 本文件证据 | 静态状态 |
|---|---|---|
| 静态 workspace / global scratch | P3 L3/software ref、face cache、audio uplink、cloud config | 已覆盖；`API-002/004/005/007` 及既有 face IDs |
| singleton / 单 owner / 指定 task | MIC、speaker、LED、mode、wake、audio uplink/board、LVGL callback | 已覆盖；缺口见 `API-001/004/006/008/009/010`、`LIFE-006` |
| 重复 init/deinit 与失败重试 | MIC、P3 L3/software ref、speaker、mode、wake、face、LED、battery、cloud config | 已覆盖；新旧 findings均给出关闭序列 |
| 同步 API 被 callback 重入 | battery listener、audio uplink `after_end`、wake、LVGL pre-handler | 已覆盖；`API-003/004/009/010` |
| 递归入口、C library/`errno` | 核对公开入口及静态 buffer调用；未发现独立 `errno`/C library共享状态问题 | A级未形成新 finding；仍需动态重入测试验证时序 |
| 正式产品路径与 host/unlinked 分离 | CMake 149项反向枚举；host check/stub仅作边界证据 | 已完成，不把未链接原型当产品能力或产品缺陷 |

## 8. 阶段结论

- 未确认新的 P0。
- 新登记 10 项：`API-001/002/006` 为 P1；`API-003/004/005/007/008/009/010` 为 P2。
- Face、LED 和 Wi-Fi 风险分别沿用 `LIFE-008/RTOS-006`、`LIFE-006`、`EVENT-001/RTOS-009/EVENT-002`，本轮只补足重复 init、raw lifecycle和 owner/callback 证据，不重复计数。
- 正式主路径对 P3 L3/software ref、MIC、speaker、meeting/monitoring、wake、audio uplink 和 LED 建立了不同程度的事实 owner 串行化；这些约束尚未全部写入公开 ABI，也不能替代模块内同步或显式 reject-on-reentry。
- A1-07 的 A 级静态审计完成；B/D 级并发、故障注入和实机资源守恒验证仍开放。
