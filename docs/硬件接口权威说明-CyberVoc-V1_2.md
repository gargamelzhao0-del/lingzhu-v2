# CyberVoc V1.2 硬件接口权威说明(真机实锤)

最后核定: 2026-06-22 经真机(COM8)复测修正 AEC reference 口径
权威来源: 参考固件 `cyberkit-main/main/boards/CyberVoc-Board-V1_2/`(能跑通) + 真机 I2C 扫描 + 音频 smoke 实测
状态: **本文件取代旧版 `6.4硬件原理图详细说明-ESP-VoCat-V1_0.md`(已删除,旧版基于错误的 V1.0/原理图假设)**

> 2026-06-22 AEC reference 覆盖裁决:2026-06-18 曾把 ES7210 slot2/MIC3 判为已确认硬件 playback reference；最新同板复测显示该结论不能作为放行依据。7.0 health 修正后仍为 `slot2/ref amp1k=0.9`、`passed=0`；`cyberkit-main` reference AECVERIFY 虽能在 slot2 看到播放相关能量，但最终候选为 `slot0` 并警告正常路由 R slot2 未喂入最强候选。当前有效口径:`slot2/MIC3` 是 reference candidate，硬件 AEC 只有在 ref_health/A-B 通过后才能启用。

> 2026-06-17 复检补充:已再次对照可跑通参考代码 `cyberkit-main/main/boards/CyberVoc-Board-V1_2/`。7.0 工程以 CyberVoc V1.2 为硬件基线;参考工程顶层 `sdkconfig` 可能选择 V2.0,但本次只采用用户确认的 V1.2 板级目录为事实来源。

> ⚠️ 版本判定经过:用户最初提供的 §1.2 引脚表与真实 PCB 有出入。参考代码含 V1.0/V1.2/V2.0 三套。
> 经真机判定为 **CyberVoc V1.2 子变体**,依据:
> 1. I2C 全扫 `addrs=0x18 0x40 0x55 0x68`,IMU 在 **0x68**(排除 V2.0 的 QMI8658@0x6A/0x6B)。
> 2. 音频 smoke:`din=3 + pa=15 + GPIO48 拉高` → mic `signal=1`(smoke passed);
>    旧 `din=15 + pa=4` → `signal=0`(全零,failed)。实锤 V1.2 子变体脚位。

## 0. 模组与系统

| 项 | 值 | 来源 |
|---|---|---|
| 主控 | ESP32-S3(WROOM 类) | 真机 |
| Flash / PSRAM | **16MB flash + 8MB OCT PSRAM(N16R8)** | 真机 esptool flash_id |
| PSRAM 模式 | OCT, 80MHz | sdkconfig |
| 采样率(输入/输出) | **16000 / 16000 Hz** | V1.2 config + smoke 实测 rx_rate≈15984Hz |
| AEC reference | **候选:ES7210 slot2/MIC3**;默认 V1.2 config 未启用;2026-06-22 复测未放行 | COM8 health/AECVERIFY 复测 + V1.2 config |

## 1. I2C 共享总线(端口0)

| 信号 | GPIO | 说明 |
|---|---|---|
| I2C SCL | **1** | codec/IMU/电量计/触摸 共享 |
| I2C SDA | **2** | 同上(触摸也在此总线,非独立 port) |

挂载设备(真机扫描全部 ACK):

| 设备 | 7-bit 地址 | 用途 |
|---|---|---|
| ES8311 | **0x18** | 扬声器 codec/DAC |
| ES7210 | **0x40** | 4 通道麦克风 ADC |
| BQ27220 | **0x55** | 电量计,设计容量 **1200mAh** |
| IMU(BMI270 类) | **0x68** | V1.2 参考固件调用 BMI270 初始化/摇晃检测,真机 ACK 在位 |
| 触摸 CST816S | **0x15** | ✅ P1-4 真机 ACK 并报点/手势通过;早期未 ACK 根因=LCD RST 极性错(active-low)摁住共用 FPC,改 RST47 active-high 后正常 |

## 2. 音频(ES7210 mic + ES8311 codec + NS4150B PA)

