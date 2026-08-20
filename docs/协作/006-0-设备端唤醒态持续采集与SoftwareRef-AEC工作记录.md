# 006-0 设备端唤醒态持续采集与 Software Ref AEC 工作记录

日期：2026-07-07  
状态：进行中  
负责人：Codex  

## 1. 当前工作目标

设备端开发重心调整为“音频出入口”：

- 休眠态：不上传音频，仅保留本地唤醒词能力。
- 唤醒态：持续采集音频，本地保留预缓冲。
- 未播放时：通过保守 VAD / 能量门禁 / 降噪后上传近端人声。
- 播放时：播放不中断，同时持续采集；设备端做第一轮 AEC、降噪、近端人声门禁，再上传尽可能干净的用户声音。
- 云端负责第二轮 AEC、声纹/人声过滤、语义相关性和是否打断决策。

核心原则：设备端优先“保真人声、压设备声”，不要为了追求设备端完全干净而把用户声音压没。

## 2. 当前关键假设

本阶段先按以下假设推进：

1. Vocat slot2 硬件参考暂按“不健康/不可靠”处理。
2. 主 AEC 参考改用 software ref，即实际写入 speaker 的 PCM 环形参考。
3. slot2 后续只做健康检测和辅助诊断，未通过健康检测前不参与主 AEC。
4. MIC1 作为主上传音频来源。
5. MIC2 必须纳入主链路，优先用于近端人声判断、环境/设备声区分和门禁辅助；是否做波束成形需在实测后决定。
6. 当前测试网关继续用于构造播报和保存上行音频，后续再接云端工程师 URL。

## 3. 非目标

本阶段暂不做：

- 设备端语义判断。
- 设备端最终打断决策。
- 设备端声纹识别。
- 大规模 UI 重构。
- 替换现有 NA 协议。
- 直接整套迁移 xiaozhi-esp32-main。

## 4. 阶段计划

### S0 文档与基线确认

目标：
- 建立本工作记录。
- 固定当前工程假设和验收目标。
- 记录当前代码问题和风险。

验收：
- 文档存在并可持续追加。
- 后续所有开发/测试结果均追加到本文件。

状态：已完成。

### S1 当前主链路审计

目标：
- 梳理当前 I2S 采集、speaker 播放、software ref、live AEC、audio cleaner、VAD、上传队列的真实调用关系。
- 确认 MIC2 在主链路中的缺口。
- 确认 session command / websocket 主循环对音频实时性的影响点。

产出：
- 问题清单。
- 改造点清单。

状态：已完成。

### S2 唤醒态持续采集架构设计

目标：
- 将当前“事件触发式临时采集”调整为“唤醒态常驻采集 + 上传门禁”。
- 设计 Nuwax AudioService 风格任务拆分：
  - audio_input_task
  - audio_processor_task
  - audio_uplink_task
  - audio_playback_ref 写入路径

参考：
- `xiaozhi-esp32-main/main/audio/audio_service.*`
- `xiaozhi-esp32-main/main/audio/processors/afe_audio_processor.*`

状态：进行中。

### S3 Vocat 三路输入改造

目标：
- 新增或改造 Vocat TDM 读取接口，一次读取：
  - slot0 / MIC1
  - slot1 / MIC2
  - slot2 / 硬件参考候选
- 主链路不再在 `MIC1+ref` 与 `MIC1+MIC2` 两种读取方式之间二选一。

验收：
- 日志能同时输出 MIC1/MIC2/slot2 的 RMS、peak、健康状态。
- 主链路可获取 MIC2 特征。

状态：待开始。

### S4 Software Ref AEC 主路径

目标：
- speaker 输出时同步写入 software ref ring。
- AEC 主参考优先使用 software ref。
- slot2 未通过健康检测前不参与 AEC。
- 做 delay_samples 校准或至少提供可重复测试的 delay sweep。

验收：
- 播放 no-human 测试不误上传或极低误上传。
- 播放 human 测试中设备播报声明显降低。
- 用户人声不被严重压没。

状态：待开始。

### S5 双麦近端人声门禁

