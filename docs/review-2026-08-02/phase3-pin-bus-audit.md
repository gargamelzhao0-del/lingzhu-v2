# 阶段 3 A3-01：新版硬件 GPIO、复用与总线 owner 审计

日期：2026-08-03

状态：`completed-A;C/D-open;product-not-accepted`

授权边界：本项只执行 A 级静态审计。没有构建、host/gateway 测试、COM5、NVS/eFuse 读取或写入、OTA、烧录、示波器/逻辑分析仪测量。文中“匹配”只表示当前源码/配置与原理图设计连接一致，不代表样机贴装、电平、时序或功能已经通过。

## 1. 审计结论

1. 当前有效 `sdkconfig`、`main/board/esp_vocat_v1_0_pins.h`、商家 V2 `config.h` 与三份喵伴 2.0 原理图的主要 GPIO 映射一致：共享 I2C 为 GPIO1/2，音频 I2S 为 GPIO38~42，PA 为 GPIO4，PG2/SW3 为 GPIO7/16，QSPI LCD 为 GPIO3/9/10/12/13/14/21/46，触摸中断为 GPIO11，SD 为 GPIO45/47/48，4G 为 GPIO5/6/15，头部触摸为 GPIO8，绿灯为 GPIO18。
2. 当前产品配置没有发现两个独立运行期 driver 同时配置同一个 GPIO 的直接冲突。GPIO4 的 audio owner 与关机强制静音、GPIO7 的 bootloader 与 app 接管属于有意的分阶段 owner；GPIO43/44 的底座 UART 自检与 UART0 console 冲突已有编译期 `#error` 阻断。
3. GPIO45 是 `SD_CMD`，原理图有外部 10 kOhm 上拉；ESP32-S3 在 reset 时又把 GPIO45 用于 VDD_SPI 电压选择。当前没有绑定本样机的模组/eFuse 摘要、复位波形或量产 fixture 证据，不能证明所有复位/下载/掉电场景下 strap 安全，登记 `HW-PIN-001`（P1）。这不是当前已复现启动失败，也禁止在未知状态下直接烧 eFuse。
4. GPIO8 头部触摸、GPIO18 绿灯以及 `nuwax_hardware_selftest_run()` 均有源码并进入正式 CMake，但最新源码调用反查显示正常产品 `app_main()` 没有初始化 GPIO8 或 GPIO18，`CONFIG_NUWAX_HW_ACCEPTANCE_MODE` 也没有调用整板自检函数。`nuwax_touch_init()` 初始化的是 GPIO11 的 LCD 触摸中断，不是 GPIO8 头部输入。代码存在不等于当前镜像可达，登记 `DRV-REACH-001`（P1）。后续归属应分别为 QMI8658A 的 A3-11、LED 的 A3-12、GPIO8 产品/诊断输入与整板 selftest 的 A3-14/A3-16，不应把 GPIO8 归入 A3-11。
5. A3-01 的“所有使用 GPIO 有唯一 owner 并与权威资料一致”产品验收未通过：当前活动路径的静态映射基本成立，但 GPIO45 strap/eFuse、GPIO46 reset 采样、GPIO8/18 产品可达性、实物贴装与实际电平仍开放。

## 2. 证据与优先级

### 2.1 一级设计证据

| 资料 | SHA-256 | 本项使用内容 |
|---|---|---|
| `docs/最新硬件原理图/3591524974SCH_麦克风_2026-05-09.pdf` | `cda061f4559268e5db7f6ab66fcab997f4ad170899b0e351d2ea933ad72e4273` | MIC1/MIC2、TOUCH、LED_G 与 FPC 网络 |
| `docs/最新硬件原理图/3591576609SCH_主板一_2026-05-09.pdf` | `bb5dbe5dd050e4058484ee11cf6fb16efbb11b1c7f77b9e1bc8c9ecda176c21b` | PG1/PG2、SD 外部上拉、底座 UART、USB、电源与传感器总线 |
| `docs/最新硬件原理图/3591608336SCH_主板二_2026-05-09.pdf` | `76f52901511cc2501fbf0af8bf31ff9b00435562cea01bcb39c21d6f9b706d7a` | ESP32-S3 引脚、I2C/I2S/QSPI/4G/UART/TOUCH 和 codec 网络 |

三份 PDF 的全部 5 个相关页面已同时执行文本提取和 PNG 视觉复核。缺失 Arial Narrow 等显示字体时，网络名、器件和值仍可从矢量页面和文本层交叉读出；本轮没有用 OCR 猜测不可辨识字段。

### 2.2 实现与参考证据

