# NuwaxAIPartner-v2 可复现构建审计

生成日期：2026-08-02

状态：本文原始 B0-06 结论保留；`implementation-verification-2026-08-03.md` 记录的是历史 7.0.22 实现候选。当前另有一个既有 7.0.23 外部构建，但未绑定本轮 source/config/OTA manifest。两次隔离可复现构建、签名发布绑定和实机仍未完成。

## 1. 冻结输入

- 工程：`NuwaxAIPartner-v2`
- target：`esp32s3`
- ESP-IDF：`C:\esp\v6.0\esp-idf`，v6.0，commit `662a3be354759d9487bf4b1a629fadb766cb1800`
- 产品 defaults：`sdkconfig.defaults;sdkconfig.product-ota.defaults`
- 生成配置：每个批次使用新的工程外 `SDKCONFIG` 文件
- 构建目录：每个批次使用新的 `C:\tmp\nuwax-v2-review-<batch>-s3`
- 源码输入：`source-manifest.sha256`，2026-08-03 A2-05 续接基线共 7,918 项，SHA-256 `ba22f1e64a4b95e19cca8d32e789798f5a15ee8c1f8af6b8d38553cc5bf71450`

直接用 IDF Python 调用 `idf.py` 而不加载 export 环境会因缺少 `ESP_IDF_VERSION`/`IDF_PYTHON_ENV_PATH` 失败。必须先在同一 PowerShell 子进程加载指定 `export.ps1`，禁止依赖机器默认 PATH 或另一套 IDF。

## 2. 批次命令

以下命令已静态核对，但本轮没有执行 `set-target` 或 `build`：

```powershell
Set-Location "C:\Users\Administrator\Documents\work\Nuwax AI Partner\Nuwax AI Partner 7.0\NuwaxAIPartner-v2"
. "C:\esp\v6.0\esp-idf\export.ps1"

$Batch = "<YYYYMMDD-HHMMSS>"
$Build = "C:\tmp\nuwax-v2-review-$Batch-s3"
$GeneratedSdkconfig = "C:\tmp\nuwax-v2-review-$Batch.sdkconfig"
if ((Test-Path -LiteralPath $Build) -or (Test-Path -LiteralPath $GeneratedSdkconfig)) {
    throw "Review batch paths must be new"
}

idf.py -B $Build `
  -D "SDKCONFIG=$GeneratedSdkconfig" `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.product-ota.defaults" `
  set-target esp32s3

idf.py -B $Build `
  -D "SDKCONFIG=$GeneratedSdkconfig" `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.product-ota.defaults" `
  build
```

不得在审计构建中运行交互式 `menuconfig`；任何配置变化必须先变成受控 overlay。第二条命令继续显式传入 `SDKCONFIG` 和 defaults，避免只依赖 CMake cache。

## 3. 构建后强制校验

1. 记录命令、退出码、开始/结束时间、所有工具绝对路径和版本。
2. 保存生成 `sdkconfig`、`project_description.json`、`compile_commands.json`、map、`flasher_args.json` 和编译 warning。
3. 将生成配置与当前候选 `sdkconfig` 做归一化 diff；任何漂移先登记，不能直接覆盖。
4. 运行 partition table 解码、`esptool image-info` 和 image size/fit 检查。
5. 核对 app descriptor 7.0.24、OTA sequence 7000024、target esp32s3、partition schema、build flavor 和 srmodels 兼容性。
6. 对 bootloader、partition table、ota data、srmodels、app 和最终 OTA manifest 计算 SHA-256。
7. 第一方源码零新增 warning；第三方 warning 与冻结基线分开处理。
8. 构建完成后重新生成源码 manifest；若与本批输入不一致，构建证据作废。

## 4. 现有缓存对照

- 工程内 `build`：target esp32s3、Ninja、版本 7.0.21，陈旧。
- 外部 `C:\tmp\nuwax-v2-722-patch-s3`：历史 7.0.22 候选，使用独立 SDKCONFIG 和双 overlay；只对应历史验证快照。
- 外部 `C:\tmp\nuwax-v2-fix-7.0.23-20260803-d`：target esp32s3，版本 7.0.23，app SHA-256 `774c620149ae976f943dd7c2c7b2d2d0b40181f0f1e2bd60c8a1bfbbd47873fe`；生成配置直接指向工程 `sdkconfig`，未绑定本轮 source manifest、归一化配置和 OTA manifest。
- README 的构建方法已使用外部 build 和 SDKCONFIG，但随后 `menuconfig` 允许不可追溯变更，且 README 版本说明仍为 7.0.17。

## 5. 当前结论

B0-06 的只读命令审计完成，实际可复现性尚未证明。既有 7.0.23 外部构建只能证明其自身可解析和曾完成链接，不能替代当前 7,918 项源码的发布绑定。该项状态必须保持“部分完成，等待 C 级构建”，直到全新批次 configure/build/link、配置 diff、版本真值、镜像 hash 和 warning 门全部通过。
