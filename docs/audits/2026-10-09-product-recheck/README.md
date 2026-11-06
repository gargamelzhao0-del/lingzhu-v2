# 2026-10-09 第二轮产品审查证据

对应[上架问题复核与修复方案](../../2026-10-09-上架问题复核与修复方案.md)。本目录是故障复核和方案设计证据，不是可发布固件；未更改产品代码，未访问真实设备或攻击线上服务。

## 文件与证据层次

- `audio-ux/real-wait-output.txt`：从实际源码抽取问候等待循环及相关函数，链接现有会话状态模块；无文本/等待 TTS 时模拟 10 分钟未恢复，发送失败对照恢复。RTOS、时间、音频硬件、网络和无活动的其他服务为桩，不是整个 app_main 多任务运行。
- `audio-ux/recheck-report.txt`：具体触发条件、检查过的反例、原有解除路径和实施建议。其“R2”是本子审查的触摸取消问题，对应总报告的 P2，勿与总报告 R2 录音状态混淆。
- `audio-ux/nonzero-generation-review.txt`：追加追踪已 START、尚无首 PCM 的 terminal 恢复，确认正常绑定时具有独立收敛路径；列明未验证的绑定/物理停止失败边界。
- `audio-ux/candidate*`：仅用于设计可行性验证。新关闭原因只在候选 policy 和布尔传输桩中；epoch 递增、重连成功为注入，未修改或执行真实 transport 新原因的实现。
- `data-network/persistent-missing-sd*`：真实重试/到期/UI 函数结合确定性介质错误；两模式各 600 秒模拟、124 次重试、零 capture 启动仍 ACTIVE，包含介质恢复和到期对照。
- `data-network/recheck-findings-and-plan.json`：状态模型、归属迁移、用户体验和验收方案，均属设计。
- `data-network/wake-timeout-reconnect-replay-review.json`：重连取消补传与 ACK/删除的静态交叉审计；未验证实际云端持久化。
- `security/cloud-config-recheck.log`：直接编译生产 cloud config，使用内存 NVS 和合成凭据，四次进程运行共十二条 PASS；没有 BLE、网络、Flash 或 eFuse 操作。
- `security/commands-and-scope.txt`：配置复核执行说明和边界。若有详细域报告，以其前提限制与总报告共同阅读。
- `packaging-result.json`、`review-result.json`、`SHA256SUMS.txt`：源码未变检查、归档入口复跑结果与证据校验。

## 交叉审查后补充的限制

1. 问候候选只覆盖 WAIT_TEXT/WAIT_TTS 且 `speaker_generation=0`。生产 START 可以先绑定非零 generation，再等待 FIRST_PCM；该分支使用 terminal barrier。追加只读追踪确认其正常 finalize 会先转 LISTENING，再调用恢复，不能归为同一 R1。当前候选不覆盖此分支，应保留并回归既有机制，不得仅放宽 generation 条件。
2. 触摸取消动作应携带业务身份，由主 owner 消费时再次复核，不能让旧动作取消新回合。
3. 写失败不自动等于音频丢失：缓冲仍完整且后续全部保存，不应提示已丢；已知缺口和未知结果分开。
4. 旧档迁移需要原归属证明或经验证的原权利人授权，当前持有设备/账号并非原归属证明。
5. 强制关机由板级 FreeRTOS 按键任务执行；没有证明 MCU/调度器挂死时有独立硬件旁路。

## 可重复执行

从仓库根目录运行。需要 Python、GCC、PowerShell；这些命令只构建/运行主机测试，不构建/烧录产品。会更新本目录测试输出，并生成测试 `.exe`；安全配置测试的可执行文件放在系统临时目录。没有网络访问。

```powershell
python docs/audits/2026-10-09-product-recheck/audio-ux/recheck.py
python docs/audits/2026-10-09-product-recheck/audio-ux/candidate.py
python docs/audits/2026-10-09-product-recheck/data-network/recheck_offline_failure.py
& docs/audits/2026-10-09-product-recheck/security/reproduce.ps1 -RepoRoot (Get-Location).Path
```

候选命令依赖前一个命令生成的 baseline。音频复核脚本会读取第一轮已归档的 `audio-ux/reproduce.py` 的测试桩字符串；不会执行旧脚本。所有生产函数仍从当前工作区读取。若源码改变，须重新审查提取假设；旧 PASS 不能自动继承到新版本。

模拟 600000 ms、候选注入的 1000 ms 重连、对照 700 ms 和候选 15100/31100 ms 均不是实机性能指标。测试日志中的 PASS 是对应断言通过，包含对故障行为的确认，不代表产品无缺陷。

最终上架结论仍为 NO-GO；真实设备、APP、正式云端及硬件验收未被这些测试替代。
