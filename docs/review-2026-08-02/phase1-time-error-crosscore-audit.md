# 阶段 1 A1-06：循环、阻塞 I/O、时间、错误传播与跨核可见性只读审计

审计日期：2026-08-02  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
对应工作项：A1-06；评审计划 V1.8 阶段 1.7~1.13  
权限：A 级只读；未修改业务代码、未构建、未运行 host test、未访问 COM5、未写 NVS、未执行 OTA/烧录

> 2026-08-03 后续状态：`TIME-001` 已完成代码修复、host contract 和 ESP-IDF 6.0 构建证明；竞争时序、heartbeat 和实机证明仍开放。详见 `implementation-verification-2026-08-03.md`。下文保留原始发现快照。

## 1. 范围、方法和证据边界

本项基于正式 `main/CMakeLists.txt` 产品路径，反向检查：

- `while (true)`、`while (1)`、`for (;;)` 及条件等待循环；
- `portMAX_DELAY`、queue/semaphore/task notification、I2S、DNS/TCP/TLS/HTTP/NVS/LVGL 等阻塞边界；
- `CONFIG_FREERTOS_HZ`、`pdMS_TO_TICKS`、tick wrap、毫秒/微秒混用；
- 显式 `(void)` 丢弃返回值、`void` 初始化 API、部分成功回滚和错误伪成功；
- 普通全局、`volatile`、C11 atomic、portMUX/mutex 与跨任务/跨核 owner 边界。

原始可复核证据：

| 证据 | 数量 | 用途 |
|---|---:|---|
| `phase1-infinite-loop-rg-current.txt` | 37 | 全部无条件循环逐项分类 |
| `phase1-literal-tick-conversions.txt` | 91 | 字面量 tick 转换；含 8 个 1 ms speaker wait |
| `phase1-variable-tick-conversions.txt` | 61 | 配置/运行期 timeout、delay 与 deadline 转换 |
| `phase1-discarded-return-rg-current.txt` | 324 | 显式丢弃返回值的语法候选；318 个产品候选、6 个 host 候选 |
| `phase1-wait-time-rg-current.txt` | 原始检索清单 | mutex/queue/notification/I/O/时间 API 交叉核对 |
| `phase1-crosscore-sync-rg-current.txt` | 原始检索清单 | atomic/volatile/critical/mutex 与普通共享状态交叉核对 |

数量是审计分母，不等于 37/152/324 个缺陷。`(void)xTaskNotifyWait()`、清理阶段 best-effort close、算法中忽略非关键返回值均可能合理；相反，未写 `(void)` 的 `void` API 也可能吞掉关键失败，因此结论以调用链和产品后果为准。

## 2. 配置真值和计时基本单位

| 项目 | 当前事实 | 结论 |
|---|---|---|
| FreeRTOS tick | `sdkconfig:3276`：`CONFIG_FREERTOS_HZ=100` | 1 tick = 10 ms；`pdMS_TO_TICKS(1..9)` 为 0 |
| 产品 frame | `sdkconfig:1198`：20 ms | 当前 frame delay 为 2 ticks，不退化 |
| touch poll | `sdkconfig:1449`：10 ms | 当前恰为 1 tick；不得再向下调而不使用 ceil/min-1 helper |
| SW3 poll | `sdkconfig:1163`：20 ms | 当前 2 ticks；Kconfig 允许 5 ms，见 `TIME-002` |
| speaker write timeout | `sdkconfig:1407`：1000 ms | claim wait 为 1250 ms，runtime reset 内部 deadline 为 5000 ms |
| 当前诊断 profile | P1 hardware diag、P2 audio smoke、synthetic source 均未启用 | 对应常驻循环不进入当前产品镜像的活动分支，但仍属受支持配置代码 |
| USB provisioning control | `sdkconfig:1023` 启用 | USB worker 的永久 read loop 属当前产品路径 |

