# 阶段 2 A2-04：bootloader 电源保持、strap/eFuse 与下载模式接管审计

审计日期：2026-08-03  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
权限：A 级静态评审。未修改业务源码或配置，未构建，未运行 host/gateway 测试，未访问 COM5，未读写设备 eFuse/NVS，未执行 OTA 或烧录。

## 1. 判定摘要

`A2-04` 的 A 级静态审计完成，但产品验收未通过，状态为
`completed-A;C/D-open`。

已成立的静态结论：

1. 正常 SPI boot 会先由 ROM 从 flash 加载二级 bootloader；IDF 随后在二级
   bootloader 硬件初始化前调用 `bootloader_before_init()`。
2. 当前自定义 bootloader 组件已定义强钩子，先预装 HIGH，再切 GPIO7 为输出，
   用于保持 PG2；现有 7.0.23 外部构建 map 证明该组件和钩子实际进入了二级
   bootloader。
3. 当前 app 与 bootloader 配置恰好一致：PG2=GPIO7、active-high；
   PG1/SW3=GPIO16、active-low。app 在显示、音频、网络初始化前接管 GPIO7。
4. 原理图明确连接 `ESP_BOOT=GPIO0`，SW1 按下接地；USB D-/D+ 直接连接
   GPIO19/GPIO20；GPIO3 作为 LCD 背光控制且有 10 kOhm 下拉。
5. PA_CTRL GPIO4 在 app 驱动前没有被 bootloader 拉高；LCD 背光 GPIO3 的
   外部下拉使复位默认画面保持关闭。静态上未发现早期误开 PA 或背光的主动路径。

尚未成立、必须保留的边界：

1. ROM joint download mode 在二级 bootloader 之前接管，因此自定义 GPIO7
   保电钩子不会执行。若 USB 不能独立维持整板电源，进入下载和整个写 flash
   期间必须持续维持 SW3 或使用独立稳定供电。
2. bootloader 与 app 的 pin/极性没有共享编译契约；合法 Kconfig 改动仍可产生
   bootloader/app 不一致镜像，沿用 `POWER-002`。
3. GPIO45/46 的 ROM 采样电平、实际模组 N32R8 变体、eFuse 状态、LCD/SD
   外设在复位窗口的反向偏置，均不能由原理图和 sdkconfig 百分百证明。
4. 当前开发配置仍开放 ROM download、USB Serial/JTAG、UART0/USB 控制台、
   OCD-aware debug 和 integration-only USB 配网命令；Secure Boot、Flash
   Encryption 和 bootloader anti-rollback未形成量产关闭证据。
5. 未做 GPIO7 无低脉冲波形、GPIO0/3/45/46 上电采样、USB 重枚举、连续下载、
   下载中断电和 eFuse summary，因此不能宣称“无需按住 SW3 烧录”或“量产调试面
   已关闭”。

## 2. 冻结输入与证据等级

| 输入 | identity / SHA-256 | 用途 |
|---|---|---|
| source manifest | 7,918 项；`ba22f1e64a4b95e19cca8d32e789798f5a15ee8c1f8af6b8d38553cc5bf71450` | 当前源码边界；A2-05 后续漂移不改变 boot handoff 结论 |
| `nuwax_early_power_hold.c` | `421456834cafb96d6bdfc95a9d74ebfda20bf9d1b89e3c9ef982aa5d87a63d63` | 二级 bootloader 钩子 |
| bootloader Kconfig | `184d46d8e9821590e5edd04db2b8f1aad61440c7ec974da0739405dc486d0d7b` | early hold 可配置边界 |
| `nuwax_power_ctrl.c` | `7d54c54cc7e6385cea8e816f40a6ef2e4cfa0a8f6a099ee3cb42b43286793824` | app 电源接管/关机 |
| `sdkconfig` | `ea74b3ffc7ce84c2a02af8cf3ef022267bffd12eb6489721d77d2f32ab97e07d` | 当前生成配置 |
| 主板一原理图 | `bb5dbe5dd050e4058484ee11cf6fb16efbb11b1c7f77b9e1bc8c9ecda176c21b` | 电源、SW1/SW3、USB、SD |
| 主板二原理图 | `76f52901511cc2501fbf0af8bf31ff9b00435562cea01bcb39c21d6f9b706d7a` | ESP32-S3、pin map、LCD/codec |
| 既有 7.0.23 bootloader map | `3a442766be793cd4624bf33d3cf82d2037e1ceb38d516a27576d9aea2a9affc1` | 只读链接证据，不绑定本 source manifest |

