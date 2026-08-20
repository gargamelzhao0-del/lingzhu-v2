# 阶段 1：架构、任务与实时系统只读审计

审计日期：2026-08-02  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
权限：A 级只读；未修改业务源码、未构建、未运行 host test、未操作 COM5、未写 NVS、未 OTA。

> 2026-08-03 后续状态：`RTOS-006` 已实现跨核 slot 状态锁、rendering 失效、完成后发布和 LVGL buffer 保护协议；host source contract 和 ESP-IDF 6.0 构建通过。双核 stress、快速表情切换和画面完整性实机验证仍开放。详见 `implementation-verification-2026-08-03.md`。下文保留原始发现快照。

## 1. 审计边界与证据

本批次先完成 A1-01“任务与系统任务联合清单”。检查采用 `rg` 枚举任务创建点、`sdkconfig` 和 ESP-IDF v6.0 本地头文件/文档交叉核对，再读取创建点上下文确认优先级、核心、栈和停止 owner。源码行号以本批次读取结果为准；源码或配置发生变化后，本证据自动失效。

关键外部证据：

- `sdkconfig`：FreeRTOS 100 Hz、双核、ESP main task 16 KiB/CPU0、timer task 3584 bytes/CPU0、TCP/IP priority 18/stack 3072、Wi-Fi task pinned CPU0、NimBLE host stack 8192/CPU0、task WDT 10 s 且 panic 关闭。
- ESP-IDF v6.0 `components/esp_system/include/esp_task.h`：`ESP_TASK_PRIO_MAX=25`、timer task priority `25-3=22`、TCP/IP priority 来自 `CONFIG_LWIP_TCPIP_TASK_PRIO`、main task priority `ESP_TASK_PRIO_MIN+1`。
- ESP-IDF v6.0 `docs/en/api-guides/external-ram.rst`：允许外部 RAM 栈需要显式配置；`external-ram-esp32-notes.rst` 明确不建议将会间接调用 ROM 的任务放在 PSRAM 栈上。
- ESP-IDF v6.0 `docs/en/api-guides/performance/ram-usage.rst`：ESP-IDF 上 `uxTaskGetStackHighWaterMark()` 返回字节数，不能按标准 FreeRTOS 的字数解释。

## 2. 产品候选任务图（静态）

“常驻”表示在正式候选配置的正常启动路径创建并持续运行；“按需”表示连接、唤醒或音频流开始后创建/启动；“诊断/条件”不计入正常产品闭环，但仍需在后续阶段审计其构建隔离。

