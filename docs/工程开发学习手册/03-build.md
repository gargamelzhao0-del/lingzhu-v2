# 03 环境、配置、构建与烧录

<!-- technical-edition-20261009 -->

> 可靠构建的目标是得到可复现身份的产物：知道哪些配置生效、哪些资源参与、崩溃时该用哪一份 ELF。

**学习目标：** 从默认配置追到生成头文件和最终镜像，区分全量烧录与应用 OTA。

```zhizhan-map
默认配置和保存配置
编译与链接
镜像及资源核验
烧录后核对身份
```

## 3.1 一次构建到底做了什么

CMake 解析组件和配置，Kconfig 生成最终配置，编译器把 C 转为目标文件，链接器生成 ELF，工具再生成可烧录 BIN、分区表和烧录参数。ELF 保留符号和调试信息，BIN 用于烧录，两者不能混用。出现崩溃地址时，匹配 ELF 往往比重新编译一份“差不多的代码”更重要。

本工程还校验独立表情资源包。应用 BIN 并不包含全部模型、表情和分区初始化内容，因此“全量串口烧录”和“应用 OTA”不是同一交付物。

## 3.2 配置的三层来源

`Kconfig.projbuild` 定义可选项和基础默认值；`sdkconfig*.defaults` 选择产品配置；保存的 `sdkconfig` 是该构建环境实际采用的值。已有构建目录复用时，修改 defaults 不一定覆盖保存值。生成目录中的 `config/sdkconfig.h` 是检查产物配置的重要依据。

当前正式 helper 按以下顺序加载默认值：

```text
sdkconfig.defaults
sdkconfig.product-ota.defaults
sdkconfig.sd-replay-formal.defaults
```

它使用 `tmp/sdkconfig.sd-replay-formal`。不要把测试网关配置复制进正式配置后忘记关闭测试面板，也不要只凭 `menuconfig` 菜单截图证明交付 BIN 的配置。

## 3.3 本机当前可用的正式构建命令

以下命令在工程根目录执行，沿用本仓库现有 Windows 脚本，不是其他电脑无需安装即可使用的通用命令：

```powershell
$buildDir = 'C:/tmp/nuwax-sd-formal-build-20260921'
& ./tools/build-sd-replay-formal.ps1 -BuildDir $buildDir
```

脚本会调用配网依赖修复工具、加载 `C:/esp/v6.0/esp-idf/export.ps1`、运行 IDF 6.0 Python 环境中的 `idf.py`，随后执行构建产物验证。**它可能修补受管理依赖，不能当作纯只读检查。** 换电脑先检查 SDK/Python 安装位置，再统一调整脚本或在受控环境中配置路径。

教程不建议用一次 `fullclean` 解决所有问题：它可能只是隐藏配置复用问题，还会丢失有用证据。先保存错误日志、CMakeCache、配置和版本，再决定是否建立新的构建目录。

## 3.4 烧录前验证，再明确选择串口

构建脚本和正式烧录脚本的默认 BuildDir **目前不同**，前者含 `formal-build`，后者为 `formal`。这是已观察到的工具一致性风险，因此必须显式传入同一个目录：

```powershell
$buildDir = 'C:/tmp/nuwax-sd-formal-build-20260921'
# COM7 仅为示例，换成设备实际端口。
& ./tools/flash-sd-replay-formal.ps1 -Port COM7 -BuildDir $buildDir -VerifyOnly
# 确认端口、配置和产物后，才执行实际烧录：
& ./tools/flash-sd-replay-formal.ps1 -Port COM7 -BuildDir $buildDir
```

烧录包装器检查 `flasher_args.json`、配置与镜像身份，并依 manifest 烧录所需文件。不要手抄旧版本的 Flash 地址，不要为了连不上串口直接擦除 NVS。先检查串口占用、USB 线、供电、下载模式和设备管理器。

`VerifyOnly` 不等于设备已运行新版本。烧录后读取启动版本和 ELF SHA，并核对产品配置日志。若只是 OTA，交付的是应用 BIN 与对应 metadata，而非把全量烧录命令改成某个固定地址。

## 3.5 版本、分区与资源身份

根 CMake 要求版本格式 `7.0.N`，release sequence 为 `7000000 + N`。当前默认版本为 7.0.113，序列是 7000113。不要把旧 OTA README 中的演示版本格式直接用于本轮构建。

当前分区布局如下，容量单位均按二进制地址计算：

| 分区 | 起始地址 | 大小 | 用途 |
| --- | --- | --- | --- |
| nvs | 0x9000 | 0x6000 | 持久化配置 |
| otadata | 0xF000 | 0x2000 | OTA 启动选择 |
| phy_init | 0x11000 | 0x1000 | PHY 初始化数据 |
| model | 0x20000 | 0x2A0000 | 识别模型 |
| ota_0 | 0x2C0000 | 0x420000 | 应用槽 A |
| ota_1 | 0x6E0000 | 0x420000 | 应用槽 B |
| emotion_0 | 0xB00000 | 0x400000 | 表情资源 |
| storage | 0xF00000 | 0x100000 | 存储分区 |

来源：[partitions.customwake.csv](../../partitions.customwake.csv)。地址表用来理解布局，不取代烧录 manifest。应用最大槽位约 4.125 MiB，不能因为芯片还有空间就忽略单槽限制。

## 3.6 发布记录应该保留什么

至少保留：源码摘要/提交与工作树差异、依赖锁、实际配置、工具链版本、完整构建日志、ELF、应用 BIN、分区表、烧录参数、资源 SHA、测试结果与实机启动 SHA。若这些不匹配，崩溃定位和版本回退都会产生歧义。

当前版本的改动、产物摘要和验证边界见 [7.0.113 发布说明](../../artifacts/release-7.0.113/README.md)。其中“尚未上传云端或烧录设备”描述的是该发布记录形成时的状态，不能据此判断用户现在运行的版本。本次教材修订没有重新构建固件或认证其实机行为。后续代码改变后，不能继续沿用旧测试结果作为新版本通过的证据。

知栈收录的是可阅读的发布说明和源码附件，不是整个固件发布目录。需要 BIN、ELF 或完整构建记录时，应回到原工程对应发布目录；手机里能打开一篇发布说明不表示已经具备烧录产物。

## 3.7 常见错误的定位顺序

“找不到 SDK”：先确认 PowerShell 是否已加载正确 export；“配置没变化”：检查保存的 sdkconfig 和生成头文件；“烧录后行为不对”：先查 build directory 和 SHA；“OTA 镜像无效”：检查项目身份、芯片、版本序列、大小和完整性；“屏幕资源错乱”：核对 emotion 分区资源，不要只重刷应用。

阅读入口：[正式构建 helper](../../tools/build-sd-replay-formal.ps1)、[正式烧录 wrapper](../../tools/flash-sd-replay-formal.ps1)、[实际烧录验证逻辑](../../tools/flash-sd-replay-test.ps1)。



## 技术要点与自测

**设计要点：** 显式使用同一 BuildDir，保留配置与 ELF。修改 defaults 后要检查保存值是否仍覆盖新默认。

**自测：** BIN 已换成新版本，但 addr2line 使用旧目录 ELF，输出的函数名能当根因吗？

**参考答案：** 不能。地址布局可能已变化，必须匹配本轮 ELF 身份后再解释回溯。
