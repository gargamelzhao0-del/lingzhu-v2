# 阶段 3 音频硬件审计

日期：2026-08-03

当前完成项：`A3-06`

状态：`completed-A;C/D-open;product-not-accepted`

授权边界：本轮只执行源码、配置、原理图、规格书和既有日志的 A 级静态审计。没有修改产品源码、配置、分区、协议或硬件资料；没有构建、运行 host/gateway 测试、访问 COM5、读写 NVS/eFuse、OTA、烧录或执行新的板级测量。既有串口日志只作为对应历史样机/镜像的 D 级旁证，不能直接替代当前最终源码和发布镜像的验收。

## 1. A3-03 审计结论

1. ES7210 地址处理正确。板级头文件保存 7-bit 地址 `0x40`，传给 codec 依赖的是 8-bit `0x80`；`audio_codec_ctrl_i2c.c` 创建 ESP-IDF device 时执行 `addr >> 1`，最终仍为 7-bit `0x40`，与原理图和规格书一致。
2. 当前产品配置为 16 kHz、16-bit、四槽 TDM，ESP32-S3 是时钟主设备，ES7210 是 slave。MCLK 为 `256 Fs = 4.096 MHz`；TX 为 2 个 32-bit slot，RX 为 4 个 16-bit slot，两侧均要求 `64 BCLK/Fs = 1.024 MHz`。共享 BCLK/WS 的静态预算一致。
3. ES7210 slave 模式下 `set_fs(16000)` 的采样率配置函数直接返回成功是可解释的：采样率由 ESP32-S3 输出的 MCLK/BCLK/LRCK 决定；驱动仍设置采样位宽和 I2S 格式。该结论只证明软件主从关系自洽，仍需逻辑分析仪确认实际频率、占空比、边沿和 slot 数据。
4. 初始化顺序为：创建 I2S duplex，先启 TX 产生 MCLK并等待约 20 ms，创建 ES7210 I2C/codec interface，软件复位和配置，创建 ES8311，再启 RX/TX，最后将 ES7210 stop 5 ms 后 restart、重设 30 dB gain并 unmute。ES7210 没有独立 reset GPIO；软件复位使用寄存器 `0x00: 0xff -> 0x41`。5 ms 间隔使用 `esp_rom_delay_us(5000)`，不会在 100 Hz tick 下归零。
5. 产品板级初始化失败会调用 `nuwax_vocat_audio_board_deinit()`，按 codec interface、I2C control/GPIO interface、I2S channel 的逆序释放，当前未发现 A3-03 独立的半初始化资源泄漏。该正向结论不覆盖整个 `app_main()` 的全局回滚问题。
6. ES7210 依赖中的 `es7210_update_reg_bit()` 忽略 `es7210_read_reg()` 返回值，仍以初始化为零的值生成并写回寄存器，且只返回 write 结果。一次 I2C 瞬态读失败可能被报告为成功，并覆盖目标寄存器中未修改的位；登记 `CODEC-RMW-001`（P1）。该问题会影响模式、时钟、通道、增益或 mute 的可靠性，并与共享总线无物理恢复的 `I2C-REC-001` 叠加。
7. 驱动向 `0x41/0x42` 均写 `0x70`，源码注释为约 2.87 V；MIC1/MIC2 共用原理图网络 `ADC_MICBIAS12`。现有 11 页 ES7210 产品规格书没有寄存器编程表，不能独立证明 `0x70`、复位值、HPF/OSR、上电状态周期和 stop/start 时序符合芯片要求；必须取得完整 programming guide 或由芯片供应商签核，并实测 MICBIAS。
8. 原理图明确：MIC1/MIC2 是物理麦克风；ES8311 `OUTP/OUTN` 经 R7-R13、C7-C15 AEC 网络进入 ES7210 `ADC_MIC3_P/N`；MIC4 未连接。四路全部 enable 是为了让当前驱动进入 TDM 模式，不能解释为四个物理麦克风。未连接 MIC4 仍上电，属于功耗/噪声实测项，不是当前直接功能故障。
9. ES7210 规格书 Figure 2e 给出的 TDM-I2S 标号顺序为 `Channel 1 -> Channel 3 -> Channel 2 -> Channel 4`，而产品代码把 DMA 索引硬编码为 `MIC1/MIC2/MIC3/MIC4`。但历史逐 PGA 日志已独立确认当前样机 `MIC1 -> DMA[0]`、`MIC2 -> DMA[1]`；因此不能只凭 Figure 2e 判定索引 1/2 必然交换。由于 MIC3/MIC4 在该样机不可观测，`MIC3 -> DMA[2]` 仍没有非循环证据，登记 `AUDIO-SLOT-001`（P1）。
10. 当前配置同时打开 software reference、live AEC 和 slot2 hardware-reference capability，但 `s_hw_ref_health_passed` 默认 false；正常产品启动不运行带测试音的 health gate，只有手动/诊断或硬件验收路径可将其置 true。因此正常启动会 fail-safe 回退 software reference，不会仅因配置打开就使用静音 slot2。正向 fail-safe 不等于硬件 reference 已可用：配置能力、注释、采集拆槽和 health 检测均预设 slot2，仍存在循环验证和产品语义漂移。
11. 所有四个 ES7210 PGA 当前统一设置 30 dB。对物理 MIC1/MIC2 可作为起始工作点；对 ES8311 电气回采的 MIC3，幅度还受 DAC volume、VOUT/PA模型和 R/C 网络影响，不能在没有独立幅频、削波、噪声和延迟测量时宣称 30 dB 合适。该标定缺口交叉引用 `PA-GAIN-001`，最终裁决归 A3-07/A4-08。
12. A3-03 的产品验收不通过。静态上已证明地址和时钟契约自洽、资源失败回滚存在；尚未证明完整寄存器时序、MICBIAS电压、实际 MCLK/BCLK/LRCK、MIC3/slot2 映射和硬件 reference 可用性。

## 2. 证据基线

| 资料/源码 | SHA-256 | 本项使用内容 |
|---|---|---|
| `docs/最新硬件原理图/3591524974SCH_麦克风_2026-05-09.pdf` | `cda061f4559268e5db7f6ab66fcab997f4ad170899b0e351d2ea933ad72e4273` | MIC1/MIC2、`ADC_MICBIAS12` 和差分输入连接 |
| `docs/最新硬件原理图/3591608336SCH_主板二_2026-05-09.pdf` | `76f52901511cc2501fbf0af8bf31ff9b00435562cea01bcb39c21d6f9b706d7a` | ES7210/ES8311、I2S/I2C、OUTP/N 到 MIC3 AEC 网络 |
| `NuwaxAIPartner测试工程/docs/最新硬件原理图/C365743_ADC-DAC-专用型_ES7210_规格书_WJ69534.PDF` | `909a13dcd8770835914ef0de0cb5a44aba6d4312daf1d99768e0dab65f0e87e6` | 7-bit 地址、slave/master能力、TDM时序图、Figure 2e 通道标号 |
| `main/board/esp_vocat_v1_0_pins.h` | `8f76a1d76e28f9c98308983165d2f71e37810f63caeacc6fae4efd3fcfe4a644` | 7-bit/8-bit地址转换常量和板级引脚 |
| `main/board/vocat_audio_board.c` | `fc547edbee8becf89c31c8528c1299b66aa2cc89d065b8231e4f9100daec7e23` | I2S、codec初始化、回滚、诊断与health gate |
| `main/board/vocat_audio_board.h` | `ecf4a3062aa60df536f4f92253b37ad09022dd8d52707e3f8615f07f0a696015` | 当前候选slot语义和诊断接口 |
| `main/mic_input.c` | `b00ede50fcf3698e475892bc83c612190932aa8b5626047fea3b32de0ca083bb` | 四槽拆分为MIC1、MIC2、candidate ref |
| `managed_components/espressif__esp_codec_dev/device/es7210/es7210.c` | `8c741993683579ffbffd05a227533f4a54d086921de8ed99ff6c12f6574d5921` | 软件复位、MICBIAS、slave采样率、TDM、PGA和RMW实现 |
| `managed_components/espressif__esp_codec_dev/platform/audio_codec_ctrl_i2c.c` | `aa399a105b52b3f077c6a08a7dc4448d04c7dd8581ebc91c63f22aab9b0b3620` | 8-bit地址右移为7-bit ESP-IDF device |
| `managed_components/espressif__esp_codec_dev/esp_codec_dev_if.c` | `f186e338082f101513ea058b9283697ba33602bbf55f133640c8fa0746880d21` | codec/control interface close+free语义 |
| `main/nuwax_ai_partner_main.c` | `4d8d758ea28248c6e937e815a8292a7e4362c93cccbf4106a5148c6d65fe48c7` | hardware-reference health状态、启用三条件和software-reference回退 |
| `sdkconfig` | `e3f594fb8eb78137a340faf97a8591d8d394d8225259be8ae538b8725e701987` | 16 kHz、30 dB、software ref/live AEC/slot2 capability、100 Hz tick |
| `runtime-logs/com5-b18-hw-acceptance-monitor-20260727-112549.stdout.log` | `ee64f487b6988f79ed1e6f8769ef32287f49947503d33b45bd82f8e28a00592a` | 历史样机逐 PGA 和 MIC3/reference 失败旁证 |

