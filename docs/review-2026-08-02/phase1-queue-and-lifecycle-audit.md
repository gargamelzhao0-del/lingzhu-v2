# 阶段 1：队列、通知与对象生命周期只读审计

审计日期：2026-08-02  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
权限：A 级只读；未修改业务源码、未构建、未运行 host test、未操作 COM5、未写 NVS、未 OTA。

## 1. 审计目标与方法

本文件对应总工作记录的 A1-03、A1-04、A1-05 的第一批静态证据。审计以产品候选 CMake 路径为主，host check/stub 与未链接原型单独标记，不把它们计入正式固件能力。

对每个内核对象检查以下不变量：

1. 创建失败是否回滚已创建的对象；
2. 生产者、消费者和 payload 所有权是否唯一且可追溯；
3. 满队列、空队列、超时和取消是否有明确语义；
4. 停止时是否先停止所有访问者、再等待确认、最后删除任务/队列/锁；
5. 静态 TCB/栈、回调和延迟到达的通知是否不会访问被复用或删除的对象。

源码或配置变化后，本文件结论自动失效。

## 2. 对象枚举口径

- 使用文件系统范围的 `rg` 枚举 `xQueue*`、`vQueueDelete*`、`xEventGroup*`、`xSemaphore*`、task notification、`xTaskCreate*`、`vTaskDelete*`、`esp_timer_*` 和 `esp_event_handler_*`。
- 首轮枚举命中 432 个队列/同步 API 使用点；创建/删除/任务创建等生命周期 API 命中 145 个。命中包含 host check/stub，后续结论仅引用进入产品构建的源文件。
- 队列、通知与 event group 的精确运行时高水位、时序和销毁竞态不能由静态代码证明，列为 B/D 级验证项。

## 3. 已完成的核心队列初查

| 对象 | 创建与容量 | 生产者 -> 消费者 | 满/空语义 | 停止与销毁初判 |
|---|---|---|---|---|
| `audio_uplink.free_slots` / `ready_packets` | `conversation/audio_uplink.c:532-544`；按实时/可靠 profile 创建，优先 PSRAM、失败回退 internal | 采集路径 -> `nuwax_audio_tx` | 非阻塞 send；采集方在有限 slot wait 后对实时流返回背压/超时，终止帧有保留 slot | `finish()` 等待 10 s，超时关闭 WS；worker `finished` 后才删除任务、队列和 pool。若无法停止会 `esp_restart()`，不释放 live resources。后续仍需核对所有调用方不会在 destroy 后继续 enqueue。 |
| `barge_dispatch.s_queue` | `conversation/barge_in_dispatcher.c:340-344`；固定深度、PSRAM 优先 | 检测/调度 API -> 单例 `barge_dispatch` | `xQueueSend(..., 0)` 失败直接报错；消费者 `portMAX_DELAY` 等待 | worker 为常驻单例，当前无 deinit；队列不在运行期删除。需在 A1-04 核对 unbind/rebind 与旧 WS epoch 的边界。 |
| meeting `events` / `request_acks` | `audio_modes/meeting/meeting_mode.c:959-963`；事件队列和深度 1 ACK mailbox | capture/protocol -> meeting mode owner | `request_acks` 使用 overwrite；普通事件满时返回失败 | `nuwax_meeting_mode_deinit()` 删除队列。需要在阶段 4 核对无并发 producer 时才 deinit。 |
| monitoring `events` | `audio_modes/monitoring/monitoring_mode.c:1123` | capture/protocol -> monitoring mode owner | 非阻塞 send，满时返回失败 | `nuwax_monitoring_mode_deinit()` 删除队列；同样需要模式切换并发验证。 |
| `s_wake_word_events` | `nuwax_ai_partner_main.c:15655`；深度 2 | wake callback -> app loop | 满时主动弹出最旧事件再写新事件（latest-event） | 常驻 app 生命周期；没有运行期 delete。需要结合 wake worker stop 审计确认 callback 停止先于 app 关闭。 |
| `s_touch_events` | `nuwax_ai_partner_main.c:15636`；深度 4 | touch input -> gesture/app | 满队列语义待逐调用方确认 | 常驻 app 生命周期；后续与触摸 deinit、UI mailbox 共同审计。 |
| `s_speaker_owner_events` | `nuwax_ai_partner_main.c` 运行期创建 | speaker callback -> owner loop | 非阻塞发送，失败丢事件 | 需要检查缺事件是否会使播放/采集状态永久不一致。 |
| `s_commands`（OTA） | `nuwax_ota_new/nuwax_ota_device.c:1195`；深度 1、指针元素 | cloud/request API -> OTA controller | 满/重复命令需按 state 返回，而非覆盖 | 创建失败路径有 delete；没有完整 deinit/stop 生命周期闭环证据，后续 A1-04/阶段 6 审计。 |
| `s_vox_config_jobs` | `nuwax_provision_new/nuwax_provision_main.c:811`；PSRAM 队列、指针元素 | BLE/event handler -> app main 处理 | 单个 job 用二元 completion semaphore 等待 | 初查发现 in-flight handler 与 `close_and_drain`/queue delete 的生命周期不能由代码保证，见 RTOS-007。 |
| `s_preload_queue` | `nuwax_ui_v28/face/face_cache.c:471,632` | UI/face path -> `face_cache` worker | 队列满行为待逐调用点核对 | worker常驻且无 deinit；与 cache 共享状态数据竞争另登记 RTOS-006。 |

