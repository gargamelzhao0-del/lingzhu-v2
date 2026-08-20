# 阶段 3 A3-02：PSRAM、Flash、电源轨、温升、brownout、PA 瞬态与 I2C 卡死审计

日期：2026-08-03

状态：`completed-A;C/D-open;product-not-accepted`

授权边界：本项只执行 A 级静态审计和原理图文本/视觉复核。没有构建、host/gateway 测试、COM5、NVS/eFuse 读取或写入、OTA、烧录、示波器/逻辑分析仪/电源负载测量。文中的电压、温度、电流和时序“应测”是关闭门禁所需的证据，不是已经测得的值。

## 1. 审计结论

1. 当前配置要求 16 MiB DIO 80 MHz Flash、8 MiB Octal 80 MHz PSRAM；`CONFIG_SPIRAM_MEMTEST=y`、`CONFIG_SPIRAM_IGNORE_NOTFOUND=n`、brownout 最高档和 SPI Flash brownout reset 均已启用。源码有容量下限 selftest，但 `app_main()` 没有调用 `nuwax_hardware_selftest_run()`，所以不能把这些保护写成当前产品镜像已执行。
2. 原理图主板二标注 `ESP32-S3-WROOM-1-N32R8`，而当前 `sdkconfig`、`partitions.csv` 和烧录契约按 16 MiB Flash 闭合到 `0x1000000`。N32R8 实物料号、Flash JEDEC ID/容量和当前镜像没有绑定证据；登记 `HW-FLASH-001`（P1）。16 MiB 可能是有意的可用窗口，也可能掩盖模组/profile 漂移，不能从一次成功启动推导量产一致性。
3. `VOUT` 是电池/USB power-path 输出；`MCU_3V3` 和 `CODEC_3V3` 为独立 DCDC，4G 另有 GPIO15 控制的约 4.231 V 升压。NS4150B 的 VCC 直接接 `VOUT`，PA_CTRL 为 GPIO4，原理图给出 `R18=10 kOhm` 下拉、`C49=1 uF` 和 `R19/R20=100 kOhm` 输入网络。当前代码没有样机绑定的 VOUT/3V3/4G/PA 电流、启动/停播瞬态或温升数据，登记 `HW-ELEC-001`（P1）。
4. 共享 I2C manager 只提供 bus init/get/add/remove/probe；没有 SCL 脉冲、STOP、GPIO 临时接管、bus delete/recreate 或全设备重新绑定 API。touch、BQ27220、QMI8658A 和 codec 都复用该 bus；touch 明确在超时后保留同一 handle，battery 只回收 fuel-gauge device。SDA/SCL 被外设拉死时，当前路径最多得到各设备事务 timeout，不能证明后续能恢复，登记 `I2C-REC-001`（P1）。
5. ES8311 音量校准存在可计算的硬件参数错配：`vocat_audio_board.c` 设置 `pa_voltage=5.0f`、`codec_dac_voltage=3.3f`、未设置 `pa_gain`；依赖库按 `20*log10(dac/pa)+pa_gain` 计算并在 `set_vol()` 中扣除该值。原理图 PA VCC 并非固定 5 V，且 `240 kOhm / 100 kOhm` 的外部反馈网络约为 2.4 V/V（约 7.6 dB）。默认 `-6 dB`、AEC 测试音和不同电量/USB条件下的实际声压因此没有可信的统一标定，登记 `PA-GAIN-001`（P1）。
6. brownout 配置可以触发复位，`nuwax_power_ctrl` 只在下一次启动读取 `ESP_RST_BROWNOUT` 并打印告警；没有掉电前 rail sample、保留计数、PA/Wi-Fi 负载关联或自动恢复证据。该缺口与 `OBS-001`、`POWER-001` 交叉引用，C/D 仍开放。

## 2. 权威设计证据

