# 006-11 AEC 延迟与双 Mic 人声保护记录

时间：2026-07-07

## 背景

真人全窗口验收反馈：

- 多次出现“像中断无声”的低能量段。
- 人声不清楚且偏小。
- 设备播报声仍较大且较完整。

串口和 WAV 复核结论：

- 上行链路没有丢帧：队列、发送、网关落 WAV 都完整。
- “中断”主要是 AEC/清洗后的低能量段，不是网络或文件截断。
- 旧配置 `CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_DELAY_SAMPLES=240` 约 15ms，明显低于 ESP-VOCAT V1.0 扬声器 DMA/codec/声学路径可能需要的延迟。
- no-human 基线里设备播报也能产生高 mic 能量，所以不能只按能量把原始 mic 回混，否则会把设备声重新放大。

## 本轮修改

1. software ref 默认延迟改为 1600 samples。
   - 文件：`sdkconfig`
   - 文件：`main/Kconfig.projbuild`
   - 1600 samples 约 100ms，当前 no-human 实测优于 240/1280。

2. acceptance 测试命令支持 `delay_samples` 覆盖。
   - 文件：`main/nuwax_ai_partner_main.c`
   - 文件：`tools/run-real-agent-aec-test.ps1`
   - 目的：后续可不重编扫延迟点，并在 manifest 中记录实际测试延迟。

3. AEC 增加双 mic 近端人声保护 hint。
   - 文件：`main/conversation/live_aec.h`
   - 文件：`main/conversation/live_aec.c`
   - 触发条件来自主循环的双 mic 空间差异，不再单纯按能量触发。
   - hint 触发时：
     - 允许强近端人声绕过 AEC 输出过低保护。
     - ref 缺失时把固定 6.25% 保留提高到 25%。
     - 仍保留 ref 相关性保护，降低把设备声回混的风险。

4. 新增日志计数。
   - `dual_mic_aec_hint`
   - `hint`
   - `strong_hint`
   - `hint_output_bypass`
   - `ref_missing_hint`

## no-human 验证

旧坏样本：

- 文件：`acceptance_fullwindow_nohuman_20260707_s2`
- WAV RMS：约 1355
- `frame_rms >= 1000`：约 16.4%

delay=1280：

- 文件：`acceptance_fullwindow_nohuman_delay1280_20260707_s1`
- WAV RMS：约 856
- `frame_rms >= 1000`：约 8.6%

delay=1600：

- 文件：`acceptance_nohuman_delay1600_20260707_s1`
- WAV RMS：约 816
- `frame_rms >= 1000`：约 9.5%

当前候选版本：delay=1600 + dual mic hint。

- 文件：`acceptance_nohuman_delay1600_dualhint_20260707_s1`
- WAV RMS：约 756
- `frame_rms >= 1000`：约 8.9%
- `frame_rms >= 2000`：约 3.6%
- `dual_mic_aec_hint=0`
- `ref_missing_hint=0`

结论：无人时没有触发人声保护，设备播报残留没有被误放大；相比旧坏样本，设备声残留量化指标明显下降。

## 下一步

需要真人全窗口验收：

1. 测试时从设备开始播报后持续说 5-8 秒。
2. 验收 WAV 中重点听：
   - 设备播报是否比旧版明显降低。
   - 人声是否比旧版清晰、连续、音量更正常。
   - 是否仍有明显电流声或低能量断续。
3. 串口重点看：
   - `dual_mic_aec_hint` 是否大于 0。
   - `hint/strong_hint/ref_missing_hint` 是否与真人说话时间段对应。
   - `ref_missing` 是否仍高；若仍高，下一步继续修 software ref 连续性，而不是继续放大 raw mic。

## 2026-07-07 S8-S10 追加记录

背景：上一轮真人反馈为“有所改善，但仍有中断；设备声音仍完整但更小；少部分人声很好，大部分人声被压住”。继续基于 software ref 路线优化，slot2 硬件 ref 仍保持禁用。

本轮代码改动：