## 4. 初步问题登记

| ID | 等级 | 模块 | 静态证据 | 结论与关闭条件 |
|---|---|---|---|---|
| RTOS-006 | P1 | `nuwax_ui_v28/face/face_cache.c` | worker 在 `:591-604` 用 `xQueueReceive(..., portMAX_DELAY)`，与 UI/LVGL 路径同时读写 `s_slots`、LRU 和保护 emotion 元数据；未见 mutex、atomic 或 message handoff | 存在跨核数据竞争和指针/缓存内容被并发改写风险。修复需建立单 owner 或受保护 immutable handoff，并以 host stress 或实机快速切换验证。 |
| RTOS-007 | P1 | `nuwax_provision_new/nuwax_provision_main.c` | handler 在 `:860-912` 入队后 `xSemaphoreTake(job->done, portMAX_DELAY)`；owner 仅由 app main 在 `:1350-1407` 处理；停止路径 `:1492-1523` 删除 queue，`close_and_drain` 仅轮询排空 | in-flight handler 可在 owner 停止或队列删除后无限等待，且 job/queue 生命周期没有 join 屏障。修复需让 shutdown 拒绝新 job、唤醒/取消所有 in-flight job、等待 handler 退出后再删 queue/gate。 |

## 5. 待完成项

- A1-03：补齐每个产品队列、event group、notification 的所有权、容量、drop、终止和销毁矩阵；重点核对 speaker、OTA、touch、Wi-Fi、flash-op、face cache、模式队列。
- A1-04：核对所有动态/静态 task 的 self-delete、owner reap、强制删除、stack/TCB 复用和初始化失败回滚。
- A1-05：核对 `esp_timer`、`esp_event`、GPIO/I2C 回调及 WebSocket callback 的执行上下文、解绑和 epoch 防护。

## 6. 计划校验

- 本批证据已覆盖计划阶段 1 的队列图、停止协议、回调/延迟访问审计入口。
- 当前状态：**进行中**。上表的“初判”不等同运行时通过；在 A/B/D 级验证前，不能将任何对象的长稳、满队列、停止竞态或资源回收标为已验收。

## 7. A1-03 对象级补充矩阵（2026-08-02）

以下矩阵只列入 `NuwaxAIPartner-v2/main/CMakeLists.txt` 的正式固件源文件；`*_host_check.c`、`host_stubs` 和 `state_cmd_fast.c`（未进入正式 CMake）不计入产品对象。容量按当前 `sdkconfig`/`sdkconfig.defaults`；`CONFIG_NUWAX_VOICE_UPLINK_QUEUE_DEPTH=12`、`CONFIG_NUWAX_RT_CAPTURE_QUEUE_DEPTH=12`、`CONFIG_NUWAX_SPEAKER_QUEUE_DEPTH=128`。

