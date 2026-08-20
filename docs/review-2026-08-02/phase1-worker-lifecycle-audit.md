# 阶段 1：Worker 启停、static stack/TCB 与迟到访问只读审计

审计日期：2026-08-02  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
权限：A 级只读；未修改业务源码、未构建、未运行 host test、未操作 COM5、未写 NVS、未 OTA。

## 1. 审计目标与口径

本文件对应总工作记录 A1-04。以产品候选 CMake 路径为主，检查每个 worker 的创建、停止请求、退出确认、owner reap、static stack/TCB 释放、self-delete、强制删除、初始化失败回滚和迟到回调/通知。host check/stub 与未链接原型不计入正式 worker，但会单独标记边界。

“静态安全”只表示代码建立了可追溯的状态/所有权顺序；不等于任务在真实 I2S、TLS、flash cache disabled 或调度压力下按时退出。任何无 deadline 的停止路径、调用者栈上下文、已删除句柄通知和失败回滚缺口均登记为风险。

## 2. Worker 生命周期矩阵

| Worker | 创建/栈 | 正常退出与 owner | 静态判断 |
|---|---|---|---|
| `nuwax_wake` | `wake_word_detector.c:788`；静态 TCB internal、stack PSRAM | worker 释放 MIC/临时 buffer 后发布 `EXITING` 并 suspend；owner `detector_reap_exited_task()` suspend/确认/`vTaskDelete`，最后发布 `STOPPED` | 状态顺序清晰，但 `nuwax_wake_word_detector_stop()` 在 `wake_word_detector.c:831-843` 无总 deadline；reap 中状态等待也无总 deadline，见 RTOS-005 |
| `nuwax_ws_rx` | `ws_transport.c:1520`；WithCaps PSRAM stack | RX 发布 `RX_EXITED`、`rx_task_running=false` 后 suspend；owner `cleanup_rx_task()` 有 `NUWAX_WS_RX_TASK_STOP_TIMEOUT_MS`，确认后 `vTaskDeleteWithCaps` | 退出顺序和 RX buffer 访问隔离较完整；但 task 创建后才发布 handle/`OPEN`，可覆盖 worker 的立即退出状态，见 LIFE-009 |
| `nuwax_audio_tx` | `conversation/audio_uplink.c:590`；静态 TCB、PSRAM stack | sender 完成后 `finished=true` 并 suspend；owner finish 等待，destroy 超时先关 WS/再等，仍不退出 `esp_restart()`，成功后删 task/queue/pool | 正常/失败 ownership 反向清理明确；强制 suspend/delete 仅在 finished 已确认后使用。需验证 finish 与 capture producer 并发及重启策略 |
| `nuwax_mic_rx` | `nuwax_ai_partner_main.c:7263`；静态 TCB、PSRAM stack | stop flag -> 等待 `stopped`，超时走 `esp_restart()`；成功后 owner suspend/delete 并释放队列/帧池 | 任务不会在未确认停止时释放 live resources；重启路径是有界终止，但需核对 restart 前的 WS/音频状态 |
| `battery_service` | `board/battery_service.c:386`；WithCaps PSRAM stack | stop flag + task notify；worker 停止驱动、给 `stopped`、suspend；owner 超时保留 task，成功后 delete | 单次 stop 语义清楚；stop 与并发 start/stop 的 task handle 快照仍需压力验证 |
| `nuwax_led` | `board/led_driver.c:172`；WithCaps PSRAM stack | stop flag + notify；worker 给 `stopped`、suspend；owner 成功后 delete | `nuwax_led_set_state/toggle/start_breath` 在释放锁后使用快照 task handle；与 deinit 并发可能向已删除 task notify，见 LIFE-006 |
| `board_inputs` | `board/board_gpio_inputs.c:218`；WithCaps PSRAM stack | stop flag；worker 给 `stopped`、suspend；owner 250 ms 等待后 delete | 超时不删除且保留 initialized 状态，允许重试；需验证 GPIO callback 不在 stopped 信号后继续访问用户上下文 |
| `nuwax_touch` | `touch_input.c:719`；WithCaps/internal stack | 没有公开 stop/deinit；常驻，GPIO ISR 触发通知，fallback poll | 一次性产品路径成立；运行期重配网/重复 init 没有 task quiesce 契约，列 RTOS-004/后续 API 审计 |
| app 常驻 worker（`flash_ops/shell/touch_gesture/wifi_scan/wifi_connect/p3/p5/speaker`） | `nuwax_ai_partner_main.c:15698-16066`；helper 分配 static TCB 与 internal/PSRAM stack | 无限循环/队列等待；多数不保存 task+allocation owner，也无 stop | 只适用于“一次启动直到物理断电/重启”；任一后续启动步骤失败时无法统一回收，纳入 RTOS-008 |
| `nuwax_power_key` | `board/nuwax_power_ctrl.c:387`；普通动态 stack | 无 stop；持续轮询，正常关机依赖电源保持被切断 | `nuwax_power_ctrl_init()` 不检查已有 handle；重复调用会覆盖句柄并留下两个任务，归并 RTOS-004/阶段 2 |
| `wifi_fb_watch` | `wifi_manager.c:400`；普通动态 stack | 无句柄、无 stop；每秒等待 event group | 常驻任务；Wi-Fi init 半失败时 event group/handler 状态不保证有效，见 RTOS-009 |
| `nuwax_usb_ctrl` | `nuwax_provision_usb_control.c:97`；WithCaps PSRAM stack | 无句柄、无 stop；USB driver 安装失败时 worker 自删 | driver 失败后全局 `s_request_reprovision` 未清零，后续 start 被错误判为 already-started；见 LIFE-005。无正常 teardown |
| `face_cache` | `face_cache.c:476`；CPU0、WithCaps PSRAM stack | 无 stop/deinit，阻塞 preload queue | worker 创建失败后 queue 保留且无同步渲染回退；与 LVGL 并发 slot 写入统一见 RTOS-006 |
| `barge_dispatch` | `barge_in_dispatcher.c:352`；WithCaps PSRAM stack | 常驻，无 deinit；bind/unbind 只改变 generation | lock/queue/task 部分创建失败不反向释放，见 LIFE-001 |
| `ota_controller` | `nuwax_ota_device.c:1237`；普通动态 stack | 常驻，无完整 deinit/stop | queue、core callback/bind、boot interlock 分段建立；前置检查或 task 创建失败只删除 queue，不能恢复到 init 前状态，见 LIFE-010 |
| `p3_health_tx`/`p3_chmap_tx`/`p2_smoke_tx` | `vocat_audio_board.c:1274/1561/1776`；普通动态任务 | worker 自删并通知 owner；owner最多等待 1500 ms | `tx_ctx` 在调用者栈上；超时后 worker 仍可能访问 `ctx`，见 LIFE-002。诊断路径也必须安全 |
| `p3_active_tone` | `vocat_audio_board.c:806`；heap ctx + done semaphore | worker 给 done 后 self-delete；owner超时不释放 ctx | 避免立即 UAF 但超时泄漏 heap ctx，且重试会累积，见 LIFE-007 |
| `p1_touch_diag` | `touch_input.c:775`；普通动态 stack，未保存 handle | 无限诊断循环；无 stop/deinit，只有整机复位终止 | 仅隔离 P1 诊断 profile 使用，当前产品 profile 不启动；生命周期契约是“运行至复位”，不能在同一 boot 重启或回收，归入 RTOS-004 的无统一 owner 边界 |
| LVGL | `nuwax_lvgl_port.c:543`；静态 TCB、动态 stack/buffers | 常驻，无 stop | `esp_timer_start_periodic`、display/buffer、task stack/TCB 的初始化失败没有统一反向回滚，见 LIFE-004 |