1. 回退 `dual_mic_aec_hint` 的 confirm/hangover 实验。no-human 下 hangover 会把少量误触发扩展成连续保护，导致设备播报声被保留下来。当前策略为 `dual_mic_aec_hint_active = dual_mic_aec_hint`，不做跨帧延长。
2. 新增 `CONFIG_NUWAX_LIVE_AEC_REF_INACTIVE_ATTENUATE_Q15`，当前测试值为 `24576`（75%）。software ref 可读但低能量时不再全量 raw bypass；双麦强近端 hint 时仍完整保留。
3. 修复上行队列丢帧：`NUWAX_AUDIO_UPLINK_SEND_QUEUE_DEPTH: 6 -> 32`，`NUWAX_AUDIO_UPLINK_QUEUE_BACKPRESSURE_WAIT_MS: 20 -> 80`，`nuwax_audio_tx` task priority: `6 -> 7`。
4. 关闭能量型 raw mic 回混：`CONFIG_NUWAX_LIVE_AEC_NEAREND_RAW_MIX_Q15=0`、`CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_RAW_MIX_Q15=0`，保留 `CONFIG_NUWAX_LIVE_AEC_HINT_STRONG_RAW_MIX_Q15=8192`。原因是 no-human 日志显示仅靠 MIC1 能量会把设备播报误判成 strong nearend。

关键验证：

1. S8 `acceptance_nohuman_refinactive_guard_20260707_s1`：`sent=975 dropped=148 queue_full=37`，WAV 不可作为 AEC 判断依据。
2. S9 `acceptance_nohuman_uplink_queue_refinactive_guard_20260707_s1`：`sent=1126 dropped=0 queue_full=0`，RMS 约 845，队列修复有效但设备声泄漏仍偏高。
3. S10 `acceptance_nohuman_hint_only_rawmix_20260707_s1`：目录 `acceptance/human-audio/20260707-195428-acceptance_nohuman_hint_only_rawmix_20260707_s1`，`frames=1066 queued=1066 sent=1066 dropped=0 queue_full=0`，RMS 约 678。AEC 摘要显示 `nearend_raw_mix_q15=0`、`strong_nearend_raw_mix_q15=0`、`hint_strong_raw_mix_q15=8192`、`nearend_blend=13`。

结论：当前无真人基线优于 S6（约 697），可以进入真人播放中说话验收。真人验收必须确认 `dropped=0`；如果人声仍被压住，下一步不恢复能量型 raw mix，应改做双麦差分/双麦近端分类增强。

## 2026-07-08 S10 真人验收记录

测试请求：`acceptance_human_hint_only_rawmix_20260708_s1`

落盘目录：`acceptance/human-audio/20260708-105300-acceptance_human_hint_only_rawmix_20260708_s1`

落盘 WAV：`acceptance_human_hint_only_rawmix_20260708_s1-acc_20260708105214_acceptance_h.wav`

链路结果：

1. 网关音频会话完整结束：`frames=1086`，`duration_ms=21720`，`stored_bytes=695040`，`rms=1005`。
2. 设备上行队列无丢帧：`queued=1086 sent=1086 dropped=0 queue_full=0`。
3. WAV 波形统计无大面积全零静音；如听感仍有断续，主要来自 AEC/门禁压制后的低能量段，不是上传链路截断。

AEC/双 Mic 摘要：

1. `processed=860 bypass=8`，AEC 本体在播放窗口内确实被调用。
2. `ref_inactive_preserved=496` 偏高，说明 software ref 大量时间被判为低能量/非活跃，无法作为强 echo reference 工作。
3. `nearend_blend=34 strong_nearend_blend=34 hint=35 strong_hint=35`，真实人声保护只在少量帧触发。
4. `mic2_speech_like=641` 但 `dual_mic_voice_like=9`、`dual_mic_aec_hint=33`，说明 MIC2 有大量语音样特征，但当前双 Mic 判据过严或空间差分特征没有被充分利用。
5. `playback_gate_suppressed=977`、`playback_gate_open_frame=578`，播放期默认仍大量压制，仅在检测到近端时短暂打开。