| 对象/位置 | 容量与 payload 所有权 | Producer -> Consumer | 满/空语义 | 生命周期与静态结论 |
|---|---|---|---|---|
| `audio_uplink.free_slots` / `ready_packets` (`conversation/audio_uplink.c:532-564`) | free 深度 12、元素 `uint8_t`；ready 深度 13（实时 profile 含 1 个 terminal slot）、元素为值拷贝 `nuwax_audio_uplink_packet_t`；PCM 所有权在 `ctx->pool[slot]` | 采集任务/应用任务取 free、写 pool 后投 ready；固定 sender task 取 ready、发送后归还 free | 非阻塞投递；实时 profile 0 ms admission，有限等待后返回可恢复背压/丢弃；terminal slot 保证 END | 分配失败按相反顺序删除 queue、释放 pool、释放 singleton TCB claim（`audio_uplink.c:548-608`）；finish 等待 10 s，destroy 在超时后请求关闭并最多再等 1 s，仍不退出则 `esp_restart`（`1149-1230`）。未发现 queue-after-destroy 的正式调用路径；长稳/超时仍需动态验证。 |
| `rt_capture.free_slots` / `ready_slots` (`nuwax_ai_partner_main.c:7109-7233`) | 各深度 12，元素为 `uint16_t` slot；实际帧和 MIC/REF 缓冲由 `ctx` 所有 | `rt_capture_task` 取 free、填帧、投 ready；应用 capture/DSP 取 ready，处理后归 free | free 无槽时最多等待 10 ms；仍无槽时丢最旧 ready 或写 drop frame；ready 满时回收到 free 并计数 | `rt_capture_start` 在队列/帧池/栈/静态 TCB 任一失败时完整释放（`7141-7279`）；stop 先原子置 stop、等待 task ACK 800 ms，再 suspend/delete task、排空 ready（`7320-7356`）。静态上属于完整闭环。 |
| `rt_capture.fir_ref_delay_requests`（动态 FIR 开关） (`nuwax_ai_partner_main.c:7154-7179`) | 深度 1、值拷贝 `nuwax_aec_fir_delay_request_t` | 运行时延迟决策 -> rt capture task | `xQueueOverwrite`，只保留最新延迟计划；创建失败显式降级禁用 | 队列和 scratch 任一失败均删除/释放并禁用该可选能力；随 rt capture 释放。 |
| `barge_dispatch.s_queue` (`conversation/barge_in_dispatcher.c:332-365`) | 深度 8，值拷贝 `barge_dispatch_item_t`（字符串内嵌，无外部 payload） | barge 检测/确认 API -> 常驻 `barge_dispatch` task | `xQueueSend(...,0)`；满队列立即失败并写 FAILED 状态 | 单例常驻，无运行期 delete；worker 用 binding generation/WS epoch 丢弃陈旧项（`135-329`）。初始化若 lock/queue/task 创建失败未回收已创建前置对象（`337-365`），为 P2 重试泄漏/部分初始化问题。 |
| `speaker_playback.s_speaker_ctx.packets` (`conversation/speaker_playback.c:1546-1565`) | 深度 128，值拷贝 `speaker_playback_packet_t`，每项内嵌最多 `CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME` PCM，不借用 RX payload | WS binary callback -> speaker playback task | RX 非阻塞投递；满队列计数并安排 protocol fault/关闭；generation、epoch、START/END 验证；取消/START gate/END 会在 speaker mutex 下 reset queue | init 在 mutex 失败时删除 PSRAM queue；无 deinit，产品常驻。reset 与 worker receive 并发由 FreeRTOS 临界区和 generation 检查共同保护，但真实停播/满队列需 D 级验证。 |
| `s_speaker_owner_events` (`nuwax_ai_partner_main.c:15581-15589`) | 深度 16，值拷贝 `nuwax_speaker_owner_event_t` | speaker task 回调 -> app 主循环 `drain_speaker_owner_events` (`3181-3186`) | 非阻塞；满队列记录 overflow epoch，主循环通过 playback stats/fault generation 做补偿 | 常驻 app 生命周期；无 delete。事件丢失不会直接释放 payload，但 fallback 是否覆盖所有状态转换需动态验证。 |
| `meeting.events` / `meeting.request_acks` (`audio_modes/meeting/meeting_mode.c:959-987`) | events 深度 8，值拷贝 `nuwax_meeting_event_t`；ACK mailbox 深度 1，同样值拷贝 | WS RX callback -> app 任务 `drain_events` (`423-436`)；ACK 由同一 RX 路径进入 mailbox | events 满即失败并回 command ACK；ACK 使用无条件 `xQueueOverwrite` | init 第二队列失败会回收第一队列；deinit 直接删除。**确认 P1：ACK mailbox 没有按 request_id 保留/拒绝陈旧项；有效 ACK 到达后若另一个 stale ACK 在 app drain 前到达，会覆盖有效 ACK，随后 stale 被丢弃而原请求超时。** deinit 只在 app 启动回滚路径调用，正式运行期间无切换 delete。 |
| `monitoring.events` (`audio_modes/monitoring/monitoring_mode.c:1123-1141`) | 深度 8，值拷贝 `nuwax_monitoring_event_t` | WS RX callback -> app 任务 `drain_events` (`365-372`) | 非阻塞；满队列立即发 `control_queue_full` ACK | init/deinit 成对；正常运行无运行期 delete。mode 状态字段同时由 RX callback 读取、app task 写入，队列不能完全消除该共享状态数据竞争，需 A1-04/运行时核查。 |
| `s_session_commands` (`nuwax_ai_partner_main.c:15644-15647`) | 深度 32，值拷贝 `nuwax_session_command_envelope_t`（含 WS epoch 和内嵌 command） | WS text callback -> app 主循环 `drain_session_commands[_limited]` | ASR partial 在剩余 <=4 槽时直接丢；其他命令最多等待 40 ms，满则丢入站并保留已排队事件；音频模式边界 reset 并按计数记录 | 常驻；边界 reset (`3911-3914`) 没有停止 RX callback 的 join，但 epoch 检查降低陈旧命令影响；需 A1-04/A1-05 验证 reset 与 callback 并发。 |
| `s_p3_softref_ab_requests` / `s_p5_during_playback_candidate_requests` (`nuwax_ai_partner_main.c:15648-15654`) | 各深度 2，值拷贝 request struct | WS callback -> 各自常驻 worker (`13978-14031`) | 先尝试非阻塞发送；满时弹出最旧再写入新请求（latest request），失败才计错 | 常驻，无 delete；WS epoch 在 worker 侧过滤陈旧请求。 |
| `s_wake_word_events` (`nuwax_ai_partner_main.c:15655`) | 深度 2，值拷贝 `wake_word_event_t` | wake callback -> `wait_for_mic_start_action` (`5648-5652`) | 满时弹出最旧再写入新事件（latest wake） | 常驻；需确保 wake detector stop 先于 app 关闭，当前 app 没有运行期关闭队列。 |
| `s_shell_actions` (`nuwax_ai_partner_main.c:15642-15643`) | 深度 8，值拷贝 `nuwax_app_shell_result_t` | LVGL/app_shell 和 touch gesture -> app 主循环 `drain_shell_actions`/等待 action (`5423`, `5686`, `14559-14565`) | 非阻塞，满时丢 action 并告警；没有动态 payload | 常驻；touch gesture 仅在 queue 创建后启动。无 delete。 |
| `s_wifi_connect_requests` (`nuwax_ai_partner_main.c:15656-15657`) | 深度 2，值拷贝 SSID/password/request id | shell/MCP dispatch -> `wifi_connect_task` (`5319-5347`) | 非阻塞，满时 UI 返回 queue full；consumer `portMAX_DELAY` | 常驻；无 delete。`s_wifi_connect_mutex` 同时保护连接操作，但全局初始化失败没有回滚。 |
| `s_wifi_scan_results` / `s_wifi_status_reports` (`nuwax_ai_partner_main.c:15658-15661`) | 各深度 1，值拷贝完整 report | scan/connect worker -> app 主循环 drain (`4575-4624`) | `xQueueOverwrite` 保留 latest report；发送 WS 失败时保留队首直到下次成功 | 常驻；无 delete。深度 1 与 overwrite 语义匹配。 |
| `touch_input.s_touch_queue` (`touch_input.c:566-573`) | 深度 8，值拷贝 `nuwax_touch_point_t` | touch log task (`518-539`) -> `nuwax_touch_poll_event`/gesture task (`touch_gesture.c:231-243`) | 触摸/释放用 `touch_queue_send_latest`，满时弹最旧再写最新；释放允许 20 ms | 无 deinit；I2C init 失败保留 queue 以便冷却后重试。task notification 由 GPIO ISR 驱动。 |
| `s_touch_trigger` / `s_touch_events` (`nuwax_ai_partner_main.c:15630-15640`) | binary semaphore；queue 深度 4、值拷贝 touch point | **无 producer/consumer 调用**（仅声明和创建） | 无运行语义 | **确认 P2：孤立对象。** 在 app 启动失败时也未释放；应删除旧路径或接入真实 touch owner，避免误以为它是有效触摸通道。 |
| `nuwax_ota_new.s_commands` (`nuwax_ota_new/nuwax_ota_device.c:1195-1248`) | 深度 1，元素为 `ota_command_item_t *`；item 为堆分配、queue 转移所有权 | WS RX callback 分配/入队 -> OTA controller task (`1130-1144`)；worker 完成后 free item | 满/保留竞争时立即 free item、回滚 interlock 并发送 reject ACK (`1497-1503`) | init 各错误路径删除 queue；controller task 常驻，无 deinit。当前队列 payload 所有权清晰；`s_initialized`/controller 创建失败后的 OTA main 注册状态仍需全生命周期核查。 |
| `nuwax_provision_new.s_vox_config_jobs` (`nuwax_provision_new/nuwax_provision_main.c:811-825`) | 深度 1，元素为 `vox_config_job_t *`；job/input/done semaphore 由 handler 创建、owner 处理后 signal，handler free | BLE provisioning endpoint -> app main `process_vox_config_job` (`831-840`) | gate 控制 accepting；满/停止拒绝并 free；owner 只处理一个 job | **确认 P1：handler 未登记引用/退出屏障。** handler 在读取全局 queue/gate、或持有 gate/等待 `job->done` 时，`close_and_drain`/`stop` 可能删 queue/gate (`1492-1525`)；1 s gate 获取失败也被静默忽略 (`842-858`)。需要 shutdown 拒绝新 handler、等待已有 handler 引用归零后再删 queue/gate。 |
| `face_cache.s_preload_queue` (`nuwax_ui_v28/face/face_cache.c:470-489,631-643`) | 深度 12，值拷贝 emotion enum；无外部 payload | LVGL/face engine -> pinned face preload worker | 非阻塞 send，满时静默丢 preload；worker 空队列 `portMAX_DELAY` | **确认 P1：worker 与 LVGL 同时读写 `s_slots`/emotion/LRU/保护标记和 slot buffer，无 mutex/immutable handoff（`496-584`, `591-614`），可能返回正在被覆盖的显示 buffer。另有 P2：task 创建失败后 queue 保留但 `face_cache_preload` 不回退同步渲染。无 deinit。 |