当前 touch reset 已使用 `touch_delay_ticks_ceil()` 和 `esp_rom_delay_us(5000)`（`main/touch_input.c:148-174`）；ES7210 stop/start 间隔使用 `esp_rom_delay_us(5000)`（`main/board/vocat_audio_board.c:539-550`）。这两处是正确处理亚 tick/精确定时的正向控制，不再存在“5 ms 被截成 0 tick”的旧问题。

## 3. 37 个无条件循环逐项分类

分类：`ALG`=有限算法；`FINITE`=运行期有截止/状态退出；`WORKER`=产品寿命常驻、每轮有阻塞/延时；`PROFILE`=特定诊断配置的常驻终点；`HANDOFF`=worker 退出握手；`RECONNECT`=app 主恢复循环。

| # | 位置/函数 | 分类 | 阻塞或退出保证 | 静态结论 |
|---:|---|---|---|---|
| 1 | `conversation/aec_delay_shadow.c:306` delay coarse search | ALG | delay 单调增加，到 max 或剩余小于 step 即 break | 有限；不是 worker 忙循环 |
| 2 | `board/i2c_bus_manager.c:41` `nuwax_i2c_bus_init` | FINITE | READY/初始化 owner 返回；其他 caller 最长等 2000 ms，`vTaskDelay(1)` 是 1 tick | 有限且 wrap-safe；1 是 tick 字面量，不是 1 ms |
| 3 | `nuwax_lvgl_port.c:359` `lvgl_task` | WORKER | 每轮 mutex 最多 50 ms，handler 后按 min-1 tick delay | app-lifetime worker；init 回滚问题沿用 `LIFE-004` |
| 4 | `nuwax_ai_partner_main.c:541` `active_ws_snapshot` | FINITE | 两次 acquire-load epoch 一致即返回 | lock-free snapshot；连续 writer 理论可使 reader重试，实际 writer只在连接代次切换；保留动态 starvation 边界 |
| 5 | `nuwax_ai_partner_main.c:4270` `shell_task` | WORKER | 每轮 80 ms 或 UI interval delay | 合法常驻 UI owner |
| 6 | `nuwax_ai_partner_main.c:4374` `wifi_scan_task` | WORKER | 空闲时 notification `portMAX_DELAY`；有通知才工作 | 合法 app-lifetime mailbox wait；启动回滚沿用 `RTOS-008` |
| 7 | `nuwax_ai_partner_main.c:5199` `flash_ops_task` | WORKER | 空闲时 notification `portMAX_DELAY`；操作完成/restart | 合法单 owner wait；flash/audio 仲裁风险沿用 `CACHE-001` |
| 8 | `nuwax_ai_partner_main.c:5321` `wifi_connect_task` | WORKER | queue 空时 `portMAX_DELAY`；connect mutex 也可无限等 | worker 不占 CPU；connect owner 卡住时本 worker无独立 deadline，归入后续网络故障注入边界 |
| 9 | `nuwax_ai_partner_main.c:5417` synthetic `wait_for_start_action` | FINITE | 当前未编译；每 100 ms queue wait，WS断开返回 | 配置分支有限/可取消 |
| 10 | `nuwax_ai_partner_main.c:5618` `wait_for_mic_start_action` | FINITE | 100 ms action poll、WS/activity/模式/awake idle条件退出 | 产品等待用户输入循环；不是 CPU spin，2 分钟语义留阶段 4/9 验证 |
| 11 | `nuwax_ai_partner_main.c:13980` `p3_softref_ab_fast_task` | WORKER | queue `portMAX_DELAY` | app-lifetime诊断 command owner |
| 12 | `nuwax_ai_partner_main.c:14018` `p5_during_playback_candidate_fast_task` | WORKER | queue `portMAX_DELAY` | app-lifetime worker；与主 conversation owner共享状态见 `CROSS-001` |
| 13 | `nuwax_ai_partner_main.c:15795` P1 diag terminal loop | PROFILE | 当前 profile关闭；每 1000 ms delay | 诊断镜像明确终点，不进入产品活动分支 |
| 14 | `nuwax_ai_partner_main.c:15903` P2 audio smoke terminal loop | PROFILE | 当前 profile关闭；每 1000 ms delay | 诊断镜像明确终点，不进入产品活动分支 |
| 15 | `nuwax_ai_partner_main.c:16124` `app_main` | RECONNECT | 每次网络/WS/会话周期均有 reconnect delay或业务阻塞 | 产品主恢复循环；错误路径是否全部回滚由 `RTOS-008`、阶段 2/5关闭 |
| 16 | `board/led_driver.c:80` `led_task` | WORKER | stop flag break；每轮 notify wait 20/1000 ms | 有停止协议；并发 deinit风险沿用 `LIFE-006` |
| 17 | `nuwax_ota_new/nuwax_ota_transport.c:450` HTTP header loop | FINITE | control/total deadline + header stall deadline；每轮 10 ms delay | 有限；正向控制 |
| 18 | `nuwax_ota_new/nuwax_ota_transport.c:517` OTA download loop | FINITE | cancel/total transport control、stall deadline、complete/size边界 | 有限；read/write单次阻塞仍需 B/D 故障注入测最坏时间 |
| 19 | `nuwax_ota_new/nuwax_ota_device.c:442` `wait_for_result_ack` | FINITE | monotonic deadline，notification等剩余时间 | 有限且 wrap-safe；正向控制 |
| 20 | `nuwax_ota_new/nuwax_ota_device.c:1136` `ota_controller_task` | WORKER | queue wait 100 ms，周期处理 pending boot | 合法常驻 owner；partial init沿用 `LIFE-010` |
| 21 | `conversation/barge_in_dispatcher.c:138` `barge_dispatch_task` | WORKER | queue `portMAX_DELAY` | 合法常驻 worker；初始化竞争/回滚沿用 `LIFE-001` |
| 22 | `nuwax_provision_new/nuwax_provision_usb_control.c:47` USB control task | WORKER | USB read 最多 20 ms | 当前启用；没有 stop/retry沿用 `LIFE-005/RTOS-004` |
| 23 | `board/nuwax_power_ctrl.c:347` `power_key_task` | WORKER | 每轮按 poll delay；无正常 stop | app-lifetime SW3 owner；重复 init沿用 `RTOS-004`，错误传播见 `ERR-001` |
| 24 | `board/nuwax_power_ctrl.c:452` shutdown gate CAS | FINITE | 成功/已是目标/CLAIMED即返回 | lock-free compare-exchange；理论饥饿但不持资源、不阻塞 |
| 25 | `wifi_manager.c:378` fallback watcher | WORKER | event-group最多 1000 ms等待 | 合法常驻 worker；创建失败被上层忽略见 `ERR-002` |
| 26 | `wake_word_detector.c:571` worker terminal suspend | HANDOFF | worker发布 EXITING后永久 suspend，等待 owner reap/delete | worker本身不忙等；owner stop/reap无总 deadline沿用 `RTOS-005` |
| 27 | `nuwax_provision_new/nuwax_provision_main.c:1355` provisioning run | FINITE | got IP、resource exhausted或 effective deadline break；每轮 job wait 100 ms | 有限；callback/job销毁风险沿用 `RTOS-007/EVENT-003` |
| 28 | `touch_input.c:451` P1 touch diag task | PROFILE | 当前 profile关闭；每轮 poll delay | 诊断常驻 worker |
| 29 | `touch_input.c:491` touch log task | WORKER | notification/poll timeout 10 ms或 idle fallback | 合法常驻 worker；无 deinit属于 `RTOS-004` 生命周期边界 |
| 30 | `nuwax_ui_v28/face/face_cache.c:594` preload task | WORKER | queue `portMAX_DELAY`；suspend/defer时 20~120 ms delay | 合法 worker wait；跨核 cache race沿用 `RTOS-006` |
| 31 | `conversation/speaker_playback.c:520` packet I/O claim | FINITE | 1250 ms real-time deadline | 有限，但 1 ms delay退化为 0 tick，见 `TIME-001` |
| 32 | `conversation/speaker_playback.c:1705` joined stop wait | FINITE | completion/failure/owner消失或 1250 ms deadline | 有限，但 1 ms delay退化，见 `TIME-001` |
| 33 | `conversation/speaker_playback.c:2021` cancel action-claim wait | FINITE | all claims clear或 1250 ms deadline | 有限，但 1 ms delay退化，见 `TIME-001` |
| 34 | `conversation/speaker_playback.c:2754` runtime reset cleanup | FINITE | 5000 ms deadline、最多2次 stop retry | 内层有限；进入此循环前的 reset claim无 deadline，见 `WAIT-001` |
| 35 | `conversation/speaker_playback.c:3325` START authorization | FINITE | authorized、generation失效或 start-gate deadline | 依赖 gate state不变量；1 ms delay退化见 `TIME-001` |
| 36 | `conversation/speaker_playback.c:3400` speaker task | WORKER | queue receive wait按帧/探针 deadline，最大100 ms | 合法播放 owner；init API缺陷见 `API-006` |
| 37 | `conversation/touch_gesture.c:236` touch gesture task | WORKER | poll event 100 ms；失败显式 20 ms delay | 合法常驻 worker，不会因触摸芯片缺失忙循环 |