当前判断：

1. 当前版本已经解决了“队列丢帧导致音频文件断续”的问题。
2. 当前 AEC 仍不等价于成熟硬件 ref AEC。software ref 的有效参考不足，导致设备播报声不能稳定抵消，只能靠门禁/衰减降低音量。
3. 人声被压住的主因不是 raw mix 太小，而是“近端人声识别触发少且不连续”。继续恢复 MIC1 能量型 raw mix 会把 no-human 设备播报重新放大。
4. 下一步应先做双 Mic 诊断与近端分类增强：利用 MIC1/MIC2 的差分、能量比例、相关性和时间连续性，提高真人说话期间的保护命中，同时避免 no-human 播放时误开门。

## 2026-07-08 S11 起音保护修正

用户反馈：S10 真人 WAV 中人声清晰度不错，但仍有一部分人声被压掉，特别是人声开头；设备播报声已经较低；中断减少但仍至少出现三次。

本轮代码改动：

1. `dual_mic_aec_hint` 增加 2 帧短保持：`NUWAX_DUAL_MIC_AEC_HINT_HANGOVER_FRAMES=2`。目标是补 40ms 左右的人声起音/短缺口。
2. `dual_mic.voice_like` 增加 4 帧短保持：`NUWAX_DUAL_MIC_VOICE_HINT_HANGOVER_FRAMES=4`。该分支在 S10 no-human 为 0，风险低于纯 MIC1 能量保护。
3. `audio_cleaner` 增加外部 `speech_protect_hint` 参数；当双 Mic AEC hint active 时，cleaner 的 quiet gate 不再继续压低该帧。
4. 未恢复 `CONFIG_NUWAX_LIVE_AEC_NEAREND_RAW_MIX_Q15` 和 `CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_RAW_MIX_Q15`，仍保持 0，避免 no-human 设备播报被纯能量路径放大。

构建与烧录：

1. 构建目录：`C:/tmp/nuwax_build_audio_s11_hint_hangover_cleaner`
2. 构建结果：通过，`NuwaxAIPartner.bin` 大小 `0x2a2ba0`。
3. 已烧录 COM8。

no-human 验证：

1. 请求：`acceptance_nohuman_hint_hangover_cleaner_20260708_s1`
2. WAV 目录：`acceptance/human-audio/20260708-111641-acceptance_nohuman_hint_hangover_cleaner_20260708_s1`
3. 链路完整：`frames=1091 queued=1091 sent=1091 dropped=0 queue_full=0`
4. 网关 RMS：约 `814`。相对 S10 no-human `678` 有升高，但低于更早不可接受样本。
5. AEC 摘要：`dual_mic_aec_hint=6 active=16 hangover=10`，`dual_mic_voice_like=0`，`playback_gate_open_frame=0`。

结论：短保持没有在 no-human 下触发对话上传，但设备残留量可能略升。是否保留该修正，需要真人验收判断人声开头和短中断是否明显改善。

## 2026-07-08 S11 真人验收结果

测试请求：`acceptance_human_hint_hangover_cleaner_20260708_s1`

落盘目录：`acceptance/human-audio/20260708-111906-acceptance_human_hint_hangover_cleaner_20260708_s1`

落盘 WAV：`acceptance_human_hint_hangover_cleaner_20260708_s1-acc_20260708111820_acceptance_h.wav`

链路结果：

1. 网关音频会话完整结束：`frames=1069`，`duration_ms=21380`，`stored_bytes=684160`，`rms=910`。
2. 设备上行队列无丢帧：`queued=1069 sent=1069 dropped=0 queue_full=0`。
3. WAV 分段统计无大面积全零段；如听感仍有中断，仍应归因于 AEC/门禁压制，而不是链路断流。

相对 S10 的客观变化：

