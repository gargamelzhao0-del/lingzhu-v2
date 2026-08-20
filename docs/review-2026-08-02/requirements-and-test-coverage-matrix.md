# B0-17 需求、状态、错误路径与测试覆盖矩阵

审计日期：2026-08-03  
审计对象：`NuwaxAIPartner-v2` 当前文件系统快照及工作区级测试支撑资产  
计划项：B0-17，对应 `docs/2026-08-02-NuwaxAIPartner-v2全工程代码与产品稳定性评审计划.md:168`  
授权等级：A 级静态只读。除本证据文件外未修改源码、配置、索引或执行记录；未构建、未运行测试、未操作设备、未访问串口、未 OTA/烧录。

## 1. 目标、范围与结论边界

本审计建立需求、产品源码、状态迁移、错误路径和测试资产五类覆盖分母，并区分 static、host、gateway、build、device、hardware/acoustic 证据。目标不是证明产品已经通过，而是回答以下问题：

1. 当前可以稳定计数的覆盖对象有哪些；
2. 每个产品域具有什么级别的直接证据；
3. 哪些脚本只是源码文本契约，哪些测试会实际编译或执行产品逻辑；
4. 哪些历史日志或旧板数据不能用于证明 V2 当前固件；
5. 关闭 B0-17 以及后续发布门还缺少什么。

本文件中的 `R-CAND-*`、`STATE-CAND-*` 和 `ERR-CAND-*` 只是审计候选键，不是产品批准的规范性需求 ID 或正式测试 ID。当前没有发现稳定的 `requirement_id`、`需求ID` 或 `REQ-*` 目录，因此不能计算“已验证需求数/全部需求数”。这是本审计的首要结论，不得以本文的产品域行数替代规范性需求分母。

路径约定：未加前缀的路径均相对 `NuwaxAIPartner-v2/`；`../cloud-gateway/` 和 `../tools/` 是工作区级支撑资产。行号用于定位本次静态快照，源码变化后必须重新取证。

2026-08-03 A2-04 基线续接：source manifest 已更新为 7,918 项、SHA-256
`ba22f1e64a4b95e19cca8d32e789798f5a15ee8c1f8af6b8d38553cc5bf71450`。
新增 `main/conversation/realtime_log.h` 使第一方活动 C/H 分母从 349 变为
350（204 C + 146 H），但它是被正式 C 源包含的 header，不改变 OTA profile
150/非 OTA 145 的 CMake app source 分母。该 header 及受影响行号已同步进入
memory-safety 逐文件矩阵。后续监控 VAD、preroll 与 capture pause/resume ACK 改动没有改变文件分母，但扩大了 `R-CAND-MONITOR` 的语义表面；相关锚点已重绑，产品/云端 oracle 仍受 `PARITY-002` 阻塞。

2026-08-03 A2-06续接没有改变产品源码分母。离线能力静态审计确认：UI/touch/SW3 task在网络前建立，但wake、speaker/Wi-Fi worker和shell action owner被首次联网/WS生命周期门控；新增`OFFLINE-001/OFFLINE-002`。对应B/D缺口是冷启动断网、云死、shell action跨断线、resource soak和本地wake/sleep行为，不能由现有gateway脚本代替。

2026-08-03 A2-07续接没有改变文件分母，但并行开发使7个纳入文件发生hash漂移。按最新 `app_main()`、speaker和WebSocket实现复核后，启动失败覆盖仍缺少可执行的逐cut fault injection：全局startup没有owner ledger和逆序rollback，Wi-Fi/LVGL/OTA存在partial init，static task成功后TCB/stack owner丢失；现有display/audio/wake/controller局部cleanup只能算A级正向控制。A2-07复用`RTOS-008/RTOS-009/LIFE-004/LIFE-010/ERR-001`，B级需要allocation/register/init逐点失败和第二次init断言，D级需要低内存、driver错误、OTA pending和1000次启动恢复。审计结束后必须以最终manifest重跑受漂移结论复核，当前中间快照不构成发布证据。

2026-08-03 A2-08续接没有改变源码分母。按当前150个正式app C源和bootloader源复算，fatal/restart站点仍为3/32/3/4/0/0；当前UART为921600 baud，日志调用为1,481处。A2-08确认当前没有持久core dump、fatal breadcrumb或crash-loop决策；16 MiB分区零空余且app-only OTA不能迁移partition table，24 KiB NVS也没有容量/磨损/诊断ring预算。对应C级缺口是release panic/WDT/console/dump/partition/profile构建证据，D级缺口是逐fatal站点、断console、连续复位、NVS损坏和1000次recovery验证；A证据不能替代这些行为结果。

## 2. 证据等级与判定规则

| 等级 | 定义 | 可以证明 | 不能证明 |
|---|---|---|---|
| A - static | 对当前源码、配置、协议、脚本和已有证据做只读检查，记录路径、行号、计数与 hash | 对象存在、静态调用/状态/错误分支、测试入口关联、配置意图 | 分支实际执行、时序、资源水位、目标机行为、声学效果 |
| B - host | 固定源码/config hash 后，实际编译并运行 host check 或纯逻辑测试，保存命令、退出码、stdout/stderr | 被编译逻辑在 host stub 条件下的确定性行为 | ESP-IDF 链接、FreeRTOS/驱动真实时序、硬件和声学行为 |
| B - gateway | 固定设备协议版本和网关源码 hash 后，实际运行 Node 网关/协议测试 | 网关解析、状态机、HTTP/WS 协议和异常应答 | 设备固件已正确发送/接收、真机重连和资源稳定性 |
| C - build | 对唯一产品 profile 执行干净 configure/build/link，并保存最终 `sdkconfig`、map、镜像和 hash | 编译、链接、实际成员、分区/镜像容量、配置分支 | 功能路径实际运行、设备和声学验收 |
| D - device | 在指定 V2 设备和镜像上执行可复现用例，保存 `run_id`、设备 ID、串口/服务日志和结果 | 真机协议、状态迁移、重连、功耗、时序、长稳行为 | 未测环境、未测硬件批次和声学场景 |
| D - hardware/acoustic | 在指定硬件版本、距离、音量、噪声、仪器和校准条件下保存原始音频/波形及分析结果 | AEC、双讲、人声保护、触摸、电量、电源和物理接口的验收结果 | 其他板版、其他装配、无绑定的新固件 |

适用规则：

