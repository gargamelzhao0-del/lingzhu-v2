# A2-07 启动失败、半初始化与资源回滚审计

日期：2026-08-03  
证据等级：A（静态源码审计）  
产品状态：未通过；B/D 故障注入开放  
对应计划：阶段 2 `A2-07`

## 1. 结论

`A2-07` 的 A 级静态审计完成，但验收条件“每个启动失败点资源回到基线或进入可诊断的受控重启”不成立。

主要结论如下：

1. `app_main()` 仍是没有显式阶段 owner 的线性启动脚本。中后段失败多为 `ERROR + return`，已经创建的常驻任务、队列、mutex、controller、音频预留和驱动不会统一逆序退出，继续归入 `RTOS-008`。
2. OTA 启动最早抢占 SW3 shutdown gate；WebSocket pointer mutex、OTA core/pending 检查或 controller task 后续失败时没有统一释放 gate、撤销 callback/bind/interlock，继续归入 `LIFE-010`。其中 pointer mutex OOM 会直接留下“启动失败且关机仍受抑制”的半启动状态。
3. Wi-Fi event group、netif、driver、两个 handler 和 start 分段成功后，任一步失败都直接返回；handler instance 与 netif 没有保存为模块 owner，重试可能重复注册或泄漏，继续归入 `RTOS-009/EVENT-001`。
4. LVGL port 在 mutex、draw buffer、display/indev、tick timer、task TCB/stack 与 task 之间没有统一失败清理；`lv_display_create()`/可选 `lv_indev_create()` 还未检查空返回，继续归入 `LIFE-004`。
5. `create_app_task*()` 成功后没有统一保存静态 task 的 TCB/stack ownership，多数调用甚至不保存 task handle。后续失败时不能通过简单补一个 `vTaskDelete()`完成资源守恒。
6. 电源控制 init 仍为 `void`，PG2/SW3 GPIO 或 power-key task 失败只能打印日志，不能阻止下游显示、网络和云连接，继续归入 `ERR-001`。
7. 显示 SPI/DMA、会议/监控 controller、共享音频板/I2S/codec、麦克风、wake detector、LED、battery 和 BLE handler 注册存在若干局部正向回滚。它们只能证明模块内部分路径较完整，不能外推为整机启动事务已闭合。

本项没有新增 canonical issue ID；复用并增强 `RTOS-008`、`RTOS-009/EVENT-001`、`LIFE-004`、`LIFE-010`、`ERR-001`。这避免把同一启动 ownership 根因重复计数。

## 2. 范围、方法与并行开发边界

### 2.1 纳入范围

- `app_main()` 从 OTA/power/NVS 到 WebSocket 恢复循环前的全部启动切点；
- task、queue、mutex、event group、timer、LVGL object、netif、event handler、Wi-Fi driver、I2S/DMA、codec、controller 与大块音频内存；
- init 失败后的局部 cleanup、全局 rollback、重复 init 和 retry 语义；
- 已有 `A1-03/A1-04/A2-01` 结论的重新落码核对。

### 2.2 未执行

- 没有运行 host/gateway test；
- 没有构建或链接固件；
- 没有访问 COM5、NVS、eFuse；
- 没有 OTA、烧录或真机故障注入；
- 没有修改产品源码、配置、协议或资产。

### 2.3 并行开发处理

本项开始时，`A2-06` 源清单为 7,918 项。审计期间重新复算发现文件集合无增减，但 `main/nuwax_ai_partner_main.c` 发生一次并行修改；因此旧 hash 不作为本项证据。受影响主入口已按最新内容重新审查。最终 source manifest 和本文件 hash 在收口校验后记录。

由于 `NuwaxAIPartner-v2` 整目录未被上级 Git 跟踪，不能用 `git diff` 区分工程师改动。后续每个工作项均继续使用文件系统全量 SHA-256 检测漂移。所有阶段完成后，必须对最终源码重新执行一次逐项代码校验；任何已审文件发生变化，其相关 A 级结论自动失效，直到重新落码确认。

## 3. 失败分类

| 分类 | 定义 | 当前代表路径 | 判定 |
|---|---|---|---|
| `CLEAN` | 失败后本模块对象逆序释放，可安全返回/重试 | display SPI/DMA；meeting/monitoring queues；wake init；speaker output；mic；LED | 静态局部成立，仍待故障注入 |
| `RETAIN-RETRY` | 刻意保留可复用资源，状态可识别且不会重复创建 | touch I2C device/queue 的同句柄重试；battery static sync objects | 设计可成立，必须证明资源有界且 retry 幂等 |
| `RESTART-ONLY` | live worker/资源不能安全 unwind，必须携带原因受控重启 | OTA/全局启动理想兜底；现有少数 audio worker deadline 路径 | 当前启动失败没有统一实现 |
| `OPEN` | 失败后留下对象、任务、注册、gate 或无法证明的 owner | app_main、LVGL、Wi-Fi、OTA controller、部分常驻 worker | 产品验收失败 |

