# NuwaxAIPartner-v2 源码基线 Manifest 范围

生成批次：2026-08-03-A2-08-continuation（7.0.24 静态评审续接）

## 纳入范围

- `NuwaxAIPartner-v2` 下 tracked 与 untracked 文件均从文件系统枚举，不依赖 Git。
- 纳入第一方源码、第三方源码、头文件、脚本、CMake/Kconfig、`sdkconfig*`、分区表、依赖锁、协议/评审前既有文档、UI/模型/归档二进制和生成资源。
- `_archive` 不排除；其文件进入 hash，但只能按历史归档解释。
- 输出按相对路径的 ordinal 顺序排列，格式为 `<lowercase SHA-256> *<forward-slash relative path>`。

## 显式排除

| 规则 | 原因 |
|---|---|
| 根目录第一段匹配 `^build(?:-|$)` | 历史/可再生构建输出，不属于当前源码基线 |
| 任一路径段严格等于 `build` | 嵌套 host/test 可再生构建输出，不属于源码输入 |
| 任一路径段 `.git`、`.cache`、`__pycache__`、`node_modules` | 版本控制元数据或缓存/依赖安装输出 |
| 扩展名 `.pyc` | Python 可再生缓存 |
| `docs/review-2026-08-02/` | 本轮生成的证据目录；避免 manifest 自引用 |
| `docs/2026-08-02-NuwaxAIPartner-v2全工程代码评审执行工作记录.md` | 执行中持续更新，单独记录版本与最终 hash |

评审计划本身纳入 manifest；业务源码或纳入范围文件变化后，当前批次自动失效并必须重建。执行工作记录和证据目录应在阶段收口时另生成 evidence manifest。

## 复算入口和路径异常

- 唯一复算入口为 `docs/review-2026-08-02/freeze-source-manifest.ps1`；脚本使用 Win32 long-path API 读取文件，并用 `StringComparer.Ordinal` 排序。
- 2026-08-03 复算发现根目录存在合法 NTFS 文件 `CON`（11,733 bytes，SHA-256 `6b3888b17443a59ab4ce3e3f520e701c0690374f22c684e9d5640ea8d54f5bc0`）。它通过 `\\?\` 路径可读，内容是一个 sdkconfig 片段，因此按“文件系统全量”规则纳入 manifest；但该 Windows 保留名无法由普通 Win32 工具稳定处理，登记为 `BASE-003`，在来源确认前不得作为构建输入，也不得擅自删除。
- `test/host/build/nuwax_host_tests.exe` 属于嵌套可再生输出，按新增规则排除；这不是源文件缺失。

## 当前冻结结果

- 条目数：7,918。
- manifest SHA-256：`0ec684337512eae921916825db78ca92b56cd7fce1fd8c764359e21fd272ce3e`。
- 相对最初 7,917 项旧基线仍新增 `main/conversation/realtime_log.h`；当前续接快照还包含配网模拟器/契约、监控 VAD 与 capture pause/resume ACK、主入口/session 协议和 `sdkconfig*` 的后续修改。文件集合未增减。A2-05 收口期间并行修改依次触及 charging UI、speaker generation/admission/ring buffer、CMake/Kconfig、`sdkconfig.defaults`、产品 overlay、两个 host runner 与最终 `sdkconfig`；电池/OTA数值保持 8 s、10/15%、1200 mAh、30%、4000 mV。所有更早 hash 均已失效。
- A2-07收口前，相对A2-06正式manifest为0 missing、0 extra、7个hash mismatch：`main/conversation/audio_uplink.c`、`main/conversation/barge_in_dispatcher.c`、`main/conversation/speaker_playback.c`、`main/nuwax_ai_partner_main.c`、`main/ws_transport.c`、`tools/run-conversation-host-checks.ps1`、`tools/run-v2-product-host-checks.ps1`。其中启动主入口、speaker和WebSocket已按最新代码重审；其余漂移会使相关旧结论进入最终复核队列。
- A2-08期间又检测到4个并行漂移文件：`main/capture_phase_handoff_host_check.c`、`main/conversation/capture_phase_handoff.h`、`main/nuwax_ai_partner_main.c`、`tools/run-conversation-host-checks.ps1`。主入口已按最新hash重新计算fatal站点，3/32/3/4/0/0结论不变；capture handoff与runner进入最终跨阶段复核队列。runner在收口期间连续修改两次，最后停在SHA-256 `4a5090606150d99bd8f2d118eb7c0de83212be6b9a73cfa746c33fb5ade08f68`。
- 并行开发仍在进行，任何旧阶段hash都只代表中间快照。A2-08最后连续两次完整复算均为7,918项、0 missing/mismatch/extra/duplicate和同一manifest hash。全工程审计结束后还必须以最终source manifest重新执行跨阶段代码结论校验。本轮新增/修改的评审目录和执行工作记录按既定规则不进入源码manifest。