原理图视觉检查覆盖主板一全部 1 页和主板二全部 3 页。PDF 渲染警告缺少若干
窄体字体，但网络名、器件、引脚和阻值可辨；本轮未修改或重新导出原 PDF。

证据等级限制：现有 `C:\tmp\nuwax-v2-fix-7.0.23-20260803-d` 是本轮开始前已
存在的构建资产。它能证明某次 7.0.23 构建链接了钩子，不能证明该 bin 与本轮
source manifest、最终量产 eFuse 或待测 COM5 设备一一绑定。

## 3. ROM、二级 bootloader 与 app 接管时序

```text
上电/复位
  |
  +-- ROM 采样 GPIO0/3/45/46 和 eFuse
  |     |
  |     +-- SPI boot
  |     |     -> ROM 从 flash 加载二级 bootloader
  |     |     -> bootloader_before_init()
  |     |          采样固定 GPIO16 / RTC STORE0 marker
  |     |          条件成立时 GPIO7 预装 HIGH -> GPIO output -> HIGH
  |     |     -> bootloader_init / 分区选择 / 镜像校验
  |     |     -> app_main
  |     |          nuwax_power_ctrl_init()
  |     |          GPIO7 再预装 hold level -> output -> hold level
  |     |          启动 SW3 task，之后显示/音频/网络初始化
  |     |
  |     +-- ROM joint download mode
  |           -> ROM USB/UART 下载协议
  |           -> 不加载二级 bootloader
  |           -> bootloader_before_init() 不执行
  |           -> GPIO7 不能由固件 early hook 保电
  |
  +-- 电源是否持续由硬件/USB/按住 SW3/治具决定
```

IDF 证据：`C:\Espressif\frameworks\esp-idf-v5.5.4\components\bootloader\
subproject\main\bootloader_start.c:22-40` 明确说明 ROM 已加载二级 bootloader 后才
进入 `call_start_cpu0()`，并在 `bootloader_init()` 前调用 before-init hook。
`bootloader_hooks.h:27-36` 将该 API 定义为二级 bootloader 前/后初始化钩子。

项目链接证据：

- `bootloader_components/nuwax_power_hold/CMakeLists.txt:1-2` 注册组件；
- `nuwax_early_power_hold.c:19-23` 用 `bootloader_hooks_include()` 配合 IDF
  `-u` 链接选项强制拉入 archive；
- 既有 `bootloader.map:5596-5598,5678-5680` 分别给出
  `bootloader_before_init=0x403c898c` 和
  `bootloader_hooks_include=0x403c9568`；
- map 中 `libnuwax_power_hold.a` 被加载，故不是“源码存在但未链接”。

## 4. bootloader 早期保持逻辑

### 4.1 正向控制

`nuwax_early_power_hold.c:25-58` 的顺序是：

1. 固定把 GPIO16 配为输入并按 active-low 采样；
2. 读取 RTC STORE0 的 intentional-off marker；
3. intentional-off 且 deep-sleep reset 时记为 SW3 wake；
4. `key_pressed || sw3_deep_sleep_wake || !intentional_off` 时清 marker；
5. 对 configured hold pin 先写 HIGH，再接 GPIO matrix、选择 GPIO function、
   关闭 open-drain、启用输出，最后再次写 HIGH。

预装输出值后再 output-enable 的意图正确，可降低软件主动产生低脉冲的概率。
但 LL 寄存器顺序只能证明指令顺序，不能证明 GPIO7 从电源有效到钩子执行之间的
模拟波形，也不能证明板级锁存阈值和 RC 裕量。

### 4.2 配置边界

| 项目 | bootloader | app | 当前结果 |
|---|---|---|---|
| PG2 pin | `CONFIG_BOOTLOADER_NUWAX_POWER_HOLD_GPIO=7` | `CONFIG_NUWAX_POWER_CTRL_GPIO=7` | 匹配 |
| PG2 active level | 固定 HIGH | Kconfig 可选 HIGH/LOW/OBSERVE；当前 HIGH | 匹配但可漂移 |
| SW3 pin | 宏固定 GPIO16 | Kconfig 当前 GPIO16 | 匹配但可漂移 |
| SW3 active level | 固定 LOW | Kconfig 当前 active-low | 匹配但可漂移 |
| pin 范围 | bootloader `0..21` | hold `-1..48`，key `0..21` | 未共享 |
| 冲突检查 | 未排除 GPIO0/3/16 或 hold=key | 未与 bootloader 做构建断言 | 不满足长期契约 |

