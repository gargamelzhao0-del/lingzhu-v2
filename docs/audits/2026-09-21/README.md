# 2026-09-21 工程审查证据

主报告：[工程全面审查与后续优化计划](../../2026-09-21-工程全面审查与后续优化计划.md)。

本目录记录本轮代码审查和主机测试，不含新的实机运行结果。没有为使检查通过而修改业务代码或测试判据。

| 文件 | 内容/结果 |
| --- | --- |
| `audit-baseline.json` | 工作区关键源码 SHA、Git HEAD 和最近固件 SHA；工作区有未提交修改 |
| `effective-config-excerpt.txt` | 从已构建固件的 sdkconfig.h 摘出的相关开关；不包含 PoP 或设备密钥 |
| `source-size-inventory.json` | 源码文件物理行数；仅作为模块规模参考 |
| `wifi.log` | Wi-Fi 恢复脚本退出码 0 |
| `meeting.log` | 会议脚本退出码 0 |
| `monitoring.log` | 监控脚本退出码 0 |
| `offline.log` | 离线音频脚本退出码 0 |
| `provisioning.log` | 配网策略可执行检查通过，但后续源码契约使脚本整体退出码 1；失败位置 tools/run-provisioning-host-checks.ps1:176 |
| `synthetic-longrun-negative.log` | 人工构造的异常日志，用于验证长稳门禁；不是设备实测数据 |
| `longrun-gate-reproduction.txt` | 上述异常日志被现有门禁误判 PASS 的输出；退出码 0 |

配网源码契约已确认至少一条过时：规则要求 `heap_caps_free(previous1)`，当前 LVGL 代码采用 `release_draw_buffer(previous1)`，其中包含驱动缓冲归还处理。这个检查失败本身不能证明实际配网恢复错误；修正时应保留行为约束并检查同组其余条件。

复现长稳门禁漏检（工程根目录执行）：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\check-device-longrun-gate.ps1 -LogPath .\docs\audits\2026-09-21\synthetic-longrun-negative.log
```

预期正确行为应该是拒绝该日志；本次观察到脚本返回 PASS。后续修复时应保留这份反例。

其他检查采用各 `tools/run-*-host-checks.ps1`，GCC 路径为 `C:/Users/Administrator/scoop/apps/gcc/current/bin/gcc.exe`。完整命令和日志保留在本次任务记录中。
