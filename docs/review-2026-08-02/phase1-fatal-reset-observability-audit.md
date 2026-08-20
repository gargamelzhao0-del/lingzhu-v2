# 阶段 1 A1-09：致命退出、复位与可观测性静态审计

审计日期：2026-08-02  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
权限：A 级只读；只新增评审证据，未修改业务源码/配置，未构建，未运行 host test，未操作 COM5，未写 NVS，未 OTA/烧录。

2026-08-03 A2-08续接更正：本文件主体的行号和1,494个日志站点属于A1-09中间快照。当前7.0.24 manifest下，致命调用分母仍为3个hard check、32个without-abort、3个abort、4个restart、0个正式assert/configASSERT；日志调用按150个当前正式C源重算为1,481处（E494/W412/I538/D37/V0），66个文件有日志。当前UART0是GPIO43/44、921600 baud，并启用secondary USB Serial/JTAG，不再使用主体中的115200描述。最新阶段2结论以`phase2-reset-recovery-matrix.md`第10节为准；历史数字保留用于说明当时快照，不得继续当作当前配置。

## 1. 范围、分母与结论边界

本工作项对应计划 1.18 和 1.20。源码分母不是全目录模糊搜索，而是从当前 `main/CMakeLists.txt` 解析出的 150 个唯一正式 `.c` 条目，并加入产品 bootloader 组件 `bootloader_components/nuwax_power_hold/nuwax_early_power_hold.c`，合计 151 个文件、0 缺失。新增 NXFP parser 不包含 fatal 入口；host check/stub、未链接原型和 `_archive` 不计入产品致命入口，它们的 `assert()` 只属于主机测试，不得误报为设备固件断言。

| 输入 | SHA-256 |
|---|---|
| `source-manifest.sha256`（7,912项） | `ade1a6755435869651cd277bef9d865b2fc59e4af61a738d40eee0f3c959f4e8` |
| `main/CMakeLists.txt` | `e0e721f1451c5a059ab5a23d68a7b3668eee8e9855dd7ce23617c1a17cca3987` |
| 当前 `sdkconfig` | `b09f3ade1df3c2fae52993403dde3fd8e14485d78ada76ca58ffd65d048461fe` |
| 活动 `partitions.customwake.csv` | `e498063228a1a46792d3243a8ec29c2c5e40b7556e9885f16dc6e5729ee05a19` |

静态结论：没有发现新的直接 P0。正式产品路径有 3 个硬 `ESP_ERROR_CHECK`、32 个 `ESP_ERROR_CHECK_WITHOUT_ABORT`、3 个 `abort()` 和 4 个 `esp_restart()`；没有 `assert()`/`configASSERT()`。新增三个 canonical P1：`OBS-001`（复位现场、致命站点和 crash-loop 恢复证据缺失）、`LOG-001`（WS RX/音频实时路径没有统一日志速率预算）与 `PRIV-001`（默认 INFO 日志输出用户会话正文和稳定设备/网络标识）。Task heartbeat、OTA 初始化和 NVS 启动问题分别沿用 `RTOS-001`、`LIFE-010`、`NVS-001`，不重复计数。阶段 1 的 A 级静态 P1 因此由 31 个增加到 **34 个**，仍为 0 个直接 P0；B/C/D 动态关闭证据均未执行。

## 2. 正式致命入口精确计数

| 检索项 | 命中数 | 文件数 | 解释 |
|---|---:|---:|---|
| `ESP_ERROR_CHECK(...)` | 3 | 1 | 错误时进入 panic/abort 路径 |
| `ESP_ERROR_CHECK_WITHOUT_ABORT(...)` | 32 | 11 | 记录错误后继续；不是致命入口，但必须核对是否安全降级 |
| `abort()` | 3 | 3 | 三条均用于未提交 audio uplink 无法安全销毁时 fail-closed |
| `esp_restart()` | 4 | 3 | 两条安全性重启、两条预期产品重启 |
| `assert()` | 0 | 0 | 正式源无命中；host check 中的断言不进入固件 |
| `configASSERT()` | 0 | 0 | 正式源无命中 |

计数规则使用带单词边界和调用括号的语法检索，`ESP_ERROR_CHECK` 不会把 `ESP_ERROR_CHECK_WITHOUT_ABORT` 重复计入。条件 OTA 的 5 个 CMake 源也在分母中，因为当前产品 `sdkconfig` 启用 OTA。