bootloader Kconfig 的 `range 0 21` 不等于引脚合法性检查。它允许 GPIO0、GPIO3、
GPIO16 等产品已占用/strap/key 引脚，也未禁止 early hold pin 与 key pin 相同。
因此沿用 `POWER-002`，不把“当前值匹配”写成风险已关闭。

## 5. app 接管与 intentional-off

`main/nuwax_ai_partner_main.c:15537-15545` 在显示、音频、Wi-Fi 和云初始化前调用
`nuwax_power_ctrl_init()`。`nuwax_power_ctrl.c:401-445` 先处理 intentional-off
marker，再配置 GPIO7 并启动 SW3 worker。

接管函数 `configure_power_hold_output()` 在 `:78-96` 同样采用“预装电平 ->
配置输出 -> 重写电平 -> 解除 deep-sleep hold -> 重写电平”的顺序。当前正常
profile 是 HIGH；静态上 bootloader 和 app 之间没有主动 LOW 写入。

intentional-off 路径在 `:224-258` 检查 marker：

- SW3 仍按下时清 marker并继续启动；
- 没按下时配置 EXT1 wake，明确把 GPIO7 写成 release level并进入 deep sleep；
- USB 仍给 MCU 供电时表现为黑屏/deep sleep，下一次 SW3 唤醒；
- 电池 rail 真正掉电时 RTC marker/no-init 保留能力取决于供电消失方式。

该逻辑已由 A2-02/A2-03审计。本项只确认二级 bootloader和 app 的接管边界，
不重复宣称系统级 quiesce 已完成。

## 6. 启动、关机和下载模式矩阵

| 场景 | ROM 选择 | early hook | GPIO7 owner | 预期 | 当前证据/缺口 |
|---|---|---|---|---|---|
| 电池冷启动，点按 SW3 | SPI boot | 执行 | bootloader -> app | SW3 可在 early hold 后松开 | A级逻辑成立；D级测最短按压和 PG2 波形 |
| USB+电池冷启动 | SPI boot | 执行 | bootloader -> app | rail稳定、USB枚举 | 未测 USB/电池反灌与重枚举 |
| app 单击关机 | 不立即重进 ROM | 不适用 | app释放PG2后deep sleep | PA/背光关闭，黑屏 | 系统 quiesce 缺口沿用 `POWER-001` |
| intentional-off 后 SW3唤醒 | SPI boot/deep-sleep reset | 执行 | bootloader -> app | 清 marker并保持PG2 | A级链成立；D级重复验证 |
| brownout/WDT/panic reset | SPI boot | 执行 | bootloader -> app | 非 intentional-off 时重新拉高 | crash-loop/供电恢复属于A2-03 |
| SW1 拉低 GPIO0 复位 | ROM download | **不执行** | ROM；GPIO7无固件 owner | 等待USB/UART下载 | 稳定供电条件未闭合 |
| ROM download 写 flash中 | ROM download | **不执行** | ROM | 写入期间电源持续 | 松开SW3可能掉电并留下不完整镜像 |
| 下载完成 hard reset | 重新采样 strap | 取决于GPIO0是否释放 | SPI boot后重新执行 | 新镜像启动 | 需验证SW1释放、USB重枚举、启动日志 |
| 两个 OTA 槽均不可用 | ROM download恢复 | **不执行** | ROM | 有线恢复 | 无factory槽，恢复同时依赖 `POWER-003/PART-003` |

## 7. strap pin 静态矩阵

ESP32-S3 ROM 把 GPIO0/3/45/46 作为复位采样输入。项目自身也在
`esp_vocat_v1_0_pins.h:77-81` 和 `nuwax_board.c:67-71` 明确标注“validate
reset levels/eFuses”，说明这些值原本就不是仅靠运行期 GPIO driver 可关闭的风险。