1. 证据等级不自动继承。A 级源码正则检查不能记为 B 级行为覆盖；B 级 host 通过不能记为 C 级链接或 D 级真机通过。
2. “入口存在”不等于“本批次已运行”，“测试文件存在”不等于“已由默认或发布 runner 注册”。
3. 文本契约只证明特定字符串/排序仍存在。`tools/run-conversation-host-checks.ps1:202-225`、`tools/run-v2-product-host-checks.ps1:93-118`、`tools/run-provisioning-host-checks.ps1:23-60` 中的 `Get-Content`/正则检查继续归类为 A。
4. C/D 证据至少绑定 `run_id`、UTC/本地时间、源码 hash、最终配置 hash、镜像 hash、设备唯一标识、硬件版本、完整命令、退出码和原始日志；声学证据还必须绑定原始音频 hash、距离、方位、音量、噪声和标注者。
5. 缺少固件或设备身份的历史日志只能作为问题线索，不能关闭当前 V2 的需求、状态或错误路径。
6. V1.2、COM8 或未知板版的结果不能证明 V2.0/COM5；模拟器结果不能证明设备驱动。
7. 任何分母变化都必须保留旧分母、变更原因和新 hash，禁止用缩小分母的方式提高覆盖率。

## 3. 覆盖分母总账

### 3.1 分母状态摘要

| 分母 | 当前值 | 是否闭合 | 说明 |
|---|---:|---|---|
| 规范性需求 | 0 个稳定需求 ID | 否 | 未发现可追踪的需求目录；本文件只能建立候选产品域 |
| 第一方活动 C/H | 350（204 C + 146 H） | 是，限文件系统分母 | 新增 `main/conversation/realtime_log.h`；逐文件证据见 memory-safety 矩阵 |
| 产品 app 源文件 | OTA profile 150；非 OTA 145 | 是，限 `main/CMakeLists.txt` 源表 | 新增正式 `face_path_blob.c`；OTA 由 `sdkconfig.product-ota.defaults:16` 启用；源表见 `main/CMakeLists.txt` |
| bootloader 第一方源文件 | 1 | 独立账闭合 | `bootloader_components/nuwax_power_hold/CMakeLists.txt:1` |
| JSON compatibility component 源文件 | 1 | 源表闭合，最终链接未闭合 | `components/json/CMakeLists.txt:1`；是否进入产品镜像仍需 C 级 map 证明 |
| 活动但未进入 app 源表 | 44 | 文件清单闭合，处置未闭合 | 36 个 host 资产和 8 个非 host 文件，见 `docs/review-2026-08-02/module-inventory.md:38-42` |
| app 源文件直接 host 关联 | 45/150 | 关联账闭合，行为覆盖未闭合 | 26 个被 host 编译，27 个被文本契约读取，并集 45；105 个没有直接 host 关联 |
| host check C 文件 | 36 | 文件账闭合，runner 账未闭合 | 31 个被 PowerShell runner 引用，5 个孤立 |
| PowerShell host runner | 6 | 入口账闭合，执行未授权 | conversation、product、meeting、monitoring、provisioning、wake-word |
| UI host runner | 1 | 入口账闭合，执行未授权 | `test/host/run_tests.sh:32-53`，6 个产品 UI 源和 7 个测试文件 |
| gateway `test-*.js` | 29 | 文件账闭合，注册未闭合 | npm scripts 仅注册 14 个；15 个未注册；默认 `npm test` 仅运行 3 个，见 `../cloud-gateway/package.json:7-13` |
| V2 原始声学样本 | 0 | 否 | manifest 有 26 行元数据，但引用的物理音频均缺失且板版是 V1.2 |
| 顶层状态轴 | 至少 11 | 否 | app、awake、prelisten、turn、Wi-Fi、WS、cloud、meeting、monitoring、OTA、shutdown；还需纳入子状态轴 |
| 语义错误路径 | 未知 | 否 | 已有词法候选计数，但尚未语义去重并赋稳定 ID |

六个 PowerShell 入口分别为 `tools/run-conversation-host-checks.ps1`、`tools/run-v2-product-host-checks.ps1`、`tools/run-meeting-host-checks.ps1`、`tools/run-monitoring-host-checks.ps1`、`tools/run-provisioning-host-checks.ps1` 和 `tools/run-wake-word-host-check.ps1`。

### 3.2 产品 app 源文件分布

产品分母采用 OTA 启用 profile 的 150 个唯一 app 源条目；不把 bootloader 源、兼容组件、host check、生成器或网关测试混入同一分母。

| 模块 | app 源文件数 | 占 150 比例 |
|---|---:|---:|
| `main/` 根目录 | 27 | 18.0% |
| `main/board/` | 15 | 10.0% |
| `main/conversation/` | 24 | 16.0% |
| `main/audio_modes/meeting/` | 3 | 2.0% |
| `main/audio_modes/monitoring/` | 2 | 1.3% |
| `main/nuwax_ui_v28/` | 70 | 46.7% |
| `main/nuwax_ota_new/` | 5 | 3.3% |
| `main/nuwax_provision_new/` | 4 | 2.7% |
| 合计 | 150 | 100.0% |

### 3.3 直接 host 关联分布

“编译”表示某个项目 host 入口直接编译产品 `.c`；“文本契约”表示 runner 读取产品源码后用字符串或正则检查；“并集”对同一源文件去重。该表是静态关联率，不是语句、分支或行为覆盖率。

| 模块 | 正式源 | host 编译 | 文本契约 | 并集 | 无直接关联 |
|---|---:|---:|---:|---:|---:|
| board | 15 | 0 | 1 | 1 | 14 |
| conversation | 24 | 10 | 9 | 16 | 8 |
| main root | 27 | 4 | 9 | 10 | 17 |
| meeting | 3 | 3 | 1 | 3 | 0 |
| monitoring | 2 | 2 | 1 | 2 | 0 |
| OTA | 5 | 0 | 1 | 1 | 4 |
| provisioning | 4 | 0 | 2 | 2 | 2 |
| UI | 70 | 7 | 3 | 10 | 60 |
| 合计 | 150 | 26 | 27 | 45 | 105 |

对应比率：host 编译关联 26/150 = 17.3%，文本契约关联 27/150 = 18.0%，去重并集 45/150 = 30.0%，无直接 host 关联 105/150 = 70.0%。工程内未发现 gcov、lcov、llvm-cov 或等价覆盖率插桩，因此不能把 30.0% 换算为行/分支覆盖率。