## 3. 硬终止与重启路径清单

| 位置 | 类型与触发 | 终止前动作 | 静态判定 | 关联问题 |
|---|---|---|---|---|
| `nuwax_ai_partner_main.c:15542-15547` | NVS init 返回页耗尽/新格式时全擦；erase 或再次 init 失败由 `ESP_ERROR_CHECK` panic | `nuwax_power_ctrl_init()` 已执行，随后没有应用级恢复状态 | 失败本身可视为启动期不可恢复，但“先全擦”会丢失凭据/OTA floor；持续失败可循环 panic | `NVS-001`；现场缺失归 `OBS-001` |
| `nuwax_ai_partner_main.c:15565` | `nuwax_ota_device_init()` 任一步失败由 `ESP_ERROR_CHECK` panic | OTA init 可能已注册 core、装载 pending 状态或施加 interlock | 不是单纯 OOM 退出；部分初始化回滚不闭合且可形成启动循环 | `LIFE-010`；现场缺失归 `OBS-001` |
| `meeting_mode.c:685-700` | listen-start 发送失败，且未提交 uplink sender 无法在 deadline 内 discard 时 `abort()` | 请求关闭同一 WS epoch；记录最后一条 error | sender 仍持调用栈对象，直接 unwind 会 UAF；发送 finish 又会为未提交 listen 伪造 END。fail-closed 合理，但复位后只剩 PANIC 类别 | `OBS-001` |
| `monitoring_mode.c:686-700` | 与 meeting 同类的未提交 sender discard 失败时 `abort()` | 请求关闭同一 WS epoch并记录 error | 与 meeting 相同；不能把安全 abort 写成普通可恢复 return | `OBS-001` |
| `p5_fullduplex_overlap_probe.c:907-923` | P5 诊断 listen-start 失败且 sender 无法 discard 时 `abort()` | 请求关闭同一 WS epoch并记录 error | 诊断路径也不能释放仍被 worker 引用的栈对象；缺统一 fatal breadcrumb | `OBS-001` |
| `audio_uplink.c:1194-1222` | sender 在 stop deadline 后仍未确认结束时 `esp_restart()` | 先请求关闭绑定 epoch，继续有限等待；不释放 live task/queue/pool | 防止 UAF 的安全重启，优于强删；但与正常软件重启同为 `ESP_RST_SW` | `OBS-001`；worker liveness 交叉引用 `RTOS-001` |
| `nuwax_ai_partner_main.c:7320-7337` | realtime capture task 未在 stop deadline 内确认时 `esp_restart()` | 不释放 live capture resources，重启后永久 park | 防止 task 继续访问已释放 frame/queue；缺失持久化站点与运行代数 | `OBS-001`；worker liveness 交叉引用 `RTOS-001` |
| `nuwax_ai_partner_main.c:5197-5225` | BLE 重配网状态成功保存后预期 `esp_restart()` | 写入 rollback/reprovision 状态，延时 500 ms | 正常产品重启；局部持久化状态可辅助推断，但没有统一 reset-intent 记录 | `OBS-001` |
| `nuwax_ota_new/nuwax_ota.c:385-399` | 已验证 OTA 镜像 staged 后预期 `esp_restart()` | 回调发布 reboot-pending；OTA report/boot state已持久化 | 正常产品重启；OTA 上下文较完整，但 reset reason 本身仍只显示 SW | `OBS-001`；版本绑定沿用 `VER-001/005` |

三个 `abort()` 都发生在“sender 仍拥有当前调用栈对象、listen-start 又未提交”的窄失败链上。当前代码没有在已知可安全 unwind 的普通网络错误上无条件 abort；因此本审计不把 `abort()` 本身重复登记为新缺陷，问题是所有 panic/安全重启缺少统一、可跨重启读取的原因和证据。

## 4. `WITHOUT_ABORT` 继续运行路径

32 个调用全部会由 IDF 宏记录错误但继续执行。按后果完整分组如下，分组计数总和为 32：

