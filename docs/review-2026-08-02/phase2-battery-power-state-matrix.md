# 阶段 2 A2-05：电池、电源状态与 OTA 准入 A 级静态审计

审计日期：2026-08-03  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
工作项：`review-work-item-index.csv` 的 `A2-05`  
授权：A 级纯静态源码/配置审计；只写评审证据、索引和执行记录。未修改业务源码、配置、正式协议或资源；未构建、未运行 host/gateway 测试、未访问 COM5、未读写 NVS/eFuse、未 OTA/烧录。

## 1. 结论先行

1. 电池服务的正常采样、陈旧标记、I2C 错误保留、连续错误后的驱动重试和 OTA fail-closed 基本闭合。当前没有发现“低电量自动关机”路径；低电量主要影响 UI、表情和 OTA 准入。
2. 板上没有 MCU 可见的 VBUS/电源良好信号。`classify_power()` 从不返回 `NUWAX_BATTERY_POWER_EXTERNAL`；因此当前代码不能证明外接供电，更不能把“无电池”解释为稳定外部电源。
3. UI 将 UNKNOWN、无效或陈旧非低电量状态映射成“外接供电”，而顶栏和抽屉对 external 使用不同图标。它是确定的错误语义，不是电源检测能力。
4. 未认证 CEDV/profile 的 SOC 和 BQ 的低电标志仍参与 `low_latched` 产品决策。这样可能造成错误低电 UI/OTA 拒绝；同时可信 SOC、低电 flag、充电状态的优先级没有统一的产品电源状态来源。
5. OTA admission 和新镜像 boot-health gate 对“新鲜充电”直接放行，绕过 SOC、电压、低电 flag、连续样本和真实 VBUS/PG 检查。可信 SOC 分支也没有统一的硬电压下限，并且不检查 `battery.low`。这是 `BAT-004` P1，属于发布阻塞。
6. `battery_insights.c` 是可调用的纯计算模块，但当前没有产品调用者；温度、健康度、剩余/充满时间不会影响 UI、OTA 或关机策略。它不能被描述为已经接入的产品功能。

因此：A 级静态审计完成；A2-05 产品验收未通过；B/D 电源边界和实机证据仍开放。下面的矩阵是“当前代码会怎么做”，不是“设备已通过”。

## 2. 审计输入与证据边界

### 2.1 主要代码

| 组件 | 当前证据 |
|---|---|
| BQ27220 驱动 | `main/board/bq27220_driver.c:38-63,418-452,454-516` |
| battery service | `main/board/battery_service.c:142-250,253-358,360-471` |
| battery API contract | `main/board/battery_service.h:12-87` |
| battery insights | `main/board/battery_insights.c:15-106`、`battery_insights.h:54-59` |
| UI producer | `main/nuwax_ai_partner_main.c:633-695` |
| UI merge/snapshot | `main/app_shell.c:115-172,623-649`；`main/nuwax_ui_v28/nuwax_device_state.h:59-104` |
| status bar/drawer | `main/nuwax_ui_v28/nuwax_status_bar.c:795-807,916-940`；`main/nuwax_ui_v28/nuwax_drawer.c:276-306` |
| OTA admission | `main/nuwax_ota_new/nuwax_ota_device.c:716-746,840-849,1451-1459` |
| OTA boot health | `main/nuwax_ai_partner_main.c:4757-4802` |

### 2.2 当前配置

当前 `sdkconfig`：采样 8000 ms、低电进入/退出 10%/15%、设计容量 1200 mAh、OTA 最低 SOC 30%、未认证 gauge 电压门槛 4000 mV、FreeRTOS 100 Hz。Kconfig 默认 OTA 最低 SOC 仍为 20%（`main/Kconfig.projbuild:68-82`），当前值与干净重配置的追溯问题沿用 `CONFIG-003`，不能在本文件重复解释成运行时 bug。

### 2.3 证据限制

没有样机电芯对应的 CEDV/profile 认证资料、生产 BOM/netlist、可见 VBUS/PG 测量、充电器输入能力和电源边界波形。因此本文件不判断电量百分比精度、充电硬件是否正确或 USB/电池供电的实物行为。