结论：37 个无条件循环中未发现新的“无条件且完全无阻塞/无状态出口”的永久 CPU spin。真正的无界等待出现在条件循环 `nuwax_speaker_playback_reset_runtime()` 的 reset ownership claim，而不是上述 raw `while(true)` 分母；wake terminal suspend 本身是 owner-reap handshake，但 owner端无 deadline已由 `RTOS-005` 登记。

## 4. Tick 转换和时间比较审计

### TIME-001（P1）：speaker 八个 1 ms delay 在 100 Hz 下变为零 tick

证据：`CONFIG_FREERTOS_HZ=100` 时 `pdMS_TO_TICKS(1)==0`。`main/conversation/speaker_playback.c:564,964,1772,2050,2115,2746,2856,3393` 均调用 `vTaskDelay(pdMS_TO_TICKS(1))`；这些路径覆盖 START admission、I/O claim、joined stop、cancel claim、runtime reset ownership/cleanup和 start authorization。多数循环有 1250~5000 ms wall-clock deadline，但 `vTaskDelay(0)` 只 yield，不保证 lower-priority task获得 CPU；高优先级 speaker/调用者可在 deadline窗口内持续轮询，放大 UI、电源、OTA、控制 worker延迟并使等待对象更难获得调度。

`speaker_playback.c:2629` 的 fade step也用 `pdMS_TO_TICKS(step_ms == 0 ? 1 : step_ms)`。当前内部调用的 200/220/default 320 ms fade会产生 >=10 ms step，barge的 0 ms直接走立即 cancel；但公开 API 接受 1~19 ms时仍可能退化为零 tick，属于同一计时契约缺口。

