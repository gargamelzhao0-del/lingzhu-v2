# NuwaxAIPartner-v2 全工程代码评审执行工作记录

版本：WIP V1.0

开始日期：2026-08-02

对应计划：`docs/2026-08-02-NuwaxAIPartner-v2全工程代码与产品稳定性评审计划.md` V1.8

执行边界：当前仅获 A 级授权，执行只读源码、配置、已有构建资产和文档审计；不修改业务代码，不运行会编译源码的 host test，不构建固件，不操作 COM5，不写 NVS，不 OTA/烧录。B/C/D 级动作必须分别获得用户批准并单独记录。

---

## 1. 记录规则

1. 每个工作项按 `待执行 / 进行中 / 已完成 / 阻塞 / 延期实机` 更新。
2. “已完成”必须同时具备：执行动作、命令或源码位置、证据、对应计划条款、复核结论。
3. 静态推演、host check、构建和实机证据严格分开；低等级证据不能写成实机通过。
4. 发现问题立即登记，禁止在工作记录中静默修复或把问题留到最终总结。
5. 源码或配置变化后，受影响结论自动失效，必须基于新 hash 重新执行。

## 2. 总体进度

| 阶段 | 内容 | 状态 | 当前证据/说明 |
|---|---|---|---|
| 计划复核 | 范围、顺序、门禁、可执行性 | 已完成（V1.8） | V1.8 新增 SBOM/license/CVE、隐私/录音留存、第一方 memory-safety/UB、生成资产 provenance、V1->V2 等价、需求/测试覆盖、安全威胁模型/密钥生命周期和全阶段唯一工作项索引 |
| 阶段 0 | 基线冻结与可追溯性 | A级静态审计完成；B/C/D开放 | B0-01~05、B0-07~20 A级静态项已完成；B0-06仅A级命令契约完成并等待C级干净构建 |
| 阶段 1 | 架构、任务和实时系统 | A级静态审计完成；B/C/D开放 | A1-01~A1-09均已完成A级静态审计；0个直接P0、34个去重P1，动态故障/构建/实机关闭证据未执行 |
| 阶段 2 | 启动、关机、电源和恢复 | A级静态审计收口；产品/B/C/D开放 | A2-01~A2-09 A级证据完成；八项产品验收均未通过；42个去重问题（28 P1/14 P2），最终manifest跨阶段复核强制保留 |
| 阶段 3 | 底层硬件驱动与时序 | 进行中（A3-01~A3-06 A级完成） | 引脚/电气约束与ES7210/ES8311/PA/I2S静态链已审计；硬件证据、owner/lifecycle、C/D测量及A3-07~16开放 |
| 阶段 4 | 音频、AEC、打断和模式 | 待执行 | - |
| 阶段 5 | Wi-Fi、WebSocket 和云端故障 | 待执行 | - |
| 阶段 6 | BLE、身份、安全和 OTA | 待执行 | - |
| 阶段 7 | UI、状态同步和 MCP | 待执行 | - |
| 阶段 8 | 内存、存储和资源预算 | 待执行 | - |
| 阶段 9 | 性能与实时性 | 待执行 | - |
| 阶段 10 | 故障注入与长稳 | 待执行 | - |

## 3. 阶段 0 检查表

| ID | 工作项 | 状态 | 对应计划 | 证据 |
|---|---|---|---|---|
| B0-01 | 记录工程路径、Git 根、分支、commit 和工作区状态 | 已完成 | 阶段 0.1 | 工程路径、Git 根、分支和 HEAD 已记录；v2 整目录未跟踪，见 BASE-001 |
| B0-02 | 生成第一方/第三方源码与 CMake 链接清单 | 已完成 | 阶段 0.2、0.8、2.4 | `docs/review-2026-08-02/module-inventory.md`；活动/原始/归档分离，150 个 CMake 条目均存在且无重复 |
| B0-03 | 记录 sdkconfig/Kconfig profile 与条件构建矩阵 | 已完成 | 阶段 0.3、0.7 | `docs/review-2026-08-02/config-diff.md`；配置分类/hash/构建矩阵与 3 项风险已登记 |
| B0-04 | 记录分区、bootloader、app/model/OTA 容量和 hash | 已完成（只读） | 阶段 0.4 | `docs/review-2026-08-02/partition-build-assets.md`；活动布局通过，发布镜像未冻结，PART-001~005/BUILD-001 已登记 |
| B0-05 | 记录 IDF/Python/GCC/Node/esptool 和依赖锁版本 | 已完成（只读） | 阶段 0.1、0.5 | `docs/review-2026-08-02/toolchain-baseline.md`；工具/依赖已核对，TOOL-001/002、DEP-001、BASE-VER 已登记 |
| B0-06 | 核对当前可复现构建命令和外部 build 目录 | 部分完成；等待 C 级 | 阶段 0.5 | `build-reproducibility.md`；命令/环境契约已核对，干净 configure/build 未授权 |
| B0-07 | 建立完整 source/config/resource hash manifest 与排除规则 | 已完成（只读） | 阶段 0.6、0.8 | `source-manifest.sha256` 7,918 项，SHA-256 `ba22f1e6...1450`；A2-06收口连续两次复算0 missing/mismatch/extra，含`CON`/`BASE-003` |
| B0-08 | 冻结硬件资料 hash、证据缺口和商家参考唯一版本 | 已完成（只读） | 阶段 0.10 | `hardware-evidence-manifest.md`；8 份硬件资料和 1,221 项商家 V2 manifest 已全量校验 |
| B0-09 | 建立源码/hello/OTA/文档/已有镜像版本真值表 | 已完成（只读） | 阶段 0.9 | `version-truth-table.md`；VER-001~006 已登记，不代表当前 7.0.24 可发布 |
| B0-10 | 记录 NVS schema/key 与 app-only OTA 兼容基线 | 已完成（只读） | 阶段 0.11、阶段 6 | `persistence-schema-and-ota-compat.md`；namespace/key、局部迁移、兼容矩阵与 NVS-001~005 已登记 |
| B0-11 | 测试能力、非 C 质量门和 secret scan 预检 | 已完成（A级预检） | 阶段 5.1、阶段 10 | `test-capability-precheck.md`；PS/Node/Python 语法通过，Bash/Clang/sanitizer/fuzz/secret scanner 能力缺口已登记 |
| B0-12 | 冻结工程外 cloud-gateway 与根 tools 支持测试资产 | 已完成（只读） | 阶段 0.12、阶段 5.1 | `supporting-test-assets-manifest.sha256` 188 项；状态/录音/报告排除规则已校验 |
| B0-13 | 建立实际链接成员 SBOM、许可证、CVE/VEX 和第三方 notices 一致性 | 已完成（A级静态；B/C/D开放） | 阶段 0.13 | `phase0-b013-sbom-license-cve-audit.md`；依赖锁、7个托管组件、3类本地vendored来源、31个license/notice文件及notices差异已盘点；实际linked成员仍待C级map |
| B0-14 | 建立隐私、音频数据流、同意、访问、保留和删除策略 | 已完成（A级静态；B/C/D开放） | 阶段 0.14 | `phase0-b014-privacy-data-flow-retention.md`；设备/网关/第三方数据流、同意、LAN访问、RAM残留和1.49GB测试语料留存已盘点；发布仍阻塞 |
| B0-15 | 建立生成资产 generator/input/output provenance 与可复现性 | 已完成（A级静态；B/C/D开放） | 阶段 0.15 | `generated-artifact-provenance.md`；41项生成输出、外部输入/根生成器、CMake成员、工具/许可证和确定性缺口已盘点；发布仍阻塞 |
| B0-16 | 建立 V1 -> V2 产品功能等价矩阵 | 已完成（A级静态；B/C/D开放） | 阶段 0.16 | `v1-v2-product-feature-parity.md`；10类产品功能已映射；监控扩展已实现但权威协议资产仍与20 ms固件/会议fail-close/V2身份不一致，发布仍阻塞 |
| B0-17 | 建立需求、状态、错误路径与测试覆盖矩阵 | 已完成（A级静态；B/C/D开放） | 阶段 0.17 | `requirements-and-test-coverage-matrix.md`；需求/状态/错误路径分母边界及150个正式源与测试资产关联已建立，发布仍阻塞 |
| B0-18 | 建立威胁模型、量产安全profile与密钥生命周期 | 已完成（A级静态；B/C/D开放） | 阶段 0.18、阶段6 | `security-threat-model.md`、`release-security-profile.md`、`key-lifecycle.md`；静态威胁/配置/密钥基线完成，不可逆eFuse仍需独立审批 |
| B0-19 | 建立阶段0~10全局唯一工作项/evidence/run索引 | 已完成（A级） | 阶段 0.19、阶段10 | `docs/review-2026-08-02/review-work-item-index.csv`；125个稳定ID、0重复、0缺字段，阶段10的8类执行项均强制绑定run_id |
| B0-20 | 第一方 memory-safety/UB 独立审计 | 已完成（A级静态；B/C/D开放） | 阶段 0.20、阶段8.3 | `first-party-memory-safety-ub-audit.md`；350/350逐文件覆盖，`MEM-002`原路径A级静态修复，动态/构建/目标机证据开放 |

## 4. 阶段 1 检查表

| ID | 工作项 | 状态 | 对应计划 | 证据 |
|---|---|---|---|---|
| A1-01 | 枚举应用及 ESP-IDF 系统任务、创建者、优先级、核心、栈、内存、停止 owner | 已完成（A级静态） | 阶段 1.1、1.13、1.15 | `phase1-architecture-rtos-audit.md`；任务图、系统任务配置和 RTOS-001~004 |
| A1-02 | 枚举 mutex/semaphore/spinlock/critical section，建立锁顺序和无限等待白名单 | 已完成（A级静态） | 阶段 1.3、1.7、1.11、1.12 | `phase1-architecture-rtos-audit.md`；锁图、白名单、非白名单链路与 RTOS-005~007 |
| A1-03 | 枚举 queue/ringbuffer/event group/task notification，核对元素所有权、容量、满队列和销毁 | 已完成（A级静态） | 阶段 1.4、1.16 | `phase1-queue-and-lifecycle-audit.md` 第 7~10 节；24 组正式队列/slot、Wi-Fi event group、任务通知及 RTOS-006~009/QUEUE-001~002/LIFE-001~003 已登记；动态验证仍待 B/D |
| A1-04 | 审计 worker 启停、static stack/TCB、self-delete、owner delete 和迟到访问 | 已完成（A级静态） | 阶段 1.5、1.16 | `phase1-worker-lifecycle-audit.md`；23 个创建点/28 个具名 worker 均已映射，确认 RTOS-004/005、LIFE-001~002/004~010，动态退出验证仍待 B/D |
| A1-05 | 审计 callback/timer/event/ISR 生命周期和执行上下文 | 已完成（A级静态） | 阶段 1.6、1.9、1.14、1.19 | `phase1-callback-isr-audit.md`；callback/ISR/DMA/cache 矩阵完成，EVENT-002~003、WS-CB-001~002、TIMER-002、CACHE-001、ISR-001 已登记；B/D 动态验证开放 |
| A1-06 | 审计无限循环、阻塞 I/O、tick/时间换算、错误传播和跨核可见性 | 已完成（A级静态） | 阶段 1.7~1.13 | `phase1-time-error-crosscore-audit.md`；37个无条件循环、152个tick转换、324个discard候选完成分类，新增TIME/WAIT/ERR/MEM/CROSS问题 |
| A1-07 | 审计公开 API 可重入性、静态工作区、单 owner 契约和回调重入 | 已完成（A级静态） | 阶段 1.17 | `phase1-api-reentrancy-audit.md`；修正版登记API-001~010，独立差异复核已补speaker/P3 software ref/mode/wake/LVGL遗漏 |
| A1-08 | 形成任务/锁/队列/内存/状态图、无界等待清单和 P0/P1 问题表，并反向校验计划 | 已完成（A级静态） | 阶段 1 输出 | `phase1-consolidated-topology-state-risk-audit.md`；多轴状态表、31个canonical P1、0个直接P0及B/C/D关闭矩阵完成；新增STATE-001/002 |
| A1-09 | 建立 `ESP_ERROR_CHECK`/assert/abort/restart 致命路径清单及 reset/core-dump/heartbeat/日志可观测性矩阵 | 已完成（A级静态；B/D开放） | 阶段 1.18、1.20 | `phase1-fatal-reset-observability-audit.md`；151文件分母、3/32/3/4/0/0精确计数、reset/core-dump/heartbeat/log/privacy矩阵完成；新增OBS-001/LOG-001/PRIV-001 |

## 4A. 阶段 2 检查表

| ID | 工作项 | 状态 | 主证据 | 反向结论 |
|---|---|---|---|---|
| A2-01 | `app_main`与主任务初始化依赖顺序 | 已完成（A级；产品/B/C/D开放） | `phase2-startup-power-recovery-audit.md`第4~15节 | 启动ledger、failure cut、capability缺口和问题ID闭合；验收未通过 |
| A2-02 | SW3点按/去抖/电源保持与关机顺序 | 已完成（A级；产品/B/C/D开放） | 同上第16节 | 默认按键状态机静态成立；系统quiesce、电气和循环验收未通过 |
| A2-03 | brownout/WDT/panic/abort后的NVS与OTA恢复 | 已完成（A级；产品/B/C/D开放） | `phase2-reset-recovery-matrix.md`第1~9节 | OTA局部恢复链存在；NVS全擦、crash-loop、factory/回退目标未闭合 |
| A2-04 | bootloader电源保持、strap/eFuse/USB-JTAG/download接管 | 已完成（A级；产品/C/D开放） | `phase2-boot-power-handoff.md` | 正常SPI boot当前匹配；ROM下载供电、strap/eFuse/量产debug未验收 |
| A2-05 | 低电、充电、外部供电、读数失败与OTA准入 | 已完成（A级；产品/B/C/D开放） | `phase2-battery-power-state-matrix.md` | 状态矩阵闭合；电量可信度、UI语义、OTA硬门未通过 |
| A2-06 | Wi-Fi/云离线时本地能力 | 已完成（A级；产品/B/D开放） | `phase2-offline-local-capability.md` | 本地部分能力独立；wake/local owner和陈旧动作未通过，配网漂移待最终重审 |
| A2-07 | 启动失败的悬挂任务、半初始化、重复注册与DMA回滚 | 已完成（A级；产品/B/D开放） | `phase2-startup-failure-rollback.md` | 模块局部rollback存在；全局startup owner和资源守恒未通过 |
| A2-08 | fatal/restart原因、core dump与持久化预算 | 已完成（A级；产品/C/D开放） | `phase2-reset-recovery-matrix.md`第10节 | fatal分母与站点闭合；site/dump/heartbeat/privacy/storage未通过 |
| A2-09 | 阶段2时序图、状态矩阵与反向收口 | 已完成（A级；产品/B/C/D开放） | `phase2-startup-power-recovery-audit.md`第23节 | 八项证据、42个去重ID、修复顺序和门禁反查完成；阶段2仅A级收口 |

## 4B. 阶段 3 检查表

| ID | 工作项 | 状态 | 主证据 | 反向结论 |
|---|---|---|---|---|
| A3-01 | 新版硬件GPIO复用、方向、上下拉、open-drain与冲突 | 已完成（A级；产品/C/D开放） | `phase3-pin-bus-audit.md` | 最新活动主映射与原理图/商家V2一致；GPIO45 strap/eFuse、GPIO8头部输入、GPIO18 LED和整板selftest可达性未通过；GPIO11面板触摸不替代GPIO8 |
| A3-02 | PSRAM/flash/电源轨/温升/brownout/PA瞬态/I2C卡死测量 | 已完成（A级；产品/C/D开放） | `phase3-electrical-measurements.md` | 16 MiB/8 MiB配置与N32R8标注未绑定；shared I2C无物理恢复；PA VOUT/反馈增益未进入ES8311音量模型；rail/热/brownout测量未执行 |
| A3-03 | ES7210地址、复位、MICBIAS、采样率、slot与MIC3路径 | 已完成（A级；产品/C/D开放） | `phase3-audio-hardware-audit.md` | 地址/主从/16 kHz和64-BCLK预算、局部回滚成立；RMW读错静默写回、MIC3/slot2非循环映射、完整寄存器资料和实测未闭合 |
| A3-04 | ES8311 DAC/ADC、I2S主从、音量、静音与pop时序 | 已完成（A级；产品/C/D开放） | `phase3-audio-hardware-audit.md` | DAC-only/slave和16 kHz/64-BCLK预算、软件音量与reference同源成立；codec错误吞没/非事务状态、shared-TX故障无物理静音、完整寄存器与pop实测未闭合 |
| A3-05 | NS4150B/PA_CTRL极性、mute、enable与掉电默认态 | 已完成（A级；产品/D开放） | `phase3-audio-hardware-audit.md` | GPIO4 active-high与R18下拉成立；normal stop仅数字quiet，PA启停ready、故障物理静音和关机掉电前quiet未闭合 |
| A3-06 | I2S/DMA时钟、slot、buffer、共享TX/RX与stall恢复 | 已完成（A级；产品/C/D开放） | `phase3-audio-hardware-audit.md` | 双向64-bit/frame和90 ms DMA预算成立；诊断绕过owner、RX drop/partial/stall不可观测、lease/teardown代次未闭合 |
| A3-07 | MIC1/MIC2/MIC3映射、方向、reference与AEC health gate | 待执行 | `phase3-microphone-reference-validation.md` | - |
| A3-08 | LCD/QSPI/LVGL时钟、DMA、PSRAM buffer与异常屏幕 | 待执行 | `phase3-display-touch-sensor-audit.md` | - |
| A3-09 | 触摸reset、I2C retry、IRQ、手势与总线卡死 | 待执行 | `phase3-display-touch-sensor-audit.md` | - |
| A3-10 | BQ27220电量/充电/外部电源、错误读数与刷新 | 待执行 | `phase3-battery-imu-led-audit.md` | - |
| A3-11 | QMI8658A初始化、量程、FIFO/中断与断开恢复 | 待执行 | `phase3-battery-imu-led-audit.md` | - |
| A3-12 | LED动画、颜色/亮度、关机默认态与队列释放 | 待执行 | `phase3-battery-imu-led-audit.md` | - |
| A3-13 | SD/底座UART/4G静态隔离与失败影响 | 待执行 | `phase3-optional-driver-isolation.md` | - |
| A3-14 | 诊断/自检/测试音频/USB入口、权限、音量和停止 | 待执行 | `phase3-diagnostic-surface-audit.md` | - |
| A3-15 | BLE/NimBLE连接、MTU、会话释放和Wi-Fi共存 | 待执行 | `phase3-ble-driver-coexistence.md` | - |
| A3-16 | 阶段3引脚、时序、驱动状态机和未决项收口 | 待执行 | `phase3-driver-consolidated-audit.md` | - |

