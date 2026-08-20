# NuwaxAIPartner-v2 全工程代码与产品稳定性评审计划

版本：V1.8

日期：2026-08-02

评审对象：`NuwaxAIPartner-v2` ESP32-S3 正式工程及其设备端配套测试资产

评审性质：分级执行计划。第一轮仅做只读审计，不修改业务代码、不运行会编译源码的主机测试、不构建固件、不操作 COM5；后续主机编译测试、固件构建和设备写入分别授权。需要实机写入或 OTA 时，必须经过单独批准并记录设备、镜像、哈希和回滚条件。

本版本复核说明：已对当前仓库的源码文件、`main/CMakeLists.txt`、bootloader/component CMake、Kconfig、sdkconfig profile、partition table、测试脚本和生成资源进行清单交叉核对，并补齐权限分级、版本真值、硬件资料冻结、测试能力预检、非 C 质量门、持久化/恢复、模糊测试、量化 SLO、工程外 `cloud-gateway`/根工具脚本的支持资产基线，以及软件定时器/事件循环、ESP-IDF 系统任务联合调度、FreeRTOS 对象销毁和可重入性审计。V1.7 进一步把致命复位路径、ISR/IRAM/DMA 安全和崩溃/日志可观测性写入阶段 1 的显式交付物。V1.8 根据执行前反向复核新增 SBOM/许可证/CVE、隐私与录音留存、第一方内存安全/UB、生成资产 provenance、V1->V2 功能等价、需求/测试覆盖、威胁模型/密钥生命周期和全阶段唯一工作项索引；本说明不代表任何功能已经通过实机验收。

---

## 1. 评审目标

本评审不是只检查“能否编译”或“单个功能是否能运行”，而是回答设备作为长期运行产品是否具备以下属性：

1. 功能闭环：启动、配网、联网、唤醒、正常对话、播报、打断、会议模式、监控模式、UI、音量、电量、OTA 和异常恢复的状态是否闭合。
2. 实时性：FreeRTOS 任务、音频采集、AEC、VAD、上行、下行、I2S 播放和 UI 是否满足确定性时间预算；是否存在无界等待、优先级反转或音频路径被网络/显示阻塞。
3. 资源安全：内部 RAM、PSRAM、DMA 内存、任务栈、队列、环形缓冲、TLS、Opus/AEC/LVGL 缓冲是否有预算、峰值、释放和长稳证据。
4. 故障韧性：云端不可用、DNS/TLS 失败、WebSocket 半断、服务器无响应、网络抖动、I2C 外设缺失、麦克风超时、扬声器卡住、低电量、NVS 损坏、OTA 中断和掉电重启时，设备能否安全降级、恢复或明确进入故障态。
5. 并发正确性：任务、回调、队列、锁、generation、epoch、状态快照和 UI 更新是否存在竞态、死锁、悬挂指针、重复释放、旧会话污染新会话等高位风险。
6. 协议与安全：设备和测试/正式云端的认证、服务就绪、音频序号、END/stop、命令 ACK、OTA 授权、反回滚和错误码是否有明确契约，是否存在首包自证、越权或重放窗口。
7. 可维护性：模块边界、所有权、错误传播、日志、配置来源、测试桩和构建产物能否支持后续硬件版本、云端版本和量产发布。

最终结论必须能区分：

- **代码已证明**：由源码、静态约束、主机测试或构建产物直接证明；
- **设备待验证**：只能通过真实硬件、电气信号、声学、网络或长稳测试证明；
- **产品/云端待决策**：不是代码错误，而是协议、策略或发布边界尚未冻结；
- **发布阻塞**：任何会导致安全、数据完整性、无法恢复、随机死机或 OTA 失控的 P0/P1 风险。

---

## 2. 评审基线与范围

### 2.1 源码范围

| 层级 | 目录/文件 | 评审重点 |
|---|---|---|
| 启动与主控 | `main/nuwax_ai_partner_main.c`、`app_state.*`、`app_shell.*`、`diagnostics.*` | 启动顺序、主循环、状态机、异常恢复、任务创建、云连接生命周期 |
| 板级抽象 | `main/board/` | 引脚、电源、GPIO、I2C、I2S、DMA、Codec、PA、传感器、LED、触摸、存储和驱动所有权 |
| 音频采集 | `main/mic_input.*`、`conversation/audio_uplink*`、`audio_frame.*` | 帧格式、采集队列、序号、超时、背压、END、释放、丢帧和实时年龄；未链接的 `audio_reliable_capture.c` 仅在原型轨审计 |
| 音频处理 | `conversation/live_aec.*`、`audio_cleaner.*`、`mic_level.*`、`preroll_window.*`、`opus_encoder_wrapper.*`、`p3_*`、`p5_*`、`p6_*` | AEC reference、AEC 健康门、人声保护、设备声压制、VAD、预录、Opus、打断候选和证据可信度 |
| 音频播放 | `speaker_output.*`、`conversation/speaker_playback.*` | TTS ingress、generation、I2S、DMA、PA/Codec 静音、stop watchdog、背压、取消和播放完成回调；未链接的 `audio_reliable_playback.c` 仅在原型轨审计 |
| 对话状态 | `conversation/conversation_state.*`、`awake_session_state.*`、`turn_guard.*`、`listen_resume_state.*`、`prelisten_state.*` | 唤醒、持续对话、处理、播报、等待、休眠、turn ownership、重连保留和超时语义 |
| 打断与双讲 | `barge_in_controller.*`、`barge_in_detector.*`、`barge_in_dispatcher.*`、`capture_phase_handoff.*`、`touch_gesture.*` | 设备声与人声判别、首音节保留、停播时序、取消/恢复、epoch/generation 和 fail-closed |
| 会议/监控 | `main/audio_modes/meeting/`、`main/audio_modes/monitoring/` | 模式互斥、停播失败门、listen start/END/listen stop、序号、重开流、弱网和落盘语义 |
| 网络传输 | `ws_transport.*`、`wifi_manager.*`、`session_protocol.*`、`cloud_protocol_compat.*` | TCP/TLS/WebSocket、连接 epoch、RX/TX、认证、服务就绪、断线和重连 |
| 配网与身份 | `nuwax_provision_new/`、`wifi_saved_network_list_internal.h` | BLE 配网、云 URL/密钥、重启所有权、NVS、设备 ID、凭据生命周期和越权 |
| OTA | `nuwax_ota_new/`、`partitions.customwake.csv`、`sdkconfig.product-ota.defaults` | 下载、写入、校验、pending/commit/rollback、序号反回滚、掉电、状态上报和授权 |
| UI 与显示 | `nuwax_ui_v28/`、`nuwax_lvgl_port.*`、`display.*` | LVGL 线程唯一性、状态快照、revision、触摸、图标语义、电量、Wi-Fi/云状态和内存 |
| UI 资源与生成资产 | `nuwax_ui_v28/assets/`、`face_paths_core.bin`、`tools/gen_face_paths.py` | 大体积 RGB565/font/icon 资源是否进入镜像、是否被引用、是否重复/孤立、PSRAM/flash 占用和生成脚本可复现性 |
| MCP/命令 | `mcp_server.*`、`session_dispatch.*`、`shell_action_dispatch.*` | capability registry、命令总线、幂等、ACK、最终状态回读和权限；未链接的 `state_cmd_fast.c` 单独判定归属 |
| 唤醒 | `wake_word_detector.*`、MultiNet 配置、`srmodels` | worker 生命周期、模型加载、阈值、唤醒到 greeting 的竞态和资源回收 |
| 兼容/基础设施 | `ctype_compat.c`、`time_sync.*`、`diagnostics.*`、`synthetic_audio.*`、`lvgl_env_probe.*`、`lvgl_psram_mem.c` | 链接兼容、系统时钟、合成音频测试、堆/栈诊断、LVGL/PSRAM 探测和异常日志 |
| 启动前电源保持 | `bootloader_components/nuwax_power_hold/` | bootloader 阶段 SW3/电源保持 GPIO 时序、复位默认态、app 接管顺序和异常复位保持 |
| JSON/组件兼容层 | `components/json/json_compat.c`、`main/ctype_compat.c`、`managed_components/`、`lib/lvgl/` | 解析边界、ABI/链接兼容、组件版本、编译选项、许可证和已知漏洞；不能把兼容层当作无风险胶水代码 |
| 原型/孤立实现 | `audio_reliable_capture.c`、`audio_reliable_playback.c`、`filter_conservative.c`、`full_duplex_coord.c`、`conversation/d1_playback_path.c`、未进入 `main/CMakeLists.txt` 的非测试 `.c` | 必须判定“迁移/删除/明确不参与产品构建”；不能把这些文件里的 TODO 或 30 ms 原型逻辑当成当前固件已实现功能 |
| 质量与诊断 | `hardware_selftest.*`、`board/*host_check*`、`*_host_check.c`、`tools/` | 可测试性、故障注入、日志证据、回归覆盖和测试与产品配置是否一致 |