目标：
- 基于 MIC1/MIC2 建立近端人声辅助判断：
  - 双麦能量
  - 双麦相关性
  - 双麦能量差
  - MIC1/MIC2 与 playback ref 的相关性
  - AEC 后残余能量
- VAD 只作为 speech activity，不作为“用户身份识别”。

验收：
- 没有人声时，设备播报不应稳定触发上传。
- 有人声时，能稳定打开上传门禁。
- 不要求设备端判断是否打断。

状态：待开始。

### S6 音质与实时性优化

目标：
- 优化电流声、断续、拖延、回声残留。
- 增大 during-playback preroll，避免人声开头被切。
- 降低 session command 对音频循环的阻塞。
- 保持状态命令低延迟执行。

验收：
- 人声连续性明显改善。
- 上行音频开头不明显缺字。
- 状态/表情命令仍能实时响应。

状态：待开始。

### S7 集成验收

目标：
- 使用测试网关完成：
  - no-human 播放测试
  - human during-playback 测试
  - 未播放唤醒态说话测试
  - 休眠态不上传测试
- 记录每轮固件版本、日志、请求 ID、用户主观验收结果。

状态：待开始。

## 5. 当前已知问题

1. 当前主链路没有合理使用 MIC2。
2. 当前 slot2 硬件参考在多轮日志中表现不稳定，暂不适合作为主 AEC ref。
3. 当前采集是事件窗口式，播放前期人声容易错过。
4. 当前上传门禁依赖 AEC 输出连续性，真人声音可能被 AEC 压掉后无法开门。
5. 当前 preroll 偏短，人声开头容易丢失。
6. 当前 session command 处理仍可能影响采集/AEC 实时性。
7. 当前 P5/P6 本地打断实验路径与新目标不完全一致，需要降级或隔离。

## 6. 实时进度记录

### 2026-07-07 / 006-0 / 初始化

内容：
- 建立本工作记录。
- 明确当前假设：slot2 硬件参考暂按不健康处理，主路径切到 software ref。
- 明确后续开发以设备端唤醒态持续采集、设备端第一轮 AEC/降噪/近端门禁为核心。

进度：
- S0：进行中。
- S1-S7：待开始。

下一步：
- 进入 S1，审计当前主链路代码和实际日志，形成可执行改造点。

### 2026-07-07 / 006-1 / S1 主链路审计

范围：
- `main/mic_input.c`
- `main/mic_input.h`
- `main/nuwax_ai_partner_main.c`
- `main/speaker_output.c`
- `main/conversation/live_aec.c`

审计结论：

1. 当前 Vocat 槽位定义为：
   - slot0：MIC1 primary
   - slot1：MIC2 secondary
   - slot2：AEC ref candidate
   - slot3：NC

2. 当前主上传链路在播放期调用 `nuwax_mic_input_read_vocat_mr_pcm16()`，只读取：
   - MIC1
   - slot2 ref

3. `nuwax_mic_input_read_vocat_mic12_pcm16()` 已存在，但不参与主上传/AEC/VAD 路径，只能读取 MIC1/MIC2，不能同时读取 slot2 ref。

4. 因此当前代码无法在主链路同时拿到 `MIC1 + MIC2 + ref`，不满足后续双麦近端门禁和 ref 健康诊断的基本输入要求。

5. 当前 `speaker_output.c` 已在 speaker 写出时调用 `nuwax_p3_software_ref_write_interleaved_lr()`，software ref 基础路径存在，可作为 slot2 不健康时的主参考来源。

6. 当前 `live_aec.c` 在外部 ref 不活跃时会 fallback 到 software ref，但主链路仍优先传入 slot2 ref。按本阶段假设，后续应显式禁用 slot2 作为主 AEC ref，避免无效硬件参考污染判断。

7. 当前 `stream_microphone_audio()` 仍由 websocket 会话循环驱动，不是独立常驻音频服务。这是后续 S2 的架构改造点。

8. 当前 during-playback 门禁仍主要依赖 AEC 输出连续性。若 AEC 把真人近端压低，会导致无法打开上传门禁。后续必须引入 MIC2 近端辅助证据。

决定：
- 先执行 S3 的基础代码改造：新增 Vocat 三路同帧读取接口。
- 本次只补输入能力，不立即改变上传门禁策略，降低风险。