| 任务/线程 | 创建点 | 优先级/核心 | 栈与内存 | 停止/owner 结论 |
|---|---|---:|---|---|
| `app_main`（IDF） | ESP-IDF `app_startup.c` | 1 / CPU0 | 16384 bytes，IDF 管理 | 系统 owner；主循环不返回 |
| `nuwax_flash_ops` | `main/nuwax_ai_partner_main.c:15698` | 6 / 无固定核 | 4096 bytes；TCB internal、栈 internal | 静态 helper 创建，无应用停止入口；进程生命周期 |
| `nuwax_shell` | `main/nuwax_ai_partner_main.c:15908` | 3 / CPU1 | 4096 bytes；TCB internal、栈 PSRAM | 无句柄返回；常驻循环 |
| `nuwax_touch`（日志/输入） | `main/touch_input.c:719` | 5 / 无固定核 | `CONFIG_NUWAX_TOUCH_TASK_STACK_SIZE`=4096；internal | `s_touch_log_task` 保存，但当前没有 deinit/stop API；GPIO ISR 与 fallback poll 常驻 |
| `nuwax_touch_gesture` | `main/nuwax_ai_partner_main.c:15920` | 5 / 无固定核 | config=16384；TCB internal、栈 PSRAM | 无句柄返回；常驻循环 |
| `battery_service` | `main/board/battery_service.c:386` | 4 / 无固定核 | 4096；WithCaps PSRAM | `s_service.task` 保存；stop 等 stopped 后 `vTaskDeleteWithCaps` |
| `nuwax_power_key` | `main/board/nuwax_power_ctrl.c:387` | 4 / 无固定核 | 3072；普通动态栈 | 无停止 API；无限轮询，物理关机依赖电源切断 |
| `nuwax_usb_ctrl` | `main/nuwax_provision_new/nuwax_provision_usb_control.c:97`（配置开启） | 3 / 无固定核 | 3072；WithCaps PSRAM | 无句柄/停止 API；直接持有 USB Serial/JTAG 驱动生命周期 |
| `nuwax_wifi_scan` | `main/nuwax_ai_partner_main.c:16049` | 4 / 无固定核 | 8192；TCB internal、栈 PSRAM | 句柄保存；常驻通知等待 |
| `nuwax_wifi_conn` | `main/nuwax_ai_partner_main.c:16052` | 4 / 无固定核 | 4096；TCB internal、栈 PSRAM | 无句柄返回；常驻队列等待 |
| `wifi_fb_watch` | `main/wifi_manager.c:400` | 5 / 无固定核 | 2048；普通动态栈 | 无句柄/停止 API；1 s event wait 常驻 |
| `p3_softref_ab` | `main/nuwax_ai_partner_main.c:16055` | 5 / 无固定核 | 8192；TCB internal、栈 PSRAM | 无句柄返回；常驻 request wait |
| `p5_candidate` | `main/nuwax_ai_partner_main.c:16058` | 5 / 无固定核 | 24576；TCB internal、栈 PSRAM | 无句柄返回；常驻 request wait |
| `nuwax_speaker` | `main/nuwax_ai_partner_main.c:16066` | 10 / 无固定核 | 6144；TCB internal、栈 PSRAM | 无句柄返回；常驻播放控制循环 |
| `face_cache` | `main/nuwax_ui_v28/face/face_cache.c:476` | 2 / CPU0 | 4096；WithCaps PSRAM | 无停止 API；常驻 preload queue |
| `ota_controller` | `main/nuwax_ota_new/nuwax_ota_device.c:1237` | 6 / 无固定核 | 16384；普通动态栈 | `s_controller_task` 保存；模块没有完整 deinit/stop 闭环证据 |
| `barge_dispatch` | `main/conversation/barge_in_dispatcher.c:352` | 8 / 无固定核 | 6144；WithCaps PSRAM | 静态单例；bind/unbind 只解除连接，不停止 worker |
| `nuwax_ws_rx` | `main/ws_transport.c:1520` | 10 / 无固定核 | `CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES`=12288；WithCaps PSRAM | 连接级；RX 发布 EXITED 后 owner 等待/删除 |
| `nuwax_wake` | `main/wake_word_detector.c:788` | 4 / 无固定核 | 8192；静态 TCB internal、栈 PSRAM | 按需；显式 `RUNNING -> EXITING -> REAPING -> EXITED -> STOPPED` |
| `nuwax_mic_rx` | `main/nuwax_ai_partner_main.c:7263` | 12 / CPU0 | 6144；静态 TCB、栈 PSRAM | 按需 singleton；owner 请求 stop，超时直接 `esp_restart()` |
| `nuwax_audio_tx` | `main/conversation/audio_uplink.c:590` | 9 / CPU1 | 10240；静态 TCB、栈 PSRAM | 按流 singleton；owner stop/reap 逻辑需在 A1-04 继续核对 |
| BLE provisioning/NimBLE host、event loop、TCP/IP、Wi-Fi、timer、idle | ESP-IDF/组件内部 | TCP/IP=18；timer=22；NimBLE CPU0；Wi-Fi CPU0；其余由 IDF 配置 | 见 `sdkconfig` | 外部组件 owner；本工程只能通过注册/注销与状态 API 管理 |

## 3. A1-01 静态结论

### 已确认

1. 正常产品路径不是只有 `app_main`：至少包含连接级 WebSocket RX、流级 mic/audio TX、按需 wake 和常驻 UI/网络/电源/OTA worker；后续状态和资源审计必须按这些生命周期分层。
2. 音频采集 `nuwax_mic_rx` 固定 CPU0、优先级 12；Wi-Fi 固定 CPU0，TCP/IP priority 18，timer task priority 22。采集任务不能抢占这些系统任务，网络重连/定时器回调高负载时存在可观测抖动风险，必须在阶段 9 以 p95/p99 和丢帧数据验证，不能仅凭代码宣称实时性。
3. 多数应用 worker 的 TCB 放 internal、栈放 PSRAM，并依赖 `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`；这符合当前配置，但 ESP-IDF 文档对会间接调用 ROM/缓存禁用路径的 PSRAM 栈任务明确给出不建议，需在阶段 3/8 对 flash/NVS/OTA 期间的调用路径做专项证明。
4. 一部分常驻 worker 没有句柄或 stop API（电源键、USB 控制、Wi-Fi fallback、shell、gesture、speaker、face cache、barge dispatcher 等）。这对一次性启动可成立，但错误回滚、运行期重配网、OTA 前停机和测试重复 init 不能直接假定安全。
5. `CONFIG_ESP_TASK_WDT_PANIC` 未启用，当前配置只保证 WDT 记录/告警而非自动 panic/reset；代码也未见统一的应用 worker heartbeat/订阅矩阵。阻塞但不占满 CPU 的 worker 可能不会被 idle WDT 发现，需登记为稳定性风险。