## 4. `app_main()` 启动失败切点矩阵

| 阶段/切点 | 失败时已可能存活 | 当前动作 | 静态结果/问题 |
|---|---|---|---|
| OTA boot shutdown inhibit，`nuwax_ai_partner_main.c:15519-15525` | shutdown gate 已 inhibited | 失败只日志；成功后依赖 OTA 后续决定释放 | gate owner 在 controller 建立前已生效；后续失败缺统一释放，`LIFE-010/ERR-001` |
| NVS init，`:15557-15562` | power-key task、shutdown gate | `ESP_ERROR_CHECK` panic | 有界终止但缺 fatal site/crash-loop策略；恢复语义由 `NVS-001/OBS-001` 跟踪 |
| OTA WS pointer mutex，`:15566-15571` | power-key task、shutdown gate | `ERROR + return` | mutex OOM 后 shutdown inhibit 可永久保留至复位；`RTOS-008/LIFE-010` |
| OTA device init，`:15572-15580` | 上述对象、OTA queue/core callback/bind/pending/interlock 的子集 | `ESP_ERROR_CHECK` panic | controller内部仅删queue，不完整撤销；可启动循环，`LIFE-010` |
| preroll 分配，`:15583-15587` | OTA controller、WS mutex、power task/gate | `ERROR + return` | 大块 PSRAM 失败时全局无 unwind，`RTOS-008` |
| speaker owner queue/manager，`:15596-15624` | OTA、preroll、power | `ERROR + return` | manager局部分配失败可清自身ring，但前置资源不回滚；speaker manager没有完整deinit API，`RTOS-008` |
| live AEC pool，`:15637-15643` | 前置全部 | deinit自身并降级继续 | 本模块 fail-closed 正向；普通 capability 语义仍由 `START-002` 跟踪 |
| touch trigger/events，`:15645-15656` | 前置全部 | `ERROR + return` | 两对象无实际生产/消费且前置/已创建对象不删，`LIFE-003/RTOS-008` |
| app queue batch，`:15657-15683` | 先创建成功的 queue/mutex 与前置全部 | 批量末尾检查后 `ERROR + return` | 典型 partial allocation；无逐项 delete，`RTOS-008` |
| meeting/monitoring init，`:15685-15712` | 全局 queues、speaker/AEC/OTA/power | monitoring失败会deinit meeting；其余前置不回滚 | controller局部 `CLEAN`，全局仍 `OPEN` |
| flash task，`:15713-15720` | 两个 mode controller和前置全部 | 只deinit mode controller后 return | task创建前自身TCB/stack失败会释放；全局前置不回滚，`RTOS-008` |
| display/touch/LVGL/UI，`:15738-15815` | flash及全部前置 worker/objects | 多数降级继续 | display局部clean；LVGL partial init不clean；readiness由 `START-001/002` 跟踪 |
| shell/touch gesture task，`:15938-15955` | UI、battery、flash、mode、OTA、audio、power | create失败直接 return | 成功task没有TCB/stack owner；后续失败无法回收，`RTOS-008` |
| Wi-Fi init，`:15967-15974` | 本地tasks及全部前置 | `ERROR + return` | Wi-Fi自身也可能partial；`RTOS-008/009` |
| BLE provisioning，`:15978-16043` | Wi-Fi及本地全部前置 | 调用stop/凭据rollback后部分路径return | provisioning局部rollback明显优于主链；in-flight handler join仍由 `RTOS-007` 跟踪 |
| fallback watcher/connect，`:16048-16060` | watcher及全部前置 | watcher start返回值被忽略；connect失败return | 无统一cleanup；重复start当前由bool抑制但task创建失败不可观测，`ERR-002/RTOS-008` |
| wake与后续五个常驻worker，`:16071-16103` | Wi-Fi在线和全部前置 | wake失败降级；任一worker创建失败return | 前面已启动的worker无stop，static task内存owner丢失，`RTOS-008` |
| speaker physical preallocation，`:16103-16108` | speaker task与完整runtime | void prepare，只通过ready标记 | 物理输出失败不会回滚启动；普通能力由 `START-002` 跟踪 |