进度：
- S0：完成。
- S1：完成。
- S3：准备开始。

### 2026-07-07 / 006-2 / S3 三路读取接口基础实现

内容：
- 在 `main/mic_input.h` 新增 `nuwax_mic_input_read_vocat_mic12_ref_pcm16()` 声明。
- 在 `main/mic_input.c` 新增对应实现。
- 新接口一次读取 Vocat TDM：
  - slot0 -> MIC1 primary
  - slot1 -> MIC2 secondary
  - slot2 -> hardware ref candidate
- 本次只补齐输入能力，暂未接入主上传循环，避免在未验证前改变现有音频行为。

设计判断：
- MIC1 沿用现有 `apply_vocat_input_gain()`，保持主麦行为不变。
- MIC2 和 slot2 先按原始通道取出，后续通过 S5 做双麦校准和近端特征判断。
- raw stats 暂继续以 MIC1 为主，保证原有诊断语义不变。

待验证：
- ESP-IDF build：通过。

进度：
- S3：基础接口已实现，构建通过。

构建记录：
- 构建目录：`C:\tmp\nuwax_build_audio_ab`
- 结果：成功。
- 固件大小：`0x2a0500`
- 新增警告：无。
- 既有警告：wake word / P5 overlap / P3 diag / P5 helper unused 等，均非本次新增接口导致。

下一步：
- 将主采集循环切换为 Vocat 三路同帧读取。
- 按当前假设保持 slot2 不参与 AEC，主 AEC 走 software ref。

### 2026-07-07 / 006-3 / S3 主采集循环接入三路读取

内容：
- `stream_microphone_audio()` 在 Vocat 板型下改为调用 `nuwax_mic_input_read_vocat_mic12_ref_pcm16()`。
- 每帧同时读取：
  - `s_audio_samples`：MIC1，继续作为主上传音频。
  - `s_audio_mic2_samples`：MIC2，先进入统计和后续近端门禁准备。
  - `s_audio_ref_samples`：slot2 hardware ref candidate，只作为候选健康信号。
- 新增 `CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE` 默认值为 `0`。
- 默认不再把 slot2 传入 `nuwax_live_aec_process_with_ref()`，而是走 `nuwax_live_aec_process()`，由已有 software ref 路径承担主 AEC 参考。
- 采集结束 summary 增加：
  - `slot2_ref_candidates`
  - `mic2_speech_like`
  - `slot2_aec_ref_enabled`
  - `hw_ref_frames`

专业判断：
- 在 slot2 健康检测不过或尚未证明可靠前，硬件 ref 不应参与主 AEC，否则会把错误参考引入 AEC 判决，导致设备播报残留、误开门禁或压制真人近端声音。
- MIC2 现在先作为旁路观测进入主循环，后续 S5 再做双麦能量差、相关性、与 playback ref 的关系判断；当前阶段不直接改变上传门禁，避免同时引入太多变量。
- software ref 已由 speaker 写出路径提供，是当前硬件条件下最可控的 AEC 参考源，后续优化重点应转向 delay 对齐、参考增益、双麦近端证据和 during-playback preroll。

构建记录：
- 构建目录：`C:\tmp\nuwax_build_audio_ab`
- 结果：成功。
- 固件文件：`C:\tmp\nuwax_build_audio_ab\NuwaxAIPartner.bin`
- 固件大小：`2754144` 字节，`0x2a0660`。
- 新增致命错误：无。
- 既有警告：P5/P3 helper unused、wake greeting helper 等，非本次主链路接入导致。

进度：
- S3：主链路接入完成，待烧录验证。
- S4：software ref 已成为默认主 AEC 路径，待通过无人声/真人 during-playback 测试验证。

下一步：
- 烧录当前 `C:\tmp\nuwax_build_audio_ab` 固件到 COM8。
- 先运行 no-human 真实播报测试，验证播放期不应稳定误上传。
- no-human 通过后再通知真人参与 during-playback 说话验收。

### 2026-07-07 / 006-4 / 烧录与 no-human 播报门禁验证