| 证据 | 快照位置/事实 |
|---|---|
| 当前 pin map | `main/board/esp_vocat_v1_0_pins.h:14-81`；文件名是历史兼容名，正文明确为 Miaopan 2.0 |
| 当前有效配置 | `sdkconfig:1156-1164,1362-1365,1400-1404,1421-1447,2835-2856,5301-5302` |
| 商家 V2 参考 | `../NuwaxAIPartner测试工程/cyberkit-main/main/boards/CyberVoc-Board-V2_0/config.h`；映射与当前主要 pin map 一致 |
| GPIO 初始化 | `main/board/nuwax_power_ctrl.c`、`board_gpio_inputs.c`、`led_driver.c`、`uart_driver.c`、`sd_card_driver.c`、`vocat_audio_board.c`、`display.c`、`touch_input.c` |
| ESP32-S3 约束 | 当前 IDF 5.5.4 文档明确 GPIO45 reset 电平决定 VDD_SPI；GPIO0/45/46 为应避免无条件复用的 strap GPIO |

商家源码属于实现参考，不覆盖当前原理图；原理图属于设计证据，不覆盖 BOM/netlist/PnP 和实物测量。资料缺口继续由 `HW-EVID-001` 跟踪。

## 3. 当前有效 GPIO owner 矩阵

“产品可达”按当前 `sdkconfig` 和 `app_main()` 调用链判定；“诊断可达”要求入口与编译条件同时存在。没有执行构建或真机验证。