## 3. 正式任务创建点反向校验

以 `main/CMakeLists.txt` 的 `SRCS_COMMON`、产品启用的 OTA 源和 `SRCS_LVGL_UI` 为边界，对全部 `xTaskCreate*`/`xTaskCreate*WithCaps` 创建点反向映射到上表：

| 创建文件/位置 | 创建的任务 | 矩阵覆盖 |
|---|---|---|
| `conversation/audio_uplink.c:590` | `nuwax_audio_tx` | 已覆盖 |
| `conversation/barge_in_dispatcher.c:352` | `barge_dispatch` | 已覆盖 |
| `board/battery_service.c:386`、`board/board_gpio_inputs.c:218`、`board/led_driver.c:172`、`board/nuwax_power_ctrl.c:387` | `battery_service`、`board_inputs`、`nuwax_led`、`nuwax_power_key` | 已覆盖 |
| `board/vocat_audio_board.c:806/1274/1561/1776` | `p3_active_tone`、`p3_chmap_tx`、`p3_health_tx`、`p2_smoke_tx` | 已覆盖；诊断条件路径 |
| `nuwax_lvgl_port.c:543` | LVGL task | 已覆盖 |
| `nuwax_ai_partner_main.c:4307/4334/4361` 及调用点 `:15698/:15908/:15920/:16049/:16052/:16055/:16058/:16066` | `nuwax_flash_ops`、`nuwax_shell`、`nuwax_touch_gesture`、`nuwax_wifi_scan`、`nuwax_wifi_conn`、`p3_softref_ab`、`p5_candidate`、`nuwax_speaker` | 已由 app 常驻 worker 行覆盖 |
| `nuwax_ai_partner_main.c:7263` | `nuwax_mic_rx` | 已覆盖 |
| `nuwax_provision_new/nuwax_provision_usb_control.c:97` | `nuwax_usb_ctrl` | 已覆盖 |
| `nuwax_ui_v28/face/face_cache.c:476` | `face_cache` | 已覆盖 |
| `touch_input.c:719/775` | `nuwax_touch`、`p1_touch_diag` | 已覆盖；后者是隔离诊断条件路径 |
| `wake_word_detector.c:788`、`wifi_manager.c:400`、`nuwax_ota_new/nuwax_ota_device.c:1237`、`ws_transport.c:1520` | `nuwax_wake`、`wifi_fb_watch`、`ota_controller`、`nuwax_ws_rx` | 已覆盖 |

