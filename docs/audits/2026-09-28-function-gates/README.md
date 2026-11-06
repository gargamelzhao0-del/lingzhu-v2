# 功能门槛审查证据

关联报告：`../../2026-09-28-功能门槛与逻辑质量深度审查.md`。

- `shake-rearm.c`：直接包含生产策略，验证周期性漏采打断静止重新使能。断言证明当前问题存在，修复后应转换为防回归测试，不能继续要求问题复现。
- `shake-loss.c`：直接包含生产策略，比较60组合成波形在无漏采/每五次漏采一次时的结果。只打印样本结果，不能视为真实召回率测试。
- `shake-init.c`：当前启动函数、结构体、初始化失败分支的抽取快照；调度桩只模拟创建返回前执行失败分支，不模拟FreeRTOS内核回收。
- `*-result.txt`：本次主机执行的原始输出。
- `source-sha256.json`：相关源码、配置、测试脚本、上轮验证日志与最新实机日志的审查时摘要。不是整仓库可复现构建清单。

在工程根目录，安装有GCC的PowerShell中重跑（生成文件位于tmp；无烧录或设备操作）：

```powershell
$auditEvidence = 'docs/audits/2026-09-28-function-gates'
$auditBuild = 'tmp/gate-audit-rerun'
New-Item -ItemType Directory -Path $auditBuild -Force | Out-Null
foreach ($auditCase in @('shake-rearm', 'shake-loss', 'shake-init')) {
    gcc -std=c11 -I main "$auditEvidence/$auditCase.c" -lm -o "$auditBuild/$auditCase.exe"
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $auditCase" }
    & "$auditBuild/$auditCase.exe"
    if ($LASTEXITCODE -ne 0) { throw "case failed: $auditCase" }
}
```

更新生产代码后须重新审核抽取快照，并将复现断言改为修复后的预期；不要仅凭旧快照PASS宣称新固件正确。源码哈希不符时，本证据仅代表历史基线。