## 5. 计划复核结果

当前结论：V1.8 已覆盖 tracked+untracked/归档源码、未链接原型、bootloader、组件兼容层、配置矩阵、驱动、音频、网络、OTA、UI、资源、故障注入和长稳，并把此前只有上位概念、没有独立证据入口的 SBOM/license/CVE、隐私/录音留存、第一方 memory-safety/UB、生成资产 provenance、V1->V2 功能等价、需求/测试覆盖、威胁模型/密钥生命周期和全阶段唯一工作项索引补成正式交付物。计划条款完整性本轮复核完成；阶段0、阶段1和阶段2的 A 级静态证据已收口，阶段2八项产品验收均未通过；所有B/C/D门仍开放，不能把“计划或静态盘点已完成”写成“发布或设备已通过”。并行工程师仍在修改源码；任何源码/配置漂移都必须重算manifest、刷新受影响结论，并在全部审计完成后按最终代码做一次跨阶段全量复核。

## 6. 问题登记

| ID | 等级 | 模块 | 问题 | 状态 |
|---|---|---|---|---|
| BASE-001 | P1 | 基线/发布追溯 | `NuwaxAIPartner-v2` 在上级 Git 仓库中整体未跟踪，HEAD 无法唯一标识当前源码；必须用完整文件 hash manifest 冻结，最终发布前应纳入受控版本库 | 开放；不阻止只读审计，阻止基于 commit 的发布结论 |
| BASE-002 | P2 | 源码归属 | 8 个非 host `.c` 未进入正式 CMake，其中多个为带 TODO/30 ms/占位逻辑的历史原型，容易被误认为产品能力 | 开放；已建立逐文件处置清单 |
| BASE-003 | P2 | Windows工作区/发布输入 | 工程根存在保留名文件 `CON`，内容是sdkconfig片段；普通Win32工具无法稳定访问，来源和用途不明 | 开放；已通过长路径纳入manifest，来源确认前不得作为构建输入或擅自删除 |
| CONFIG-001 | P1 | 配网/传输安全 | 当前候选配置使用固定开发 PoP 且允许不安全 WS，不满足量产唯一凭据与 WSS-only 门禁 | 开放；不等同当前联网故障，阻止量产发布 |
| CONFIG-002 | P1 | 启动/存储安全 | Secure Boot 与 Flash Encryption 未启用 | 开放；阻止量产发布，eFuse 操作需独立批准 |
| CONFIG-003 | P2 | 配置追溯 | `sdkconfig.barge-final` 与当前 `sdkconfig` 有 49 项归一化配置漂移；产品 overlay 也未冻结当前 30% OTA 电量门槛 | 开放；需归档用途并把安全阈值写入唯一产品 overlay |
| BUILD-001 | P1 | 发布基线 | 当前源码 7.0.24/7000024 没有与源码 manifest、冻结配置和 OTA manifest 绑定的发布产物；既有外部 7.0.23 build仍未与本 manifest绑定 | 开放；阻止发布/生产 OTA |
| PART-001 | P1 | OTA/分区兼容 | 运行时 partition_schema=1 未校验 label/address/size/model/partition hash，旧表可能误判兼容 | 开放；阻止 OTA 兼容放行 |
| PART-002 | P2 | OTA 容量 | 物理 app 槽约 4.75 MiB，但 OTA manifest 上限 4 MiB；既有 7.0.23 app 3,554,880 bytes，在4 MiB协议上限内仅余639,424 bytes | 开放；需容量策略 |
| PART-003 | P2 | 恢复 | 无 factory recovery，双 OTA 槽损坏只能有线恢复 | 开放；产品/发布决策 |
| PART-004 | P2 | 构建入口 | 两套旧 CSV 留在根目录，手工混烧存在误用风险 | 开放；需入口阻断/归档 |
| PART-005 | P2 | OTA 资源 | app-only OTA 不更新 srmodels/partition；模型或布局变化无兼容机制 | 开放；需资源 OTA 或全量升级策略 |
| TOOL-001 | P2 | 工具链 | C:\Espressif 与 %USERPROFILE%\.espressif 两套工具根，且存在 IDF 5.5.4/6.0 并存 | 开放；需统一初始化入口 |
| TOOL-002 | P3 | 测试工具依赖 | simulator requirements 固定直接版本但无传递依赖 hash 锁 | 开放；不阻止当前只读阶段 |
| DEP-001 | P1 | 依赖来源 | 本地 lib/lvgl 覆盖 registry LVGL，lock hash 不能证明实际本地源码 | 开放；需 vendored provenance |
| SBOM-001 | P1 | 发布/SBOM | 未发现覆盖最终ELF、ESP-IDF、托管组件、本地vendored库、预编译库和模型的SPDX/CycloneDX产品SBOM；单独的cJSON sbom文件不能代表产品 | 开放；阻止发布，需绑定干净构建map/ELF/component graph/image hash生成并校验SBOM |
| LIC-001 | P1 | 第三方许可证 | `THIRD_PARTY_NOTICES.md`仍是计划性登记，未按实际链接成员闭合版本、来源、SPDX license、版权和发布义务，且混入未锁定的计划`bmi270` | 开放；阻止发布，需link map反向生成清单并与lock/notices三方差异校验 |
| LIC-002 | P1 | Vendored许可证 | `lib/GFX Library for Arduino`有版本和来源URL但未见同目录LICENSE/NOTICE或manifest license字段；是否链接也未由当前发布map绑定 | 开放；链接则补齐来源/许可证/义务，未链接则进入明确排除清单 |
| CVE-001 | P1 | 漏洞治理 | 未发现产品CVE inventory、扫描报告、数据库快照、VEX或例外审批，不能证明当前依赖无已知漏洞 | 开放；阻止发布，需组件/CPE/版本绑定扫描与VEX闭环 |
| GEN-001 | P1 | 生成资产边界 | V2正式资源依赖工作区根generator和sibling handoff输入，发布树自身不能独立重建；外部hash基线没有绑定同一release identity | 开放；阻止发布，需受控generator/input/lock/license资产包或纳入V2 |
| GEN-002 | P1 | 生成溯源 | face/dialog/status缺确切converter/命令，face-diff缺历史current-dir，PNG/SVG权威输入和自定义资产ownership/license未逐输出绑定 | 开放；阻止发布，需机器可读逐资产provenance且空字段失败 |
| GEN-003 | P1 | 生成工具链 | menu和字体生成路径仍指向V1，face-path读工程外输入；Python/Pillow/lv_font_conv/图片converter版本与传递依赖未锁定 | 开放；阻止发布，需hermetic工具链和显式V2输入输出路径 |
| GEN-004 | P1 | 确定性重生成 | 无41项byte-for-byte重生成门禁，face-diff还嵌入wall-clock时间，按设计不能产生相同输出 | 开放；阻止发布，移除非确定性并在双干净目录重生成、逐项hash比较、绑定image identity |
| BASE-VER | P2 | 版本追溯 | 工程内旧 build仍为7.0.21，源码为7.0.24；外部7.0.23构建虽存在但未绑定当前source manifest | 开放；目录名/缓存不能作为发布证据 |
| HW-EVID-001 | P1 | 硬件证据 | 无生产 BOM/netlist/PnP，缺 BQ27220/QMI8658A 原厂完整资料，原理图不能证明样机贴装与铜线 | 开放；阻止完整硬件验收结论 |
| HW-PIN-001 | P1 | GPIO45/启动strap | 新板把GPIO45用于带10 kOhm外部上拉的SD_CMD，而ESP32-S3在reset时以GPIO45选择VDD_SPI；当前无绑定样机的模组/eFuse摘要、复位采样电平和量产料号证据 | 开放；禁止从曾成功启动外推全批次安全，先做只读eFuse/模组绑定和复位波形，任何eFuse写入需独立审批 |
| HW-FLASH-001 | P1 | Flash/模组容量绑定 | 原理图标注ESP32-S3-WROOM-1-N32R8，当前sdkconfig/分区/烧录契约按16 MiB，selftest只做Flash>=16 MiB且未从app_main调用 | 开放；绑定JEDEC/模组/eFuse与image identity，冻结16 MiB可用窗口或32 MiB产品决策，统一分区/OTA/烧录/selftest |
| HW-ELEC-001 | P1 | 电源/热/brownout证据 | 无绑定样机和镜像hash的VOUT、MCU_3V3、CODEC_3V3、4G_VCC、PA电流/瞬态、温升、受控brownout及恢复证据；代码只有配置和重启后告警 | 开放；需冷启动/USB/电池/Wi-Fi/音频/AEC/音量/brownout C/D波形与阈值签核 |
| I2C-REC-001 | P1 | 共享I2C卡死恢复 | shared bus manager只有init/get/add/remove/probe，无SCL脉冲、STOP、GPIO接管或bus重建；touch保留handle，battery只回收device，SDA/SCL拉死后无可证明恢复路径 | 开放；增加唯一owner的有界物理recovery和epoch重绑，完成SDA-low/SCL-low/掉电恢复D级验证 |
| PA-GAIN-001 | P1 | PA音量/供电模型 | 原理图NS4150B VCC接VOUT且RIN/RF约240k/100k（约7.6 dB），ES8311代码固定pa_voltage=5.0f且pa_gain=0；依赖库将该值用于set_vol补偿，低电量/USB/高音量模型不可信 | 开放；冻结VOUT范围、实装反馈与扬声器规格，将真实pa_voltage/pa_gain纳入唯一volume/AEC标定并做削波/声压验收 |
| PA-TIME-001 | P1 | NS4150B启停/掉电时序 | 规格书仅给启动/关断典型120/80 ms且无ready反馈；当前PA high后无ready门，deinit无shutdown完成门，SW3强制low失败仍继续且只等20 ms便释放PG2 | 开放；audio-board唯一owner显式PA状态机和有界ready/quiet门，首PCM与PG2释放必须受门禁；同步GPIO4/OUTP/N/VOP/N/扬声器/VOUT波形验收 |
| AUDIO-SLOT-001 | P1 | ES7210 TDM/MIC3 reference | 历史逐PGA只确认当前样机MIC1->DMA[0]、MIC2->DMA[1]；MIC3/MIC4不可观测。规格书Figure 2e通道标号、代码slot2注释及以slot2为目标的health检查不能形成MIC3->DMA[2]的非循环证明 | 开放；向MIC3P/N注入独立差分信号并逐PGA确认唯一DMA索引，再执行PA-off/DAC-only相关性与延迟验证；未闭环前强制software reference |
| CODEC-RMW-001 | P1 | ES7210/I2C错误传播 | `es7210_update_reg_bit()`忽略寄存器read错误，仍以0值写回并只返回write结果；I2C瞬态读失败可能被报告成功且覆盖目标寄存器未更新位 | 开放；read失败必须立即返回且禁止write，逐RMW fault injection验证错误上报、0次写回、状态不变和板级受控回滚 |
| CODEC-ERR-001 | P1 | ES8311/I2C与GPIO错误传播 | `set_fs()`不判NULL、非法bits静默降级并对正常请求固定成功；open/start/config_sample覆盖此前错误；RMW可在read失败后write；mute和PA helper丢弃write/GPIO结果，板级检查不能证明硬件成功 | 开放；检查NULL/位宽/rate并逐步fail-fast，保留首错/寄存器/阶段，read失败0次write，PA helper返回错误；逐事务注入NACK/timeout且不得继续enable/PA |
| CODEC-STATE-001 | P1 | ES8311 enable/disable/pair状态事务 | start最终失败仍add pair且enable继续开PA/unmute；enabled保持false使后续disable可no-op。disable/close忽略quiet/suspend错误，delete仍无条件free | 开放；显式状态机，成功后才commit pair/enabled；失败强制mute+PA low+suspend并保留对象可重试，禁止close失败后无条件free |
| AUDIO-STOP-001 | P1 | shared-TX故障物理静音 | V2正常停播依赖完整DMA零预装；I2S disable/preload/re-enable失败时不调用codec mute或PA low，PA仍可能为high，且re-enable失败注释错误声称PA保持muted | 开放；audio-board提供有界physical-quiet API，任一stop错误立即codec mute并验证GPIO4 low，锁住新播放直至受控恢复；故障注入测物理静音deadline |
| AUDIO-OWN-001 | P1 | 音频诊断owner | 云端/手工AEC health只检查speaker pending与normal prelisten，未检查wake/meeting/monitoring；随后直接读共享RX、写共享TX并改codec/PA，绕过MIC owner和board lease | 开放；诊断纳入唯一audio command bus，以generation token原子申请RX+TX+codec独占权；活动owner存在时fail-closed，完成后事务恢复并做交错测试 |
| I2S-OBS-001 | P1 | I2S采集完整性/恢复 | ESP-IDF RX queue满会静默drop oldest，项目未注册overflow callback/counter；上层sequence无法识别物理缺口，timeout partial-read也未校验TDM frame对齐，会议/监控持续stall最坏约50 s才停止 | 开放；per-generation overflow/underflow/partial/stall telemetry，frame-byte断言和有界重建；发生drop必须显式失败或新session，禁止伪装同session连续 |
| AUDIO-LIFE-001 | P1 | audio-board lease/teardown代次 | RX/TX lease仅bool且release无token/generation；迟到release可清他人lease。final teardown先归零refcount并清句柄，再disable/delete；失败后不能重试回收且下次init可能冲突 | 开放；token化lease与代次校验，显式UP/QUIESCING/FAULTED/DOWN状态；全部delete成功后才清句柄/refcount，失败保留可重试owner或受控restart |
| DRV-REACH-001 | P1 | 板级驱动/诊断可达性 | GPIO8头部触摸、GPIO18绿灯及整板selftest源码均进入正式CMake，但最新正常产品启动无GPIO8/18初始化，`nuwax_touch_init()`仅初始化GPIO11面板触摸，`CONFIG_NUWAX_HW_ACCEPTANCE_MODE`也未调用`nuwax_hardware_selftest_run()` | 开放；功能代码存在不等于镜像可用，需冻结产品owner/诊断入口并完成C/D验证 |
| BAT-001 | P1 | 电量可信度 | 缺样机电芯对应 BQ27220 CEDV/profile 认证资料 | 开放；阻止“可信电量百分比”验收 |
| BAT-002 | P2 | 电源/UI语义 | 板上无 MCU 可见 VBUS，service 的 EXTERNAL 状态不可达，但 UNKNOWN/invalid/stale 被 UI 强制表达为“外接供电”；status bar 还画成 battery 图标而 drawer 画 external | 开放；需 UNKNOWN/verified-external 分离并统一 UI 映射 |
| BAT-003 | P2 | 低电决策 | 未认证 CEDV/profile 的 SOC/TDA/FD 仍参与 low latch，可能错误触发低电 UI/表情和 OTA 拒绝；当前无自动低电关机 | 开放；估算显示与产品安全低电原因必须分离 |
| BAT-004 | P1 | OTA电源门 | charging 单样本无条件绕过 SOC/电压/low/VBUS，trusted SOC 分支也无统一硬电压下限且忽略 low flag；同一逻辑用于 OTA admission 和新镜像 boot health | 开放；阻止产品 OTA/发布，需唯一 fail-closed admission、硬电压门、连续充电样本和真实 VBUS/PG 证据 |
| VENDOR-001 | P2 | 商家参考 | 两套 cyberkit 均无 Git provenance 且核心音频有分歧 | 已冻结测试工程 V2 树；来源 provenance 仍开放 |
| VER-001 | P1 | 发布真值 | 源码/config/partition/model/app/OTA manifest/hash 未绑定成单一发布 identity | 开放；阻止发布 |
| VER-002 | P1 | 云端契约 | 正式交接文档/schema 未闭合实际 firmware/OTA identity；会议/监控唯一权威文档还把当前20 ms/640-byte上行写成16 ms/512 bytes，并保留旧会议停播和COM8/V1.2身份描述 | 开放；需设备/协议资产/测试/正式云同步 |
| VER-003 | P1 | 版本语义 | hello release_sequence 混合当前镜像 identity 与 anti-rollback floor，可能与 descriptor/hash 不一致 | 开放；需 identity tuple |
| VER-004 | P1 | 构建版本 | PROJECT_VER 与 release sequence 手工双维护且无构建期一致性断言 | 开放；需单一版本源 |
| VER-005 | P2 | OTA 观测 | OTA ACK/progress/result 未统一返回 release_sequence/image_sha256 | 开放；审计依赖上下文 |
| VER-006 | P2 | 文档 | 根 README 仍为 7.0.17/7000017，历史目录名也不可信 | 开放；需更新/隔离历史资料 |
| NVS-001 | P1 | 恢复/安全 | NVS 页耗尽或新格式时启动代码直接全量 erase，清除云/Wi-Fi/OTA accepted sequence | 开放；需受控恢复并保留安全 floor |
| NVS-002 | P1 | 凭据保护 | NVS/Flash Encryption 未启用，Wi-Fi 密码和 device secret 明文且有重复存储 | 开放；量产阻塞 |
| NVS-003 | P2 | Schema/RMA | 无统一 NVS schema 和受控 factory reset/RMA 契约 | 开放；需清除/保留矩阵 |
| NVS-004 | P2 | Flash 寿命 | Wi-Fi reconnect/音量/L3/OTA 写入无寿命与容量预算，部分相同值仍 commit | 开放；需节流、stats 和 endurance 测试 |
| NVS-005 | P2 | 掉电一致性 | BLE 配网跨 ESP Wi-Fi 与多个应用 namespace 顺序提交，静态代码不能证明所有掉电点 old/new pair 一致 | 开放；需逐 commit 掉电矩阵 |
| TESTCAP-001 | P2 | 测试环境 | Bash/WSL、Clang/ASan/UBSan、cppcheck/clang-tidy、Kconfig lint 未冻结可执行入口 | 开放；需 B 级能力建设 |
| TESTCAP-002 | P2 | 故障测试 | fault injection/fuzz 只有分散脚本/计划，无统一 harness/corpus/CI 门禁 | 开放；需 B 级能力建设 |
| SEC-001 | P1 | 配网安全 | 固定开发 PoP 仍在产品候选配置和源代码 fallback | 开放；量产阻塞 |
| SEC-002 | P1 | 发布资产 | 测试签名私钥在工作区，无自动 allowlist/交付包阻断 | 开放；量产阻塞 |
| SEC-003 | P2 | 发布审计 | 无正式 secret scanner，尚未覆盖高熵/历史/二进制泄漏 | 开放；需安全门建设 |
| RTOS-001 | P1 | 任务健康 | Task WDT开启但panic关闭，业务worker没有统一订阅/heartbeat/有限恢复；阻塞worker可长期无声失效 | 开放；需heartbeat/WDT策略和卡死注入 |
| RTOS-002 | P1 | PSRAM任务栈 | 多个实时/网络worker使用PSRAM stack，尚无逐任务cache-disabled/ROM调用安全证明 | 开放；需调用路径分类、必要时迁移internal并做C/D验证 |
| RTOS-003 | P1 | 实时调度 | MIC任务与Wi-Fi/TCPIP/timer等系统任务的核心/优先级竞争尚无p95/p99、丢帧和队列水位证据 | 开放；需阶段4/9 trace与压力测试 |
| RTOS-004 | P2 | 常驻worker | power key、touch、USB等常驻任务缺统一stop/reap或重复init契约 | 开放；需模块级生命周期和重复init验证 |
| RTOS-005 | P1 | 唤醒任务停止 | wake detector stop/reap 无 deadline；worker 卡住时主控可永久等待，业务 worker 未被统一 WDT/heartbeat 覆盖 | 开放；需有限 stop、fault/restart 策略和卡死注入 |
| RTOS-006 | P1 | UI/face cache | `face_cache` worker 与 LVGL/UI 路径跨核并发读写 cache slot、LRU、保护 emotion 元数据和 slot buffer，无 mutex/atomic/immutable handoff；task 创建失败还会保留 preload queue 且无同步回退 | 开放；需单 owner或同步发布，补齐初始化回滚，并做并发 stress/实机切换验证 |
| RTOS-007 | P1 | BLE 配网任务队列 | vox-config handler 入队后用 `portMAX_DELAY` 等 completion；stop/close 可删除 queue/gate，但没有 in-flight handler 引用计数、join/cancel 屏障 | 开放；需 shutdown gate、取消/完成全部 job、等待 handler 引用归零后再销毁对象 |
| RTOS-008 | P1 | 启动回滚 | `app_main` 线性启动在后续分配/驱动失败时直接返回，前置 task/queue/controller 未按反向依赖清理 | 开放；统一 fail-cleanup 或明确安全重启，并为每条失败路径提供故障注入证据 |
| QUEUE-001 | P1 | Wi-Fi 状态/扫描报告 | producer `xQueueOverwrite` 与 consumer `peek -> 网络发送 -> receive` 之间可发生 overwrite，consumer 可能删除最新报告 | 开放；改为 receive-first/revision/retry-safe 语义并做并发测试 |
| QUEUE-002 | P1 | 会议 ACK mailbox | 深度 1 `xQueueOverwrite` 无 request_id/epoch 过滤，陈旧 ACK 可覆盖有效 ACK，导致请求超时 | 开放；按 request_id/epoch 保留、拒绝陈旧项并测试乱序 ACK |
| RTOS-009 / EVENT-001 | P1 | Wi-Fi 初始化与 event handler 回滚 | event group、netif、driver、event handler 任一步失败直接返回；handler instance 只保存在局部变量，重试可能泄漏、重复注册并产生多次回调 | 开放；保存 handler/netif 句柄并按反向顺序注销/删除，加入逐点失败注入；同一根因只计一次 |
| LIFE-001 | P2 | barge dispatcher 初始化 | lock/queue/task 部分初始化失败时未统一回收；并发首次 `bind()->init()` 也可创建并覆盖不同lock/queue/task，形成多worker或泄漏 | 开放；永久lifecycle gate、反向回滚和并发重复init/deinit测试 |
| LIFE-002 | P1 | 诊断音频 worker | vocat 临时 TX task 等待 1500 ms 超时后函数返回，worker 仍可能访问栈上 `tx_ctx` | 开放；超时必须取消并 join，或将上下文移到受控 heap/静态 owner；诊断配置也需安全关闭 |
| LIFE-003 | P2 | 触摸孤立对象 | `s_touch_trigger`/`s_touch_events` 仅创建无生产者/消费者，失败路径也不回收 | 开放；删除废弃对象或接入唯一 touch owner，并覆盖启动失败回滚 |
| LIFE-004 | P1 | LVGL 初始化回滚 | timer/display/draw buffer 已创建后，timer start 或 task 内存/创建失败直接返回，timer 可能继续运行且资源无法回收 | 开放；统一 `goto fail` 反向删除 timer/display/indev/buffer/mutex/task 资源并做逐点失败注入 |
| LIFE-005 | P2 | USB 控制 worker | USB driver 安装失败时 worker 自删，但 callback/context 注册状态未清空，后续 start 永久报已启动 | 开放；保存 task 状态，失败清空注册并提供 stop/retry |
| LIFE-006 | P2 | LED worker/通知 | setter 解锁后使用 task handle 快照，可能与 deinit 删除 task 交错后通知失效句柄 | 开放；通知纳入同一生命周期屏障或 generation，并做并发 setter/deinit 测试 |
| LIFE-007 | P2 | 诊断 active-tone | stop 超时后 live heap ctx/task/TX lease 仅能由调用者持同一 handle 重试回收，无模块异步 reap | 开放；冻结重试契约或由模块接管异步回收，验证超时后无泄漏/lease 占用 |
| LIFE-008 | P2 | face cache 初始化 API | `face_cache_init()` 无幂等 guard，重复调用会覆盖 slot 指针并泄漏旧 buffer | 开放；增加初始化状态/显式拒绝或 deinit，并做重复 init 测试 |
| LIFE-009 | P1 | WebSocket RX 启动发布 | `xTaskCreateWithCaps()` 返回前新 RX 可立即退出并发布 `RX_EXITED/running=false`，创建者随后无条件写 `client->rx_task` 和 `OPEN`，把退出事实覆盖为已连接 | 开放；创建前发布 STARTING/handle owner，创建后以 CAS/epoch 检查 worker 是否已退出，禁止覆盖终态，并做立即断连交错测试 |
| LIFE-010 | P1 | OTA controller 部分初始化 | register/core init/pending/interlock/task-create 任一步失败时主要只删除 command queue，未完整 unregister/deinit/撤销 interlock，重试和测试恢复不闭合 | 开放；按反向依赖统一回滚，明确 controller stop/deinit，并做逐点失败注入和重复 init 测试 |
| NET-CONC-001 | P1 | WebSocket 锁/背压 | TX 持 lifecycle 与 I/O 锁覆盖完整网络写/重试；RX 共用 I/O 锁，close 获取 lifecycle 锁超时后缺可靠锁存/重试 | 开放；需拆分 RX/TX、锁存 close intent 并做 TX stall 故障注入 |
| NET-CONC-002 | P1 | WebSocket 生命周期 | CONNECTING 期间的 close/cancel 可在握手结束后被无条件 OPEN/connected 发布覆盖 | 开放；需阶段性 cancel/epoch 检查与 connect/close 交错测试 |
| AUDIO-CONC-001 | P1 | TTS ingress/speaker | WS RX callback 在 speaker ingress 前无限等待 manager mutex；播放 owner 可持该锁做 I2S lazy init/retry | 开放；RX 只允许有界/非阻塞提交，硬件 init 移出 manager lock |
| AUDIO-CONC-002 | P2 | 音量/播放锁 | 音量 setter 在 manager lock 内同步等待 output lock，I2S writer 可长持 output lock，间接阻塞 RX ingress | 开放；逻辑音量与硬件应用异步化并以 revision 回读 |
| NET-CONC-003 | P2 | WS 控制 ACK | meeting/monitoring busy/full ACK 在 RX callback 内走普通同步 WebSocket send | 开放；ACK 投递有界 control-TX owner，禁止 RX 同步写网络 |
| EVENT-002 | P1 | Wi-Fi 跨任务状态 | default event loop、fallback worker 和 app caller 无锁读写 retry/connect/fallback/active index、SSID/IP、scan 状态；字符串 snapshot 可撕裂 | 开放；改为单 owner + command/event queue，或用短锁保护带 revision 的完整快照；并发 scan/connect/disconnect/fallback stress 关闭 |
| EVENT-003 | P2 | BLE provisioning lifecycle flag | `s_started` 由 owner 普通读写，event callback 即使持 `s_state_lock` 读取也无 happens-before | 开放；将 started/stopping/state 纳入同一锁或 C11 atomic，并做 start/stop/disconnect 交错测试 |
| WS-CB-001 | P1 | WebSocket activity timestamp | RX task 与 app owner 以 `volatile int64_t` 共享毫秒时间戳；ESP32-S3 32 位访问可能撕裂且无内存序 | 开放；改为原子 64 位、同一 portMUX snapshot，或 wrap-safe 32 位计数，并做双核高频交错测试 |
| WS-CB-002 | P1 | 会议/监控 RX 预判 | RX callback 读取仅为 `volatile`/普通字段的 mode owner 状态并提前决定 busy/can_start，meeting/monitoring 同时 enable 可得到错误 ACK 或双入队 | 开放；RX 只解析并入队，互斥与 ACK 由单一 mode owner 决策；交错测试必须恰有一个成功 |
| TIMER-002 | P1 | LVGL timer 分配失败 | status bar/focus menu 创建 timer 后未判空即调用 timer API；内存压力下可 assert/reset 或 NULL 访问 | 开放；检查结果、传播 UI init 失败并做 allocator 逐点失败测试 |
| CACHE-001 | P1 | NVS/Flash 与实时音频 | 普通运行期 volume/wake/L3/Wi-Fi NVS commit 没有统一 audio/cache-disabled 仲裁，而 I2C/I2S 完整 IRAM-safe 未启用 | 开放；持久化集中到 flash owner 并 coalesce/选择静默窗口；B/D 量化 NVS GC、I2S gap/序号和 I2C timeout |
| ISR-001 | P2 | Touch GPIO ISR cache-safe | handler 本体为 IRAM 且只通知 task，但 GPIO ISR service 用 flags=0，整条 dispatch 链不具 IRAM-safe 契约 | 开放；由单一 owner 以 IRAM flag 安装并证明数据驻留，或以规格书/逻辑分析证明 cache 窗口事件可接受 |
| API-001 | P1 | MIC owner/API | live mic object重复init先清空唯一resource/owner metadata，再因全局owner被占而失败，导致lease/refcount/buffer不可回收 | 开放；opaque instance/generation、无副作用reject和重复/陈旧handle测试 |
| API-002 | P1 | P3 L3 lifecycle | enable/disable/process/stats共享AFE/buffer无同步，重复init可使live AFE stranded | 开放；单audio owner或lifecycle lock+in-flight，资源守恒测试 |
| API-003 | P2 | Battery service | listener self-stop会等待自身completion；并发stop共享binary signal且结果不可线性化 | 开放；callback-safe async stop或唯一join owner，多waiter测试 |
| API-004 | P2 | Audio uplink | per-context API触碰process-global batch workspace；双producer/未start context和after_end重入finish/destroy契约不闭合 | 开放；workspace移入context/唯一producer与callback self-wait保护 |
| API-005 | P2 | Cloud config | active/pending flags与struct在endpoint callback和provision owner间无同一同步域 | 开放；revisioned immutable snapshot或同一mutex，交错测试 |
| API-006 | P1 | Speaker init | 重复init无guard/deinit，会替换live worker使用的queue/mutex并泄漏旧对象 | 开放；一次性lifecycle state或完整stop/join/deinit，重复init故障注入 |
| API-007 | P2 | P3 software ref | 首次mutex创建非原子；feed/read在取锁前读取可被reconfig改写的普通metadata | 开放；永久init gate、完整同步域和并发reconfig测试 |
| API-008 | P2 | Meeting/monitoring object | init先memset live caller object，重复/并发init会丢失旧queue且无run/RX/deinit屏障 | 开放；分阶段临时创建后原子发布，stop/drain/join后deinit |
| API-009 | P2 | Wake init | 首次init在稳定lock前无锁检查并memset singleton；并发init/deinit可覆盖lock/model状态 | 开放；永久lifecycle state machine和逐点失败/交错测试 |
| API-010 | P2 | LVGL callback ABI | pre-handler callback与context分两次普通写/读，运行期替换可配对不同revision | 开放；在LVGL同步域发布完整callback snapshot并drain解绑 |
| TIME-001 | P1 | Speaker timing | 100 Hz下八处 `pdMS_TO_TICKS(1)` 为0 tick，deadline轮询可压制低优先级UI/电源/OTA/control任务 | 开放；ceil/min1或事件等待，B/D contention与heartbeat测量 |
| WAIT-001 | P1 | Speaker reset | runtime reset ownership claim无deadline；内层超时又由void API吞掉，重连caller无法判断reset失败 | 开放；返回错误、共享总deadline、fault interlock和并发reset测试 |
| ERR-001 | P1 | Power control init | GPIO/power latch/SW3 task失败无法经void init传播，app仍继续显示/联网 | 开放；结构化health和逐点失败安全启动验证 |
| MEM-001 | P1 | P3 AFE buffer | AFE chunksize未验证正值和<=256；可形成除零、负值长度或固定栈buffer越界 | 开放；严格范围/乘法检查，B stub异常值与C实际library契约 |
| MEM-002 | P1（历史） | NXFP parser | 原未对齐typed load、无尾界parser和signed scanline overflow路径已替换为byte reader、cursor/end边界及int64计算 | A级静态修复；B sanitizer/fuzz、C build/link、D UI渲染仍开放 |
| MEM-003 | P2 | Audio frame ABI | PCM sample_count乘法和byte payload到`int16_t *`的对齐/effective-type契约未封闭 | 开放；边界乘法、byte reader/明确zero-copy契约与fuzz |
| MEM-004 | P2 | 未链接reliable audio原型 | playback负长度/NULL可形成巨大memcpy；capture/playback均未进入当前固件 | 开放；删除/归档或链接前修复并做sanitizer/并发测试 |
| MEM-005 | P2 | UI状态枚举 | drawer以未统一验证的wifi/cloud/power枚举直接索引数组 | 开放；发布边界normalize、COUNT sentinel和全域property test |
| START-001 | P1 | 启动/display | display init失败后仍进入LVGL/app shell并可能发布UI ready，后续flush使用未初始化显示 | 开放；mandatory dependency gate与display/LVGL逐点失败注入 |
| START-002 | P1 | 启动/readiness | 普通启动无带revision的统一能力快照，云循环可在本地关键能力缺失时继续 | 开放；统一capability/product-ready contract并复用OTA health gate |
| OFFLINE-001 | P1 | 本地能力/网络解耦 | wake、speaker/Wi-Fi worker和本地控制主循环排在首次Wi-Fi成功之后；WS断开又显式stop wake，云离线时只有部分UI/touch/SW3仍运行 | 开放；建立网络前local runtime owner、revisioned capability snapshot和独立offline sleep/wake策略 |
| OFFLINE-002 | P1 | 离线本地命令 | shell action仅由在线audio loop消费，断线不清且payload无时间/epoch/generation；重连后可迟到执行listen/volume/Wi-Fi/meeting动作 | 开放；常驻local command owner，按policy立即执行/拒绝/可取消等待；持续采集enable禁止跨断线重放 |
| POWER-001 | P1 | SW3关机 | power-key task只强制PA/背光并释放PG2，未停止/join音频、模式、UI、网络或普通NVS写入 | 开放；系统级有界quiesce coordinator与C/D关机故障/波形验收 |
| POWER-002 | P2 | bootloader/application电源契约 | bootloader硬编码SW3 GPIO16/active-low和hold-high，application Kconfig允许独立改pin/极性/模式；当前defaults匹配但无构建断言 | 开放；共享board power contract和bootloader/app一致性门禁 |
| POWER-003 | P2 | ROM下载/有线恢复供电 | ROM joint download在二级bootloader前接管，自定义GPIO7 early hold不执行；若USB不能独立保电，下载全程必须按住SW3或使用稳定治具，当前无冻结SOP/波形/连续恢复证据 | 开放；明确唯一供电拓扑和允许松开时点，完成连续下载及中途掉电恢复矩阵 |
| CROSS-001 | P1 | Conversation/P5 | P5 worker无同步读写main owner的完整 `s_prelisten`，可形成数据竞争和跨generation取消 | 开放；typed command/revision handoff和状态交错测试 |
| TIME-002 | P2 | SW3 timing | Kconfig允许5~9 ms poll，在100 Hz下变0 tick并错误累计debounce时间；当前20 ms安全 | 开放；提高range或ceil+真实elapsed，配置/示波器验收 |
| NET-IO-001 | P2 | Test WebSocket | `ws://` 路径在设置socket timeout前同步DNS/blocking connect，无本地连接总deadline | 开放；统一resolver/connect deadline，量产WSS-only |
| ERR-002 | P2 | Wi-Fi fallback | fallback watcher启动被重复调用且两次均忽略task创建失败，能力可静默缺失 | 开放；单owner检查结果、capability health和allocation failure测试 |
| ERR-003 | P2 | P3 L3 error | realtime process返回被丢弃；NVS set/commit失败仍返回persist成功 | 开放；control-owner初始化、显式bypass health和最终持久化ACK |
| CROSS-002 | P2 | Vocat diagnostics | TX stop/tone跨任务仅用volatile bool，不建立C11 happens-before | 开放；atomic/notification+join，诊断stop/tone交错测试 |
| STATE-001 | P1 | 全局conversation状态 | authenticated cloud `set_state`可把canonical状态任意改为booting/Wi-Fi/WS/idle/sleeping/listening/thinking/speaking/error，但不执行停采集、awake/turn清理、wake启停或网络迁移副作用；该状态又参与sleep/wake/cancel决策 | 开放；移除通用远端状态写入口，改typed semantic command + owner校验的`current,event,next`迁移；B级遍历命令矩阵，D级验证MIC/wake/WS/UI/generation一致 |
| STATE-002 | P2 | 云连接三态 | `nuwax_ws_connect()`因401/403失败后仍调用`mark_cloud_auth_failed()`并无条件发布`transport_connected=true`，形成不存在的transport连接；当前因authenticated=false仍fail-closed | 开放；auth-failed latch与实时transport分离，B/D核对401/403、hello reject、UI和OTA gate |
| OBS-001 | P1 | 崩溃/复位可观测性 | panic为打印后0秒重启，活动配置/分区无core dump；RTC仅记录boot count和通用reset reason。panic/abort、音频安全重启、OTA/BLE预期重启没有统一fatal site、任务/epoch、image identity、reset intent或crash-loop恢复记录 | 开放；版本化RTC breadcrumb+受控持久ring、intent/site区分、隐私受控core dump和crash-loop safe mode；逐站点断串口/连续复位验证 |
| LOG-001 | P1 | 实时日志/背压 | WS RX可对每个ASR partial同步输出多条INFO；20 ms MIC partial/short可每帧输出两条WARN，uplink/AEC/speaker异常路径也缺统一速率预算 | 开放；实时任务只记有界telemetry，first+聚合/指数退避、每tag预算/丢弃计数；B/D量化日志B/s、p95/p99、队列与序号 |
| PRIV-001 | P1 | 会话与设备隐私 | 当前production候选默认INFO通过UART0和USB输出最长96字节的ASR partial/final、用户文本、助手回复及device command文本，并输出完整SSID/IP/UID/device ID/cloud URL；未发现密码/device secret明文日志 | 开放；production禁正文预览，集中redaction和identifier keyed hash，URL去path/query；B0-14完成同意/访问/保留/删除并做golden log/实机导出验证 |
| PRIV-002 | P1 | 持续收音同意 | 已认证云命令可直接开启会议/监控持续采集；监控无本地菜单/硬停止，两种模式只显示通用“收音中”，缺少每次本地同意、明确持久指示、撤销和审计 | 开放；阻止发布，需consent状态机、持续指示、本地hard-stop和授权/撤销测试 |
| PRIV-003 | P2 | 敏感易失内存 | 5.12s preroll及多份PCM副本、临时音频、BLE secret/SRP verifier、OTA signed URL/query未统一zero-before-free/clear | 开放；补全scrub契约和单测；若量产保留JTAG/core dump则升级P1 |
| PRIV-004 | P1 | 网关访问控制 | 测试网关默认绑定0.0.0.0，`/`和`/api/status`无认证返回含ASR text/raw、会话和调试路径的snapshot，redaction不覆盖普通text/textPreview字段 | 开放；未授权必须401/403，授权输出字段最小化并有golden test |
| PRIV-005 | P1 | 录音留存/删除 | 测试网关默认保存uplink、TTS、会议和监控WAV/metadata，无全局TTL、用户删除、崩溃残留、factory-reset/RMA清理；当前语料约1.49GB | 开放；默认关闭录音并实现TTL/删除/访问/导出/异常清理门禁 |
| PRIV-006 | P2 | MCP最小化 | `self.get_device_status`返回精确SSID/IP/free heap，缺字段级目的、角色和调用审计 | 开放；最小化默认字段，敏感网络信息仅显式诊断授权返回 |