注：ES7210 规格书目前只在测试工程资料目录中，V2 权威资料目录未包含该文件。这不影响本次只读审计，但正式硬件证据包必须归档唯一受控副本及来源。

## 3. 地址、主从和时钟推导

| 项目 | 当前值 | 代码/资料链 | A级判断 |
|---|---:|---|---|
| ES7210 7-bit地址 | `0x40` | pin header定义`0x40`，codec宏左移为`0x80`，I2C control右移为`0x40` | 一致 |
| sample rate | 16,000 Hz | `VOCAT_AUDIO_SAMPLE_RATE_HZ`和voice input/output均为16 kHz | 一致 |
| sample width | 16 bit | TX/RX和codec `set_fs`均为16 bit | 一致 |
| ESP32-S3角色 | I2S master | IDF TX/RX默认master clock配置；板级不设置codec master | 一致 |
| ES7210角色 | slave | `es7210_codec_cfg_t`零初始化，`master_mode=false` | 一致 |
| MCLK | 4.096 MHz | `mclk_multiple=256`，即`16000*256` | 静态一致，需实测 |
| TX BCLK | 1.024 MHz | 2 slot * 32 bit * 16 kHz | 与共享线预算一致 |
| RX BCLK | 1.024 MHz | 4 slot * 16 bit * 16 kHz | 与共享线预算一致 |
| slave `set_fs` | 不写采样分频 | `es7210_config_sample()`在slave直接成功 | 由外部时钟决定，逻辑合理；需实测 |

当前配置没有启用 vendor 24 kHz profile或 reference-style TDM。若后续切换这些诊断 profile，必须重新计算 TX/RX BCLK；不能沿用本表结论。

## 4. 初始化与失败路径

```text
init_i2s_duplex
  -> enable TX/MCLK
  -> wait about 20 ms
  -> create ES7210 I2C ctrl
  -> create/open ES7210 codec
     -> SW reset 0xff -> 0x41
     -> slave mode, bias/OSR/main clock, four-channel TDM, default 30 dB
  -> set_fs(16 kHz/16-bit/4ch), enable, reapply 30 dB, unmute
  -> create/open ES8311
  -> enable RX and TX
  -> ES7210 stop -> 5000 us -> start -> regain -> unmute
  -> publish s_initialized=true
```

任一步失败时，板级入口调用统一 deinit。当前释放顺序满足 codec close 需要 I2C/GPIO/clock 仍存活的依赖关系。需要保留的风险是：

- `es7210_open()` 多个寄存器写使用按位 OR 汇总，能把非零错误归并为失败，但不能保留第一个精确错误和失败寄存器。
- `es7210_update_reg_bit()` 的 read error 被丢失；这是本项最直接的 codec 驱动错误。
- `log_es7210_key_regs()` 仅记录读失败为 `??`，不作为 init 成功门；必须由产品决定哪些关键寄存器需要强制 readback。
- 缺完整 programming guide，不能把源码注释中的时序和电压当作芯片级权威证据。

## 5. MICBIAS、物理输入与增益

| 路径 | 原理图事实 | 软件动作 | 结论 |
|---|---|---|---|
| MIC1 | 上方物理麦，ES7210 MIC1P/N | enable，30 dB | 历史逐 PGA 证实到DMA[0]；灵敏度/噪声仍需声学验收 |
| MIC2 | 上方物理麦，ES7210 MIC2P/N | enable，30 dB | 历史逐 PGA 证实到DMA[1]；方向性归A3-07 |
| MICBIAS12 | MIC1/MIC2共用 | `REG41=0x70`，注释约2.87 V | 寄存器定义和实测电压均未闭环 |
| MIC3 | ES8311 OUTP/N经AEC网络到ES7210 MIC3P/N | enable，30 dB，候选DMA[2] | 原理图设计成立；当前样机信号和映射未通过 |
| MIC4 | NC | 为TDM而enable，30 dB | 不得作为麦克风；需测功耗/噪声影响 |

统一 30 dB 不能替代分路径标定。MIC3是电气reference，不应默认沿用物理麦的PGA目标；需要在多个播放音量、电池/USB供电和扬声器负载下检查噪声底、削波、线性度、频响及与真实写入PCM的延迟。

## 6. TDM slot证据和边界

历史样机逐 PGA 日志给出：

```text
MIC1 30 dB -> 0 dB: DMA[0] drop 29.86 dB, inferred_slot=0
MIC2 30 dB -> 0 dB: DMA[1] drop 29.99 dB, inferred_slot=1
MIC3 30 dB -> 0 dB: baseline candidate约1，observable=0
MIC4 30 dB -> 0 dB: baseline candidate约0，observable=0
```

同一历史运行还记录：

```text
P3-0D MIC3 silent electrical probe: passed=0
AEC ref health: tone_amp=0.1, passed=0
hardware AEC disabled; fallback to software reference
```

因此证据只能支持：

- 当前样机 `DMA[0]=driver MIC1`、`DMA[1]=driver MIC2`。
- 原理图设计把电气reference接到ES7210 MIC3。
- 代码候选 `DMA[2]=MIC3` 尚未被独立确认。
- 既有样机的硬件reference不通过，不能宣称硬件AEC可用。

不能支持：

- 仅凭Figure 2e宣布当前DMA[1]和DMA[2]已交换。
- 用代码中`VOCAT_AUDIO_SLOT_AEC_REF=2`的同一假设证明health gate监测对象正确。
- 从V1.2或商家参考板的成功日志外推当前V2样机和量产板。

## 7. 产品主链路的fail-safe与语义漂移

当前主链路启用硬件reference需同时满足：

```text
CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE
&& s_hw_ref_health_passed
&& current_frame_ref_active
```

正常产品启动不运行带声测试音的 boot gate，`s_hw_ref_health_passed` 初值为 false，故默认回退软件reference。这一行为是当前的正向安全控制。残余风险是：

- `sdkconfig` 看起来已允许slot2 hardware reference，但正常启动没有建立产品级可通过的health状态，能力声明与运行语义不一致。
- 手动诊断若在错误slot假设上通过，会把同一假设写回全局health标志；必须先用非循环方法冻结映射。
- health状态不是与硬件序列号、镜像hash、codec重初始化generation绑定的持久证据；任何codec/I2S重新初始化后都应失效并重新验证，而不是沿用旧状态。

## 8. Canonical问题

| ID | 等级 | 真实性与影响 | 建议处理 |
|---|---|---|---|
| `AUDIO-SLOT-001` | P1 | MIC1/2映射有历史实测，MIC3/slot2不可观测；规格书通道标号、代码注释和以slot2为目标的health检查不能形成非循环闭环。误映射会令硬件AEC读取静音或错误通道 | 先对MIC3P/N注入独立已知差分信号，逐PGA确认ES7210 MIC3和唯一DMA索引；再跑DAC-only、PA-off相关性/延迟测试。以样机ID、镜像hash和原始采集绑定结果，未通过前强制software ref |
| `CODEC-RMW-001` | P1 | `es7210_update_reg_bit()`忽略read错误，以0执行写回并可能返回成功；I2C瞬态故障可静默破坏未更新位 | read非OK立即返回且禁止write；保留精确错误。对所有RMW调用做read-fail fault injection，断言0次write并验证寄存器/codec状态不变 |
| `HW-EVID-001` | P1 | 缺BOM/netlist/PnP和绑定样机的贴装/铜线证据 | 不从原理图外推量产板，完成实装检查和样机绑定 |
| `I2C-REC-001` | P1 | shared bus无SCL/STOP/bus重建与device epoch重绑 | codec修复不能替代总线owner恢复；完成SDA/SCL拉低注入 |
| `PA-GAIN-001` | P1 | MIC3 reference幅度依赖尚未冻结的DAC/PA/VOUT和AEC网络模型 | 统一音量、reference PGA、实际声压/电气幅度与削波标定 |

## 9. C/D关闭矩阵