## 4. 需求候选矩阵

下表只建立产品域候选和证据缺口。`A` 表示已定位当前实现；`B` 列的“入口存在”不表示本轮运行；`C/D` 均需后续授权。

| 候选键 | 产品义务 | 主要代码锚点 | A 静态 | B host/gateway | C build | D device/hardware | 当前判定 |
|---|---|---|---|---|---|---|---|
| `R-CAND-BOOT` | V2 上电保持、SW3 单击关/开机、异常路径不破坏电源锁存 | `bootloader_components/nuwax_power_hold/nuwax_early_power_hold.c:51-55`；`main/board/nuwax_power_ctrl.c:266-285`；`main/nuwax_ai_partner_main.c:15537-15551` | 已定位 owner、gate 和启动顺序 | 仅少量产品文本契约；无 bootloader host 行为 | 未执行 | 需 V2 冷启动、重复点击、OTA/配网互锁和掉电波形 | 未完成 |
| `R-CAND-AUDIO-DRV` | ES7210/ES8311/PA/I2S 路由可初始化、可恢复，采播格式和引脚符合 V2 原理图 | `main/board/audio_board.c:114-268`；`main/board/esp_vocat_v1_0_pins.h:41-69`；`main/board/vocat_audio_board.c:333-691` | 实现和引脚候选已定位 | board 仅 1/15 源有文本关联，0 个被 host 编译 | 未执行 | 必须示波器/I2S/真实采播、codec 寄存器和硬件 AEC 路径验收 | 未完成，高硬件依赖 |
| `R-CAND-HMI-DRV` | 屏幕、触摸、LED、电量采样及 UI 映射可靠，I2C/SPI 故障可恢复 | `main/display.c:915-1037`；`main/touch_input.c:549-605`；`main/board/battery_service.c:142-250,323-347`；`docs/review-2026-08-02/phase2-battery-power-state-matrix.md` | 主路径、错误保留和14类电源/UI状态已定位；UNKNOWN 被错映射 external，见 BAT-002/003 | UI 纯逻辑入口存在；驱动和电源策略无直接 host 行为 | 未执行 | 需点击/滑动、亮度、LED、电池充放电、VBUS/PG和 I2C 故障注入 | 未完成；电量精度与UI语义开放 |
| `R-CAND-WIFI` | 保存网络连接、回退、断连重连和状态发布一致 | `main/wifi_manager.c:244-298`；`main/wifi_manager.c:477` | 事件、重试和 NVS 路径已定位 | product 文本契约；gateway 不能替代设备 Wi-Fi | 未执行 | 需 AP 消失、密码错误、弱网、DHCP 和长稳 | 未完成 |
| `R-CAND-PROV` | BLE 配网能原子更新 Wi-Fi、正式云 URL/设备凭证，失败回滚且不泄露密钥 | `main/nuwax_provision_new/nuwax_cloud_config.c:214-299`；`main/nuwax_provision_new/nuwax_provision_main.c:1288-1492` | transaction/rollback 静态可见 | provisioning runner 含 1 个编译入口和文本契约；未运行 | 未执行 | 需手机 BLE、错误密码、中断、重复配网、重启后持久化 | 未完成 |
| `R-CAND-WS` | WS/TLS 连接、认证、业务 ready、收发、关闭和重连无死锁且状态准确 | `main/ws_transport.h:17-30`；`main/nuwax_ai_partner_main.c:3568-3678` | lifecycle 与 cloud 三态已定位 | host 有部分 lifecycle/文本契约；gateway 有 hello/协议脚本 | 未执行 | 需 TLS 异常、401/403、半开连接、云端静默和重连 soak | 未完成 |
| `R-CAND-CONV` | 唤醒后持续完成采集、上行、云等待、TTS 播放、自动跟进和休眠循环 | `main/app_state.h:3-13`；`main/nuwax_ai_partner_main.c:8244`；`main/nuwax_ai_partner_main.c:13611`；`main/conversation/turn_guard.h:11-28` | 主状态和 owner 已定位 | conversation/product host 入口存在；网关 conversation 脚本存在 | 未执行 | 需正式协议、真人多轮、2 分钟 idle 边界和断网恢复 | 未完成 |
| `R-CAND-WAKE` | 睡眠态检测目标唤醒词，生命周期安全，误唤醒/漏唤醒达到阈值 | `main/nuwax_ai_partner_main.c:2291-2541`；`main/wake_word_detector.c:577-671`；`main/wake_word_detector.c:728-877` | 生命周期和阈值路径已定位 | wake-word runner 存在；未运行 | 未执行 | 需 V2 麦克风、距离/方位/噪声数据集 | 未完成 |
| `R-CAND-AEC-BARGE` | 任意设备音量下不被自身 TTS 误打断；用户开口快速停播；保留首音节并消除重叠回声 | `main/conversation/live_aec.c:2293-3514`；`main/conversation/barge_in_controller.c:97-891`；`main/nuwax_ai_partner_main.c:5786-6503` | DSP、保护和 fail-closed 路径已定位 | 多个 host 入口存在，但 5 个关键 check 未接 runner；历史 replay 不代表当前 V2 | 未执行 | 必须有当前 V2 原始多通道音频、全音量/距离/噪声/双讲验收 | 未完成，不能由旧日志放行 |
| `R-CAND-MEETING` | start/active/stop/error、连续上传、序号、END、停播前置条件和失败回收正确 | `main/audio_modes/meeting/meeting_mode.h:16-45`；`main/audio_modes/meeting/meeting_mode.c:561-933` | 3/3 源均有直接 host 关联 | meeting runner 与 gateway meeting 脚本存在；未运行 | 未执行 | 需 V2 长时录音、断网/停播失败、WAV/序号/会话一致性 | 未完成 |
| `R-CAND-MONITOR` | 按冻结的 v1 连续流语义，或按经批准的新分段/latest-data语义，实现 enable/active/disable/error、静默 pause、人声 resume、ACK、序号、结束和恢复 | `main/audio_modes/monitoring/monitoring_mode.h:15-42`；`main/audio_modes/monitoring/monitoring_mode.c:266-535`；`main/audio_modes/monitoring/monitoring_mode.c:1025-1577`；`docs/review-2026-08-02/v1-v2-product-feature-parity.md:188-218` | 2/2 源均有直接 host 关联；新增分段控制已定位，产品语义仍受 `PARITY-002` 阻塞 | monitoring runner 存在；网关需显式聚合入口 | 未执行 | 产品/云端先冻结唯一语义，再执行 V2 一小时、断网、旧流终止、ACK、序号和音频文件验收 | 未完成；决策阻塞 |
| `R-CAND-OTA` | 认证后下载、写入、校验、切换、确认/回滚、断电恢复、版本和电源策略正确 | `main/nuwax_ota_new/nuwax_ota.h:18-25`；`main/nuwax_ota_new/nuwax_ota.c:157-198`；`main/nuwax_ota_new/nuwax_ota_device.c:716-746,1191-1310`；`main/nuwax_ai_partner_main.c:4757-4802` | 状态机和设备编排已定位；充电/trusted SOC 分支缺统一硬电压与可信供电门，BAT-004 P1 | 仅 1/5 源有文本关联；gateway OTA 脚本存在；无 battery admission host 模型 | 未执行 | 需当前镜像 hash、低压/充电拔插/I2C stale、HTTP 故障、断电、回滚、跨网与耗时验收 | 未完成；BAT-004阻塞产品OTA |
| `R-CAND-UI` | 仅 UI owner 写 LVGL，完整快照/revision 不丢状态，显示语义与业务 ready 一致 | `main/app_shell.c:76-94`；`main/app_shell.c:160-215`；`main/nuwax_ui_v28/nuwax_state_bridge.c:351`；`main/nuwax_ui_v28/nuwax_status_bar.c:814-831` | mailbox、revision 和 READY 映射已定位 | 6 个产品 UI 源进入 bash host runner；其余 60/70 无直接关联 | 未执行 | 需真机并发状态、触摸、图标、掉帧和长稳 | 未完成 |
| `R-CAND-PERSIST` | Wi-Fi/云配置、OTA 状态、schema 和失败恢复跨重启兼容 | `main/wifi_manager.c:58-201`；`main/nuwax_provision_new/nuwax_cloud_config.c:115-183`；`main/nuwax_ota_new/nuwax_ota.h:68-71` | 存储路径已定位，已有独立兼容审计 | 部分 provisioning/OTA 文本契约 | 未执行 | 需旧 schema、写失败、断电和升级/回滚矩阵 | 未完成 |
| `R-CAND-DIAG` | MCP/诊断只公布有效能力，不阻塞产品 owner，不泄露敏感信息 | `main/mcp_server.c:81-228`；`main/CMakeLists.txt:1-75` | 入口和能力实现可静态检查 | MCP capability host 入口见 `tools/run-v2-product-host-checks.ps1:88-91` | 未执行 | 需设备命令总线、并发、权限和错误回读 | 未完成 |
| `R-CAND-SEC` | 设备身份、凭证、配网 PoP、OTA 签名、Secure Boot、Flash/NVS Encryption 和 anti-rollback 达到量产门 | `main/nuwax_provision_new/nuwax_provision_main.c:1119-1310`；`main/nuwax_ota_new/nuwax_ota_manifest.c:1` | 安全机制候选可见，但规范/量产 profile 尚未闭合 | 协议脚本只能覆盖格式/拒绝路径 | 未执行 | 需签名仪式、熔丝/加密、轮换/吊销和攻击测试 | 发布阻塞 |