| strap | 原理图连接 | 复位态静态判断 | app复用 | 未关闭风险 |
|---|---|---|---|---|
| GPIO0 | `ESP_BOOT`，SW1 按下接 GND；未见独立外部上拉 | 松开时依赖模组/芯片默认；按下为 LOW | 无普通业务复用 | SW1抖动、低脉冲宽度、ROM实际boot mode需D级 |
| GPIO3 | LCD_BL NMOS gate，R5 10 kOhm下拉 | 板级明确 LOW，背光默认关闭 | LCD背光 active-high | JTAG source语义仍受eFuse策略影响；需记录strap/eFuse |
| GPIO45 | SD_CMD，主板一 SD区有10 kOhm上拉 | 板级倾向 HIGH | SD 1-bit CMD | VDD_SPI/模组变体/eFuse必须与N32R8实物一致；当前未读eFuse |
| GPIO46 | LCD_SDA3直连面板，未见独立上下拉 | 芯片内部默认之外，外设可能产生偏置 | LCD QSPI D3 | 面板上电/复位窗口电平未知；影响 joint download 组合 |

关键边界：原理图标注模组为 `ESP32-S3-WROOM-1-N32R8`，但没有生产 BOM、
模组 eFuse summary 或样机丝印闭环。不能根据型号字符串反推本样机 GPIO45 电平和
VDD_SPI 已正确，更不能在未知 eFuse 上执行不可逆烧写。

## 8. PA、背光、USB 与串口早期状态

| 资源 | 原理图/配置 | ROM/bootloader阶段 | 判定 |
|---|---|---|---|
| PA_CTRL GPIO4 | NS4150B CTRL；app定义active-high | 自定义hook不配置GPIO4 | 没有早期主动拉高证据；仍需示波器确认上电无爆音 |
| LCD_BL GPIO3 | R5 10 kOhm下拉、active-high NMOS | 自定义hook不配置GPIO3 | 复位默认背光关闭；后续app显式开启 |
| USB GPIO19/20 | Type-C D-/D+直接连接模组 | ROM download、USB Serial/JTAG共用 | 硬件路径存在；稳定性和安全策略未验收 |
| UART0 GPIO43/44 | 原理图接底座UART net | 当前主控制台UART0，921600 | 底座非本阶段功能验收，但量产debug surface仍开放 |

## 9. eFuse、下载和调试安全矩阵

| 控制面 | 当前静态配置 | 能证明 | 不能证明/量产要求 |
|---|---|---|---|
| Secure Boot | `CONFIG_SECURE_BOOT` 未启用 | 当前构建不要求签名boot链 | 实物eFuse状态、签名key、revoke策略均未知 |
| Flash Encryption | 未启用 | 当前构建允许明文flash | 实物crypt count、密钥注入/RMA策略未知 |
| bootloader anti-rollback | 未启用 | app rollback开启但不是eFuse anti-rollback | secure version和烧写流程未定义 |
| ROM download | Secure Boot/Flash Encryption关闭，未选择永久禁用 | 开发阶段有线下载设计可用 | `CONFIG_SECURE_ROM_DL_MODE_ENABLED=y`只是Kconfig能力派生值，不能证明secure download eFuse已烧 |
| USB Serial/JTAG | driver enabled；secondary console enabled | app可通过GPIO19/20输出/接收 | JTAG禁用eFuse、RMA授权和端口负测未完成 |
| UART0 | custom console GPIO43/44、921600 | 当前有应用控制台 | 量产是否关闭、脱敏、限权未完成 |
| OCD-aware debug | app/FreeRTOS OCD-aware开启 | 开发调试友好 | 量产攻击面未关闭 |
| USB配网控制 | `CONFIG_NUWAX_PROVISION_USB_CONTROL_ENABLE=y` | 精确命令可触发 BLE reprovision | 无认证，仅适合集成构建；量产必须关闭 |

`main/nuwax_provision_new/nuwax_provision_usb_control.c:47-72` 从物理
USB Serial/JTAG读取完整行；精确匹配
`NUWAX_CTRL start_ble_provisioning` 后调用重配网 callback。该入口依赖物理接入，
但没有命令级认证。此风险已由 `CONFIG-004` 和量产 debug surface 审计覆盖，本项
不创建重复安全 ID。

## 10. Canonical 问题映射