烧录记录：
- 端口：COM8。
- 构建目录：`C:\tmp\nuwax_build_audio_ab`。
- 固件大小：`0x2a0660`。
- 写入结果：bootloader、partition table、ota data、app 全部写入并 hash 校验通过。
- 复位结果：`Hard resetting via RTS pin... Done`。

no-human 测试记录：
- 测试脚本：`tools\run-real-agent-aec-test.ps1`。
- RequestId：`real_agent_playback_gate_nohuman_20260707_s3_triread_softref`。
- 串口日志：`runtime-logs\nuwax7-serial-20260707-145454.log`。
- 测试方式：测试网关下发文本请求，设备播放智能体回复；测试期间不做人声输入。

关键日志：
- `I2S microphone prelisten start mode=during_playback ... upload_gate=during_playback_nearend`
- `live AEC ready ... external_ref=0 ... delay_samples=240`
- `I2S microphone stream done codec=pcm16 frames=0 queued=0 sent=0 ... slot2_ref_candidates=2889 mic2_speech_like=331 slot2_aec_ref_enabled=0 hw_ref_frames=0 ... playback_gate_candidates=107 playback_gate_suppressed=628 playback_gate_open_frame=0 playback_gate_max_streak=7 playback_gate_ref_unhealthy=363`
- `during-playback prelisten ended without nearend upload candidates=107 suppressed=628 max_streak=7 ref_unhealthy=363`
- `conversation metrics done ... uploaded_frames=0 played_frames=766`
- 未发现 `conversation speech detected`。
- 未发现 `asr_result`。

判断：
- no-human 播报门禁通过：设备播放自身声音时没有稳定误打开上传，最终 `frames=0 / queued=0 / sent=0 / uploaded_frames=0`。
- slot2 禁用生效：`slot2_aec_ref_enabled=0` 且 `hw_ref_frames=0`。
- 三路读取生效：`slot2_ref_candidates=2889`，说明主循环确实持续拿到了 slot2 候选帧。
- MIC2 已进入观测链路：`mic2_speech_like=331`。这同时说明 MIC2 在设备播报/环境声下也会达到单麦能量型 speech-like 阈值，后续 S5 不能把 MIC2 单独作为近端人声依据，必须结合双麦差分、MIC1/MIC2 与 software ref 的相关性、AEC 残差连续性和最小持续时间。
- `playback_gate_max_streak=7`，按 20ms frame 约 140ms，低于打开上传所需连续时间，因此没有误触发。
- `playback_gate_ref_unhealthy=363` 和 `ref_missing=365` 表明 software ref 对齐/供给仍有明显不连续或参考不足，后续 S4 的重点是 delay/ref FIFO/播放参考增益与时序对齐，而不是恢复 slot2。

进度：
- S3：烧录验证完成。
- S4：no-human 基线通过；待真人 during-playback 验收。

下一步：
- 进入真人 during-playback 测试前需要人工配合说话。
- 测试提示：听到设备开始播报后立刻说话，并持续 2 秒以上；建议说固定短句，例如“你好，今天天气怎么样，请帮我推荐出门安排”。

### 2026-07-07 / 006-5 / human 播报期验收失败与门禁修正

human 测试记录：
- RequestId：`real_agent_playback_gate_human_20260707_s3_triread_softref`。
- 串口日志：`runtime-logs\nuwax7-serial-20260707-150351.log`。
- 测试方式：设备播放测试网关智能体回复，真人在播放期间说话。

关键结果：
- `conversation metrics done ... uploaded_frames=0`
- `I2S microphone stream done ... frames=0 queued=0 sent=0 ... slot2_ref_candidates=2871 mic2_speech_like=901 slot2_aec_ref_enabled=0 hw_ref_frames=0 ... playback_gate_candidates=299 playback_gate_suppressed=1494 playback_gate_open_frame=0 playback_gate_max_streak=11 playback_gate_ref_unhealthy=750`
- `during-playback prelisten ended without nearend upload candidates=299 suppressed=1494 max_streak=11 ref_unhealthy=750`
- 未发现 `conversation speech detected`。
- 未发现 `asr_result`。

