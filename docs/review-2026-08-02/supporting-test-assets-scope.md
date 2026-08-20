# 工程外支持测试资产 Manifest 范围

生成批次：2026-08-02-A

## `cloud-gateway/`

纳入网关源码、测试、package/lock、README 和测试签名私钥。排除 `node_modules`、build/cache、`.git`、`.log/.tmp`、`audio-debug/` 和 `runtime/`；后二者是录音/运行状态，不是测试代码。

## 工作区根 `tools/`

纳入源码、头文件、PowerShell/Python/Node 脚本、CMake、JSON、README 和静态资源。排除 `node_modules`、`.git`、`__pycache__`、`.pyc/.exe` 和任意 `reports/` 生成报告目录。

输出路径以 `cloud-gateway/` 或 `tools/` 开头，按 ordinal 排序。任何纳入文件变化都会使本批 manifest 失效；运行时证据另以测试批次保存，不并入源码基线。