关闭条件：统一使用 ceil/min-one helper（正延时至少 1 tick），或使用 task notification/semaphore 等待状态变化并设置绝对 deadline；不能用高优先级轮询代替 handoff。B/D 级记录 contention 下 CPU core占用、lower-priority heartbeat、stop/reset p95/p99 和 physical mute时延。

### TIME-002（P2，配置依赖）：SW3 Kconfig 允许小于一 tick的 poll值

`main/Kconfig.projbuild:1454-1458` 允许 `CONFIG_NUWAX_POWER_KEY_POLL_MS=5..100`；`board/nuwax_power_ctrl.c:374` 直接 `pdMS_TO_TICKS(poll_ms)`。当前值20 ms安全，但受支持 profile可选择5~9 ms并退化为零 tick，使 power-key task忙轮询，同时 `stable_ms += poll_ms` 把未实际阻塞的时间当作已经过去，导致去抖时序错误。

关闭条件：Kconfig最小值提高到当前 tick周期，或使用 ceil/min-one helper并按真实 elapsed time累计 debounce；增加5/10/20 ms profile静态断言与逻辑分析仪验收。

### 其余转换与 wrap 结论

- 91 个字面量候选除 speaker八处1 ms外，产品路径最小常用值为10 ms；15 ms会向下量化到10 ms但不会变0。是否满足芯片规格留阶段3测量。
- 61 个变量候选中，LVGL `delay_ticks_min1()`、speaker output lock、WebSocket I/O lock/deadline helper已经显式把正延时提升到至少1 tick，是正向控制。
- touch 50 ms reset用 ceil helper，5 ms低脉冲和ES7210 5 ms间隔用 `esp_rom_delay_us()`，是精确亚 tick控制。
- OTA/result ACK、I2C init、face defer、speaker deadline等使用无符号差值或 `(int32_t)(now-deadline)` half-range比较；在各自远小于2^31 ms/ticks的期限内 wrap-safe。未发现把普通 `now >= deadline` 用于可能跨32位 wrap的新增缺陷。
- public timeout API若允许0通常表达 non-blocking poll；1~9 ms的最小等待语义仍应统一文档化，后续 memory/API审计不得把“0”和“被截为0”混为一谈。