## 5. 状态转移覆盖矩阵

### 5.1 分母定义

状态覆盖分母必须是去重后的迁移四元组：

`(state_axis, from_state, event_or_guard, to_state)`

并额外建立两类用例：

- 每个状态轴的非法迁移、重复事件、超时和失败恢复；
- 跨轴不变量，例如 `OTA active -> audio blocked + shutdown inhibited`、`meeting active XOR monitoring active`、`service_ready -> transport_connected AND authenticated`。

枚举值数量不能替代迁移数量。当前在活动、非 host 的 `main/` 树中发现 55 个文件包含 90 个 enum 定义；这些只是发现候选。已明确的 app、turn、WS、meeting、monitoring、OTA 六个枚举共 36 个状态值，加上 shutdown 的 3 个原子 gate 状态，共 39 个已知状态值，也仍不是状态覆盖分母。

| 已明确状态轴 | 已知状态值 | 代码锚点 |
|---|---:|---|
| app presentation/conversation | 9 | `main/app_state.h:3-13` |
| turn guard | 5 | `main/conversation/turn_guard.h:11-17` |
| WS lifecycle | 6 | `main/ws_transport.h:17-24` |
| meeting | 5 | `main/audio_modes/meeting/meeting_mode.h:16-22` |
| monitoring | 5 | `main/audio_modes/monitoring/monitoring_mode.h:15-21` |
| OTA core | 6 | `main/nuwax_ota_new/nuwax_ota.h:18-25` |
| shutdown gate | 3 | `main/board/nuwax_power_ctrl.c:43-45`、`main/board/nuwax_power_ctrl.c:266-285` |

### 5.2 状态轴覆盖账