## 3. 状态来源与决策规则

### 3.1 原始 gauge 到 service snapshot

- `battery_present` 来自 `BATTPRES`。
- `is_charging = !FC && !DSG && !CHGINH && current_ma > 20`，是一次采样基于寄存器的推导，不是 VBUS/charger-PG 读数。
- `is_low = SOC <= low_threshold || TDA || FD`。
- `valid = battery_present && gauge_ready && measurements_plausible`。
- `soc_trusted = valid && design_capacity_matches && profile_verified`。
- `low_latched` 在 `status->is_low` 或 `percent <= low_enter` 时置位，在 `percent >= low_exit` 时清除；该判断没有检查 `soc_trusted`。
- `power_state` 的实际可达集合只有 `UNKNOWN`、`BATTERY`、`CHARGING`、`LOW`；`EXTERNAL` 在当前板级实现不可达。

### 3.2 陈旧与错误

采样周期为 8 s，快照年龄超过三个周期（约 24 s）才标记 `stale`。I2C 错误保留最后一次测量并递增 `consecutive_errors`；连续 3 次错误后驱动逻辑 deinit，随后按退避重新 init。服务停止成功后 task 被 owner 删除，但停止本身不发布 UI 快照。

### 3.3 UI、OTA 和关机

- UI listener 只在 `valid` 且非陈旧时显示 SOC；陈旧低电量例外保留低电警告，但 SOC 改为估算形式。
- `app_shell`、状态桥和 UI 将 `valid=false` 映射到 `NUWAX_POWER_EXTERNAL`；这只是“无可用电量数据”的占位，不是外部电源证明。
- OTA admission 先拒绝无样本、陈旧、无电池和 invalid，再按 charging、trusted SOC、untrusted SOC 分支判断。
- 当前源码未见电池低电自动关机、降低音量或停止对话的产品路径；只有 OTA 拒绝、低电表情/图标和 issue 统计。

## 4. 电源状态矩阵

表中“UI”描述当前代码的可见语义；“OTA”同时适用于设备 OTA command preflight。除特别说明外，“无自动关机”均为静态调用图结论。

| 场景 | snapshot 关键 flags | service power_state | 当前 UI | OTA admission | 自动关机 |
|---|---|---|---|---|---|
| 尚无首样本/服务刚启动 | `sample_available=0, valid=0` | UNKNOWN | `--`，被表达为“外接供电”；顶栏 external 实际画电池图标 | 拒绝 `power_unknown` | 无 |
| 新鲜、可信、正常放电，SOC >= 30% | valid=1, trusted=1, stale=0, low=0 | BATTERY | 真实 `%`、电池图标 | 允许 | 无 |
| 新鲜、可信，SOC 10%~29% | valid=1, trusted=1, low 通常为0 | BATTERY（SOC 未到10%时） | 显示真实 `%`，未必显示低电 | 拒绝 `low_power` | 无 |
| 新鲜、可信，SOC <=10% 或 TDA/FD | low_latched=1 | LOW（除非 charging 优先） | 低电图标/低电表情 | trusted 分支只看 `<30%`；若异常出现 SOC>=30% 且 low flag=1，仍可能允许，属于 BAT-004 | 无 |
| 新鲜、未认证、正常且电压 >=4000 mV | valid=1, trusted=0, stale=0, low=0 | BATTERY | `~%` 估算百分比 | 允许 | 无 |
| 新鲜、未认证且 low flag 或电压 <4000 mV | valid=1, trusted=0, low=1 或 voltage<4000 | LOW 或 BATTERY | 可能显示低电，也可能只显示 `~%` | 拒绝 `low_power` | 无 |
| 新鲜 charging（包括低 SOC/低电压样本） | valid=1, charging=1 | CHARGING（优先于 LOW） | 充电图标；低电 flag 可能被遮蔽 | 直接允许，绕过 SOC/电压/连续样本/VBUS，BAT-004 | 无 |
| 新鲜 full | `full=1, charging=0` | BATTERY 或 LOW | 没有独立“满电”状态；按电池/低电显示 | 仍走 SOC/low 分支 | 无 |
| battery absent | `sample_available=1, battery_present=0, valid=0` | UNKNOWN | `--`，错误显示为“外接供电” | 拒绝；external 分支不可达 | 无 |
| gauge 未 ready / CFGUPDATE / plausibility 失败 | sample available，valid=0 | 可能被原始 flags 分类为 BATTERY/CHARGING/LOW，但 UI 一律 invalid | `--`，错误显示为“外接供电” | 在 invalid 前置检查处拒绝 | 无 |
| 单次 I2C 错误，旧样本年龄 <=24 s | 保留旧 valid/percent，stale=0 | 保留旧状态 | 保留上次 UI | 按旧样本继续判断；这是设计的三周期 freshness 窗口 | 无 |
| 连续错误后 stale retained low | valid=1, stale=1, low=1 | 保留 LOW | 保留低电警告，百分比为 `~%` | 拒绝 `power_unknown` | 无 |
| 连续错误后 stale non-low | valid=1, stale=1, low=0 | 旧状态保留在 snapshot | listener 更新后 `--`，并表达为“外接供电” | 拒绝 `power_unknown` | 无 |
| service 被显式 stop | `running=0`；不会主动通知 UI | snapshot 依旧保留旧值，getter按年龄标 stale | UI 可能无限保留最后一次发布值 | stop 后约24 s内 preflight 只看快照，未检查 `is_running()`；之后因 stale 拒绝。当前无产品 caller，但 API 语义不完整 | 无 |