## 5. 无界等待和阻塞 I/O结论

### WAIT-001（P1）：speaker runtime reset claim无截止，失败结果又被 `void` API吞掉

`nuwax_speaker_playback_reset_runtime()` 在 `main/conversation/speaker_playback.c:2736-2748` 先执行 `while (!reset_claimed)`；若 `runtime_reset_in_progress` 因并发 caller卡住、异常路径未清除或 owner失活，第二 caller没有 deadline、取消或错误返回，会永久等待。取得 claim 后的清理循环有5000 ms deadline，但 `:2840-2854` 超时只记录错误、清除 flag并从 `void` 函数返回。调用者 `main/nuwax_ai_partner_main.c:16369` 随后继续重连流程，无法知道物理 stop/reset未闭合，可能让旧播放状态污染新连接。

关闭条件：API返回 `esp_err_t`；claim和cleanup共用一个绝对总deadline；失败锁存 fault并阻止新 downlink/新连接进入可播放态，直到明确恢复或安全重启。并发 reset、owner不释放、speaker output stop超时和WS重连交错测试必须在有限时间得到一致终态。

### NET-IO-001（P2，配置依赖）：不安全 `ws://` DNS/TCP connect没有本地总deadline

`main/ws_transport.c:657-709` 的 test/insecure TCP路径先同步 `getaddrinfo()`，再对每个地址调用 blocking `connect()`，只有成功连接后才在 `:687` 配置 socket timeouts。正式 `wss://` 路径在 `:712-747` 使用ESP-TLS 15 s timeout并取得socket后设置非阻塞/超时，因此当前正式云路径边界更强；但工程仍支持不安全 test transport，DNS或TCP栈异常可长期占住connect caller。

关闭条件：`ws://` 和 `wss://` 使用统一解析/连接总deadline；socket在connect前设置non-blocking并poll，DNS使用可取消/有界 resolver；量产 profile继续 WSS-only。B级用无响应DNS、黑洞IP、多A/AAAA地址验证总时限。

### 已确认的正向/残余边界

- OTA header/download同时有 stall和transport total control；短包、超长响应、cancel和write/hash错误均进入cleanup。
- WebSocket I/O/lifecycle lock acquisition使用有限 timeout，close前会interrupt socket。`esp_tls_conn_destroy()` 仍可能在第三方实现中耗时，但静态代码不能证明它永久阻塞；保留为B/D TLS-destroy latency故障注入边界，不登记“已证实死锁”。
- 合法 `portMAX_DELAY` 主要用于 app-lifetime worker等待其唯一 queue/notification。凡对象可在运行期销毁者已分别由 `RTOS-007`、`LIFE-*`、`API-*` 审计，不能仅凭无限等待参数判死锁。