### 未能由静态代码证明

- ESP-IDF Wi-Fi、NimBLE、event loop 和 TLS 内部任务的精确运行时优先级/栈余量，以及它们在不同 IDF 构建选项下的变化。
- PSRAM 栈任务在 flash cache disabled、OTA 写入、NVS commit 和异常回调中的实际安全性。
- 所有任务的 stack high-water、CPU 占用、调度延迟和 WDT 触发行为；需要 B/C/D 级证据。

## 4. 开放问题登记（本阶段新增）

| ID | 等级 | 位置/范围 | 结论 | 后续动作 |
|---|---|---|---|---|
| RTOS-001 | P1 | `sdkconfig:2974~2983`、应用任务创建点 | Task WDT 开启但 panic 关闭，且没有统一 worker heartbeat/订阅和有限恢复协议；阻塞 worker 可能长期无声失效 | 阶段 2/8/10：建立任务 heartbeat、WDT 策略和故障注入证据；未授权前不改配置 |
| RTOS-002 | P1 | `main/nuwax_ai_partner_main.c`、`audio_uplink.c`、`ws_transport.c`、`face_cache.c` 等 PSRAM 栈任务 | 多个实时/网络任务依赖外部栈；IDF 文档明确不建议涉及 ROM/缓存禁用路径的任务使用 PSRAM 栈，当前没有逐任务安全证明 | 阶段 3/8：按调用路径分类，必要时将关键任务迁移 internal，并用构建/实机验证 |
| RTOS-003 | P1 | `nuwax_mic_rx` CPU0 prio12；Wi-Fi CPU0；TCP/IP prio18；timer prio22 | 音频采集与系统网络/定时器抢占关系存在延迟预算风险；静态代码不能证明高负载下不丢帧 | 阶段 4/9：trace、队列高水位、丢帧和 p95/p99 测量 |
| RTOS-004 | P2 | `nuwax_provision_usb_control.c:97`、`nuwax_power_ctrl.c:387` 等 | 常驻任务无统一 owner/stop/reap，运行期重配网、OTA 前停机和重复 init 的资源回收契约不完整 | 阶段 2/6：补任务生命周期矩阵和重复初始化/失败回滚测试 |

## 5. A1-01 计划校验

- 计划条款 1.1、1.13、1.15 已逐项有任务清单、配置证据和未决验证项。
- A1-01 状态：**已完成（A级静态）**。这不等同于任务运行时通过；运行时栈、CPU、WDT 和故障恢复留在后续授权阶段。
- 下一项：A1-02 锁图和无限等待白名单。

## 6. A1-02 锁图与无限等待白名单（A级静态）

本批只读核对范围：`xSemaphoreTake(..., portMAX_DELAY)`、`xQueueReceive(..., portMAX_DELAY)`、`ulTaskNotifyTake(..., portMAX_DELAY)`、`xTaskNotifyWait(..., portMAX_DELAY)`、`spi_device_acquire_bus(..., portMAX_DELAY)`、`vTaskDelay(portMAX_DELAY)`、任务 stop/reap 路径、跨核共享状态和回调持锁边界。没有构建、没有运行 host test、没有连接 COM5，也没有读取 ESP-IDF provisioning manager 源码实现，因此凡依赖第三方组件“回调已完全 quiesce”的结论均标为静态边界。

### 6.1 锁图（已核对主路径）