| 分组 | 数量 | 精确位置 | 继续运行语义 |
|---|---:|---|---|
| 电源/板级 best-effort | 3 | `nuwax_power_ctrl.c:94,116`；`vocat_audio_board.c:658` | pad hold/RTC GPIO/可选 codec power GPIO 失败后继续；PG2 和 SW3 的总体失败传播仍由 `ERR-001` 跟踪 |
| cleanup/deinit | 6 | `p5_fullduplex_overlap_probe.c:1106`；`speaker_playback.c:3098`；`mic_input.c:817,819`；`nuwax_ai_partner_main.c:13437`；`p3_softref_aec_diag.c:833` | 释放失败只记录，调用者多继续释放内存或返回；属于 cleanup telemetry，B/D 仍需验证资源是否真正归零 |
| UI/外设启动与像素写降级 | 9 | `display.c:1063`；`nuwax_ai_partner_main.c:15623,15724,15738,15748,15768,15786,15792,15916` | live AEC pool、display、touch、LVGL、app shell、battery 或诊断 task 可失败后降级；部分调用随后显式检查结果，部分能力仅靠日志可见 |
| 协议 ACK/pong best-effort | 6 | `session_dispatch.c:22`；`nuwax_ai_partner_main.c:3878,14090,14436,14478`；`ws_transport.c:1312` | ACK/pong 失败不重启；主业务继续或由 epoch/连接关闭逻辑收敛，协议丢失需后续阶段 5 验证 |
| Wi-Fi 持久化/scan list 清理 | 6 | `wifi_manager.c:686,687,730,735,775,776` | 当前连接或失败结果仍返回，但凭据保存/列表清理可能失败；持久化一致性沿用 NVS/阶段 5 审计 |
| 网络主循环 | 2 | `nuwax_ai_partner_main.c:16137,16197` | 时间同步失败继续连云；hello 发送错误另有显式关闭分支，非静默继续 |

本表只确认“不会因宏本身 panic”，不等于所有降级策略已经产品验收。例如 display/touch/battery 失败后继续运行可能符合无头降级，也可能违反本产品的必选能力；其产品级必选/可选判定分别留在阶段 2、3、7 和 V1->V2 等价矩阵中。

## 5. Reset、崩溃、heartbeat 与日志矩阵

| 能力 | 当前证据 | 静态结论 |
|---|---|---|
| Reset reason | `nuwax_power_ctrl.c:405-423` 读取 `esp_reset_reason()`，映射 POWERON/EXT/SW/PANIC/WDT/BROWNOUT 等并记录 RTC boot count | 有复位类别和跨软复位 boot count；没有致命站点、任务、会话/epoch、重启意图或 crash-loop 决策 |
| RTC 保留 | `s_boot_magic/s_boot_count` 使用 `RTC_NOINIT_ATTR`；bootloader RTC STORE0 只保存 intentional power-off marker | 可区分真实断电后的新计数；没有通用 fatal breadcrumb、CRC/schema 或已消费标记 |
| Panic/backtrace | `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y`，reboot delay 为 0 秒，UART0 115200 和 secondary USB Serial/JTAG 开启 | 串口在线时可打印 panic/backtrace，但立即重启；离线设备没有留存证据 |
| GDB | `CONFIG_ESP_GDBSTUB_ENABLED=y`，但 `CONFIG_ESP_SYSTEM_PANIC_GDBSTUB` 和 `CONFIG_ESP_DEBUG_STUBS_ENABLE` 均未启用 | 组件被编入不等于 panic 会停在 GDB stub；当前产品 panic 路径仍是 print-and-reboot |
| Core dump | 所有 `sdkconfig*` 均无 `ESP_COREDUMP` 配置；活动分区表没有 coredump 分区；第一方源码没有 `esp_core_dump*` 调用 | 实际没有 flash/UART core-dump 交付链，现场只能依赖瞬时控制台输出 |
| Crash-loop | RTC boot count只打印，没有阈值、时间窗、稳定运行确认、safe mode 或回退 owner | `ESP_ERROR_CHECK` 持续失败可无限重启；`LIFE-010/NVS-001` 是已知触发根因，缺恢复策略归 `OBS-001` |
| Task WDT | task WDT 10 s 启用，panic 关闭；仅配置两个 idle task 自动检查；第一方无 `esp_task_wdt_add/reset` | 可检测部分 CPU 饥饿但不会统一复位；等待锁/I/O 的业务 worker通常不会饿死 idle，也没有业务 heartbeat，沿用 `RTOS-001` |
| Interrupt WDT | interrupt WDT 300 ms 开启并检查 CPU1 | 对中断禁用/ISR 卡死有基础保护，不替代任务健康或外设超时监测 |
| 业务进度监测 | WS ping/pong和 RX idle deadline；conversation ASR progress watchdog；OTA boot-health gate | 这些监测特定协议阶段，不是全部 worker 的生命周期 heartbeat |
| Epoch/序号 | WS connection epoch、audio session ID/sequence、speaker generation、conversation metrics、OTA update ID/progress 均有日志/状态 | 对在线会话定位较强，是正向控制；未统一绑定到 fatal record和启动 crash evidence |
| Heap/stack | `diagnostics.c` 提供 heap checkpoint和当前任务 high-water；battery/LED/OTA等若干 worker记录 stack水位 | 覆盖是 checkpoint/局部任务，不是所有关键 worker 的周期性健康快照；长稳水位仍待阶段 8/10 |
| 固件 identity | 启动日志含 version/project/IDF/build date/time；hello在 OTA 启用时可带 image SHA-256 | panic前的本地启动日志没有唯一 image hash/partition tuple；发布身份缺口沿用 `VER-001` |
| 日志等级/出口 | bootloader和应用默认/最大 INFO；UART0 + secondary USB Serial/JTAG；第一方无全局 sink、动态等级或速率预算 | 150 个正式源中有 1,502 个 `ESP_LOGE/W/I/D/V` 调用站点；没有持久化 ring、丢弃计数或每任务带宽上限 |
| 敏感字段 | hello JSON和 caller secret在发送后显式清零；检索未发现 password/device secret/token 明文日志 | 密钥清理是正向控制，但默认 INFO 会输出最长96字节的 ASR partial/final、用户文本和助手回复预览，还会输出SSID/IP/device ID/cloud URL/request ID，见 `PRIV-001` |
| 留存预算 | 活动分区表只有 NVS/model/OTA/storage，无 coredump/log 分区；未发现日志文件轮转 | 没有容量、保留期、删除、导出授权或覆盖顺序；完整隐私/录音留存由 B0-14继续审计 |