| 资料/源码 | SHA-256 | 本项使用内容 |
|---|---|---|
| `docs/最新硬件原理图/3591576609SCH_主板一_2026-05-09.pdf` | `bb5dbe5dd050e4058484ee11cf6fb16efbb11b1c7f77b9e1bc8c9ecda176c21b` | VOUT power-path、MCU_3V3、4G_VCC、CODEC_3V3、SD 上拉、电池与 BQ27220 |
| `docs/最新硬件原理图/3591608336SCH_主板二_2026-05-09.pdf` | `76f52901511cc2501fbf0af8bf31ff9b00435562cea01bcb39c21d6f9b706d7a` | N32R8 标注、I2C/I2S、PA_CTRL、NS4150B PA、AEC 网络 |
| `main/board/i2c_bus_manager.c` | `40eba8c20ac9442dc0d9cf179085eb35b90f00fd85fbff5ebdd345677676411e` | shared bus 生命周期和 API |
| `main/board/vocat_audio_board.c` | `fc547edbee8becf89c31c8528c1299b66aa2cc89d065b8231e4f9100daec7e23` | codec/PA 初始化、音量参数、停播/测试音路径 |
| `main/board/hardware_selftest.c` | `63fa8174348ed59ca59eaf80df6da08d6309c9d2e8159ba3007f9deb232310cf` | Flash/PSRAM/I2C/LED/电源自检实现 |
| `managed_components/espressif__esp_codec_dev/esp_codec_dev_vol.c` | `3ec591e1757b331832c2b68c267bc18d0c6edc8b2a6933184c16935ca9ba102c` | `pa_voltage`/`pa_gain` 换算公式 |
| `managed_components/espressif__esp_codec_dev/device/es8311/es8311.c` | `89bea26707abf7313806c2fc15299a22bcd96d8072d83794ff0e2dd2afd20dbe` | `set_vol()` 如何应用 hardware gain |
| `partitions.csv` | `c693eadce94dcf8fdc35686cb55d33b99f8589b31beae2efc52df29b828b6af0` | 16 MiB 闭合分区布局 |

## 3. 原理图电源/负载矩阵

| 网络/器件 | 原理图事实 | 当前软件关联 | A级判断 |
|---|---|---|---|
| `VOUT` | 电池/USB power-path 后的系统输出；PA VCC 直接接此网 | `nuwax_power_ctrl` 只释放 PG2 关机，音频驱动不测 rail | 电池电压、USB、PA 峰值和 brownout 需 D 级测量 |
| `MCU_3V3` | TLV62569 DCDC，图中反馈给出约 3.315 V | ESP32/数字外设供电 | 没有样机冷启动、Wi-Fi 峰值和音频并发波形 |
| `CODEC_3V3` | 独立 SY8088 DCDC；原理图没有 GPIO codec power switch | `NUWAX_VOCAT_CODEC_PWR_GPIO=(-1)`，由 DCDC 常供电 | 正确避免虚构 GPIO 电源 owner，但上电纹波/负载瞬态未验收 |
| `4G_VCC` | TPS61023，GPIO15=`4G_EN`，图中约 4.231 V | app 启动强制关闭；AT profile 才打开 | 当前产品不启 4G；启用前必须测升压启动和 MCU_3V3 串扰 |
| NS4150B | VCC=`VOUT`，CTRL=GPIO4；R18 10 kOhm 下拉，C49 1 uF | ES8311 codec GPIO interface 管理 PA，关机路径强制 GPIO4 low | active-high 静态一致；RC 上电/停播边沿和 PA 峰值未测 |
| BQ27220/QMI8658A | 共享 GPIO1/2 I2C，地址分别 0x55/0x6a | 各自 device handle，未提供 shared bus recovery | 设备事务超时不等于总线恢复 |

## 4. Flash/PSRAM/brownout 静态矩阵