## 5. 模块级回滚矩阵

| 模块 | 关键资源 | 失败清理 | 重复 init/retry | A级判定 |
|---|---|---|---|---|
| display | 3个DMA buffer、SPI bus/device/acquire | `st77916_release_init_resources()`逆序 release/remove/free，下一次 init 还先检查残留 | 有显式恢复入口 | `CLEAN`，但 cleanup API 失败仍需D验证 |
| LVGL port | mutex、draw buffers、LVGL display/indev、esp_timer、TCB/stack/task | 多个直接return；无deinit；buffer为局部变量，成功后也无独立owner字段 | 重复init会覆盖全局mutex/display并泄漏 | `OPEN`，`LIFE-004` |
| Wi-Fi | event group、default netif、driver、2个handler、started state | 任一步失败直接return；handler instance为局部变量 | `s_wifi_initialized`只在最后置true，partial后重试会重新创建/注册 | `OPEN`，`RTOS-009/EVENT-001` |
| OTA device/core | command queue、callbacks、core init/bind、pending state、shutdown/audio interlock、controller task | init失败主要只删queue；无device/core deinit/unregister | `s_initialized`仅最后置true，partial后可再次init但旧core/interlock仍在 | `OPEN`，`LIFE-010` |
| BLE provisioning | 4个event handler、manager、security buffers、vox queue/gate | partial handler统一unregister；init `goto fail` deinit manager；stop释放主要对象 | init/stop可重新进入；in-flight handler未join | 局部较完整；`RTOS-007`仍开放 |
| meeting/monitoring | mode queues/ACK queue | 第二个queue失败删除第一个；deinit清零 | 可重新init | `CLEAN`（静态） |
| wake detector | lock、model list、MultiNet/WakeNet data、task TCB/stack | init统一 `goto fail -> detector_release_resources()`；deinit只在task确认STOPPED后释放 | 状态机显式，但stop总deadline问题另属`RTOS-005` | init `CLEAN`；stop动态开放 |
| shared Vocat audio board | TX/RX DMA channel、codec ctrl/if、GPIO if | init任一步失败调用完整deinit；codec依赖先删，再disable/delete I2S | `audio_board` refcount/lease防止final deinit | `CLEAN`（静态），真实driver delete错误待D |
| mic/speaker output | audio-board lease或独立I2S、buffer、mutex | 主要失败路径释放lease/channel/buffer/owner | stale speaker output在retry前先deinit | `CLEAN`（静态） |
| speaker manager | PSRAM ring、mutex、常驻task、物理output | init自身ring/mutex失败可清；无完整manager stop/deinit，只有output deinit | 重复manager init可覆盖既有ring/mutex | 局部 init clean；runtime owner `OPEN`，归`RTOS-008` |
| live AEC pool | AFE/AEC内部大块internal内存 | prepare/reserve失败立即deinit并标 unavailable | 失败状态被缓存，boot内不再尝试 | fail-closed且资源clean；是否允许永久降级由`START-002`决定 |
| touch | static lock、queue、I2C device、log task、ISR handler | 连接失败刻意保留queue/device用于同句柄重试；task创建失败清handle | retry有cooldown；无完整stop/deinit | `RETAIN-RETRY`，需证明有界；shutdown/global rollback仍开放 |
| battery/LED | static sync对象、worker、driver/PWM | task创建失败清局部状态；stop按请求/确认/delete | battery保留static sync；LED init失败删sync | 局部较完整；并发stop/set另由`LIFE-006`跟踪 |
| app static-task helper | heap TCB、heap stack、task | 创建失败会释放；成功后不保留TCB/stack owner | 多数task无handle，无stop/join | `OPEN`，是`RTOS-008`修复前置 |

## 6. 重点问题落实

### 6.1 `RTOS-008`：全局启动不是事务

问题不是单纯“有几处忘记 free”，而是启动没有统一 ownership ledger：

- 没有 `stage` 或 capability revision；
- 没有按依赖逆序执行的 `stop -> join -> deinit -> delete`；
- task helper 在成功后丢失 TCB/stack ownership；
- 多个模块只提供 init/start，没有完整 stop/deinit；
- `ERROR + return` 后 FreeRTOS scheduler仍运行，残留worker继续访问静态全局对象；
- 当前 boot 通常不会在同一 `app_main()` 自动重跑，但 OTA health、未来子系统重试、测试故障恢复和软重启前quiesce都需要该契约。

因此不能用“设备最终可硬重启”关闭该问题。可接受的两条产品策略只有：