## 6. 错误传播审计

### ERR-001（P1）：power control初始化无法向 app传播关键GPIO/task失败

`nuwax_power_ctrl_init()` 为 `void`（`main/board/nuwax_power_ctrl.c:401-447`），`app_main` 在 `main/nuwax_ai_partner_main.c:15512` 无条件继续启动。power-hold GPIO配置/电平设置存在多个丢弃返回值（例如 `board/nuwax_power_ctrl.c:90-95,255-257,317,433`）；`start_power_key_task()` 在 `:378-397` 遇到PG1配置或task创建失败只记录日志并返回。结果是设备可能在无法确认PG2 latch或没有SW3 shutdown owner的情况下仍显示正常并连接云端，产品无法通过状态/API知道电源控制已降级。

关闭条件：init返回 `esp_err_t` 和结构化 capability/health；PG2 hold失败必须进入安全启动故障或受控重启，SW3 worker失败至少锁存UI/diagnostic fault并禁止宣称电源功能正常。逐点注入GPIO config/set和task allocation失败，验证app状态、日志和恢复路径。

### ERR-002（P2）：Wi-Fi fallback worker结果被重复且无条件丢弃

`main/nuwax_ai_partner_main.c:16018,16026` 两次调用 `nuwax_wifi_start_fallback_watcher()`，均不检查返回；`wifi_manager.c:396-406` 在task创建失败时明确返回 `ESP_FAIL`。重复成功调用因 `s_watcher_started`通常幂等，但该flag自身跨任务同步问题已由 `EVENT-002`覆盖；首次创建失败则双SSID fallback静默不可用，第二次可能偶然重试成功却仍无可观测状态。

关闭条件：只由单一启动owner调用一次并检查结果；失败进入带revision的网络capability/health快照，按有限退避重试或明确降级。任务分配失败测试要求UI/诊断能区分“主Wi-Fi可用、fallback不可用”。

### ERR-003（P2）：P3 L3实时处理和持久化失败可被伪装为成功

`main/nuwax_ai_partner_main.c:10937` 丢弃 `nuwax_p3_l3_process_chunk()` 的返回值。启用状态下AFE allocation失败会在实时frame路径反复尝试/记录并继续处理未经L3的样本，调用者没有结构化降级状态。`main/p3_afe_l3.c:90-99` 又忽略 `nvs_set_u8()`/`nvs_commit()`，即使persist失败仍在 `:103` 返回 `ESP_OK`；remote command因而可能ACK“已持久化”，重启后却恢复旧值。

关闭条件：AFE初始化从实时frame路径移到control owner；process失败锁存一次、显式bypass并上报health，不能每帧重试分配。持久化必须检查open/set/commit并区分“runtime已改/持久化失败”，command ACK返回最终状态与revision。生命周期并发根因另见 `API-002`，本项不重复计算。

### MEM-001（P1）：P3 AFE chunksize未验证，存在除零或固定栈buffer越界边界

`main/p3_afe_l3.c:52` 接受 `afe_aec_get_chunksize()` 的任意 `int`；`nuwax_p3_l3_process_chunk()` 在 `:122` 用它作除数，并在 `:124-138` 把该值用于固定 `int16_t ref_buf[256]`、software-ref读取、MR写入和memcpy。返回0会除零；负值转换到 `size_t` 可造成巨大长度；大于256会越界栈buffer。当前固定ESP-SR库预计返回受支持正值，但代码、构建断言和SBOM没有冻结 `1..256` 契约，依赖/ABI变化可把核心音频路径升级为崩溃或内存破坏。

关闭条件：ensure阶段验证chunksize >0、<=明确编译期上限且乘法不溢出；更稳妥是按验证后的chunksize分配reference buffer，所有长度使用`size_t`并检查。异常值必须拒绝enable、释放AFE并返回确定错误。B级用stub注入0/-1/257/极大值，C级核对实际library版本和runtime值。

