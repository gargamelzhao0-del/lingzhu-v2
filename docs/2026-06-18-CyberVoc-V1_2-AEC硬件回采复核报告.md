# 2026-06-18 CyberVoc V1.2 AEC 硬件回采复核报告

状态: **历史报告:2026-06-22 COM8 复测已覆盖；不得再作为硬件 AEC 放行依据**
硬件: CyberVoc V1.2,COM8
证据: `runtime-logs/aecverify-cyberkit-com8-20260618-221952-fixed.txt`
参考代码: `cyberkit-main/main/boards/CyberVoc-Board-V1_2/` + `cyberkit-main/main/audio/codecs/box_audio_codec.cc`

> 覆盖裁决(2026-06-22):同一块 COM8 板重新构建并刷录 `cyberkit-main` reference AECVERIFY 后，日志 `runtime-logs/cyberkit-aecverify-com8-20260622-163947.log` 显示全槽探测 `slot0 score=609.3`、`slot2 score=433.3`、`slot1 score=206.4`、`slot3 score=0`，最终候选为 `ES7210 slot0`，并警告当前 R slot2 未喂入最强候选。结合 7.0 health 修正后 `slot2/ref amp1k=0.9`、`passed=0`，当前只能把 `slot2/MIC3` 作为 reference candidate；硬件 AEC 不放行。

## 1. 复核背景

此前 `7.0工作记录（实时更新）.md` 顶部曾裁定“MIC3 硬件 reference 不可用,P3 改走软件 reference”。该裁定和本次 `cyberkit-main` 诊断实测冲突,因此本报告以**可跑通参考代码 + 真机串口日志**重新判定实际硬件。

本次不再只按原理图推断。原理图只说明设计意图:`ES8311 OUTP/OUTN -> RC/耦合网络 -> ADC_MIC3_P/N -> ES7210 MIC3P/MIC3N`;真实结论以固件读到的 ES7210 TDM slot 数据为准。

## 2. 测试方法

`box_audio_codec.cc` 已加入 AEC route verify 诊断:

- 强制 `input_reference_=true,input_channels_=2`。
- 正常输入路由选择 `slot0 + slot2`,即 channel mask `0x5`。
- 启动时临时打开 ES7210 4-slot probe,播放 ES8311 1kHz tone,逐 slot 计算 base/tone RMS 与 1kHz 分量。
- 若检测到播放相关 slot,恢复正常路由并判断该 slot 是否作为 AFE 的 R 通道。

短路径构建位于 `C:\tmp\ck-aec`,用于规避 Windows 长路径/空格导致的 ninja 命令行过长问题。构建产物项目名仍显示 `CyberVoc-Board-V2_0`,这是 `project_description/flasher_args` 命名残留;实际 CMake/sdkconfig 选中 `CONFIG_BOARD_TYPE_CYBERVOC` 并编译 `CyberVoc-Board-V1_2` 源码,真机日志也检测为 V1.2。

## 3. 关键实测日志

```text
I (411) CyberVoc: PCB verison V1.2
I (1471) BoxAudioCodec: input_reference: 1, input_channels: 2
I (1531) BoxAudioCodec: [AECVERIFY] normal ES7210 open: channel=4 mask=0x5 sample_rate=16000 input_reference=1 input_channels=2
I (1621) BoxAudioCodec: [AECVERIFY] self-test start: ES7210 full-slot probe, normal_mask=0x5, probe_mask=0xf, input_reference=1, input_channels=2
I (4181) BoxAudioCodec: [AECVERIFY] slot0: base_rms=122.8 tone_rms=579.3 base_1k=1.2 tone_1k=403.3 peak=1148 score=411.2
I (4181) BoxAudioCodec: [AECVERIFY] slot1: base_rms=1.9 tone_rms=285.3 base_1k=0.0 tone_1k=200.6 peak=432 score=206.2
I (4191) BoxAudioCodec: [AECVERIFY] slot2: base_rms=147.4 tone_rms=600.5 base_1k=1.4 tone_1k=418.3 peak=1191 score=426.0
I (4201) BoxAudioCodec: [AECVERIFY] slot3: base_rms=19.5 tone_rms=1.6 base_1k=0.0 tone_1k=0.0 peak=7 score=0.0
I (4211) BoxAudioCodec: [AECVERIFY] detected playback-reference candidate: ES7210 slot2. Current normal route selects slot0 and slot2; AFE treats selected channel1 as R.
I (4221) BoxAudioCodec: [AECVERIFY] verdict: current mask includes the detected reference slot as AFE R channel.
I (4281) BoxAudioCodec: [AECVERIFY] normal input route restored: channel=4 mask=0x5
```

## 4. 判读

1. **MIC3/slot2 数字侧不是死的。** 播放 1kHz 时 slot2 的 `tone_1k=418.3`,`tone_rms=600.5`,且被诊断程序选为 playback-reference candidate。
2. **当前正确路由是 slot0 + slot2。** `mask=0x5` 等于 `slot0 | slot2`;日志明确 `AFE treats selected channel1 as R`,并给出 verdict: detected reference slot 已作为 AFE R 通道。
3. **slot0/slot1 会听到播放声,但那不是硬件 reference。** slot0 是主麦,播放 tone 会经扬声器声学耦合进入麦克风;slot1 也有较弱 1kHz 分量。slot2 在没有真实物理麦的前提下读到强播放相关信号,结合原理图,应判为 ES8311 OUTP/OUTN 经模拟回采进入 ES7210 MIC3。
4. **MIC4/slot3 不可用。** slot3 tone 期间 RMS 反降、1kHz 分量为 0,符合 MIC4 未接/无参考输入的判断。
5. `AUDIO_INPUT_REFERENCE=false` 只能说明参考固件默认未启用硬件 reference,**不能再作为硬件不存在的证据**。

## 5. 历史结论(已被 2026-06-22 覆盖)

此前“硬件 AEC reference 不可用 / MIC3 死 / P3 必须走软件 reference”的顶层裁定曾在 2026-06-18 被本次实测推翻。但该推翻结论已在 2026-06-22 被同板 reference AECVERIFY 与 7.0 health 复测覆盖。

本节以下内容只作为历史线索保留，不作为当前施工放行依据。当前有效产品口径为:

- **slot2/MIC3 是候选 reference route:** 2026-06-18 能量证据不足以证明其干净、稳定、被 AFE R 正确使用。
- **硬件 AEC 不放行:** 必须等待 `ref_health_score` 通过并完成 AEC on/off A/B、ERLE、播放中误 VAD、近端语音保真度验收。
- **P3-1/L3/L4/DOA 暂停:** 当前只允许继续 P3-0 差异收敛，或转软件 reference/禁 L3 的降级设计。

## 6. 历史下一步裁定(已改判)

1. 仍保留 `slot0=MIC1 primary`,`slot1=MIC2 secondary`,`slot2=MIC3 reference candidate`,`slot3=MIC4 unused` 的语义冻结。
2. `audio_board_caps_t` 和 hello 可声明 `has_ref=true`,`aec_reference=es7210_slot2`,`channels=2(M,R)`，但必须分层表达为候选能力，不得被云端或端侧解释为 AEC 可用。
3. 下一步改为 P3-0 差异收敛:比较 7.0 与 reference 的 ES7210 初始化、channel mask、TDM slot、增益与 `esp_codec_dev_open()` 路径。
4. 若 ref_health 仍无法通过，产品路线切到软件 reference 或禁 L3。
5. 示波器复核可作为硬件链路排查，不作为软件侧继续推进 P3-1 的前置替代证据。
