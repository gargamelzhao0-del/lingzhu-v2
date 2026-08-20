# NuwaxAIPartner-v2 未进入正式 main 目标的 C 源清单

生成日期：2026-08-02

状态：B0-14 只读基线。比较对象为 main 目录中排除 _archive 后的全部 C 文件，与 main/CMakeLists.txt 中 SRCS_COMMON、条件 OTA source 和 SRCS_LVGL_UI 的 quoted C source 条目。

## 1. 复算结果

| 项目 | 数量 |
|---|---:|
| main 活动 C 文件 | 192 |
| CMake 唯一 C source 条目 | 149 |
| 未进入正式 target 的活动 C 文件 | 43 |
| 其中 host check / host stub 测试资产 | 35 |
| 其中非 host 文件 | 8 |

149 个 CMake source 条目均存在且无重复。这里的清单不表示 43 个文件存在编译错误；它只证明它们不进入当前正式 main target，不能被当作当前固件能力证据。

## 2. Host 测试资产

以下 35 个文件是 host check 资产。它们应当只在 B 级 host runner 中使用，并在执行时验证所有宏、stub 和参数来自当前产品配置。

- main/aec_delay_shadow_host_check.c
- main/aec_fir_delay_control_host_check.c
- main/audio_frame_terminal_host_check.c
- main/audio_modes/meeting/meeting_capture_transaction_host_check.c
- main/audio_modes/meeting/meeting_mode_host_check.c
- main/audio_modes/meeting/meeting_processor_host_check.c
- main/audio_modes/meeting/meeting_protocol_host_check.c
- main/audio_modes/monitoring/monitoring_capture_host_check.c
- main/audio_modes/monitoring/monitoring_mode_host_check.c
- main/audio_modes/monitoring/monitoring_protocol_host_check.c
- main/audio_uplink_low_latency_host_check.c
- main/awake_session_state_host_check.c
- main/barge_in_controller_host_check.c
- main/barge_in_detector_host_check.c
- main/capture_phase_handoff_host_check.c
- main/cloud_protocol_compat_host_check.c
- main/conversation/conversation_audio_health_host_check.c
- main/conversation/conversation_metrics_host_check.c
- main/conversation/turn_guard_host_check.c
- main/dialog_state_policy_host_check.c
- main/emotion_ui_pending_host_check.c
- main/listen_resume_state_host_check.c
- main/live_aec_output_provenance_host_check.c
- main/mcp_server_capability_host_check.c
- main/nuwax_provision_new/nuwax_provision_policy_host_check.c
- main/ota_release_policy_host_check.c
- main/p5_live_fast_gate_host_check.c
- main/p6_gray_confirm_host_check.c
- main/preroll_window_host_check.c
- main/session_wake_word_host_check.c
- main/speaker_backpressure_policy_host_check.c
- main/speaker_generation_policy_host_check.c
- main/speaker_stream_policy_host_check.c
- main/ui_sync_pending_host_check.c
- main/wifi_saved_network_list_host_check.c

## 3. 非 host 文件和处置门

| 文件 | 当前判定 | 不得宣称的能力 | 阶段 1-9 关闭动作 |
|---|---|---|---|
| main/audio_reliable_capture.c | 30 ms reliable capture 原型，含 I2S TODO | 当前采集链路 | 删除、归档或迁移并重新构建/验证 |
| main/audio_reliable_playback.c | 30 ms playback 原型，含 I2S TODO | 当前播放链路 | 删除、归档或迁移并重新构建/验证 |
| main/conversation/d1_playback_path.c | 历史 D1 playback 草稿 | 正式 TTS playback | 明确历史归属或迁移 |
| main/filter_conservative.c | 简化环境过滤原型 | 当前过滤/AEC 效果 | 明确弃用或产品化并验收 |
| main/full_duplex_coord.c | 占位全双工协调实现 | 双讲完整闭环 | 明确弃用或替换为正式链路 |
| main/state_cmd_fast.c | 命令队列原型，存在 TODO | MCP/命令总线能力 | 明确弃用或实现并纳入构建 |
| main/nuwax_ui_v28/assets/faces/face_21_listening_360_rgb565.c | 生成但未链接的资源 | listening face 已在镜像 | UI 资源审计确定替代关系或纳入 CMake |
| main/nuwax_ui_v28/assets/faces/face_22_speaking_360_rgb565.c | 生成但未链接的资源 | speaking face 已在镜像 | UI 资源审计确定替代关系或纳入 CMake |

## 4. 历史归档

main/_archive/v15-goertzel-failed/p3_device_voiceprint_goertzel.c 不在上述活动 192 文件内。它已进入 source manifest，但保持为失败的历史归档，不能作为产品实现或未来迁移的默认候选。

## 5. 关闭条件

1. 每个非 host 文件取得明确的 删除、历史归档、迁移入构建 或 保留生成资源 结论。
2. 任何文件被加入 main/CMakeLists.txt 后，重新生成本清单、构建矩阵、资源预算和受影响模块审计。
3. host check 在 B 级真实执行前，不得以文件存在或 host stub 编译为硬件/产品功能通过证据。

## 6. 对应计划校验

本文件补齐计划 2.2 对 main CMake 差异的单独归档要求，并与 module-inventory.md 的统计和非 host 初判一致。