| 候选键 | 轴/关键迁移族 | A 静态状态 | B 入口状态 | 必须补齐的转移与证据 |
|---|---|---|---|---|
| `STATE-CAND-APP` | BOOTING -> WIFI/WS -> IDLE/SLEEPING/LISTENING/THINKING/SPEAKING/ERROR | enum 和主要写入点已查；完整边表未赋稳定 ID | 部分文本契约 | 全部 writer、重复事件、错误恢复、UI 同步；B + D |
| `STATE-CAND-AWAKE` | sleeping -> wake generation -> active budget -> idle timeout -> sleeping | phase 1 已识别为独立轴 | conversation host 有部分逻辑 | generation 交错、两分钟边界、云等待期间活动续期；B + D |
| `STATE-CAND-PRELISTEN` | inactive -> capture active -> promoted/cancelled -> inactive | 结构和调用可静态追踪 | conversation host 部分覆盖 | WS 断连、speaker 状态变化、重入、stale flag；B + D |
| `STATE-CAND-TURN` | INACTIVE -> WAIT_TEXT -> WAIT_TTS -> PLAYING -> DRAINING -> INACTIVE | phase、ID/generation guard 明确 | turn guard 被 host 编译 | 非法边、旧 epoch、重复 TTS/end、stop failure；B + gateway + D |
| `STATE-CAND-WIFI` | init/disconnected/connecting/connected/fallback | event bits、重试和 NVS 路径已定位 | 以文本关联为主 | auth fail、AP missing、DHCP timeout、fallback 竞态、长断网；B + D |
| `STATE-CAND-WS` | IDLE -> CONNECTING -> OPEN -> CLOSE_REQUESTED/RX_EXITED/FAULTED -> IDLE | enum 和 owner 模型明确 | 部分 host lifecycle | connect cancel、RX 先退出、锁/close timeout、TLS 半开、epoch 旧帧；B + D |
| `STATE-CAND-CLOUD` | transport/auth/service 三个正交布尔轴 | `main/nuwax_ai_partner_main.c:3568-3678` 已拆分 | hello gateway 脚本存在 | 101、hello ACK、正向业务活动、401/403、服务静默的全部组合；B gateway + D |
| `STATE-CAND-MEETING` | DISABLED -> STARTING -> ACTIVE -> STOPPING -> DISABLED/ERROR | enum、owner 和 3/3 源关联 | host 入口存在 | stop playback fail、session create/send fail、断网、重复 enable/disable；B + gateway + D |
| `STATE-CAND-MONITOR` | DISABLED -> STARTING -> ACTIVE -> STOPPING -> DISABLED/ERROR | enum、owner 和 2/2 源关联；流内 ARMED/STREAMING 只描述当前实现，不代表 v1 产品决策 | host 入口存在 | 先关闭 `PARITY-002`，再按唯一协议验证连续流或分段/latest-data、旧 session、断网、重复控制、queue overflow；B + gateway + D |
| `STATE-CAND-OTA` | IDLE -> DOWNLOADING -> WRITING -> VERIFYING -> DONE/FAILED -> retry/rollback | core enum 和 transition 函数已定位 | 以文本契约为主 | cancel、manifest reject、短写、断电、pending verify、云 ACK、rollback；B + C + D |
| `STATE-CAND-SHUTDOWN` | OPEN <-> INHIBITED；OPEN -> CLAIMED；失败 -> OPEN | atomic gate 可见 | 无直接 host 驱动行为 | SW3 抖动、OTA/配网互锁、GPIO/marker/wakeup 失败和真实掉电；B pure logic + D hardware |
| `STATE-CAND-SPEAKER` | idle/buffering/playing/draining/stopping/error 及 generation | worker/queue/回调路径已发现 | speaker 逻辑部分被编译 | RX 背压、underrun、stop timeout、旧 generation、fade/cancel；B + D acoustic |
| `STATE-CAND-WAKE` | stopped/starting/running/exited/reaped/fail | lifecycle API 已发现 | wake runner 存在 | init 每个失败点、stop/reap timeout、MIC owner 竞争、重复 start；B + D |
| `STATE-CAND-PROVISION` | unprovisioned/staged/connected/verified/committed/rollback/stopped | transaction 顺序可静态识别 | runner 有文本契约 | BLE disconnect、错误 Wi-Fi、NVS 写失败、重启恢复、in-flight stop；B + D |
| `STATE-CAND-AEC` | unavailable/reserved/active/degraded/fail-closed + human protection gates | live AEC 和保护路径已发现 | 若干 host check 存在，部分孤立 | ref loss、delay jump、双讲、音量全范围、fallback、恢复；B + D acoustic |
| `STATE-CAND-UPLINK` | capture/encode/enqueue/send/backpressure/stop | audio_uplink 被 host 编译和文本检查 | conversation host 入口存在 | queue full、WS close、sequence、deadline、stale audio、模式差异；B + gateway + D |
| `STATE-CAND-OTA-DEVICE` | command accepted/quiescing/worker active/result pending/cloud ACK/reboot | device wrapper 静态路径已发现 | product 文本契约 | core 与 device 双状态组合、断连 ACK、shutdown release、worker init rollback；B + D |

状态分母仍未闭合的原因是：尚未给每条合法/非法边和跨轴不变量分配稳定 ID，也没有把 90 个 enum 候选逐一分类为产品状态、数据枚举、协议枚举或 host-only 枚举。故当前不得报告状态覆盖百分比。

## 6. 错误路径覆盖矩阵

### 6.1 词法候选分母

对 150 个正式 app C 源的静态候选统计如下。类别高度重叠，例如同一个 `if (err != ESP_OK)` 可能同时包含 timeout、`goto fail` 和非 OK 返回，因此不能相加为错误路径总数。

| 候选模式 | 命中数 | 涉及文件 | 解释 |
|---|---:|---:|---|
| 直接返回非 OK 错误 | 682 | 54 | 包括向上传播和局部转换，不等于独立失败路径 |
| `goto` | 148 | 15 | 包括正常 cleanup 和错误回滚 |
| `ESP_ERROR_CHECK*` | 35 | 11 | 可能触发 abort/restart，需逐项判断产品可接受性 |
| `!= ESP_OK` / `== ESP_ERR_*` 词法比较 | 699 | 45 | 同一路径可能多次检查同一错误；未剥离注释、字符串或条件编译分支 |
| `ESP_ERR_TIMEOUT` token | 106 | 25 | 包括返回、比较和映射；不是独立超时路径数 |
| abort/restart 调用 | 7 | 6 | 需区分有意恢复、诊断和不可接受重启 |

正式错误路径分母应由语义去重后的 `(owner, operation, injected_failure, observed_effect, cleanup_or_recovery)` 构成，并为每条路径标注可注入层级和期望可观察结果。

### 6.2 错误路径候选账

