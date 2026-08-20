# 阶段 2 A2-03/A2-08：异常复位、NVS/OTA恢复与可观测性矩阵

审计日期：2026-08-03  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
权限：A 级静态评审；未修改业务源码、配置或资产，未构建，未运行 host/gateway 测试，未访问 COM5，未写 NVS，未执行 OTA 或烧录。

## 1. 验收边界与证据

A2-03 要回答的是：brownout、WDT、panic、`abort()`、软件重启或写入中断后，设备是否能恢复到旧固件或明确错误态，并且不会静默破坏 NVS、OTA 身份或业务配置。

本轮直接审计：

- `main/nuwax_ai_partner_main.c` 的 NVS 初始化、OTA 初始化和 boot-health 标记；
- `main/board/nuwax_power_ctrl.c` 的 reset reason、RTC boot count 和 intentional-off marker；
- `main/nuwax_ota_new/nuwax_ota.c`、`nuwax_ota_main.c`、`nuwax_ota_device.c`、`nuwax_ota_transport.c` 的下载、写入、激活、pending-verify、确认、回滚和上报恢复；
- `main/nuwax_provision_new/nuwax_cloud_config.c`、`nuwax_provision_main.c` 的云配置与 BLE 重配网持久事务；
- `sdkconfig`、`partitions.customwake.csv`，以及实际构建指向的 ESP-IDF 6.0 bootloader/NVS 实现。

关键输入 hash：

| 输入 | SHA-256 / identity |
|---|---|
| `nuwax_ai_partner_main.c` | `4519cc063d020dafb65ca6ff61795933cb6122f32c5f4c575491b101cf4706fd` |
| `nuwax_power_ctrl.c` | `7d54c54cc7e6385cea8e816f40a6ef2e4cfa0a8f6a099ee3cb42b43286793824` |
| `nuwax_ota.c` | `cf0bd7140de77db04c27d184f4d05a10212127945039eaeea3a57923cf94ad41` |
| `nuwax_ota_main.c` | `0adc0ec3c8cd7b6b1e01401257b6bd9278a03e975eeab2e39cc96cb71a2b2174` |
| `nuwax_ota_device.c` | `e2efaad602533559d90fc551b693b82072bed65182005fd049746bcab6cfc5e8` |
| `nuwax_ota_transport.c` | `858544eba1cffb65c20bb5ed5bc56c8b143678167a1e0088e3d3708d06357870` |
| `sdkconfig` | `ea74b3ffc7ce84c2a02af8cf3ef022267bffd12eb6489721d77d2f32ab97e07d` |
| source manifest | 7,918 项；`3074e4582fdc8bc53dec2f74cdfaa67d801544b24d69cf2d2d3fc3d5e22db60b` |
| 活动分区表 | `e498063228a1a46792d3243a8ec29c2c5e40b7556e9885f16dc6e5729ee05a19` |
| ESP-IDF | `v6.0` commit `662a3be354759d9487bf4b1a629fadb766cb1800`，工作区 clean |

7.0.24 续接源码包含实时音频日志门、OTA release policy 和监控 pause/resume/VAD 后续修改；上述恢复
owner、NVS/otadata 检查点和 20 ms 音频帧没有改变。由于 `sdkconfig` 和主入口
行号发生漂移，本文件已重绑当前 hash；A2-03 的 A 级恢复结论不自动升级为 B/C/D
证据。

## 2. 复位原因并不选择恢复策略

`nuwax_power_ctrl_init()` 在 `nuwax_power_ctrl.c:405-423` 读取并打印 `POWERON/EXT/SW/PANIC/INT_WDT/TASK_WDT/WDT/DEEPSLEEP/BROWNOUT`，RTC no-init 区只保留 magic 和 boot count。除 brownout 多打一条供电检查日志外，所有原因随后进入同一个 `app_main()` 启动路径。