## 7. 执行日志

### 2026-08-02 初始化

- 已创建本工作记录。
- 已确认当前仅执行只读审计。
- 下一项：B0-01 Git/工作区基线。

### B0-01 Git/工作区基线完成

- 工程：`C:\Users\Administrator\Documents\work\Nuwax AI Partner\Nuwax AI Partner 7.0\NuwaxAIPartner-v2`。
- Git 根：`C:\Users\Administrator\Documents\work\Nuwax AI Partner\Nuwax AI Partner 7.0`。
- 分支：`codex/smart-barge-in-v1`。
- HEAD：`8b7cf297460dbb4ffc8a8594a27a1143c199818e`。
- 上级仓库状态共有 134 个条目：63 个已跟踪变更、71 个未跟踪条目。
- `git status --short -- NuwaxAIPartner-v2` 返回整个目录 `?? ./`，不能从 Git 获得 v2 内部逐文件版本。
- 对应计划校验：阶段 0.1 的路径/分支/commit/状态已完成；阶段 0.6 的完整 hash manifest 升级为强制门禁。
- 证据等级：本机 Git/文件系统直接证据。

### B0-02 源码与 CMake 清单完成

- 活动第一方代码/脚本 357 个，`_archive` 历史代码/脚本 11 个，原始第一方合计 368 个；`lib/` 2209 个；`managed_components/` 939 个。
- `main/` 有 192 个活动 `.c`、193 个原始 `.c`；CMake 有 74 个公共条目、5 个条件 OTA 条目、70 个 UI 条目，共 149 个唯一条目。
- 149 个 CMake 条目全部存在，无重复。
- 43 个活动未链接 `.c` 中 35 个为 host 资产，8 个为非 host 文件；原始未链接 `.c` 为 44 个，额外 1 个是明确归档的 Goertzel 失败实现。
- 证据文件：`docs/review-2026-08-02/module-inventory.md`。
- 对应计划校验：计划 2.1、2.2、2.4 和阶段 0.2 已完成；模块调用图和 owner 将在阶段 1 继续。