| 锁/资源 | owner/创建点 | 获取点 | 释放/等待策略 | A1-02 静态结论 |
|---|---|---|---|---|
| LVGL 全局互斥 `s_lvgl_mutex` | `main/nuwax_lvgl_port.c:420~425` | LVGL task 每轮 `xSemaphoreTake(..., 50ms)`；外部 `nuwax_lvgl_port_lock(timeout_ms)` | LVGL task handler 后释放；外部调用方用有限 timeout | 白名单：外部 UI 调用不应无限等 LVGL；但 face cache 不受该锁保护，见 RTOS-006 |
| face cache slot metadata/buffer | `main/nuwax_ui_v28/face/face_cache.c:459~493` | LVGL/UI 侧 `face_cache_get/protect/defer/set_suspended`；core0 worker `render_into_cache` | 无 mutex/critical/atomic（除两个 `volatile` flag） | 非白名单：跨核共享 slot metadata 与 buffer，见 RTOS-006 |
| speaker ctx `s_speaker_ctx.mutex` | `main/conversation/speaker_playback.c:197~205` | speaker 状态机多处 `speaker_lock()` | `portMAX_DELAY` 本地互斥；主要回调和 I/O 在解锁后执行，但少量 output init/volume setter 在锁内 | 条件白名单：未发现 output/p3 反向获取 speaker ctx；锁序为 `speaker_ctx -> speaker_output.lock -> p3_software_ref.lock`。后续需实测锁持有时间 |
| speaker output lock | `main/speaker_output.c:209~242` | `speaker_output_lock(lock, timeout_ms)` | volume/write/stop/deinit 均有限 timeout 或错误返回 | 白名单：不是无限锁；但被 speaker ctx 锁内调用，需阶段 4/9 量测长尾 |
| P3 software ref `s_ref.lock` | `main/p3_software_ref.c:172~179` | read/write/reset/stats/history span | `portMAX_DELAY`；保护 ring metadata/buffer；未见回调或外部 I/O 反向取锁 | 条件白名单：本地 bounded copy/memset 临界区；大 span 仍需运行时耗时与高水位证据 |
| Wi-Fi connect mutex `s_wifi_connect_mutex` | `main/nuwax_ai_partner_main.c:15662` | `begin_wifi_connect_operation()`；worker 用 `portMAX_DELAY`，recovery 用 0 | `end_wifi_connect_operation()` 释放 | 条件白名单：用于串行化 Wi-Fi 连接 worker；若 holder 卡死，worker 会静默等待，当前 WDT 不覆盖业务 worker |
| BLE `vox-config` gate/queue/job done | `main/nuwax_provision_new/nuwax_provision_main.c:806~827` | BLE endpoint handler、main provisioning loop、stop/drain | gate 1s；job done `portMAX_DELAY`；stop 删除 queue/gate | 非白名单：handler in-flight 与 stop/delete 无模块内 refcount/join 证明，见 RTOS-007 |
| wake detector lock/task stop | `main/wake_word_detector.c:169~177`、`:831~843` | stop/request/is_running/reap | lock 无限互斥；stop 无 deadline；reap 自旋等状态 | 非白名单：状态机卡住会阻塞主控，见 RTOS-005 |
| display SPI bus | `main/display.c:825~854` | `spi_device_acquire_bus(s_spi, portMAX_DELAY)` | display init 持有专用 SPI2 device/bus | 条件白名单：当前 `main/` 只发现 display 使用 `SPI2_HOST`；仍需实机确认无外部组件复用 |
| WebSocket RX stop | `main/ws_transport.c:1129~1135` 等 | stop/join 轮询 `rx_task_running` | 调用方传入 timeout；先前核对最长 5s stop 路径 | 白名单：非无限 join；保留 owner 后续回收 |
| `esp_restart()` 后 `vTaskDelay(portMAX_DELAY)` | `main/conversation/audio_uplink.c:1217~1222`、`main/nuwax_ai_partner_main.c:7330~7337` | stop 超时失败路径 | 触发 restart 后停止释放 live resources | 白名单：故障终止哨兵，不是业务等待 |

### 6.2 无限等待白名单

| 类型 | 位置 | 结论 |
|---|---|---|
| 常驻 worker 空闲等待 | `wifi_scan_task`、`flash_ops_task`、`wifi_connect_task`、`p3_softref_ab_fast_task`、`p5_during_playback_candidate_fast_task`、`face_cache_preload_task`、`barge_dispatch_task` | 队列/通知等待本身可接受；风险只在 owner stop/delete、消息处理长阻塞或 WDT 覆盖不足时成立 |
| 本地短临界区 mutex | `barge_in_dispatcher.c`、`mic_input.c`、`p3_software_ref.c`、`speaker_playback.c` | A1-02 未发现“持锁后等待同锁 owner 回调”的闭环；但 `speaker_ctx -> output -> p3` 的长尾需后续实测 |
| 专用硬件独占 | `display.c:854` | 静态白名单候选：SPI2 bus 当前由 display 独占；需要设备级确认 |
| 失败后停止 | `vTaskDelay(portMAX_DELAY)` after `esp_restart()` | 白名单：已选择重启，不再释放 live resource |