1. 播放期 VAD 打开更早：`playback_gate_open_frame=430`，S10 为 `578`。
2. cleaner quiet gate 压制显著减少：`quiet_attenuated=7 playback_quiet=6`，S10 为 `39/38`。
3. 双 Mic AEC 保护触发更连续：`dual_mic_aec_hint=31 active=60 hangover=29`，S10 active 约 `33`。
4. AEC 近端回混增加：`nearend_blend=59 strong_nearend_blend=59`，S10 为 `34/34`。
5. no-human 代价：S11 no-human RMS 约 `814`，高于 S10 no-human `678`，设备声残留可能略增。

待主观验收：

1. 人声开头是否比 S10 更完整。
2. 断续次数是否减少。
3. 设备播报残留是否仍可接受。

## 2026-07-08 S11 主观反馈与根因判断

用户反馈：

1. 开头人声依然被压没。
2. 后续仍有大量人声被压没。
3. 本次没有明显中断。
4. 设备声音勉强可接受，但仍需优化。
5. 核心问题变为：人声保护不足。

根因判断：

1. S11 的短保持只解决了帧间断续，没有解决“多数人声没有被识别为近端”的问题。
2. 当前 live AEC 只处理 MIC1 + software ref；MIC2 只用于 hint，不参与输出音频生成。
3. `dual_mic_aec_hint_active=60/1069`，而用户实际说话时长远大于 60 帧；说明人声保护覆盖率严重不足。
4. `audio_cleaner speech_protected=902` 只能防止 cleaner 再次压低，不能恢复已经被 live AEC/残余消除压没的人声。
5. 当前 `CONFIG_NUWAX_LIVE_AEC_NEAREND_RAW_MIX_Q15=0`、`STRONG=0` 是为了避免 no-human 设备声泄漏；这使得没有双 Mic hint 的人声帧完全没有原始近端保底路径。
6. software ref 来自扬声器写入前的播放 PCM，不是真实空气回声；固定 `delay_samples=1600` 只能粗略对齐，不能稳定代表房间声学路径。

下一步技术方向：

1. 不再继续单纯延长 hangover 或放大 MIC1 raw mix；这会提高设备声泄漏，但仍不能精准保护人声。
2. 需要引入“双 Mic 近端分量”路径：用 MIC1/MIC2 的差分、能量比例、相关性形成近端候选音频，再与 AEC 输出混合。
3. 需要小延迟输出缓冲（约 40-80ms），在检测到人声起音后能回补前几帧，解决开头被吃掉。
4. 需要把后置 residual cancel 改为近端感知；在 nearend score 高时降低或跳过 post-blend residual，避免二次压掉人声。
5. 需要新增分段/直方图日志，按 1 秒统计 raw/aec/diff/out 能量与保护命中，用于判断人声保护覆盖率，而不是只看总计数。

## 2026-07-08 S12 双 Mic 近端分量回补

目标：不再只依赖稀疏 `dual_mic_aec_hint` 回混 MIC1 raw，而是在 live AEC 的同一 chunk 内使用 MIC1/MIC2 生成近端差分分量，按 nearend score 回补到 AEC 输出。

代码改动：

1. `nuwax_live_aec_t` 增加 `pending_mic2`，在 AEC 内部与 `pending_mic` 同步分块，避免外部回混造成时序错位。
2. 新增 `nuwax_live_aec_process_with_dual_mic()` 和 `nuwax_live_aec_process_with_ref_and_dual_mic()`。
3. Vocat 主采集路径改用双 Mic AEC API；slot2 ref 仍按当前配置禁用时走 software ref。
4. `live_aec.c` 新增双 Mic 近端恢复：
   - 估计 `mic2 -> mic1` common-mode gain；
   - 生成 `near = mic1 - gain * mic2`；
   - 根据 raw/mic2/diff/AEC suppression/hint/ref corr 计算 nearend score；
   - score 达标时以小增益回补 near component；
   - 回补后跳过 post-blend residual，避免刚补回的人声被再次消掉。
5. 新增日志：`dual_restore`、`dual_restore_hint`、`dual_restore_guard`、`dual_restore_post_skip`、`dual_score_max`、`dual_diff_*`。

