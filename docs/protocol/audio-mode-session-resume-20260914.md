# 会议/监控断网恢复的会话延续

日期：2026-09-14。设备端实现，适用于同次开机中未退出的会议或监控。

## 恢复效果

首次开启仍使用原流程确定 `audio_session_id`。断网时设备保存该 ID，离线 SD
采集继续使用它。联网认证成功后，每次恢复申请生成新的 `request_id`，并携带
原 `audio_session_id` 和 `reason: "offline_audio_recovered"`。云端按原开启
流程响应，后续 listen 与归档仍属于原音频会话。监控人声暂停/恢复也沿用该 ID。

```json
{
  "type": "audio_mode_request",
  "request_id": "meeting_resume_new_request",
  "mode": "meeting",
  "state": "enabled",
  "reason": "offline_audio_recovered",
  "audio_session_id": "original_audio_session"
}
```

监控使用 `mode: "monitoring"`。示例 ID 仅说明语义，不规定 ID 前缀。
ACK 关联本次新请求；`audio_session_id` 关联持续的音频业务。
该可选字段增补了恢复申请；SD begin/chunk/commit 和 NA/NWXOFF1 格式不变。

## 云端要求

- 接受恢复申请中的 `audio_session_id` 并恢复原业务会话，不用新 request_id
  创建第二段会议。设备收到不同音频会话 ID 时会记录警告并保留原值；云端若
  仍强制使用新 ID，不能视为对接完成。
- 新 listen 的 START 表示传输段开始，在线 sequence 必须从 0 开始，包括
  断网恢复及监控人声暂停后的重新开流。
- 按 `device_id + audio_session_id` 关联在线及 SD 数据，按
  `device_id + segment_id` 保持补传幂等；归档帧按 timestamp_ms 排序，不能按到达时间拼接。
- SD 物理采集序号在在线、离线、恢复之间延续；在线 sequence 每次 listen
  重置，两种序号不能互相去重。在线 NA timestamp_ms 是流内相对时间，SD
  timestamp_ms 是开机时间，不能直接比较。完整录音优先使用覆盖在线/离线的
  SD 归档；如需用在线数据填补 SD 缺口，云端需另行建立各 listen 的时间映射
  或音频对齐，解码为统一 PCM 后消除重叠。现有会话 ID 本身不提供此时间映射。
- 监控静音区间可按产品需求保留静音或仅拼接人声。采集切换期间仍可能有缺口，
  此改动不补回未采集音频，也不提供跨重启绝对时间映射。

## 生命周期与验证

身份仅保存在 RAM。用户关闭、云端终止、离线时长达到上限或恢复放弃后清理；
重新开启应建立新会话。掉线清理传输状态时保留身份及采集序号，SD 异常结束
后重试也接续已有采集序号。

主机检查覆盖重复恢复请求 ID 更新、旧会话 ID 保留、云端返回不同 ID、SD
非零累计序号下在线从 0 开始及 START/END、监控暂停恢复及 SD 镜像会话一致性、
显式关闭后新会话。
实机及正式云端仍需联调：开启→断网录音→恢复→再次断网恢复→关闭并等待补传，
核对新 request_id、相同 audio_session_id、归档 session_id 和帧时间。

2026-09-14 首轮验证结果（后被实机发现序号兼容问题，以下哈希为旧固件）：`run-meeting-host-checks.ps1`、
`run-monitoring-host-checks.ps1`、`run-offline-audio-host-checks.ps1` 全部通过；
SD 测试配置整包构建通过，尚未烧录或进行本轮实机验证。
固件：`C:/tmp/nuwax-sd-test-20260911/NuwaxAIPartner.bin`，3,000,304 字节，
SHA256：`12781fac0c4bbda1bc315211fc845202f071079e24b84d1b694ab7807893a029`。

## 实机问题修正

实机在恢复后报 `expected=0 actual=2391 first=1`，会议进入 active 后立即因
`uplink_enqueue_failed` 退出。根因是上层把 SD 累计序号传给要求首帧为 0 的
上传模块。现已将两类序号分开，同时修复监控暂停后再次开流的相同风险。

捕获回归测试从 `audio_uplink.c` 提取当前生产入队函数和 PCM 构帧函数编译，
使用真实 NA 构帧和 END 标记实现，仅替换异步队列/网络边界。测试明确检查
2391 作为首帧被拒绝，并验证恢复后的实际采集路径从 0 连续上传、原会话 ID
保留。此检查不代替 SD 卡性能、网络收发和正式云端的实机验证。

修正版验证：会议、监控、离线录音三组主机检查通过，SD 测试配置整包构建
通过，8 小时离线配置与测试网关地址保持原配置。修正版固件仍位于
`C:/tmp/nuwax-sd-test-20260911/NuwaxAIPartner.bin`，3,000,304 字节，
SHA256：`a72911bf164e89081ba04541a2bc1e79c9094b51fa64e759960bbbd0340f9aca`。
尚未烧录；本轮未修复实机日志中的 SD 队列满丢帧及测试网关连接失败问题。