| ID | 等级 | A2-04 证据与状态 |
|---|---|---|
| `POWER-002` | P2 | bootloader固定SW3 pin/active-low和PG2 HIGH，app可独立改变；当前匹配但无共享断言，继续开放 |
| `POWER-003` | P2 | 新增：ROM download绕过二级bootloader，GPIO7 early hold不执行；稳定下载依赖按住SW3、USB独立保电或治具，当前无冻结SOP/波形/连续恢复证据 |
| `POWER-001` | P1 | app关机不是系统级quiesce，A2-02已有唯一登记，本项不重复 |
| `PART-003` | P2 | 无factory槽时最终恢复依赖ROM download，而ROM路径又受 `POWER-003` 供电条件约束 |
| `CONFIG-002` | P1 | Secure Boot/Flash Encryption未启用，量产启动/存储安全开放 |
| `CONFIG-004` | P1 | product overlay继承开发下载、控制台、JTAG和USB配网控制面，量产profile未闭合 |
| `HW-EVID-001` | P1 | 无生产BOM/netlist/PnP和模组/eFuse绑定，原理图不能证明样机贴装/strap电平 |

### POWER-003 关闭条件

该问题不是“ROM bootloader 软件 bug”。关闭必须在硬件、工装和SOP三者中给出
唯一、可重复的保电方案：

1. 明确下载模式供电拓扑：USB能否独立维持 MCU、flash和必要rail，或必须按住
   SW3，或使用量产治具强制PG2/主电源；
2. SOP写明进入下载、连接确认、擦写、hash verify、hard reset和允许松开SW3的
   唯一时点；写入中禁止释放；
3. 连续至少30次从关机态进入ROM下载、完整写入、hash verify、退出并正常启动，
   不出现COM消失、write timeout、半写镜像或供电掉落；
4. 在下载开始前、擦除中、写入中和verify后分别断电，确认可再次进入下载并恢复；
5. 保存GPIO7、VOUT、MCU_3V3、VBUS、EN和GPIO0/46波形，绑定板号、固件hash、
   bootloader hash和eFuse summary。

## 11. C/D 级关闭用例

| 等级 | 用例 | 必须断言 |
|---|---|---|
| C | 用冻结 product profile干净构建bootloader/app | map中钩子唯一且地址有效；boot/app PG2、SW3 pin/极性构建断言一致 |
| C | 检查最终bootloader/app/sdkconfig/image-info tuple | target/flash/mode/partition/secure配置与发布manifest绑定 |
| C | 量产profile负向构建 | 固定PoP、ws、USB配网控制、未批准console/JTAG/ROM策略导致构建失败 |
| D | 冷启动最短SW3按压扫描 | 从按下到GPIO7稳定HIGH的p95/p99及最差板温/电量有余量 |
| D | GPIO7接管波形 | ROM->boot hook->app全程无低于锁存阈值/最小脉宽的掉电毛刺 |
| D | GPIO0/3/45/46复位采样 | 松开/按下SW1、SD卡插拔、LCD不同上电相位均进入预期boot mode |
| D | eFuse summary只读留证 | 模组变体、VDD_SPI、JTAG、USB download、secure download、secure boot/encryption状态明确 |
| D | USB/UART下载矩阵 | USB直连、UART治具、仅USB、USB+电池、低电量均按批准策略成功或明确拒绝 |
| D | 下载中掉电恢复 | 任意断点不造成不可恢复状态；可重新进ROM并写入已批准镜像 |
| D | 早期输出波形 | PA_CTRL不误高、LCD_BL保持低、无喇叭爆音或背光闪烁 |
| D | intentional-off/USB组合 | 关机黑屏、USB插拔、SW3唤醒、USB重枚举和RTC marker语义稳定 |

不可逆 eFuse 动作不属于上述“只读 summary”。任何 eFuse 写入、JTAG/ROM download
永久关闭、Secure Boot或Flash Encryption启用必须另行评审、备份恢复策略并获得
用户明确批准，不能作为普通测试步骤执行。

## 12. 最终判定

- 二级 bootloader early hold：**源码顺序正确，现有 7.0.23 map证明已链接**。
- 正常 SPI boot 的 bootloader -> app PG2接管：**当前配置静态一致**。
- ROM download：**确定绕过 early hook，不能依赖固件保持GPIO7**。
- strap/eFuse：**原理图已定位，但GPIO45/46和实物eFuse仍无D级证据**。
- USB/JTAG/console：**开发能力开启，量产安全profile未关闭**。
- A2-04：`completed-A;C/D-open`，不是硬件电源时序、ROM恢复或量产安全验收通过。
