# 灵珠 Lingzhu V2

当前基线：**7.0.223（2026-11-06）**。本仓库以该版本的设备端工程为准，取代原先的 7.0.89 文件快照；历史审查材料仅作追溯，不能覆盖当前源码、配置或最新设计文档。

## 项目文档

- [完整设计与技术实现说明（Markdown）](docs/项目介绍/灵珠工程完整设计与技术实现说明.md)
- [完整设计与技术实现说明（离线 HTML，下载后用浏览器打开）](docs/项目介绍/灵珠工程完整设计与技术实现说明.html)
- [本版构建与软件验证记录](docs/项目介绍/依据/verification.json)
- [本版修复与验证说明](docs/项目介绍/依据/修复与验证说明.txt)

两份说明覆盖设计思路、硬件、启动流程、状态机、对话音频、唤醒、会议与监控、SD 存储与补传、联网配网、UI、OTA、任务调度、故障恢复和后续验证边界。HTML 已内嵌图表，无需外部图表服务。

## 工程结构

| 目录 | 内容 |
| --- | --- |
| `main/` | 主应用、板级驱动、对话、会议监控、协议、UI、配网和 OTA |
| `lib/lvgl/` | 当前使用且经过本地修改的 LVGL 9.5 源码 |
| `lib/`、`components/`、`bootloader_components/` | 工程依赖与启动组件，保留其许可证 |
| `assets/` | 开机图、字体及构建必需的表情资源包 |
| `tools/`、`test/` | 构建辅助、依赖修补、主机回归和协议工具 |
| `config/sdkconfig.7.0.223.defaults` | 从本版发布构建归档复制的完整配置基线 |
| `docs/项目介绍/` | 最新设计说明与可核查依据 |
| `docs/` 其他文件 | 协议说明、开发材料与有日期的历史审查记录 |

## 构建

目标硬件为 ESP32-S3 N16R8，工具链为 **ESP-IDF v6.0**。先进入 ESP-IDF v6.0 的 PowerShell 环境，再在本仓库根目录执行以下命令。使用新的、短路径的外部构建目录，避免复用旧版本配置。

```powershell
$releaseBuild = 'C:/tmp/lingzhu-v2-r223-build'
$releaseConfig = 'C:/tmp/lingzhu-v2-r223.sdkconfig'
Copy-Item -LiteralPath './config/sdkconfig.7.0.223.defaults' -Destination $releaseConfig
idf.py -B $releaseBuild -D "SDKCONFIG=$releaseConfig" reconfigure
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF configure failed' }

# 首次 reconfigure 下载受锁文件约束的 managed_components 后，应用已有的配网修复。
python ./tools/apply-network-provision-reset-fix.py
if ($LASTEXITCODE -ne 0) { throw 'Provisioning dependency repair failed' }

idf.py -B $releaseBuild -D "SDKCONFIG=$releaseConfig" build
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF build failed' }
```

`dependencies.lock` 与 `main/idf_component.yml` 管理组件版本；CMake 自动执行工程内的其他构建期修补。上述命令只构建，不烧录、不擦除 NVS。部分历史工具脚本默认旧版本号、旧构建路径或依赖开发机布局，不能据此判断当前发布版本。

普通对话、会议与监控、正式云端补传、OTA 等行为以 `config/sdkconfig.7.0.223.defaults` 和源码为准。保留原有开发配网兼容设置；本次提交不迁移设备凭据或更改 App/云端协议。用户 Wi-Fi 与设备绑定仍由配网写入 NVS，仓库不包含真实设备密钥、Wi-Fi 密码、录音或原始串口采集。

## 表情资源与 OTA

`assets/emotion/emotion-assets.bin` 是构建必需的固定输入，SHA-256 为：

```text
c8e9258f5963f6b08089ef80af6190ba1053e3e73dfc990d529d090a6f752897
```

它写入独立的 `emotion_0` 分区；应用 OTA 不更新该资源分区、分区表、语音模型或 NVS。完整烧录应使用本次构建生成的分区与烧录参数，不能复用旧版本地址。此提交为源码交付，不附带旧版本固件包或本机 build 输出。

## 验证边界

本次提交核对了当前源码、资源摘要和文档引用，没有连接或操作实体设备。归档记录显示 7.0.223 已通过构建、29 组主机回归及 2 组原生 LVGL 配置验证，但 `physicalAcceptance=false`，不能将它解释为本次重新进行了硬件验收。

源码发布清单见 [publication-manifest.json](docs/项目介绍/publication-manifest.json)。第三方许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和各依赖目录的许可文件。
