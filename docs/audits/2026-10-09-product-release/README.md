# 产品上架评估证据

关联报告：[2026-10-09 产品上架评估](../../2026-10-09-产品上架评估.md)。

这里保存当前 7.0.113 的基线核验、重新执行的回归日志和独立故障复现。所有复现均在主机上执行，没有烧录、操作真实设备或证明真机通过。

- `baseline-verification.json` 和 `source-hashes.json`：本轮审查基线与 636 个文件摘要。不是完整第三方依赖的可复现构建清单。
- `test-results.json`：7 组已有核心回归的退出码和耗时。
- `focused-results.json`：5 组补充充电与显示检查。
- `artifact-results.json`：20 项当前构建产物检查。
- `security-profile-evidence.json`：正式配置及候选 BIN 的安全属性，不包含凭据值。
- `audio-ux`：真实生产恢复/超时函数、实际状态模块与硬件桩的组合，复现问候超时无法自恢复，以及问候期间屏幕取消无效。
- `data-network`：真实模式状态同步函数，复现无采集而保留 lease 时仍发布 ACTIVE。

下列命令在工程根目录执行，需要 Python 和 GCC，只在对应审查目录产生文件：

```powershell
py -3 docs/audits/2026-10-09-product-release/audio-ux/reproduce.py
py -3 docs/audits/2026-10-09-product-release/data-network/reproduce_false_recording_state.py
```

复现程序退出 0 表示确认了报告中的缺陷，不表示产品通过。修复之后应将这些条件转换为防回归预期，不能继续以复现旧缺陷为验收成功。`source-evidence.json` 和 `false-recording-state-evidence.json` 包含源码/函数哈希与主机验证范围；源码变更后应重新审查。

目录中的证据收集脚本是当时临时目录工作的快照，原执行位置为 `tmp/product-launch-review-20261009`；重新执行故障用例请使用上面的两个命令。