### 2.2 配置、构建和依赖范围

必须同时审计，而不能只看 `main/`：

- 根 `CMakeLists.txt`、`main/CMakeLists.txt`、`main/idf_component.yml`、`lib/lvgl/idf_component.yml`、`dependencies.lock`；
- `sdkconfig`、`sdkconfig.defaults`、`sdkconfig.product-ota.defaults`、`sdkconfig.opus*`、`Kconfig.projbuild`；
- `partitions.customwake.csv`、bootloader 配置、`bootloader_components/nuwax_power_hold/Kconfig`、OTA 双槽容量和 `srmodels` 资源；
- `managed_components/`、`components/`、`lib/lvgl/`、`bootloader_components/`；
- `tools/run-*-host-checks.ps1`、云端 `cloud-gateway/` 测试、协议 schema 和 fake-device；
- 当前构建目录的 `CMakeCache.txt`、`compile_commands.json`、`project_description.json`、map 文件和镜像 hash；
- 交接、硬件原理图、协议、AEC、OTA 和已知问题文档，用于核对代码与产品约束，而不是替代代码证据。

覆盖门禁：源码清单必须包含工程目录内实际存在的 tracked、untracked 第一方 `.c/.h/.cpp/.S/.ld`、脚本、生成代码、资源和配置文件，并显式记录排除的 build/cache/可再生输出；不能只使用 `git ls-files`。每个文件都要标注所属模块、是否进入当前构建、调用/被调用关系、测试入口和审计结论。`managed_components/`、第三方库和生成产物不能因为“不修改”而跳过，至少要记录版本、编译选项、已知限制、许可证/漏洞扫描结果及其对运行时的影响。无法取得证据的文件必须进入“未覆盖清单”，不得默认为通过。

当前仓库还必须执行 `main/CMakeLists.txt` 对比：`main` 下所有 `.c` 与 `SRCS_COMMON/SRCS_LVGL_UI` 的差异要单独列入 `not-linked-sources.md`。host check 可以标记为测试资产；但任何包含真实业务含义、TODO、旧协议、30 ms 帧、可靠队列或命令通道的未编译文件都要形成结论：删除、迁移、纳入构建、或在文档中标记为历史草稿。否则评审不能写“全工程功能已闭环”。

同时对每个产品构建 profile 扫描 `TODO/FIXME/NOT_IMPLEMENTED/ESP_ERR_NOT_SUPPORTED/stub`：逐项标记是否可达、是否仅为旧板兼容、是否为故障注入/host stub、是否有真实 fallback 和测试证据。`ESP_ERR_NOT_SUPPORTED` 不是自动故障，但若从正式硬件路径可达且没有产品降级语义，至少登记 P1；可达的空实现、静默丢弃和未实现 TODO 不得计为通过。

### 2.3 硬件范围

本轮计划覆盖所有已接入工程的硬件驱动：ESP32-S3、N32R8/PSRAM、LCD/QSPI、触摸控制器、ES7210、ES8311、NS4150B/PA、MIC1/MIC2/MIC3、I2C 总线、电源保持、SW3、LED、BQ27220 电量、QMI8658A、Wi-Fi、BLE、SD、底座 UART、4G。SD、底座 UART、4G 即使暂不做本轮验收，也必须完成代码路径、初始化失败和资源占用审计，并标记为“延期实机验收”，不能标记为已验证。

硬件证据唯一权威目录暂定为 `NuwaxAIPartner测试工程/docs/最新硬件原理图/`。阶段 0 必须对其中原理图、电路图和器件规格书建立逐文件 SHA-256 清单，记录文件名、页数/大小和用途；任何同名副本只可作为对照。当前无法取得的 BOM、netlist、BQ27220/QMI8658A 完整规格书或实物返修记录必须列入“硬件证据缺口”，不得由代码或商家示例推断为已确认。

商家参考工程必须在阶段 0 冻结唯一来源路径和 hash。根目录 `cyberkit-main` 与测试工程内同名目录若同时存在，先比较来源、版本和内容；未确定权威版本前，商家 AEC/驱动代码只能作为候选参考，不能作为新版硬件事实。

### 2.4 当前源码清单复核结果

本节是对当前仓库的计划校验事实，不是任何功能已通过的证明。后续进入阶段 0 时必须以冻结 commit 重新生成。

- 当前 `NuwaxAIPartner-v2` 整目录在上级 Git 仓库中未跟踪，`git ls-files` 不能代表其内容；阶段 0 必须以全文件 SHA-256 manifest 冻结，最终发布前应纳入受控版本库。

- 原始文件系统中 `main/` 当前有 193 个 `.c`；其中 1 个位于明确失败的 `_archive/v15-goertzel-failed/`。排除该归档后活动源码为 192 个；`main/CMakeLists.txt` 列出 149 个潜在源文件条目，其中 OTA 源文件受 `CONFIG_NUWAX_OTA_ENABLE` 条件控制；活动源码中 43 个、连同归档共 44 个没有直接进入该目标。
- 43 个活动未链接文件中 35 个是 host check/host stub；仍有 8 个非 host 文件：`audio_reliable_capture.c`、`audio_reliable_playback.c`、`conversation/d1_playback_path.c`、`filter_conservative.c`、`full_duplex_coord.c`、`state_cmd_fast.c` 和两个未列入 CMake 的 `face_21_listening_360_rgb565.c`/`face_22_speaking_360_rgb565.c`。另有归档的 `p3_device_voiceprint_goertzel.c`，必须明确保持排除。
- 这些文件不得因名称或注释而被计入已实现功能；必须按“历史原型、测试资产、待迁移实现、生成但未使用资产、或构建遗漏”逐项关闭。
- 当前配置资产包括 `main/Kconfig.projbuild`、bootloader Kconfig、多个 `sdkconfig*.defaults`/fixture/vendor/CI profile、多个 partition table、`managed_components/` 与 `lib/lvgl/`。因此“当前正式产品配置”必须在基线中精确命名，不能仅写“默认配置”。
- 本次清单中 `main/Kconfig.projbuild` 有 348 个配置符号，bootloader Kconfig 有 2 个，根目录有 11 个 `sdkconfig*` 文件与 3 个 partition table；这些数字仅供本次复核追溯，实际审计时必须重新生成并记录差异。
- 按 `.c/.h/.cpp/.hpp/.S/.s/.ld/.py/.ps1/.js/.ts` 统计，活动第一方目录 `main/`、`components/`、`bootloader_components/`、`test/`、`tools/` 有 357 个代码/脚本文件；另有 11 个 `_archive/v15-goertzel-failed` 历史源文件，必须进入排除清单和 hash manifest。`lib/` 有 2209 个、`managed_components/` 有 939 个。第三方文件不要求无差别逐行重审，但必须完成实际链接成员、版本、编译选项、SBOM、许可证/漏洞和项目封装边界审计。

这项复核使后续评审有两条强制轨道：一条审计实际链接进每个产品镜像的代码；另一条审计仓库内未链接、生成、测试和历史代码的归属，防止未来迁移时把未验证实现带入产品。

---

## 3. 评审原则与证据等级

### 3.1 证据优先级

从强到弱：

1. 真实硬件可重复测试、逻辑分析仪/示波器/电流计/声学录音和故障注入结果；
2. 完整当前配置的 IDF configure/build/link、镜像 image-info、分区检查和 hash；
3. 可控 fake gateway、协议端到端测试、主机状态机/队列/内存故障注入；
4. 静态源码检查、编译器警告、`-Werror`、脚本契约和资源图；
5. 注释、旧日志、历史文档或设计推断。

低等级证据不能覆盖高等级风险。例如主机测试通过不能证明硬件 AEC、I2C 时序、物理停播残声或长稳内存无泄漏。

### 3.2 风险等级

| 等级 | 定义 | 例子 | 放行规则 |
|---|---|---|---|
| P0 | 会导致死机、死锁、内存破坏、无限阻塞、错误 OTA、认证越权或不可恢复 | RX/owner 锁死、UAF、OTA 未认证、双重释放、错误分区写入 | 立即阻塞，必须修复并复测 |
| P1 | 关键功能失效、数据完整性破坏、异常无法自动恢复、明显性能超预算 | 音频缺帧伪成功、云死后无法重连、播放卡死、状态机不可达 | 发布前必须关闭或有明确降级 |
| P2 | 可见体验、资源余量、可维护性或中等概率异常 | UI 状态落后、栈余量不足、队列年龄过大、日志无法定位 | 需有负责人、计划和临时规避 |
| P3 | 优化、文档、可观测性和后续能力 | 运行期阈值 setter、MCP 扩展、渲染优化 | 不阻塞当前功能发布，但要登记 |