日志站点按等级复算为：ERROR 483、WARN 410、INFO 564、DEBUG 37、VERBOSE 0，共 1,494；66 个正式文件至少含一个日志站点。数量本身不是缺陷，风险来自实时循环中没有限速的路径和缺少统一带宽/丢弃策略。

## 6. 新确认问题

### OBS-001（P1）：复位现场、致命站点和 crash-loop 恢复证据缺失

- 触发：任一 hard check panic、三个未提交 uplink abort、audio sender/rt capture 安全重启，或持续 OTA/NVS 启动失败。
- 当前结果：重启后只能看到 `PANIC` 或 `SW`、RTC boot count和普通启动 identity；`SW` 不能区分 OTA、BLE重配网、audio sender卡死和capture卡死。没有 core dump、fatal-site ID、task/epoch/session snapshot、统一 reset-intent，也没有 crash-loop breaker。
- 产品影响：现场设备可能自动恢复但无法解释原因；若根因持续存在则反复重启，串口未在线时证据完全丢失。OTA/音频恢复问题无法可靠区分，缺陷关闭和远程运维均不成立。
- 修复方向：
  1. 定义版本化 `nuwax_reset_record_t`：magic/schema/CRC、reason/site、intentional标志、task、WS epoch、audio session hash、firmware image identity、单调计数。
  2. 已知安全重启/abort 前先写 RTC no-init breadcrumb；下次 NVS 可用后由单一 diagnostics owner限频落入小型持久化 ring并清除“未消费”标记。不要在任意 panic/cache-disabled 上下文直接写 NVS。
  3. 为 IDF panic评估专用加密/受控 core-dump 分区和导出流程；core dump可能包含音频、密码或 device secret，必须与 B0-14/B0-18 的访问、加密、保留和删除策略一起批准。
  4. 给 intentional OTA/reprovision 与 safety restart使用不同 site/intent；建立有限时间窗 crash-loop阈值和 safe/recovery mode，禁止无限重启同一失败站点。
- 关闭证据：B级验证 record编码/CRC/boot消费和隐私过滤；C级证明 coredump/record分区、镜像identity和配置一致；D级逐一注入10个硬终止/重启站点，断开串口后仍能唯一还原站点，连续失败按策略进入恢复且不泄露 secret。