| 编号 | 验证 | 方法 | 通过条件 |
|---|---|---|---|
| A3-03-C1 | 寄存器序列 | 在冻结产品构建中记录I2C事务并与完整programming guide/供应商签核表逐项比对 | reset、slave、TDM、bias、gain、mute、start/stop值和顺序全部一致，无未解释写入 |
| A3-03-C2 | RMW read-fail | 对每类RMW注入read timeout/NACK | read失败后0次write，错误准确上报，板级init受控回滚且可重试 |
| A3-03-D1 | 时钟 | 逻辑分析仪同时测MCLK/BCLK/LRCK/DOUT | 4.096 MHz/1.024 MHz/16 kHz在容差内；64 BCLK/Fs、16-bit数据、四槽边界稳定，无停顿/毛刺 |
| A3-03-D2 | reset/start | I2C分析与时钟波形绑定冷启动、codec重启 | MCLK先于codec事务稳定；5 ms间隔及芯片要求满足；重复100次无失锁/静音 |
| A3-03-D3 | MICBIAS | 高阻探头测MICBIAS12，覆盖冷启动、录音、停录、低电/USB | 电压和纹波满足麦克风/codec规格，无越界和异常跌落 |
| A3-03-D4 | 非循环slot映射 | 对MIC1/2/3输入分别注入不同频率/幅度的独立差分信号，逐PGA A/B | 每个ES7210通道只对应一个稳定DMA索引；MIC3映射不依赖预设slot2 |
| A3-03-D5 | MIC3电气reference | PA强制off，DAC-only输出多频/多幅tone并同步保存实际TX PCM与四槽RX | 唯一通道与TX强相关，幅频/噪声/削波/延迟达冻结阈值，quiet无伪相关，cleanup全部成功 |
| A3-03-D6 | 运行fail-safe | 正常启动、health失败、codec重启、I2C故障和health成功五种状态 | 未验证/失效时只用software ref；成功状态与当前hardware generation绑定；错误不沿用旧pass |

阈值必须由芯片规格、模拟链计算和音频负责人共同冻结，不能由现有诊断源码中的经验常量反向定义“通过”。

## 10. A3-03收口

- 地址换算：通过 A 级静态审计。
- 主从/16 kHz/64-BCLK时钟预算：通过 A 级静态审计；实波形开放。
- 初始化和局部资源回滚：通过 A 级静态审计。
- 完整寄存器、MICBIAS和reset/start时序：资料与D级证据开放。
- MIC1/MIC2历史映射：有对应历史样机旁证。
- MIC3/slot2、reference增益和硬件AEC：未通过，不得放行。
- 最终状态：`completed-A;C/D-open;product-not-accepted`。

A3-04及之后的ES8311、PA、I2S/DMA和MIC/reference章节将继续追加到阶段3专项文档；全部审计结束后，还必须用最终source manifest重新核验本节引用的每个源码、配置和调用点。

## 11. A3-04 ES8311 DAC/ADC、I2S主从、音量、静音与pop时序审计结论

1. 当前产品把 ES8311 配置为 `ESP_CODEC_DEV_WORK_MODE_DAC`、slave、外部 MCLK；ESP32-S3 I2S0 TX 是 master。16 kHz、16-bit 数据、双 32-bit slot 产生 MCLK 4.096 MHz、BCLK 1.024 MHz、LRCK 16 kHz，即 `MCLK/LRCK=256`、`MCLK/BCLK=4`。驱动系数表包含 `4.096 MHz/16 kHz/bclk_div=4`，与静态时钟预算一致。
2. ES8311 规格书确认其支持 8~96 kHz、I2S slave、外部同步 LRCK/SCLK、DSDIN 在 SCLK 上升沿采样、差分 OUTP/N 以及 pop/click suppression。现有 12 页规格书没有寄存器表、reset/start/suspend/mute 编程序列、稳定时间或 PA 先后时序，不能独立证明当前寄存器值和 pop 控制正确。
3. ES8311 的 ADC 不属于本产品输入主链路：板级明确使用 DAC-only，麦克风输入由 ES7210 提供。当前静态审计只能证明 ES8311 DAC 路径被配置；不能把芯片具有 ADC 能力写成产品 ES8311 ADC 已启用或已验证。
4. V2 相比根目录参考依赖做了正向改进：`open()` 先 setup 并拉低 PA，`enable(true)` 在 start 后再开 PA/unmute，`enable(false)` 先 mute/关 PA再 suspend，`close()` 也先 mute/关 PA。初始化前 I2S TX/MCLK 已开启，理论上可避免 PA 在无时钟条件下提前开启；但此时没有显式预装并验证完整零DMA ring。该顺序的方向合理，但没有成功门、显式初始静音证明、稳定延时和实测波形时不能宣称无 pop。
5. `es8311_set_fs()` 不检查 `fs==NULL`，非法位宽还会静默按16 bit处理；对正常非空请求则无条件忽略位宽、格式和采样率三个子操作的返回值并返回成功。其下层多处 RMW 在 read 失败后仍基于零值写回；`es8311_config_sample()` 还会在末次 read 时覆盖此前累计错误。板级虽然检查 `set_fs` 返回值，实际仍可能在寄存器配置失败后继续 volume、enable 和 PA 开启。登记 `CODEC-ERR-001`（P1）。
6. `es8311_open()` 在读取 reset 寄存器时覆盖此前初始化事务的错误；`es8311_start()` 在读取 SDP 寄存器时覆盖前两次写错误；`es8311_set_mute()` 忽略实际 write 结果并可能在 read 失败后继续写；PA GPIO `setup/set` 返回值全部被丢弃。这些均属于同一错误传播根因，统一归入 `CODEC-ERR-001`，不重复拆分编号。
7. `es8311_start()` 无论最终寄存器结果是否成功都调用 `es8311_add_pair()`；`es8311_enable(true)` 即使 start 失败仍继续开 PA和解除静音。由于 `codec->enabled` 只在最终返回成功时更新，失败后调用 `enable(false)` 会因“已经是 false”直接返回，无法回滚刚刚发生的 pair/PA/寄存器副作用。登记 `CODEC-STATE-001`（P1）。
8. disable/close 同样不是事务化状态机：disable 忽略 mute 和 PA disable 结果，先从全局 pair 中移除再执行 suspend；close 忽略 mute、PA和suspend全部错误并始终返回成功。`audio_codec_delete_codec_if()` 又会在 close 后无条件 free；板级 teardown 即使检查返回值，也无法发现当前 close 的硬件失败，更不能保留对象和依赖重试。该问题统一归入 `CODEC-STATE-001`。
9. 产品用户音量不是持续修改 ES8311 volume register，而是在 `speaker_output.c` 对每个 PCM 样本按 0~100% 做线性幅度缩放，再复制到左右两个 TX slot。software reference 写入的是同一份缩放后、且已确认完成 I2S 写入的 PCM，静态上保持了正常播放音量和软件 reference 的一致来源。板级向 codec 请求的基准是 `-6 dB`，但驱动还会扣除当前硬件gain模型，不能把请求值等同实际DAC寄存器或声压；诊断路径才会临时改变该codec请求值。
10. 上述正向音量链不消除 `PA-GAIN-001`：板级仍用固定 `pa_voltage=5.0 V`、`codec_dac_voltage=3.3 V`、`pa_gain=0` 计算硬件补偿，而原理图 VOUT 和 NS4150B 反馈网络不匹配该模型。UI 百分比、真实声压、MIC3 电气 reference 和削波上限尚无统一标定。
11. 正常每轮停播不调用 ES8311 mute 或关闭 PA，而是 disable shared TX、向整个 DMA ring 预装零 PCM、重新 enable 并等待 settle。这可减少每轮 PA 开关导致的 pop；完整零预装失败时会保持 TX disabled，避免重放旧 PCM。该主路径的静态防陈旧数据设计成立，实际 DMA ring 长度、静音建立时间和声学 pop 仍归 A3-06/A4-13 实测。
12. shared-TX 故障路径没有物理 fail-safe：I2S disable、零预装或 re-enable 失败时，代码不会调用 codec mute/PA off；V2 的 `speaker->muted` 也不代表 GPIO4 已拉低。尤其 re-enable 失败分支注释称 PA “remains muted”，与 shared V2 实际 PA 仍开启不一致。设备可能处于 TX disabled/不确定而 PA 仍上电的状态，登记 `AUDIO-STOP-001`（P1），并交叉归 A3-05/A3-06/A4-13 关闭。
13. A3-04 产品验收不通过。静态上只证明主从/时钟预算、自定义 PA 顺序的设计方向、软件音量/reference同源和成功停播的零 DMA 策略；错误传播、状态回滚、故障物理静音、完整寄存器依据、pop/声压/波形均未闭合。

## 12. A3-04证据基线