| GPIO | 原理图网络 | 方向/电气语义 | 当前 owner | 产品可达性 | 静态结论与残余风险 |
|---:|---|---|---|---|---|
| 0 | `ESP_BOOT` | reset strap；运行期输入，低有效；板上 SW1 对地 | ROM；`board_gpio_inputs` 仅诊断读取 | 正常产品不初始化 board input | 下载语义明确；外部输入/fixture 与 GPIO0 释放时点需 D 级验证 |
| 1 | `ES_I2C_SCL`/`I2C_SCL`/`TP_SCL` | open-drain 双向；原理图 2.2 kOhm 上拉，driver 还启用内部上拉 | `i2c_bus_manager` I2C0 | 是 | 单 bus owner 正确；上拉等效值、上升沿和卡死恢复归 A3-02/A3-09 |
| 2 | `ES_I2C_SDA`/`I2C_SDA`/`TP_SDA` | open-drain 双向；原理图 2.2 kOhm 上拉，driver 还启用内部上拉 | `i2c_bus_manager` I2C0 | 是 | 同 GPIO1；全部器件共享物理总线与代码单 bus 一致 |
| 3 | `LCD_BLK` | output，active-high；原理图 R5 10 kOhm 下拉驱动 NMOS；strap 相关 | `display` LEDC/GPIO；关机 owner 可强制 low | 是 | 运行态一致，reset 默认关闭；strap/eFuse 与上电波形仍需 D 级证据 |
| 4 | `PA_CTRL` | output，active-high；NS4150B CTRL 有 10 kOhm 下拉 | codec GPIO interface；关机路径强制 low | 是 | 分阶段 owner 合理；mute/pop/掉电波形归 A3-05 |
| 5 | `4G_TX`（模组 TX -> ESP RX） | UART2 RX input | `uart_driver` 可选 4G | 默认只把 4G rail 关断，不启 UART | 命名易按网名误读，但当前和商家 V2 `ML307_RX_PIN=GPIO5` 一致 |
| 6 | `4G_RX`（ESP TX -> 模组 RX） | UART2 TX output | `uart_driver` 可选 4G | 默认不启 UART | 与商家 V2 `ML307_TX_PIN=GPIO6` 一致；功能不在本轮硬件验收范围 |
| 7 | `PG2` | output，active-high power hold；原理图有下拉/三极管 latch | bootloader early hook -> app `nuwax_power_ctrl` | 是 | 当前 pin/polarity 匹配；bootloader/app 可独立漂移及 ROM download 绕过沿用 `POWER-002/003` |
| 8 | `TOUCH` | input，原理图头部触摸输出；无软件 pull | `board_gpio_inputs` | 否 | driver 已链接但正常启动无 init；`nuwax_touch_init()` 仅对应 GPIO11 TP_INT，属于 `DRV-REACH-001` |
| 9 | `LCD_SDA1` | QSPI data output | `display` SPI2 | 是 | 与原理图/商家一致 |
| 10 | `LCD_CS` | QSPI CS output，idle high | `display` SPI2/GPIO | 是 | 与原理图/商家一致；不是当前文档所定义的板级 strap owner |
| 11 | `TP_INT` | input、内部 pull-up、falling-edge IRQ | `touch_input` | 是 | 与原理图/商家一致；ISR/恢复归 A3-09 |
| 12 | `LCD_SDA2` | QSPI data output | `display` SPI2 | 是 | 与原理图/商家一致 |
| 13 | `LCD_SDA0` | QSPI data output | `display` SPI2 | 是 | 与原理图/商家一致 |
| 14 | `LCD_SCL` | QSPI clock output | `display` SPI2 | 是 | 当前 40 MHz；时钟/信号完整性归 A3-08 |
| 15 | `4G_EN` | output，high enables 4.231 V rail；driver 配下拉 | `uart_driver` | app 启动立即驱动 low | 默认 fail-safe 关闭；可选 AT 测试才拉高，功能不在本轮硬件验收范围 |
| 16 | `PG1` | RTC input，active-low；内部 pull-up，SW3 feedback | `nuwax_power_ctrl` | 是 | 与原理图/商家一致；deep-sleep EXT1 与关机事务归 A2-02/A3-02 |
| 17 | `LCD_TE` | panel output -> MCU input，无 pull | `display` TE selftest | 正常刷新未用；诊断需开关 | 无冲突；静态不能证明 TE 频率/连线 |
| 18 | `LED_G` | LEDC output；原理图 LED 到 3V3，active-low sink | `led_driver` | 否 | driver/policy 已链接但正常启动无 init，属于 `DRV-REACH-001`；生命周期另沿用 `LIFE-006` |
| 19 | `USB_DN` | USB fixed function | ROM/USB Serial-JTAG | 是 | 不作为普通 GPIO owner；量产 debug surface 沿用 `CONFIG-004` |
| 20 | `USB_DP` | USB fixed function | ROM/USB Serial-JTAG | 是 | 同 GPIO19 |
| 21 | `LCD_RST` | output，active-low | `display` | 是 | 当前配置与面板/原理图一致 |
| 22~37 | 未作为板级通用 IO 使用 | N32R8 模组内部/未引出组合 | flash/PSRAM/未用 | 不适用 | 不得仅按芯片裸 die GPIO 表重新分配；实物模组仍由 `HW-EVID-001` 约束 |
| 38 | `I2S_DI` | input，ES7210 TDMOUT -> ESP | `vocat_audio_board` shared I2S0 RX | 是 | 与原理图/商家一致；slot 和 reference 归 A3-03/A3-07 |
| 39 | `I2S_WS` | master output，共享 TX/RX | `vocat_audio_board` I2S0 | 是 | 单 duplex channel owner；时钟归 A3-06 |
| 40 | `I2S_BCK` | master output，共享 TX/RX | `vocat_audio_board` I2S0 | 是 | 同 GPIO39 |
| 41 | `I2S_DO` | output，ESP -> ES8311 DSDIN | `vocat_audio_board` I2S0 TX | 是 | 与原理图/商家一致 |
| 42 | `I2S_MCLK` | master output，共享 codec clock | `vocat_audio_board` I2S0 | 是 | 与原理图/商家一致；实测频率归 A3-03/A3-04/A3-06 |
| 43 | `U0TXD`/底座 `U1TXD` net | UART0 console TX；诊断候选 UART1 TX | console；可选 base UART | 当前仅 console | `hardware_selftest.c:24-26` 在 console 开启时禁止 base loopback 构建，当前无双 owner |
| 44 | `U0RXD`/底座 `U1RXD` net | UART0 console RX；诊断候选 UART1 RX | console；可选 base UART | 当前仅 console | 同 GPIO43 |
| 45 | `SD_CMD` | SD command 双向，原理图 10 kOhm 上拉；reset strap | ROM strap；可选 SDMMC | 当前不挂载 SD | 外部上拉与 VDD_SPI strap/eFuse 未绑定，`HW-PIN-001` |
| 46 | `LCD_SDA3` | reset strap；运行期 QSPI output | ROM strap -> `display` SPI2 | 是 | 映射一致；reset 电平/eFuse/面板端行为无 D 级证据，沿用 `HW-EVID-001` |
| 47 | `SD_DA0` | SD data 双向，内部/外部 pull-up | 可选 `sd_card_driver` | 当前不挂载 | 无其他 owner；本轮不宣称 SD 可用 |
| 48 | `SD_CLK` | SD clock output | 可选 `sd_card_driver` | 当前不挂载 | 无其他 owner；本轮不宣称 SD 可用 |

## 4. 总线 owner 与复用裁决