| 候选键 | 错误族 | 代表代码 | A 静态状态 | 后续关闭条件 |
|---|---|---|---|---|
| `ERR-CAND-INIT` | 分段初始化失败和逆序回滚 | `main/display.c:915-1037`；`main/nuwax_provision_new/nuwax_provision_main.c:1080-1193` | 分支可见，尚未逐失败点编号 | 对每个 allocation/register/init 点做 host fault injection 或 D 级故障注入，证明无泄漏/残留 owner |
| `ERR-CAND-ALLOC` | heap/PSRAM/queue/task/mutex 创建失败 | `main/nuwax_ai_partner_main.c:1729-1866`；`main/touch_input.c:549-605` | 部分采用 fail-closed；全工程语义未去重 | B 级 allocator/queue/task 注入，C 级内存 profile，D 级低内存压力 |
| `ERR-CAND-TIMEOUT` | 有限等待超时、零 tick、无限等待和迟到完成 | `main/ws_transport.h:26-30`；`main/board/vocat_audio_board.c:825-875` | 106 个 `ESP_ERR_TIMEOUT` token 仅为候选 | 建立每个 wait 的 owner/deadline/recovery ID，按 100 Hz/1000 Hz tick profile 验证 |
| `ERR-CAND-WS` | TLS/WS 连接、读写、close/join、epoch 和重连失败 | `main/ws_transport.h:17-30`；`main/nuwax_ai_partner_main.c:3568-3678` | 状态与错误接口已定位 | B mock transport + D 云断连/半开/401/403/静默 soak；不得出现永久锁等无日志失活 |
| `ERR-CAND-PROTOCOL` | 非法 JSON、未知 type、缺字段、乱序/重复/旧 session | `main/session_protocol.c:1`；`main/audio_modes/meeting/meeting_protocol.c:1`；`main/audio_modes/monitoring/monitoring_protocol.c:1` | parser/dispatch 候选已定位 | host parser、gateway 双向 conformance、D 原始帧绑定同一协议版本 |
| `ERR-CAND-AUDIO` | codec/I2S 初始化、读写、采集终止、speaker underrun/stop 失败 | `main/board/audio_board.c:114-142`；`main/conversation/speaker_playback.c:1` | 静态路径分散，驱动几乎无 host 关联 | B 级 pure policy + D 级 I2C/I2S/PA 故障注入和资源恢复 |
| `ERR-CAND-AEC` | reference 不可用、delay 不可信、DSP 失败、near-end 保护不确定 | `main/conversation/live_aec.c:2433-3514`；`main/nuwax_ai_partner_main.c:1681-1866` | fail-closed 意图可见 | 运行 5 个孤立 check；建立 V2 原始音频 corpus；D acoustic 验证误打断/漏打断 |
| `ERR-CAND-MODE` | 会议/监控停播失败、session 建立失败、capture ACK 超时/拒绝、queue/WS 中断 | `main/audio_modes/meeting/meeting_mode.c:561-933`；`main/audio_modes/monitoring/monitoring_mode.c:1025-1577` | 状态和协议源已定位；监控 oracle 受 `PARITY-002` 阻塞 | B 全失败分支 + gateway 故障 + D 长时断网，验证不创建错误 session；监控丢弃/分流规则必须来自批准协议 |
| `ERR-CAND-OTA` | manifest/auth/download/short write/hash/partition/ACK/boot verify/rollback 失败 | `main/nuwax_ota_new/nuwax_ota.c:157-198`；`main/nuwax_ota_new/nuwax_ota_device.c:1191-1310` | 状态机可见，4/5 源无直接 host 关联 | B mock transport/partition，C 分区和链接，D 断电/损坏/回滚和耗时 |
| `ERR-CAND-PERSIST` | NVS 缺失、schema 不兼容、写失败、staged config 重启 | `main/wifi_manager.c:58-201`；`main/nuwax_provision_new/nuwax_cloud_config.c:214-299` | 事务 API 可见 | B NVS stub + D 断电矩阵，核对旧版本迁移和恢复策略 |
| `ERR-CAND-SHUTDOWN` | GPIO/RTC wake/marker/静音失败和关键操作互锁 | `main/board/nuwax_power_ctrl.c:150-183`；`main/board/nuwax_power_ctrl.c:232-285` | 错误日志和 gate 可见 | B gate model + D 波形/掉电/唤醒，失败必须可观察且不损坏 OTA/NVS |
| `ERR-CAND-FATAL` | `ESP_ERROR_CHECK*`、abort、restart、WDT 和不可恢复降级 | 35 个 check/7 个 abort-restart 候选；独立证据见 `docs/review-2026-08-02/phase1-fatal-reset-observability-audit.md:1` | 候选已有独立审计，尚未映射到需求/测试 ID | 每个 fatal 点指定原因、持久化诊断、重启预算和 B/C/D 验证；禁止重启风暴 |

## 7. 测试资产与证据矩阵

| 资产/入口 | 静态清点 | 实际覆盖语义 | 当前证据等级 | 缺口/限制 |
|---|---|---|---|---|
| 6 个 PowerShell host runner | conversation、product、meeting、monitoring、provisioning、wake-word | 31 个编译输出；runner 中共 73 个 `throw` token | A：入口存在；本轮未执行 B | 新增 NXFP parser check；`throw` 数不等于断言或分支数；需固定 compiler/stub/config/hash 后运行 |
| 36 个 `*_host_check.c` | 31 个被 runner 引用，5 个孤立 | 纯逻辑、协议和契约检查 | A | 孤立项见下表；未进入 runner 就不能作为发布回归证据 |
| UI bash host test | `test/host/run_tests.sh:32-53` 编译 6 个产品 UI 源和 7 个测试文件 | menu style/data/Wi-Fi/status/wheel/surface pure logic | A：入口存在 | 需在记录版本的 Bash/WSL 运行；独立 product runner另静态关联 NXFP parser，合计只直接编译 7/70 UI 源 |
| conversation 文本契约 | `tools/run-conversation-host-checks.ps1:202-225` | 检查源码文本、配置和特定区块 | A | 即使 runner 返回 PASS，该部分仍不是行为执行覆盖 |
| product 文本契约 | `tools/run-v2-product-host-checks.ps1:93-118` | Kconfig/defaults/CMake/源码字符串契约 | A | 不能证明链接分支或设备行为 |
| provisioning 文本契约 | `tools/run-provisioning-host-checks.ps1:23-60` | transaction 顺序和内存默认值正则 | A | 不能证明 NVS/BLE/Wi-Fi 事务实际完成 |
| gateway Node 测试 | 29 个顶层 `test-*.js`；npm scripts 注册 14 个 | hello、conversation、barge、meeting、onboarding、OTA 等网关逻辑 | A：入口清点 | 15 个不在 package runner；默认 `npm test` 仅 3 个；无 aggregate release gate/CI |
| 项目 release README | `README.md:93-100` 只列 product/conversation/meeting/monitoring | 文档化发布入口 | A | 遗漏 provisioning、wake-word、UI bash；与计划 `docs/2026-08-02-NuwaxAIPartner-v2全工程代码与产品稳定性评审计划.md:545-563` 不一致 |
| 协议 fixture 生成器 | `tools/protocol/generate-na-fixtures.js:253-280` | 生成 binary fixtures 和 manifest | 非测试；本轮未运行 | 会写工程树；后续 B 执行须用隔离输出目录或获得写授权，不能当只读 validator |
| 音频样本 manifest | 26 行：interrupt 10、non_interrupt 10、noise 6；25 labeled/pass、1 rejected | 仅元数据和人工结果声明 | A，且 stale/不适用 V2 | 26 个音频路径全部缺失；25 个 runtime log 存在但无当前身份链；全部 `device_profile=esp_vocat_v1_0`，其中 25 个为 `build-p6-gray-cancel-com8`、1 个为 `build-l3-ws120-q512-com8`；`test/audio_samples/README.md:36-40` 明确是 CyberVoc V1.2 |
| 音频 manifest validator | `../tools/check-audio-sample-manifest.ps1:2` 默认指向旧 `NuwaxAIPartner` | JSON 字段、ID、类别、标签和数量门 | A 级元数据校验 | `../tools/check-audio-sample-manifest.ps1:68-103` 不检查物理音频存在/hash；`:107-149` 只检查分类数量 |
| AEC historical replay | 默认八个 2026-07-14 日志，见 `tools/check-aec-human-protection-replay.ps1:18-39` | 对历史日志做阈值/事件回放，再检查当前源码文本 | A 级历史线索 | `tools/check-aec-human-protection-replay.ps1:353-356` 把旧日志和当前源码/config 混合；日志无固件、源码、配置身份，不能关闭 V2 |
| 既有 build 文档/镜像 | 已有 build reproducibility、partition 等静态证据 | 构建意图和历史产物 | A 或历史 C 候选 | 必须在当前 source/config hash 上重建并以 map 证明实际链接成员 |
| 设备/长稳/声学日志 | 工作区存在历史日志和人工描述 | 真机功能线索 | 历史 D 候选 | 未统一绑定 run/source/config/image/device/audio hash，不能形成当前覆盖率 |