| 触发 | 当前配置/入口 | 复位后的应用动作 | 静态判定 |
|---|---|---|---|
| Brownout | `CONFIG_ESP_BROWNOUT_DET=y`、level 7；flash brownout reset 开启 | 记录 `BROWNOUT` 后普通启动 | 能重新启动，但没有 brownout 次数窗、供电 safe mode 或对在途 NVS/OTA 的原因级处理 |
| Panic / `abort()` | panic print-and-reboot，delay 0；meeting/monitoring/P5 各有一个 fail-closed `abort()` | 只得到 `PANIC` 类别 | 没有 fatal site、任务、session、image identity 或持久 breadcrumb；沿用 `OBS-001` |
| Interrupt WDT | 300 ms，检查 CPU1 | bootloader 重启后普通启动 | 可形成复位，但没有 WDT 专用恢复或 crash-loop breaker |
| Task WDT | 10 s，仅自动检查两个 idle task，`CONFIG_ESP_TASK_WDT_PANIC` 未启用 | 超时不构成可靠的整机复位入口 | 业务任务锁死/I/O 阻塞不能依赖本矩阵恢复；沿用 `RTOS-001` |
| Safety `esp_restart()` | audio sender、realtime capture stop 失败 | `ESP_RST_SW` 后普通启动 | 与 OTA/BLE 的预期软件重启不可区分；沿用 `OBS-001` |
| OTA staged reboot | `nuwax_ota_ack_and_reboot()` | bootloader 选择新槽并进入 pending-verify | 有独立 OTA durable state，但 reset reason 仍只有 `SW` |
| 真断电/外部复位 | rail 断电或 EXT reset | 普通启动 | NVS/otadata 恢复依赖底层存储与事务记录；无应用级 reset budget |

因此“可以识别 reset reason”不等于“按 reset reason 恢复”。确定性启动缺陷在同一站点持续发生时，当前代码会继续重启；RTC boot count 只打印，不触发旧固件、recovery mode 或功能降级。

## 3. 持久化域与原子性边界

| 持久化域 | 布局/owner | 掉电保护边界 | 应用级缺口 |
|---|---|---|---|
| NVS | `0x9000`、24 KiB；Wi-Fi、云配置、OTA 元数据、配网 marker、音量、唤醒词等共享 | ESP-IDF NVS 6.0 要求 `nvs_commit()`；底层含页/entry 恢复测试 | 不提供跨 namespace 的全局事务；项目没有统一 schema、generation、容量和 recovery policy |
| otadata | `0xF000`、8 KiB；ESP-IDF bootloader/app-update owner | 两个 sector、CRC 和 OTA state 控制槽选择 | 项目无 factory 槽；未在接受 OTA 前显式断言运行槽为 `VALID` 且存在可靠回退 |
| `ota_0/ota_1` | 各 `0x4C0000` | 新镜像写入 next slot；切换前保留运行槽 | 两槽都无有效镜像时只能有线恢复，沿用 `PART-003` |
| RTC no-init / STORE0 | boot count 和 intentional-off marker | 部分 reset/deep sleep 可保留 | 没有 CRC/schema/consume bit；不是 NVS/OTA 事务日志 |

`app_main()` 在 `nuwax_ai_partner_main.c:15585-15590` 对 `ESP_ERR_NVS_NO_FREE_PAGES` 或 `ESP_ERR_NVS_NEW_VERSION_FOUND` 直接调用 `nvs_flash_erase()`，然后重新初始化。该动作不是“恢复损坏 key”，而是擦除整个共享 NVS 分区：Wi-Fi、云 URL/secret、OTA accepted floor/identity、pending transaction、report context、配网 rollback、音量和唤醒词同时消失。擦除或再次 init 失败由硬 `ESP_ERROR_CHECK` panic；擦除成功则静默以空 NVS 继续。这仍是 `NVS-001`，也是 A2-03 无法通过产品验收的首要原因。

## 4. OTA 掉电检查点

OTA 的关键顺序为：持久化 report context -> 持久化 v3 pending transaction -> 下载/写 next slot -> SHA/descriptor 校验 -> `esp_ota_set_boot_partition()` -> staged ACK -> 重启 -> boot health + 云正向活动 -> staged result ACK -> 标记 image valid -> 提交 accepted sequence/identity -> committed result -> 清 report context。