## 5. 已确认问题

### BAT-001（P1，既有）：电量可信度未完成

缺少样机电芯对应的 BQ27220 CEDV/profile 认证资料、生产 BOM/netlist 和完整 gauge 资料。`profile_verified` 的静态比较不能替代“这块样机电芯实际容量/曲线一致”的 D 级证据。因此 UI 的 `~%` 可以作为估算展示，但不能宣称“可信电量百分比”已验收。

### BAT-002（P2，新）：UNKNOWN/无效电源被错误表达为外接供电，且顶栏/抽屉不一致

证据：`battery_service.c:146-152` 明确说明板上没有 MCU 可见 VBUS，`classify_power()` 永不返回 EXTERNAL；`nuwax_ai_partner_main.c:644-647`、`app_shell.c:633-636` 把 UNKNOWN/invalid 映射到 UI EXTERNAL；`drawer.c:276-305` 显示“外接供电”，而 `status_bar.c:795-807` 的 EXTERNAL 分支返回 `POWER_BATTERY` 图标，抽屉则返回 external 图标。

影响：用户、日志和 UI 诊断会把“没有可信电量数据/未验证电源”误读为“已接外部电源”。当前 OTA 对 UNKNOWN 仍 fail-closed，所以这是语义和运维风险，不直接证明会放行危险 OTA。

建议：引入独立 `UNKNOWN` UI 状态或 `external_power_verified` 能力位；只有新增并验证 VBUS/PG 输入后才允许显示 external。顶栏和抽屉必须共用同一个状态到图标/文案映射；invalid/stale 统一显示“电量未知/读取失败”，不得显示外接供电。

### BAT-003（P2，新）：未认证 SOC/低电 flag 参与产品低电决策

证据：`battery_service.c:162-168` 在 `soc_trusted` 计算前更新 `low_latched`，输入是一次采样的 `status->is_low` 或原始 SOC；`bq27220_driver.c:504-510` 的 `is_low` 又来自 SOC/TDA/FD。随后 UI 和 OTA untrusted 分支均读取 `snapshot.low`。

影响：CEDV/profile 未认证或不匹配时，估算 SOC/告警 flag 可能错误触发低电 UI、低电表情和 OTA 拒绝；产品同时把“估算值”与“安全低电结论”混在一个字段中。它主要造成误报/误拒绝，当前没有自动关机路径，因此等级为 P2。

建议：分开 `soc_display_estimate`、`hard_low_voltage`、`gauge_low_flag` 和 `product_low_decision`；未认证 SOC 只用于带 `~` 的显示，产品低电必须由经过 profile 认证的 SOC 或经批准的硬电压/硬件电源信号触发。每个决策记录 reason/source，并为 charging/low 的优先级建立单一状态表。