### 6.3 非白名单触发链

1. Wake detector stop 无 deadline：`stop_sleep_wake_word_detector()` 同步调用 `nuwax_wake_word_detector_stop()`，后者只轮询 `is_running()`；若 worker 卡在模型/驱动或状态迁移未到 STOPPED，主控状态机会被永久卡住。
2. BLE `vox-config` ACK/in-flight stop：handler 入队后 `job->done` 无限等待；stop/drain/delete 路径缺少 handler in-flight 引用计数和“回调全部退出后再 delete queue/gate”的本地证明。
3. Face cache 跨核无锁：LVGL/UI 侧在 CPU1 使用缓存指针和 metadata，`face_cache` worker 在 CPU0 渲染并替换 slot；没有 mutex/atomic/version publish 协议。

## 7. A1-02 新增问题登记

| ID | 等级 | 位置/范围 | 触发链 | 结论 | 静态复核边界/后续动作 |
|---|---|---|---|---|---|
| RTOS-005 | P1 | `main/wake_word_detector.c:265~293`、`:831~843`；调用点 `main/nuwax_ai_partner_main.c:2329~2334` | 主状态机进入 stop sleep wake -> `nuwax_wake_word_detector_stop()` -> 20ms 轮询 `is_running()`；reap 路径还会 `vTaskSuspend()` 后无截止等待 `eTaskGetState(task) == eRunning` 退出 | Wake detector stop/reap 不是白名单无限等待；worker 卡住时会永久阻塞主控，且当前 WDT 不订阅业务 worker | 只读静态成立；需补 stop deadline、失败转 fault/restart 策略，并用卡死注入验证 |
| RTOS-006 | P1 | `main/nuwax_ui_v28/face/face_cache.c:459~643`、`main/nuwax_ui_v28/face/face_engine.c:922~984`、`:1175~1200`、`:1370~1392`；LVGL task `main/nuwax_lvgl_port.c:18~20`、`:543~545` | `face_cache_init()` 创建 CPU0 worker；worker `render_into_cache()` 选择 LRU slot 并在 `face_cache_render()` 写 259200-byte buffer，之后更新 `s_slot_emotion/lru`；LVGL CPU1 同时 `face_cache_get()` 读取 slot 指针并把 buffer 交给 `lv_image_set_src()`/动画使用 | Face cache metadata、LRU、protected emotion、suspended/defer flag 和 slot buffer 跨核共享无锁。尤其 `face_cache_get()` 在 worker 渲染完成前可能命中正在被改写的旧 emotion slot，导致 UI 读到半写 buffer 或 metadata tear | 静态复核已确认 `FACE_CACHE_ASYNC=1` 且 worker pinned CPU0、LVGL pinned CPU1；未运行设备，未证明实际撕裂概率。需加 mutex/versioned publish/双阶段 slot state，或只在 LVGL task 内提交缓存 |
| RTOS-007 | P1 | `main/nuwax_provision_new/nuwax_provision_main.c:806~858`、`:860~912`、`:1350~1407`、`:1492~1524` | BLE endpoint handler 通过 `s_vox_config_jobs/gate` 分配 job；若正常入队则 `xSemaphoreTake(job->done, portMAX_DELAY)` 等 main provisioning loop 处理。`nuwax_provision_main_stop()` 先 `close_and_drain_vox_config_jobs()`，随后 `network_prov_mgr_stop_provisioning/deinit()`，再删除 queue/gate；本模块没有 in-flight handler 计数或 quiesce barrier | `vox-config` job ACK 无限等待与 stop/delete 生命周期耦合不足。若 provisioning manager 不保证 stop/deinit 返回前所有 custom endpoint 回调已退出，handler 可能在 queue/gate 被删除后继续使用，或卡在 job done 等待 | 依赖 ESP-IDF provisioning manager 回调语义，当前未读取 IDF 源实现，故边界是“本模块缺少自证”。需补 handler refcount、closing flag、有限 job wait、stop 等 in-flight=0 后再 delete |

## 8. A1-02 计划校验

- A1-02 状态：**已完成（A级静态）**；已覆盖锁图、无限等待白名单、非白名单等待和新增高风险项。
- 未授权事项未执行：未修改业务代码、未构建、未运行测试、未连 COM5、未 OTA。
- 下一项建议：A1-03/A1-04 继续核对 speaker/output/p3 复合锁长尾、业务 worker WDT/heartbeat 矩阵，以及 provisioning manager 回调 quiesce 的 IDF 源码证据。