| 掉电/复位点 | 当时 durable state | 下次启动与最终状态 | 静态结论 |
|---|---|---|---|
| 接受命令前 | 当前槽、accepted identity | 启动当前槽 | 无 OTA 状态变化 |
| report context 已 commit，pending txn 尚未 commit | 当前 boot 槽不变；可能保留上报上下文 | 当前槽启动；无 pending image，云恢复路径报告 `interrupted` 后清 context | 状态可收敛；多 key context 的逐断点原子性仍需 `NVS-005` 动态证明 |
| pending txn 已 commit，`esp_ota_begin()` 前 | v3 record 含 sequence/address/size/version/SHA；boot 槽不变 | `reconcile_pending_txn(false)` 发现 target 不是 running，清 stale pending；上报 interrupted | 保留旧固件 |
| `esp_ota_begin/write` 中 | next slot 部分写；boot 槽仍为旧槽 | 旧槽启动；pending record 清理；下一次 OTA 从 byte 0 擦写 | 保留旧固件；需 D 级逐 sector 断电证明 flash/NVS/otadata 无关联损坏 |
| `esp_ota_end()` 后、set boot 前 | next slot 镜像有效但未激活 | 旧槽启动并清 stale pending | 保留旧固件 |
| `esp_ota_set_boot_partition()` 后、DONE/主动重启前 | otadata 指向新槽；pending txn/context 已存在 | 任意复位都会进入新槽首次启动 | 顺序正确：激活发生在完整 SHA、project/version 和 image validation 之后 |
| 新槽第一次 boot | bootloader 把 `NEW` 改为 `PENDING_VERIFY` | app 启动 OTA controller，锁住 SW3/音频，等待 health decision | rollback 已启用且有编译期 `#error` 防止误关 |
| pending image 在 confirm 前 panic/WDT/brownout/断电 | otadata 仍为 `PENDING_VERIFY` | ESP-IDF 6.0 bootloader 下次启动在 `bootloader_utility.c:395-450` 将其标为 `ABORTED` 并选前一有效槽 | 条件性自动回旧固件；条件是确有另一有效槽 |
| 本地 health 失败 | pending image + context | `esp_ota_mark_app_invalid_rollback_and_reboot()` | 显式回旧固件；rollback API 失败时每 5 s 重试，但无 factory fallback |
| 120 s 内无云正向活动/health 未完成 | pending image + context | controller timeout 主动 rollback | 云端不可用不会把候选固件误确认；代价是离线健康候选也回滚 |
| staged result 未获 ACK | pending image + context | 不确认，要求 WS 重连；最终 timeout rollback | fail-closed，正式云必须实现 result ACK 契约 |
| 标记 valid 之前掉电 | pending image | 下次 bootloader 回旧槽 | accepted sequence 不会前移 |
| `mark valid` 成功、accepted sequence commit 前掉电 | 新槽已 `VALID`；pending v3 record 保留 | `reconcile_pending_txn(false)` 校验 running partition/version/SHA 后补提交 sequence/identity | 能恢复到新固件；此时已经不能 rollback，NVS 永久失败会阻塞后续 OTA |
| accepted sequence 已 commit、accepted identity/pending cleanup 前掉电 | 新槽 valid；pending record 仍是恢复源 | 补写 accepted identity，再清 pending | 能幂等收敛 |
| pending 已清、report context 未清 | 新槽 valid + accepted identity | 云恢复路径报告 `committed`，再清 context | 能重放终态；清理失败可能在未来 boot 重复上报 |

### 4.1 OTA 的正向控制

1. `nuwax_ota_main_start()` 在下载前保存单 blob v3 transaction，字段含 magic/format/size/release sequence/target/image size/version/SHA。
2. `nuwax_ota_transport_run()` 只有在 Content-Length、实际长度、SHA-256、project、version 和 `esp_ota_end()` 全部通过后才设置 boot partition。
3. pending image 的事务必须匹配 running partition、固件 version 和实际分区 SHA；legacy v1/v2 记录不能直接确认。
4. 本地 speaker/MIC/AEC/display/touch/LVGL/runtime task/battery health 与云正向活动共同构成确认门；`ESP_ERR_NOT_FINISHED` 可在 deadline 内重试。
5. image valid 与 accepted sequence 分两阶段提交时，代码故意先标 valid、保留 pending record，再用启动 reconcile 补齐 NVS floor。

### 4.2 OTA 恢复仍未闭合的边界