每个问题必须同时给出：影响、触发条件、代码位置、可重复步骤、根因、修复建议、验证证据、残余风险、责任人和关闭条件。

### 3.3 执行授权等级

| 等级 | 允许动作 | 明确禁止 | 本轮状态 |
|---|---|---|---|
| A：只读审计 | 读取源码/配置/已有日志和镜像，计算 hash，运行不编译且不写设备的检查 | 修改业务代码、编译、写 NVS、COM5、OTA/烧录 | 已授权，当前执行 |
| B：主机编译测试 | host check、网关测试、sanitizer、lint；只写独立临时产物 | 固件构建、COM5、设备持久化 | 需用户批准 |
| C：固件构建 | 使用全新外部 build 和独立 `SDKCONFIG` configure/build/image-info | COM5、OTA、NVS/eFuse 写入 | 需用户批准 |
| D：设备操作 | 经批准的串口、OTA、烧录、配网、NVS/flash 写入和实机测试 | 未列入批准范围的设备/端口；任何未单批批准的 eFuse | 每批单独批准 |

进入更高等级不自动授权更低等级以外的其他破坏性动作。BLE 配网会写 NVS，属于 D；仅扫描广播/建立不落盘连接的 radio smoke 才可列为无持久化设备测试。Secure Boot、Flash Encryption 和 eFuse 始终需要独立不可逆操作审批。

---

## 4. 分阶段评审总流程

### 阶段 0：基线冻结与可追溯性

目标：确保后续所有结论都针对同一份源码、配置和镜像。

工作项：

1. 记录工程绝对路径、Git 工作区状态、分支/commit（若未提交则记录文件 hash）、IDF、Python、GCC、Node、esptool 版本。
2. 生成源码清单，按模块统计 `.c/.h` 行数、源文件是否进入 `main/CMakeLists.txt`、是否存在孤立文件和重复实现。
3. 记录所有 sdkconfig overlay 的优先级和最终生成配置，标出测试模式、产品模式、诊断模式之间的差异。
4. 记录分区表、bootloader、app、model、storage、ota_0/ota_1 的地址、容量、实际镜像大小和余量。
5. 记录构建命令、环境变量、依赖锁文件和可复现性；同一源码至少执行一次干净 configure/build/link。
6. 建立基线目录和 hash 清单；任何源文件修改都必须递增审计批次，禁止继续引用旧镜像或旧日志作为当前证据。
7. 建立构建配置矩阵：列出全部 `sdkconfig*`、`Kconfig.projbuild` 和 CMake 条件分支，标记产品支持、诊断、fixture、CI、历史 profile；每个产品支持 profile 必须独立 reconfigure/build，未覆盖的预处理分支必须进入风险清单。
8. 全文件 manifest 必须从文件系统枚举 tracked+untracked 内容，覆盖第一方源码、头文件、CMake/Kconfig、脚本、协议、资源、分区表和锁文件；逐项记录相对路径、大小、SHA-256、活动/测试/历史归档/生成物分类。`build/`、缓存和可再生输出必须用显式规则排除，`_archive/` 必须计数并 hash 后标记为不参与构建，不能被搜索工具默认忽略。
9. 建立版本真值表，核对根 `PROJECT_VER`、OTA release sequence、app descriptor、设备 hello/状态、OTA header/manifest、README/交付文档和已有镜像 `project_description.json`；历史 build 版本不能冒充当前源码产物。
10. 建立硬件资料 SHA-256 清单，并冻结商家参考工程唯一来源；无法取得的 BOM/netlist/规格书必须进入证据缺口清单。
11. 记录持久化 schema/version、NVS namespace/key、分区 schema、srmodels/app/bootloader 兼容关系，为 OTA 和恢复测试建立基线。
12. 对工程外但参与验证的 `cloud-gateway/` 和工作区根 `tools/` 可执行脚本/fixture 建独立 hash manifest；运行时状态、录音、日志、simulator 报告和缓存必须明确排除，不能混入测试代码基线。
13. 对实际链接进产品镜像的第一方/第三方/预编译库和模型建立 SBOM、许可证、CVE/VEX 与例外清单；必须以 linker map/组件解析结果为准，不能只依据 `dependencies.lock`，并检查 `THIRD_PARTY_NOTICES.md` 与当前 V2 硬件/组件一致。
14. 建立隐私与音频数据流、访问控制、传输/静态加密、用户同意、录音指示、保留/删除和导出策略；覆盖正常对话、pre-roll、会议、监控、诊断音频、WAV、日志和 core dump，测试网关默认录音必须有单独授权和自动清理门禁。
15. 建立生成代码/资源 provenance：逐资产记录 generator、输入路径、输入/输出 hash、工具版本、许可证和 byte-for-byte 重生成结果；工程外输入和仍指向旧工程的生成脚本必须进入缺口清单。
16. 建立 `NuwaxAIPartner` V1 -> `NuwaxAIPartner-v2` 功能等价矩阵，按保留/替换/删除/待决策映射正常对话、打断、会议、监控、BLE 配网、OTA、UI、唤醒、电源和云协议，不以目录拷贝或文本 diff 代替回归证据。
17. 建立需求/测试覆盖矩阵，逐模块、状态转移和关键错误分支标注 static、host、gateway、device、hardware 证据和未覆盖项，记录覆盖分母；脚本存在不等于路径已验证。
18. 建立 `security-threat-model.md`、`release-security-profile.md` 和 `key-lifecycle.md`，覆盖设备身份、PoP、Wi-Fi/cloud secret、OTA 签名、Secure Boot、Flash/NVS Encryption、anti-rollback、debug surface、core dump、RMA、轮换/吊销和签名仪式；量产 profile 必须有独立可验收配置。
19. 建立全局 `review-work-item-index.csv`：每个阶段的稳定 `plan_id`、源码范围、授权等级、方法、证据路径、验收条件、问题 ID、反向校验和状态均唯一可追溯；测试执行另用唯一 `run_id` 并绑定 source/config/image hash。
20. 对第一方 C/C++ 代码执行独立 memory-safety/UB 静态审计，逐模块覆盖长度加乘溢出、signed/unsigned 与窄化、NUL/格式化、结构体布局/对齐/aliasing、UAF/double-free/错误路径释放、栈大对象/VLA、DMA/PSRAM/cache buffer 生命周期；A/B/C/D 证据等级分开记录。

输出：

- `review-baseline.json`：版本、工具链、配置、分区、hash；
- `module-inventory.md`：模块、owner、依赖、测试、风险；
- `config-diff.md`：defaults/overlay/最终 sdkconfig 差异；
- `build-reproducibility.md`：干净构建和产物证据。
- `source-manifest.sha256` 与排除规则：完整源码/配置/资源冻结；
- `hardware-evidence-manifest.md`：硬件资料、缺口和商家参考版本；
- `version-truth-table.md`：源码、协议、OTA 和现有镜像版本对应关系。
- `supporting-test-assets-manifest.sha256`：网关与工作区级测试代码/脚本冻结。
- `sbom.cdx.json`（或 SPDX 等价物）与 `third-party-license-cve-audit.md`：实际链接成员、许可证、CVE/VEX 和例外。
- `privacy-data-flow-retention.md`：音频/日志/core dump 的同意、访问、保留和删除策略。
- `generated-artifact-provenance.md`：生成器、输入输出 hash、版本、许可证和重生成结果。
- `v1-v2-product-feature-parity.md`：V1/V2 功能、协议、配置和回归证据映射。
- `requirements-and-test-coverage-matrix.md`：需求/状态/错误路径覆盖分母与证据等级。
- `security-threat-model.md`、`release-security-profile.md`、`key-lifecycle.md`：量产安全边界和密钥流程。
- `review-work-item-index.csv`：阶段 0~10 的唯一工作项与 evidence/run 映射。
- `first-party-memory-safety-ub-audit.md`：第一方内存安全/未定义行为审计与未覆盖清单。

放行门：没有基线 hash、最终 sdkconfig、构建命令、第三方 provenance、隐私策略、生成资产 provenance、安全 profile 或全阶段工作项索引时，后续结果只能标记为草稿。计划和执行记录属于不同生命周期：计划可持续更新，source manifest 必须在阶段收口前按当前计划重建，执行记录和 evidence 目录使用独立 evidence manifest。

### 阶段 1：架构、任务和实时系统评审