## 8. Event group、task notification 与无 ring/stream 对象清单

| 对象 | 生产/消费与语义 | 生命周期审计 |
|---|---|---|
| `wifi_manager.s_wifi_event_group` (`wifi_manager.c:304`) | Wi-Fi/IP event callback 设置/清除 `CONNECTED/FAIL/FALLBACK/SCAN_DONE`；connect/scan 任务等待并按超时读取 (`244-295`, `379-394`, `459-474`, `716-730`) | **确认 P1：`wifi_init_once` 在 `esp_netif_init`、default event loop、STA netif、`esp_wifi_init`、任一 handler register、mode/start 任一步失败时直接 return (`310-369`)；既不删除 event group、注销已注册 handler，也不释放 netif/driver。重试会覆盖全局指针并可重复注册 handler。** 正常产品没有 deinit，故需把失败回滚视为启动健壮性缺陷。 |
| `touch` task notification (`touch_input.c:76-85,495`) | GPIO ISR `vTaskNotifyGiveFromISR`，touch log task `ulTaskNotifyTake(pdTRUE, wait)`；通知可合并，另有 fallback poll | 无 stop/deinit；ISR 注册成功后常驻。task 创建失败会清空 handle，ISR 不安装。 |
| Wi-Fi scan notification (`nuwax_ai_partner_main.c:4371-4418`) | app/MCP `xTaskNotifyGive(s_wifi_scan_task)`，scan task `ulTaskNotifyTake(..., portMAX_DELAY)`；重复请求合并 | task 常驻、无 delete；task 创建失败后的 app_main 直接 return，之前的 queue/worker 仍存活。 |
| Flash/reprovision notification (`nuwax_ai_partner_main.c:5197-5287`) | UI/USB -> `xTaskNotify(..., NUWAX_FLASH_OP_REPROVISION, eSetBits)`；flash task `xTaskNotifyWait(..., portMAX_DELAY)`，bit 可合并 | task 常驻；无 delete。通知失败会清 reservation。 |
| OTA controller notifications (`nuwax_ota_device.c:439-458,487-529,1130-1173,1550-1587`) | controller/WS callback 使用 `CLOUD_READY`、`RESULT_ACK` bit；状态同时在 `s_lock` 下确认，避免单纯通知丢失 | controller 常驻；`xTaskNotifyWait` 取通知后通过 pending flags 补偿。未发现通知访问已删除 task 的正式运行期路径；初始化失败后仍需验证 OTA core unregister。 |
| Battery service notification (`board/battery_service.c:279-291,416-438`) | stop 置 flag 后通知 battery task 提前醒来，task 周期采样 | stop 等 `stopped` semaphore，超时不 delete；普通单caller主路径可重试。后续A1-07确认listener self-stop会自等待、多个stop caller共享binary completion且结果不可线性化，canonical问题为 `API-003`。 |
| LED notification (`board/led_driver.c:76-127,172-178,288-315`) | 状态 setter 通知 LED worker，worker 周期/通知刷新 PWM；deinit 通知后等待 `stopped` | 超时保留 task；成功才 delete task/semaphores。setter 在锁外使用 task handle，和 deinit 并发时可能向已删除 handle 通知，列 A1-04。 |
| Vocat diagnostic TX notifications (`board/vocat_audio_board.c:134-175,1398-1403,1649-1654,1832-1835`) | 临时 TX task 完成时通知调用方，调用方等待最多 1500 ms | **P1（诊断路径）：超时后调用方继续返回，而临时 task 仍可能使用栈上 `tx_ctx`；task 随后通知/自删除时可能访问失效栈。** 诊断模式不属于正常云端路径，但正式 CMake 已链接，需按测试固件风险处理。active-tone 使用 heap ctx + done semaphore，超时则泄漏而非 UAF (`777-840`)。 |
| ringbuffer / streambuffer / messagebuffer | 正式 CMake 源文件中无 `xRingbuffer*`、`xStreamBuffer*`、`xMessageBuffer*` 创建/删除或发送/接收调用 | 不适用；此前文件系统命中仅来自 host stub/未链接原型，已排除。 |

