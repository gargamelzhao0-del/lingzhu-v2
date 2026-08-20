# 阶段 1：WebSocket 与音频并发边界只读审计

审计日期：2026-08-02  
对象：`NuwaxAIPartner-v2` 当前文件系统快照  
权限：A 级只读；未修改业务源码、未构建、未运行 host test、未操作 COM5、未写 NVS、未 OTA。

## 1. 范围

本文件记录阶段 1 中 WebSocket RX/TX、speaker ingress、音量同步和 audio uplink 的锁/背压/生命周期证据。结论来自当前源码静态调用链；实际阻塞频率、峰值延迟和堆碎片仍需 B/D 级压力与故障注入验证。

## 2. 已确认问题

### NET-CONC-001（P1）：发送路径把 lifecycle 与 I/O 锁耦合到完整网络写

证据链：

- `ws_transport.c:959-999`：发送路径取得全局 `s_lifecycle_lock` 后，在持锁状态调用 `send_frame_unlocked()`。
- `ws_transport.c:576-611`：底层写在 `io_lock` 内重试，并在锁内 `vTaskDelay(10 ms)`；完整发送预算可达约 4 s。
- RX 读取同样需要 `io_lock`（`:540-563`）。
- epoch close 请求只有限等待约 50 ms 获取 lifecycle lock（`:1712-1738`）；失败由布尔值表达，而多个调用方没有形成可靠重试/latched close intent。

静态结论：拥塞上行能够让 RX、ping/pong 和 close 清理等待发送锁；同时 close 请求可能因锁超时而丢失。该路径不是 `portMAX_DELAY` 死锁，但违反“控制/关闭不能排在音频发送背压后”的产品不变量。

关闭条件：close intent 必须先以原子/事件方式锁存；TX 与 RX 不共享长持有 I/O 锁，或采用 poll/wakeup；网络写重试期间不得持 lifecycle lock；以 TX stall + RX/ping + close 并发故障注入证明有限恢复。

### NET-CONC-002（P1）：CONNECTING 期间接受的 close 可被 OPEN 发布覆盖

证据链：

- `ws_transport.c:1447-1450`：connect 开始发布 CONNECTING、`connected=false`。
- `ws_transport.c:1734-1736`：`request_close_if_epoch()` 可发布 CLOSE_REQUESTED、`connected=false`。
- TLS/TCP/handshake 后 `ws_transport.c:1489-1536` 无条件发布 `connected=true` 和 OPEN，发布前没有重新检查 close/cancel generation。

当前主流程通常在 `nuwax_ws_connect()` 返回后才向全局发布 `ws`（`nuwax_ai_partner_main.c:16161-16189`），降低已知路径的可达概率，但 transport API 本身存在竞态，不能作为安全契约。

关闭条件：每个 connect 阶段完成后、尤其 OPEN 发布前检查 cancel/epoch/lifecycle；被取消时只允许进入 owner cleanup，不得重新发布 OPEN；增加 connect/close 交错 host property test。

### AUDIO-CONC-001（P1）：WebSocket RX 的 speaker ingress 可被硬件初始化和 manager mutex 无限阻塞

证据链：

- `nuwax_ai_partner_main.c:4226-4248`：WS binary callback 直接进入 speaker ingress；回调由 RX task 执行（`ws_transport.c:1299-1303`）。
- `speaker_playback.c:1385-1471`：ingress 在 nominally nonblocking queue send 之前用 `portMAX_DELAY` 等 `s_speaker_ctx.mutex`。
- `speaker_playback.c:1099-1116`：播放任务可能在持有 manager mutex 时 lazy-init speaker output。
- `speaker_output.c:184-205,295-360`：output 初始化包含 I2S 分配、重试和 delay。

静态结论：首次初始化或硬件恢复时，WS RX 可被 manager mutex 阻塞，导致业务控制、ping/pong 和 close 不能及时处理；日志中“callback did not block”的描述不构成代码保证。

关闭条件：RX ingress 只允许固定/有界、try-lock 或无锁队列提交；I2S/codec init/deinit 只能由 speaker owner 在 manager mutex 外执行；注入 I2S init/write stall 验证 RX heartbeat、控制帧和 close 延迟。

### AUDIO-CONC-002（P2）：音量设置跨两层锁同步等待 I2S writer

证据链：

- `speaker_playback.c:3103-3141`：UI/业务音量更新取得 speaker manager mutex，再同步调用 output setter（同步位置 `:660-675`）。
- `speaker_output.c:396-412`：output setter 可等待 output lock 约 250 ms。
- `speaker_output.c:426-557`：PCM writer 持有 output lock 执行最长约 1000 ms 的 I2S write（由当前 sdkconfig 配置）。

静态结论：音量更新可放大 manager mutex 持有时间，继而阻塞 RX speaker ingress。逻辑音量更新与硬件应用缺少异步边界。

关闭条件：manager 锁内只更新 revision/目标值，物理音量由 speaker owner 异步应用；UI/MCP/cloud 获得最终 revision 回读；测量 p95/p99。

### NET-CONC-003（P2）：RX callback 同步发送 busy/queue-full ACK

证据链：

- `nuwax_ai_partner_main.c:3970-4012`：文本帧在 WS RX callback 内分发；关键命令入队还可等待约 40 ms（`:4042-4080`）。
- meeting busy/full 在 `audio_modes/meeting/meeting_mode.c:1066-1114` 同步发送 ACK；monitoring 在 `audio_modes/monitoring/monitoring_mode.c:265-275,1161-1193` 同样处理。
- ACK 调用普通 `nuwax_ws_send_text()`（`ws_transport.c:1546-1550`），进入完整 lifecycle/I/O 锁和发送预算。

静态结论：RX 回调可能因为向同一 transport 同步写 ACK 而被数秒背压，控制帧优先级和 callback 有界性均不成立。

关闭条件：RX 仅验证/入队，ACK 由有界 control-TX owner 发送；critical control 与音频数据分级；满队列时仍有可观测、有限的拒绝结果。

## 3. 未发现直接 P0 的部分

- 当前 `audio_uplink` 的 queue/pool/task cleanup 按 finished/timeout 分层，停止未确认时选择关闭 WS，最终仍无法停止则重启而不是释放 live task resources；静态代码未见直接 queue UAF。
- `audio_uplink` 每轮动态创建/删除两个队列、packet pool 和约 10 KiB PSRAM task stack，当前不能据此宣称泄漏；但高频短会话必须以 heap largest-block、fragmentation 和 1000 次生命周期 stress 验证。

## 4. 对应计划校验

- 覆盖阶段 1.3、1.4、1.6、1.7、1.11、1.17，并为阶段 4/5/8/9/10 建立动态验证入口。
- 当前状态：静态问题确认完成；修复与动态关闭未授权，不能标记为已解决或实机通过。