目标：证明任务、状态、锁、队列和回调的所有权闭合，且没有无界等待。

检查清单：

1. 绘制任务图：任务名、创建者、优先级、核心亲和性、栈大小、静态/动态内存、WDT 订阅、输入队列、输出队列、停止方式和 owner。
2. 绘制状态图：启动、关机、sleep、wake、Wi-Fi、WebSocket transport、service-ready、authenticated、normal conversation、meeting、monitoring、OTA、error/reconnect。
3. 绘制锁图：每把 mutex/semaphore/spinlock 的 owner、获取顺序、持有范围、超时、ISR 可用性和跨任务回调；检测锁反转、递归获取、持锁 I/O 和 `portMAX_DELAY`。
4. 绘制队列图：元素所有权、深度、字节预算、入队失败语义、drop 策略、终止帧保留、清空和销毁顺序。
5. 检查任务停止协议：`RUNNING -> EXITED/STOP_REQUESTED -> owner delete -> STOPPED`，禁止 worker 自删后 owner 继续使用句柄或静态栈。
6. 检查回调生命周期：注册对象、解绑时机、连接 epoch/generation、旧回调到达新连接时的行为、回调是否可能持锁或调用阻塞 API。
7. 逐个审计 `portMAX_DELAY`、`while(true)`、等待队列、I2S 写、TLS 读写、DNS、NVS、LVGL API 和任务通知；所有路径必须有终止条件或明确 owner 保障。
8. 检查 FreeRTOS tick、tick wrap、时间差比较、`pdMS_TO_TICKS` 小于一个 tick 的退化和微秒时序混用。
9. 检查中断/DMA/ISR：ISR 只做允许操作，cache/PSRAM/DMA 能力匹配，缓存行、对齐、buffer 生命周期和 cache invalidate 正确。
10. 检查错误传播：每个 `esp_err_t`、队列失败、超时和分配失败是否被处理，是否把部分成功伪装成成功，是否统一进入回滚路径。
11. 检查跨核可见性：`volatile`、临界区、portMUX、atomic、generation/epoch 读写必须有一致内存序；禁止用普通 `volatile` 替代跨核同步。
12. 检查 mutex 优先级继承、二值信号量误用、持锁调用回调、持锁日志/I/O、锁持有最长时间和等待者优先级；所有 `portMAX_DELAY` 必须进入白名单并给出不会永久阻塞的证据。
13. 检查任务 WDT 订阅、喂狗位置、CPU core 亲和性、CPU 占用、重建/退出 owner 和栈 high-water；任何 worker 卡死都必须能被 owner 发现并有限时间恢复。
14. 检查 FreeRTOS software timer、`esp_timer`、`esp_event` 和驱动回调的执行上下文、最大执行时间、注册/注销顺序、重复注册、对象释放后迟到回调和回调中禁止的阻塞操作。
15. 将应用任务与 ESP-IDF 系统任务联合建图，至少覆盖 Wi-Fi、TCP/IP、event loop、timer、LVGL、USB/JTAG 和音频驱动相关任务；核对配置派生的优先级、栈、核心亲和性及其与实时音频任务的抢占关系。
16. 检查 queue/semaphore/event group/timer/task notification 等内核对象的创建、复用、清空和删除；销毁前必须证明没有等待者、ISR 或迟到回调继续访问，静态 control block/stack 不得提前复用。
17. 检查模块 API 的可重入性和调用上下文契约：静态工作区、全局 scratch buffer、`errno`/C 库调用、递归入口、同步 API 被回调重入，以及公开 API 是否明确只允许单 owner 或指定任务调用。
18. 对 `ESP_ERROR_CHECK`、`assert`、`abort`、`esp_restart` 等致命路径做独立清单：区分启动期不可恢复错误与运行期网络/音频/用户输入错误，禁止异步回调把可恢复错误升级为无提示重启；每条保留复位原因和恢复证据。
19. 对 ISR/IRAM/DMA 回调单列检查：ISR 及 cache-disabled 路径不得访问不可执行 flash/PSRAM、申请堆、持普通 mutex、调用非 ISR-safe API 或传递已释放 buffer；回调解绑必须先阻止新事件，再等待已进入回调退出。
20. 对日志、诊断和崩溃可观测性单列检查：reset reason、panic/backtrace/core dump、任务 heartbeat、关键 epoch/序号、日志等级/速率限制、敏感字段脱敏和持久化空间预算必须可重现；日志洪泛不能阻塞实时任务或覆盖故障证据。

输出：任务图、锁图、队列/内存图、状态转移表、无界等待清单、回调/ISR 生命周期矩阵、致命复位路径清单、崩溃与日志可观测性矩阵、P0/P1 并发问题登记。每个输出必须在执行工作记录中具有唯一工作项、证据文件和反向校验结果，不能只以计划正文“已覆盖”代替交付证据。

### 阶段 2：启动、关机、电源和恢复

目标：设备从上电到运行、点按 SW3 关机、重启、低电量和异常复位都能进入安全状态。

检查项：

- `app_main`/主任务初始化顺序：时钟、PSRAM、NVS、GPIO、电源保持、显示、触摸、Codec、麦克风、Wi-Fi、BLE、WS、OTA、唤醒模型；失败时是否停止后续依赖。
- SW3 的点按/长按/去抖/电源保持极性、关机时 PA/Codec/DMA/LCD/Wi-Fi/BLE 的关闭顺序。
- Brownout、看门狗、Guru Meditation、abort、异常重启后的 NVS 和 OTA 状态恢复；重启原因是否落盘并可诊断。
- bootloader 早期电源保持、strap/eFuse/USB-JTAG/download mode 与 app 阶段电源 GPIO 接管是否一致；复位默认电平不能导致误关机、误开 PA 或影响烧录。
- 低电量、充电、外部电源和电量读取失败的 UI/策略；电量异常不能触发错误关机或 OTA。
- Wi-Fi/云端未连接时，设备是否仍能本地显示、触摸、唤醒或安全休眠；云端死掉不能阻塞主 UI 和电源控制。
- 所有启动失败必须满足：无悬挂任务、无半初始化驱动、无重复注册、无未释放 DMA/队列/锁。
- `ESP_ERROR_CHECK`/assert/abort/restart 导致的异常必须记录原因、启动阶段和可恢复性；panic/core-dump/重启计数的存储不能破坏 NVS/OTA 安全状态，日志不能泄露凭据。

输出：启动时序图、重启原因矩阵、电源状态矩阵、异常恢复证据。

### 阶段 3：底层硬件驱动与引脚/时序

目标：代码与当前新版硬件原理图、BOM/器件规格和实物引脚一致，并且初始化/故障路径安全。

#### 3.1 引脚与总线

逐项核对 `esp_vocat_v1_0_pins.h`、board 初始化和原理图：GPIO 复用、上下拉、输入输出方向、open-drain、strap pin、USB/JTAG、UART、I2C、I2S、QSPI、触摸中断、PA 控制和电源保持。

检查同一 GPIO 是否被多个模块声明、初始化顺序是否改变电平、禁用功能后是否仍占用引脚，以及下载/运行模式是否冲突。

必须补充测量：PSRAM memtest/校准、flash/PSRAM 频率和供电、电源轨 3V3/电池/充电切换峰值电流、温升、brownout 边界、PA 打开/关闭瞬态，以及 I2C 总线卡死时 SCL/SDA 释放和器件 reset 恢复。

#### 3.2 音频硬件

- ES7210：I2C 地址、复位、MICBIAS、采样率、slot/TDM、PDM/ADC 路径、MIC3 reference 输入、读写时序和错误恢复。
- ES8311：DAC/ADC、I2S 主从、采样率、音量、静音、PA 前后顺序和 pop noise。
- NS4150B/PA：GPIO4/PA_CTRL 极性、mute、enable、掉电默认态和 stop 时的物理静音。
- I2S/DMA：时钟、slot 宽度、单双声道、DMA buffer 数量、共享 TX/RX、underflow/overflow 和 stall 恢复。
- MIC1/MIC2/MIC3：实际通道映射、物理方向、ADC_MIC3_P/N、参考通道 raw frame 保存和 AEC health gate。

#### 3.3 其他驱动