| 总线/资源 | 引脚 | owner 与模式 | 冲突审计 | 结论 |
|---|---|---|---|---|
| I2C0 | 1/2 | `i2c_bus_manager` 创建唯一 master bus；touch、ES8311、ES7210、QMI8658A、BQ27220只加 device handle | 未发现第二个正式 product bus create | A 级 owner 一致；时序/卡死恢复未验收 |
| I2S0 duplex | 38/39/40/41/42 | `vocat_audio_board` 一次 `i2s_new_channel` 同时取得 TX/RX；mic/speaker wrapper借用 shared handles | 当前 V2 条件分支避免 mic/speaker各自再建 channel | A 级 owner 一致；格式/时钟/DMA归 A3-03~06 |
| SPI2/QSPI LCD | 3/9/10/12/13/14/21/46 | `display` 唯一初始化 SPI2，BL 独立 LEDC | 未发现正式产品第二个 SPI2 owner；vendored库声明不等于被当前路径实例化 | A 级 owner 一致；GPIO46 strap和信号完整性开放 |
| UART0 console | 43/44 | 921600 console，secondary USB Serial/JTAG开启 | base UART loopback若开启则由编译期 `#error` 阻止 | 当前配置无双 owner；诊断 profile必须切USB-only console后独立 C/D 验证 |
| UART2/4G | RX=5、TX=6、EN=15 | 默认只把 EN 拉低；AT probe显式打开 | 与其他当前 owner无冲突 | 静态隔离成立，A3-13继续审计失败影响 |
| SDMMC 1-bit | CLK=48、CMD=45、D0=47 | 仅显式 hardware acceptance 且确认插卡时挂载 | 当前产品路径不挂载 | 无运行态冲突；strap和硬件功能均未放行 |
| USB | 19/20 | ROM/USB Serial-JTAG | 不作为普通 GPIO | 开发可用；量产暴露面开放 |

## 5. 配置与调用可达性

### 5.1 当前活动功能

- `CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0=y` 是历史兼容符号，当前 pin header 和 board banner 均明确指向 Miaopan 2.0；不能按名字把它误判为 V1.0 实物。
- 显示、触摸、共享 I2S、speaker、battery、PG2/SW3均在当前配置启用；4G rail 在 `app_main()` 开头显式置低。
- `CONFIG_NUWAX_HW_ACCEPTANCE_MODE`、SD fixture、base UART loopback、4G AT 和 P2 audio smoke 均未在当前配置启用。

### 5.2 `DRV-REACH-001`（P1）：板级功能/整板自检不可达

静态调用反查结果：

- `nuwax_led_init()` 只被 `led_status.c` 与 `hardware_selftest.c` 调用，但 `nuwax_led_status_init()` 在第一方产品源码中没有调用者。
- `nuwax_board_gpio_inputs_init()` 只被 `hardware_selftest.c` 调用；GPIO8 头部触摸 callback/config API同样没有产品调用者。正常产品中的 `nuwax_touch_init()` 是 GPIO11 面板触摸路径，不能作为 GPIO8 的调用证据。
- `nuwax_hardware_selftest_run()` 只有定义和头文件声明。`app_main()` 使用 `CONFIG_NUWAX_HW_ACCEPTANCE_MODE` 影响依赖和 battery 启动条件，却没有执行该函数。

影响：当前镜像不能因为这些 `.c` 文件被链接，就宣称头部触摸、绿灯或“整板 acceptance mode”可用；启用 acceptance Kconfig 还会抑制部分普通服务，却不产生预期自检结果。

关闭方案：

1. 产品先决定 GPIO8 头部输入和 LED_G 是否为正式交互能力。若是，分别建立唯一 product policy owner，在确定的启动 stage 初始化，并在 shutdown/OTA/BLE/异常回滚中停止；若否，从产品 capability/文档和正式 CMake 中明确移出，不能保持伪能力。GPIO8 的产品接入/诊断归 A3-14/A3-16，不能由 QMI8658A A3-11 代替。
2. `CONFIG_NUWAX_HW_ACCEPTANCE_MODE` 必须在独立诊断 profile 的明确入口调用 `nuwax_hardware_selftest_run()`，或删除该开关及误导性的主入口条件。自检不得进入量产默认路径。
3. C 级分别构建 product 与 hardware-acceptance profile，保存预处理/链接 map，证明入口存在且 UART console 冲突门生效。
4. D 级验证 GPIO8 press/release/long press、LED off/on/blink/breathe/关机默认态及自检 summary；每项绑定 source/config/image hash。

### 5.3 `HW-PIN-001`（P1）：GPIO45 SD 上拉与 VDD_SPI strap 未闭合