| 资料/源码 | SHA-256 | 本项使用内容 |
|---|---|---|
| `NuwaxAIPartner测试工程/docs/最新硬件原理图/C962342_音频接口芯片_ES8311_规格书_WJ221363.PDF` | `69cc689b6afbd586ecfa97ff8a84566116814beb0f3027f547c2a079714156e4` | 12页完整视觉复核；功能、slave时钟、I2S边沿、差分输出和电气边界 |
| `managed_components/espressif__esp_codec_dev/device/es8311/es8311.c` | `89bea26707abf7313806c2fc15299a22bcd96d8072d83794ff0e2dd2afd20dbe` | 时钟系数、open/start/suspend、mute、PA、volume、set_fs和状态机 |
| `managed_components/espressif__esp_codec_dev/esp_codec_dev.c` | `b7a17e2ad412f4c08e0e9324aea0f217a49305510fd80e7ef3420fb31d7fd2ce` | 上层open/close对codec错误的处理边界 |
| `managed_components/espressif__esp_codec_dev/esp_codec_dev_if.c` | `f186e338082f101513ea058b9283697ba33602bbf55f133640c8fa0746880d21` | codec interface close后无条件free语义 |
| `main/board/vocat_audio_board.c` | `fc547edbee8becf89c31c8528c1299b66aa2cc89d065b8231e4f9100daec7e23` | I2S配置、DAC-only、默认volume、PA配置、init/deinit和诊断路径 |
| `main/board/vocat_audio_board.h` | `ecf4a3062aa60df536f4f92253b37ad09022dd8d52707e3f8615f07f0a696015` | 16 kHz产品采样率和板级音频契约 |
| `main/board/audio_board.c` | `e10ec96f70d91014b437533d50bd012e0f3e988860f5e823887e4ada92c40b02` | shared TX/RX所有权和板级refcount |
| `main/speaker_output.c` | `a5c4bda7c6bafda13e04528699989d2fc22d572dda74b7f9d857dc41c8fcaa3f` | 软件音量、I2S写、零DMA停播、故障状态和deinit |
| `main/conversation/speaker_playback.c` | `20cdbfc1422fc398625d28b98fb462d1b4cb0f4b3b3baa364e0a81b8c9536420` | logical volume同步、lazy init、idle stop和播放owner；A3-04完成后持续发生并行漂移，已按2026-08-03 21:12:28当前文件复核stop-fault调用链 |
| `sdkconfig` | `2ae83cfdb5fe0bcd079d27fc1f787730c0de0e445f8a1b2c8f53d698e5a8efb9` | 16 kHz、默认用户音量和V2 shared audio配置；A3-04完成后发生并行漂移，相关配置值未变 |
| `NuwaxAIPartner测试工程/cyberkit-main/main/audio/codecs/box_audio_codec.cc` | `1d1c94a4214a611c2d5c03b9efbb8f714e566f15694028c235d5968e14a7074d` | 冻结商家V2活动树的DAC/ES7210组合、24 kHz/I2S和volume/enable调用方式对照 |

PDF 的物理页数由本轮 `pdfinfo` 和 12 张逐页渲染图共同确认。该文件页脚页码为 1~12；旧硬件证据清单沿用的“9页”已同步纠正。渲染只用于临时视觉核验，不作为交付产物。

冻结商家V2活动树未包含 `managed_components/.../es8311.c` 源文件，只有其 `box_audio_codec.cc` 调用层。因此只能证明商家同样选择DAC-only、外部MCLK、固定5.0/3.3 V gain模型和`esp_codec_dev` API；不能把根目录V1.2/AECVERIFY派生树中的ES8311依赖实现当作商家V2错误传播正确性的证据。

## 13. ES8311时钟、格式和产品角色

| 项目 | 当前值 | 静态证据 | A级判断 |
|---|---:|---|---|
| 产品角色 | DAC-only | `codec_mode=DAC`；输入由ES7210四槽TDM提供 | ES8311 ADC未用于产品主链，不能宣称已验证 |
| 时钟角色 | slave | `master_mode=false`，ESP32-S3 I2S0为master | 与规格书slave能力一致 |
| sample rate | 16,000 Hz | 板级常量、I2S TX和codec `set_fs`一致 | 静态一致 |
| sample width | 16 bit | TX数据16 bit、codec请求16 bit | 静态一致；寄存器写成功未保证 |
| TX frame | 2 x 32-bit slot | 标准Philips、stereo both、slot width 32 | BCLK=64 Fs |
| MCLK | 4.096 MHz | `mclk_multiple=256` | 系数表存在对应项；需实测 |
| BCLK | 1.024 MHz | `16000*2*32` | `MCLK/BCLK=4`，与系数表静态一致 |
| LRCK | 16 kHz | 每帧一次 | 与slave外部同步要求一致 |
| PCM路由 | mono复制到L/R | `speaker_output`把每个样本写入两个slot | 能向mono DAC提供两侧相同数据；实际选路需寄存器指南/波形确认 |

规格书说明 DSDIN 在 SCLK 上升沿采样，当前代码未反相 BCLK并使用IDF Philips格式。静态配置意图匹配常规I2S边沿，但只有逻辑分析仪同时观察 BCLK/LRCK/DOUT 和模拟 OUTP/N 才能证明建立/保持时间、通道边界和真实采样无误。

## 14. 初始化、播放、停播与销毁链

```text
audio board first init
  -> create shared I2S0 TX/RX as ESP master
  -> enable TX first and provide MCLK/BCLK/LRCK
     (no explicit full zero-ring preload/readback at this point)
  -> initialize ES7210
  -> ES8311 new/open
       -> setup GPIO4 and request PA low
       -> write initial registers
  -> set_fs(16 kHz, 16 bit, 2 ch)
  -> set codec base volume about -6 dB
  -> enable(true)
       -> start register sequence
       -> PA high
       -> codec unmute
  -> board calls unmute once more
  -> publish shared TX/RX

normal TTS
  -> logical 0..100% linear PCM scaling
  -> duplicate mono sample into L/R
  -> bounded I2S writes
  -> mirror only completed physical stereo frames to software reference

normal stop
  -> disable TX
  -> preload complete DMA descriptor ring with zeros
  -> re-enable TX
  -> advance reference silence and wait settle
  -> keep ES8311 and PA enabled

final board teardown
  -> codec close: mute -> PA low -> suspend
  -> delete codec interface
  -> delete I2C/GPIO interfaces
  -> disable/delete I2S channels
```

成功路径顺序的设计方向是合理的：先启时钟，再开 PA；正常轮间保持 codec/PA 和时钟，使用零 DMA 实现静音，避免频繁电源切换。但冷启动开PA前没有显式完整零ring证明，失败路径也没有同等可靠性：依赖驱动吞错可使“成功”并非硬件成功，而 I2S hard failure 没有强制 GPIO4 low 的最后防线。

## 15. 错误传播与状态一致性矩阵

| 位置 | 当前行为 | 可见后果 | 归属 |
|---|---|---|---|
| `set_fs()` | NULL fs可解引用；非法bits静默按16 bit；忽略bits/fmt/sample三个返回值，正常请求始终OK | 崩溃或板级继续volume/enable/PA | `CODEC-ERR-001` |
| `config_sample()` | read失败后继续RMW；末次read覆盖此前错误 | 部分时钟寄存器失败可被静默 | `CODEC-ERR-001` |
| `open()` | reset read覆盖前段事务错误；PA setup/low无返回 | 初始化失败可被报告成功，PA默认态不可证明 | `CODEC-ERR-001` |
| `set_mute()` | read失败仍write；write返回被丢弃 | mute/unmute可报告成功但硬件未变化 | `CODEC-ERR-001` |
| `start()` | 早期write错误被read覆盖；失败也add_pair | 软件pair与真实硬件状态分离 | `CODEC-ERR-001`、`CODEC-STATE-001` |
| `enable(true)` | start失败仍PA high并unmute | codec配置失败时可能放大不确定输出 | `CODEC-STATE-001` |
| `enable(false)` | mute/PA结果忽略；pair先移除 | 返回值和enabled/pair/PA可能互相矛盾 | `CODEC-STATE-001` |
| `close()/delete` | close总是OK，delete无条件free | 物理静音/挂起失败无法观测或重试 | `CODEC-STATE-001` |
| shared TX stop error | 不调用codec mute或PA low | TX异常时没有物理quiet兜底 | `AUDIO-STOP-001` |

这些问题与 `I2C-REC-001` 相互放大但不能合并：总线恢复解决“总线如何恢复”，本节问题解决“单次事务失败后驱动是否准确传播、回滚并保持可重试状态”。两类修复缺一不可。

## 16. 音量、静音和pop边界

- 逻辑用户音量为 PCM 线性幅度百分比，而不是 dB 或感知响度曲线。它可正确产生 0% 的数字零和 100% 的原样PCM，但“50%听感一半”并不成立；产品体验标定应另行冻结。
- software reference 使用缩放后且实际写出的PCM，避免把未写成功尾部或缩放前数据送入AEC。这一静态正向结论不证明真实扬声器路径的DAC/PA增益和声学延迟已建模。
- 板级默认向ES8311请求 `-6 dB`，但驱动会先扣除错误的hardware gain模型；该请求不等于实际寄存器dB，OUTP/N幅度、PA输出和声压需绑定VOUT/反馈网络测量。
- 正常stop的零DMA策略不是 codec mute，也不是 PA physical off。成功时应安静但PA仍有底噪/模拟输出风险；失败时更没有物理静音保证。
- ES8311只声明支持pop/click suppression，不给当前寄存器与延时要求。必须以 I2S、GPIO4、OUTP/N、PA输出和扬声器端同步波形/录音验收冷启动、首帧、正常END、cancel、I2S故障和deinit。

## 17. A3-04 Canonical问题