- LCD/QSPI/LVGL：时钟、DMA、PSRAM buffer、刷新任务和异常屏幕状态；
- 触摸：reset 脉宽、I2C retry、IRQ、去抖、滑动/点按和 I2C 总线卡死；
- BQ27220：电量/充电/外部电源、错误读数、UI 百分比和刷新周期；
- QMI8658A：初始化、量程、数据读取、FIFO/中断和异常断开；
- LED：动画任务、颜色/亮度、关机默认态和队列释放；
- SD/UART/4G：即使本轮暂不验收，也审计初始化失败、任务、句柄、DMA 和是否影响主链路；
- 诊断/自检/测试音频和 USB 控制：核对编译开关、云端/MCP/本地入口权限、持续时间/音量上限、停止超时和失败回收，避免测试路径在生产镜像中成为无界 PA/麦克风控制入口；
- BLE/NimBLE：连接数、MTU、配网会话、断开释放、密钥缓存和运行期与 Wi-Fi 的资源竞争。

输出：引脚核对表、总线时序表、驱动状态机、故障注入结果、需硬件确认的未决项。

### 阶段 4：音频全链路、AEC 和拟人对话

目标：确认“采集—处理—上传—云端—TTS—播放—打断—重新采集”在正常、并发和异常情况下都保持音频完整性与可恢复性。

#### 4.1 帧和协议不变量

冻结并验证：PCM16、16 kHz、单声道、20 ms、320 samples/640 bytes（如配置允许最大帧长则必须明确）；capture sequence、wire sequence、timestamp、generation、epoch、session ID、END flags、listen start/stop 和 bytes/samples totals。

当前计划必须明确区分“线上协议帧”和“内部缓冲上限”：20 ms/16 kHz/mono = 320 samples/640 bytes 是 wire frame 不变量；`CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME=960` 只能作为内部最大缓冲/兼容上限，不能被测试、文档或原型代码误读为 60 ms 或 30 ms 的正式上行帧。任何 480 samples/30 ms 的历史文件必须标记为未编译原型或完成迁移。

任何重复、乱序、缺口、少字节、错误 frame length、END 先于 START、stop 早于 END、跨 generation 帧都必须有明确拒绝/关闭/重开语义，不能静默丢帧后返回成功。

#### 4.2 采集与上行

- 采集 owner 是否在正常对话、播放中、打断和会议/监控切换中被重复创建或拆除；
- pre-roll 是否覆盖 onset、确认窗口和 live handoff，onset 后有 gap 时是否 fail-closed；
- realtime queue 的最大年龄、bounded backpressure、END 独立保留槽、可靠队列的陈旧音频上限；
- 麦克风 timeout、I2S 错误、WebSocket 断开时是否停止/重建并保留正确 stop reason；
- 采集错误是否可能进入 THINKING/发送文本，或把部分音频伪装成完整会话。

#### 4.3 AEC、人声和设备声

- software reference 的来源是否为实际播放 PCM、音量缩放后的 reference、物理写出量和延迟是否对齐；
- 硬件 MIC3 reference 仅在引脚、器件、时序和 health gate 全部成立时启用，失败时不得宣称硬件 AEC；
- AEC 输出、raw mic、reference、residual、human rescue、cleaner、VAD、barge evidence 的样本数和 generation 是否一一对应；
- 设备音量 0～100% 下，设备声不能触发打断；用户低声、近场、双讲和重叠语音仍应被保留；
- 首音节必须从 onset 前导窗口开始，设备声重叠段必须有可量化的残声抑制和人声保真指标。

#### 4.4 播放与打断

- TTS 二进制帧进入固定/有界 ingress，不得让 WebSocket RX 直接等待 speaker queue；
- START/PCM/END/PLAYBACK_COMPLETE 的 generation、epoch、sample count、duration 和 deadline；
- PA/Codec mute、shared-TX DMA 静音、I2S write stall、stop watchdog、cancel recovery 和 post-playback rearm；
- 人声候选、确认、停播、取消 turn、保存 onset 前帧、重新开启下一轮监听的顺序；
- 空播放、拒绝播放、播放失败、云端取消和新 TTS 代次不能把采集永久留在错误状态。

#### 4.5 模式特有路径

- 会议：停播失败必须 fail-closed，不创建 session、不发 listen start；正常路径的 END/stop、持续落盘、停止和重开；
- 监控：latest-data/旧流终止/重新 ARMED 的策略，capture_failed、序号完整性和断线恢复；
- 正常对话：awake 2 分钟只累计等待时间，播报/处理暂停，播放完成重新开始完整预算。

输出：音频状态图、帧不变量表、延迟预算、AEC/打断证据、模式协议矩阵和声学待验收清单。

### 阶段 5：Wi-Fi、WebSocket、服务协议和云端故障

目标：云端不可用或行为异常时，设备不死机、不无限等待、不泄漏资源、不误报已连接。

检查项：

1. Wi-Fi 状态机：扫描、连接、获取 IP、断线、重连、多个保存网络、密码错误、AP 不存在、RSSI 变化和 DHCP 超时。
2. WebSocket：URL 解析、DNS、TCP/TLS、HTTP 101、证书、握手、RX/TX、ping/pong、close、半关闭、读写超时和 TLS destroy。
3. 三态分离：`transport_connected`、`service_ready`、`authenticated` 不得混用；UI 和 OTA 只在正确状态下行动。
4. 连接 epoch：旧 RX、旧 worker、旧 speaker callback、旧 session command 到达新连接时必须被拒绝或定向关闭旧连接。
5. 云端停止响应：服务器不发 hello ACK、只发 pong、不发 END、反复发错误、发送畸形 JSON/二进制、超长帧、控制帧交错、重复 ACK 和过期命令。
6. 背压：控制帧、ping/pong、stop/cancel、OTA ACK 不得排在 TTS 或音频数据之后；所有队列满、发送失败和超时必须有上限。
7. 重连：短暂断网是否保留 awake session、旧 turn 是否清理、是否避免重复 listen start、是否避免新旧 audio session 混用。
8. 云端完全死掉时：本地触摸、显示、音量、关机、唤醒和错误提示仍可用；重连退避不能占满 CPU 或耗尽 heap。

输出：协议时序图、连接状态机、断网/云死故障矩阵、控制帧优先级证据和重连长稳结果。

### 阶段 6：BLE 配网、身份、安全和 OTA

#### 6.1 配网与身份

- BLE 服务是否只允许一个连接，写入字段是否长度/字符集/版本严格校验；
- Wi-Fi SSID、密码、URL、device ID、device secret 是否以事务方式写 NVS，断电中途不能留下半配置；
- 配网成功后的 Wi-Fi/BLE 所有权切换、是否需要重启、旧凭据清理和重放保护；
- 设备唯一 ID、云端密钥、PoP、日志脱敏、二维码内容和测试凭据是否能进入量产镜像；
- 正式云与测试网关 URL 切换是否能明确显示当前环境，不能因为连接了 WebSocket 就显示业务可用。
- 配网/NVS 写入频率、wear leveling 或冗余副本、schema version/migration、损坏 key/半写事务和凭据轮换/吊销；
- 恢复出厂、RMA、遗失凭据、无可用 Wi-Fi 和 NVS 损坏时的可恢复入口；恢复流程不能清除 anti-rollback 或把测试凭据带回量产设备。
- 量产 debug surface：USB Serial/JTAG、UART console、BLE/USB reprovision、诊断命令和 core dump 读取必须有构建 profile 与授权矩阵；测试入口不得凭物理接入或未认证云消息直接修改凭据、播放测试音或触发升级。

#### 6.2 OTA

- OTA 命令必须检查“当前消息到达之前已认证”的连接，不接受首包自证、未知连接或错误 device ID；
- URL 必须是 HTTPS，版本、target chip/board、partition schema、build flavor、release sequence、size 和 SHA-256 完整校验；
- TLS 必须验证 CA、主机名、证书链和证书有效期；系统时间未同步时不得盲目接受过期/未来证书，必须有明确的失败状态和重试策略；
- OTA manifest、下载授权和最终镜像必须具备签名或等效不可伪造认证；“建议签名”不能作为量产放行条件。日志和 manifest 不得泄露 device secret、Wi-Fi 密码、PoP 或上传凭据；
- 下载、写入、校验、staged、reboot、pending_verify、committed、rollback 状态是否持久化且幂等；
- 下载中断、TLS 失败、空间不足、写入失败、校验失败、掉电、服务器返回短包和重启循环是否安全；
- anti-rollback floor、release sequence、旧 NVS/新 NVS 恢复和当前槽/目标槽是否一致；
- app-only OTA 对 bootloader、partition table、`srmodels`、NVS schema 和硬件 revision 的兼容矩阵；不能通过 app OTA 静默要求未更新的分区/model/bootloader；
- OTA 期间停止音频、BLE、显示或网络任务的顺序；失败后设备必须回到可用旧固件；
- 双 OTA 分区和 app 容量余量是否留有升级增长空间。