| 项目 | 当前配置/代码 | 风险边界 |
|---|---|---|
| Flash 模式 | DIO、80 MHz、16 MiB (`sdkconfig:905-939`) | 原理图模块标 N32R8；未绑定 JEDEC/料号，登记 `HW-FLASH-001` |
| 分区 | nvs 0x9000/0x6000，otadata 0xf000/0x2000，factory/ota_0/ota_1 各 0x400000，storage 0x3e0000 | 当前布局闭合到 16 MiB；任何 32/16 MiB 产品决策、OTA manifest 和烧录脚本必须统一 |
| PSRAM | Octal 80 MHz、boot init、memtest、missing 不忽略、malloc 走 PSRAM，internal reserve 65536 bytes (`sdkconfig:2767-2813`) | `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` 要求逐任务 cache-disabled/ROM 路径证据；总容量没有产品启动时的精确 SKU 绑定 |
| selftest | `platform_memory_test()` 用 `esp_flash_get_size()` 和 `esp_psram_get_size()` 检查 Flash>=16 MiB、PSRAM>=8 MiB (`hardware_selftest.c:33-52`) | selftest 只有定义；最新 `app_main()` 不调用，不能作为当前镜像实测证据 |
| brownout | `CONFIG_ESP_BROWNOUT_DET=y`、level=7、`CONFIG_ESP_BROWNOUT_USE_INTR=y`、`CONFIG_SPI_FLASH_BROWNOUT_RESET=y` (`sdkconfig:2600-2622,4277-4283`) | 只能证明配置意图；无受控电压跌落、复位计数、NVS/OTA完整性和负载相关性证据 |

## 5. 关键代码链

### 5.1 I2C 卡死

- `nuwax_i2c_bus_init()` 创建唯一 I2C0 bus，状态只在 `UNINITIALIZED -> INITIALIZING -> READY`，没有 deinit/recovery 状态；`nuwax_i2c_bus_add_device/remove_device/probe()` 只是 handle 管理或 probe (`main/board/i2c_bus_manager.c:38-128`)。
- touch、QMI8658A、BQ27220 和 codec 都用 `i2c_master_transmit*()`，超时分别来自配置/驱动常量；touch recovery 明确保留现有 I2C handle (`main/touch_input.c:270-283`)。battery 连续错误只回收 BQ device，不会释放 shared bus。
- 因此 SDA 被拉低、SCL 被拉低、半帧后从设备保持 ACK/clock stretch 等场景没有从 GPIO matrix 临时接管并发送 9 个 SCL 脉冲/STOP 的路径；只加大 timeout 不能恢复物理总线。

### 5.2 PA/音量/瞬态

- `vocat_audio_board.c:586-595` 创建 ES8311，配置 `pa_pin=GPIO4`、`pa_voltage=5.0f`、`codec_dac_voltage=3.3f`，未设置 `pa_gain`。
- 依赖库 `esp_codec_dev_vol.c:50-60` 计算 `20*log10(dac_voltage/pa_voltage)+pa_gain`；ES8311 `set_vol()` 在 `es8311.c:400-412` 用该值扣减后写 DAC volume register。原理图 page 3 的 NS4150B 外部反馈/输入网络给出 `240 kOhm/RIN`，当前 RIN=100 kOhm，约 7.6 dB；VOUT 也不是固定 5 V。
- `set_external_pa_enabled()` 只对 GPIO4 做 set/readback (`vocat_audio_board.c:923-941`)；它不测 VOUT、PA 电流或 CTRL RC 边沿。关机另由 `nuwax_power_ctrl.c:155-174,299-318` 强制 GPIO4 low、等待 20 ms 后释放 PG2，不能替代停播/上电波形验收。

### 5.3 电源与 brownout

- bootloader/app 的 PG2 先置 high 再接管，正常启动和 intentional-off 路径已登记在 A2-04；app 对 brownout 只在 `nuwax_power_ctrl_init()` 读取 reset reason 并记录 (`nuwax_power_ctrl.c:401-423`)。
- 没有 MCU 温度传感器、PA/VOUT ADC、输入电流、DCDC PG/EN readback 或 brownout breadcrumb；BQ27220 温度字段只代表电池 gauge，不代表 ESP32/PA 温升。

## 6. 新增问题与关闭方案

### `HW-FLASH-001`（P1）：模块容量、Flash profile 与分区未绑定

原理图标注 `ESP32-S3-WROOM-1-N32R8`，当前配置和分区按 16 MiB；selftest 仅检查下限且未从 `app_main()` 调用。若量产模组、Flash JEDEC 或烧录脚本发生变体，无法由当前静态资料证明启动、OTA 和分区边界一致。

关闭条件：读取并绑定指定样机的 Flash JEDEC/manufacturer/capacity、PSRAM ID/capacity、模组料号和 eFuse 摘要；冻结“32 MiB 物理/16 MiB 可用”或“16 MiB 物理”唯一产品决策；让分区、`sdkconfig`、esptool、OTA descriptor 和 selftest 对同一 identity 做构建期检查。不得通过盲目扩大分区或写 eFuse 解决。