原理图把 GPIO45 连接 `SD_CMD`，SD 卡区域存在 10 kOhm 上拉。ESP32-S3 reset 语义为 GPIO45 high 选择内部 1.8 V VDD_SPI，low/NC通常选择 3.3 V；实际行为还受模组内部 flash/PSRAM、eFuse 和板级电平影响。当前资料没有本样机 eFuse summary、复位采样电压或 N32R8生产绑定。

影响：不能从“当前设备曾正常启动”外推所有样机、下载模式、掉电斜坡和 SD 插卡状态都安全；错误 strap 可能在 app 执行前影响 flash/PSRAM 供电选择。

关闭方案：

1. 只读获取指定样机的 eFuse summary并绑定设备唯一ID、模组丝印、硬件版和源码/镜像；不得在本项执行 eFuse 写入。
2. 核对模组官方 N32R8 flash/PSRAM供电规格与量产料号，补生产 BOM/netlist/PnP。
3. 示波器测 GPIO45 在电池冷启动、USB冷启动、SW3开机、hard reset、ROM download、SD插入/拔出时的 reset 采样窗口电平，并同时确认 3V3/VDD_SPI/PG2。
4. 由硬件负责人裁决是否需要电阻改值、隔离或固定 eFuse；任何不可逆 eFuse 方案必须独立审批和量产SOP。

## 6. 正向控制

1. I2C0 和 I2S0 都有单一 board-level creator，避免同一外设被多个高层 wrapper重复安装。
2. PG2 在 bootloader 中先 preload high 再 output-enable，app接管也先预装电平再配置方向，降低接管低脉冲风险。
3. PA_CTRL与LCD_BL都有外部下拉设计证据，app关机路径会强制置低；这只是设计/静态控制，不替代波形验收。
4. GPIO43/44 的自检冲突由编译期门禁阻止，不会在当前 console profile中静默形成 UART0/UART1双 owner。
5. SD、base UART和4G不是当前产品启动依赖；默认配置不会挂载SD或打开4G UART，app只把4G rail置低。

## 7. 后续证据门禁

| 级别 | 必须执行 | 通过条件 |
|---|---|---|
| A | 全部阶段结束后按最终 source manifest 重跑本矩阵 | pin/config/callsite无漂移；`DRV-REACH-001/HW-PIN-001`状态按最新代码刷新 |
| C | product、hardware-acceptance、USB-only UART loopback profile分别干净构建/link | board profile唯一；冲突profile fail-fast；实际入口进入ELF且无重复driver owner |
| D | GPIO/总线静态电平、reset strap、I2C rise/fall、I2S/QSPI/UART/SD波形及交互验证 | 每项绑定设备/硬件/镜像/run_id；无错误方向、误拉电平或不可恢复冲突 |

A3-01 不能单独关闭音频、显示、触摸、传感器或电源的功能验收。它只证明当前活动源码快照的主要引脚设计映射基本一致，并明确了仍阻止产品验收的 owner、可达性和硬件证据缺口。

## 8. 本次执行日志

- 2026-08-03 并行漂移后重新只读复核最新源码快照：`main/nuwax_ai_partner_main.c` SHA-256 `6ae1308012bd969b69c275155b2b8ba8a9770c536eec8b241bb8098b35328da1`（776223 bytes，2026-08-03 19:05:10 +08:00）；`sdkconfig` SHA-256 `e3f594fb8eb78137a340faf97a8591d8d394d8225259be8ae538b8725e701987`；`main/Kconfig.projbuild` SHA-256 `83fd22d181c1dd0cfc1897cecc5b5c20a5c5437c86e6143c9918ffa136b8b70c`。
- `app_main()` 的当前调用链（约 `main/nuwax_ai_partner_main.c:15725-16240`）包含 `nuwax_touch_init()`、显示和音频初始化，但没有 `nuwax_board_gpio_inputs_init()`、`nuwax_led_status_init()` 或 `nuwax_hardware_selftest_run()`；`CONFIG_NUWAX_HW_ACCEPTANCE_MODE` 只出现在条件编译/服务抑制相关分支，没有 selftest 调用。
- 本次只修改审计证据、索引和工作记录；未修改产品源码、配置、分区、协议或硬件资料，未构建、未运行 host/gateway 测试，未访问 COM5/NVS/eFuse，未 OTA/烧录。`DRV-REACH-001` 和 `HW-PIN-001` 继续开放，A3-01 保持 `completed-A;C/D-open;product-not-accepted`。
- 由于 `main/nuwax_ai_partner_main.c` 在前一快照后发生并行漂移，本次 A3-01 结论已按上述哈希重算；该漂移及其影响继续进入“全部审计完成后按最终 source manifest 的跨阶段全量复核队列”。