| ID | 等级 | 真实性与影响 | 建议处理 |
|---|---|---|---|
| `CODEC-ERR-001` | P1 | ES8311多处覆盖/丢弃I2C和GPIO错误、read失败后继续RMW；`set_fs`不判NULL、非法bits静默降级且正常请求固定成功。板级错误检查因此不能证明硬件配置成功 | 所有read失败立即返回且0次write；逐步fail-fast并保留first error/寄存器/阶段；`set_fs`检查NULL/位宽/rate和每个子步骤；PA helper返回错误。对每个事务点做NACK/timeout注入 |
| `CODEC-STATE-001` | P1 | start失败仍add pair、开PA、unmute；enabled保持false使disable可no-op。disable/close状态非事务化，delete后硬件失败不可重试 | 改为显式`CLOSED/OPENED/STARTING/ENABLED/STOPPING/FAULT_SAFE`状态；成功后才commit pair/enabled；失败执行mute+PA low+suspend回滚并返回首错；close失败保留对象及依赖，禁止无条件free |
| `AUDIO-STOP-001` | P1 | shared V2 I2S stop失败时PA仍高，代码没有codec mute/PA low fallback；注释错误声称PA muted | 增加audio-board owner的有界physical-quiet API；任何disable/preload/re-enable失败立即codec mute并验证GPIO4 low，锁住新播放直至受控恢复；用故障注入和PA/扬声器波形验收deadline |
| `PA-GAIN-001` | P1 | 固定5 V/0 dB PA模型不符合VOUT与反馈网络 | 冻结电气模型并统一UI音量、DAC寄存器、PCM缩放、MIC3和声压标定 |
| `I2C-REC-001` | P1 | shared bus无物理恢复和device epoch重绑 | 与codec fail-fast配套实现唯一owner恢复；不能用重试掩盖SDA/SCL卡死 |
| `HW-EVID-001` | P1 | 无BOM/netlist/PnP和绑定样机证据；ES8311资料缺programming guide | 取得完整寄存器/时序资料和供应商签核，绑定实装与样机/镜像证据 |

## 18. A3-04 C/D关闭矩阵

| 编号 | 验证 | 方法 | 通过条件 |
|---|---|---|---|
| A3-04-C1 | set_fs事务注入 | 对bits/fmt/sample的每次read/write逐点注入NACK/timeout | 首个失败准确返回；失败read后0次write；板级不得继续enable/PA；状态可重试 |
| A3-04-C2 | enable/disable状态模型 | 对start、mute、PA、suspend和pair commit逐点故障注入 | 失败后PA low、无陈旧pair、状态为可解释fault-safe；disable/close可重试且不提前free |
| A3-04-C3 | 参数边界 | 覆盖NULL fs、非法bits、不支持rate、重复enable/disable/close | 无崩溃、无静默降级、返回稳定错误、硬件副作用符合状态机 |
| A3-04-D1 | 时钟/格式 | 逻辑分析仪测MCLK/BCLK/LRCK/DOUT并同时采OUTP/N | 4.096 MHz/1.024 MHz/16 kHz在容差内；Philips边沿、16-bit数据、L/R复制和模拟输出一致 |
| A3-04-D2 | 冷启和销毁pop | 同步采GPIO4、I2S、OUTP/N、PA输出和扬声器录音，冷启/反复deinit至少100次 | PA只在时钟/零数据稳定后开启；mute/PA/suspend顺序满足供应商要求；pop峰值低于冻结阈值 |
| A3-04-D3 | 音量曲线 | 0~100%多点、USB/电池/VOUT边界，测PCM、DAC、PA和SPL | 无削波/失真越界；软件ref与实际写PCM一致；产品音量/SPL曲线和MIC3标定可重复 |
| A3-04-D4 | 正常stop | END/cancel/idle反复执行，测完整DMA静音和模拟输出 | 无旧PCM重放；settle后OUTP/N/扬声器端达到冻结quiet阈值；下一轮首帧正常 |
| A3-04-D5 | stop故障物理静音 | 注入disable/preload/re-enable失败并观察GPIO4/扬声器 | 硬deadline内GPIO4 low或等效物理quiet；不继续对话/新播放；恢复后无陈旧音频和pop |

## 19. A3-04收口

- DAC-only产品角色、I2S slave和16 kHz/64-BCLK预算：通过 A 级静态审计；实波形开放。
- 初始化时先提供I2S时钟、后PA的设计方向：通过 A 级静态审计；开PA前没有显式完整零DMA ring证明，寄存器依据、稳定时间和pop开放。
- PCM软件音量与software reference同源：通过 A 级静态审计；真实硬件增益/SPL开放。
- ES8311错误传播和状态事务：不通过，`CODEC-ERR-001`、`CODEC-STATE-001`开放。
- shared-TX故障物理静音：不通过，`AUDIO-STOP-001`开放。
- 完整寄存器指南、BOM/实装、I2C恢复、PA增益和C/D测试：开放。
- 最终状态：`completed-A;C/D-open;product-not-accepted`。

全部审计结束后仍必须按最终source manifest重新核对本节每个文件和调用点，不能以本次哈希替代最终复核。

## 20. A3-05 NS4150B/PA_CTRL极性、mute、enable与掉电默认态审计结论

1. 原理图把 ESP32-S3 GPIO4 直接命名为 `PA_CTRL`，连接 NS4150B `CTRL`，并通过 R18 10 kOhm 下拉到 GND。NS4150B 规格书明确 `CTRL=H` 为 Open、`CTRL=L` 为 Shutdown，`VIH` 最小 1.2 V、`VIL` 最大 0.2 V。产品板级常量和商家 V2 配置均选择 GPIO4；ES8311 配置零初始化使 `pa_reverted=false`，因此当前软件高电平使能、低电平关闭与芯片和原理图一致。
2. 外部 10 kOhm 下拉使 ESP32-S3 复位、高阻和应用尚未接管期间的设计默认态为 PA shutdown。ES8311 `open()` 还会把 GPIO4 配成输出并请求低电平；SW3 关机路径也直接把 GPIO4 拉低。该结论只证明设计意图，不能代替 GPIO4 在上电、复位、brownout、ROM下载、deep sleep 和 VOUT 斜坡期间始终小于 `VIL=0.2 V` 的实测。
3. PA 模拟输入为 ES8311 `OUTP/OUTN` 经 C44/C45 100 nF、R19/R20 100 kOhm 进入 `INP/INN`；NS4150B 内部反馈 240 kOhm，静态闭环增益约 2.4 倍，即约 7.6 dB。PA VCC 接 VOUT，输出为差分 `VOP/VON` 直达扬声器连接器。该事实继续证明 `PA-GAIN-001`：固定 `pa_voltage=5.0 V`、`pa_gain=0` 不是当前硬件的可信统一模型。
4. 初始化先启 I2S TX，并在 ES7210 初始化前等待约 20 ms，因此 GPIO4 拉高前已存在 MCLK/BCLK/LRCK；随后 ES8311 `open()` 请求 PA low，`enable(true)` 执行 codec start、PA high、codec unmute。正向先时钟后 PA 的方向成立，但开 PA 前没有显式验证完整零 DMA ring，PA high 后也没有等待或状态门便允许播放链使用 shared TX。
5. NS4150B 规格书只给出启动时间典型 120 ms、关断时间典型 80 ms，没有保证的最大值，也没有状态反馈引脚。当前代码在 PA high 后没有显式 ready wait；真实网络启动通常可能给出偶然余量，但代码契约不能保证首帧一定落在 PA ready 之后，存在首音节衰减、截断或 pop 风险。登记 `PA-TIME-001`（P1）。
6. 正常每轮 TTS stop 不拉低 GPIO4，而是完整零预装 shared TX DMA ring并保持 codec/PA 工作。这是减少频繁启停 pop 的有意设计，但它是数字 quiet，不是 NS4150B shutdown，也不消除 PA 静态噪声。是否允许正常 stop 保持 PA high 必须由产品 quiet/SPL、功耗和 pop 指标裁决；不能把 `speaker->muted` 或“stop成功”解释为 GPIO4 已 low。
7. shared-TX stop 的 disable、preload或 re-enable 任一步失败时仍没有 codec mute/PA low 的物理兜底；该问题继续归 `AUDIO-STOP-001`。A3-05 进一步确认 GPIO4 low 是芯片支持且原理图可达的硬件 shutdown 手段，因此故障路径缺少 physical-quiet API 不是硬件能力限制，而是当前 owner/API 缺口。
8. ES8311 驱动 PA helper 为 `void` 并丢弃 GPIO setup/set 返回值；start 失败后仍可能开 PA，disable/close 又忽略 mute、PA和suspend失败。这使软件成功、`enabled`、pair、GPIO4和真实芯片状态可能不一致，继续归 `CODEC-ERR-001` 与 `CODEC-STATE-001`。
9. SW3 关机路径调用一个返回 `void` 的 `force_output_level(GPIO4, 0)`；即便 setup/set 失败，只记录日志，仍在固定 20 ms 后释放 PG2并进入 deep sleep。20 ms 小于规格书的典型 80 ms 关断时间，且没有供应商签核的最坏时序、PA状态反馈或扬声器 quiet 判据；因此不能证明 VOUT 消失前 PA 已完成静音。该关机缺口同时归 `PA-TIME-001` 和既有系统级 `POWER-001`。
10. 最终 teardown 依赖 ES8311 close 执行 mute、PA low、suspend，随后立即删除 codec/I2C/GPIO 和 I2S channel，没有 NS4150B shutdown完成门。由于 close 又吞掉这些错误，deinit 返回成功不能证明 GPIO4 low或扬声器已静音。
11. NS4150B 自带上/掉电噪声抑制、欠压、过流和过热保护，但规格书没有给出本板输入网络、VOUT斜坡、codec时序和扬声器负载下的 pop 保证。这些器件能力不能替代产品级启动、normal stop、cancel、I2S故障、SW3关机、brownout和反复复位波形验收。
12. A3-05 产品验收不通过。极性、GPIO owner和外部默认下拉的静态设计通过；启停完成时序、错误传播、故障物理静音、关机掉电前 quiet、真实增益/功耗/pop 以及绑定样机波形均未闭合。