- `nuwax_ota_main_start()` 只拒绝 running image 为 `NEW/PENDING_VERIFY`，没有要求当前运行槽状态明确为 `ESP_OTA_IMG_VALID`；OTA controller也没有在激活前证明 rollback target 可用。当前正常全量烧录后的槽预期为 valid，但异常 otadata、手工混烧或历史状态不能靠这个假设放行。该证据扩充既有 `PART-003`，不另建重复问题。
- 分区表没有 factory recovery。若旧槽损坏/invalid，显式 rollback 返回 `ESP_ERR_OTA_ROLLBACK_FAILED`；controller只能重试，无法进入独立 recovery image。
- bootloader 的双 sector/CRC 和 NVS 的底层掉电恢复是基础保护，不等于本产品已完成逐 flash operation 的 brownout 证明；B/C/D 仍开放。
- OTA init 在 `nuwax_ai_partner_main.c:15608` 使用硬 `ESP_ERROR_CHECK`。pending metadata 不可读、accepted floor/identity损坏、queue/task创建失败等会 panic；部分 init 回滚和普通有效镜像的持续失败仍沿用 `LIFE-010/OBS-001`。

## 5. NVS 业务恢复矩阵

| 对象/中断点 | 启动恢复动作 | 最坏结果 | 关联问题 |
|---|---|---|---|
| 单 key 音量、唤醒词、UID cache | 读不到时使用默认或由硬件 UID 重建 | 设置丢失；UID cache可重建 | `NVS-003`；寿命归 `NVS-004` |
| 云 `pending` 写入中断 | active 不被主动替换；invalid pending 会单 key 清理 | 保留旧 active 或等待重新 stage | 局部正向控制 |
| 云 pending -> active commit 中断 | 启动分别校验 active/pending | NVS 底层结构可恢复，但 old/new 两 key的业务原子性未做故障注入 | `NVS-005` |
| BLE reprovision 多 namespace 提交中断 | rollback record、Wi-Fi commit/verified marker和 cloud pending用于重建或回滚 | 跨 ESP Wi-Fi NVS、`nuwax_wifi`、`nuwax_reprov`、`nuwax_cloud` 仍可能形成未验证组合 | `NVS-005` |
| OTA v3 pending txn 单 blob中断/损坏 | 不可读时 fail-closed；pending candidate不会在无身份匹配时确认 | pending image回滚；普通 valid image可因 OTA init硬失败进入 crash-loop | `LIFE-010;OBS-001` |
| OTA report context 多 key commit中断 | 只有全部 key可读才装载；pending image缺context时不会确认，最终 timeout rollback | 可用性下降；request/update/identity跨代混合没有 generation/CRC 证明 | `NVS-003;NVS-005` |
| accepted sequence commit中断 | pending record仍在时 reconcile补写 | NVS永久失败后新 image已valid，继续运行但后续OTA fail-closed | `NVS-001;NVS-003` |
| `NO_FREE_PAGES/NEW_VERSION_FOUND` | 全分区 erase 后从空 NVS启动 | 凭据、云密钥、OTA floor/identity和所有恢复记录同时消失 | `NVS-001` |
| 其他 NVS init/erase失败 | hard check panic并立即重启 | 确定性 crash-loop，无 safe mode | `OBS-001` |

ESP-IDF 6.0 NVS 的接口契约确认“只有 `nvs_commit()` 后才保证写入非易失存储”，其底层也有随机 power-off 恢复测试；但这些只能证明存储结构可再次初始化，不能证明本项目跨 key/namespace 的 old/new generation 一致。因此本轮不把静态调用顺序写成掉电测试通过。

## 6. `reset reason -> 状态 -> 动作 -> 结果` 总矩阵