## 9. 启动失败回滚专项结论

`app_main` 在 `nuwax_ai_partner_main.c:15504-16072` 以线性方式创建 OTA、speaker、live-AEC pool、触摸孤立对象、全局 queues、meeting/monitoring queues、flash task、UI/touch task、Wi-Fi/provisioning/fallback task。多处失败分支只 `set_state(ERROR); return;`，没有统一 `goto fail`：

- OTA pointer mutex/OTA controller、speaker queue/mutex、live-AEC pool 在后续 `s_touch_trigger`、全局 queue、mode 或 task 创建失败时不会释放；
- 全局 10 组 queue、`s_wifi_connect_mutex` 在任一 queue 创建失败时不会回收已成功创建的前置对象；
- shell/touch/Wi-Fi/provisioning 后续 task/init 失败时，前面已创建的 task、queue、event group、mode controller 仍运行，`app_main` 结束后没有业务 owner；
- `wifi_init_once` 的 event group/handler/netif/driver partial-init 回滚缺失，见 RTOS-009；
- `face_cache_init`、`barge_dispatcher_init`、LED/battery 等模块各自的 partial-init 不能由 app_main 统一回滚。

**RTOS-008（P1，确认）**：正式启动发生可恢复的分配/驱动失败时，系统进入“ERROR + 残留后台任务/对象”而不是原子失败；若未来增加重试、热重启子系统或 OTA health retry，会把残留对象转化为重复回调、重复 handler、内存耗尽或悬空 owner。关闭条件是统一 fail-cleanup（按反向依赖顺序停止 producer、等待 worker、删 queue/event/lock）或明确调用 `esp_restart`，并为每条失败路径提供 host fault-injection 证据。