### LOG-001（P1）：WS RX与音频实时路径没有统一日志速率预算

- WS RX同步链：`ws_transport.c:1293-1302`在 RX task直接调用 `on_text`；`nuwax_ai_partner_main.c:3970-4014`同步分类/解析；`session_protocol.c:820-821,1084-1095`可为每个云端 ASR partial至少输出 type、ASR metadata和文本预览，频率受远端消息控制且没有节流。
- MIC热路径：`nuwax_ai_partner_main.c:9430-9471`中 partial timeout在`:9461`记录，随后 short read又在`:9469`记录；当前20 ms frame下，持续partial short read每帧最多两条WARN，即约100条/秒。
- 其他放大点：realtime uplink的1/2/4帧batch入队失败在`audio_uplink.c:658-681,705-726`逐batch WARN；live AEC每8个health chunk可在`live_aec.c:1615-1655`输出长WARN；speaker stale/superseded/not-active packet在`speaker_playback.c:3541-3575`逐包INFO。
- 正向控制：零样本MIC timeout采用“首条+每5次”；mic level、AEC常规统计和部分queue drop已有局部采样。问题不是所有日志都无节流，而是没有覆盖所有实时/RX路径的统一预算、聚合和丢弃计数。
- 产品影响：UART0 115200同时承担panic和普通日志；云端高频partial或硬件故障风暴可使同步格式化/输出占用RX/audio owner时间，放大队列背压、音频补零和故障证据覆盖。
- 修复方向：RX/capture/speaker实时任务只更新有界telemetry计数或投递给bounded低优先级worker；事件首条立即记录，后续按1秒、指数退避或power-of-two聚合delta/total/max；为每tag/等级定义产品预算、丢弃计数和fatal快照优先级。
- 关闭证据：B级同时模拟高频ASR partial、20 ms partial/short和queue pressure，证明日志上限与计数不丢；D级在I2S故障+云端partial+TTS压力下量化日志B/s、RX/capture p95/p99、queue水位、补零帧和序号缺口，并证明fatal记录不被洪泛覆盖。

### PRIV-001（P1）：默认INFO日志输出用户会话正文和稳定标识

- 会话正文：`session_protocol.c:136-148`的 `log_text_preview()`在INFO输出最长96字节原文；`:1058-1095`对每个ASR partial/final输出文本预览，`:1135-1169`再次输出ASR result，`:1173-1223`同时输出`user_text`和`assistant_text`。`:1006-1040`还可输出device command自由文本和status lines。
- 稳定标识：`wifi_manager.c:110,237,413-472,485-532`输出SSID/IP；`nuwax_device_uid.c:84-95`输出完整32字符UID；`nuwax_cloud_config.c:209-266`输出device ID和完整cloud URL；`nuwax_ai_partner_main.c:4581-4588,5190`再次输出SSID/IP。URL validator在`nuwax_cloud_config.c:38-97`允许query，因此未来携带query token的URL也会被原样记录。
- 当前暴露面：默认和最大日志等级均为INFO，UART0 115200和secondary USB Serial/JTAG均开启；这些不是隔离diagnostic profile才启用的日志。
- 正向控制：正式源码没有发现Wi-Fi password、device secret、token或Authorization明文日志；`session_protocol.c:194-210,358-364`和`nuwax_ai_partner_main.c:16163-16199`主动清零hello/caller secret；配网日志只输出password长度，不输出密码本身。
- 产品影响：任何能读取默认串口/USB日志的人都可得到用户说话内容、助手回复、设备和网络身份；这与`NVS-002`的flash明文凭据是不同根因，属于发布前必须关闭的隐私泄露。
- 修复方向：production profile禁止正文预览，只记录长度/消息类型/不可逆关联ID；SSID/UID/session/request ID使用带设备盐的短hash或批准尾号；URL只保留scheme+host+port并剥离path/query。建立集中redaction API、开发diagnostic override、访问审计、保留/删除和core-dump敏感内存策略。
- 关闭证据：B0-14形成完整数据流/同意/访问/保留/删除矩阵；静态secret/PII scan和golden log证明无正文/明文secret，D级production profile的UART/USB/远程导出只出现批准字段。

## 7. 已确认的正向控制