安全自动检查必须覆盖源码、配置、文档、日志、构建脚本和网关测试资产的 secret scan；测试私钥必须进入明确 allowlist、标记用途/保管边界/失效日期。`cloud-gateway/test-device-config-signing-private-key.pem` 只能作为测试专用资产处理。发布门禁还要验证测试 PoP、Wi-Fi 密码、device secret、上传密钥和私钥没有进入产品镜像或交付包，并具备凭据轮换与吊销流程。

输出：身份/OTA 状态图、授权矩阵、镜像 manifest、掉电/失败/回滚证据和量产安全阻塞清单。

### 阶段 7：UI、状态同步和 MCP

目标：UI 是状态的可靠呈现，不成为跨任务数据竞争或功能控制的第二套真相。

检查项：

- LVGL 只有 UI task 调用；业务任务不直接操作 widget；
- canonical `nuwax_device_state_t` 是否由 delta merge + monotonic revision + latest snapshot mailbox 更新；
- dialog、meeting、monitoring、cloud、Wi-Fi、battery、volume、OTA、auth 和 error 的优先级/互斥关系；
- transport connected 与 service/authenticated 的图标、文字和重连状态是否语义准确；
- UI mailbox 满、锁竞争、LVGL 延迟、对象已删除、页面切换和快速重复触摸是否安全；
- 电量、充电、低电量、外部电源和传感器无效时显示是否一致；
- MCP `tools/list` 是否只公布真实 callback；servo 等无效工具必须隐藏；命令是否统一进入 typed command bus、具备 request_id/idempotency、ACK 和最终 revision 回读；
- UI、MCP、云端同时操作音量/会议/监控时，是否有单一 owner 和确定的冲突策略。

输出：UI 状态优先级表、状态 revision 轨迹、LVGL 线程检查结果、MCP capability/command 矩阵。

### 阶段 8：内存、存储和资源预算

目标：证明设备在最坏并发组合和长期运行中不会耗尽或严重碎片化。

#### 8.1 资源台账

对每个静态/动态对象记录：模块、分配位置（internal/PSRAM/DMA）、大小、数量、峰值、生命周期、owner、释放 API、失败策略和观测方式。

至少覆盖：

- LVGL draw buffer、face/font/asset、UI mailbox；
- Wi-Fi/LwIP/TLS/mbedTLS 证书和 WebSocket RX/TX buffer；
- 当前正式构建的 audio capture/realtime/speaker queue、frame pool、pre-roll、AEC reference/residual/human rescue、Opus encoder；若任何 `audio_reliable_*` 原型以后纳入构建，必须以独立资源预算和协议语义重新审计；
- ES7210/ES8311/I2S/DMA buffer、shared-TX ring、PA mute buffer；
- BLE NimBLE heap、GATT prepare entries、配网 JSON/NVS 临时 buffer；
- OTA HTTP buffer、SHA context、manifest、storage/NVS 状态；
- task TCB/stack、timers、event groups、mutex/semaphore 和日志缓冲。

#### 8.2 测量方法

- 启动、Wi-Fi 已连、WS 已连、正常对话、播放/打断、会议、监控、BLE 配网、OTA、异常重连分别记录 free internal、largest internal、minimum internal、free PSRAM、largest PSRAM；
- 每个任务记录 stack high-water mark，低于团队设定阈值即 P1/P2；
- 使用 heap tracing、allocation failure hook、`heap_caps_check_integrity_all()` 和 1000 次生命周期循环；
- 比较 30 分钟/1 小时 soak 前后 heap/minimum/fragmentation、队列高水位和句柄数量；
- 对所有 `WithCaps`/普通分配、队列、任务、DMA、NVS handle 和 TLS handle 做 fault injection，确认成功/失败路径配对释放。

#### 8.3 第一方 memory-safety/UB 独立审计

阶段 8 不仅测量剩余内存，还必须执行阶段 0.20 的第一方 memory-safety/UB 证据闭环。`first-party-memory-safety-ub-audit.md` 应逐文件给出覆盖分母、源码位置、问题 ID、A/B/C/D 证据等级和未覆盖原因；重点入口包括 `audio_frame.c`、`session_protocol.c`、`ws_transport.c`、`nuwax_ota_manifest.c`、`nuwax_ota_transport.c`、`nuwax_provision_main.c`。静态“未发现”不能替代 ASan/UBSan/fuzz 或目标机 DMA/cache 风险的明确边界。

量化门槛建议：30 分钟 smoke 后 internal heap/minimum/largest block 不得持续单调恶化；1 小时核心链路 drift 不得超过基线的 5% 或 16 KB（取更严格者）；P1/P0 修复后建议增加 24 小时 RC soak，量产候选增加 72 小时 soak。任何阈值调整必须写入产品验收标准，不能用“基线附近”笼统关闭。

输出：`memory-budget.csv`、`first-party-memory-safety-ub-audit.md`、栈水位表、堆曲线、分配失败矩阵、长稳泄漏结论。

### 阶段 9：性能与实时性

目标：把“感觉快/偶尔卡”变成可测量的预算和 p95/p99 证据。

当前需先冻结并验证的基线（不得在测试脚本、Kconfig 和文档中各写一套）：

| 路径 | 基线/边界 | 测量点 |
|---|---:|---|
| 音频帧 | 20 ms，PCM16/16 kHz/mono | 采集、AEC 输入/输出、wire、播放 |
| realtime capture | queue depth 12，处理前年龄目标 60 ms | MIC timestamp 到 DSP 入队 |
| barge | 端到端 frame age 上限 100 ms | onset 到 stop/cancel/第一帧上行 |
| uplink | 12 packet slot，实时年龄上限 120 ms | capture 到 send completion |
| speaker | control/PCM ingress bounded，I2S 写有 stall 上限 | RX 到首帧、播放首帧到物理静音 |
| awake | 等待用户累计 120 s | 处理/播报暂停，播放完成重置 |
| UI | UI task 单 owner，revision 不回退 | 生产者提交到屏幕刷新 |

必须建立单一 timing registry，把 Kconfig、源码宏、host stub、测试脚本和产品文档映射到同一参数。首批必须核对的当前值包括：正常 VAD 尾静音 450 ms、播放中尾静音 500 ms、打断物理停播上限 2000 ms、本地 fade 0 ms、auto-followup grace 350 ms；这些值在未完成实测前只能称为“当前实现”，不能称为体验目标。

首轮暂定 SLO 用于暴露超预算，不代表已经验收；若产品/网络条件改变，必须在测试前书面冻结：

| 事件 | 暂定范围与门槛 | 备注 |
|---|---|---|
| UI 本地触摸/音量反馈 | p95 <= 100 ms，p99 <= 200 ms | 不依赖云端 |
| 已确认真人打断到物理静音 | p95 <= 150 ms，p99 <= 300 ms，硬上限 <= 1000 ms | 当前 2000 ms watchdog 需单独判定 |
| WS 已建立到认证/服务就绪 | p95 <= 3 s，p99 <= 5 s | 云端正常且 RTT 另记 |
| 网络恢复到设备重新业务可用 | p95 <= 10 s，p99 <= 30 s | 区分断线检测时间和重连时间 |
| 明确 TLS/Socket 错误后的本地清理 | p95 <= 1 s，p99 <= 3 s | 不允许无限等待或 UI/电源失活 |
| OTA accepted 到 committed | 同一稳定网络 p95 <= 180 s，p99 <= 300 s | 同时记录镜像大小、下载/写入吞吐和重启耗时 |

测量要求：使用单调时钟；记录 min/avg/p95/p99/max、队列高水位、丢帧/重排/重试和 CPU 核占用；区分云端 RTT、ASR/TTS、设备调度、AEC/DSP、I2S 和 UI 时间，不用总耗时掩盖瓶颈。

重点检查：CPU 240 MHz 下 AEC/LVGL/Wi-Fi 竞争、PSRAM/cache miss、优先级反转、TLS 大包阻塞、TTS 背压、Wi-Fi 重连风暴和高音量播放时的双讲处理能力。

输出：延迟预算表、trace/日志、p95/p99 报告、CPU/栈/队列相关性和优化建议排序。

### 阶段 10：故障注入与稳健性矩阵

必须至少覆盖以下场景，每个场景记录触发方法、期望状态、实际状态、恢复时间、数据完整性和资源差值：