反向检索共得到 23 个直接 `xTaskCreate*` 语法位置；其中 3 个 app helper 被 8 个具名任务调用，最终对应上表 28 个具名 worker。未发现正式 CMake 路径中仍未映射的第一方任务创建点。ESP-IDF/第三方组件内部任务不在本表重复枚举，已由 `phase1-architecture-rtos-audit.md` 的系统任务联合调度清单覆盖；host check/stub、归档与未链接原型也不计入产品 worker 数量。

## 4. 已确认问题

| ID | 等级 | 位置 | 触发链 | 结论/关闭条件 |
|---|---|---|---|---|
| RTOS-004 | P2 | `board/nuwax_power_ctrl.c:378-445`；同类常驻边界见 `touch_input.c:719-783` | 再次调用 `nuwax_power_ctrl_init()` 时不检查 `s_power_key_task_handle`，直接创建新任务并覆盖句柄；旧任务仍永久轮询 SW3 | 重复初始化可形成多个关机 owner。给 init 加并发安全的幂等 guard，或提供 stop/join 后再启动；以重复/并发 init 和单次 SW3 事件只产生一个关机决策验收 |
| RTOS-005 | P1 | `wake_word_detector.c:831-843`、`:265-293` | stop 请求后 worker 未进入 EXITING/STOPPED，owner 20 ms 轮询；reap 状态等待无总 deadline | 主控可能永久等待。增加有限 deadline、fault/restart 策略，并注入 worker 卡死验证 |
| LIFE-001 | P2 | `barge_in_dispatcher.c:337-365` | mutex 成功、queue/task 后续失败时不释放前置对象 | 重复 init/retry 会残留 lock/queue；补齐反向回滚并重复 init/deinit 验证 |
| LIFE-002 | P1 | `vocat_audio_board.c:1274/1561/1776`、`:1400/1650/1834` | owner 超过 1500 ms 未等到通知就返回，worker 仍读写栈上 `tx_ctx` | 诊断 worker 可 UAF；超时必须取消+join，或用 owner 管理的 heap/静态上下文且不能提前返回 |
| LIFE-004 | P1 | `nuwax_lvgl_port.c:505-510,410-560` | timer/display/buffer 已创建后，timer start、task TCB/stack 或 task 创建失败直接 return | 部分初始化资源泄漏，timer 可能在无 LVGL owner 时继续回调；统一 `goto fail` 反向清理并故障注入 |
| LIFE-005 | P2 | `nuwax_provision_usb_control.c:31-45,82-105` | USB driver install 失败时 task 自删，但全局 callback/context 保留 | 后续 start 永久返回 `ESP_ERR_INVALID_STATE`，且 callback 生命周期不可重置；失败路径清空全局并提供 stop/retry |
| LIFE-006 | P2 | `board/led_driver.c:197-220,288-315` | setter 解锁后持有 task 快照；deinit 可在此期间停止并删除 task，setter 随后 notify 旧句柄 | 并发 API 下存在已删除 task handle 使用；将 notify 纳入同一锁或使用 generation/stop barrier，并做并发压力测试 |
| LIFE-007 | P2 | `vocat_audio_board.c:777-840` | active-tone stop 超时后返回，heap ctx/task/board TX lease 仍保持 live；只有调用者保留同一 handle 并重试 stop 才能回收 | 避免立即 UAF，但缺异步 reap/强制终止契约，调用者丢弃 handle 会永久泄漏并占用 TX；明确“必须重试”契约或由模块接管异步回收 |
| LIFE-008 | P2 | `face_cache.c:459-493` | `face_cache_init()` 无 initialized guard；若调用者重复初始化会覆盖旧 slot 指针并泄漏旧 buffer/queue/task | API 需要幂等初始化或显式拒绝重复调用；补重复 init/deinit 测试 |
| LIFE-009 | P1 | `ws_transport.c:1318-1325,1511-1536` | `xTaskCreateWithCaps()` 可立即调度高优先级 RX；若对端在握手后立刻关闭，worker 先发布 `RX_EXITED/running=false` 并挂起，创建者随后写入 `client->rx_task`、把 lifecycle 覆盖为 `OPEN` 并返回成功 | owner 可收到“连接成功”但实际 worker 已退出且状态倒退。增加启动 gate（先保存 handle/发布 OPEN，再放行 RX），或用 CAS 防止覆盖 `RX_EXITED` 并在返回前复核 connected/running；以“握手后立即关闭 + 创建点强制调度切换”压力测试关闭 |
| LIFE-010 | P1 | `nuwax_ota_new/nuwax_ota_device.c:1182-1249`、`nuwax_ota_new/nuwax_ota_main.c:702-760` | register/core init/boot pending/interlock/task create 分段成功后，失败路径只删除 `s_commands`；没有 unregister/deinit，pending boot 时还可能保留 shutdown/audio interlock | 当前 app 用 `ESP_ERROR_CHECK` 重启不能证明模块级回滚；持续失败会形成启动循环，即便单次瞬态失败，模块也不能在同一 boot 安全重试。将 init 改成分阶段状态 + `goto fail` 反向撤销 callback/bind/interlock/queue，提供 controller stop/join/deinit；逐点 allocation/NVS/interlock/task-create 故障注入并验证重复 init |