### 324 个discard候选的总体判定

- host-only 6项不进入产品行为结论；其余318项按“wait结果仅用于唤醒”“best-effort清理”“状态/网络发送失败”“硬件/持久化关键错误”分类。
- `ulTaskNotifyTake`/`xTaskNotifyWait`在周期worker中丢弃返回通常只表示timeout/notification均触发下一次状态检查；不单独登记。
- stop/close/deinit清理返回被丢弃只有在主错误已经保留、资源owner仍可重试时才可接受；诊断TX stack UAF等已由 `LIFE-002/007`登记。
- UI delta publish失败、会议/监控ACK发送失败、WebSocket close失败等已有队列/协议/lifecycle问题归属，本项不另起重复ID。
- 本轮新增 `ERR-001/002/003` 与 `MEM-001`；A1-09将独立审计 `ESP_ERROR_CHECK`/assert/abort/restart，不在此处把fatal宏与普通error-return混算。

## 7. 跨任务/跨核可见性

### CROSS-001（P1）：P5 fast worker无同步访问conversation owner的完整 `s_prelisten`

`s_prelisten` 是 `main/nuwax_ai_partner_main.c:746` 的普通全局struct。P5 worker在 `:14015-14064` 独立运行，并由 `run_p5_during_playback_candidate_manual()` 在 `:15323-15339` 直接读取 `pending/capture_active`、调用 `cancel_auto_followup_capture()` 修改cancel状态，再次读取决定是否继续。主conversation owner同时在大量路径调用 `nuwax_prelisten_state_start_* / clear / finish_capture`；只有单个 `cancel_capture`字段可能被声明为volatile，struct其余字段及组合不受lock/atomic/revision保护。

这不仅是读到稍旧状态：worker可在主owner正切换pending/capture/mode/deadline时写cancel，形成普通C数据竞争、丢取消或错误拒绝P5；组合snapshot也可来自不同状态代次。

关闭条件：P5 worker不得直接改owner struct；通过typed command queue把“请求取消并取得snapshot/revision”交给conversation owner，owner返回不可变结果和generation。若保留共享对象，完整state mutation必须使用同一mutex/atomic state machine。用P5 command与prelisten start/finish/handoff/WS reconnect全排列测试，要求revision单调、一次请求只影响目标capture generation。

### CROSS-002（P2）：Vocat诊断TX上下文用 `volatile bool` 代替任务同步

`main/board/vocat_audio_board.c:84-99` 的三个TX context以 `volatile bool stop/tone`跨owner和worker共享；worker在 `:138-194`循环读取，owner在 `:828,1399,1569,1649,1833`写入。`volatile`不提供C11 happens-before、原子性或完成确认；编译器/双核可见性契约不足，尤其tone切换和stop/join超时路径可能观察陈旧值。实际stack-context提前返回的更严重后果已由 `LIFE-002`登记，本项只记录同步原语缺口。

关闭条件：使用C11 atomic acquire/release或task notification/event bit；stop必须与join完成信号组成生命周期协议。诊断task交错测试要求stop在deadline内可见，tone revision不丢失，timeout后owner不释放live context。

### 既有跨核问题交叉引用

以下已在A1-01~A1-05登记，不重复编号：

- `WS-CB-001`：64位WebSocket activity timestamp用volatile共享；
- `WS-CB-002`：meeting/monitoring RX callback读取mode owner状态；
- `EVENT-002/003`：Wi-Fi/provisioning跨任务普通状态；
- `RTOS-006`：face cache metadata/buffer跨核；
- `API-002/005/007/010`：P3 L3、cloud config、software ref、LVGL callback pair同步缺口。

## 8. 新增问题汇总和关闭等级