构建与烧录：

1. 构建目录：`C:/tmp/nuwax_build_audio_s12_dual_restore`
2. 构建结果：通过，`NuwaxAIPartner.bin` 大小 `0x2a3840`。
3. 已烧录 COM8。

no-human 验证：

1. 请求：`acceptance_nohuman_dual_restore_20260708_s1`
2. WAV 目录：`acceptance/human-audio/20260708-114503-acceptance_nohuman_dual_restore_20260708_s1`
3. 链路完整：`frames=1102 queued=1102 sent=1102 dropped=0 queue_full=0`
4. 网关 RMS：约 `762`，低于 S11 no-human `814`，未见总能量放大。
5. 风险点：`dual_restore=592`，无真人时回补触发较多；由于回补的是双 Mic 差分分量，当前 RMS 未恶化，但仍需真人主观听感判断设备播报是否变清晰。

待真人验收：

1. 人声开头是否完整。
2. 后续人声是否仍被压没。
3. 设备声是否因差分回补变清晰或变大。

## 2026-07-08 S12 真人验收结果

测试请求：`acceptance_human_dual_restore_20260708_s1`

落盘目录：`acceptance/human-audio/20260708-120616-acceptance_human_dual_restore_20260708_s1`

落盘 WAV：`acceptance_human_dual_restore_20260708_s1-acc_20260708120530_acceptance_h.wav`

链路结果：

1. 网关音频会话完整结束：`frames=1085`，`duration_ms=21700`，`stored_bytes=694400`，`rms=867`。
2. 设备上行队列无丢帧：`queued=1085 sent=1085 dropped=0 queue_full=0`。
3. WAV 20ms 分帧统计：`silent_lt80=31`，`low_lt200=321`，`med_200_800=583`，`high_ge800=181`，说明不是链路中断造成的人声缺失。

AEC/双 Mic 摘要：

1. `processed=862 bypass=4`，live AEC 在播放窗口内正常运行。
2. `dual_restore=715`，`dual_restore_hint=188`，非 hint restore 触发过多。
3. `dual_restore_post_skip=687`，大量 restore 后跳过 post residual，导致设备播报残留容易被差分通道重新放出来。
4. `dual_diff_avg_max=4023`，`dual_diff_peak_max=17266`，真人语音时双 Mic 差分强度明显高于 no-human。
5. `dual_balance_q15_min=17311`，真人近场存在更明显的双 Mic 空间不平衡。

主观反馈：

1. 人声清晰度有改善，但开头和大量后续人声仍被压没。
2. 本轮没有明显中断。
3. 设备声勉强可接受，但仍需要优化。

根因判断：

1. S12 的核心问题不是上传链路，也不是 cleaner；链路无丢帧，cleaner 只少量压低。
2. 主要问题在 `live_aec`：双 Mic restore 太宽，很多纯设备播报帧也被恢复。
3. restore 后直接跳过 post residual，等于把已经压下去的设备声又补回一部分。
4. 下一版需要把 restore 从“能量/差分宽松触发”改为“强空间证据触发”，并且恢复后保留一层高阈值残余回声保护。

## 2026-07-08 S13 空间门禁 restore 修正

目标：保留 S12 对真人语音的差分恢复能力，但显著减少 no-human 场景下设备播报通过 restore 通道被重新放大。

代码改动：

1. `main/conversation/live_aec.h` 增加 restore 诊断计数：`dual_restore_spatial_guard`、`dual_corr_guard`、`dual_no_ref_guard`、`dual_post_residual`、`dual_balance_min`。
2. `main/conversation/live_aec.c` 提高 restore 基础差分门槛：`min_diff=260/900`。
3. 非 hint restore 新增空间门禁：
   - 普通空间近端：`balance<=22000` 且 `diff>=320/1100`。
   - 很强近端：`balance<=28000` 且 `diff_avg>=1200` 或 `diff_peak>=4200`。