| Reset reason/触发 | 复位时状态 | 启动动作 | 最终状态 | 结论/缺口 |
|---|---|---|---|---|
| POWERON/EXT/BROWNOUT | valid image，非持久写入中 | 同一 app 普通启动 | 原固件运行 | 条件性成立；brownout重复无 safe mode |
| PANIC/abort/INT_WDT/SW safety restart | valid image，确定性缺陷仍在 | 同一 app 普通启动 | 可能无限重复同一故障 | 不满足；`OBS-001/RTOS-001` |
| 任意 reset | NVS commit中 | `nvs_flash_init()` + 各模块局部 reconcile | 单 key通常可读；跨 key结果未证明 | `NVS-005` D级开放 |
| 任意 reset | OTA下载/写入，boot partition未切换 | 旧槽启动，清 pending，上报 interrupted | 旧固件 | A级静态恢复链成立 |
| SW/POWERON/BROWNOUT/PANIC/WDT | boot partition已切换，新槽未确认 | 首次进入 pending；再次复位自动 abort/rollback | 前一有效槽 | 条件是旧槽 valid；无 factory，`PART-003` |
| 云不可用/本地health失败 | pending image | 120 s timeout或立即显式rollback | 前一有效槽 | fail-closed；回滚失败无第三槽 |
| 任意 reset | image刚标valid，NVS floor未提交 | 新槽启动，pending record补提交 | 新固件或OTA被阻塞 | 不会降回旧槽；需要明确 degraded UI/运维状态 |
| 任意 reset | NVS init触发全擦 | 擦空后继续或hard panic | 失去联网/身份/OTA恢复上下文 | 不满足；`NVS-001` |
| 任意 reset | 两个OTA槽均不可用/otadata无有效回退 | bootloader无 factory可选 | 只能有线恢复或行为依赖bootloader扫描 | 不满足；`PART-003` |

## 7. Canonical 问题映射

本轮没有为同一根因重复创建新 ID；A2-03 将以下既有问题落实为恢复矩阵：

| ID | A2-03 新增证据 |
|---|---|
| `NVS-001` P1 | NVS全擦会同时破坏业务配置、OTA anti-rollback floor与恢复上下文；pending image虽会回滚，但设备仍失去联网配置 |
| `NVS-003` P2 | 无全局schema/generation/recovery状态；OTA report context和多个简单设置无法统一判定代次 |
| `NVS-005` P2 | BLE多namespace事务及OTA context/accepted metadata必须逐commit做掉电矩阵 |
| `OBS-001` P1 | reset reason只分类，不含fatal site/intent/crash-loop breaker/safe mode |
| `RTOS-001` P1 | Task WDT panic关闭且无业务task订阅，业务卡死不能依赖复位恢复 |
| `LIFE-010` P1 | OTA init任一步失败由app硬panic；部分初始化和持久错误缺受控recovery owner |
| `PART-003` P2 | 无factory槽，且OTA admission未显式要求当前槽为VALID/证明回滚目标可用 |

## 8. 关闭验证矩阵

| 等级 | 用例 | 必须断言 |
|---|---|---|
| B | 用 mock NVS/otadata 覆盖 OTA 14 个检查点，每次在写前/写后重启状态机 | 结果只能是旧固件、新固件已提交或明确fail-closed；sequence不回退、不跨代确认 |
| B | OTA context、cloud active/pending、reprovision四namespace逐set/commit故障 | 不产生可被误认为完整事务的mixed generation；重放结果幂等 |
| B | reset record/crash-loop policy模型 | 同一fatal site在有限时间窗达到阈值后进入safe/recovery，而非无限重启 |
| C | 用冻结 ESP-IDF 6.0、product config和活动分区表干净构建 | rollback/brownout/WDT/panic配置与bootloader/app一致；镜像和分区hash绑定 |
| C | 检查运行槽状态 admission 与 recovery image策略 | OTA只在明确有效回退存在时接受，或交付独立factory/recovery槽 |
| D | OTA下载、每个NVS commit、`esp_ota_end`、set boot、mark valid、accepted commit处切电/复位 | 每点至少多次重复，验证槽、otadata、NVS、云终态和UI状态 |
| D | 注入brownout、INT WDT、task死锁、panic、三个abort和两个safety restart | reset site可唯一还原；可恢复故障有限恢复，持续故障进入safe mode |
| D | 损坏 pending blob、accepted seq/txn、report context、单页/整页NVS和一份/两份otadata | 不静默全擦安全floor；无有效回退时进入明确有线recovery指示 |
| D | 云离线超过120 s、health失败、result ACK丢失、rollback API失败 | candidate不被误确认；旧槽可用时必回退，无旧槽时错误态可诊断 |

## 9. A2-03 判定