失败判断：
- 设备不是完全采不到声音，日志里出现多段较高 `mic_avg/mic_peak` 和 AEC 后输出能量。
- 失败主因是播放期门禁过保守：
  - `during_playback_nearend_candidate()` 在 ref 不健康时直接返回 false。
  - 当前 software ref 仍存在 `ref_missing/ref_unhealthy` 间歇缺口，导致真人高能量帧也被硬拦截。
  - 现有播放期最小连续近端时间是 300ms；本轮 human 最大连续候选 11 帧，约 220ms；no-human 最大连续候选 7 帧，约 140ms。
- 因此本轮应先做低风险放宽，而不是启用 slot2。

修正内容：
- `NUWAX_DURING_PLAYBACK_NEAREND_MIN_SPEECH_MS`：300ms -> 200ms。
- `NUWAX_AUTO_FOLLOWUP_PREROLL_FRAMES`：12 帧 -> 24 帧，即 240ms -> 480ms。
- `during_playback_nearend_candidate()` 改为：
  - ref 健康时继续使用原有 AEC 后输出 + 比例 + 高相关抑制逻辑。
  - ref 不健康时不再一律拒绝；仅允许 AEC 后输出达到 strong energy、输出/原始比例足够、并且与参考低相关的帧成为候选。

风险控制：
- 不启用 slot2 主 AEC ref。
- 不用 MIC2 单独打开上传，因为 no-human 也有 `mic2_speech_like`。
- no-human 需重新跑，确认放宽后不会误上传。

构建记录：
- 构建目录：`C:\tmp\nuwax_build_audio_ab`。
- 结果：成功。
- 固件大小：`0x2a0750`。
- 新增致命错误：无。
- 警告仍为既有 unused helper / wake greeting 等。

下一步：
- 烧录 `0x2a0750` 固件到 COM8。
- 重新跑 no-human 播报测试。
- no-human 通过后，再通知真人重测 human during-playback。

### 2026-07-07 / 006-6 / 门禁回收与播放后误触发修正

问题复盘：
- `0x2a0750` 版本将 ref 不健康 fallback、200ms 门禁、24 帧 preroll 同时启用。
- no-human 复测 `real_agent_playback_gate_nohuman_20260707_s4_200ms_preroll24` 中出现：
  - 播放期误判 `local VAD speech confirmed ... playback_gate=1`。
  - `audio send queue start failed: ESP_ERR_NO_MEM`，during-playback 音频会话启动但 0 帧上传。
  - 随后 auto-followup 又误上传 103 帧，ASR 返回 `no_speech=true`。
- 判断：该版本放宽过头，不可进入 human 复测。

回收修正：
- 恢复 `NUWAX_AUTO_FOLLOWUP_PREROLL_FRAMES`：24 -> 12。
- 撤销 ref 不健康 fallback：ref 不健康仍直接拒绝播放期近端候选。
- 保留 `NUWAX_DURING_PLAYBACK_NEAREND_MIN_SPEECH_MS=200`，利用上一轮统计中 human 最大连续 11 帧、no-human 最大连续 7 帧的差异。

第二个问题：
- `real_agent_playback_gate_nohuman_20260707_s4_200ms_refstrict` 中，播放期不再因 ref fallback 误开，但播放结束后仍处于 `during_playback` 预听窗口时退回普通 VAD，环境声触发上传 71 帧，ASR `no_speech=true`。
- 判断：`during_playback_mode && playback_active == false` 不应直接使用普通低阈值 VAD，否则播放后的环境声会误上传。

播放后门禁修正：
- 新增播放后专用门槛：
  - `NUWAX_DURING_PLAYBACK_POST_MIN_AVG_ABS=900`
  - `NUWAX_DURING_PLAYBACK_POST_MIN_PEAK=2500`
  - `NUWAX_DURING_PLAYBACK_POST_MIN_SPEECH_MS=300`
- 在 `during_playback_mode && !playback_active` 时使用播放后专用门槛。
- 播放中仍使用 ref-strict 的播放期近端门禁。
- 普通唤醒/auto_followup 模式仍使用原本本地 VAD。

构建与烧录：
- 构建目录：`C:\tmp\nuwax_build_audio_ab`。
- 固件大小：`0x2a07a0`。
- 构建结果：成功。
- 烧录端口：COM8。
- 烧录结果：写入和 hash 校验通过。