| 信号 | GPIO | 备注 |
|---|---|---|
| I2S MCLK | **42** | 256×fs = 4.096MHz |
| I2S BCLK | **40** | |
| I2S WS/LRCK | **39** | |
| I2S DOUT(→ES8311) | **41** | |
| **I2S DIN(←ES7210)** | **3** | ⚠️ V1.2 子变体值(V1.0 为 15) |
| **PA_CTRL(NS4150B 使能)** | **15** | ⚠️ V1.2 子变体值(V1.0 为 4) |
| **codec 模拟电源使能** | **48** | ⚠️ V1.2 必须**启动早期拉高**,否则 ES7210 mic 无模拟供电、采样全零 |

> 真机验证:GPIO48=1 + din=3 + pa=15 时,ES7210/ES8311 init ok,smoke `signal=1` 通过。
>
> 2026-06-18 AEC 回采复核曾显示 ES7210 slot2/MIC3 有播放相关能量；但 2026-06-22 同板复测覆盖该放行口径。当前只能把 slot2/MIC3 作为候选回采链路，不能据此开启硬件 AEC。MIC4/slot3 仍按 unused/NC 处理。

## 3. 电源控制

| 信号 | GPIO | 说明 |
|---|---|---|
| POWER_CTRL | **9** | V1.x 单脚电源保持/关机控制(✓ 原 §1.2 此项正确;V2.0 才是 LCD 数据线) |

> 注:V1.2 参考固件在 `InitializeButtons()` 中将 `POWER_CTRL(GPIO9)` 配为输出并写 0。用户已确认硬件供电正常后,7.0 vocat 默认策略对齐为 **HOLD_LOW**;OBSERVE/HOLD_HIGH 仅保留作诊断模式。

## 4. 显示 LCD QSPI(ST77916,360×360)

| 信号 | GPIO | 备注 |
|---|---|---|
| QSPI PCLK/SCK | **18** | |
| QSPI CS | **14** | |
| QSPI D0 | **46** | [strap] |
| QSPI D1 | **13** | |
| QSPI D2 | **11** | |
| QSPI D3 | **12** | |
| **LCD_RST** | **47** | ⚠️ V1.2 子变体值(V1.0 为 3);参考代码 `reset_active_high = 1` |
| LCD_BL(背光) | **44** | 复用 U0RXD;参考代码 `PwmBacklight(GPIO44, false)`,即 active-high |

> ⚠️ 勘误:旧 §1.2 标的 `LCD_DC=GPIO45`、`LCD_TE=GPIO8` 在 V1.x config 中**不存在**
> (QSPI 协议不用 DC;TE 未引出)。GPIO45/8 的真实用途待**硬件侧**核实,勿当 LCD 控制线用(非阻塞,与 P1-4 触摸无关)。

## 5. 触摸

| 信号 | GPIO | 备注 |
|---|---|---|
| 触摸芯片 | CST816S | ⚠️ 旧 §1.2 写 CST816T,实为 **CST816S**;✅ P1-4 真机报点/手势通过(addr 0x15) |
| TP_INT | **10** | V1.2 值(V2.0 为 11) |
| TP_RST | 无(NC) | |
| TOUCH_PAD1 / TOUCH_PAD2 | **7 / 6** | 电容触摸/唤醒脚(V1.2:PAD2=6) |

## 6. SD 卡(1-bit)、UART、其它

| 信号 | GPIO | 备注 |
|---|---|---|
| SD CLK / CMD / D0 | **16 / 38 / 17** | |
| UART1 TX / RX | **5 / 4** | V1.2 子变体值 |
| LED_G | **43** | 复用 U0TXD |
| BOOT | **0** | [strap] ESP_BOOT |

## 7. Strap 脚(启动敏感)

GPIO0(BOOT)、GPIO46(LCD_D0)为 strap 脚,外部上下拉与上电状态需复核,避免影响启动模式。
(旧 §1.2 把 GPIO45 也列为 strap-LCD_DC,但 V1.x 无 LCD_DC,该项作废。)

## 8. V1.0 vs V1.2 子变体差异速查(本机=V1.2)

| 信号 | V1.0 | **V1.2(本机)** |
|---|---|---|
| I2S DIN | 15 | **3** |
| PA_CTRL | 4 | **15** |
| codec 模拟电 | 无 | **GPIO48 拉高** |
| LCD_RST | 3 | **47** |
| TOUCH_PAD2 | NC | **6** |
| UART1 TX/RX | 6/5 | **5/4** |

> 参考固件靠运行时探测(拉高 GPIO48 后探 0x18 ACK)自动切到 V1.2。本工程当前为**硬编码 V1.2 值**。