- A 级静态审计：**完成**。
- OTA 下载/写入中断：**静态上保留旧 boot 槽，恢复链成立**。
- pending image panic/brownout/WDT：**在存在另一有效槽时由 ESP-IDF rollback 恢复**。
- image valid 后 NVS sequence/identity提交中断：**pending v3 record提供补提交路径**。
- NVS全擦、确定性crash-loop、Task WDT覆盖、无factory/回滚目标证明：**未满足产品验收**。
- A2-03总体状态：`completed-A;B/C/D-open`，不是设备异常复位、NVS抗掉电或OTA量产恢复通过。

## 10. A2-08 启动期 fatal、core dump 与持久化预算续接

### 10.1 范围与当前静态分母

A2-08复用A1-09的致命入口清单，但按本文件当前source manifest重新计算，而不是沿用旧行号或旧日志数量。分母为`main/CMakeLists.txt`中150个唯一正式C源，加产品bootloader电源保持源1个，共151个文件；host check、host stub、归档和未链接原型不进入固件致命入口分母。

当前正式路径精确计数：

| 调用 | 数量/文件 | 当前站点 | 解释 |
|---|---:|---|---|
| `ESP_ERROR_CHECK` | 3/1 | `nuwax_ai_partner_main.c:15587,15590,15608` | NVS erase、NVS init、OTA device init失败进入panic |
| `ESP_ERROR_CHECK_WITHOUT_ABORT` | 32/11 | 电源、audio、UI、网络、cleanup等 | 记录后继续，不是fatal，但降级是否安全另行审计 |
| `abort()` | 3/3 | meeting `:697`、monitoring `:914`、P5 `:919` | 未提交uplink sender仍引用栈对象时fail-closed |
| `esp_restart()` | 4/3 | BLE `main:5252`、rt capture `main:7365`、audio uplink `:1218`、OTA `:398` | 两条安全重启、两条预期产品重启 |
| `assert/configASSERT` | 0/0 | 正式产品源无命中 | host断言不计入固件 |

并行修改后的`audio_uplink.c`已重新核对：worker未确认停止时仍选择重启而不释放live task资源，安全边界没有反转。三个`abort()`也仍只发生在无法安全unwind的未提交uplink窄路径。本项不把这些保护动作本身重复登记为新bug；缺陷是重启前后无法可靠还原site、intent、task、session/epoch和image identity。

### 10.2 reset reason与fatal site不可区分

`nuwax_power_ctrl.c:37-38,405-423`只在`RTC_NOINIT_ATTR`保存magic和boot count，并打印`POWERON/EXT/SW/PANIC/INT_WDT/TASK_WDT/WDT/DEEPSLEEP/BROWNOUT`。RTC STORE0另由SW3 intentional-off marker独占。当前没有版本化fatal record、CRC、site、intent、启动stage、task、WS epoch、audio session、running partition或image SHA。

| 事件 | 重启后可见内容 | 无法区分的对象 | 判定 |
|---|---|---|---|
| NVS/OTA hard check | `PANIC`或普通panic打印 | NVS erase/init与OTA init具体cut、partial owner | 不满足验收 |
| meeting/monitoring/P5 abort | `PANIC` | 三个模式和具体session/epoch | 不满足验收 |
| audio sender/rt capture安全重启 | `SW` | 两个worker卡死与正常产品重启 | 不满足验收 |
| BLE重配网/OTA staged重启 | `SW` | 两种预期操作；只能依靠各自局部NVS上下文旁证 | 局部可恢复，但统一诊断不成立 |
| brownout/WDT | reset类别和boot count | 故障站点、连续次数窗、供电/任务现场 | 不满足验收 |

boot count在真实POWERON时清零，没有稳定运行确认、时间窗、同site计数、safe mode或已消费标记。持续NVS/OTA启动错误会反复进入同一路径；RTC count只增加并打印，不参与决策。`OBS-001`因此继续是P1发布阻塞。

### 10.3 panic、WDT与core dump能力

当前`sdkconfig`为panic print-and-reboot、0秒延时；UART0自定义GPIO43/44、921600 baud，同时启用secondary USB Serial/JTAG。`CONFIG_ESP_SYSTEM_PANIC_GDBSTUB`关闭。Interrupt WDT为300 ms并检查CPU1；Task WDT为10 s、检查两个idle task，但panic关闭，第一方正式源没有`esp_task_wdt_add/reset/delete`，因此锁/I/O阻塞的业务worker没有统一heartbeat或自动恢复契约。