no-human 复测结果：
- RequestId：`real_agent_playback_gate_nohuman_20260707_s4_postgate`。
- 串口日志：`runtime-logs\nuwax7-serial-20260707-152037.log`。
- 关键结果：
  - `conversation metrics done ... uploaded_frames=0`
  - `conversation prelisten expired mode=during_playback`
  - `I2S microphone stream done ... frames=0 queued=0 sent=0 ... slot2_ref_candidates=2892 mic2_speech_like=322 slot2_aec_ref_enabled=0 hw_ref_frames=0 ... playback_gate_candidates=99 playback_gate_suppressed=636 playback_gate_open_frame=0 playback_gate_max_streak=6 playback_gate_ref_unhealthy=359`
  - `during-playback prelisten ended without nearend upload candidates=99 suppressed=636 max_streak=6 ref_unhealthy=359`
  - 未发现 `conversation speech detected`。
  - 未发现有效 `audio_session` 上传。
  - 未发现 `asr_result`。

判断：
- `0x2a07a0` no-human 通过。
- 播放后专用门槛成功抑制了上一轮播放结束后的环境声误上传。
- 当前可进入 human during-playback 复测。

下一步：
- 通知真人准备。
- human 复测重点：
  - 是否出现 `conversation speech detected mode=during_playback`。
  - 是否不再出现 `ESP_ERR_NO_MEM`。
  - 是否产生有效 `audio_session` 帧。
  - 云端保存音频中真人声音是否可辨识，设备播报残留是否可接受。

### 2026-07-07 / 006-7 / human 复测未开门

human 复测记录：
- RequestId：`real_agent_playback_gate_human_20260707_s4_postgate`。
- 串口日志：`runtime-logs\nuwax7-serial-20260707-152528.log`。
- 固件：`0x2a07a0`。

关键结果：
- 未发现 `conversation speech detected`。
- 未发现有效 `audio_session` 上传。
- 未发现 `asr_result`。
- 未发现 `ESP_ERR_NO_MEM`。
- `conversation metrics done ... uploaded_frames=0`
- `conversation prelisten expired mode=during_playback`
- `I2S microphone stream done ... frames=0 queued=0 sent=0 ... slot2_ref_candidates=2903 mic2_speech_like=447 slot2_aec_ref_enabled=0 hw_ref_frames=0 ... playback_gate_candidates=93 playback_gate_suppressed=588 playback_gate_open_frame=0 playback_gate_max_streak=5 playback_gate_ref_unhealthy=278`
- `during-playback prelisten ended without nearend upload candidates=93 suppressed=588 max_streak=5 ref_unhealthy=278`

初步判断：
- 当前版本没有误上传，也没有内存错误，但 human 复测没有打开 during-playback 上传门禁。
- 日志里早期存在若干高原始麦克风能量帧，例如 `mic_avg=3871 mic_peak=13714`，但 AEC 后输出/清洗后门禁候选不连续，最大连续候选只有 5 帧，低于 200ms。
- 如果本轮真人确实持续说话 2 秒以上，则说明 ref-strict 仍过保守，需要进入下一版：引入 MIC2 辅助近端证据或滑动窗口候选累计，不能继续只依赖 AEC 后输出连续性。
- 如果本轮真人没有说话或错过窗口，则应先重测，不应基于该日志继续放宽门禁。

待确认：
- 真人是否在设备播报期间持续说话 2 秒以上。

### 2026-07-07 / 006-8 / S5 双 MIC 影子特征与播放期候选积分

目标：
- 不恢复 slot2 硬件 ref，继续以 software ref 作为主 AEC 参考。
- 不再只依赖 AEC 后输出“连续命中”打开播放期上传门禁，避免 AEC 短时压制真人声音导致无法开门。
- 引入 MIC2 作为辅助证据，但先保持保守：MIC2 不单独开门，必须与 MIC1 原始能量、AEC 后输出能量、双 MIC 差分共同满足条件。