## 10. A1-03 结论与后续验证边界

- A1-03 的**静态对象枚举、容量、payload ownership、满/空语义、停止/销毁顺序**已完成；正式源文件中未发现 ring/stream/message buffer。
- 已确认缺陷：meeting ACK overwrite（P1）、face cache 跨任务共享无保护（P1）、BLE provisioning handler/queue 缺少 shutdown join（P1）、Wi-Fi event group/handler partial-init 无回滚（P1）、app_main 全局启动失败无统一回滚（P1）、孤立 touch semaphore/queue（P2）、barge/face worker partial-init 重试残留（P2）、诊断 TX notification 超时后的栈生命周期风险（P1，诊断配置限定）。
- 已确认静态安全模式：audio uplink、rt capture、OTA command item 在正常成功/分配失败路径中拥有明确的值/slot/heap ownership 和反向清理；这不等价于运行时长稳通过。
- 动态验证仍需 A/B/D 阶段：队列高水位与背压、WS epoch 与 queue reset 并发、speaker reset/stop race、BLE handler 真正退出时间、face cache 显示 buffer 覆盖、Wi-Fi handler 重复注册、诊断 TX 超时故障注入。

**A1-03 状态：静态审计完成（待运行时验证关闭）。本句原始记录时 A1-04/A1-05 尚未完成；截至当前总工作记录，A1-04~A1-07 均已完成A级静态审计，后续证据对本文件的早期正向判断具有优先级。**