## 21. A3-05证据基线

| 资料/源码 | SHA-256 | 本项使用内容 |
|---|---|---|
| `NuwaxAIPartner测试工程/docs/最新硬件原理图/3591608336SCH_主板二_2026-05-09.pdf` | `76f52901511cc2501fbf0af8bf31ff9b00435562cea01bcb39c21d6f9b706d7a` | GPIO4/PA_CTRL、R18下拉、NS4150B输入/反馈/旁路/供电/输出连接 |
| `NuwaxAIPartner测试工程/docs/最新硬件原理图/3591576609SCH_主板一_2026-05-09.pdf` | `bb5dbe5dd050e4058484ee11cf6fb16efbb11b1c7f77b9e1bc8c9ecda176c21b` | VOUT、PG2电源保持、MCU_3V3和CODEC_3V3供电拓扑 |
| `NuwaxAIPartner测试工程/docs/最新硬件原理图/C189961_音频功率放大器_NS4150B_规格书_音频功率放大器_NS4150B_中文规格书.PDF` | `8f26ce641fd0ead96cc949a23e7ebc8d3d51d066ed7de7f958ac6327a9fbb7c5` | 9页完整视觉复核；CTRL极性/阈值、启动/关断典型时间、shutdown电流、反馈增益及保护能力 |
| `main/board/esp_vocat_v1_0_pins.h` | `8f76a1d76e28f9c98308983165d2f71e37810f63caeacc6fae4efd3fcfe4a644` | GPIO4板级唯一PA映射 |
| `main/board/vocat_audio_board.c` | `fc547edbee8becf89c31c8528c1299b66aa2cc89d065b8231e4f9100daec7e23` | I2S/codec/PA初始化、诊断直接PA控制和teardown |
| `managed_components/espressif__esp_codec_dev/device/es8311/es8311.c` | `89bea26707abf7313806c2fc15299a22bcd96d8072d83794ff0e2dd2afd20dbe` | PA setup/enable/disable极性、错误传播和状态顺序 |
| `managed_components/espressif__esp_codec_dev/platform/audio_codec_gpio.c` | `2c87ffb6b65ce58f842223cdbc53e4b3c4138476617892afb532d3faccb8926c` | GPIO setup为输出float、set/get语义 |
| `main/speaker_output.c` | `a5c4bda7c6bafda13e04528699989d2fc22d572dda74b7f9d857dc41c8fcaa3f` | shared-TX normal stop、故障路径和physical mute边界 |
| `main/board/nuwax_power_ctrl.c` | `7d54c54cc7e6385cea8e816f40a6ef2e4cfa0a8f6a099ee3cb42b43286793824` | SW3强制PA low、20 ms quiet、释放PG2和错误处理 |
| `sdkconfig` | `2ae83cfdb5fe0bcd079d27fc1f787730c0de0e445f8a1b2c8f53d698e5a8efb9` | V2 profile、GPIO4、16 kHz、shared DMA；并行开发快照 |
| `NuwaxAIPartner测试工程/cyberkit-main/main/boards/CyberVoc-Board-V2_0/config.h` | `0a48a4fa5ae5e7746f6aaf9d3d21d6c9ff5c2eeb65e08ba19ed690cba6f5ae65` | 商家V2同样映射PA为GPIO4 |
| `NuwaxAIPartner测试工程/cyberkit-main/main/audio/codecs/box_audio_codec.cc` | `1d1c94a4214a611c2d5c03b9efbb8f714e566f15694028c235d5968e14a7074d` | 商家V2 DAC-only、默认非反相PA配置和同一codec API |

NS4150B PDF 实际为9个物理页，已通过 `pdfinfo` 和9张逐页渲染图完整复核。规格书页3的启动/关断时间均为典型值；本文没有把它们误写为芯片保证的硬性最大时间。

## 22. PA状态与owner矩阵

| 场景 | GPIO4/PA预期 | 当前代码 | A级判断 |
|---|---|---|---|
| MCU复位/高阻 | 外部R18下拉，`CTRL<0.2 V` | 应用尚未接管 | 设计为shutdown；需复位/斜坡实测 |
| ES8311 open | low | setup output-float后set low，返回值丢失 | 意图正确，成功不可证明 |
| codec enable | high | start后set high再unmute | 极性正确；无PA ready门，失败状态非事务 |
| normal TTS stop | high | DMA ring置零、PA保持工作 | 数字quiet，不是物理shutdown |
| stop hard failure | 可能high | 无codec mute/PA low兜底 | 不通过，`AUDIO-STOP-001` |
| codec disable/close | low | mute、PA low、suspend，但错误被吞 | 顺序方向正确，完成不可证明 |
| SW3关机 | low后释放PG2 | GPIO错误只记日志，固定20 ms后掉电 | 不通过，`PA-TIME-001`/`POWER-001` |
| brownout/异常reset | 依赖外部下拉和电源斜坡 | 无运行时协调 | 需绑定样机波形，`HW-ELEC-001` |

`CONFIG_NUWAX_SPEAKER_SD_GPIO=4` 仍存在于统一配置，但 V2 shared-TX 正常分支由 ES8311 codec owner控制GPIO4；generic discrete-speaker GPIO路径在 `vocat_shared_tx=true` 时不执行。当前未发现两个运行时owner同时驱动GPIO4，后续配置重构仍应增加编译期互斥断言，防止 profile 漂移。

## 23. A3-05 Canonical问题

| ID | 等级 | 真实性与影响 | 建议处理 |
|---|---|---|---|
| `PA-TIME-001` | P1 | NS4150B启动/关断典型120/80 ms且无ready反馈；当前PA high后无ready门，deinit无shutdown完成门，SW3仅等20 ms且GPIO失败仍掉电。首音节、pop和掉电前物理静音均无确定保证 | 由audio-board唯一owner实现显式`OFF/STARTING/ON/STOPPING/FAULT_SAFE`状态和有界deadline；首PCM只在供应商签核/测量得到的ready条件后提交；关机必须检查GPIO low并在quiet条件满足后才释放PG2，否则进入明确force-safe分支。绑定GPIO4、OUTP/N、VOP/N、扬声器和VOUT波形验收 |
| `AUDIO-STOP-001` | P1 | shared-TX hard failure没有GPIO4 low物理兜底 | 将有界physical-quiet API接入disable/preload/re-enable任一错误，并阻止新播放直至恢复 |
| `CODEC-ERR-001` | P1 | PA GPIO setup/set、codec mute错误被吞，软件成功不能证明硬件状态 | PA helper返回首错并readback；readback只证明MCU pad逻辑，仍需故障注入和物理波形 |
| `CODEC-STATE-001` | P1 | start失败仍可开PA，disable/close非事务化 | 由显式状态机在成功后commit；失败保持PA low且对象可重试 |
| `POWER-001` | P1 | SW3关机不协调音频owner且强制PA low失败后仍继续切电 | 系统quiesce调用同一PA owner的physical-quiet，并给每步deadline和force-safe语义 |
| `PA-GAIN-001` | P1 | VOUT/7.6 dB实链与固定5 V/0 dB模型不一致 | 冻结VOUT、扬声器和实装值，统一DAC/PCM/SPL/reference标定 |
| `HW-ELEC-001` | P1 | 无GPIO4/VOUT/PA电流、温升、brownout和pop绑定波形 | 按样机ID、镜像hash、电源条件和探头点保存原始波形与判据 |
| `HW-EVID-001` | P1 | 无BOM/netlist/PnP，原理图不能证明R18、R19/R20、NS4150B和扬声器实装 | 完成实装/铜线/料号核验并绑定证据 |

## 24. A3-05 D级关闭矩阵