### 7.1 未接入 runner 的 host check

以下 5 个文件存在，但未被六个 PowerShell runner 或 UI bash runner引用：

| 文件 | 风险域 | 处置要求 |
|---|---|---|
| `main/aec_delay_shadow_host_check.c` | AEC delay shadow | 接入唯一 runner，或以设计/替代测试证明后删除 |
| `main/aec_fir_delay_control_host_check.c` | FIR delay control | 同上 |
| `main/barge_in_detector_host_check.c` | 人声打断检测 | 接入 conversation release runner；必须与 V2 corpus 联动 |
| `main/p5_live_fast_gate_host_check.c` | live fast gate | 接入并冻结所用配置/阈值 |
| `main/p6_gray_confirm_host_check.c` | gray confirm/cancel | 接入并证明不是旧阶段遗留逻辑 |

## 8. 覆盖发现

| ID | 严重度 | 发现 | 客观证据 | 影响 | 关闭条件 |
|---|---|---|---|---|---|
| `COV-001` | P1 | 没有稳定的规范性需求目录和需求 ID，需求覆盖分母不可用 | 计划要求见 `docs/2026-08-02-NuwaxAIPartner-v2全工程代码与产品稳定性评审计划.md:168-187`；当前检索未发现 `requirement_id`、`需求ID`、`REQ-*` 体系 | 无法证明所有产品承诺均有实现和验收；也无法阻止测试只覆盖易测路径 | 建立批准的需求 catalog，每条含 owner、版本、验收条件、优先级、代码和测试映射；矩阵以稳定 ID 重建 |
| `COV-002` | P1 | 150 源文件的直接测试关联不足，且没有覆盖率插桩 | 仅 26/150 被 host 编译，27/150 被文本读取，并集 45/150；105/150 无直接关联；UI 60/70、board 14/15 无关联 | “runner PASS”不能代表全工程；驱动、UI、OTA 等高风险路径可能从未执行 | 每个 150 源文件映射需求/状态/错误/test ID；为可 host 化逻辑补测试；C 级 map + B 级行/分支覆盖报告；合理豁免需 owner/理由 |
| `COV-003` | P1 | 5 个关键 AEC/打断 host check 孤立于 runner | `main/aec_delay_shadow_host_check.c`、`main/aec_fir_delay_control_host_check.c`、`main/barge_in_detector_host_check.c`、`main/p5_live_fast_gate_host_check.c`、`main/p6_gray_confirm_host_check.c` | 发布脚本可以全部通过而这些检查从未执行，尤其会虚高双讲/AEC信心 | 接入统一 release runner/CI，保存每个可执行文件结果；若废弃则删除并记录替代证据 |
| `COV-004` | P1 | V1.2 元数据和缺失音频可能被误用为 V2 AEC/打断通过证据 | `test/audio_samples/README.md:36-40` 指定 CyberVoc V1.2；`:147-149` 声明开发门通过；26 个 manifest 路径均无物理音频；validator 不校验文件/hash | 无法复算、复听或确认板版/固件；最关键的“自身声不误打断、首音节保留”没有当前 V2 声学证明 | 采集 V2/COM5 当前固件原始多通道音频，保存 hash、条件和标注；validator 强制检查存在、格式、时长、hash、设备/镜像身份 |
| `COV-005` | P2 | gateway 和发布 runner 不完整，且无聚合 CI 门 | `../cloud-gateway/package.json:7-13` 注册 14/29，默认 test 3；`README.md:93-100` 只列四个项目 runner | 人工可能漏跑 15 个 gateway 文件、provisioning、wake 和 UI；不同人得到不同“全通过”结果 | 创建只读、失败即停的 aggregate release runner 和 CI；显式列出所有 test 或带批准的排除清单；生成 machine-readable run manifest |
| `COV-006` | P1 | 历史/设备证据未统一绑定 run、源码、配置、镜像、设备和原始资产 hash | AEC replay 默认使用 2026-07-14 日志 `tools/check-aec-human-protection-replay.ps1:18-39`，再读取当前源码/config `:353-356`；日志无同一身份链 | 旧固件结果可被错误归因给当前代码，修复后也无法判断回归来自何处 | 建立 evidence bundle schema；执行前冻结 source/config/image/device，执行后 hash 原始日志/音频并由 run manifest 关联 |
| `COV-007` | P1 | 状态和错误路径只有候选清单，语义分母未闭合 | 至少 11 个顶层轴；六个显式枚举 36 状态值、shutdown 3；另有 90 enum 候选。错误 token 682/148/35/699/106/7 高度重叠 | 无法证明非法迁移、跨轴竞态和关键恢复分支被测试；枚举/错误 token 数会造成虚假覆盖率 | 为每个合法/非法迁移、跨轴不变量和语义错误路径赋稳定 ID，映射 owner、注入点、预期副作用、恢复和 B/C/D 证据 |

## 9. 关闭条件与后续 B/C/D 工作