### BAT-004（P1，新）：OTA 电源门存在无统一硬门槛的放行分支

证据：

- `nuwax_ota_device.c:731-746` 在 `charging && power_state==CHARGING` 时直接返回允许，不检查 SOC、电压、low flag、连续样本、充电时长或 VBUS/PG；`is_charging` 只是 `current_ma > 20` 与几个 gauge flags 的一次推导。
- 同一函数的 trusted SOC 分支 `:738-745` 只检查 `percent < CONFIG_NUWAX_OTA_MIN_BATTERY_PERCENT`，不检查 `battery.low` 或统一的硬电压下限。
- 新镜像 `nuwax_ai_partner_main.c:4782-4797` 复用了同样的 charging 直通和 trusted SOC 分支。因此 OTA 下载前和 pending-verify 不是两个独立、更加保守的电源门。

影响：在低电压、瞬态/错误充电电流、充电检测抖动、SOC 与 flags 不一致或输入电源不足时，当前代码仍可能开始 OTA 或确认候选镜像。双槽/回滚保护不能抵消下载中掉电、反复重启或设备进入不可用状态的风险。

建议修复方向（本轮不改代码）：

1. 建立一个唯一的 `battery_ota_admission()`，供 command preflight 和 boot health 共同调用；任何分支先满足 fresh、service running、valid、硬电压下限和无 low/fault flags。
2. charging 只能作为“输入电源稳定性”的附加证据，要求连续 N 次采样、最小充电电流/电压窗口，以及真实 VBUS/charger-PG（若硬件可提供）；没有 VBUS/PG 时不得声称 external。
3. 明确可信 SOC 与硬电压下限的产品数值，并把值写入产品 overlay，而不是依赖 Kconfig 默认 20% 与当前 `sdkconfig` 30% 的漂移。
4. 对 OTA 下载中电压跌落、充电拔出、I2C stale、brownout 做 D 级注入；失败必须停止写入、保留旧槽并产生可追溯终态。

## 6. 实现但未接入的电池洞察

`nuwax_battery_compute_insights()` 能从快照计算温度状态、健康度、剩余时间、充满时间和 `low_power_recommended`，但 `rg` 未发现产品调用者；当前 CMake 仅把 `battery_insights.c` 编入镜像。该模块不写 gauge、不改 power state、不触碰 UI，属于“实现但未接入”的死功能。后续若产品要显示温度/健康度/时间，需显式接入状态快照并补 UI、OTA、告警和测试合同；在此之前不得称作已实现产品能力。

## 7. 关闭矩阵

| 等级 | 用例 | 关闭证据 |
|---|---|---|
| A | 源码/配置/状态矩阵与问题去重 | 本文、索引、执行记录；完成 |
| B | 纯状态模型：unknown/invalid/stale/charging/low/trusted 的组合与不变量 | host 可执行模型，证明 invalid 不会进入 allow，charging 不可绕过 hard floor |
| C | 干净 product OTA profile | `sdkconfig`、map、分区、镜像和阈值 hash；证明唯一 admission helper 被两处链接 |
| D | 电池/USB 充电/拔插/低压/读数错误/掉电 | 绑定 V2 板、样机电芯、run_id、波形、串口日志；验证 UI 文案、OTA拒绝/允许和旧槽恢复 |

## 8. A2-05 判定

- A 级静态审计：**完成**。
- 正常 I2C 错误保留和 stale fail-closed：**静态成立**，未做 B/D。
- “无效电量不触发错误关机”：**当前没有自动低电关机路径，静态成立**；仍需确认没有其他电源/应用策略在设备 profile 中接入。
- “无效电量不触发 OTA 且 UI 显示确定”：**未满足**。OTA 对 invalid/unknown 多数 fail-closed，但 UI 把 unknown/invalid 显示为 external；charging/trusted 分支还存在 BAT-004 放行风险。
- 新增 canonical 问题：`BAT-002` P2、`BAT-003` P2、`BAT-004` P1；既有 `BAT-001` 保持开放。
- B/D：**开放**。本文件不构成电量精度、外部电源、充电安全、OTA 安全或设备发布通过证明。