## 5. 静态安全模式与运行时边界

- wake、WS RX、audio TX、rt capture、battery、LED、board inputs 的正常停止路径都遵循“请求 -> 退出确认 -> owner 删除”的主要顺序；这只覆盖静态控制流，不覆盖真实 I/O 阻塞。
- `vTaskDeleteWithCaps` 的调用者必须确认 worker 已 suspend/finished/stopped；任何只按 handle 删除的路径均需要 B/D 故障注入。当前 `audio_uplink`、`ws_transport` 在主路径有确认；vocat 诊断任务没有。
- 使用 PSRAM stack 的 WebSocket/audio/UI/worker 在 flash/NVS/OTA/cache-disabled 路径中的安全性留给阶段 3/6/8，不由本文件判定通过。
- 无 stop API 的常驻任务必须在 OTA、重配网、重复初始化和 app 启动失败时有明确“永久常驻”或“不可重入”契约；当前多模块仍只有隐含假设。

## 6. 计划校验

- A1-04 的静态 task 创建、停止、self-delete、owner reap、static stack/TCB 和迟到访问清单已完成；按正式 CMake 源反向检索后，全部第一方 `xTaskCreate*` 创建点均映射到生命周期矩阵。
- A1-04 状态：**已完成（A 级静态）**；RTOS-004/005、LIFE-001/002/004/005/006/007/008/009/010 保留为开放问题，B/D 运行时验证未执行。
- 未修改业务源码、未构建、未运行 host test、未操作 COM5、未写 NVS、未 OTA。