### 9.1 B0-17 文档级关闭条件

本文件完成了 A 级分母清点，但以下事项完成前，覆盖体系本身不能标为“已闭合”：

1. `COV-001`：批准规范性需求 catalog，并用稳定 ID 替换所有 `R-CAND-*`。
2. `COV-007`：生成完整状态迁移/非法边/跨轴不变量清单，以及语义去重错误路径清单。
3. `COV-002`：150 个 app 源、1 个 bootloader 源和 1 个兼容组件分别映射到需求、测试或批准豁免；用 C 级 map 确认最终链接边界。
4. `COV-003`：接入或正式处置 5 个孤立 host check。
5. `COV-005`：统一项目、UI、协议和 gateway 的 release runner/CI 清单，所有排除都有理由和 owner。
6. `COV-004/006`：建立当前 V2 可复算的 device/acoustic evidence bundle，旧板、旧日志和缺失资产从通过统计中剔除。

### 9.2 后续授权等级与最小执行顺序

| 顺序 | 等级 | 工作 | 最低输出 |
|---:|---|---|---|
| 1 | A | 冻结批准需求、状态和错误路径 catalog | 稳定 ID、owner、验收条件、source/config hash |
| 2 | B host | 运行六个 PowerShell runner、UI bash runner、5 个补接检查；加入 fault injection 和覆盖插桩 | 命令、工具版本、退出码、每测试结果、coverage report、run manifest |
| 3 | B gateway | 注册并运行 29 个 gateway test，补统一协议 conformance 和故障矩阵 | 聚合结果、协议版本、网关 hash、原始输出 |
| 4 | C build | 对当前 V2 OTA 产品 profile 干净构建，另验证所有正式 profile | 最终 sdkconfig、map、partition、镜像、source/config/image hash |
| 5 | D device | V2 真机执行启动/关机、Wi-Fi/WS、配网、正常对话、会议、监控、OTA、异常恢复和长稳 | 设备 ID、硬件版、run_id、日志、指标、故障注入结果 |
| 6 | D acoustic | 全音量、距离、方位、噪声和双讲 corpus 验证 AEC/打断/首音节 | 原始多通道音频、hash、自动指标、盲标结果、阈值和通过门 |

发布门必须按需求 ID 汇总最强证据等级和未覆盖原因。安全、AEC/打断、OTA/回滚、电源、云断连恢复、会议/监控长稳等 P0/P1 产品义务不得仅以 A 或 B 证据放行。

## 10. 静态复现命令与结果

本轮只执行只读枚举和文本分析。以下命令类别不构建、不运行测试、不访问设备：

```powershell
# 正式 app 源表、条件源和模块分布
Get-Content .\main\CMakeLists.txt

# host check、runner 和 UI test 入口清点
Get-ChildItem .\main -Recurse -Filter '*_host_check.c' -File
Get-ChildItem .\tools -Filter 'run-*-host-check*.ps1' -File
Get-Content .\test\host\run_tests.sh

# gateway 文件与 npm 注册入口清点（工作区根执行）
Get-ChildItem .\cloud-gateway -Filter 'test-*.js' -File
Get-Content .\cloud-gateway\package.json

# 状态与错误分支候选，只用于建立待语义去重分母
rg -n 'typedef enum|ESP_ERROR_CHECK|ESP_ERR_|goto|timeout|abort|restart' .\NuwaxAIPartner-v2\main
```

源文件关联复算方法为：展开 `main/CMakeLists.txt` 的 `SRCS_COMMON`、OTA 条件追加和 `SRCS_LVGL_UI`，将六个 PowerShell runner 的编译输入与 `test/host/run_tests.sh:32-50` 的 `MODULES` 规范化为项目相对路径，再独立提取三个 runner 的 `Get-Content` 产品源码路径；最后分别做集合去重和并集。文本正则的数量不进入“host 编译”列，头文件、host check 和 stub 不进入 150 个产品源分母。

错误候选同样只扫描上述 150 个正式 C 源。其中“错误比较”使用 raw-source 正则 `!=\s*ESP_OK|==\s*ESP_ERR_[A-Z0-9_]+`：`!= ESP_OK` 为 635 处/44 文件，`== ESP_ERR_*` 为 64 处/16 文件，去重文件并集为 45，合计 699 处。对称正则加入 `ESP_OK\s*!=` 与 `ESP_ERR_*\s*==` 后结果不变。该计数未解析 C 语法，也未剥离注释、字符串或未启用的条件编译分支，只能用于发现待审计位置，不能解释为 699 条独立错误路径。

静态复算结果：

- OTA 产品 app 源表 150，非 OTA 145；模块合计与 `docs/review-2026-08-02/module-inventory.md:38-42` 一致。
- 36 个 host check C 文件，31 个被 PowerShell runner 引用，5 个孤立。
- 6 个 PowerShell host runner，静态识别 31 个编译输出和 73 个 `throw` token。
- UI bash runner 编译 6 个产品源和 7 个测试文件。
- gateway 顶层 29 个 `test-*.js`，npm scripts 去重注册 14 个，15 个未注册，默认 `npm test` 为 3 个。
- 音频 manifest 26 行，25 个有效标注/通过、1 个 rejected；26 个引用音频均缺失，25 个 runtime log 存在；全部 profile 指向 V1，且构建标识均为 COM8 历史构建。
- 150 个 app C 源的错误候选为：682 个非 OK 返回、148 个 goto、35 个 `ESP_ERROR_CHECK*`、699 个错误比较、106 个 `ESP_ERR_TIMEOUT` token、7 个 abort/restart 调用。所有数字均需语义去重。

`node .\tools\protocol\generate-na-fixtures.js` 未运行。该入口在 `tools/protocol/generate-na-fixtures.js:253-280` 创建目录并写 binary fixture/manifest，是生成器而不是只读测试；后续只能在隔离输出路径或明确写授权下执行。

## 11. 最终判定

本文件已完成 B0-17 所要求的 A 级静态清点：产品 app 源分母和测试资产文件分母可复算，需求、状态迁移和错误路径的未闭合边界已被明确记录，且 `COV-001` 至 `COV-007` 均有关闭条件。

这不构成产品功能通过或发布批准。当前没有规范性需求分母，没有完整状态/错误语义分母，没有当前批次 B/C 执行结果，也没有绑定当前 V2 固件的 D 级设备与声学证据。

**B0-17 A-level complete; B/C/D open; release blocked.**