1. 三个未提交 uplink路径在 sender无法确认退出时不会 unwind并释放栈对象，也不会伪造 END；fail-closed避免了更严重的 UAF/协议错误。
2. audio sender和rt capture停止失败时不会强删仍运行的worker或释放live资源，而是整机安全重启。
3. OTA staged reboot和BLE重配网重启前已有各自的持久化状态，优于无准备重启；但仍需统一 reset record。
4. reset reason、brownout和跨软复位boot count已有基础日志；interrupt WDT、WS heartbeat/idle timeout、ASR progress watchdog和OTA boot-health gate覆盖了部分故障域。
5. WebSocket epoch、audio session/sequence、speaker generation、conversation metrics和OTA update ID提供了较强在线链路证据。
6. hello/device secret和临时JSON有显式清零，当前检索未发现密码或device secret明文日志；这项正向控制不能抵消会话正文默认输出的`PRIV-001`。

## 8. B/C/D 关闭矩阵

| ID/交叉引用 | B级主机/协议 | C级构建/镜像 | D级设备/故障注入 |
|---|---|---|---|
| `OBS-001` | reset record编码、CRC、intent/site、消费幂等、crash-loop状态机、隐私过滤 | coredump/diagnostic分区和配置；identity绑定；release profile | 10个终止/重启点、断串口、连续复位、掉电中断、safe-mode/回退 |
| `LOG-001` | 高频ASR partial、20 ms partial/short、queue pressure；每tag预算、聚合/丢弃计数 | release日志等级/最大等级和console profile固定 | I2S故障+云端partial+Wi-Fi/TTS压力下的日志B/s、p95/p99、丢帧/序号和fatal证据保留 |
| `PRIV-001` | secret/PII scan、golden log、redaction稳定性 | production/diagnostic profile差异和调试入口门禁 | UART/USB/远程导出实测，不出现未批准字段 |
| `RTOS-001` | worker heartbeat模型、超时/恢复状态机 | task WDT panic/subscription profile | 锁死、I/O永久阻塞、CPU饥饿分别注入，证明告警/恢复/重启策略 |
| `LIFE-010` | OTA init逐点失败与重复init资源守恒 | OTA on/off profile均构建 | pending-verify/interlock/task-create失败不启动循环且可恢复 |
| `NVS-001` | schema迁移和受控恢复模型 | 加密/分区/安全floor配置 | 页耗尽、新版本、损坏、掉电恢复，不静默全擦安全状态 |
| `VER-001` | 单一release identity生成和协议绑定 | source/config/partition/model/app/manifest hash一致 | 启动、hello、OTA、reset record报告同一identity |

## 9. 计划 1.18/1.20 反向校验

| 计划要求 | A1-09证据 | 状态 |
|---|---|---|
| `ESP_ERROR_CHECK/assert/abort/restart` 独立清单 | 第2~4节；151文件分母、3/32/3/4/0/0精确计数 | A级完成 |
| 区分启动不可恢复、运行安全终止和预期产品重启 | 第3节逐站点分类 | A级完成；动态触发待B/D |
| reset reason、panic/backtrace/core dump | 第5节配置/源码/分区三向核对 | A级完成；core dump能力开放 |
| task heartbeat/WDT | 第5节；确认只检查idle、panic关闭、无业务订阅 | A级完成；沿用`RTOS-001` |
| epoch/序号、heap/stack | 第5、7节正向与缺口矩阵 | A级完成；长稳水位待D |
| 日志等级、速率和洪泛 | 第5节1,494站点统计；`LOG-001`落实WS RX、20 ms capture、uplink/AEC/speaker路径 | A级完成；带宽/时延待D |
| 敏感字段脱敏 | 第5、6节；secret正向控制与identifier缺口分离 | A级完成；完整策略由B0-14关闭 |
| 持久化空间预算/保留 | 活动分区无coredump/log区，缺口明确 | A级完成；设计/实机待B/C/D |

## 10. A1-09结论

A1-09的 A 级静态交付已完成：正式源码分母、所有致命入口、继续运行检查、reset/core-dump/heartbeat/日志/隐私矩阵和关闭条件均已建立。该结论只允许把阶段 1 标记为“**A 级静态审计完成，B/C/D验证开放**”，不能写成设备稳定性或量产发布通过。新增 `OBS-001/LOG-001/PRIV-001` 保持开放；`RTOS-001/LIFE-010/NVS-001/VER-001`继续沿原问题关闭，不重复计数。