| 类别 | 场景 |
|---|---|
| 云端/网络 | DNS 失败、Wi-Fi 无 AP、错误密码、DHCP 超时、系统时间未同步/回拨、TLS CA/主机名/证书过期失败、HTTP 非 101、WS 立即断开、半关闭、RX 卡住、TX 超时、持续 ping 无业务 ACK、服务器完全关机、丢包/乱序/延迟/重复帧 |
| 对话 | 唤醒后无语音、短语音、连续语音、ASR 无结果、文本超时、TTS 空/畸形/中断、播放卡住、用户在首帧/中段/尾帧打断、重复唤醒、旧 turn 回包 |
| 音频硬件 | MIC timeout、I2S underflow/overflow、ES7210/ES8311 I2C NACK、Codec reset、PA 失效、DMA stall、AEC reference 缺失/延迟错误、强弱音量切换 |
| 模式 | 会议 start/stop 失败、监控 start/stop 失败、模式切换竞态、云断线中切换、session END 丢失、序号缺口/重复/乱序、文件落盘失败/空间不足 |
| 电源/存储 | 低电量、充电切换、温度/电压波动、运行中点按关机、启动中复位、brownout、OTA 下载/写入/校验/重启各阶段掉电、NVS 半写、分区空间不足、文件系统异常 |
| 并发/资源 | heap 分配失败、PSRAM 不足、DMA 不足、队列满、锁持有超时、任务停止竞态、旧回调到达新连接、LVGL 页面快速切换、BLE 与 Wi-Fi 同时操作 |

通过标准：P0 场景不能死机或无限阻塞；P1 场景必须在有限时间内回到可用态或明确错误态；数据协议必须显式报告 incomplete/interrupted，禁止部分成功伪装完整成功；恢复后 heap、任务、句柄、队列和连接数回到基线附近。

每个故障用例至少重复 3 次；涉及断网、TLS、OTA、I2S、I2C 总线和电源的 P0/P1 用例至少覆盖“启动中、正常对话中、播报中、会议/监控中、OTA 中”相关注入时机。每次失败都必须保存 reset reason、crash dump/backtrace、任务 heartbeat、堆/栈水位、连接 epoch 和最后 200 行串口/网关日志；若无 crash dump 能力，需要先登记诊断缺口。

结构化输入还必须增加 property/fuzz 测试：WebSocket 长度/分片/控制帧、JSON 深度/类型/超长字段、音频头和字节数、BLE prepare write/字段组合、OTA manifest/version/size/hash/signature。断言重点是拒绝畸形输入、无越界/泄漏/死锁、旧 epoch 不污染新状态、错误响应有界。若当前没有 harness，先登记能力缺口并在 B 级授权后建设最小 host harness，不能把“计划执行 ASan/UBSan/fault injection”写成已有能力。

能力预检必须记录 Windows PowerShell、Node、Python、CMake/Kconfig 解析器，以及 WSL/Clang/GCC/ASan/UBSan 是否可用。不能在 Windows 原生环境中假定 shell/ASan 已可运行；缺失工具要形成可复现安装/CI 方案，而不是跳过后宣称覆盖。

阶段 2~10 的每一项检查必须先从 `review-work-item-index.csv` 分配稳定 ID（例如 `A2-*` 至 `A10-*`），在执行记录中登记唯一证据文件、输入 hash、授权等级、验收条件和 reverse-check；阶段 10 每个故障注入场景还必须有唯一 `run_id`，不得用“脚本存在”或共享日志替代项级证据。

---

## 5. 测试实施顺序与门禁

### 5.1 A 级：纯只读静态审计

1. 对 tracked+untracked 第一方文件执行全文件清单、SHA-256、CMake 源文件覆盖率、配置冲突和文本格式检查；`git diff --check` 仅作为已跟踪文件的补充，不能替代未跟踪 v2 源码检查；
2. 建立硬件资料 hash、商家参考版本、版本真值、分区/已有镜像和配置矩阵；已有 build 只能作为历史产物登记；
3. 静态审计整数溢出、`size_t`/有符号转换、结构体对齐、长度乘法、格式化字符串、未定义行为、错误码丢失、任务/锁/队列/分配生命周期；
4. 对 PowerShell、Node.js、Python、Shell、CMake 和 Kconfig 建立语法/lint 能力矩阵；本阶段只执行不会编译源码、不会安装依赖、不会改设备的现成检查；
5. 执行 secret scan 和测试密钥 allowlist 盘点，但输出必须脱敏，不在评审文档复制有效密钥；
6. 预检 host compiler、Clang/ASan/UBSan、WSL、协议/fuzz/fault-injection harness，缺失能力登记为建设项。

### 5.2 B 级：主机编译与协议测试

进入条件：用户批准 B 级；所有产物写入独立临时目录，不修改设备。

1. 以 `-Wall -Wextra -Werror` 执行现有 host checks 和 `test/host/run_tests.sh`；
2. 执行 cloud-gateway 全部协议测试、protocol validator/fixture generator、fake-device conformance、AEC replay 和工作区长稳脚本的主机部分；
3. 对可移植模块执行 clang-tidy/cppcheck；在能力存在时执行 ASan/UBSan、allocation failure 和最小 fuzz/property harness；
4. PowerShell/Node/Python/Shell/CMake/Kconfig 语法与 lint 通过，或逐项登记环境限制；
5. 所有退出码、工具版本、输入配置 hash 和输出路径写入证据包。

### 5.3 C 级：可复现固件构建

进入条件：用户批准 C 级；使用全新外部 build 目录、独立生成 `SDKCONFIG` 和冻结的 defaults，先 `set-target esp32s3`，再 configure/build。禁止复用工程内 `build/` 或当前根 `sdkconfig` 作为生成输出。

每个受支持产品 profile 必须生成 link/map/image-info、app descriptor、partition fit、镜像 SHA-256、编译器 warning 基线和版本真值核对。第一方代码不得新增 warning；历史第三方 warning 必须单独基线化，不能用全局忽略掩盖。C 级只产出镜像，不连接 COM5、不 OTA。

P0 默认阻止 release/生产 OTA，但不形成无法验证的闭环：若某个硬件型 P0 只能通过修复固件确认，在根因和回滚方案完成静态/主机复核后，可由用户单批批准 C/D 级“验证镜像”；该镜像不得作为发布候选，验证范围和失败恢复必须预先记录。

### 5.4 D 级：设备无持久化 smoke

- 经用户批准后，上电、显示、触摸、LED、电量、已保存 Wi-Fi、BLE radio、Codec、麦克风、扬声器和传感器 smoke；
- 逻辑分析仪检查 reset/I2C/I2S/PA 时序；
- 无人声合成 PCM 测试采集、上行、下行、END、播放和模式协议；
- 串口日志只读采集，记录设备 ID、版本、配置 hash、重启原因、错误计数和 heap/stack；
- 不修改 flash、不擦除/写入 NVS、不配网、不烧录；串口读取、COM5 和任何外设操作仍须 D 级批准。

### 5.5 D 级：配网/OTA/烧录写入阶段

进入条件：

- 用户明确批准；
- 设备在线、认证、OTA capability 正确；
- 镜像 URL、大小、SHA-256、version、release sequence、target board/chip、partition schema 已双人复核；
- 具备回滚和串口/电源恢复方案；
- 当前设备状态、旧版本和 OTA 状态已留档。

BLE 配网、Wi-Fi/云 URL/密钥变更、恢复出厂等会写 NVS 的动作在本节执行，必须和 OTA/烧录一样记录前后配置版本、回滚/恢复方法；不能归入“硬件只读”。

执行后必须轮询：

`accepted -> downloading -> writing -> verifying -> staged -> reboot -> pending_verify -> committed`

并验证：新版本、release sequence、OTA state、当前/上一个槽、无 rollback、无协议错误、设备重新上线、Wi-Fi/云连接、显示/触摸、合成音频和会议/监控 ACK。OTA 失败必须保留旧固件可启动，不能只根据 HTTP 202 或设备断开宣称成功。

### 5.6 D 级：长稳阶段

建议矩阵：

- 30 分钟正常对话合成音频循环；
- 30 分钟播报/打断/取消/重新监听循环；
- 30 分钟 Wi-Fi/WS 周期断连重连；
- 1 小时会议/监控无真人持续采集，检查 WAV 头、PCM16/16 kHz/mono、实际时长、文件大小、序号和 `capture_failed`；
- 设备低电量/充电切换和 UI 快速操作；
- 每 5 分钟记录 heap、stack、队列高水位、CPU、温度、Wi-Fi RSSI、重连次数和错误计数。
- P0/P1 修复候选至少执行 24 小时 RC soak；量产候选执行 72 小时 soak，并重复 OTA、模式切换和 WS 重连周期；
- 长稳通过必须量化：无死机/重启风暴/任务泄漏，重连退避有上限，核心 heap/stack/queue 指标满足第 8 阶段 drift 门槛。

---

## 6. 当前主机/网关测试入口

评审计划要求每次批次记录实际命令和退出码，不能只复制“通过”文字：