全部11个`sdkconfig*`文件未出现`ESP_COREDUMP`配置；活动分区表没有coredump子类型，第一方源码也没有`esp_core_dump*`读写/导出入口。现状不是“core dump待验证”，而是当前产品配置没有持久core dump能力，串口未在线时panic现场只剩通用reset reason。

### 10.4 分区与持久化空间预算

活动16 MiB flash布局为：NVS 24 KiB、otadata 8 KiB、phy 4 KiB、model 4 MiB、两个OTA槽各4.75 MiB、`storage` 2.375 MiB。最后一个分区结束地址为`0x1000000`，未分配空间为0。未发现第一方正式代码挂载或使用`storage`分区，但这不自动授权覆盖它。

由此得到三个约束：

1. 新增独立coredump/log分区必须缩小、替换或重新解释现有分区，并更新partition table；当前app-only OTA不更新partition table，不能无线把已部署设备安全迁移到新布局。
2. 任何布局变更都需要完整有线刷写/受控迁移、旧数据处理和回退SOP；不能只通过Kconfig打开flash coredump。
3. core dump可能包含PCM、Wi-Fi/cloud secret、BLE材料、signed URL和会话正文。当前Flash/NVS Encryption关闭，量产若保留dump，必须先定义加密、鉴权、数量上限、TTL、导出审计和删除；否则production profile应明确禁用，而不是保持未决。

NVS只有24 KiB且承载Wi-Fi、cloud、OTA、配网事务、音量、唤醒词等。第一方没有`nvs_get_stats()`或`nvs_get_used_entry_count()`，也没有crash ring的entry/erase预算。不能在每次panic前直接写NVS：panic/cache-disabled上下文未必允许，NVS本身也可能是fatal根因，crash-loop还会放大磨损。`NVS-004`继续开放。

### 10.5 日志、隐私和故障证据保留

按当前150个正式C源复算，日志调用为ERROR 494、WARN 412、INFO 538、DEBUG 37、VERBOSE 0，共1,481处，66个文件有日志。默认/最大等级仍为INFO。这个数量只表示静态调用站点，不代表运行频率，但当前没有全局sink、持久ring、每tag带宽预算、丢弃计数或fatal日志优先级。

`session_protocol.c:136-148,1087-1095,1180-1188,1196-1241`仍会输出ASR、用户和助手正文预览；Wi-Fi、UID和cloud config仍输出SSID/IP/完整UID/device ID/cloud URL。当前未发现password/device secret/token常规明文日志，临时hello/secret有显式清零，这是正向控制；但不能据此满足“日志不泄密”，`PRIV-001`和`LOG-001`仍开放。core dump若未来开启，还必须纳入`PRIV-003`的RAM敏感数据清理与访问策略。

### 10.6 正向控制

1. reset reason覆盖常见硬件/软件类别，brownout有独立告警，RTC boot count可辅助识别连续软复位。
2. 三个abort避免释放仍被sender引用的栈对象，audio/rt capture安全重启避免强删live worker后形成UAF。
3. BLE重配网前保存rollback记录；OTA staged/pending/accepted context比通用reset reason更完整。
4. OTA pending candidate受本地speaker、mic、AEC、display、touch、LVGL、UI、runtime task、battery和云正向活动门控；未确认candidate会rollback而非误提交。
5. 启动和云循环有heap/minimum/largest checkpoint，部分worker记录stack high-water；这些是局部诊断，不是全任务heartbeat或fatal snapshot。

### 10.7 建议闭合架构