| ID | 等级 | 类型 | 当前产品是否直接可达 | 最小关闭证据 |
|---|---|---|---|---|
| `TIME-001` | P1 | 100 Hz下speaker零tick轮询 | 是 | B/D contention timing + heartbeat；修复后无0-tick正延时 |
| `WAIT-001` | P1 | speaker reset ownership无deadline/void吞错 | 是 | B/D并发reset/stop/重连故障注入，有限一致终态 |
| `ERR-001` | P1 | 电源初始化失败不可传播 | 是 | B/C/D逐点失败注入与安全启动状态 |
| `MEM-001` | P1 | AFE chunksize除零/栈越界边界 | L3启用时 | B stub异常值 + C实际library/runtime契约 |
| `CROSS-001` | P1 | P5 worker与conversation owner共享prelisten | P5命令路径 | B交错/property test，单owner revision handoff |
| `TIME-002` | P2 | SW3支持profile可配置零tickpoll | 当前值安全 | B配置边界 + D去抖测量 |
| `NET-IO-001` | P2 | `ws://` DNS/connect无总deadline | test/insecure路径 | B黑洞DNS/IP总时限；量产WSS-only |
| `ERR-002` | P2 | fallback创建结果重复丢弃 | 是 | B allocation failure + capability状态 |
| `ERR-003` | P2 | L3 process/persist错误伪成功 | L3命令/启用路径 | B allocation/NVS failure +最终状态ACK |
| `CROSS-002` | P2 | 诊断TX volatile同步 | 诊断路径 | B/D stop/tone/join交错 |

本轮未确认新的直接P0。`MEM-001`具有潜在内存破坏后果，但触发依赖固定第三方AFE API返回违反预期契约；在完成依赖/SBOM与runtime值证明前按P1发布阻塞处理，而不声称当前设备已发生栈破坏。

## 9. A1-06 / 计划 1.7~1.13 反向校验

| 计划要求 | 本文件证据 | A级状态 |
|---|---|---|
| 1.7 无条件循环、queue/I/O/NVS/LVGL等待 | 37循环表、WAIT/NET-IO章节及既有ID交叉引用 | 已完成静态分类；动态I/O时延开放 |
| 1.8 tick、wrap、亚tick和微秒混用 | 152转换分母、TIME-001/002、正向ceil/us控制 | 已完成静态核对 |
| 1.9 ISR/DMA | 本项仅核对与阻塞/时间交叉面；完整矩阵在A1-05 | 由 `phase1-callback-isr-audit.md`覆盖，不重复 |
| 1.10 错误传播 | 324候选分母、ERR-001~003、MEM-001 | 已完成高风险调用链静态归属；fatal路径留A1-09 |
| 1.11 跨核可见性 | CROSS-001/002及既有WS/EVENT/RTOS/API IDs | 已完成静态归属 |
| 1.12 锁/信号量/持锁I/O/portMAX_DELAY | WAIT-001、合法worker waits、A1-02交叉引用 | 已完成静态分类；故障注入开放 |
| 1.13 WDT/CPU/owner/恢复 | TIME-001 CPU风险、RTOS-001/003/005交叉引用 | 静态边界完成；trace/heartbeat需B/D |

## 10. 阶段结论

- 37个无条件循环已全部分类，未把app-lifetime queue wait误报为死锁，也未把有wall-clock deadline但零tick轮询的speaker路径误报为安全。
- 新登记5个P1：`TIME-001`、`WAIT-001`、`ERR-001`、`MEM-001`、`CROSS-001`；5个P2：`TIME-002`、`NET-IO-001`、`ERR-002`、`ERR-003`、`CROSS-002`。
- OTA transfer deadline、touch/ES7210精确5 ms延时、WebSocket有限I/O lock和wrap-safe deadline helper是正向控制。
- 本文件只完成A1-06的A级静态闭环；CPU占用、TLS destroy、DNS黑洞、任务heartbeat、NVS/allocator failure和真实硬件时序仍需相应B/C/D授权，不能写成产品已通过。
