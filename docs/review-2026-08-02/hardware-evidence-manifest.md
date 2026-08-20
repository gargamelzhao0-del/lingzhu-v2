# 新版硬件资料与商家参考基线

生成日期：2026-08-02

状态：B0-08 只读证据。未解析为新的电气结论、未构建、未操作设备。

## 1. 权威硬件目录

唯一权威目录：

`C:\Users\Administrator\Documents\work\Nuwax AI Partner\Nuwax AI Partner 7.0\NuwaxAIPartner测试工程\docs\最新硬件原理图\`

该目录恰有 8 个文件、7,902,121 bytes，无子目录或隐藏附加文件。

| 文件 | 大小 | 页数 | SHA-256 |
|---|---:|---:|---|
| `3588650892H0185Y078T078 V0规格书1.85寸360X360 圆 MIPI QSPI 8BIT SRGB.pdf` | 1,547,049 | 19 | `9ce8b50b4582968eeee667e52d7463e3b09f576c3a27c508c33fb7c91bcb3eab` |
| `3591524974SCH_麦克风_2026-05-09.pdf` | 103,986 | 1 | `cda061f4559268e5db7f6ab66fcab997f4ad170899b0e351d2ea933ad72e4273` |
| `3591576609SCH_主板一_2026-05-09.pdf` | 315,886 | 1 | `bb5dbe5dd050e4058484ee11cf6fb16efbb11b1c7f77b9e1bc8c9ecda176c21b` |
| `3591608336SCH_主板二_2026-05-09.pdf` | 561,681 | 3 | `76f52901511cc2501fbf0af8bf31ff9b00435562cea01bcb39c21d6f9b706d7a` |
| `C189961_音频功率放大器_NS4150B_规格书_音频功率放大器_NS4150B_中文规格书.PDF` | 3,341,583 | 9 | `8f26ce641fd0ead96cc949a23e7ebc8d3d51d066ed7de7f958ac6327a9fbb7c5` |
| `C365743_ADC-DAC-专用型_ES7210_规格书_WJ69534.PDF` | 1,058,784 | 12 | `909a13dcd8770835914ef0de0cb5a44aba6d4312daf1d99768e0dab65f0e87e6` |
| `C962342_音频接口芯片_ES8311_规格书_WJ221363.PDF` | 546,416 | 12 | `69cc689b6afbd586ecfa97ff8a84566116814beb0f3027f547c2a079714156e4` |
| `产品3D结构爆炸图.png` | 426,736 | N/A | `429059ce91ac5110fbc485d9c9b697b7bedddaf4885de05023a4c3fd12630382` |

页数最初引用现有审计 `NuwaxAIPartner测试工程/docs/2026-07-24-新版硬件原理图与硬件AEC代码深度审计.md`；文件数、大小和 hash 已在本批重新计算。ES8311 页数在 A3-04 通过 `pdfinfo` 和 12 页逐页渲染重新确认并纠正为 12；NS4150B 页数在 A3-05 通过 `pdfinfo` 和 9 页逐页渲染重新确认并纠正为 9。

## 2. 证据缺口

- 没有生产 BOM、netlist、PCB 装配/PnP 文件；无法由原理图单独证明样机贴装值、选料和铜线完全一致。
- 原理图包含 BQ27220YZFR 和 QMI8658A 的设计连接，但权威目录没有 BQ27220 官方 datasheet/TRM，也没有 QMI8658A 官方规格书/应用说明。
- BQ27220 商家驱动的 README/寄存器定义和 QMI8658A 商家代码属于二手实现依据，不能替代原厂资料。
- 缺少样机电芯对应的 BQ27220 CEDV/profile 认证资料，因此 UI 能显示电量不等同电量精度可信。
- 无 BOM/netlist 和实际量测前，MIC3 reference、PA 极性、电源保持、器件地址/贴装只能分别标记为设计证据或待实机确认。

## 3. 商家参考工程冻结

工作区有两套独立、非 junction、无 `.git` provenance 的 `cyberkit-main`：

| 路径 | 活动项目 | 关键 hash | 判定 |
|---|---|---|---|
| `NuwaxAIPartner测试工程/cyberkit-main` | `CyberVoc-Board-V2_0`，2.0.5.6，`CONFIG_BOARD_TYPE_CYBERVOC_V2_0=y` | CMake `2573a1c5f66e87a69c71be7129dce1583bbd69b82494c357aff84effd9c3cac2`；sdkconfig `3cf3a87ce125581c5c88dd1cb5a9a871ab7e81fa192f7c74a00e75481d00dd64` | 冻结为商家 V2.0 权威参考 |
| 根目录 `cyberkit-main` | `CyberVoc-Board-V1_2`，2.0.5.6，`CONFIG_BOARD_TYPE_CYBERVOC=y` | CMake `03adcbd616411762e774296b273cc432a3cbc0572d26547a6901b557e6db69ba`；sdkconfig `7157ffc765a18d48b07ed17c9bae51bb4a40ac98f97b18092e200ff7ea060500` | V1.2/AECVERIFY 派生实验工程，不作为 V2 基线 |

两树不是可互换副本：源码可见清单中 1,190 个同路径文件相同，10 个同路径文件不同，差异涉及 CMake/defaults/README 和 5 个核心音频文件；根目录另有 5 个 AECVERIFY 文件。两边 `main/boards/CyberVoc-Board-V2_0` 的 70 个文件逐项一致，但活动项目配置和共享音频实现不同。

测试工程中的 V2.0.5.6 V2 release zip 大小 5,553,590 bytes，SHA-256 `7d7758229b9c5cb3a04e88a005f82cab515fc2d0485ec8ce739e7e6c20f59dc0`。

## 4. 商家 V2 树 Manifest

`vendor-v2-reference-manifest.sha256` 从权威商家 V2 树文件系统生成，共 1,221 项，manifest SHA-256：

`0a2329d5cfcfb1cba8454bfacfd9a370a03bbd54526323d2344f7836b7996e80`

纳入源码、配置、文档和 releases；显式排除根 `build*`、`managed_components`、`.git`、`__pycache__`、`node_modules` 和 `.pyc`。任何参考树变化都会使该基线失效。由于没有 Git 历史，来源 provenance 仍是开放证据缺口。

## 5. 结论

B0-08 已冻结现有硬件资料与商家 V2 参考来源。它证明“本轮看的是哪份资料/代码”，不证明实物电路、贴装、硬件 AEC、电量精度或传感器功能已经通过。

## 6. A3-06 ESP-IDF I2S驱动证据快照

A3-06使用的实际工具链为`C:\esp\v6.0\esp-idf`，Git commit `662a3be354759d9487bf4b1a629fadb766cb1800`，describe为`v6.0`，审计时工作树无修改。关键文件如下：

| 文件 | SHA-256 | 证据用途 |
|---|---|---|
| `components/esp_driver_i2s/i2s_common.c` | `28a8b87a590197f675c9e89421c1239475c3a1ab91d5e04df1b6c687b2aef633` | full-duplex创建、DMA队列drop-oldest、callback及channel生命周期 |
| `components/esp_driver_i2s/i2s_std.c` | `0c740df87fa4fa7c029a98dce91c5ec98593e9dbd9c5d760d1a007248efd30ad` | standard时钟、共享BCLK/WS及MCLK绑定 |
| `components/esp_driver_i2s/i2s_tdm.c` | `2c81e8a377cc62ce6819eba538f7dc45619c6aaeb57ea71a9299a87f9edd5ca1` | TDM时钟和后初始化RX转slave |
| `components/esp_hal_i2s/esp32s3/include/hal/i2s_ll.h` | `efb9da85bdb4f4df93d401e6c97b2661c1b413814070f26cdb4e5331ea65555a` | ESP32-S3低层clock/share/stop依据 |

该快照只证明A3-06静态推理所依据的IDF版本；它不是硬件波形、DMA overflow恢复或产品长稳通过证据。工具链、commit或文件hash任一漂移均触发A3-06重审。