1. 完整启动事务：每一阶段由唯一owner记录资源，失败时逆序有界rollback；或
2. 明确的受控重启：记录fatal site/stage/原因，先进入安全输出状态，再由crash-loop breaker决定重启/回退/safe mode，禁止留下不可诊断的半启动。

### 6.2 `LIFE-010`：OTA boot gate 与controller rollback不闭合

具体链路：

1. `nuwax_ota_device_inhibit_shutdown_during_boot()`在power controller初始化前就把shutdown gate设为 inhibited；
2. pointer mutex OOM会直接return，没有release gate；
3. `nuwax_ota_main_register/init`、pending检查或interlock失败只删除command queue；
4. controller task创建失败也没有撤销core callback/bind与pending interlock；
5. device/core没有公开的stop/deinit/unregister入口；
6. app以`ESP_ERROR_CHECK`重启，若根因持续存在会形成不可区分的启动循环。

修复必须把 boot inhibit 也纳入同一 transaction：只有controller接管成功才转移所有权；任何此前失败都必须释放gate。pending-verify时若无法安全释放，则应记录site并走可诊断rollback/restart，而不是普通return。

### 6.3 `RTOS-009/EVENT-001`：Wi-Fi重复注册与partial driver

`wifi_init_once()` 需要保存：event group、是否拥有default event loop、STA netif、Wi-Fi init/start状态、WIFI handler instance、IP handler instance。失败时按以下顺序撤销：

1. stop Wi-Fi（若started）；
2. unregister IP handler；
3. unregister WIFI handler；
4. deinit Wi-Fi driver；
5. destroy default STA netif；
6. 仅在本模块创建且全局无其他用户时处理default event loop；
7. delete event group并清全部静态状态。

default netif/event loop具有进程级共享语义，不能不加所有权判断就无条件销毁。B级fake必须验证每个cut后第二次init只有一组callback，并且事件只到达一次。

### 6.4 `LIFE-004`：LVGL partial init和空返回

当前失败点包括：

- draw buffer OOM后mutex保留；
- `lv_display_create()`未判空，可能在setter处崩溃；
- 可选`lv_indev_create()`未判空；
- timer create/start失败后display/indev/buffer/mutex保留；
- task TCB或stack分配失败时timer已周期运行；
- task创建失败时TCB/stack/timer/UI objects全部保留；
- buffer仅存在于局部变量，缺少deinit所需owner；
- 没有初始化状态和幂等guard，重复init会覆盖全局句柄。

修复需要把全部对象存入一个context，任务启动作为最后一步；`goto fail`按 task/timer/indev/display/buffer/mutex 逆序回收。若LVGL本身不支持在同一boot完全`lv_deinit()`，则模块必须明确“一次init，失败后受控重启”的策略，不能返回后允许隐式重试。

### 6.5 `ERR-001`：电源init错误不可传播

`nuwax_power_ctrl_init()` 和内部 `start_power_key_task()` 均为void。PG2 hold、PG1输入或power-key task失败后，app仍继续。修复需要结构化返回：

- `hold_asserted`；
- `key_input_ready`；
- `key_task_ready`；
- `shutdown_gate_owner`；
- `fatal/degraded policy`。

PG2 hold失败在电池供电产品上应是mandatory fatal；power-key task失败至少必须阻止声明product-ready，并提供替代安全关机/受控重启策略。

## 7. 正向控制与不可外推边界

| 正向控制 | 代码事实 | 不能外推为 |
|---|---|---|
| display init有完整rollback并在retry前清残留 | `display.c:863-1039` | LVGL/UI也会回滚；当前不会 |
| Vocat audio board失败调用deinit | `vocat_audio_board.c:631-691,1892-1994` | 所有runtime audio worker均可停止；speaker manager/task仍无全局deinit |
| wake init统一fail cleanup | `wake_word_detector.c:577-720` | wake stop永不阻塞；`RTOS-005`仍开放 |
| meeting/monitoring第二队列失败会删第一队列 | `meeting_mode.c:950-987`；`monitoring_mode.c:1448-1485` | app后续失败会deinit两者；仅flash task失败路径会做局部deinit |
| provisioning handler partial register会统一unregister | `nuwax_provision_main.c:325-381` | stop不会与in-flight handler竞争；`RTOS-007`仍开放 |
| battery/LED有请求-确认-delete主路径 | `battery_service.c:360-439`；`led_driver.c:133-194` | shutdown coordinator已调用这些stop；当前SW3不会 |

## 8. 修复架构与顺序

建议按以下顺序实施，避免先写零散cleanup又无法获得owner：