| 编号 | 验证 | 方法 | 通过条件 |
|---|---|---|---|
| A3-05-D1 | 复位默认态/极性 | 同步测GPIO4、VOUT、MCU_3V3，覆盖电池/USB冷启、EN reset、brownout、ROM下载、deep sleep和100次复位 | 非意图播放阶段GPIO4始终低于0.2 V；高电平稳定高于1.2 V且不超过芯片绝对边界；无窄高脉冲误使能 |
| A3-05-D2 | 启动ready与首帧 | 同步测GPIO4、I2S、ES8311 OUTP/N、PA VOP/N和扬声器录音，覆盖冷启与lazy init | 首个非零PCM只在PA稳定ready后提交；第一音节不衰减/截断；pop峰值低于冻结阈值 |
| A3-05-D3 | normal stop/cancel | END、cancel、barge-in和idle反复执行，测PA保持high时的模拟quiet、功耗和下一轮首帧 | quiet/SPL/功耗满足冻结产品政策，无陈旧PCM、底噪越限或pop；政策明确数字quiet是否可替代物理shutdown |
| A3-05-D4 | SW3/deinit关断 | 同步测GPIO4、OUTP/N、VOP/N、扬声器、VOUT和PG2，覆盖播放中/静音/满音量关机 | 扬声器达到冻结quiet阈值后才释放PG2；无pop、余音或rail反灌；时序有供应商签核或最坏实测依据 |
| A3-05-D5 | 故障物理静音 | 注入I2C mute/PA GPIO setup/set/readback、I2S disable/preload/re-enable和关机owner超时 | 任一错误在硬deadline内进入GPIO4 low或等效物理quiet；不继续播放/对话；错误可观测且恢复不重放旧音频 |
| A3-05-D6 | 供电/负载边界 | 多音量、电池低/高、USB切换、实际扬声器阻抗下测电流、温升、THD+N、削波和保护恢复 | 不超NS4150B/扬声器/电源边界；保护触发可观测且不会形成重启循环或不受控重播 |

## 25. A3-05收口

- GPIO4/PA_CTRL映射、active-high极性和R18外部下拉默认态：通过 A 级设计审计；复位/斜坡波形开放。
- 先I2S时钟后PA的基本顺序：通过 A 级静态审计；完整零ring和PA ready门未建立。
- normal stop：是保持PA high的数字DMA quiet，不是物理shutdown；是否满足产品政策开放。
- enable/disable/deinit/SW3关机的错误传播、状态事务和完成时序：不通过，`PA-TIME-001`、`CODEC-ERR-001`、`CODEC-STATE-001`、`POWER-001`开放。
- shared-TX故障物理静音：不通过，`AUDIO-STOP-001`开放。
- 增益、实装、电源、pop/功耗/温升和全部D级波形：开放。
- 最终状态：`completed-A;D-open;product-not-accepted`。

下一项为A3-06 I2S/DMA时钟、slot、buffer、共享TX/RX与stall恢复。由于其他工程师仍在并行修改源码，全部审计完成后必须按最终source manifest重做本节所有哈希、调用点和结论复核。

## 26. A3-06 I2S/DMA时钟、slot、buffer、共享TX/RX与stall恢复审计结论

1. 当前有效配置为 ESP32-S3、ESP-IDF v6.0、16 kHz。板级只创建一个 I2S0 controller pair：TX 使用 standard Philips、16-bit data、双 32-bit slot；RX 使用 TDM Philips、16-bit data、四个 16-bit slot。两方向均为 64 bit/frame，因此静态预算一致：MCLK=`16000*256=4.096 MHz`，BCLK=`16000*64=1.024 MHz`。这证明配置算术相容，不证明引脚上的占空比、边沿、抖动和停启窗口已经实测通过。
2. ESP-IDF `i2s_new_channel(..., &tx, &rx)` 在创建时即把 controller 标记为full-duplex。项目先初始化master TX，再初始化RX；IDF会把后初始化的RX改为共享时钟slave并设置`full_duplex_slave=true`。ESP32-S3的MCLK绑定到master TX clock，BCLK/WS由full-duplex硬件共享。该行为已绑定本机IDF commit `662a3be354759d9487bf4b1a629fadb766cb1800`，不能无条件外推到其他IDF版本。
3. `i2s_channel_disable(tx)` 会停止TX serial/DMA并清TX queue，但当前IDF路径没有显式执行TX clock-disable；项目随后完整预装零DMA ring再re-enable。这是保留共享RX的正向设计。然而MCLK绑定TX，且芯片级TX stop/re-enable期间BCLK/WS/MCLK是否无毛刺连续、RX DMA是否丢样或重相位，仍只能由同步波形和连续序号/相关性实测关闭，静态代码不能宣称无影响。
4. TX/RX都配置`6 x 240 frames`，每方向时间容量约90 ms。TX每descriptor 960 bytes、总5760 bytes；RX每descriptor 1920 bytes、总11520 bytes。板级RX工作块为240个四槽frame（15 ms），诊断TX块为320个stereo frame（20 ms）；正式speaker将较大PCM请求按240 frame切块并以单一整体deadline计算剩余时间。最大MIC请求960 samples即60 ms，小于90 ms DMA时间容量，但这不构成调度阻塞、flash/cache或owner冲突下不会overflow的证明。
5. TX设置`auto_clear_after_cb=true`，在应用来不及提交新PCM时由驱动清零已发送descriptor，可降低旧音频重复风险。正式speaker只把已完成物理写出的完整stereo frame写入software reference，且短写、非整frame和整体长度不符均返回错误；这是本项明确的正向控制。
6. IDF RX ISR在message queue满时会先移除最旧DMA buffer，再放入最新buffer；只有注册`on_recv_q_ovf`才会通知应用。项目没有对RX或TX注册I2S event callback，也没有底层drop counter。因此底层已丢的物理frame不会反映到读出后才生成的会议/监控/uplink sequence，现有“wire sequence连续”不能证明采集时间线连续。登记`I2S-OBS-001`（P1）。
7. `mic_input`允许“`ESP_ERR_TIMEOUT`但`bytes_read>0`”作为可用，并在shared TDM路径直接用`bytes_read/8`取整；没有验证`bytes_read%8==0`、请求是否短读、被截断尾字节、连续partial-read或底层overflow计数。常规descriptor和请求长度均按完整frame对齐，故本次不宣称已发生半frame；该接口完整性缺口作为`I2S-OBS-001`的子项，不重复登记独立编号。
8. MIC read timeout为500 ms。会议和监控都要连续100次零样本才以`mic_read_timeout`停止，最坏约50 s；任一短读又会把计数清零。该策略不满足工作项“stall在时限内恢复”的验收，而且只停止当前模式，没有I2S disable/re-enable、driver/controller重建或有界重试恢复状态机。正常对话和wake的错误策略不同，不能用某一路的重启/退出兜底外推会议和监控长流可靠性。
9. 正常MIC路径同时使用`mic_input`全局owner和`audio_board` RX lease；speaker使用TX lease。会议/监控单任务顺序read/deinit，正常对话在deinit前停止并回收capture worker，worker停止超时会受控重启，未发现这些主路径直接并发read/deinit。
10. 云端`p3_ref_health`命令和手工AEC health路径只拒绝speaker pending及正常对话prelisten，未拒绝wake、meeting或monitoring owner。随后直接调用`nuwax_vocat_audio_board_ref_probe()`/`nuwax_vocat_aec_ref_health_check()`；板级诊断直接读共享`s_rx`、写共享`s_tx`并改变codec/PA，不经过`mic_input` owner、RX/TX lease或模式controller。该真实产品控制面可偷走合法owner的DMA数据、制造MIC timeout、破坏AEC对齐或与播放状态冲突，登记`AUDIO-OWN-001`（P1）。
11. `audio_board` RX/TX lease只是两个bool，release为无token、无generation、返回void；迟到或错误实例可以清除其他实例的live lease。final deinit把refcount先减到0，底层又在disable/delete前清`s_initialized`和全局TX/RX句柄；若disable/delete失败，句柄已丢失且refcount为0，无法对同一资源重试回收，下一次init可能在残留driver resource上重新创建。登记`AUDIO-LIFE-001`（P1），并与既有`API-001`交叉引用但不合并：`API-001`是MIC实例重复init破坏live metadata，本项是board lease/teardown代次与回收事务缺失。
12. A3-06产品验收不通过。时钟/slot算术、DMA容量、正式speaker分块deadline、partial TX/reference和零descriptor正向控制静态成立；共享时钟连续性、底层drop可观测性、会议/监控stall deadline、诊断owner互斥、lease generation和失败可重试teardown均未闭合。

## 27. A3-06证据基线