```powershell
Set-Location "C:\Users\Administrator\Documents\work\Nuwax AI Partner\Nuwax AI Partner 7.0\NuwaxAIPartner-v2"
.\tools\run-conversation-host-checks.ps1
.\tools\run-v2-product-host-checks.ps1
.\tools\run-meeting-host-checks.ps1
.\tools\run-monitoring-host-checks.ps1
.\tools\run-provisioning-host-checks.ps1
.\tools\run-wake-word-host-check.ps1
.\tools\check-aec-human-protection-replay.ps1
node .\tools\protocol\validate-protocol-assets.js
node .\tools\protocol\generate-na-fixtures.js
node .\tools\fake-device\fake-device-conformance.js
```

`test/host/run_tests.sh` 需要在已记录版本的 Bash/WSL 环境运行；不得因 Windows PowerShell 无法直接执行而跳过，也不得在未确认脚本写入范围前直接运行。

```powershell
Set-Location "C:\Users\Administrator\Documents\work\Nuwax AI Partner\Nuwax AI Partner 7.0\cloud-gateway"
npm test
npm run test:device-hello
npm run test:conversation-endpoint
npm run test:barge-in
npm run test:meeting
npm run test:onboarding
npm run test:ota
```

工作区级脚本需要先审阅参数、设备/网关依赖和写入行为，再按授权级别执行：

```powershell
Set-Location "C:\Users\Administrator\Documents\work\Nuwax AI Partner\Nuwax AI Partner 7.0"
.\tools\check-audio-sample-manifest.ps1
.\tools\run-smart-barge-in-nohuman-test.ps1
.\tools\run-smart-barge-in-test.ps1
.\tools\run-speaker-cancel-recovery-test.ps1
.\tools\run-monitoring-long-duration-test.ps1
```

其中涉及网关启动、串口、设备、真人、音频文件生成或长稳运行的脚本属于相应 B/D 授权，不因列在本节而自动获准。脚本质量门还应分别记录 PowerShell AST parse、`node --check`、Python `py_compile`、Shell `bash -n`、CMake configure/lint 和 Kconfig 解析结果；工具不存在时登记能力缺口。

构建与产物检查：

```powershell
Set-Location "C:\Users\Administrator\Documents\work\Nuwax AI Partner\Nuwax AI Partner 7.0\NuwaxAIPartner-v2"
. "C:\esp\v6.0\esp-idf\export.ps1"
$Build = "C:\tmp\nuwax-v2-review-<batch>-s3"
$GeneratedSdkconfig = "C:\tmp\nuwax-v2-review-<batch>.sdkconfig"
if ((Test-Path $Build) -or (Test-Path $GeneratedSdkconfig)) { throw "Use a new review batch path" }
idf.py -B $Build `
  -D "SDKCONFIG=$GeneratedSdkconfig" `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.product-ota.defaults" `
  set-target esp32s3
idf.py -B $Build `
  -D "SDKCONFIG=$GeneratedSdkconfig" `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.product-ota.defaults" `
  build
python -m esptool --chip esp32s3 image-info "$Build\NuwaxAIPartner.bin"
Get-FileHash "$Build\NuwaxAIPartner.bin" -Algorithm SHA256
```

测试脚本必须读取当前产品生成配置，而不是只依赖 host stub/fallback；若脚本和 `sdkconfig` 参数不一致，先登记配置漂移 P1，不能以 host 通过关闭问题。

旧版 `P8 Fault Injection Matrix`、历史串口日志、旧镜像 hash 和旧 host check 只能作为用例来源，不能作为当前版本结论。所有 P0/P1 关闭证据必须基于当前源码、当前 `SDKCONFIG_DEFAULTS`、当前构建目录和当前镜像重新执行。

---

## 7. 评审记录模板

### 7.1 模块评审记录

```text
模块：
责任人：
源码范围：
依赖/被依赖：
状态机/任务/锁/队列：
资源预算与峰值：
正常路径：
错误路径：
断电/断网/重启路径：
静态证据：
主机测试证据：
实机证据：
已知问题：
结论：代码已证明 / 待实机 / 产品待决策 / 发布阻塞
```

### 7.2 问题登记模板

```text
ID：
等级：P0/P1/P2/P3
模块：
现象：
触发条件：
影响：
最小复现：
源码位置（绝对路径+行号）：
根因证据：
修复方案：
修复边界/副作用：
验证命令与产物：
实机/云端/硬件证据：
残余风险：
责任人/截止批次：
关闭条件：
```

### 7.3 每次测试的最小证据包

- 源码批次和配置 hash；
- 命令、环境、开始/结束时间、退出码；
- 串口/网关日志和脱敏后的设备 ID；
- 音频 WAV、JSON manifest、帧统计、序号统计和 WAV header；
- heap/stack/queue/CPU/温度/RSSI 快照；
- OTA manifest、HTTP 状态、设备 OTA 状态、重启原因和回滚结果；
- 失败时的最小复现和未决问题链接。

---

## 8. 角色、并行方式与收口规则

建议分工：

| 角色 | 负责范围 |
|---|---|
| 固件架构负责人 | 任务/锁/状态/所有权、问题裁决和版本基线 |
| 音频负责人 | MIC/I2S/AEC/VAD/打断/播放、声学指标和帧完整性 |
| 硬件负责人 | 原理图、引脚、器件规格、示波器/逻辑分析仪和返修排查 |
| 网络/云端负责人 | WS/TLS/认证/服务协议/会议监控/OTA gateway 和故障注入 |
| UI/应用负责人 | LVGL、状态快照、触摸、电量、MCP、模式交互 |
| 测试负责人 | host、fake gateway、实机、长稳、证据包和回归矩阵 |
| 产品负责人 | 2 分钟休眠、会议/监控弱网语义、延迟预算、量产安全和放行决策 |

可以并行：静态模块盘点、硬件引脚核对、协议/网关模型、主机资源测试。必须串行：基线冻结 -> 修复/重新构建 -> OTA -> 实机长稳 -> 最终放行。

收口规则：

1. P0 未关闭不得发布或执行生产 OTA；仅当 P0 必须依赖硬件验证、静态/主机复核已证明验证镜像不会扩大风险、具备可回滚恢复路径且用户单批批准时，才允许烧录明确标记的非发布验证镜像；
2. P1 必须关闭、降级到明确可接受状态，或由产品负责人书面接受风险；
3. “待实机”和“产品待决策”不能写成“通过”；
4. 修复后的源码、配置、测试脚本和镜像必须使用同一批次 hash；
5. 每次 OTA 后至少重复启动、联网、无真人音频、模式协议和回滚检查；
6. 任何不可逆安全操作（Secure Boot/Flash Encryption/eFuse）必须单独审批，不在普通功能验证中执行。

---

## 9. 最终交付物与产品放行标准

### 9.1 交付物

1. 全工程模块清单和依赖图；
2. 任务/锁/队列/状态机图；
3. 引脚和硬件时序核对表；
4. 音频帧/AEC/打断/播放验收报告；
5. 网络、云端故障和 OTA 安全报告；
6. 内存/栈/CPU/延迟预算与长稳报告；
7. P0/P1/P2/P3 问题登记及修复验证矩阵；
8. 当前版本完整构建、镜像、manifest、hash、OTA/回滚记录；
9. 未决硬件/云端/产品决策清单；
10. 最终“可发布/限制发布/不可发布”评审结论。

### 9.2 可稳定运行的必要条件

只有同时满足下列条件，才可以把设备称为“可稳定运行产品”：

- 当前配置可重复完整构建，分区和镜像校验通过；
- 所有 P0 关闭，P1 有证据关闭或有明确书面接受；
- 启动、关机、断网、云端停止、TLS 错误、任务/队列/内存故障均能有限时间恢复或安全降级；
- 正常对话、播报、打断、会议、监控的协议帧无重复/缺口/乱序/伪成功；
- AEC、人声保留、设备声抑制、物理停播和首音节完整性有真实声学证据；
- 30 分钟/1 小时长稳无持续 heap/stack/queue 恶化、无重启风暴、无任务泄漏；
- OTA 认证、校验、掉电、rollback 和 anti-rollback 有端到端证据；
- app-only OTA 与 bootloader/partition/srmodels/NVS schema 兼容矩阵闭合，恢复出厂/RMA/损坏配置有可用恢复路径，NVS/flash 擦写寿命有预算；
- UI、MCP、BLE 配网和电量状态与业务真实状态一致；
- 量产安全项（签名、Secure Boot、Flash/NVS Encryption、唯一凭据、WSS-only）通过独立 release gate。

若任一项只有静态推断而无所需等级的运行证据，结论必须写为“尚未证明”，不能写为“功能正常”。