1. 定义 `nuwax_runtime_owner_t`，记录每个stage、capability、task `{handle,tcb,stack,state}`、queue/mutex/controller/driver ownership；
2. 改造 `create_app_task*()` 返回完整task token，禁止成功后丢失TCB/stack；
3. 为常驻模块补齐 idempotent `stop(deadline)/join/deinit`，先覆盖flash、shell、touch gesture、Wi-Fi workers、P3/P5、speaker、OTA；
4. 建立 `startup_rollback(owner, failed_stage, err)`，先阻止新producer，再停止网络/音频/UI worker，最后释放driver/memory/sync对象；
5. 独立修复LVGL、Wi-Fi和OTA模块内部partial init，使模块可以在不依赖app全局cleanup时资源守恒；
6. 把power/OTA gate改成可转移owner的token，禁止无owner的全局bool锁存；
7. 发布带revision的capability snapshot，mandatory failure走安全重启，optional failure显式degraded；
8. 加fatal breadcrumb、crash-loop breaker和OTA pending rollback策略；
9. 执行逐点fault injection，验证heap/task/handler/DMA基线与第二次init；
10. 最后执行真机低内存、driver错误、断电和OTA pending矩阵。

## 9. B/D关闭矩阵

| 等级 | 故障注入 | 必须断言 |
|---|---|---|
| B | app每个queue/mutex/task allocation cut逐点失败 | task/queue/heap回到基线，或产生唯一受控restart decision；无后台日志继续增长 |
| B | static task helper的TCB、stack、create分别失败；随后重复startup | 所有token可回收；同名worker至多一个；无TCB/stack泄漏 |
| B | LVGL display/indev/timer/TCB/stack/task每点失败 | 不空指针；timer不残留；buffer/mutex/object守恒；retry策略明确 |
| B | Wi-Fi event group/netif/init/handler1/handler2/mode/start逐点失败，再次init并投递事件 | 一组netif/driver/handler；每事件只回调一次；event group与heap守恒 |
| B | OTA register/core init/pending/interlock/task create逐点失败 | shutdown/audio interlock归零或进入明确pending安全重启；callback/bind/queue/task无残留 |
| B | meeting/monitoring/wake/speaker/mic/display/LED/battery init逐点失败并重复init | 局部正向控制得到动态证明；资源水位不下降，owner/lease归零 |
| D | internal/PSRAM/DMA低内存分层注入 | 屏幕、音频、Wi-Fi、OTA失败策略符合mandatory/optional contract；SW3仍可安全处理 |
| D | I2S/SPI/I2C/codec/netif/Wi-Fi driver错误 | DMA/ISR/handler无残留；PA、背光和总线进入安全态；下次启动可恢复 |
| D | OTA pending-verify各启动cut连续复位 | 不提交坏candidate，不无限crash-loop；可回滚到已验证槽，gate不永久锁死 |
| D | 1000次启动/故障/恢复循环 | task、heap、DMA、handler、NVS/OTA状态无累积；每次失败site可诊断 |

所有run必须记录源码清单hash、配置hash、设备ID、硬件版、故障cut、复位原因、前后heap/DMA/task/handler计数和最终状态。没有这些字段不能作为关闭证据。

## 10. 计划反向校验

| 计划要求 | 本文件证据 | 状态 |
|---|---|---|
| 所有启动失败的悬挂任务 | 第4、5、6.1节；task helper owner缺口 | A级完成，B/D开放 |
| 半初始化资源 | 第4、5节；LVGL/Wi-Fi/OTA矩阵 | A级完成，验收未通过 |
| 重复注册 | 第5、6.3节；Wi-Fi handler/netif owner | A级完成，B/D开放 |
| DMA回滚 | display/audio board/mic/speaker正向矩阵 | A级静态主路径完成，driver失败与D级开放 |
| 每个失败点回到基线或受控重启 | 第4、6节 | 不满足 |
| 逐点分配/驱动故障注入 | 第9节 | 未执行，需要B/D授权 |

## 11. A2-07判定

- A级静态审计：**完成**。
- 产品验收：**未通过**。
- 新问题：**0**；复用并增强5组既有问题。
- B/D故障注入：**开放**。
- C级构建：本项不是独立关闭手段；后续修复后仍必须通过干净build/link。
- 本轮没有修改产品源码、配置、协议或资产，也没有运行测试、构建、COM5、NVS、OTA或烧录。
- 全工程审计完成后的最终代码复核：**强制待执行**。必须基于最终source manifest逐项重新核对所有受漂移影响的结论，不能直接复用执行过程中间快照。