### `HW-ELEC-001`（P1）：电气/热/掉电验收证据缺失

当前没有绑定设备、硬件版、镜像 hash 和运行场景的 VOUT/MCU_3V3/CODEC_3V3/4G_VCC/PA 电流、温升、PA_CTRL 瞬态、brownout 和恢复数据。代码只提供配置或事后日志，不能证明连续对话、Wi-Fi 峰值、播报高音量和低电量场景稳定。

关闭条件：使用同一 release manifest 在冷启动、电池/USB、Wi-Fi 连接、采集、播放、AEC 测试音、最大/最小音量和 brownout 受控跌落场景采集 rail/电流/温度/复位/文件完整性证据；阈值由芯片/电源/扬声器规格和硬件负责人冻结，并将每次波形绑定 `run_id`。

### `I2C-REC-001`（P1）：共享 I2C 无物理卡死恢复

关闭条件：在共享 bus manager 增加唯一 owner 的有界 recovery API（停用事务、临时 GPIO、SCL 脉冲、STOP、恢复输入模式、重新建 bus/device handles），并规定失败后各 driver 的 epoch/重试策略；C 级构建检查无第二 bus owner，D 级分别注入 SDA-low/SCL-low/设备掉电/恢复，验证 touch、codec、BQ、IMU 不互相破坏。

### `PA-GAIN-001`（P1）：PA 供电/反馈增益未进入 ES8311 音量模型

关闭条件：硬件负责人确认 VOUT 的最小/标称/最大值、NS4150B RIN/RF 实装值和扬声器额定功率；代码将实际 `pa_voltage`/`pa_gain` 纳入唯一 volume model，并在各音量档测量 DAC、PA 输出、削波率和 AEC reference，确保低电量/USB/高音量不因错误补偿而过驱。

## 7. C/D 验收矩阵（本轮未执行）

| 证据 | 方法 | 通过条件 |
|---|---|---|
| Flash/PSRAM identity | 读取 JEDEC/容量、绑定模组与 image/partition manifest；冷启动 + 连续 PSRAM/Flash 访问 | identity 唯一；无 cache/PSRAM fault、panic、hash mismatch；配置/分区/烧录一致 |
| Rail/thermal | 电池/USB冷启动、Wi-Fi峰值、采集、播报、AEC、最大音量、4G关闭基线；测 VOUT/MCU_3V3/CODEC_3V3/PA 电流/温升 | 所有值在规格/硬件预算内；无异常纹波、过热或 PA_CTRL 误脉冲 |
| Brownout | 受控跌落并记录 reset reason、PG2、NVS/otadata、下一次启动和 OTA 状态 | 只发生受控复位；无 NVS/分区破坏；可区分 brownout 与其他 reset |
| I2C stuck | fixture 拉低 SDA/SCL、设备掉电/恢复，记录每事务耗时和 bus recovery 波形 | 每次恢复有界；SCL/STOP符合器件要求；所有共享设备可重新 probe，主音频/UI不永久阻塞 |
| PA/volume | 低电量、USB、音量档位和 1 kHz/TTS；同步测 GPIO4、VOUT、PA 输出、DAC 削波和 MIC3 reference | 实测声压/增益与模型一致；无高音量过驱；停播后 PA 关闭且不混入采集 |

## 8. 本次执行日志

- 2026-08-03 完成原理图主板一1页、主板二3页的文本提取和 PNG 视觉复核；确认 VOUT、MCU_3V3、CODEC_3V3、4G_VCC、N32R8、PA_CTRL/NS4150B 和 I2C level-shifter 网络。
- 本轮只修改审计证据、索引和工作记录；未修改产品源码、配置、分区、协议或硬件资料，未构建、未运行 host/gateway 测试，未访问 COM5/NVS/eFuse，未 OTA/烧录。A3-02 保持 `completed-A;C/D-open;product-not-accepted`。
- `main/nuwax_ai_partner_main.c`、`sdkconfig` 或关键 board/codec 文件若在后续继续漂移，A3-02 的中间结论必须在最终 source manifest 收口时重新执行。