1. 增加固定大小、版本化的RTC breadcrumb：magic/schema/CRC、site、intent、stage、task ID、WS epoch、audio session短hash、partition/address、firmware identity、boot sequence和consumed位。安全重启和已知abort前先写breadcrumb；未知panic通过IDF hook能力另行评估，不能在非法上下文调用普通NVS。
2. NVS健康后由单一diagnostics owner把未消费breadcrumb限频落入有界持久ring；记录固定条数、固定写入上限和redaction后的reason code。NVS异常时只保留RTC并进入recovery，不得为写诊断再次触发NVS fatal。
3. crash-loop以同site、同image和有限boot/time window计数；达到阈值后进入明确定义的safe/recovery mode。OTA pending优先依赖bootloader rollback；普通valid image则禁用高风险子系统或进入有线/BLE恢复入口，不能无限重启。
4. core dump由产品安全负责人二选一：production明确禁用；或重新设计分区并提供加密、鉴权、TTL、次数上限、导出/删除审计。分区迁移只能走批准的全量升级流程。
5. 为关键worker建立有界heartbeat/进度registry，区分CPU starvation、锁/I/O停滞和正常长操作；Task WDT订阅/panic策略必须按任务验证，不能只依靠idle task。
6. release日志只输出结构化reason/site/计数和不可逆短关联ID；正文、SSID/IP/UID/URL path/query禁止进入production console和持久诊断。

### 10.8 C/D关闭矩阵

| 等级 | 用例 | 必须断言 |
|---|---|---|
| C | 当前product defaults干净构建并冻结最终sdkconfig/map/partition | panic/WDT/console/core dump策略与release profile一致；fatal site表链接完整 |
| C | reset record与分区设计检查 | breadcrumb尺寸/CRC/schema固定；若启用dump，分区不重叠且具加密/权限/TTL；app-only OTA明确拒绝布局不兼容 |
| C | NVS/diagnostic budget静态和镜像检查 | namespace/entry/最大写次数/保留条数/磨损预算闭合；安全floor不与日志共擦 |
| D | 3个hard check、3个abort、2个安全restart、BLE和OTA预期restart逐项注入 | 每次都唯一还原site/intent/stage/image；不依赖串口在线 |
| D | panic、INT WDT、Task WDT、brownout、外部复位和真断电 | reset类别正确；持续同site达到阈值后safe mode/rollback，无无限循环 |
| D | 断开UART/USB后触发崩溃并重启 | 允许的持久证据可读取、可鉴权、可删除，且不含正文或secret |
| D | NVS页耗尽/损坏/commit失败与crash record同时发生 | 不全擦anti-rollback/身份；诊断写失败不会掩盖原fatal或产生二次循环 |
| D | 1000次fatal/recovery与24/72小时soak | RTC/NVS计数收敛，flash写入在预算内，heap/task无累积，旧记录按策略覆盖/删除 |

### 10.9 A2-08反向校验与判定

| 计划要求 | 本节证据 | 状态 |
|---|---|---|
| fatal/assert/abort/restart记录原因和阶段 | 10.1、10.2完整站点和可区分矩阵 | A级完成；当前实现不满足 |
| panic/core dump/重启计数 | 10.2~10.4配置、RTC与分区三向核对 | A级完成；持久dump不存在，crash-loop决策不存在 |
| 存储不破坏NVS/OTA | 10.4 NVS/partition预算及A2-03恢复矩阵 | 静态风险明确；C/D开放 |
| 日志不泄密 | 10.5当前日志/console/字段核对 | 不满足；`PRIV-001/LOG-001`开放 |
| 异常可恢复 | 10.6正向控制与10.8故障矩阵 | 局部OTA/BLE成立；普通fatal未通过 |

A2-08的A级静态审计完成，但产品验收“每次异常可区分阶段/原因且日志不泄密、不破坏NVS/OTA”未通过。没有新增独立canonical ID；复用并增强`OBS-001`、`LOG-001`、`PRIV-001`、`RTOS-001`、`LIFE-010`、`NVS-001`、`NVS-004`、`NVS-005`、`PART-003`、`VER-001`。状态为`completed-A;C/D-open`。本轮没有构建、设备复位注入、COM5、NVS/eFuse、OTA或烧录证据。

A2-08收口期间检测到第二批并行漂移：`capture_phase_handoff_host_check.c`、`capture_phase_handoff.h`、`nuwax_ai_partner_main.c`和conversation host runner。已按最新主入口重新计算fatal分母和站点，3/32/3/4/0/0结论不变；新增capture handoff逻辑没有增加panic/abort/restart入口。四个文件及其跨阶段对话结论进入最终source manifest全量复核队列。