实现计划：
- 在主采集循环每帧计算双 MIC 特征：
  - MIC1/MIC2 平均能量。
  - MIC1-MIC2 平均差分与峰值差分。
  - 双 MIC 能量差分比例。
  - 双 MIC 能量平衡比例。
- 播放期门禁增加候选积分：
  - AEC/ref-strict 候选给较高积分。
  - MIC2 辅助候选给较低积分。
  - 非候选帧扣分，允许短缺口但不允许稀疏噪声长期累计。
- 日志 summary 增加：
  - dual_mic_voice_like
  - playback_gate_dual_candidates
  - playback_gate_score_max
  - dual_mic_diff/balance 统计

风险控制：
- 不直接上传 MIC2。
- 不用 MIC2 单路能量开门。
- no-human 必须先通过：播放期设备播报不应触发 `conversation speech detected` 或有效 `audio_session`。

### 2026-07-07 / 006-9 / S5 no-human 失败、回收与 shadow-only 通过

第一次实现：
- MIC2 辅助候选接入统计。
- 播放期候选积分参与开门确认。

no-human 失败记录：
- RequestId：`real_agent_playback_gate_nohuman_20260707_s5_dualmic_score`。
- 串口日志：`runtime-logs\nuwax7-serial-20260707-155341.log`。
- 结果：
  - 出现 `local VAD speech confirmed ... playback_gate=1 gate_score=10`。
  - 出现 `conversation speech detected mode=during_playback`。
  - 上传 `audio_session=audio_mracriuz_d6a7017d`，215 帧。
  - ASR 误识别设备播报为 `阳光文化。`，触发后续追问。
- 关键判断：
  - `playback_gate_dual_candidates=0`、`dual_mic_voice_like=0`，说明误触发不是 MIC2 辅助造成。
  - 误触发主因是候选积分参与开门；no-human 下零散 AEC/ref-strict 候选虽然最大连续只有 5 帧，但积分累积到 10 帧阈值后开门。

回收修正：
- 保留双 MIC 特征和积分统计。
- `playback_gate_candidate` 恢复只使用原 ref-strict AEC 候选。
- `confirm_frames` 恢复为连续 `vad_speech_frames`，积分不参与开门。
- MIC2 辅助候选暂时只作为 shadow 指标，不改变上传行为。
- 修复短读时 MIC2 尾部未补零的问题。

回收版构建与烧录：
- 构建目录：`C:\tmp\nuwax_build_audio_s5_dualmic`。
- 固件大小：`0x2a0e80`。
- 构建结果：成功。
- 烧录端口：COM8。
- 烧录结果：写入和 hash 校验通过。

shadow-only no-human 复测：
- RequestId：`real_agent_playback_gate_nohuman_20260707_s5_shadowonly`。
- 串口日志：`runtime-logs\nuwax7-serial-20260707-155746.log`。
- 关键结果：
  - `conversation metrics done ... uploaded_frames=0 played_frames=765`
  - `I2S microphone stream done ... frames=0 queued=0 sent=0 ... mic2_speech_like=307 dual_mic_voice_like=0 ... playback_gate_candidates=77 playback_gate_dual_candidates=0 playback_gate_suppressed=669 playback_gate_open_frame=0 playback_gate_max_streak=7 playback_gate_max_score=23 playback_gate_ref_unhealthy=363`
  - `during-playback prelisten ended without nearend upload candidates=77 dual_candidates=0 suppressed=669 max_streak=7 max_score=23 ref_unhealthy=363 dual_voice_like=0`
  - 未发现 `conversation speech detected`。
  - 未发现有效 `audio_session` 上传。
  - 未发现 `asr_result`。

结论：
- shadow-only 版本 no-human 通过。
- 候选积分不能直接用于开门，至少当前阈值下会把设备播报零散候选累积成误触发。
- MIC2 当前在 no-human 播放中没有触发 `dual_mic_voice_like`，可作为下一步真人测试的观察指标。
- 下一步需要真人 during-playback 测试：如果真人说话仍不开门，需根据 `dual_mic_voice_like`、`dual_diff_*`、`playback_gate_max_streak`、`playback_gate_max_score` 判断是否可设计更严格的“MIC2 + ref 质量 + AEC 输出”组合门禁。