| 资料/源码 | SHA-256 | 本项使用内容 |
|---|---|---|
| `main/board/vocat_audio_board.c` | `fc547edbee8becf89c31c8528c1299b66aa2cc89d065b8231e4f9100daec7e23` | shared I2S0创建、STD/TDM配置、诊断直读写和teardown |
| `main/board/audio_board.c` | `e10ec96f70d91014b437533d50bd012e0f3e988860f5e823887e4ada92c40b02` | refcount、RX/TX bool lease及无token release |
| `main/mic_input.c` | `b00edefcfcf3698e475892bc83c612190932aa8b5626047fea3b32de0ca083bb` | 全局MIC owner、shared RX lease、timeout/partial-read/frame换算 |
| `main/speaker_output.c` | `a5c4bda7c6bafda13e04528699989d2fc22d572dda74b7f9d857dc41c8fcaa3f` | 240-frame分块、整体deadline、partial write/reference及shared stop |
| `main/conversation/speaker_playback.c` | `20cdbfc1422fc398625d28b98fb462d1b4cb0f4b3b3baa364e0a81b8c9536420` | speaker owner stop/cancel调用边界 |
| `main/nuwax_ai_partner_main.c` | `ebaec59ae656cf989da088356b5e46bf620cc0af8fc96c977e348586955a62d6` | 云端/手工AEC health入口和不完整的运行owner门禁 |
| `sdkconfig` | `ac1b2d80fb6a0769e401f2f06ef2f4a081e91d64e8b5bf20aef64159dc250cae` | 2026-08-03 21:31并行开发快照；ESP32-S3、16 kHz、6x240 DMA、500/1000 ms I/O timeout、100 Hz tick |
| ESP-IDF `components/esp_driver_i2s/i2s_common.c` | `28a8b87a590197f675c9e89421c1239475c3a1ab91d5e04df1b6c687b2aef633` | commit `662a3be...`；full-duplex创建、DMA overflow drop-oldest、callback、enable/disable/delete |
| ESP-IDF `components/esp_driver_i2s/i2s_std.c` | `0c740df87fa4fa7c029a98dce91c5ec98593e9dbd9c5d760d1a007248efd30ad` | STD 64-bit frame、MCLK绑定TX和共享BCLK/WS |
| ESP-IDF `components/esp_driver_i2s/i2s_tdm.c` | `2c81e8a377cc62ce6819eba538f7dc45619c6aaeb57ea71a9299a87f9edd5ca1` | TDM 4-slot clock计算及后初始化RX转slave |
| ESP-IDF `components/esp_hal_i2s/esp32s3/include/hal/i2s_ll.h` | `efb9da85bdb4f4df93d401e6c97b2661c1b413814070f26cdb4e5331ea65555a` | ESP32-S3 low-level clock/share/stop语义依据 |

本项IDF证据绑定本机`C:\esp\v6.0\esp-idf`的clean commit `662a3be354759d9487bf4b1a629fadb766cb1800`（`v6.0`）。若工具链切换、IDF子模块漂移或补丁改变，上述驱动推理必须重做。

## 28. I2S/DMA状态、owner与可观测性矩阵

| 场景 | 当前控制 | 静态判断 |
|---|---|---|
| TX/RX steady full-duplex | TX master、RX later-init slave，共享64-bit frame clocks | 算术通过；波形/相位实测开放 |
| 正式MIC采集 | global MIC owner + board RX lease | 主路径互斥成立；底层drop不可见 |
| 正式speaker播放 | board TX lease、240-frame chunk、整体deadline | 短写/reference边界较完整；underflow计数缺失 |
| normal shared stop | TX disable、完整90 ms零ring preload、re-enable | 防陈旧PCM正向；共享RX连续性和故障physical quiet开放 |
| RX queue overflow | IDF静默drop oldest，项目无callback/counter | 不通过，`I2S-OBS-001` |
| timeout partial read | 有字节即usable，按frame bytes整除 | 完整性未证明，归`I2S-OBS-001` |
| 会议/监控持续stall | 500 ms * 100次才停止，无driver重建 | 最坏约50 s，不通过 |
| 云端/手工health诊断 | 只查speaker pending/prelisten，直接操作板级TX/RX/codec | 绕过owner，不通过，`AUDIO-OWN-001` |
| final deinit失败 | 清发布状态/句柄后disable/delete，错误返回但无法重试同一句柄 | 不通过，`AUDIO-LIFE-001` |

## 29. A3-06 Canonical问题

| ID | 等级 | 真实性与影响 | 建议处理 |
|---|---|---|---|
| `AUDIO-OWN-001` | P1 | authenticated云端/手工AEC health入口可在wake、meeting或monitoring持有RX时直接读共享RX/写共享TX并改codec/PA，绕过MIC owner与board lease；会偷DMA数据、制造timeout或破坏AEC时间线 | 建立唯一audio command bus/owner token；诊断先以generation原子申请RX+TX+codec独占权，明确停止/拒绝所有活动mode和wake，完成后事务恢复；并发交错B/D测试覆盖每个owner与取消/断连 |
| `I2S-OBS-001` | P1 | IDF RX queue满会drop oldest，但项目未注册overflow callback/counter；上层sequence在读出后生成，不能发现物理缺口。partial timeout也未验证frame对齐或短读连续性 | 注册IRAM-safe最小callback只递增per-generation counter；owner读取时消费overflow/underflow/partial/stall telemetry并使当前连续流显式失败或重开，禁止在同session伪装连续；增加frame-byte断言和有界恢复状态机 |
| `AUDIO-LIFE-001` | P1 | board lease无token/generation，任意迟到release可清他人lease；teardown先丢句柄/归零refcount再disable/delete，失败后无法重试回收且可能与下次init冲突 | acquire返回不可伪造的lease token `{direction,generation,handle}`，release校验；采用`UP/QUIESCING/FAULTED/DOWN`事务状态，只有全部delete成功才清句柄/refcount，失败保留owner可重试或进入受控restart |
| `CACHE-001` | P1 | flash/NVS/cache-disabled活动与I2S实时链缺统一准入；当前I2S ISR/control也未配置IRAM-safe | 沿用既有问题：冻结cache-off预算，音频active时仲裁写flash/NVS/OTA并在C/D注入验证DMA水位、drop counter和恢复 |

## 30. A3-06 C/D关闭矩阵

| 编号 | 验证 | 方法 | 通过条件 |
|---|---|---|---|
| A3-06-C1 | profile/IDF构建契约 | 冻结sdkconfig、IDF commit并做clean configure/build/map检查 | 唯一I2S0 pair、DMA配置和能力位与本文一致，无旧generic channel进入镜像 |
| A3-06-D1 | 共享时钟连续性 | 同步采MCLK/BCLK/WS/DIN/DOUT，循环TX disable/preload/re-enable并保持RX单频/递增样本 | 时钟满足冻结容差；RX无丢frame、重复、乱序、相位跳变或channel swap |
| A3-06-D2 | RX overflow可观测性 | 人为阻塞reader超过90 ms并注入已知递增/PRBS四槽源 | 每次driver drop均有同generation counter；流在冻结deadline内显式失败/重开，绝不在同session生成伪连续sequence |
| A3-06-D3 | TX underflow/stall | 阻塞producer、注入partial write/disable/preload/re-enable错误并测DOUT/OUTP/N/扬声器 | 无旧PCM重放；drop/underflow可观测；失败在deadline内physical quiet并可受控恢复 |
| A3-06-D4 | 会议/监控MIC stall | 断BCLK/DIN、冻结reader、恢复时钟，覆盖armed/streaming/END | 在产品冻结deadline内停止或重建；stop reason准确，END/新session语义明确，不等待约50 s |
| A3-06-D5 | owner冲突 | 在wake、normal capture、meeting、monitoring、TTS各状态交错下发所有AEC诊断命令及取消/WS断连 | 非独占时fail-closed且不触碰DMA/codec；取得token后只有一个owner，恢复不丢原owner状态 |
| A3-06-D6 | lease/teardown代次 | 迟到release、双release、stale token、delete失败、重复init/deinit和1000次生命周期注入 | stale操作被拒；refcount/句柄/driver资源守恒；失败可重试或受控restart，无残留controller |
| A3-06-D7 | partial read/frame边界 | 注入0~请求长度的timeout/short read及非8-byte尾部 | 非完整TDM frame不上传；短读/尾部/连续partial有计数和确定stop/reassembly语义 |
| A3-06-D8 | cache/系统压力 | 音频全双工时执行经批准的Wi-Fi/TLS、UI、flash/NVS/OTA压力 | DMA drop为0或按政策显式恢复；CPU/heap/stack/latency满足冻结SLO |

## 31. A3-06收口

- 16 kHz、TX双32-bit slot、RX四16-bit slot和共同64 BCLK/Fs：A 级算术通过；物理时钟验收开放。
- 双向6x240 frame约90 ms DMA容量、speaker 240-frame分块与整体deadline：A 级静态通过；调度/flash压力余量开放。
- TX auto-clear、完整零ring stop、partial write仅记录完整physical reference：正向控制成立；不替代physical quiet和RX连续性实测。
- RX overflow/partial-read/stall可观测性：不通过，`I2S-OBS-001`开放。
- health诊断与运行owner互斥：不通过，`AUDIO-OWN-001`开放。
- board lease generation与失败可重试teardown：不通过，`AUDIO-LIFE-001`开放。
- 最终状态：`completed-A;C/D-open;product-not-accepted`。

下一项为A3-07 MIC1/MIC2/MIC3通道映射、方向、reference与AEC health gate。由于其他工程师仍在并行修改，全部审计完成后仍必须按最终source manifest重做本节配置、IDF、owner入口和调用链复核。