### 计划最终缺漏复核完成

- 独立复核确认原计划在权限分级、P0 验证闭环、完整测试入口、工具能力预检、非 C 质量门、硬件资料来源、版本真值、关键 timing、持久化恢复、安全扫描、BLE 配网写 NVS、数值 SLO 和 fuzz/property 方面存在缺口。
- 已逐项合入计划 V1.3；计划现在明确 A/B/C/D 四级权限，并允许经单批批准、可回滚的非发布验证镜像关闭硬件型 P0。
- 已补充完整 host/gateway/protocol/fake-device/AEC/工作区测试入口；列出工具只是纳入范围，不代表已授权执行或已经通过。
- 复核校验：关键缺口均可在计划中定位，版本已更新为 V1.3，未改变业务源码或设备状态。

### B0-03 配置与条件构建矩阵完成

- 已分类 11 个 `sdkconfig*` 文件、3 套分区表入口、348 个主 Kconfig 符号、2 个 bootloader Kconfig 符号和 OTA 条件源。
- 产品候选组合冻结为 `sdkconfig.defaults;sdkconfig.product-ota.defaults`；当前 `sdkconfig` 只作为候选文件证据，尚未由干净构建证明可复现。
- 配置证据文件：`docs/review-2026-08-02/config-diff.md`，包含逐文件 SHA-256、支持/诊断/历史 profile 分类和构建矩阵。
- 已登记 CONFIG-001/002/003；阶段 0.3、0.7 的静态部分完成，实际 reconfigure/build 留待 C 级批准后的 B0-06。

### B0-04 分区、容量和已有产物只读审计完成

- 活动 `partitions.customwake.csv` 无重叠并覆盖 16 MB；`model` 4 MiB、两个 OTA 槽各 0x4C0000，当前模型和 app 均在容量内。
- 三套 CSV 的用途差异已实测解析；只有 `customwake` 与完整烧录集合兼容，旧 `wakeword` 模型槽会溢出，普通 `partitions.csv` 会发生地址覆盖风险。
- 工程内 build 是 7.0.21；外部 `C:\tmp\nuwax-v2-722-patch-s3` 为 7.0.22 最新候选，但未绑定当前 manifest/OTA manifest，且 OTA 最低电量与当前 sdkconfig 有 20%/30% 漂移。
- 证据：`docs/review-2026-08-02/partition-build-assets.md`。只读阶段完成；BUILD-001、PART-001~005 开放，不能宣称 OTA 已放行。

### B0-05 工具链与依赖只读审计完成

- ESP-IDF v6.0 commit、Python 3.13.12、Xtensa GCC 15.2.0、CMake 4.0.3、Ninja 1.12.1、esptool 5.3.0、Node 24.15.0、npm 11.12.1、Git 2.52.0、Component Manager 3.0.2 已记录。
- `dependencies.lock` SHA-256、registry 组件版本/hash 和本地 LVGL 覆盖关系已记录；本地 LVGL 需要独立 provenance。
- 证据：`docs/review-2026-08-02/toolchain-baseline.md`。只读阶段完成；未运行构建或 host test。

### B0-07 完整 source manifest 完成

