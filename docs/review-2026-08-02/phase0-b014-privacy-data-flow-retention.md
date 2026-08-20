# B0-14 隐私、音频数据流、同意、访问、保留与删除静态审计

审计日期：2026-08-03  
授权级别：A 级只读静态审计  
范围：设备 PCM/文本路径、会议/监控持续收音、BLE 配网凭据、OTA manifest、MCP 状态、测试网关 HTTP/日志/WAV 留存和外部 ASR/TTS/API 流程。

## 1. 结论

A级数据流盘点完成；B/C/D 尚未执行，隐私与录音留存验收不通过，不能作为量产或正式云发布依据。设备侧没有发现把普通会话 WAV 主动写入 flash/SD 的主路径，但 RAM 预录、UART/USB 日志、网关快照和网关录音仍构成敏感数据落点。

当前 canonical 问题：`PRIV-001`、`PRIV-002`、`PRIV-003`、`PRIV-004`、`PRIV-005`；`PRIV-006` 可作为 MCP 最小化问题并入访问控制策略。

## 2. 数据流矩阵

| 数据源 | 处理/传输 | 落点 | 静态结论 |
|---|---|---|---|
| 唤醒 PCM | I2S mic -> MultiNet/wake detector | RAM 瞬时缓冲 | 本地判断，不上云；缓冲生命周期未全量 scrub |
| 正常对话 PCM | prelisten -> AEC/VAD -> WebSocket uplink | 设备 RAM、gateway ASR/录音 | 预录最多 256 帧×20 ms，约 5.12 s，存在多份 PCM 副本；网关可保存 WAV |
| ASR/用户/助手文本 | WS/session protocol -> gateway -> OpenAI/Volc/Nuwax API | UART/USB、gateway snapshot/log、供应商 | 设备 `session_protocol.c` 最长约 96 字节正文预览；网关 redaction 不覆盖 `text/textPreview/raw` |
| TTS | assistant text -> provider TTS -> gateway -> speaker | gateway TTS WAV/metadata | 测试网关默认保存，未发现统一 TTL/删除门禁 |
| 会议模式 | authenticated cloud enable 或本地 UI | 设备 continuous session、gateway WAV | 云端 enable 无本地同意；UI 仅使用通用“收音中”语义 |
| 监控模式 | authenticated cloud enable/disable | 设备 continuous session、gateway WAV | 没有对应本地菜单/硬停止路径；同样缺少持久录音指示 |
| BLE vox-config | Security 2 配置 JSON、deviceSecret、SRP 数据 | PSRAM/cJSON/stack/NVS | 释放前没有统一显式 scrub；NVS 保护沿用 NVS-002 |
| OTA | authenticated cloud manifest -> HTTPS download | 静态 `s_transport_manifest`、NVS 事务元数据 | manifest 可能保留签名 URL/query，直到覆盖或重启 |
| MCP status | authenticated WS tool | 云端/日志 | `self.get_device_status` 返回 SSID/IP/free heap，缺字段最小化与调用审计 |

## 3. 关键证据

### PRIV-001（P1，扩展既有问题）

`main/session_protocol.c:136-149,1006-1023,1083-1095,1160-1169,1212-1223` 的 `log_text_preview()` 会在默认 INFO 日志输出 ASR partial/final、用户文本、助手文本和设备命令文本的预览，最长约 96 字节。`wifi_manager.c`、`nuwax_device_uid.c`、`nuwax_cloud_config.c` 还输出稳定 SSID/IP/UID/cloud URL。测试网关 `cloud-gateway.js` 的 snapshot/log 路径保留 `lastAsr.text/raw`，redaction key 列表不把 `text/textPreview` 统一视为敏感字段。

这不是“只输出长度”的结论；设备协议层确实输出正文预览。当前未发现 Wi-Fi password/device_secret/token 的常规明文日志，hello/secret 缓冲存在显式清零，但不抵消会话正文泄露。

### PRIV-002（P1）