4. ref 缺失或 ref 低能量时，不再允许普通非 hint restore；只有 hint 或很强近端证据才可恢复。
5. restore 后增加高阈值残余回声保护：`post_residual_corr>=22000`、`max_gain=32768` 时才做轻量抵消，避免把真人语音二次消掉。
6. `live AEC ready/levels/summary` 日志增加 S13 参数和计数，便于比较每轮验收。

构建与烧录：

1. 构建目录：`C:/tmp/nuwax_build_audio_s13_spatial_restore_guard`
2. 构建结果：通过，`NuwaxAIPartner.bin` 大小 `0x2a3c60`。
3. 已烧录 COM8。

no-human 验证：

1. 测试请求：`acceptance_nohuman_spatial_restore_guard_20260708_s1`
2. 落盘目录：`acceptance/human-audio/20260708-122539-acceptance_nohuman_spatial_restore_guard_20260708_s1`
3. 落盘 WAV：`acceptance_nohuman_spatial_restore_guard_20260708_s1-acc_20260708122453_acceptance_n.wav`
4. 链路完整：`frames=1033 queued=1033 sent=1033 dropped=0 queue_full=0`
5. 网关 RMS：`831`
6. WAV 20ms 分帧统计：`silent_lt80=20`，`low_lt200=404`，`med_200_800=502`，`high_ge800=127`，`p50=222.9`，`p90=955.9`。
7. AEC 摘要：`dual_restore=35`，`dual_restore_hint=35`，`dual_spatial_guard=86`，`dual_restore_post_skip=35`，`dual_post_residual=0`，`dual_balance_min=10338`。

结论：

1. S13 成功把 S12 no-human 的大面积 restore 收住：`dual_restore 592 -> 35`，且 35 次均来自 hint。
2. 无真人 RMS 从 S12 的约 `762` 回升到 `831`，接近 S11 的 `814`；是否可接受需要真人听感判断。
3. 当前版本适合进入真人验收：如果人声完整度改善且设备声不明显变大，则保留 S13；如果设备声变差，则进一步收紧 hint restore 或降低 hint gain。

## 2026-07-08 S13 真人验收结果

测试请求：`acceptance_human_spatial_restore_guard_20260708_s1`

落盘目录：`acceptance/human-audio/20260708-141100-acceptance_human_spatial_restore_guard_20260708_s1`

落盘 WAV：`acceptance_human_spatial_restore_guard_20260708_s1-acc_20260708141014_acceptance_h.wav`

链路结果：

1. 网关音频会话完整结束：`frames=1074`，`duration_ms=21480`，`stored_bytes=687360`，`rms=893`。
2. 设备上行队列无丢帧：`queued=1074 sent=1074 dropped=0 queue_full=0`。
3. WAV 20ms 分帧统计：`silent_lt80=21`，`low_lt200=315`，`med_200_800=561`，`high_ge800=198`，`p50=299.6`，`p90=1136.7`，`p95=1924.3`。

AEC/双 Mic 摘要：

1. `processed=859 bypass=13`，live AEC 工作正常。
2. `dual_restore=62`，`dual_restore_hint=62`，比 S12 真人 `715/188` 大幅收紧。
3. `dual_spatial_guard=150`，说明 S13 空间门禁确实挡住了大量非近场差分恢复。
4. `dual_diff_avg_max=1636`，`dual_diff_peak_max=5561`，`dual_balance_min=5173`，说明本轮真人说话近场特征明确。
5. `post_blend_residual_cancel=12`，`dual_post_residual=0`；恢复后的帧没有达到高相关残余抵消阈值。
6. `audio_cleaner speech_protected=887`，`quiet_attenuated=18`，cleaner 不是本轮人声丢失的主要来源。

待主观验收：

1. 人声开头是否比 S11/S12 更完整。
2. 后续人声是否还被大量压没。
3. 设备播报声是否因为 S13 收紧 restore 后更低，或是否仍完整可辨。
4. 是否仍出现明显中断、电流声、拖延或回音。