- 按 `source-manifest-scope.md` 的显式排除规则从文件系统枚举 7,912 项，生成 `source-manifest.sha256`，约 1.03 MB。
- 计划 V1.4 补入支持资产范围后已重建；独立逐条复算结果：7,912 entries、BadFormat 0、Missing 0、HashMismatch 0、PathEscape 0、ExclusionViolations 0；manifest SHA-256 为 `4e4b177967d002d6277157eccf4adee1d8358ddbf0767fcdd0c82f2d475cf7a6`。
- V1.8计划更新后复核发现唯一mismatch是被纳入manifest的计划文件，业务源码/配置其余7,911项未漂移；更新计划条目后再次全量复算为7,912 entries、BadFormat/Missing/HashMismatch/PathEscape均0，当前manifest SHA-256为 `ade1a6755435869651cd277bef9d865b2fc59e4af61a738d40eee0f3c959f4e8`。
- 工作记录和 review evidence 目录被明确排除以避免自引用；源码/配置/资源变化后该批次必须重建。
- 2026-08-03 A2-02续接后使用 long-path冻结脚本重建为7,917项。第一次相对中间快照只有`docs/串口日志（截取部分）.md`发生1个hash变化；以其最新内容为基准立即第二次完整复算为0 missing、0 mismatch、0 extra、0 duplicate，manifest SHA-256为`9deb65edcd0f010b2320c7c149bcf06c24f37197b9b4c36a0ae5e7c7d811e6d3`。
- 根目录NTFS保留名`CON`（11,733 bytes，SHA-256 `6b3888b17443a59ab4ce3e3f520e701c0690374f22c684e9d5640ea8d54f5bc0`）通过`\\?\`路径纳入清单并登记`BASE-003`；未删除、未作为构建输入。

### B0-06 可复现构建命令只读审计完成

- 已冻结 ESP-IDF v6.0 路径、全新外部 build、独立 `SDKCONFIG`、双 overlay 和 `set-target esp32s3 -> build` 顺序。
- 直接调用 IDF Python 的 `idf.py` 而不加载 export 环境会因环境变量缺失失败；统一入口必须先加载 `C:\esp\v6.0\esp-idf\export.ps1`。
- README 中的 `menuconfig` 会允许不可追溯配置变化，评审批次禁止交互式 menuconfig；配置变更必须进入 overlay。
- 证据：`docs/review-2026-08-02/build-reproducibility.md`。实际 configure/build 尚未获 C 级批准，因此 B0-06 只能标记部分完成。

### B0-08 硬件资料与商家 V2 参考冻结完成

- 权威硬件目录恰有 8 个文件、7,902,121 bytes；文件名、大小、页数和 SHA-256 已记录。
- 已明确缺少 BOM/netlist/PnP、BQ27220/QMI8658A 原厂完整资料和电芯 CEDV/profile，不能由原理图或商家代码替代。
- 两套 `cyberkit-main` 不同：测试工程内版本激活 V2.0，根目录版本激活 V1.2 并含 AECVERIFY 改动；测试工程内版本冻结为商家 V2 权威参考。
- 商家 V2 manifest 共 1,221 项，全量复算 BadFormat/Missing/Mismatch/PathEscape 均为 0，manifest SHA-256 `0a2329d5cfcfb1cba8454bfacfd9a370a03bbd54526323d2344f7836b7996e80`。
- 证据：`docs/review-2026-08-02/hardware-evidence-manifest.md` 与 `vendor-v2-reference-manifest.sha256`。

### B0-09 版本真值表完成

- 源码默认版本 7.0.22、compiled sequence 7000022；启动日志和 BLE device-info 均读运行 app descriptor。
- hello 的协议 `version=1` 与 firmware version 已区分，但 release sequence 当前可能表达 anti-rollback floor 而非当前 binary identity，且缺 image SHA tuple。
- 正式交接 schema 未定义实际 firmware/OTA 字段；README 仍有 7.0.17/7000017。
- 证据：`docs/review-2026-08-02/version-truth-table.md`；VER-001~006 开放。

### B0-10 持久化与 app-only OTA 兼容基线完成

- 已列出 9 类应用/ESP NVS namespace、关键 keys、写入 owner、局部 schema 和恢复逻辑。
- 云 active/pending、reprovision rollback 和 OTA transaction v3 存在实质事务/迁移设计；不是空实现。
- 启动时全量 NVS erase、明文凭据、无统一 factory reset、无寿命预算和 app-only partition/model 弱兼容仍是开放风险。
- 证据：`docs/review-2026-08-02/persistence-schema-and-ota-compat.md`；NVS-001~005 开放。

### B0-11 测试能力与敏感资产预检完成

- 16 个 PowerShell、24 个 Node.js、10 个活动第一方 Python 文件语法预检通过；未执行脚本逻辑或编译。
- `test/host/run_tests.sh` 存在但当前 PATH 无 Bash/WSL；cppcheck/clang-tidy/clang、ASan/UBSan、fuzz/fault injection harness 和 Kconfig lint 未就绪。
- 固定开发 PoP 非空；build-time Wi-Fi 密码为空，动态密码来自 NVS；测试签名私钥路径和 hash 已脱敏记录。
- 证据：`docs/review-2026-08-02/test-capability-precheck.md`；TESTCAP-001/002、SEC-001~003 开放。

### B0-12 工程外支持测试资产冻结完成

- `cloud-gateway` 的源码/测试/package lock/测试私钥与根 `tools` 的脚本/源码/静态 fixture 已纳入独立 manifest。
- 明确排除 gateway runtime/audio-debug、node_modules、日志/临时文件，以及 root tools 的 reports、缓存和可执行输出，避免把运行产物当作测试代码基线。
- 独立复算结果：188 entries、BadFormat/Missing/Mismatch/PathEscape 均为 0，manifest SHA-256 `15cc4491154c111f1b600457eaad41cbf7b39ef55588760631759bf3849a93df`。
- 证据：`docs/review-2026-08-02/supporting-test-assets-scope.md` 与 `supporting-test-assets-manifest.sha256`。

### 计划 V1.5 完整性复核完成

- 在 V1.4 基础上再次执行“计划条款 -> 可枚举对象 -> 证据产物 -> 放行条件”反向核对。
- 新增四类此前仅被上位概念覆盖、但没有独立验收项的范围：软件 timer/`esp_timer`/`esp_event` 回调上下文，ESP-IDF 系统任务与应用任务的联合优先级图，内核对象有等待者时的销毁安全，以及模块 API/静态工作区的可重入性。
- 对应计划校验：阶段 1 新增第 14~17 项；工作记录新增 A1-01~A1-08 检查表。未修改业务源码、配置、构建产物或设备状态。

### A1-03 队列与对象生命周期审计启动

- 已从第一方产品/host 源文件枚举 queue、semaphore、event group、task notification 等 API 使用点 432 个，生命周期创建/删除类命中 145 个；host stub/check 与产品构建路径在证据中分开。
- 已初查 audio uplink、barge dispatcher、meeting/monitoring、wake/touch/speaker owner、OTA、BLE vox-config 和 face cache 队列的 owner、容量、满队列与停止语义。
- 新证据：`docs/review-2026-08-02/phase1-queue-and-lifecycle-audit.md`；原始枚举保存为 `phase1-queue-api-inventory.txt` 与 `phase1-lifecycle-api-inventory.txt`。
- 已确认两个需要关闭的 P1 静态风险：CONC-001 face cache 跨核共享状态无同步；CONC-002 BLE vox-config in-flight handler 可在 owner 停止/queue 销毁时无限等待或竞态。
- 对应计划校验：阶段 1.4、1.16 已有可追溯入口，但 A1-03 尚未完成；speaker、OTA、touch、Wi-Fi、flash-op、face cache 和模式队列仍需逐对象收口。

### WebSocket/音频并发边界静态审计完成（阶段 1 子项）

- 已沿 `WS RX callback -> speaker ingress -> manager lock -> output/I2S` 和 `TX -> lifecycle lock -> I/O lock -> socket retry` 两条链路核对锁持有与回调执行上下文。
- 确认 NET-CONC-001/002、AUDIO-CONC-001/002、NET-CONC-003；当前没有把风险写成已复现死锁，阻塞频率和最坏时延保留给 B/D 级故障注入。
- `audio_uplink` 停止未确认时不会直接释放 live queue/task/pool，静态代码未发现直接 queue UAF；每轮动态分配仍需 1000 次短会话 heap/fragmentation 验证。
- 证据：`docs/review-2026-08-02/phase1-ws-audio-concurrency-audit.md`。
- 对应计划校验：阶段 1.3/1.4/1.6/1.7/1.11/1.17 已有代码证据，后续阶段 4/5/8/9/10 已有明确关闭用例。

### A1-02 锁图与无限等待白名单完成

- 已核对 LVGL、face cache、speaker manager/output、P3 software reference、Wi-Fi connect、BLE vox-config、wake detector、display SPI 和 WebSocket stop 等主路径锁/独占资源。
- 条件白名单仅表示当前静态调用链未发现锁反转；speaker/output/P3 长持有时间、SPI 独占和业务 worker heartbeat 仍需运行时证据。
- 确认 RTOS-005 wake stop 无 deadline、RTOS-006 face cache 跨核无同步、RTOS-007 BLE job stop/delete 缺 join 三项 P1。
- 编号复核：A1-03 初查中的 `CONC-001/002` 与 RTOS-006/007 是同一根因，已统一为 RTOS 编号，避免重复统计。
- 证据：`docs/review-2026-08-02/phase1-architecture-rtos-audit.md` 第 6~8 节。
- 对应计划校验：阶段 1.3、1.7、1.11、1.12 已完成 A 级静态闭环；动态锁时延和故障恢复留给后续授权阶段。

### A1-03 队列、通知与对象生命周期静态审计完成

- 正式 CMake 路径已逐对象核对 24 组队列/slot、1 个 Wi-Fi event group 及全部产品任务通知；无正式 ring/stream/message buffer。队列 API 原始枚举、任务生命周期枚举和当前回调/等待检索分别保存于 `docs/review-2026-08-02/phase1-queue-api-inventory.txt`、`phase1-lifecycle-api-inventory.txt`、`phase1-task-lifecycle-rg.txt`、`phase1-queue-rg-current.txt`。
- 静态确认的 ownership/回滚相对完整对象：audio uplink、实时采集、OTA command item；其余对象的运行时高水位、停止竞态和长稳仍需 B/D 级验证，不能写成设备通过。
- 新增/确认问题：`RTOS-008`（`app_main` 线性启动失败无统一反向清理，可能留下已创建 task/queue/controller）；`QUEUE-001`（Wi-Fi latest report 的 `peek -> 网络发送 -> receive` 可删除后来 overwrite 的最新报告）；`QUEUE-002`（meeting ACK 深度 1 无条件 overwrite，陈旧 ACK 可覆盖有效 ACK）；`RTOS-009`（Wi-Fi 初始化中途失败不注销 handler/删除 event group/netif/driver，重试可能重复注册）；`RTOS-006`（face cache worker/LVGL 跨核共享及 preload worker 失败回滚）；`RTOS-007`（BLE vox-config handler 缺引用计数/join）；`LIFE-001`（barge dispatcher 部分初始化失败未回收前置对象）；`LIFE-002`（vocat 诊断 TX 等待 1500 ms 超时后，临时 task 仍可能访问调用者栈上的 `tx_ctx`）；`LIFE-003`（app 启动中创建的 `s_touch_trigger`/`s_touch_events` 无生产者/消费者）。face/BLE 均统一沿用 RTOS-006/007，不再重复登记 RTOS-010/011。
- 证据：`docs/review-2026-08-02/phase1-queue-and-lifecycle-audit.md` 第 7~10 节，已包含对象容量、payload 所有权、满/空语义、停止/销毁顺序、启动回滚专项和动态验证边界。
- 对应计划校验：阶段 1.4、1.16 的 A 级静态对象清单、所有权和销毁入口已完成；A1-03 状态更新为“已完成（A 级静态）”。B/D 级队列压力、handler 退出时间、WS epoch 并发、face buffer 覆盖和诊断超时注入仍按计划保留。

### A1-04 Worker 启停与迟到访问静态审计完成

- 两轮反向检索全部正式 CMake 任务创建点：23 个直接 `xTaskCreate*` 位置映射到 28 个具名 worker，0 个未映射；补入此前遗漏的 `p1_touch_diag`。worker 按永久常驻、连接级、音频流级、按需唤醒和诊断临时任务分类，逐项核对 task handle、stack/TCB owner、stop flag、退出确认、self-delete/owner delete 和失败回滚。
- 静态较完整链路：WebSocket RX、audio uplink、rt capture、battery、LED、board inputs 均有退出确认后再 delete 的主路径；其中实际 I/O 阻塞时能否在 deadline 内退出仍待 B/D 验证。
- 确认 RTOS-004/005、LIFE-001/002/004/005/006/007/008/009/010；新增 `LIFE-009`（WS RX 创建后启动发布竞态）与 `LIFE-010`（OTA controller 部分初始化无完整反向回滚）。同时纠正 `phase1-architecture-rtos-audit.md` 中对 touch task 的旧描述：当前 `touch_input.c` 没有 deinit/stop API，不能写成“stopped 后 owner delete”。
- 证据：`docs/review-2026-08-02/phase1-worker-lifecycle-audit.md` 与原始任务删除检索 `phase1-worker-delete-rg-current.txt`。
- 对应计划校验：阶段 1.5、1.16 的 A 级 static stack/TCB、self-delete、owner reap 和迟到访问清单已完成；A1-04 标记“已完成（A 级静态）”。运行时退出时延、PSRAM stack/cache-disabled 安全和并发 stop 仍按阶段 3/6/8/10 验证。

### A1-05 Callback、Timer、Event、ISR 与 Cache-safe 静态审计完成

- 以正式 CMake 链接路径反向枚举 `esp_timer`、LVGL timer/event、`esp_event`、provisioning callback、GPIO ISR、WebSocket callback、音频/电源/触摸 worker callback 及 DMA/cache-disabled 边界；逐项记录执行上下文、payload/user_ctx 生命周期、解绑顺序、epoch/generation 和内存能力。
- 确认并登记 `EVENT-002`（Wi-Fi event loop/fallback/app 对普通状态无同步）、`EVENT-003`（provisioning `s_started` 同步缺口）、`WS-CB-001`（64 位 WebSocket activity timestamp 使用 volatile 跨核共享）、`WS-CB-002`（meeting/monitoring RX 预判读取 owner 状态）、`TIMER-002`（两个 LVGL timer 创建后未判空）、`CACHE-001`（普通运行期 NVS/Flash 与音频/cache-disabled 无统一仲裁）和 `ISR-001`（touch GPIO service flags=0 不形成 IRAM-safe 契约）。`EVENT-001` 是 `RTOS-009` 的 callback 侧补充证据，不重复计数；`TIMER-001` 是 `LIFE-004` 的同根因别名。
- 未发现新的直接 P0；WebSocket、touch、wake、battery、board input 的 payload/UAF 主路径静态顺序尚可，但真实 callback drain、NVS 最坏时延、I2S gap、cache-disabled 触摸和跨核交错仍必须在 B/D 级验证，不能写成产品通过。
- 证据：`docs/review-2026-08-02/phase1-callback-isr-audit.md`，以及 `phase1-callback-api-inventory.txt`、`phase1-callback-timer-rg-current.txt`、`phase1-crosscore-sync-rg-current.txt`。对应计划校验：阶段 1.6、1.9、1.14、1.19 的 A 级静态检查完成；A1-05 标记“已完成（A级静态）”。

### 计划 V1.6 缺漏复核完成

- 对阶段 1~10 的“检查项 -> 证据 -> 放行门”再次反向核对，新增四个独立门禁：ISR/IRAM/DMA/cache-disabled 安全；`ESP_ERROR_CHECK`/assert/abort/restart 致命路径；reset/core-dump/日志速率与隐私观测；生产诊断音频、USB Serial/JTAG 和 reprovision 入口的授权边界。
- 这些项目不能仅由普通错误传播、硬件驱动或安全章节的概括替代，已写入计划 V1.6 的阶段 1、2、3 和 6，且有对应 B/D 级验证条件。
- 未修改业务源码、配置、构建产物或设备状态；计划版本与本记录引用已同步。

### 计划 V1.7 执行映射复核完成

- 再次按“计划检查项 -> 工作项 -> 证据文件 -> 关闭条件”反查时确认：V1.6 阶段 1.18、1.20 已在计划正文提出致命复位和可观测性要求，但执行检查表没有独立 owner；阶段 1.19 也未被 A1-05 显式引用。这会造成计划看似完整、执行时漏交证据。
- 已将 A1-05 映射扩展到阶段 1.19，并新增 A1-09，专门交付 `ESP_ERROR_CHECK`/assert/abort/restart 清单和 reset/core-dump/heartbeat/日志矩阵；阶段 1 输出同步增加这些证据。计划升为 V1.7。
- 反向校验：阶段 1.1~1.20 现均有 A1-01~A1-09 的唯一执行入口；A1-09 完成前阶段 1 不得收口。此次只修改评审文档，未修改业务源码、未构建、未运行测试、未操作 COM5/OTA。

### 计划 V1.8 最终缺漏复核与基线重建完成

- 独立按“范围 -> 唯一工作项 -> 证据产物 -> 验收/放行门”反查，确认V1.7仍遗漏八类独立交付：SBOM/license/CVE、隐私/录音留存、第一方memory-safety/UB、生成资产provenance、V1->V2功能等价、需求/测试覆盖、威胁模型/密钥生命周期和全阶段唯一工作项索引。
- 已将其写入计划V1.8的阶段0工作项/产物、阶段8.3、阶段10 run_id和放行门；同时把量产OTA manifest签名从“建议”改为强制认证要求。
- 新增B0-13~B0-20执行入口。该段记录的是V1.8建项时状态：当时除B0-19启动外其余均待执行；B0-13~17随后完成，B0-18/B0-20的最终收口状态见本记录后续条目。
- source manifest因计划文件变化曾出现唯一mismatch；更新计划hash后全量7,912项复算0 mismatch/0 missing/0 path escape，业务源码/配置没有随文档复核漂移。

### A1-06 循环、时间、错误传播与跨核静态审计完成

- 对37个无条件循环逐项分类；91个字面量和61个变量tick转换完成核对；324个显式discard候选以产品/host和调用后果分层审计。
- 新增P1：`TIME-001`（speaker八处1ms在100Hz退化为0 tick）、`WAIT-001`（runtime reset claim无deadline/void吞错）、`ERR-001`（电源初始化失败不可传播）、`MEM-001`（AFE chunksize除零/越界边界）、`CROSS-001`（P5 worker无同步访问prelisten）。
- 新增P2：`TIME-002`、`NET-IO-001`、`ERR-002/003`、`CROSS-002`；已有WS/EVENT/RTOS/API问题只交叉引用，不重复计数。
- 证据：`docs/review-2026-08-02/phase1-time-error-crosscore-audit.md`。反向校验覆盖计划1.7~1.13；本轮未构建、未运行测试、未触碰设备。

### A1-07 公开API可重入性差异复核完成

- 初稿5项问题经独立差异复核后发现遗漏：speaker重复init、P3 software ref首次建锁/重配置、meeting/monitoring live object init、wake首次init和LVGL callback/context发布。
- 修正版登记 `API-001~010`：P1为`API-001/002/006`，其余为P2；barge并发首次init扩充既有`LIFE-001`，不重复编号。
- 已修正原稿中“P3 software ref/speaker未发现新增问题”的错误正向结论，并补充动态关闭矩阵与计划1.17反向校验。
- 证据：`docs/review-2026-08-02/phase1-api-reentrancy-audit.md`。A1-07现标记A级静态完成，B/D交错和资源守恒测试仍开放。

### B0-19 全阶段唯一工作项索引完成

- 已创建 `docs/review-2026-08-02/review-work-item-index.csv`，按计划 V1.8 将阶段0的B0-01~20、阶段1的A1-01~09和阶段2~10的A2/A3/A4/A5/A6/A7/A8/A9/A10工作项全部分配稳定ID。
- 索引共125行、125个唯一ID、0重复、0缺字段；阶段分母为B0 20项、A1 9项、A2 9项、A3 16项、A4 16项、A5 9项、A6 14项、A7 8项、A8 10项、A9 6项、A10 8项。
- 每行均包含源码/产品范围、A/B/C/D授权、方法、唯一预期证据、验收条件、问题ID、reverse-check、状态和run_id字段；A10-01~08均标记执行时必须分配run_id，未执行项没有伪造运行编号。
- CSV以PowerShell `Import-Csv`反向解析通过，UTF-8 replacement字符为0；完整ID序列复算Expected 125/Actual 125/Missing 0/Extra 0。A1-08完成状态同步后当前SHA-256为 `ef7b02af13bfc8992fdadcc3d627b47b0c52a7eed82aa1a28a225c4d1d670e75`。
- 对应计划校验：阶段0.19和阶段10的唯一工作项/evidence/run门禁已建立；这只关闭索引交付，不代表索引中的功能已通过，也不扩大B0-06、B0-13~18或B0-20的授权。本条记录时B0-13~15仅完成A级静态证据，其B/C/D门禁仍开放；后续状态以索引和本记录后续条目为准。

### A1-08 综合拓扑、状态与风险审计完成

- 已创建 `docs/review-2026-08-02/phase1-consolidated-topology-state-risk-audit.md`，综合A1-01~07并重新落实全部正式状态写入点，而非仅复制enum。
- 已形成任务/owner/context图、锁顺序与无锁共享图、queue/slot/payload/memory图；app、awake、prelisten、turn guard、Wi-Fi、WS lifecycle、cloud transport/service/auth、meeting、monitoring、OTA和shutdown gate均有owner、触发、合法边、副作用与恢复表。
- 新确认`STATE-001`（P1）：远端通用`set_state`绕过semantic transition副作用，却改变参与sleep/wake/cancel判断的canonical状态；新确认`STATE-002`（P2）：401/403连接失败路径短暂发布不存在的transport连接。
- 阶段1 canonical问题独立复算为P0 0项、P1 31项，31个ID唯一且0遗漏/0重复；别名`EVENT-001 -> RTOS-009`、`TIMER-001 -> LIFE-004`、旧`CONC-001/002 -> RTOS-006/007`不重复计数。
- 文档共360行、UTF-8 replacement字符0、四组Mermaid围栏完整；P1表Expected 31/Actual 31/Missing 0/Extra 0，SHA-256为 `0aa19ecade00d64c71d98da1540521032e75c55340e60c34d8d2eae6b4f1d692`。
- 对应计划反查：1.1~1.17和1.19均已有综合入口；1.18/1.20明确仍由A1-09负责。A1-08标记A级静态完成，但B/C/D关闭矩阵全部保持开放，阶段1仍为进行中。

### A1-09 致命退出、复位与可观测性静态审计完成

- 以149个正式CMake源和1个bootloader源为分母，精确复算：硬`ESP_ERROR_CHECK` 3个、`ESP_ERROR_CHECK_WITHOUT_ABORT` 32个、`abort()` 3个、`esp_restart()` 4个、正式`assert/configASSERT` 0个；host断言未混入产品结论。
- 已逐站点区分NVS/OTA启动panic、未提交uplink无法安全unwind的fail-closed abort、audio/capture worker停止失败的安全重启，以及BLE重配网/OTA staged的预期产品重启；NVS全擦、OTA init回滚和worker heartbeat分别沿用`NVS-001/LIFE-010/RTOS-001`，不重复编号。
- 新确认3个P1：`OBS-001`（无fatal site/core dump/reset intent/crash-loop恢复记录）、`LOG-001`（WS RX及20 ms capture/uplink/AEC/speaker热路径无统一日志速率预算）、`PRIV-001`（默认INFO经UART/USB输出ASR partial/final、用户/助手正文预览和稳定设备/网络标识）。正式日志未发现密码/device secret/token明文，敏感hello缓冲有显式清零；这是正向边界，不抵消会话内容泄露。
- 可观测性矩阵已覆盖reset reason、RTC、panic/backtrace/GDB、core dump、Task/Interrupt WDT、业务progress watchdog、epoch/序号、heap/stack、firmware identity、日志等级/出口/速率、敏感字段和留存预算。阶段1当前独立复算为直接P0 0个、canonical P1 34个。
- 证据：`docs/review-2026-08-02/phase1-fatal-reset-observability-audit.md`，156行、UTF-8 replacement字符0、SHA-256 `ea5b3011d5a4cfa54f216e3aee990ec8e347d98340bd99ffdfe38d6f2798d35b`。
- `review-work-item-index.csv`中的A1-09已更新为`completed-A;B/D-open`，索引仍为125行/125个唯一ID，当前SHA-256 `82606d1abc7d431bb2cb1403a7df8a41d99f5ca7da61a2b31a92bb57c5c0509d`。
- 计划反查：1.18/1.20的A级交付闭合，A1-01~09全部有唯一证据入口；阶段1只允许标记“**A级静态审计完成，B/C/D开放**”，不代表产品稳定性、隐私或量产发布通过。本轮未修改业务源码/配置，未构建，未运行测试，未操作COM5/NVS/OTA。

### B0-13 SBOM、许可证、CVE/VEX 与第三方来源静态审计完成

- 已核对 `dependencies.lock`、`main/idf_component.yml`、7个 `managed_components`、根 `lib/` 的 LVGL/CST816S/Arduino GFX、31个 license/notice 文件、`THIRD_PARTY_NOTICES.md` 和现有 linker-map 候选。
- `dependencies.lock` 固定8个依赖节点，但本地 `lib/lvgl` 通过 `EXTRA_COMPONENT_DIRS` 覆盖关系不能由registry hash证明；实际链接成员仍需C级干净构建的map/ELF/component graph确认。
- 未发现产品级SPDX/CycloneDX SBOM、CVE inventory或VEX；唯一 `sbom_cJSON.yml` 只是单组件元数据。`THIRD_PARTY_NOTICES.md`仍是计划性清单，包含未锁定的计划`bmi270`，且未汇总实际组件许可证。
- 新增 `SBOM-001`、`LIC-001`、`LIC-002`、`CVE-001` 四个P1发布阻塞问题；`DEP-001`继续交叉引用，不重复编号。
- 证据：`docs/review-2026-08-02/phase0-b013-sbom-license-cve-audit.md`。B0-13标记A级完成、B/C/D开放、release blocked；未构建、未运行扫描或测试、未接触设备。

### B0-14 隐私、音频数据流、同意、访问、保留与删除静态审计完成

- 已建立唤醒/正常对话/会议/监控/BLE/OTA/MCP/网关/第三方ASR-TTS的数据流和落点矩阵。设备普通会话主路径未发现写flash/SD WAV，但5.12s preroll、多份PCM副本、UART/USB正文预览、网关snapshot与网关录音均是敏感数据路径。
- 扩展既有 `PRIV-001`，并新增 `PRIV-002`（远程持续收音缺本地同意/持久指示/hard-stop）、`PRIV-003`（RAM敏感数据未统一scrub）、`PRIV-004`（网关status无认证）、`PRIV-005`（默认录音且无TTL/删除）和`PRIV-006`（MCP状态过度披露）。
- 测试网关当前 `runtime-logs` 静态盘点为6,945个文件、约1,495,494,945 bytes，其中5,091个WAV、约1,316,703,146 bytes；这证明默认录音与缺少保留门禁已经形成实际数据累积。
- 证据：`docs/review-2026-08-02/phase0-b014-privacy-data-flow-retention.md`。B0-14标记A级完成、B/C/D开放、release blocked；未更改网关/设备配置，未运行测试，未访问COM5/NVS/OTA。

### B0-15 生成代码/资源 provenance 静态审计完成

- 以正式 `assets/` 和 `main/CMakeLists.txt` 为分母，确认41个生成输出、共9,203,244 bytes：36个图像C array，以及字体、face diff、face path的5个C/H/BIN输出；另6个聚合C/H按手写文件分离。
- 5/5 face、6/6 dialog、13/13 status与sibling V28 handoff C逐字节一致；12/12 menu与handoff不同但与V1一致。当前CMake只编译3/5 face，另外两个显式保留为未链接资产。
- 更正“生成器不存在”的初步判断：menu和face-diff生成器位于工作区根并已进入supporting-test manifest，但不属于V2发布树；menu输出路径硬编码V1。face/path/icon输入位于工程外，dialog/status/face的确切converter和命令缺失。
- 字体输入与Source Han OFL 1.1文本存在，但输出命令仍指向V1且`lv_font_conv`版本未固定；face-diff生成器嵌入wall-clock时间并缺历史`--current-dir`绑定，当前没有byte-for-byte重生成门禁。
- 新增`GEN-001~004`四个P1发布阻塞问题；自定义资产ownership/license缺口交叉引用`LIC-001`，不重复另立license编号。
- 证据：`docs/review-2026-08-02/generated-artifact-provenance.md`。B0-15标记A级完成、B/C/D开放、release blocked；未执行generator/host test/build，未修改业务源码/配置/资产，未访问COM5/NVS/OTA。

### B0-16 V1 -> V2 产品功能等价静态审计完成

- 已建立当前文件系统 V1/V2 的源码分母：V1 `main/` 有260个 `.c/.h`，V2有334个；260个同路径文件中177个字节相同、83个改变，V1-only为0。正式CMake入口由126个增至149个，新增23个正式源；这些统计只证明迁移覆盖面，不构成功能回归证据。
- 已按正常对话、AEC/打断、会议、监控、BLE/USB配网、OTA、UI、唤醒、电源/电量/板级驱动和云协议十类功能逐项标记保留、替换、子项删除或待决策，并给出A/B/C/D证据入口。
- 确认`PARITY-001`：V1当前有49个tracked修改、`+4608/-525`，且无独立source/config/image identity，不能作为唯一可复现比较基线。
- 确认`PARITY-002`：V2监控已变为人声触发、500 ms preroll和180秒静音分流，却仍声明`monitoring_mode_v1`；这与冻结的“启用后连续上传全部音频”契约冲突，是发布阻塞项。
- 确认`PARITY-003`：正常对话/AEC/打断的backend、上行批量、owner、terminal END和guard均已替换；静态代码不能证明不同音量下零自打断、首音节完整或双讲声学质量。
- 确认`PARITY-004`：唤醒命令仍为`ni hao ling zhu`，但阈值由30%降为20%，并新增远端配置和NVS持久化，必须通过冻结语料矩阵量化误/漏唤醒。
- 证据：`docs/review-2026-08-02/v1-v2-product-feature-parity.md`。主审复算tracked diff、阈值、监控状态机和协议声明均与文档一致；B0-16标记`completed-A;B/C/D-open;release-blocked`，未运行测试、未构建、未操作设备或OTA。

### B0-17 需求、状态、错误路径与测试覆盖矩阵静态审计完成

- 已冻结 OTA 产品 app 源表 149、非 OTA 144、bootloader 第一方源 1、JSON compatibility component 源 1；149 个 app 源中 25 个被 host 直接编译、27 个被源码文本契约读取，去重并集 44（29.5%），105 个无直接 host 关联。该比例不是行或分支覆盖率。
- 已清点35个 `*_host_check.c`：30个进入PowerShell runner，5个关键AEC/打断检查仍为孤立入口；gateway有29个`test-*.js`，npm scripts只注册14个，默认`npm test`仅执行3个。
- 当前没有稳定的规范性需求ID，状态迁移和错误路径也尚未语义去重，因此不能报告需求、状态或错误路径覆盖百分比。已登记`COV-001~007`及其关闭条件。
- 149个正式C源的raw-source错误候选复算为：674个直接`return ESP_FAIL/ESP_ERR_*`、148个`goto target;`、35个`ESP_ERROR_CHECK*`、697个`!= ESP_OK`/`== ESP_ERR_*`词法比较、106个`ESP_ERR_TIMEOUT` token、7个`abort/esp_restart`调用；这些类别高度重叠，不等于独立错误路径。
- 当前V2原始声学样本为0；manifest中的26条音频均缺失且属于V1.2/COM8历史证据，不能关闭V2 AEC/打断风险。
- 主审逐项复算149个正式源和六类错误候选，并验证106个带行号代码引用全部存在且未越界；文档317行、UTF-8 replacement字符0、SHA-256为`f6a66b3efd075dacf5a4e49d5bf5c90f694eb3c902a802e00b6d6ad69f055c96`。
- 证据：`docs/review-2026-08-02/requirements-and-test-coverage-matrix.md`。B0-17标记`completed-A;B/C/D-open;release-blocked`；未运行host/gateway测试、未构建、未操作设备或OTA。

### B0-15 生成资产 provenance 交接复核通过

- 对 `generated-artifact-provenance.md` 与当前文件系统重新复算：`assets/` 仍为47个C/H/BIN、9,223,106 bytes；排除6个手写聚合文件后仍为41个生成输出、9,203,244 bytes。
- 按文档规定的`path|bytes|lowercase_sha256\n`算法，以工程根相对路径排序后的47项集合SHA-256仍为`06770B18D09F7A4712999CE430ECBB7BF3155DD174EB1DA52F7A1372B837201F`，证明本次续接前后资源集合无漂移。
- B0-15继续保持`completed-A;B/C/D-open;release-blocked`；本轮未运行generator、未修改资产，也未把静态hash一致误写成可复现生成通过。

### B0-18 威胁模型、量产安全 profile 与密钥生命周期 A 级静态审计完成

- 已逐项复核 `security-threat-model.md`、`release-security-profile.md` 和 `key-lifecycle.md` 的源码/配置引用。确认当前产品OTA overlay仍继承固定开发PoP、明文WS、USB控制和调试/INFO日志，Secure Boot、Flash/NVS Encryption及bootloader anti-rollback均未启用。
- 跟踪6个P1发布阻塞ID：`CONFIG-004`、`SEC-004`、`SEC-005`、`SEC-006`、`SEC-007`、`KEY-001`。其中 `SEC-006` 的跨重连OTA queue原始缺陷已由connection epoch在接收、排队、reservation和controller acceptance各层校验而A级静态修复；`SEC-007` 已改为每次从eFuse/factory MAC派生UID，NVS只作可修复cache，原始身份替换路径也已A级静态修复。二者仍需B/D交错、tamper和迁移验证，不能标成完全关闭。
- 仍开放的量产阻塞包括：OTA manifest独立签名/key ID、云配置签名/version floor、Secure Boot/Flash/NVS Encryption/anti-rollback、密钥轮换/吊销/RMA和发布审批链。
- B0-18已更新为`completed-A;B/C/D-open;release-blocked`。这是静态威胁/profile/key基线完成，不表示量产安全已实现；未执行secret scanner、构建、攻击测试、eFuse、NVS、COM5、OTA或烧录。

### B0-20 第一方 memory-safety/UB A 级静态审计完成

- `first-party-memory-safety-file-matrix.csv`与当前文件系统反向比对为349行/349个唯一路径，missing/extra/duplicate均为0。覆盖分层复算为deep 37、targeted 82、pattern 44、header 145、generated 41；所有记录行号也已刷新到同一静态快照。
- 跟踪`MEM-001~005`：AFE chunksize边界、NXFP parser历史问题、audio frame乘法/typed payload契约、未链接reliable原型的负长度/NULL缺口、UI枚举直接索引。`MEM-002`原未对齐/无尾界/signed overflow路径已由bounded byte parser和int64计算A级静态修复；`MEM-004`明确不进入当前固件；既有UAF/并发/cache风险继续沿用原ID。
- B0-20已更新为`completed-A;B/C/D-open;release-blocked`；同一证据对应的A8-08更新为`completed-A`。未运行ASan/UBSan/fuzz、未构建、未执行目标机DMA/cache/stack/heap验证。

### A2-01 app_main 与主任务启动依赖顺序 A 级静态审计完成

- 已复核 `phase2-startup-power-recovery-audit.md` 的26步启动ledger、failure cut、owner残留和代码引用。确认`app_main()`不是可回滚启动事务，显示失败仍无条件进入LVGL/app shell且可产生`s_ui_ready=true`，普通启动也没有mandatory/optional依赖表和带revision的能力快照。
- 新增`START-001`、`START-002`两个P1；`RTOS-008`及电源/NVS/OTA/LVGL/Wi-Fi/BLE既有问题只交叉引用。A2-01状态更新为`completed-A;B/C/D-open`，但其产品验收“依赖失败停止真实下游并进入确定状态”未通过。
- A2-01完成不代表阶段2收口；A2-02的结果见下一条，A2-03~A2-09仍开放。

### A2-02 SW3、电源保持与关机顺序 A 级静态审计完成

- 当前默认PG2 GPIO7 high、SW3 GPIO16 active-low、20 ms poll、60 ms debounce；100 Hz下默认poll为2 tick。状态机先等待启动按键释放，再接受稳定按下和释放；关机发生在释放沿，不区分短按/长按。
- Kconfig允许5~9 ms poll，在100 Hz下会变0 tick却仍累计nominal debounce，复用`TIME-002`；`nuwax_power_ctrl_init()`错误不可传播继续复用`ERR-001`。
- 新增`POWER-001`（P1）：SW3只claim gate、配置EXT1、强制PA/背光、写marker并释放PG2，没有停止/join MIC/I2S/DMA/codec/speaker/mode/UI/network或普通NVS写；LVGL display keepalive和音频owner均存在并发路径。
- 新增`POWER-002`（P2）：bootloader硬编码SW3 GPIO16/active-low和hold-high，application允许Kconfig独立改pin/极性/模式；当前defaults匹配，但没有共享header或构建一致性断言。
- A2-02更新为`completed-A;B/C/D-open`，产品验收“PA/codec/DMA/LCD/radio安全停止”未通过。未运行按键模型、未构建、未访问COM5/设备、未测电平/波形/功耗。

### 三份交接材料状态断点已修复

- `review-work-item-index.csv`已同步B0-18、B0-20、A2-01、A2-02和A8-08；B0-07登记`BASE-003`，A2-02登记`POWER-001/002`。索引仍应保持125个唯一工作ID，最终hash在本轮一致性校验后记录。
- `generated-artifact-provenance.md`补充 parser 边界：`face_path_blob.c/.h`和host check是手写代码，不进入41项生成资产分母；47项assets集合、41项生成输出、bytes和集合digest均未改变。
- 因新增正式NXFP parser/host check及host-only `test/host/stubs/esp_err.h`，受影响分母已刷新：第一方C/H 349，正式app源150，host check 36，runner关联31；`module-inventory.md`、`requirements-and-test-coverage-matrix.md`、`v1-v2-product-feature-parity.md`和fatal审计已同步。历史日志中的149/345数字保留为当时快照，不再作为当前结论。
- 本轮只修改评审证据/索引/基线；没有修改业务源码、配置或资产，没有构建、运行测试或操作COM5/NVS/OTA。

### 本轮一致性校验完成

- 工作项索引：125行/125个唯一ID、0重复、0核心字段缺失，SHA-256 `2ad5dca8461f22772d66b60de23d430d3d235a3d4049074fa48b2daefa929e11`。
- memory逐文件矩阵：349行/349个唯一路径，与当前第一方活动C/H 349项完全一致，missing/extra/line-mismatch均为0；覆盖分层为deep 37、targeted 82、pattern 44、header 145、generated 41，SHA-256 `7a6d68934ea337e341705584ba8240ec5f21f02446df99db977aa6b7053e42c2`。
- 生成资产复算：assets 47项/9,223,106 bytes；排除6项手写聚合后生成输出41项/9,203,244 bytes；集合digest仍为`06770B18D09F7A4712999CE430ECBB7BF3155DD174EB1DA52F7A1372B837201F`。
- 源码manifest最后一次全量对比保持7,917项、0 missing/mismatch/extra/duplicate，SHA-256 `9deb65edcd0f010b2320c7c149bcf06c24f37197b9b4c36a0ae5e7c7d811e6d3`。
- `review-baseline.json`可解析；评审目录关键MD/CSV/JSON/PS1/SHA256文件UTF-8 replacement字符为0。关键证据hash：memory审计`42ecf6070dde4e6785b6c93d17015295ab52b79b9e2ec7ad9b7a3903eb870032`，A2启动/电源审计`6c1da510edc0feefee0a685d82ac65c31dc0f69442ca8e959f1cc96c64a4c9e8`，生成资产审计`65b8c4da66d5914f97821450c7b69e0fae51e8e383edb149766da253a0a0d561`。

## 8. 下一步

阶段0的A级静态交付已全部收口，B0-06仍因C级干净构建授权保持`partial-A;C-blocked`。阶段1的A级静态审计已收口；阶段2的A2-01~A2-08已完成A级静态审计且产品验收均未通过。下一项按索引执行A2-09阶段2时序图、状态矩阵与反向收口，并再次检查并行代码漂移。任何B/C/D测试、构建、NVS或设备动作仍需对应授权。

### A2-03 异常复位、NVS 与 OTA 恢复矩阵 A 级静态审计完成

- 已建立 `docs/review-2026-08-02/phase2-reset-recovery-matrix.md`，逐项覆盖 POWERON/EXT/BROWNOUT/PANIC/abort/INT WDT/Task WDT/软件重启，以及 NVS、otadata、OTA 两槽和 RTC 持久域。
- 已对 OTA 从 report context、pending v3 transaction、下载/写入、`esp_ota_end()`、set boot、pending-verify、health/云 ACK、mark valid、accepted sequence/identity到终态清理的14类断点建立恢复结果。静态确认：boot partition切换前复位保留旧槽；pending candidate在存在另一有效槽时，下一次复位会由ESP-IDF 6.0 bootloader标记`ABORTED`并回退；mark valid后的NVS提交中断由保留的pending记录补齐。
- 已直接核对实际工具链ESP-IDF v6.0 commit `662a3be354759d9487bf4b1a629fadb766cb1800`：bootloader在首次选择时把`NEW`改为`PENDING_VERIFY`，再次启动把未确认项改为`ABORTED`；NVS API只在`nvs_commit()`后保证非易失写入，底层结构恢复不能替代跨key/namespace业务原子性证明。
- A2-03没有重复创建canonical ID，落实并扩充`NVS-001/NVS-003/NVS-005/OBS-001/RTOS-001/LIFE-010/PART-003`。其中`PART-003`补充：当前OTA admission未显式要求运行槽为`VALID`或证明rollback target，无factory槽时rollback失败只能有线恢复；`NVS-005`补充OTA context、accepted metadata和otadata的跨域掉电矩阵。
- 产品验收未通过的根因保持明确：`NO_FREE_PAGES/NEW_VERSION_FOUND`仍全擦共享NVS；确定性fatal没有site/intent/crash-loop breaker/safe mode；Task WDT panic关闭且无业务task订阅；两个OTA槽均不可用时没有factory recovery。
- `review-work-item-index.csv`中A2-03已更新为`completed-A;B/C/D-open`，授权补齐B级host状态机但本轮未执行。`phase2-startup-power-recovery-audit.md`已加入续接入口；`partition-build-assets.md`和`persistence-schema-and-ota-compat.md`已同步canonical问题边界。
- 本轮只修改评审证据、索引和基线说明；未修改业务源码/配置/资产，未构建，未运行host/gateway测试，未访问COM5，未读写设备NVS，未OTA或烧录。

### A2-04 bootloader电源保持、strap/eFuse/USB-JTAG/download mode接管 A级静态审计完成

- 新建 `docs/review-2026-08-02/phase2-boot-power-handoff.md`，建立 ROM -> 二级bootloader -> app电源接管时序、正常启动/intentional-off/deep-sleep/download矩阵、GPIO0/3/45/46 strap矩阵、eFuse/debug surface矩阵和C/D关闭用例。
- ESP-IDF `bootloader_start.c:22-40`确认ROM从flash加载二级bootloader后，才在`bootloader_init()`前调用`bootloader_before_init()`；因此正常SPI boot会执行早期PG2钩子，ROM joint download不会执行该钩子。
- 当前 `nuwax_early_power_hold.c:25-58` 使用“预装HIGH -> GPIO matrix/function -> output enable -> 再写HIGH”顺序；bootloader与app当前均为PG2 GPIO7 high、SW3 GPIO16 active-low。现有只读7.0.23 `bootloader.map`确认`libnuwax_power_hold.a`、`bootloader_before_init=0x403c898c`和`bootloader_hooks_include=0x403c9568`实际链接，但该外部build未绑定本source manifest。
- 已完整视觉复核主板一1页、主板二3页：SW1把ESP_BOOT/GPIO0接地；PG2=GPIO7、PG1/SW3=GPIO16；USB D-/D+直连GPIO19/20；GPIO3为背光gate且R5 10 kOhm下拉；GPIO45复用SD_CMD并有上拉；GPIO46直连LCD_SDA3且未见独立拉阻。
- 新增`POWER-003`（P2）：ROM download绕过early hook，稳定烧录依赖USB独立保电、全程按住SW3或受控治具；当前没有唯一供电拓扑、允许松开时点、连续下载和中途掉电恢复证据。这是硬件/SOP恢复边界，不误写成ROM或app代码bug。
- `POWER-002`继续开放：bootloader固定key pin/active level并只允许0..21 hold pin，app可独立配置pin/极性/mode；当前值匹配但没有共享header、冲突排除或构建断言。
- GPIO45/46 reset strap、实际N32R8模组变体、VDD_SPI/eFuse、LCD/SD复位偏置无法由当前原理图百分百确定；任何eFuse写入、JTAG/ROM永久关闭、Secure Boot或Flash Encryption启用都必须另行审批，本轮未执行。
- 当前开发profile仍开USB Serial/JTAG、UART0/secondary USB console、OCD-aware debug和无命令认证的integration USB BLE配网入口；量产安全风险沿用`CONFIG-002/004`，不重复创建安全问题。
- A2-04更新为`completed-A;C/D-open`。未构建、未运行host/gateway测试、未访问COM5、未读eFuse/NVS、未执行OTA/烧录或任何不可逆动作。

### 7.0.23评审基线续接

- 当前源码已变为`PROJECT_VER=7.0.23`、compiled OTA release sequence `7000023`；新增`main/conversation/realtime_log.h`，实时音频日志门和OTA release policy调用点发生变化。
- 删除工程内可再生成的`tmp/pdfs/a2-04`渲染文件后，source manifest连续两次复算均为7,918项、0 missing/mismatch/extra/duplicate，SHA-256 `fca4d7b06d44596df1116becaf76223a3285fa81d57294a0699578f8b9731206`。
- 第一方活动C/H分母同步为350（204 C + 146 H），`main/`为335；memory逐文件矩阵新增`realtime_log.h`并刷新`mic_level.c/live_aec.c/audio_uplink.c/nuwax_ai_partner_main.c`行数。正式app CMake源分母仍为OTA 150/非OTA 145。
- `review-baseline.json`、`source-manifest-scope.md`、`module-inventory.md`、`config-diff.md`、`requirements-and-test-coverage-matrix.md`、`phase2-reset-recovery-matrix.md`与工作项索引已同步当前基线；历史7.0.22验证文档仍只代表原执行快照，不自动改写成7.0.23测试通过。

### 7.0.24评审基线与监控协议续接

- 当前源码真值已变为 `PROJECT_VER=7.0.24`、compiled OTA release sequence `7000024`；现有工程内 7.0.21 build 和外部 7.0.23 build 均未绑定当前 source/config/OTA manifest，只保留为历史、未绑定证据。
- 当时 source manifest 连续两次全量复算稳定为 7,918 项、SHA-256 `cb8ad7c0335c3672b4363e29e42b901f326dac1c62494a0292a46b53135754e2`；该快照随后已被 A2-05 收口时的 3 项并行内容漂移取代，不能继续作为当前 hash。
- 当时 `sdkconfig` SHA-256 为 `9ce6805d646397c2995e48949aafab368abb431ddaa9bbda4726e606e8cec576`、与 `sdkconfig.barge-final` 对称差异39行；当前值已在 A2-05 记录刷新。
- 监控 pause/resume 扩展的实现面已闭合到设备 hello、schema、validator、fake-device 和正式交付入口：`session_protocol.c` 同时声明 `monitoring_mode_v1` 与 `monitoring_pause_resume_v1`，原“扩展能力未协商”疑点关闭。
- `PARITY-002/VER-002` 仍保持 P1 发布阻塞，但根因改为协议资产不一致：唯一权威交付文档固定写 16 ms/512-byte PCM 帧和 8.192 s 队列，当前 `sdkconfig` 与 `audio_uplink.c` 静态断言为 20 ms/640 bytes、约 10.24 s；该文档还保留会议停播 fail-open 和 COM8/V1.2 身份描述，均不能代表当前 V2。
- 第一方 memory 矩阵仍为 350 个唯一路径（204 C + 146 H）。重新逐文件核对发现并修正 6 个行数漂移：两个 monitoring host 文件、`monitoring_mode.c`、`nuwax_ai_partner_main.c`、`session_protocol.c/.h`；最近变化没有引出新的独立 A 级内存安全 ID，B/C/D 证据仍开放。
- 本续接只修改评审证据和索引；没有修改业务源码、配置、协议交付原文或资产，没有构建、运行 host/gateway 测试、访问 COM5/NVS/eFuse、执行 OTA 或烧录。

### A2-05 电池、电源状态与 OTA 准入 A 级静态审计完成

- 新建 `docs/review-2026-08-02/phase2-battery-power-state-matrix.md`，覆盖首样本、可信/不可信 SOC、充电、满电、低电、无电池、gauge 未 ready、单次错误、stale low/non-low 和 service stop 共 14 类状态；逐项记录 snapshot、UI、OTA 和关机效果。
- 静态确认正常控制：SOC 只有在 valid、设计容量匹配和 CEDV profile verified 时可信；I2C 错误保留旧采样并标记 stale；连续 3 次错误后逻辑 deinit/退避重试；OTA 对无样本、stale、无电池和 invalid 大多 fail-closed；当前没有自动低电关机路径。
- 新增 `BAT-002` P2：板上没有 MCU 可见 VBUS，service 的 EXTERNAL 状态实际不可达，但 UI 将 UNKNOWN/invalid/stale 显示为“外接供电”，且 status bar/drawer 图标不一致。
- 新增 `BAT-003` P2：未认证 SOC/TDA/FD 仍参与 low latch，可能错误触发低电 UI/表情和 OTA 拒绝；估算显示和产品安全决策未分离。
- 新增 `BAT-004` P1：一次 charging 推导样本直接绕过 SOC、电压、low、连续样本和 VBUS/PG；trusted SOC 分支也无统一硬电压下限且忽略 low flag；OTA command preflight 和 pending-verify boot health 同时受影响，阻止产品 OTA/发布。
- `battery_insights.c` 虽进入正式 CMake，但当前没有产品调用者；温度、健康度、剩余/充满时间不能计为已接入产品功能。
- A2-05 收口期间捕捉到多轮并行改动：charging full-face UI 被关闭、speaker 增加 generation/admission/ring-buffer 路径、CMake/Kconfig/defaults/product overlay/两个 host runner 和 `sdkconfig` 同步变化。电池/OTA数值仍为 8 s、10/15%、1200 mAh、30%、4000 mV；第一方 memory 矩阵刷新 `face_engine.c`、`nuwax_state_bridge.c`、`speaker_playback.c` 行数，目标扫描未引出新的独立内存安全 ID。
- 并行改动停止后，最终 source manifest 连续两次稳定为 7,918 项、SHA-256 `ba22f1e64a4b95e19cca8d32e789798f5a15ee8c1f8af6b8d38553cc5bf71450`，两次相对前一快照均为 0 missing/mismatch/extra/duplicate。当前 `sdkconfig` SHA-256 为 `ea74b3ffc7ce84c2a02af8cf3ef022267bffd12eb6489721d77d2f32ab97e07d`，与 `sdkconfig.barge-final` 的归一化对称差异为49行。
- 最终一致性校验：工作项索引125行/125个唯一ID、0重复/0核心字段缺失，SHA-256 `1160a5d5fc176125c59813a4c59b2fbf91b48e6196d889a0b7202f6f656b4b47`；第一方 memory 矩阵350行/350个唯一路径、0重复/0行数 mismatch，SHA-256 `93920dc63dc4bc335324305002e66bd6b2d6348de1408ee7cb784abbc1675391`；A2-05矩阵 SHA-256 `00c6e9c5fb381e9081e9f09ccb1940b87c7bb39bf1d26a467c8300292885e350`；review JSON 可解析，关键证据 UTF-8 replacement 字符为0。
- A2-05 更新为 `completed-A;B/C/D-open`。本轮未修改产品源码/配置，未构建、未运行 host/gateway 测试、未访问 COM5/NVS/eFuse、未 OTA/烧录。

### A2-06 Wi-Fi/云离线时本地能力 A 级静态审计完成

- 新建`docs/review-2026-08-02/phase2-offline-local-capability.md`，按首次Wi-Fi init/connect失败、cloud config缺失、DNS/TLS/101失败、已连接后断线和云静默建立启动/运行期矩阵。
- 静态确认正向解耦：SW3、LVGL/UI、touch和battery task都在网络前启动；同步Wi-Fi/WS调用阻塞app主task时不会停止FreeRTOS调度。BLE重配网还通过touch direct callback和预先创建的flash worker绕过shell queue。
- 静态确认`OFFLINE-001` P1：首次Wi-Fi失败会在wake init、Wi-Fi菜单worker、speaker worker和长期主循环之前return；WS断开后`:16414`显式stop wake，只有重连成功后重新进入sleep并start detector。本地wake和conversation sleep/awake不具备离线owner。
- 静态确认`OFFLINE-002` P1：`s_shell_actions`深度8，只由在线audio/cloud/exclusive循环消费；断线purge不清该队列，payload无timestamp/epoch/generation。离线触摸可能满队列或在恢复后重放，meeting enable的迟到执行与`PRIV-002`持续采集同意风险叠加。
- 复用`START-002/RTOS-001/RTOS-005/NET-IO-001/NET-CONC-001/002/LIFE-009/EVENT-002/ERR-002/POWER-001/PRIV-002`，不把网络I/O stall写成已复现死锁。当前固定3 s retry、Wi-Fi 30 s wait和WSS 15 s timeout可避免显式tight loop，但统一总deadline、指数退避/jitter、worker/resource稳定仍需阶段5和B/D验证。
- A2-06更新为`completed-A;B/D-open`，产品验收“云端死掉不阻塞UI、电源和本地能力”未通过。源manifest收口时连续两次只读检查均保持7,918项、0 missing/mismatch/extra/duplicate、SHA-256 `ba22f1e64a4b95e19cca8d32e789798f5a15ee8c1f8af6b8d38553cc5bf71450`。
- 一致性校验：工作项索引125行/125个唯一ID、0重复/0核心字段缺失，SHA-256 `d5d6e040598da8841ded191d28a5fbf68fe0d91fc5fb0177a1a5585ff6db8c5e`；第一方memory矩阵350行/350个唯一路径、0重复/0逐文件行数mismatch，SHA-256 `93920dc63dc4bc335324305002e66bd6b2d6348de1408ee7cb784abbc1675391`；A2-06证据SHA-256 `5eb9c8022b839970c8c431c54f1123bdec2ec314e9982cfcd66a75a7e578fc95`；review JSON可解析，关键证据UTF-8 replacement字符为0。A2-06索引引用的13个canonical ID在本记录均恰有一条登记。
- 本轮只修改评审证据、索引和基线说明；未修改产品源码/配置/协议/资产，未构建、未运行host/gateway测试、未访问COM5/NVS/eFuse、未OTA/烧录。

### A2-07 启动失败、半初始化与资源回滚 A级静态审计完成

- 新建 `docs/review-2026-08-02/phase2-startup-failure-rollback.md`，按 `app_main()` 启动切点建立悬挂task、queue/mutex、controller、driver、DMA、callback/handler和shutdown gate矩阵，并把模块路径分类为 `CLEAN`、`RETAIN-RETRY`、`RESTART-ONLY`、`OPEN`。
- 静态确认整机启动不是可回滚事务：中后段多数失败以 `ERROR + return` 结束，已经启动的worker和资源没有统一逆序停止；`create_app_task*()`成功后通常只保存handle或不保存handle，TCB/stack owner丢失，不能在后续失败时完整回收。复用并增强`RTOS-008`。
- OTA在controller建立前抢占SW3 shutdown gate；`s_ws_pointer_mutex` OOM会直接return且不释放gate。OTA partial init还缺callback/bind/interlock/controller task的完整撤销，复用`LIFE-010`。Wi-Fi event group/netif/driver/handler partial init缺owner与逆序rollback，复用`RTOS-009`；LVGL mutex/buffer/display/indev/timer/task partial init及空返回检查缺口复用`LIFE-004`；power init错误不可传播复用`ERR-001`。
- display SPI/DMA、共享音频板、mic、wake、会议/监控controller等局部rollback是正向控制，但不能外推为全局启动事务通过。A2-07未新增canonical ID，状态更新为`completed-A;B/D-open`，产品验收“每个失败点资源回到基线或受控重启”未通过。
- 收口前相对A2-06 source manifest发现7个并行漂移文件：`audio_uplink.c`、`barge_in_dispatcher.c`、`speaker_playback.c`、`nuwax_ai_partner_main.c`、`ws_transport.c`和两个host runner。已按最新主入口、speaker和WebSocket重新核对A2-07：WebSocket有限锁等待/owner-close是正向变化，speaker仍没有manager/ring/mutex/task完整deinit，主入口仍没有startup owner ledger；A2-07核心结论保持成立。
- 其他受漂移影响的旧阶段结论进入最终复核队列。全部审计结束后必须以最终source manifest重新落实一次跨阶段代码校验；只要文件hash变化，对应中间A级结论就不能直接沿用。
- 收口连续两次源码复算均为7,918项、0 missing/mismatch/extra/duplicate，正式manifest SHA-256为`3074e4582fdc8bc53dec2f74cdfaa67d801544b24d69cf2d2d3fc3d5e22db60b`。A2-07专项证据SHA-256为`392e2320dbacc4c5026cbcc1bd3d1e1f4443c085da94f72c4abdb9c7aa8f0d2f`；工作项索引125行/125个唯一ID、0重复/0核心字段缺失，SHA-256为`c9438d55445ffa5f8bf143fbbb60b74b4969508cfd8779679d2fee3e9c2eb179`。
- 本轮只修改审计文档、索引和基线；未修改产品源码/配置/协议/资产，未构建，未运行host/gateway测试，未访问COM5/NVS/eFuse，未OTA或烧录。

### A2-08 启动期fatal、core dump与持久化预算 A级静态审计完成

- 在`phase2-reset-recovery-matrix.md`第10节复用并刷新A1-09清单。当前正式构建分母仍为150个app C源+1个bootloader源；精确计数为3个hard check、32个without-abort、3个abort、4个restart、0个正式assert/configASSERT。
- 已按最新并行修改后的`audio_uplink.c`和当前主入口重新落码。三个abort仍只在未提交uplink sender不能安全discard的窄路径；两个安全restart仍用于避免释放live worker资源；BLE/OTA两条是预期重启。保护语义没有反转，但重启后均缺统一site/intent/stage/task/session/epoch/image identity。
- `nuwax_power_ctrl.c`只保存RTC boot magic/count并打印通用reset reason，RTC STORE0另用于SW3 intentional-off marker；没有版本化breadcrumb、CRC、consume、同site时间窗、safe mode或crash-loop breaker。复用`OBS-001`。
- 当前panic为print-and-reboot、0秒延时；UART0 GPIO43/44为921600 baud并启用secondary USB Serial/JTAG。旧A1-09中的115200描述已显式标为历史快照。INT WDT 300 ms；Task WDT 10 s但panic关闭且无业务task订阅，复用`RTOS-001`。
- 全部11个`sdkconfig*`无`ESP_COREDUMP`配置，活动分区表无coredump分区。16 MiB布局最后结束在`0x1000000`，空余0；`storage`虽未发现第一方挂载入口，也不能未经产品/迁移决策直接覆盖。新增dump需重分区且app-only OTA不能更新partition table，必须走受控全量升级，或量产明确禁用dump。
- NVS仅24 KiB且承载Wi-Fi/cloud/OTA/配网和用户设置；没有`nvs_get_stats`、diagnostic ring entry或磨损预算。panic上下文不能无条件写NVS，NVS本身也可能是fatal根因；复用`NVS-001/NVS-004/NVS-005`。
- 当前150个正式源日志调用重算为1,481处（E494/W412/I538/D37/V0），66个文件有日志；默认INFO仍输出ASR/用户/助手正文预览及SSID/IP/UID/cloud URL。复用`LOG-001/PRIV-001`，未来core dump还需纳入`PRIV-003`。
- A2-08没有新增独立canonical ID，复用`OBS-001/LOG-001/PRIV-001/RTOS-001/LIFE-010/NVS-001/NVS-004/NVS-005/PART-003/VER-001`，状态为`completed-A;C/D-open`。产品验收“异常可区分且日志不泄密、不破坏NVS/OTA”未通过。
- 收口期间并行工程师再次修改`capture_phase_handoff_host_check.c`、`capture_phase_handoff.h`、`nuwax_ai_partner_main.c`和conversation host runner。已对最新主入口复算fatal站点，数量和语义仍为3/32/3/4/0/0；capture handoff改动未增加fatal/restart入口。相关对话链结论登记为最终全量复核项。
- 最终连续两次源码复算均为7,918项、0 missing/mismatch/extra/duplicate，正式manifest SHA-256为`0ec684337512eae921916825db78ca92b56cd7fce1fd8c764359e21fd272ce3e`。A2-08恢复/可观测性矩阵SHA-256为`161c0059c1d00cc1b904163e38e78288c4b2849ea44f3f622024faecb3a05174`；工作项索引SHA-256为`6d45bb62cd116ba1477f20bca44d328a543e091e2e5b42400a029f5c0a2740d4`。
- 本轮只修改审计证据、索引和基线；未修改产品源码/配置/分区/协议/资产，未构建、未运行测试、未访问COM5/NVS/eFuse、未OTA或烧录。

### A2-09 阶段2时序、状态矩阵与反向收口完成

- `phase2-startup-power-recovery-audit.md`第23节已补齐端到端启动/关机/复位/OTA时序图、八轴状态矩阵、A2-01~08逐项反查、问题并集、修复顺序和动态门禁。
- 反查确认A2-01~08的8个主证据路径全部存在，每项都有问题ID和残余风险；索引并集为42个去重canonical ID，其中28个P1、14个P2、0个新增直接P0。该统计是静态发布风险并集，不是动态复现计数。
- 旧综合表中把A2-07/A2-08写为“未评审”的历史快照已更正。A2-09状态更新为`completed-A;B/C/D-open;product-not-accepted`；阶段2仅A级静态证据收口，八项产品验收仍全部未通过。
- A2-09期间检测到BLE配网模块持续并行变化，涉及`nuwax_provision_main.c/.h`、policy helper和host check。变化方向包含Wi-Fi owner/generation、新旧凭据竞争与Security2/vox-config守卫，但文件在审计中多次改变，因此没有把中间快照写成已生效或已验收；`A2-06/RTOS-007/NVS-005`保持开放，并进入阶段6和最终manifest强制重审。
- 本项没有修改产品源码、配置、分区或协议，没有构建、host/gateway测试、COM5/NVS/eFuse、OTA或烧录。下一项为`A3-01`硬件原理图/实物/引脚权威基线A级审计。

### A3-01 新版硬件 GPIO、复用与总线 owner A级静态审计完成

- 新建 `docs/review-2026-08-02/phase3-pin-bus-audit.md`，完整核对麦克风原理图1页、主板一1页、主板二3页，并建立 GPIO0~48、I2C0、I2S0、QSPI、UART、SDMMC和USB的owner/方向/上下拉/复用/产品可达性矩阵。
- 当前有效 `sdkconfig`、当前 pin header、商家 V2 `config.h` 与喵伴2.0原理图的主要映射一致；I2C0只有 `i2c_bus_manager` 创建物理bus，V2音频只由 `vocat_audio_board` 创建I2S0 duplex channel，GPIO43/44底座UART与UART0 console冲突已有编译期门禁，默认4G和SD路径不激活。
- 新增 `HW-PIN-001`（P1）：GPIO45同时是带10 kOhm外部上拉的SD_CMD和ESP32-S3 VDD_SPI reset strap；缺绑定样机的eFuse摘要、模组料号、reset采样波形和量产证据。禁止从单台样机曾启动外推量产安全，任何eFuse写入必须另行审批。
- 新增 `DRV-REACH-001`（P1）：GPIO8头部触摸、GPIO18绿灯和整板selftest虽有源码并进入正式CMake，但最新 `app_main()` 没有初始化GPIO8/18，也没有调用 `nuwax_hardware_selftest_run()`。产品中的 `nuwax_touch_init()` 仅初始化GPIO11面板触摸中断，不能替代GPIO8。GPIO8产品/诊断接入归A3-14/A3-16，LED归A3-12，QMI8658A仍独立归A3-11。
 - A3-01主体完成后检测到 `main/nuwax_ai_partner_main.c` 并行漂移；已按最新 SHA-256 `6ae1308012bd969b69c275155b2b8ba8a9770c536eec8b241bb8098b35328da1`（776223 bytes，2026-08-03 19:05:10 +08:00）重新反查 `app_main()` 和上述三个入口。当前 `sdkconfig` SHA-256为`e3f594fb8eb78137a340faf97a8591d8d394d8225259be8ae538b8725e701987`，`main/Kconfig.projbuild`为`83fd22d181c1dd0cfc1897cecc5b5c20a5c5437c86e6143c9918ffa136b8b70c`；可达性问题仍成立。
- A3-01更新为`completed-A;C/D-open;product-not-accepted`。所有审计结束后仍必须按最终source manifest重跑pin/config/callsite矩阵；只要上述文件hash再次变化，本结论不能直接沿用。
- 本轮只修改审计证据、索引和工作记录；未修改产品源码、配置、分区、协议或硬件资料，未构建、未运行host/gateway测试，未访问COM5/NVS/eFuse，未OTA/烧录。下一项为A3-02电气约束与测量计划A级静态审计，实测C/D门保持开放。

### A3-02 PSRAM/Flash、电源轨、温升、brownout、PA瞬态与I2C卡死 A级静态审计完成

- 新建 `docs/review-2026-08-02/phase3-electrical-measurements.md`，核对主板一电源/电池/4G/SD页和主板二 MCU/I2C/I2S/PA/AEC 三页，并将配置、分区、驱动和依赖库的电气假设绑定到同一证据矩阵。
- 静态确认当前为16 MiB DIO 80 MHz Flash、8 MiB Octal 80 MHz PSRAM，PSRAM memtest、brownout最高档和 Flash brownout reset已配置；但原理图标N32R8，当前没有Flash/PSRAM JEDEC、模组料号、eFuse和镜像identity绑定，`platform_memory_test()`也未从`app_main()`调用，新增`HW-FLASH-001`（P1）。
- 新增`HW-ELEC-001`（P1）：VOUT/MCU_3V3/CODEC_3V3/4G_VCC/PA电流、温升、PA_CTRL瞬态、brownout跌落/恢复均没有绑定样机和镜像hash的C/D证据；代码只在重启后记录brownout原因，不能宣称连续对话/高音量/低电量稳定。
- 新增`I2C-REC-001`（P1）：`i2c_bus_manager.c`只提供init/get/add/remove/probe，不含SCL脉冲、STOP、GPIO接管或bus重建；touch在超时后保留handle，battery只回收device，shared SDA/SCL拉死没有可证明恢复链。
- 新增`PA-GAIN-001`（P1）：原理图 NS4150B VCC接VOUT，RIN/RF约240 kOhm/100 kOhm（约7.6 dB）；`vocat_audio_board.c`固定`pa_voltage=5.0f`且未填`pa_gain`，依赖库会把错误hardware gain用于ES8311 `set_vol()`，导致不同电量/USB/音量和AEC测试音没有可信统一标定。
- 正向控制包括：PSRAM缺失不忽略、brownout和SPI Flash reset配置、PA_CTRL set/readback、关机前强制PA low并等待20 ms、I2C事务有单次timeout；这些都不替代样机测量和物理bus恢复。
- A3-02更新为`completed-A;C/D-open;product-not-accepted`。C/D必须按文档矩阵执行Flash/PSRAM identity、rail/thermal、brownout、I2C stuck、PA/volume五组证据；阈值由芯片/电源/扬声器规格和硬件负责人签核。
- 本轮只修改审计证据、索引和工作记录；未修改产品源码、配置、分区、协议或硬件资料，未构建、未运行host/gateway测试，未访问COM5/NVS/eFuse，未OTA/烧录。下一项为A3-03 ES7210硬件音频链静态审计，实测门保持开放。

### 并行漂移后 A3-01/A3-02 证据复核（2026-08-03）

- 重新计算 `main/nuwax_ai_partner_main.c` SHA-256 为 `6ae1308012bd969b69c275155b2b8ba8a9770c536eec8b241bb8098b35328da1`（776223 bytes，2026-08-03 19:05:10 +08:00），并重新确认 `app_main()` 没有 `nuwax_board_gpio_inputs_init()`、`nuwax_led_status_init()` 或 `nuwax_hardware_selftest_run()` 调用；`nuwax_touch_init()` 仍为 GPIO11 面板触摸路径。A3-01 的 `DRV-REACH-001`、`HW-PIN-001` 与状态 `completed-A;C/D-open;product-not-accepted` 保持成立。
- A3-02 所引用的 `sdkconfig`、`i2c_bus_manager.c`、`vocat_audio_board.c`、selftest、codec 依赖和分区证据未发现本轮哈希漂移；`HW-FLASH-001`、`HW-ELEC-001`、`I2C-REC-001`、`PA-GAIN-001` 及 `completed-A;C/D-open;product-not-accepted` 保持成立。
- 本次仅更新审计证据的快照哈希和执行记录；未修改产品源码、配置、分区、协议或硬件资料，未构建、未运行 host/gateway 测试，未访问 COM5/NVS/eFuse，未 OTA/烧录。后续仍须在所有阶段完成后以最终 source manifest 执行跨阶段全量复核。

### A3-03 ES7210地址、复位、MICBIAS、采样率、slot与MIC3路径 A级静态审计完成

- 新建 `docs/review-2026-08-02/phase3-audio-hardware-audit.md`，对ES7210地址换算、主从关系、16 kHz/16-bit/四槽TDM、MCLK/BCLK预算、软件复位、MICBIAS、PGA、初始化回滚、MIC3原理图路径和产品health gate逐项建立证据链。
- 静态确认7-bit `0x40`地址经板级8-bit `0x80`和control层右移后正确；ESP32-S3为clock master、ES7210为slave，MCLK=4.096 MHz，TX/RX均需要64 BCLK/Fs即1.024 MHz。slave模式`set_fs`不配置内部采样分频与外部时钟所有权一致，实际波形仍需C/D验证。
- 初始化先提供MCLK并等待约20 ms，ES7210软件reset为`0xff -> 0x41`；完整I2S/codec失败会逆序删除codec、control/GPIO interface和I2S channel。stop/start使用确定的5000 us，不受100 Hz tick归零影响；未新增半初始化资源泄漏ID。
- 新增`CODEC-RMW-001`（P1）：依赖驱动`es7210_update_reg_bit()`丢弃read错误，仍以0生成写回并可能返回成功；I2C瞬态故障可静默覆盖模式、时钟、增益或mute寄存器。与`I2C-REC-001`关联但根因独立。
- 新增`AUDIO-SLOT-001`（P1）：规格书Figure 2e显示`Channel 1 -> 3 -> 2 -> 4`，但不能据此推翻历史逐PGA已确认的MIC1->DMA[0]、MIC2->DMA[1]。MIC3/MIC4在历史样机不可观测，代码注释、candidate slot和health检查共同预设slot2，尚无MIC3->DMA[2]非循环闭环。
- 原理图确认ES8311 OUTP/N经R7-R13、C7-C15进入ES7210 MIC3P/N，MIC1/MIC2共用MICBIAS12，MIC4 NC。驱动向REG41/42写0x70并注释约2.87 V，但现有11页产品规格书无寄存器编程表；完整复位、bias、HPF/OSR和start/stop序列仍需programming guide/供应商签核及样机测量。
- 历史样机日志`com5-b18-hw-acceptance-monitor-20260727-112549.stdout.log`记录MIC3 silent probe失败、slot2 tone_amp=0.1和health passed=0。当前产品配置虽允许slot2 capability，但全局health初值false且正常启动不跑测试音gate，会fail-safe回退software reference；这降低误用风险，不等于硬件AEC已放行。
- A3-03更新为`completed-A;C/D-open;product-not-accepted`，引用`AUDIO-SLOT-001;CODEC-RMW-001;HW-EVID-001;I2C-REC-001;PA-GAIN-001`。本项只修改评审证据和索引，未修改产品源码/配置，未构建、测试、访问COM5/NVS/eFuse、OTA或烧录。下一项为A3-04 ES8311静态审计；最终仍须按最终source manifest全量复核。
- 文档写入后检测到并行工程师将`main/nuwax_ai_partner_main.c`更新为784,129 bytes、SHA-256 `4d8d758ea28248c6e937e815a8292a7e4362c93cccbf4106a5148c6d65fe48c7`（2026-08-03 20:14:52 +08:00）。已按最新文件重新核对hardware-reference启用三条件、manual/acceptance赋值点和boot gate编译条件：`s_hw_ref_health_passed`仍默认false，正常配置未启用MR prototype/boot gate，A3-03 fail-safe结论保持成立；对话链其他变化保留到最终manifest复核。

### A3-04 ES8311 DAC/ADC、I2S主从、音量、静音与pop时序 A级静态审计完成

- 已在`docs/review-2026-08-02/phase3-audio-hardware-audit.md`追加A3-04完整证据，对ES8311产品角色、I2S主从/时钟、寄存器错误传播、pair/enabled/PA状态、软件音量、shared-TX停播、deinit和商家V2实现逐项追踪。
- 静态确认产品只使用ES8311 DAC，ESP32-S3为master、ES8311为slave；当前16 kHz、16-bit、双32-bit slot对应MCLK=4.096 MHz、BCLK=1.024 MHz，依赖系数表有`4.096 MHz/16 kHz/bclk_div=4`项。ES8311 ADC能力没有进入产品输入主链，不得写成已启用/已验收。
- ES8311规格书实际为12个物理页，已全部渲染逐页视觉复核；确认8~96 kHz、slave、外部同步LRCK/SCLK、I2S边沿、差分OUTP/N和pop/click suppression能力。资料没有寄存器表和start/suspend/mute/PA时序，已纠正`hardware-evidence-manifest.md`中的旧9页记录，完整programming guide和供应商签核仍开放。
- 正向路径成立：V2在open先setup并拉低PA，enable按start后PA high/unmute，正常轮间停播用完整DMA零预装而不频繁切codec/PA；用户0~100%音量由PCM线性缩放实现，software reference只记录同一缩放后且完成物理写出的帧。
- 新增`CODEC-ERR-001`（P1）：ES8311 `set_fs()`不判NULL、非法bits静默降级且对正常请求固定成功；open/start/config_sample覆盖旧错误，多个RMW在read失败后继续write，mute和PA helper丢弃实际write/GPIO结果。
- 新增`CODEC-STATE-001`（P1）：start失败仍add pair，enable仍开PA/unmute；enabled保持false又使后续disable可直接no-op。disable/close非事务化且delete会无条件free，硬件失败无法可靠观测、回滚或重试。
- 新增`AUDIO-STOP-001`（P1）：shared V2 stop若disable、完整零预装或re-enable失败，没有codec mute/PA low最后防线；re-enable失败分支声称PA muted的注释不符合V2实际。必须建立有界physical-quiet API并以GPIO4/扬声器波形验收。
- A3-04更新为`completed-A;C/D-open;product-not-accepted`，引用`AUDIO-STOP-001;CODEC-ERR-001;CODEC-STATE-001;HW-EVID-001;I2C-REC-001;PA-GAIN-001`。本项只修改审计文档、证据清单和索引，未修改产品源码/配置，未构建、测试、访问COM5/NVS/eFuse、OTA或烧录。下一项为A3-05；全部审计结束后仍按最终source manifest全量复核。

### A3-04收口与并行漂移复核（2026-08-03）

- A3-04新增的`CODEC-ERR-001`、`CODEC-STATE-001`、`AUDIO-STOP-001`在canonical问题表中均恰有一条；评审目录JSON可解析，57个审计文本文件无U+FFFD替换字符。
- `main/conversation/speaker_playback.c`在A3-04完成后持续被并行更新；本次写后快照为182,476 bytes、SHA-256 `20cdbfc1422fc398625d28b98fb462d1b4cb0f4b3b3baa364e0a81b8c9536420`（2026-08-03 21:12:28 +08:00）。已按该版本复核completion/cancel/preinit的stop-fault调用链。`speaker_output.c`及ES8311、audio-board核心文件未漂移，shared-TX失败仍没有GPIO4 low物理兜底，A3-04结论保持成立。
- `sdkconfig`随后也发生并行重写，当前审计快照SHA-256为`2ae83cfdb5fe0bcd079d27fc1f787730c0de0e445f8a1b2c8f53d698e5a8efb9`；V2 profile、16 kHz、DMA 6x240、GPIO4等本项相关值保持不变。对话主文件也有并行漂移，但A3-04核心结论不依赖其具体调用点。
- 全索引反查发现19个早期历史编号尚未形成canonical问题行；A3-04新增三项不在该缺口中。该评审治理缺口保留到最终全量复核统一校正，不能因此把全索引写成已完全闭环。

### A3-05 NS4150B/PA_CTRL极性、mute、enable与掉电默认态 A级静态审计完成

- 已完整渲染并逐页视觉复核NS4150B规格书；实际为9个物理页，已纠正`hardware-evidence-manifest.md`旧11页记录。规格书确认CTRL高为Open、低为Shutdown，VIH最小1.2 V、VIL最大0.2 V，启动/关断时间典型120/80 ms，并具有上/掉电噪声抑制和保护能力。
- 原理图和代码共同确认GPIO4直连PA_CTRL且R18 10 kOhm下拉；产品与商家V2均使用GPIO4、默认非反相，因此active-high极性和复位高阻时的shutdown设计成立。实物贴装和复位/电源斜坡电平仍需D级验证。
- 原理图确认PA VCC接VOUT，ES8311 OUTP/N经100 nF和100 kOhm输入网络进入NS4150B，内部240 kOhm反馈对应约7.6 dB；继续复用`PA-GAIN-001`，固定5 V/0 dB模型不能放行。
- 初始化在PA high前已启I2S时钟，但没有完整零DMA ring证明；PA high后无ready门。normal stop保持PA high并依赖零DMA，只是数字quiet而非物理shutdown；hard stop错误没有GPIO4 low兜底，继续复用`AUDIO-STOP-001`。
- 新增`PA-TIME-001`（P1）：没有供应商签核最坏时间或PA状态反馈时，PA high后无ready门、deinit无shutdown完成门；SW3即使强制GPIO low失败仍继续，且只等20 ms便释放PG2，不能证明首帧和掉电前物理静音。
- ES8311 PA helper吞GPIO错误、start失败仍可开PA、disable/close状态非事务，继续复用`CODEC-ERR-001`与`CODEC-STATE-001`；SW3不协调音频owner继续复用`POWER-001`。
- A3-05更新为`completed-A;D-open;product-not-accepted`，引用`AUDIO-STOP-001;CODEC-ERR-001;CODEC-STATE-001;HW-ELEC-001;HW-EVID-001;PA-GAIN-001;PA-TIME-001;POWER-001`。本项只修改评审文档、证据清单和索引，未修改产品源码/配置，未构建、测试、访问COM5/NVS/eFuse、OTA或烧录。下一项为A3-06；全部审计结束后仍按最终source manifest全量复核。

### A3-06 I2S/DMA时钟、slot、buffer、共享TX/RX与stall恢复 A级静态审计完成

- 已在`phase3-audio-hardware-audit.md`新增第26~31节，绑定当前ESP-IDF v6.0 commit `662a3be354759d9487bf4b1a629fadb766cb1800`及四个I2S/HAL文件hash，复核controller full-duplex、后初始化RX转slave、MCLK绑定、DMA queue overflow和channel停启/删除语义。
- 当前16 kHz TX为双32-bit slot、RX为四16-bit slot，两方向均64 bit/frame，MCLK 4.096 MHz、BCLK 1.024 MHz；6x240 frame每方向约90 ms。speaker按240 frame分块并使用整体deadline，TX auto-clear、完整零ring和partial physical write/reference边界是正向控制。
- 新增`I2S-OBS-001`（P1）：IDF RX queue满时drop oldest，项目未注册callback/counter；上层读出后才生成sequence，不能发现物理缺口。timeout partial-read也未验证8-byte TDM frame对齐，会议/监控连续100个500 ms零读才停止，最坏约50 s且无driver重建。
- 新增`AUDIO-OWN-001`（P1）：authenticated云端/手工AEC health只检查speaker pending和normal prelisten，随后直接读写共享I2S并改codec/PA，绕过wake/meeting/monitoring的MIC owner、board lease和mode controller。
- 新增`AUDIO-LIFE-001`（P1）：board lease只有bool、release无token/generation；final teardown先归零refcount和清句柄再disable/delete，失败后无法对同一driver资源重试回收。该根因与`API-001`相关但不重复。
- `sdkconfig`在前一项后发生并行漂移，本项已改绑2026-08-03 21:31快照SHA-256 `ac1b2d80fb6a0769e401f2f06ef2f4a081e91d64e8b5bf20aef64159dc250cae`；六个核心音频源码hash与审计前复核一致。A3-06状态为`completed-A;C/D-open;product-not-accepted`，C/D矩阵覆盖共享时钟连续性、overflow、underflow、stall、owner冲突、stale lease/delete失败、partial read和cache压力。
- 本项只修改评审证据、硬件/IDF清单、索引和工作记录；未修改产品源码/配置/协议/分区，未构建或运行测试，未访问COM5/NVS/eFuse，未OTA/烧录。下一项为A3-07；所有审计完成后仍按最终source manifest全量复核。