会议 `main/audio_modes/meeting/meeting_mode.c:299-365` 和监控 `main/audio_modes/monitoring/monitoring_mode.c:278-327,1161-1194` 可由已认证云命令直接启用持续收音。会议存在本地 UI 开关，监控没有对应 focus menu/shell action；两者在 `nuwax_dialog_policy.h`/drawer 中都折叠为通用 `LISTENING/收音中`。静态代码不能证明每次远程启用都有本地同意、持久明确指示、本地硬停止、撤销和审计。

### PRIV-003（P2；若量产保留 JTAG/core dump 则升级 P1）

正常路径有 256×20 ms 的 PSRAM preroll，且每帧可能同时保留 samples/preclean/human-rescue/reference 等副本（`nuwax_ai_partner_main.c:426-445,5925-5952,6005-6034`）。sleep 主要清元数据，不保证整块音频清零；部分拒绝 barge 区间才清零。`mic_input.c`、`conversation/audio_uplink.c`、meeting/monitoring/wake 临时 PCM 释放前没有统一 scrub。BLE secret/SRP salt/verifier 和 OTA signed URL/query 也没有统一 zero-before-free/clear 契约。

### PRIV-004（P1）

测试网关默认绑定 `0.0.0.0`（`cloud-gateway.js:81-88`）。`GET /` 和 `/api/status` 直接返回 `redactSensitiveData(snapshot())`，没有认证；snapshot 展开 peer state，可能包含 ASR/text/raw、会话和调试路径。局域网客户端因此可以读取转写/状态/调试信息。

### PRIV-005（P1）

测试网关 `CLOUD_AUDIO_DEBUG_SAVE_WAV`、`CLOUD_CONTINUOUS_CAPTURE_SAVE_WAV` 默认开启。普通 uplink/TTS 会保存 WAV+metadata；会议/监控 continuous session 会创建、追加并 finalize WAV。`continuous-audio-capture.js` 只清理临时文件，没有全局 TTL、用户删除、崩溃残留、RMA/factory-reset 删除契约。静态盘点的 `runtime-logs` 为 6,945 个文件、约 1,495,494,945 bytes；其中 WAV 5,091 个、约 1,316,703,146 bytes，时间范围 2026-06-16 至 2026-08-01。

### PRIV-006（P2，可并入策略）

MCP `self.get_device_status` 返回 SSID、IP 和 free heap，虽然走认证 WebSocket，但没有字段级最小化、角色/目的限制或工具调用审计。建议最小化为设备健康摘要，网络拓扑按显式诊断权限提供。

## 4. 外部处理器与政策缺口

- `cloud-gateway.js:4724-4809` 将 WAV/PCM 发送给 OpenAI ASR；`4901-4978`、`5087-5153` 将 assistant text 发送给 OpenAI/Volc TTS；`2998-3037` 将设备文本发送到 Nuwax API。
- 代码不能证明供应商区域、用途、保留期限、删除接口、DPA 或用户同意链路；这些必须由正式云端/产品政策清单补齐。

## 5. 关闭条件

### B 级

网关默认录音关闭并有启动断言；`/`/`/api/status` 未授权返回 401/403，授权输出去除 text/raw/SSID/IP/URL query；设备 golden log 禁止正文和秘密；会议/监控实现 consent、持久录音指示、本地 hard-stop、撤销和审计；完成 TTL/delete/crash cleanup、full-ring scrub、secret/manifest scrub 与 PII scanner 测试。

### C 级

生产 profile 禁止正文 INFO，启用 WSS-only、NVS/Flash encryption，移除固定开发 PoP；core dump 必须禁用或有加密/保留策略；网关绑定受保护地址并显式开启录音；第三方处理器建立用途/区域/保留/DPA 清单。

### D 级

实机验证会议/监控指示和本地硬停止、撤销/重启边界、未授权 LAN 拒绝、UART/USB 无正文、断网/掉电无孤儿 WAV、TTL/删除/factory reset/RMA 清理和供应商删除/导出。

## 6. 反向核查

本轮只读静态审计，未修改业务源码/配置，未构建，未运行测试，未接触 COM5/NVS，未执行 OTA。B0-14 应标记为 `completed-A;B/C/D-open;release-blocked`。
