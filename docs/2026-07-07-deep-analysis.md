# Nuwax AI Partner 7.0 — Deep Code Analysis (ESP32-S3 Device End)

> Scope: `Nuwax AI Partner 7.0/NuwaxAIPartner/**` (the firmware component for the
> CyberVoc V1.2 hardware). Sources are concrete files; `docs/` and other
> documentation were deliberately excluded. Paths in this document are
> `file:line` references.
>
> Generated: 2026-07-07

## 0. Build & entry

- **Project cmake**: `Nuwax AI Partner 7.0/NuwaxAIPartner/CMakeLists.txt` — sets
  project name to `NuwaxAIPartner`, version `7.0.0`, registers
  `lib/lvgl` as an extra component dir, and enables `MINIMAL_BUILD`.
  - Lines 16-20: pulls `espressif__esp-dsp` and forces `-include cmath` for
    C++ to fix std::cos/sin/atan.
- **Component cmake**: `main/CMakeLists.txt`
  - `SRCS_COMMON` (44 sources) + `SRCS_LVGL_UI` (75 sources).
  - `PRIV_REQUIRES`: esp_wifi, esp_event, esp_netif, nvs_flash, cjson, lwip,
    esp_timer, esp-tls, mbedtls, esp_app_format, esp_driver_i2s,
    esp_driver_gpio, esp_driver_spi, esp_driver_i2c, lvgl, esp-sr,
    esp_audio_codec, esp_codec_dev (`main/CMakeLists.txt:122-125`).
  - Two linker hacks: a `_ctype_` undef'd symbol for `esp-sr`'s
    `libfst.a`, and a `lv_mem_init` undef to force the custom allocator into
    the archive (`main/CMakeLists.txt:127-134`).
- **`idf_component.yml`** — version pins: cjson ^1, esp_codec_dev ~1.5, esp-sr
  ~2.3.0, esp_audio_codec ^1, lvgl ^9.5.0.
- **`.gitignore`** — keeps `build*/` directories local; the proliferation of
  `build-*` artifact directories implies dozens of tuning builds under one
  workspace.

## 1. Hardware topology (CyberVoc V1.2 / ESP32-S3-N16R8)

### 1.1 Pin map — `main/board/esp_vocat_v1_0_pins.h`

The V1.2 reference pins. Every other module relies on `NUWAX_VOCAT_*`
macros being authoritative:

| Domain   | Pins (constants)                                                                          |
| -------- | ----------------------------------------------------------------------------------------- |
| I2C      | SCL=GPIO1, SDA=GPIO2 (`15-16`); 7-bit addrs ES8311=0x18, ES7210=0x40, BMI270=0x68, BQ27220=0x55, touch 0x15/0x2A/0x2D (`17-27`) |
| I2S      | MCLK=42 (256·fs = 4.096 MHz), BCK=40, WS=39, DOUT=41→ES8311, DIN=3←ES7210 (`30-34`)        |
| PA/codec | PA_CTRL=15 (NS4150B), CODEC_PWR=48 (drive HIGH early, V1.2) (`35-36`)                     |
| Misc     | POWER_CTRL=9 (drive LOW to hold), TP_INT=10, TOUCH_WAKE=6, BOOT=0, LCD_D0=46 (straps)     |
| LCD      | QSPI: D0=46, D1=13, D2=11, D3=12, SCK=18, CS=14, RST=47, BL=44 (active-high, NOT invert)  |

### 1.2 Board profile — `main/board/nuwax_board.{c,h}`

- `nuwax_board_name()` returns `CyberVoc V1.2 (ESP32-S3-N16R8)` for
  `CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0`, else a `legacy` string
  (`board/nuwax_board.c:22-29`).
- `nuwax_board_log_banner()` prints a full I2C/LCD/Touch/Audio banner at
  boot. Audio banner prints MCLK/BCK/WS/DOUT/DIN/PA/CODEC_PWR, then
  AFE slot semantics: `slot0=MIC1 primary, slot1=MIC2 secondary,
  slot2=AEC ref candidate, slot3=MIC4 unused` and target `input_format=MR`
  (`board/nuwax_board.c:71-74`).

### 1.3 Power control — `main/board/nuwax_power_ctrl.{c,h}`

- `RTC_NOINIT_ATTR uint32_t s_boot_count; s_boot_magic` (`board/nuwax_power_ctrl.c:17-18`).
  Survives reset, so a reset-x20 test can confirm POWER_CTRL never drops the
  rail across resets. Magic: `'NWXB'`.
- If reset reason == `POWERON` or magic mismatch → counter reset, else increment.
- Two modes: `CONFIG_NUWAX_POWER_CTRL_OBSERVE` (high-Z input + level read)
  or active `HOLD_LOW/HOLD_HIGH` drive (`board/nuwax_power_ctrl.c:54-86`).

### 1.4 Shared I2C bus — `main/board/i2c_bus_manager.c`

- `i2c_master_bus_config_t` on I2C_NUM_0; SCL/SDA from pin-map; internal
  pull-ups enabled; glitch_ignore_cnt=7 (`board/i2c_bus_manager.c:36-46`).
- `nuwax_i2c_bus_get/_bus_add_device/_bus_probe/_bus_log_probe/_bus_log_full_scan`.
- On V1.2 the probe list is `touch(0x15) ES8311 ES7210 BMI270 BQ27220`
  (`board/i2c_bus_manager.c:152-160`).

### 1.5 Audio board facade — `main/board/audio_board.{c,h}` (P2-1)

Refcounted, board-agnostic facade over the codec/I2S backend. The caps
struct has the `P3-0 frozen slot semantics` plus AEC reference capability
(see full struct in file). Default V1.2 caps
(`main/board/audio_board.c:21-37`):

- codec=`NUWAX_AUDIO_CODEC_ES7210_ES8311`, profile=`esp_vocat_v1_0`,
  mic_channels=4, has_speaker=true, full_duplex=true, shared_i2s=true,
  sample_rate=16 000, has_ref=true, aec_reference=`NUWAX_AEC_REF_ES7210_SLOT2`,
  ref_slot=2, mic_primary_slot=0, input_format=`NUWAX_AFE_INPUT_MR`.

- `nuwax_audio_board_init/_deinit`: refcounted. The first init brings up
  `nuwax_vocat_audio_board_init`; last deinit tears it down
  (`audio_board.c:71-114`).
- `nuwax_audio_board_acquire_rx/_acquire_tx`: only when `shared_i2s=true`.
  Returns `ESP_ERR_INVALID_STATE` on double-acquire (the P2-1 acceptance
  test exercises this, lines 184-234).
- `nuwax_audio_board_lifecycle_selftest(cycles)` validates refcount roundtrip
  + double-acquire rejection (`audio_board.c:184-234`).

### 1.6 V1.2 backend — `main/board/vocat_audio_board.c` (1185 LoC)

The thickest board file. Pipeline:

1. **Codec analog power** — drive `NUWAX_VOCAT_CODEC_PWR_GPIO=48` HIGH
   *before* touching codecs, otherwise ES7210 front-end has no analog
   supply and returns all-zero samples (`vocat_audio_board.c:543-553`).
2. **`init_i2s_duplex`** — single `i2s_chan_config_t` with
   `CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM` / `_DMA_FRAME_NUM`,
   `auto_clear_after_cb=true` (`vocat_audio_board.c:287-296`).
   - TX: stereo STD, 16-bit samples in 32-bit slots, MCLK×256.
   - RX: TDM 4-slot, mask = SLOT0..SLOT3. Two profiles selectable:
     `P3_REF_TDM_REFERENCE_STYLE` (AUTO/AUTO/bclk_div=8) vs the
     `7.0-fixed` profile (16/4 slots, default bclk_div)
     (`vocat_audio_board.c:336-349`).
3. **`init_es7210`** — all four mics selected (TDM mode requires ≥3,
   P2-2 alignment needs 4 even though slot3 is NC), MIC selected mask
   = `SEL_MIC1|SEL_MIC2|SEL_MIC3|SEL_MIC4` (`vocat_audio_board.c:382-388`).
   - Defaults to `CONFIG_NUWAX_VOCAT_ES7210_MIC_GAIN_DB = 30` dB.
   - **Order matters**: enable → set_fs → enable → **gain** → unmute.
     Comment at `vocat_audio_board.c:412-422` documents a real bug where
     `set_gain` before `enable()` would silently leave avg_abs at noise
     floor, VAD never triggers, no uplink.
   - Same gain-restore happens again in `restart_es7210_after_i2s_ready`
     because `enable(mic_select)` rewrites the gain nibble back to default
     (`vocat_audio_board.c:434-459`).
4. **`init_es8311`** — DAC mode, `pa_pin=NUWAX_VOCAT_PA_CTRL_GPIO=15`,
   `use_mclk=true`, `pa_voltage=5.0`, `codec_dac_voltage=3.3`
   (`vocat_audio_board.c:481-491`).
   - Default volume **−6.0 dB** (line 30).
5. **`nuwax_vocat_audio_board_ref_probe`** — P2-5/P3-0 REF TDM slot
   identification: plays a loud (28 000 / 32 767 amplitude) 1kHz tone via
   ES8311 (volume raised to 0 dB), records per-slot RMS, then silences the
   tone and records baseline. REF = the slot whose `tone_rms − quiet_rms`
   is largest. **`ref_slot=-1` if none track playback**
   (`vocat_audio_board.c:634-705`).
   - Inline comment notes the RC divider attenuates heavily → need LOUD.
   - Dumps interleaved raw slots of the last frame as `[mic1 mic2 aecref nc]`
     for human inspection (`vocat_audio_board.c:686-693`).
6. **`nuwax_vocat_aec_ref_health_check`** — runs a Goertzel 1kHz
   analysis against slot2 (candidate REF) and slot0 (sanity) with
   configurable `-6dB`/`-30dB` volume (`vocat_audio_board.h:132-133`).
7. **`nuwax_vocat_audio_board_playback_stress`** — P2-3 100-cycle
   codec-start/stop-and-write for codec-managed-PA hang/brownout check
   (`vocat_audio_board.c:579-632`).

### 1.7 Slot semantics — `main/board/vocat_audio_board.h:29-34`

Hard constants `VOCAT_AUDIO_SLOT_MIC_PRIMARY=0`,
`VOCAT_AUDIO_SLOT_MIC_SECONDARY=1`, `VOCAT_AUDIO_SLOT_AEC_REF=2`,
`VOCAT_AUDIO_SLOT_UNUSED=3`. Documented rule: **"slot1 must NOT be used
as electrical reference; slot3 is ignored"** (`vocat_audio_board.h:24-25`).

## 2. Core orchestration — `main/nuwax_ai_partner_main.c`

### 2.1 Storage and queues (static globals)

Per-frame buffers (lines 135-145):

```c
static int16_t  s_audio_samples[CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME];     // 480 samples = 30ms@16k default
static int16_t  s_audio_mic2_samples[…];                                        // MIC2 (slot1)
static int16_t  s_audio_ref_samples[…];                                         // hardware AEC ref (slot2)
#if CONFIG_NUWAX_AUDIO_SOURCE_SYNTHETIC
static uint8_t  s_audio_frame[NUWAX_AUDIO_FRAME_HEADER_BYTES + MAX_SAMPLES * sizeof(int16_t)];
#endif
#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
static int16_t  s_auto_followup_preroll[12][…];                                 // 12-frame ring
static size_t   s_auto_followup_preroll_samples[12];
static bool     s_auto_followup_preroll_speech[12];
#endif
```

Queues / handles (`main/nuwax_ai_partner_main.c:151-162`):

- `s_touch_trigger` (binary semaphore)
- `s_touch_events` (FreeRTOS queue, depth 4, touch points)
- `s_shell_actions` (PSRAM-preferred, depth 8, shell results)
- `s_session_commands` (depth 8, session commands)
- `s_p3_softref_ab_requests` (depth 2, manual A/B requests)
- `s_p5_during_playback_candidate_requests` (depth 2)
- `s_wake_word_events` (depth 2)
- `s_wifi_connect_requests`, `s_wifi_scan_results`, `s_wifi_status_reports`
- `s_wifi_connect_mutex`

### 2.2 Conversation state instance

```c
static nuwax_conversation_state_t s_conversation_state =
    NUWAX_CONVERSATION_STATE_INITIALIZER(NUWAX_APP_STATE_BOOTING);
static nuwax_prelisten_state_t        s_prelisten;
static nuwax_listen_resume_state_t    s_listen_resume;
static nuwax_aec_ref_health_t         s_aec_ref_health; // zero-init: status=NO_SIGNAL, passed=false
```

### 2.3 `app_main()` — boot sequence (`app_main()` at line 4697)

The boot proceeds as a strict ordered pipeline:

1. **Log + initial state.** `log_firmware_startup_info()`, board banner,
   `nuwax_power_ctrl_init()`.
2. **Boot config line.** Logs `app_ver=7.0 ws_url=… uplink_codec=opus/pcm16
   sample_rate=16000 frame_ms=… max_frame_samples=… speaker=… wake=…`.
3. **NVS.** `nvs_flash_init`, erase if `NO_FREE_PAGES`/`NEW_VERSION_FOUND`.
4. **Speaker subsystem early** (lines 4730-4745): `nuwax_speaker_playback_init`
   with callbacks wired to `speaker_callback_*` (get_state / set_state /
   on_tts_start / on_playback_done / schedule_auto_followup_after_playback /
   return_to_listening_after_cancel / on_volume_changed).
5. **Touch, wifi, LVGL, app** queues/mutexes.
6. **`nuwax_touch_gesture_set_action_queue(s_shell_actions)`** and
   `nuwax_touch_gesture_set_volume_preview_callback(nuwax_speaker_playback_preview_volume_percent)`.
7. **`nuwax_display_init()`** (skipped in P3-0S resource attribution builds).
8. **`nuwax_touch_init()`** (skipped in P2 smoke / P3-0S).
9. **`nuwax_lvgl_port_init()`** → `nuwax_app_create(lv_screen_active())` →
   `nuwax_state_bridge_update(...)` → `nuwax_device_state_set_volume(...)`
   → `nuwax_app_shell_init()`.
10. **Special modes first**, polled by `#if`:
    - **P1 hardware diag**: blocks in infinite `vTaskDelay`, runs
      `nuwax_touch_start_p1_diag_task` + `nuwax_i2c_bus_log_full_scan`.
    - **P2 audio smoke**: runs
      `nuwax_audio_board_lifecycle_selftest(5)` (P2-1), then
      `nuwax_vocat_audio_board_playback_stress(100)` (P2-3),
      `nuwax_vocat_audio_board_smoke_test` (P2), and P2-5/P3-0
      `nuwax_vocat_audio_board_ref_probe` + `nuwax_vocat_aec_ref_health_check`,
      then `nuwax_mic_input_capture_selftest(1600)`. Caches ref health in
      `s_aec_ref_health`.
11. **Wi-Fi bring-up.** `nuwax_wifi_init` → `nuwax_wifi_start_fallback_watcher`
    → `nuwax_p3_l3_init` (legacy L3 gating) → `nuwax_wifi_connect` →
    `nuwax_app_shell_set_wifi_status(...)` → `refresh_saved_wifi_list_ui`.
12. **Wake-word init** (if `CONFIG_NUWAX_WAKE_WORD_ENABLE`). On failure,
    warning but continue (touch wake remains available).
13. **Task creation** (`create_app_task`/`create_app_task_pinned` →
    `xTaskCreateStatic[PinnedToCore]` with PSRAM stack and TCB-in-internal):
    - `nuwax_shell` — pinned core 1, 4 KB, prio 3.
    - `nuwax_touch_log_task`, `nuwax_touch_gesture` (5).
    - `nuwax_wifi_scan` (8 KB, 4).
    - `nuwax_wifi_conn` (4 KB, 4).
    - `p3_softref_ab` (8 KB, 5).
    - `p5_candidate` (24 576 bytes, 5).
    - `nuwax_speaker` (6 KB, 6).
14. **Main WS loop.** The `while (true)` reconnect loop runs:
    - `recover_wifi_if_needed()` else reconnect-delay.
    - `nuwax_time_sync_once()`.
    - `nuwax_ws_init` + `nuwax_ws_connect` with `on_text = on_ws_text`,
      `on_binary = on_ws_binary`, `on_rx_activity = on_ws_rx_activity`.
    - Transport failures → `set_bridge_status(false)` + `set_state(ERROR)`;
      `ESP_ERR_INVALID_RESPONSE` (HTTP 401-class) keeps SSID, only
      `nuwax_wifi_is_connected()` triggers SSID flip via
      `nuwax_wifi_notify_cloud_unreachable()` (`main/nuwax_ai_partner_main.c:5050-5056`).
    - On connect: `set_active_ws`, `s_ws_last_rx_activity_ms = now_ms_i64()`,
      `nuwax_session_send_hello`, `nuwax_speaker_playback_apply_current_volume`,
      `enter_sleeping_conversation_state("boot_ready")`.
    - Inner `while (nuwax_ws_is_connected(&ws))`: `run_audio_source(&ws)` →
      `send_cloud_text_request(&ws)` → `wait_for_cloud_updates(&ws, 2500)` →
      heap log.
    - On disconnect: `stop_sleep_wake_word_detector`,
      `nuwax_speaker_playback_reset_runtime`,
      `nuwax_speaker_playback_deinit_output`, `recover_wifi_if_needed`,
      `set_state(WS_CONNECTING)`, `vTaskDelay(RECONNECT_DELAY_MS)`.

### 2.4 `on_ws_text` / `on_ws_binary` (lines 1202-1334)

`on_ws_text`:
- Parses via `nuwax_session_handle_text`. If `TTS_RESULT` arrives with
  `playback_start`, arms **during-playback prelisten** via
  `arm_during_playback_capture("tts_result_rx")`.
- `pong` → `ws_mark_rx_activity`.
- `set_emotion` command or text-response emotion → `apply_semantic_emotion_name`.
- Fast commands: `queue_p3_softref_ab_fast` and `queue_p5_during_playback_candidate_fast`
  consume their requests and return early (pinned to dedicated tasks).
- Otherwise enqueues to `s_session_commands`; on overflow, drops oldest then
  pushes newest.

`on_ws_binary`:
- Decodes multi-frame NA-protocol payload: `nuwax_audio_frame_peek` →
  `nuwax_audio_frame_parse_pcm16` → `nuwax_speaker_playback_handle_audio_frame`.
- Periodic log every 50 frames or any START/END flag (lines 1309-1318).
- Non-PCM16 codecs are skipped but counted.

### 2.5 `stream_microphone_audio` (2348-3130) — the central data pump

This is the single largest function in the firmware (≈ 780 lines).
Initialization:

- Reads `samples_per_frame = (16000 * frame_ms) / 1000` (typically 320
  samples per 20 ms frame).
- `max_record_samples = (16000 * MIC_MAX_RECORD_MS)/1000` (capped at 10 s
  default).
- Allocates a `nuwax_audio_uplink_t send_ctx = {0}` (handles the
  batched/Opus path), a `nuwax_audio_cleaner_t audio_cleaner`, a
  `nuwax_live_aec_t live_aec`, and optionally a `nuwax_opus_encoder_t`.
- Calls `nuwax_audio_cleaner_init(&audio_cleaner)` and `nuwax_live_aec_init(&live_aec)`.
- `wait_for_mic_start_action(ws)` blocks until a touch-wake /
  wake-word / etc. event fires (touch-loop integration with
  `s_shell_actions` and `s_wake_word_events`).
- `nuwax_mic_input_init_with_owner(&mic, NUWAX_MIC_OWNER_CONVERSATION_CAPTURE)`
  acquires the mic bus (single-owner mutex; reused by wake-word).
- `nuwax_audio_uplink_ensure_encoded_frame_capacity(...)` for Opus.
- **`auto_followup_mode` is set to `s_prelisten.capture_active`** — the key
  branching point between prelisten-buffered capture and direct capture.
- If not following-up, `start_mic_cloud_upload(...)` opens the
  `listen-start` request and starts the audio sender task.

#### 2.5.1 In-loop timing & gates (`main/nuwax_ai_partner_main.c:2540-2584`)

A playback-gate state machine at every frame:
```c
speaker_pending = nuwax_speaker_playback_has_pending();
speaker_session_active = nuwax_speaker_playback_session_active();
aec_session_gap_active = speaker_session_active
                       && !elapsed_ms(now, last_active + 1500 ms);
aec_hangover_active    = during_playback_mode
                       && !elapsed_ms(now, last_active + 700 ms);
playback_active = speaker_pending || aec_session_gap_active || aec_hangover_active;
```

#### 2.5.2 Frame I/O & AEC routing

- Vocat board path uses `nuwax_mic_input_read_vocat_mic12_ref_pcm16`,
  producing `(mic1, mic2, ref)`; falls back to `nuwax_mic_input_read_pcm16`
  when `ESP_ERR_NOT_SUPPORTED`.
- Slot2 ref candidates flow through `nuwax_live_aec_process_with_ref`
  vs the no-ref `nuwax_live_aec_process` for mono path
  (`main/nuwax_ai_partner_main.c:2588-2706`).
- **P3 L3 legacy interrupt-level chunk processing is gated only when
  live_aec is disabled** (`main/nuwax_ai_partner_main.c:2708-2710`).
- `nuwax_audio_cleaner_process(..., playback_active, ...)` applies
  software-reference echo suppression.

#### 2.5.3 Local VAD & near-end gating (`main/nuwax_ai_partner_main.c:2727-2858`)

- During normal capture: a frame is `is_speech` when
  `nuwax_mic_level_is_speech(level)`; min-speech threshold reached after
  `vad_speech_frames * frame_ms >= NUWAX_VOICE_LOCAL_VAD_MIN_SPEECH_MS`
  (600 ms during playback via `NUWAX_DURING_PLAYBACK_NEAREND_MIN_SPEECH_MS`).
- During playback, the VAD criterion switches to `during_playback_nearend_candidate(...)`
  (`main/nuwax_ai_partner_main.c:2222-2280`) which combines:
  - `last_ref_avg_abs >= 120` OR `last_ref_peak >= 500` (ref healthy).
  - `out_level.avg_abs >= 650` & `peak >= 1800` (normal energy).
  - `out_level.avg_abs >= 1100` & `peak >= 3200` (strong energy).
  - `level_ratio_at_least_q15(...)` against `raw_level` — Q15 mul-shifts
    using `NUWAX_DURING_PLAYBACK_NEAREND_MIN_RATIO_Q15 = 8192`
    (≈0.25) and `STRONG_RATIO_Q15 = 4915` (≈0.15).
  - High-corr guard: `corr_q15 >= 19661` (≥0.6) suppresses with strong energy.
  - Min speech duration `NUWAX_DURING_PLAYBACK_NEAREND_MIN_SPEECH_MS=300`.
- Endpoint: after `vad_speech_seen` and `captured_after_frame_ms >=
  CONFIG_NUWAX_VOICE_LOCAL_VAD_MIN_CAPTURE_MS` with
  `vad_silence_frames * frame_ms >= trailing_silence_ms`
  (600 ms normal vs 700 ms during playback).
- Max-duration cap at `MIC_MAX_RECORD_MS`.
- Followup-warmup guard: in `auto_followup` / `cancel_followup` modes
  under `NUWAX_PRELISTEN_VAD_WARMUP_MS=120`, `frame_is_speech = false`
  to suppress spurious early speech (lines 2760-2769).

#### 2.5.4 Auto-followup preroll

A `preroll` ring (12 frames) captures frames during prelisten. When VAD
finally triggers, the recent 12 (or 8 for `wake_word` mode) frames replay
into the upload task so the ASR sees the full utterance (lines 2183-2196,
2282-2329). The 8-vs-12 distinction is hard-coded in
`send_auto_followup_preroll`.

#### 2.5.5 Stop reasons and final state map (lines 3011-3128)

- `vad_endpoint_triggered`, `vad_no_speech_timeout`, `stop_requested`,
  `max_duration_reached`, `auto_followup_cancelled`,
  `auto_followup_replaced_by_command` are mutually tracked.
- After cleanup, a **short-audio filter**: any auto-followup whose total
  audio < `NUWAX_AUTO_FOLLOWUP_MIN_ASR_AUDIO_MS (600 ms)` flips
  `nuwax_listen_resume_state_set_short_audio`, gating downstream ASR
  failure handling.
- Final state map:
  - `auto_followup_cancelled` → `SPEAKING`.
  - `auto_followup_replaced_by_command` → keep session command.
  - `nuwax_listen_resume_state_has_short_audio` → `LISTENING`.
  - `auto_followup_sleep_after_no_speech` → `enter_sleeping_conversation_state("listen_timeout")`.
  - else, `listen_started || recorded_samples > 0` → `SPEAKING` if
    pending playback, else `THINKING`.
  - empty → `enter_sleeping_conversation_state("listen_empty")`.

#### 2.5.6 Post-stream stats line (lines 2963-3001)

The session tracer logs: frames, queued/sent/dropped frames, queue stats,
`audio_ms`, `wall_ms`, `realtime%`, slot2-ref candidates, mic2 speech-like
frames, AEC playback-gate counters, max streak, ref unhealthy,
stop/max flags.

### 2.6 Session event handling — `handle_session_event` (3226-3388)

Switch on `command->event`:

- `ASR_PARTIAL`/`ASR_FINAL`: `set_conversation_status_guarded("VOICE", "LISTENING"/"FINAL", command->text, true)`.
- `ASR_RESULT`:
  - `ok=true` → `clear_last_listen_resume()` + `("VOICE TEXT", "THINKING", text, true)`.
  - `ok=false` with short-audio filter + no-speech error
    → `resume_prelisten_after_no_speech` else `enter_sleeping_conversation_state("short_audio_no_speech")`.
  - other errors → `("ASR ERROR", "ERROR", error)` then sleep.
- `TEXT_RESPONSE`:
  - `set_emotion` from command, mark metrics.
  - If speaker pending or `tts_pending` → `nuwax_speaker_playback_prepare_for_tts("text_response")`
    then `("NUWAX", "SPEAKING", text, true)`.
  - Else `("NUWAX", "ANSWER", text, true)` then `schedule_auto_followup_after_playback`
    or `IDLE`.
  - On failure → `("CLOUD ERROR", "ERROR", error)` + `ERROR` state.
- `TTS_RESULT`:
  - `playback_start` → `arm_during_playback_capture("tts_result_start")`.
  - Failure → `("TTS ERROR", "LISTENING", error)` then fall to auto-followup
    schedule or `IDLE`.
- `PLAYBACK_BEGIN/PLAYBACK_END` → `nuwax_speaker_playback_note_playback_{begin,end}`.

### 2.7 Shell action plumbing — `s_session_dispatch_callbacks`

The callback table wires cloud commands to the corresponding static
helpers. Notable: `run_p3_ref_health`, `run_p3_softref_ab`,
`run_p3_hardref_ab`, `run_p3_nearend_candidate`,
`run_p3_interrupt_timing`, `run_p5_fullduplex_overlap_probe`,
`run_p5_during_playback_candidate`, `set_l3_interrupt_enabled`.

### 2.8 Lifecycle helpers

- `enter_sleeping_conversation_state(reason)` clears resume/auto-followup,
  releases idle output, marks `(SLEEP, WAKE, "hold to wake")` and starts
  the wake-word detector.
- `start_prelisten_window(mode, window_ms, ready_delay_ms)` releases
  idle output, stops wake, opens a prelisten window.
- `arm_during_playback_capture(reason)` skips if menu/drawer active.
- `schedule_auto_followup_after_playback()` returns false if disabled or
  prelisten already capturing.
- `return_to_listening_after_cancel` → `start_prelisten_window("cancel_followup", …)`.
- `resume_prelisten_after_no_speech` → `start_prelisten_window("auto_followup", …)`.
- `maybe_enter_idle_sleep` → if state == IDLE for ≥ `IDLE_SLEEP_MS (45 s)`,
  enters sleeping.

## 3. Audio input pipeline

### 3.1 NA audio frame header — `main/audio_frame.{c,h}`

- 22-byte header: magic `'N','A', ver=1`, codec/flags/channels, sample_rate
  (u16), frame_duration_ms (u16), sequence (u32), timestamp_ms (u32),
  payload_bytes (u16). Little-endian helpers via hand-rolled read_u16le /
  read_u32le (lines 7-17, 86-92). Codec enums:
  `NUWAX_AUDIO_CODEC_PCM16=1, NUWAX_AUDIO_CODEC_OPUS=2`.
- `nuwax_audio_frame_build` enforces payload modulo for PCM16 and
  payload length checks (lines 19-58).
- `nuwax_audio_frame_peek` validates magic + payload_len (lines 94-122).
- `nuwax_audio_frame_parse_pcm16` returns `samples` pointer inside the
  frame buffer (no copy; caller owns the buffer) (lines 125-156).

### 3.2 Opus encoder wrapper — `main/opus_encoder_wrapper.{c,h}`

- `esp_opus_enc_open` with `ESP_OPUS_ENC_APPLICATION_AUDIO`, bitrate 32 kbps,
  FEC off, DTX on, complexity=0.
- Restricted to 16 kHz / mono (`sample_rate != 16000 || channels != 1`
  returns `ESP_ERR_NOT_SUPPORTED`).
- `map_frame_duration` allows 5/10/20/40/60 ms (`ESP_OPUS_ENC_FRAME_DURATION_*`).
- `frame_samples`/`output_bytes` are queried via `esp_opus_enc_get_frame_size`,
  then surface back to `audio_uplink`. Successive frames are resized
  lazily in `nuwax_audio_uplink_ensure_encoded_frame_capacity`.

### 3.3 Audio uplink — `main/conversation/audio_uplink.{c,h}` (~485 LoC)

Two-queue producer / single-tx-task model:

- `free_slots` (PSRAM-preferred, depth `SEND_QUEUE_DEPTH=6`) and
  `ready_packets` (same size + payload slots).
- A single static `s_audio_upload_batch` of size
  `SEND_BATCH_FRAMES=4 * (HEADER + MAX_SAMPLES * 2)` ≈ 16 KiB holds the
  current batch.
- `nuwax_audio_uplink_start`: allocates PSRAM pool of
  `DEPTH * PACKET_BYTES`, frees the queue and pool on any alloc failure
  (lines 200-258).
- `audio_uplink_send_task`: 100 ms `xQueueReceive` timeout; per message
  emits a per-batch warn when send time ≥ `SEND_SLOW_WARN_MS=180`; closes
  on `send_failed` flag or `final` packet; self-suspends when finished
  (`vTaskSuspend(NULL)`).
- `audio_uplink_enqueue_batch`: `final ? FINAL_SLOT_WAIT_MS=180 : BACKPRESSURE_WAIT_MS=20`
  free-slot wait. On overflow increments `queue_full_events` and logs.
- `nuwax_audio_uplink_enqueue_frame` (line 323):
  - `ctx->batch_start_seq = seq` on first frame of batch.
  - Opus path: `nuwax_diagnostics_log_current_task_stack(TAG, "before_first_opus_encode")`
    before seq=0, logs encoded_len/frame_len after.
  - `ctx->encoded_bytes_total += encoded_len`.
  - On `batch_count >= STREAM_BATCH_FRAMES || final_frame`:
    `audio_uplink_enqueue_batch(..., final_frame)`; resets counters.
- `nuwax_audio_uplink_finish`: flushes any partial batch with `final=true`
  while `!ctx->finished`; warns every `QUEUE_FINAL_WAIT_MS=2000` while
  waiting; sleeps 20 ms between polls; returns
  `send_failed ? (send_result==ESP_OK?ESP_FAIL:send_result) : ESP_OK`.
- `nuwax_audio_uplink_destroy`: `vTaskDeleteWithCaps`, deletes both
  queues, frees pool.

### 3.4 Mic input — `main/mic_input.{c,h}` (~840 LoC)

- **Single-owner mutex** (`acquire_owner` in `mic_input.c:76-102`):
  exactly one of `UNSPECIFIED`, `WAKE_DETECTOR`, `CONVERSATION_CAPTURE`
  holds the I2S owner. The lock is allocated with a port-critical
  create-once pattern to handle concurrent first callers.

- **Vocat path** (`vocat_shared_rx=true`, lines 220-356):
  - `init`: calls `nuwax_audio_board_init()` + `nuwax_audio_board_acquire_rx(&rx)`,
    allocates a 4×frame raw buffer in PSRAM (fallback internal). Releases
    and deinit on any failure.
  - `read_pcm16` (lines 367-414): reads interleaved 4-slot TDM into
    `mic->raw_samples`, downmixes slot0 (MIC1) to mono, applies
    `apply_vocat_input_gain` (Q15 multiply), records raw stats
    (first/last/min/max/mean/centered_peak).
  - `read_vocat_mr_pcm16`: returns `(mic=slot0, ref=slot2)` separately
    (lines 504-584).
  - `read_vocat_mic12_pcm16`: `(mic1=slot0, mic2=slot1)` (lines 586-666).
  - `read_vocat_mic12_ref_pcm16`: `(mic1=slot0, mic2=slot1, ref=slot2)` — the
    variant actually used by `stream_microphone_audio` on V1.2
    (lines 668-753).
  - `deinit` releases refcount + frees raw_samples.

- **Legacy path**: builds its own `i2s_chan_config_t` (`mic_input.c:259-296`)
  + `i2s_std_config_t` from `CONFIG_NUWAX_MIC_I2S_*` pins; supports
  32-bit sample downconversion with DC block + auto-attenuate when
  `CONFIG_NUWAX_MIC_BITS_PER_SAMPLE_32=y`. `apply_vocat_input_gain`
  defaults to `CONFIG_NUWAX_VOCAT_MIC_INPUT_GAIN_Q15=32768` (unity).

- **Capture selftest** (`mic_input.c:790-`): runs the conversation
  capture path for ~100 ms, returns RMS/peak. Called by P2.

### 3.5 Wake word — `main/wake_word_detector.{c,h}`

- Selects backend by Kconfig `CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET`
  (`multinet` else `wakenet`). MultiNet path: `esp_srmodel_filter(...,
  ESP_MN_PREFIX, CONFIG_NUWAX_CUSTOM_WAKE_WORD_LANGUAGE="cn")`.
- `configure_multinet_commands` (line 169-): `esp_mn_commands_alloc` →
  `esp_mn_commands_add(1, command)` → `esp_mn_commands_update()` →
  must be `NULL` `mn_error` else `ESP_ERR_INVALID_ARG`.
- `wake_word_task` (200-335): allocates samples buffer in **internal RAM**,
  takes `NUWAX_MIC_OWNER_WAKE_DETECTOR` ownership on `mic_input_init_with_owner`,
  reads via `nuwax_mic_input_read_pcm16`. Optional periodic mic-level log
  per `LOG_INTERVAL_FRAMES` config.
  - Multinet detect: `state == ESP_MN_STATE_DETECTED` →
    `mn_result->num>0` → callback with display or command text.
  - Wakenet detect: `iface->detect(...)` with `DET_MODE_95`, then
    `iface->get_word_name(model, detected)`.
- `nuwax_wake_word_detector_init` (338-): loads model partition via
  `esp_srmodel_init`, picks `active_model`, sets detection threshold
  (Multinet only) — `(THRESHOLD_PERCENT/100.0f)` clamped to `[0.01, 0.99]`,
  then `set_det_threshold`.
- `_start` (450-): creates stack/TCB explicitly (PSRAM stack, internal TCB)
  + `xTaskCreateStatic` at priority 4 named `"nuwax_wake"`.
- `_stop` waits up to 20 ms increments for `running=false`.

### 3.6 Live AEC — `main/conversation/live_aec.{c,h}` (~860 LoC)

The on-mic software acoustic echo canceller. State (`live_aec.h:9-55`):
- `enabled`, `initialized`, `output_started`, `pending_external_ref`.
- `aec` (opaque handle to `esp_audio_codec` AEC).
- `chunk_samples`, `delay_samples`,
- Per-chunk PCM buffers: `pending_mic`, `pending_ref`, `ref_chunk`,
  `mr_chunk`, `out_chunk`, `output_fifo`.
- Counters: `processed_chunks`, `bypass_chunks`, `external_ref_chunks`,
  `external_ref_inactive_chunks`, `ref_missing_chunks`,
  `ref_missing_attenuated_chunks`, `output_underrun_frames`,
  `clipped_samples`, `nearend_blend_chunks`,
  `strong_nearend_blend_chunks`, `residual_cancel_chunks`,
  `residual_nearend_skip_chunks`,
  `post_blend_residual_cancel_chunks`,
  `nearend_blend_ref_guard_chunks`,
  `nearend_blend_output_guard_chunks`.
- Q15 correlation/gain pairs: `last_residual_corr_q15`/gain,
  `last_post_blend_residual_corr_q15`/gain, `last_mic_ref_corr_q15`/gain.
- Last-level snapshots: `last_mic/ref/out_avg_abs/peak`.

Backend selection macro chain (`live_aec.c:106-162`):
- `CONFIG_NUWAX_LIVE_AEC_ENABLE && CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE`
  selects between `esp_afe_aec.h` (when `CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE`)
  and `esp_aec.h` (legacy `esp_aec` API).
- Type / mode constants:
  - VC vs SR: `CONFIG_NUWAX_LIVE_AEC_FORCE_VC` defaults to 1, so default is VC.
  - HIGH_PERF vs LOW_COST: `CONFIG_NUWAX_P3_SOFTREF_AB_AEC_MODE_HIGH_PERF`.
- Defaults: `OUTPUT_GAIN_Q15=45056` (≈1.375x), `REF_MISSING_ATTENUATE_Q15=4096`
  (≈0.125).

Algorithm details:

- `ref_level_active` (`live_aec.c:174-`): `avg_abs ≥ 20 || peak ≥ 60`.
- `apply_output_gain` (`live_aec.c:190-`): per-sample `>15` shift and
  clip-count.
- `nearend_blend_active`: `avg_abs ≥ 300 || peak ≥ 1000`.
- `strong_nearend_active`: `avg_abs ≥ 550 || peak ≥ 1800`.
- `nearend_blend_aec_output_evidence`: output `avg_abs ≥ 300 || peak ≥ 1000`
  AND `min_out_ratio_q15` satisfied (avoids blending when AEC output is
  too small relative to raw mic).
- `apply_nearend_blend` (line 240): mixes raw mic back into AEC output with
  `raw_mix_q15` (1024 normal, 2048 strong), guarded by `mic_ref_corr_q15`
  thresholds: `> guard_corr` (`19661` normal / `22938` strong) ⇒ skip,
  `> full_corr (=9830)` ⇒ taper raw_mix down toward zero. `raw_mix_q15`
  clamped to ≤ 32768. Final mix: `out = (out*(32768-raw_mix) + mic*raw_mix) >> 15`.
- `compute_residual_metrics` (line 299): sums `out²`, `ref²`, dot;
  `corr_q15 = |dot| / √(out² · ref²) * 32768`; `gain_q15 = dot / ref²`,
  clamped to ± `max_gain_q15`.
- `apply_residual_ref_cancel` (line 341): when `corr_q15 ≥ 8192` AND
  `gain_q15 != 0` AND `!nearend || corr_q15 ≥ 18022` (nearend-skip), do
  per-sample `out -= (gain_q15 * ref) >> 15`. Increments
  `residual_cancel_chunks` (or `residual_nearend_skip_chunks`).
- `apply_post_blend_residual_cancel` (line 380): another `>` 12000
  correlation pass after the near-end blend (clamped `max_gain_q15=65536`).
- `fifo_append`/`fifo_read`/`fifo_clear` (line 411-): ring buffer of
  `chunk * 4` samples; FIFO write index = `(read + count) % cap`,
  read index = straight. When full, `fifo_append` returns false and the
  call site falls back to `fifo_clear` + retry.
- `nuwax_live_aec_init` (line 447): zero-fills struct, reads
  `output_gain_q15` from `CONFIG_NUWAX_LIVE_AEC_OUTPUT_GAIN_Q15`.
- `ensure_live_aec` (line 461): allocates **aligned (16-byte) PSRAM**
  buffers for `pending_mic`, `pending_ref` (chunk), `ref_chunk`,
  `mr_chunk` (chunk*2), `out_chunk` (chunk), and `output_fifo`
  (chunk*4). Validates `chunk_samples <= 2048`. Logs every parameter
  with a single giant `ESP_LOGI` for visibility.
- `append_bypass_chunk` (line 565): straight pass-through to FIFO.
- `append_ref_missing_chunk` (line 573): scales by
  `REF_MISSING_ATTENUATE_Q15=4096` (or zero-fill if atten≤0, or
  bypass if atten≥32768). Updates `last_out_avg_abs/peak`.
- `maybe_log_chunk_levels` (line 603): every 100 chunks (or first 3)
  logs every counter including `mic_ref_corr_q15`,
  `residual_corr_q15`, `last_post_blend_residual_corr_q15`,
  `last_mic/ref/out_avg/peak`, `output_count`.
- `process_pending_chunk` (line 641):
  1. `mic_level = nuwax_mic_level_measure(pending_mic)`,
     `last_mic_avg_abs/peak` updated.
  2. `ref_chunk` zeroed; if `use_external_ref` and `pending_ref` set,
     copy `pending_ref` → `ref_chunk` (then if `!ref_level_active` reset
     to zero and fall back to `nuwax_p3_software_ref_read_delayed(...)`).
  3. `copied < chunk/2` OR `!ref_level_active` ⇒
     `ref_missing_chunks++`; `append_ref_missing_chunk`; log + return.
  4. AFE backend path: interleave `mr_buf[i*2]=mic[i]; mr_buf[i*2+1]=ref[i]`;
     `afe_aec_process(...)` → `out_samples = out_bytes/2`.
     Legacy `aec_process(...)`: `out_samples = chunk`.
  5. `mic_ref_metrics = compute_residual_metrics(pending_mic, ref_chunk, …, 32768)`.
  6. `apply_residual_ref_cancel(out_chunk, ref_chunk, out_samples, nearend_active)`.
  7. `pre_blend_out_level = nuwax_mic_level_measure(out_chunk, out_samples)`.
  8. `apply_nearend_blend(mic, out_chunk, mic_level, pre_blend_out_level, mic_ref_metrics)`.
  9. `apply_post_blend_residual_cancel(out_chunk, ref_chunk, out_samples)`.
  10. `apply_output_gain(out_chunk, out_samples)`.
  11. `last_out_avg/peak` update; `fifo_append(out_chunk, out_samples)`
      (with fallback `fifo_clear + retry`); `processed_chunks++`;
      `maybe_log_chunk_levels`.
- `live_aec_process_internal` (line 729):
  - When `!playback_active`: clears pending, clears FIFO, returns OK (echo
    canceller is parked during silence).
  - Gating: `aec->pending_count > 0` && `pending_external_ref != use_external_ref`
    ⇒ flush pending + clear FIFO to avoid mid-stream reference-source swap.
  - Each sample fills `pending_mic` (and optionally `pending_ref`); when
    `pending_count >= chunk_samples`, processes and resets the count.
  - Release threshold (latency concealment): `if (!output_started) { if
    (output_count < sample_count) return zeros + output_underrun_frames;
    output_started = true; }`.
  - Then `fifo_read(aec, samples, sample_count)`; if short, zero-fill and
    `output_underrun_frames++`.
- Public: `nuwax_live_aec_process` (no external ref) /
  `nuwax_live_aec_process_with_ref(mic, ref, n, playback_active)`.
- `nuwax_live_aec_log_summary` dumps every counter, `output_count`,
  `clipped_samples`, levels.
- `nuwax_live_aec_deinit` calls `afe_aec_destroy`/`aec_destroy` and frees
  the six PSRAM buffers.

### 3.7 Software reference buffer — `main/p3_software_ref.{c,h}` (~320 LoC)

- `nuwax_p3_software_ref_init(sample_rate, capacity_ms)` enforces
  `capacity_ms ≥ 100` and `capacity_samples > 0`, allocates **PSRAM ring**
  (16-bit PCM, capacity in samples). Idempotent: same params returns OK
  immediately.
- `write_samples(samples, n, stride)` records RMS/peak via `sqrt(sum_sq/n)`,
  advances monotonic `write_index` (modulo capacity). Detects overruns by
  `sample_count > free_until_full`. Periodic structured log every
  `LOG_INTERVAL_MS=1000` ms when configured (with `"P3-0S-SOFTWARE-REF"`
  tag).
- `write_samples_locked` (line 68-): stride-aware: `samples[i*stride]`.
  Tracks `overwritten_samples`.
- `read_delayed(out, n, delay_samples)`: returns the buffer content that
  was at `write_index - delay_samples - n … write_index - delay_samples`,
  zero-filling leading missing samples (handled via `leading_missing`).
  When delayed read partly underruns (or older than `oldest_index`,
  i.e. wrap-around), gaps remain zero (`abs_index < oldest_index || >=
  target_end || >= write_index ⇒ continue`).
- `stats_get` exposes all counters under `mutex`.

### 3.8 Audio cleaner — `main/conversation/audio_cleaner.{c,h}` (~590 LoC)

Per-frame pre-processing applied *after* AEC. Returns
`nuwax_audio_cleaner_result_t { before, after, softref_*,
zeroed/attenuated/playback_quiet, speech_protected, softref_active,
softref_linear_cancel, softref_echo_gate, softref_nearend_guard }`.

- `update_noise_floor` (line 142): `floor = floor*15/16 + new/16`.
- `compute_softref_metrics` (line 168): `corr_q15 = |dot|/√(mic²·ref²)
  * 32768`, `gain_q15 = dot/ref²`, clamped to ± `MAX_LINEAR_GAIN_Q15=24576` (≤ 0.75).
- `process_softref_echo` (line 232): reads software ref via
  `nuwax_p3_software_ref_read_delayed(... SOFTREF_DELAY_SAMPLES=240)`;
  sets `softref_ref_active_frames` only if `ref_level.avg_abs ≥ 120 ||
  peak ≥ 350`; on `speech_protected`, increments `softref_nearend_guard_frames`
  and returns without cancel. `nearend_guard_levels = 300/1200`; bypass
  high-corr echo (`SOFTREF_GUARD_BYPASS_CORR_Q15=16056`) but only past
  `MIC_AVG_ABS=9000 || PEAK=16000`. After cancel, applies
  `SOFTREF_ATTENUATE_Q15=16384` (0.5) or `HIGH_CORR_ATTENUATE_Q15=4096` (0.125)
  via `apply_softref_attenuation`.
- `highpass_frame` (line 353): 1-pole HP IIR
  `y = x − prev_in + (prev_hp * `HIGHPASS_COEFF_Q15=32604`) >> 15`.
- `notch_100hz_frame` (line 381): Direct-form-II biquad with float
  coefficients `b0=0.9975522, b1=−1.9935663, b2=0.9975522, a1=−1.9935663,
  a2=0.9951045`.
- `nuwax_audio_cleaner_init` (line 421): if `SOFTREF_ENABLE` and `LIVE_AEC_ENABLE`
  both on, allocates a PSRAM scratch buffer of `MAX_SAMPLES_PER_FRAME=960`
  int16s (~ 2 KB) for software-ref echo path.
- `nuwax_audio_cleaner_process` (line 455):
  1. `result->before = nuwax_mic_level_measure(samples, n)`;
     `processed_frames++`.
  2. highpass → notch → recompute level `after_hpf`.
  3. `speech_now = speech_protect_level(after_hpf)`; speech hangover
     counter (`SPEECH_HANGOVER_FRAMES=12`).
  4. `process_softref_echo(...)`: if `CONFIG_NUWAX_LIVE_AEC_ENABLE` and
     `playback_active` ⇒ skip (live AEC owns echo).
  5. Quiet-frame gate: uses live-AEC conditional either
     `QUIET_AVG=90, QUIET_PEAK=420` (live on) or
     `PLAYBACK_QUIET=220/900` (live off). Dynamic floor adds
     `floor*2 + 30` (live-off only).
  6. quiet-frame handling: if `QUIET_ZERO_ENABLE=0` (default), scales by
     `QUIET_ATTENUATE_Q15=8192` (or playback attenuate `=4096`); else zero.
  7. Impulse handling: `peak ≥ 10000 && avg ≤ 260` ⇒ `samples[i] /= 4`.
  8. `result->after = nuwax_mic_level_measure(...)`.
- `nuwax_audio_cleaner_get_stats` returns a copy of `cleaner->stats`.

### 3.9 L3 interrupt level — `main/p3_afe_l3.{c,h}` (~170 LoC)

Legacy experiment, enabled by NVS flag `nuwax_l3/en` (default OFF):

- `nuwax_p3_l3_init`: read `en` once.
- `nuwax_p3_l3_set_enabled(en, persist)`:
  - `enable`: `afe_aec_create("MR", 4, AFE_TYPE_VC, AFE_MODE_LOW_COST)`
    (already "real-time" MR with type VC, low cost).
  - `disable`: `afe_aec_destroy`.
- `nuwax_p3_l3_process_chunk(samples, frames)`:
  - Increments `s_stats.f` (frames-in).
  - When disabled: `b += frames` and return.
  - When enabled: align frames into AEC chunks; per chunk:
    `nuwax_p3_software_ref_read_delayed(ref_buf, chunksize, 0)` → pad zero;
    interleave `mr_buf[i*2] = p[i]; mr_buf[i*2+1] = ref_buf[i]`;
    `afe_aec_process(s_aec_handle, s_mr_buf, s_out_buf)`;
    `memcpy(p, s_out_buf, chunksize*sizeof(int16_t))`.
  - Per ~16 000 frames, structured log "L3 stats".
- Wired only when `!nuwax_live_aec_is_enabled(&live_aec)` in the main loop.

### 3.10 P3 diagnostics & A/B harness — `main/p3_softref_aec_diag.{c,h}`

- `run_aec_ab(source, duration_ms, delay_samples, out_result)` (line 544):
  stages: heap-snapshot → softref init (or hardref label) → mic init →
  `afe_aec_create("MR", filter, TYPE, MODE)` → chunksize check → allocate
  aligned PSRAM `mic_buf, mic2_buf, ref_buf, mr_buf, out_buf`.
- Per-target-frame loop: read vocat dual-mic (or vocat-MR for hardref)
  through `nuwax_mic_input_read_vocat_mic12_pcm16` /
  `nuwax_mic_input_read_vocat_mr_pcm16`. Softref source: read
  `nuwax_p3_software_ref_read_delayed(ref_buf, chunk, delay_samples)`.
- Interleave `mr_buf[i*2]=mic[i]; mr_buf[i*2+1]=ref[i]`; process AEC.
- `accumulate_frame` & `merge_accum` (lines 168-202, 202-)
  maintain sum-of-squares for mic/ref/out, dot products,
  peak/zero/positive/negative counts, residual dot products; subset
  of frames land in `active_*` accumulators when
  `frame_ref_rms ≥ CONFIG_NUWAX_P3_SOFTREF_AB_MIN_REF_RMS`.
- All nine heap snapshots tracked: `heap_start_internal/dma`,
  `heap_after_softref`, `heap_after_mic`, `heap_after_aec`,
  `heap_after_buffers`, `heap_process_min_*`.
- `evaluate_result` writes `data_plane_ok`, `active_quality_ok`,
  `quality_ok`, `resource_ok`, `passed` (lines 351-).
- `nuwax_p3_softref_aec_ab_run` and `nuwax_p3_hardref_aec_ab_run` are the
  public entry points; gates on `CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE`
  / `CONFIG_NUWAX_P3_HARDREF_AEC_AB_ENABLE`.

### 3.11 P3 near-end candidate — `main/p3_nearend_candidate.{c,h}`

- `evaluate_candidate(r)` (line 289-): computes `gate_ok`, `quality_gate_ok`,
  `strong_nearend_ok`, `mic_ok`, `energy_ok`, `nearend_observed` from the
  AEC diagnostic field set.
- P6 classifications (one bool per shape):
  - `p6_low_ref_nearend_ok`, `p6_low_ref_normal_voice_ok`,
    `p6_high_ref_voice_ok` (kept for log/schema but not authoritative),
    `p6_mid_ref_strong_voice_ok`, `p6_normal_voice_ok`,
    `p6_mid_ref_high_erle_voice_ok` (narrow normal-voice shape).
- `p6_cancel_candidate` requires:
  - `nearend_observed`
  - `data_plane_ok`, `read_timeouts == 0`, `ref_underrun_ratio ≤ 0.05`
  - `active_corr_abs_after ≤ CONFIG_NUWAX_P3_NEAREND_MAX_CORR_AFTER_ABS`
  - `p6_cancel_discriminator_ok` (dual-mic OR any of the five single-mic
    voice shape booleans)
  - `p6_low_ref_cancel_energy_ok` OR `p6_normal_ref_cancel_energy_ok`
- `p6_record_candidate = candidate || p6_low_ref_nearend_ok ||
  p6_cancel_candidate` (any of these opens the gate to record-barge-in).
- `log_candidate_result` emits a single 100+ field structured log line
  with all numerics+booleans+thresholds (lines 467-639).
- `nuwax_p3_nearend_candidate_run(duration_ms, delay_samples, out)` is the
  cloud-callable runner; internals gate on
  `CONFIG_NUWAX_P3_NEAREND_CANDIDATE_RECORD_ENABLE`.
- `nuwax_p3_nearend_candidate_evaluate(duration_ms, delay_samples, aec,
  out)` is the pure evaluator used by host-tests / P6.

### 3.12 P5 full-duplex overlap probe — `main/conversation/p5_fullduplex_overlap_probe.{c,h}`

`nuwax_p5_fullduplex_overlap_probe_run_ex(ws, request_id,
audio_session_id, duration_ms, delay_samples, enable_live_cancel_candidate,
out_result)` (line 763-). Captures:
- `capture_frames, upload_frames, queued_frames, queue_drops,
  write_failures, read_timeouts`.
- `ref_rms, mic_rms, mic2_rms`, `dual_mic_*` correlation/level-delta/diff-rms.
- `live_cancel_candidate`, `live_candidate_elapsed_ms`, full
  `live_candidate` (p3 near-end candidate result).
- `passsed`, char `result[32]` ("pass"/"fail"), heap snapshots.

The simpler `nuwax_p5_fullduplex_overlap_probe_run(...)` returns a
correctly populated `out_result`. The `live_cancel_detector_t` drives
its own `afe_aec_create("MR", ...)`, dual-mic accumulators, and ASR
candidate cross-check via `live_evaluate_aec_result(...)` (lines
394-499).

### 3.13 P6 gray-confirm state machine — `main/p6_gray_confirm.{c,h}` (~100 LoC)

Pure-function state machine, no I/O:

```c
if      (!input->p6_record_candidate)         → NONE ("no_record_candidate")
else if (!input->data_ok)                    → NONE ("data_unhealthy")
else if (!input->resource_ok)                → NONE ("resource_unhealthy")
else if (!input->read_timeouts_ok)           → NONE ("read_timeouts_exceeded")
else if (input->confirmation_received &&  confirmation_success) → CONFIRMED_CANCEL
else if (input->confirmation_received && !confirmation_success) → REJECTED_GRAY
else if (input->timeout)                      → REJECTED_GRAY ("confirmation_timeout")
else                                          → GRAY_CANDIDATE ("awaiting_cloud_confirm")
```

`nuwax_p6_gray_confirm_kind_name` for log-friendly enum strings.
Wired through `p5_during_playback_candidate_fast_task` in main — ack_ok
considers `gray_confirm` and `gray_cancel` specially.

## 4. Audio output pipeline

### 4.1 Speaker output — `main/speaker_output.{c,h}` (~400 LoC)

- `clamp_volume_percent` caps `[0..100]`.
- `init_p3_software_ref_if_enabled` calls
  `nuwax_p3_software_ref_init(SAMPLE_RATE, CAPACITY_MS=2000)` when
  `P3_SOFTWARE_REF_ENABLE`.
- `nuwax_speaker_output_init`:
  - V1.2 path: `audio_board_init` → `audio_board_acquire_tx`; sets
    `vocat_shared_tx=true`, default `volume=80%`. Notes that creating a
    separate port-1 I2S would fail with `ESP_ERR_NO_MEM` once the shared
    codec owns DMA.
  - Legacy path: drives `SPEAKER_SD_GPIO` high, allocates its own
    I2S STD TX (`BSP_BCLK/WS/DOUT`), retries STD init up to
    `INIT_RETRIES=2` with `INIT_RETRY_DELAY_MS=20` between attempts, on
    failure enables SD GPIO to release resources.
- `set_volume_percent` stores value under `lock` (mutex).
- `_write_pcm16`:
  - Allocates `stereo_buffer` of `2*n` int16s if needed, lazily grows.
  - Scales samples by `volume_percent/100`, writes to L=R.
  - SD GPIO cleared of muted flag.
  - `i2s_channel_write(...)` with `timeout_ms`. On success, feeds the L/R
    PCM to `nuwax_p3_software_ref_write_interleaved_lr` for software-ref
    downstream use.
- `_stop` (legacy path only):
  - SD low → `i2s_channel_disable`.
  - Allocate silence (`STOP_PRELOAD_MS=20` worth of samples), call
    `i2s_channel_preload_data` to refill DMA.
  - Re-enable, `STOP_MUTE_MS=20 ms` delay.
- `_deinit`:
  - V1.2 path: `audio_board_release_tx` + `audio_board_deinit` + free
    stereo + free lock + zero struct.
  - Legacy path: SD low; `i2s_channel_disable`; `i2s_del_channel`.

### 4.2 Speaker playback — `main/conversation/speaker_playback.{c,h}` (~940 LoC)

State at `speaker_ctx_t s_speaker_ctx` (lines 46-69): `output`, queue
`packets` (PSRAM-preferred, `NUWAX_SPEAKER_QUEUE_DEPTH=512`), mutex,
callbacks, `drop_until_end`, `playback_active`, `playback_session_active`,
`drop_deadline_ms`, `dropped_frames`, `playback_generation` (turn id),
`queued_frames`, `played_frames`, `queue_dropped_frames`, `write_failures`,
`stale_playback_*`, `volume_percent`, `keep_initialized=true`,
`active_turn_id[48]`, `active_playback_id[48]`.

Per-packet struct (line 36-44) contains `generation, sequence,
sample_rate, channels, flags, sample_count, samples[VOICE_MAX_SAMPLES_PER_FRAME]`.

Key lifecycle functions:

- `speaker_lock/unlock`: take/give `s_speaker_ctx.mutex`.
- `clamp_speaker_volume_percent(int v) → [0..100] u8`.
- `load_speaker_volume_percent()` (line 106): NVS key `nuwax/spk_vol`,
  fallback `CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT=80`.
- `save_speaker_volume_percent(uint8_t)`: NVS write + `nvs_commit`.
- `speaker_get_drop_state(now_ms)`: if `drop_until_end &&
  now ≥ drop_deadline`, log + clear; return current drop.
- `speaker_note_cancelled_frame(seq, flags)`: increment counters; if
  `flags & END`, drop complete.
- `speaker_clear_queue()`: drain all packets, log count.
- `ensure_speaker_output_ready()`: lazy `nuwax_speaker_output_init` +
  apply current volume.
- `speaker_mute_idle_output(reason)`: if `keep_initialized` →
  `_stop`; else `_deinit`.
- `speaker_finish_playback_if_current`: if `generation == current
  generation`, clear `active_*` + `session_active`; call
  `callbacks->on_playback_done(played, failures)`; mute idle output; if
  `schedule_auto_followup_after_playback` returns true, leave state
  alone, else `set_state(IDLE)`.
- `speaker_enqueue_packet(generation, seq, flags, sample_rate, channels,
  samples, n)` (line 307): `control_end_marker = (n==0 && (flags &
  END))`; reject `n > MAX_SAMPLES_PER_FRAME`. `xQueueSend` with
  `QUEUE_SEND_WAIT_MS=25`; on overflow, drop oldest then retry; on
  failure increment `queue_dropped_frames`.

Operation:

- `nuwax_speaker_playback_handle_audio_frame(seq, flags, sample_rate,
  channels, samples, n)` (line 705-):
  1. `speaker_get_drop_state(now_ms)` ⇒ `speaker_note_cancelled_frame`
     if dropping; also fires `callbacks->return_to_listening_after_cancel("cancel_followup")`
     outside `NUWAX_APP_STATE_SLEEPING`.
  2. `(flags & START)` ⇒ `on_tts_start`, `speaker_clear_queue`,
     `playback_generation++` (with zero-guard → 1), reset counters,
     `frame_generation = playback_generation`, set `playback_active=true`.
  3. Otherwise, capture current `playback_generation`.
  4. `speaker_enqueue_packet(...)`.

- `nuwax_speaker_playback_task` (line 749-):
  1. `xQueueReceive(packets, &packet, portMAX_DELAY)`.
  2. `speaker_get_drop_state(now_ms)` ⇒ `speaker_note_cancelled_frame`
     + return.
  3. Empty packet + `END`: snapshot counter set under lock and call
     `speaker_finish_playback_if_current`.
  4. Otherwise, `START` flag ⇒ set `playback_active=true`; in all cases
     `callbacks->set_state(SPEAKING)`.
  5. `ensure_speaker_output_ready()`; `nuwax_speaker_output_write_pcm16(...,
     CONFIG_NUWAX_SPEAKER_WRITE_TIMEOUT_MS)`.
  6. Lock; increment `played_frames` on success else `write_failures`.
  7. `END` ⇒ `speaker_finish_playback_if_current(...)`.
  8. Non-fatal error logged; loop.

- `nuwax_speaker_playback_has_pending()` ⇒ `playback_active ||
  uxQueueMessagesWaiting > 0`.
- `nuwax_speaker_playback_session_active()` ⇒ `playback_session_active`.
- `nuwax_speaker_playback_get_stats(...)` snapshot under lock.
- `nuwax_speaker_playback_cancel_now(source)` (line 428-):
  - `drop_window_ms = had_active_playback ? 2500 : 1500`;
  - Stale `_turn_id/_playback_id` set;
  - `stale_playback_sessions_dropped++`;
  - `drop_until_end = true`, `drop_deadline_ms = now + drop_window`;
  - `playback_active = false`, `playback_session_active = false`,
    `playback_generation++` (zero-guard);
  - `nuwax_speaker_output_stop(...)` afterwards.
- `nuwax_speaker_playback_fade_cancel(source, fade_ms)` (line 488-):
  - If `!had_active_playback || fade_ms == 0` ⇒ straight `cancel_now`.
  - Else `steps = fade_ms / 40` clamped to `[2, 12]`; per step
    `volume = restore_volume * (steps-step)/steps`; `vTaskDelay(step_ms)`
    or until `!has_pending`.
  - Then `cancel_now`; restore `volume_percent`.
- `nuwax_speaker_playback_prepare_for_tts(reason)` (line 538-): if not
  pending, ensure output ready and `_stop` it for re-warm; log muted.
- `nuwax_speaker_playback_release_idle_output(reason)` (line 568-):
  `should_release = !pending && (output.initialized || tx_handle)`. If
  `keep_initialized` ⇒ `speaker_mute_idle_output`; else `_deinit` with
  `ESP_ERROR_CHECK_WITHOUT_ABORT`.
- `nuwax_speaker_playback_reset_runtime()` (line 588-): clears all
  runtime state (queue + flags + IDs).
- `nuwax_speaker_playback_note_playback_begin(turn_id, playback_id, frames, bytes)`:
  If `drop_until_end && new_playback_id != stale_playback_id` ⇒ clear
  drop window. Set `playback_session_active=true`. Log.
- `nuwax_speaker_playback_note_playback_end(turn_id, playback_id, reason)`:
  If `matched` (both IDs match active), clear `playback_session_active`.
- Volume apply/preview flow connects to `nuwax_device_state_set_volume`
  via `on_volume_changed` callback.

## 5. Session / protocol layer

### 5.1 NA protocol & command structs — `main/session_protocol.{c,h}`

- `nuwax_session_command_t` (`session_protocol.h:22-77`): four
  `has_*` bitfields (`has_command`, `has_mcp`, `has_event`, `has_pong`) plus
  type-tagged event/command fields:
  - event/ok/definite/tts_pending/playback_start/playback_end
  - sequence/at_ms/tts_frames/playback_*/playback_bytes
  - `has_volume_db/volume_percent/volume_persist` + the floats
  - `has_duration_ms/delay_samples/delay_ms`
  - `has_interrupt_enabled/interrupt_persist`
  - `has_audio_cleaner_enabled` + `audio_cleaner_enabled`
  - `run_ref_probe`
  - 12 char-buffer strings: command/mode/request_id/audio_session_id/
    turn_id/playback_id/playback_reason/provider/ssid/password/state/
    emotion/title
  - `lines[NUWAX_SESSION_STATUS_LINES=6][48]`, `text[512]`,
    `user_text[256]`, `error[128]`, `mcp_payload[512]`.
- `nuwax_session_handle_text(payload, len, command*)` parses an incoming
  JSON text frame: handles `pong`, `device_command`, `mcp`,
  `asr_partial`, `asr_final`, `asr_result`, `text_response`,
  `playback_begin`, `playback_end`, `tts_result`, `cancel_turn_ack`,
  `audio_session`, plus `input_event_ack` and generic `frames_received/
  last_rms`/`error`. Accepts camelCase variants: `audioSessionId`,
  `volumeDb`, `volumePercent`, `volumePersist`, `durationMs`,
  `delaySamples`, `delayMs`, `runRefProbe`, `audioCleanerEnabled`,
  `interruptEnabled`. Each parsed event populates an `out_command` slot.

Send helpers (each builds a `cJSON` root and calls `cJSON_PrintUnformatted`
+ `nuwax_ws_send_text`):

- `_send_hello(ws)` (line 127-): emits `{"type":"hello", "version":1,
  "device_id": CONFIG_NUWAX_DEVICE_ID, "transport":"websocket",
  "features": { audio_upload, audio_playback, mcp }, "audio_params":
  { input_format=codec_str, codec, opus, sample_rate, channels,
  mic_channels, frame_duration_ms, speaker, wake_word, vad=false,
  full_duplex, shared_i2s, hardware_profile, aec=false,
  aec_reference="es7210_slot2"/"software"/"none",
  has_ref, ref_slot, afe_input_format="MR"/"MMR"/"MM" }}`.
- `_send_ping(ws, now_ms)`, `_send_listen(ws, state, mode)`,
  `_send_listen_with_audio_session`, `_send_text_request`,
  `_send_cancel_turn(ws, request_id, turn_id, reason)`,
  `_send_pc_status_request`, `_send_status_app_request`,
  `_send_touch_event(ws, point, action)`,
  `_send_wifi_scan_result(ws, ok, message, ssids[..][33], rssis,
  authmodes, count)`, `_send_wifi_status(ws, phase, ssid, ip, ok,
  message)`, `_send_command_ack(ws, request_id, command, ok, message)`.

UTF-8 helpers (`session_protocol.c:49-115`): `utf8_safe_prefix_len`
detects half-truncated UTF-8 lead bytes via the `0xC0..0xF8` mask
sequence; `utf8_copy_trunc(dst, dst_size, src)` truncates safely;
`log_text_preview(label, text)` emits a 96-char preview log
emitted with UTF-8 boundary safety.

### 5.2 WebSocket transport — `main/ws_transport.{c,h}` (~700 LoC)

URL parser (`parse_ws_url`, line 100-):
- Recognises `ws://` (port 80) or `wss://` (port 443). Splits host /
  port / path; accepts `?key=value` and folds it under "/?" prefix
  (`path_start = slash < query ? slash : query`).

- `trycloudflare` detection (when `CONFIG_NUWAX_TLS_TRYCLOUDFLARE_CA_ENABLE`):
  Bundles the Google GTS Root R4 PEM as `NUWAX_GTS_ROOT_R4_PEM`. Any host
  ending in `.trycloudflare.com` (or exactly `trycloudflare.com`) is
  pinned to that CA via `cfg.cacert_buf` rather than the default
  `crt_bundle_attach`.

- `transport_write/read/close/has_handle/is_open` (line 156-): TLS path
  uses `esp_tls_conn_write/read`; plain path uses `send/recv`.

- `connect_tcp` (line 262-): getaddrinfo + try each addrinfo entry until
  `connect()` succeeds; sets `SO_SNDTIMEO` to `NUWAX_WS_SOCKET_TIMEOUT_MS=3000`.

- `connect_tls` (line 317-): `esp_tls_init` → `esp_tls_conn_new_sync`
  (15 s timeout). Requires `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`.

- `websocket_handshake` (line 355-): reads `esp_fill_random(16)` bytes,
  base64-encodes (`base64_encode_16`, line 81-); sends a 1 KB HTTP/1.1
  GET with `Sec-WebSocket-Key` and optional `X-Bridge-Token`; reads until
  `\r\n\r\n` and requires `101 Switching Protocols`. On any TLS
  `WANT_READ/WANT_WRITE`, 10 ms `vTaskDelay` retry.

- `send_frame_unlocked` (line 416-): builds a 14-byte header with FIN+opcode,
  client-mask, and payload length (7/16/64-bit variants). Splits into
  `NUWAX_WS_SEND_CHUNK_BYTES = SSL_OUT_CONTENT_LEN − 128` (or 1024) chunks,
  XOR-masking each against the rotating key before send.

- `send_frame` (line 460-): guarded by `s_send_lock` (binary mutex,
  10 s wait). Imposes send-order serialisation across the firmware.

- `websocket_receive_task` (line 485-):
  - Reads 2-byte header → 2/8-byte ext length → 4-byte mask (if any) → payload.
  - Frames > 16 KB are discarded to keep the malloc small.
  - Per opcode: 0x1 text / 0x2 binary / 0x8 close (terminates) /
    0x9 ping (echoes 0xA) / 0xA pong (silent). Always fires `on_rx_activity`
    first. `payload[len] = '\0'` for safety as text payload.
  - On exit: `client->connected = false; transport_close;
    rx_task_running = false; vTaskSuspend(NULL)`.

- `cleanup_rx_task`: if current != rx_task, `wait_for_rx_task_stop(1500)`
  then `vTaskDeleteWithCaps`.

- `nuwax_ws_connect(ws, config)` (line 603-): call order — url parse →
  `nuwax_ws_close` reset → `connect_tls` or `connect_tcp` →
  `websocket_handshake` → set callbacks → create `nuwax_ws_rx` task
  (priority 5, PSRAM stack `CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES=12288`).

- `nuwax_ws_send_text` (0x1) / `_send_binary` (0x2) / `_send_ping` (0x9
  "nuwax") / `_close` (sets `connected=false`, closes transport, stops
  rx task) / `_is_connected` (transport_is_open).

### 5.3 MCP server — `main/mcp_server.{c,h}` (~270 LoC)

JSON-RPC style bridge over `nuwax_ws_client_t`:

- `build_tools_list()` (line 113-): three tools.
  - `self.get_device_status` returns
    `{"state","ssid","ip","free_heap","min_free_heap"}`.
  - `self.ui.set_emotion` (`emotion` string).
  - `self.servo.head_shake` (`times` int 1..5).
- `handle_tool_call(name, arguments, status, callbacks)` (line 153-):
  emits `create_text_content(text, is_error)`; supports the three tools
  plus `unknown tool` error.
- `nuwax_mcp_handle_message(ws, payload_json, status, callbacks)`
  (line 194-):
  - Parses request, builds response `{ "type":"mcp",
    "payload": { "jsonrpc":"2.0", "id": ..., "result":{...} } }`.
  - Methods: `initialize` (returns protocolVersion=`2024-11-05`,
    serverInfo=`Nuwax AI Partner ESP32S3`, version=`7.0`),
    `tools/list`, `tools/call`. Errors: -32700 (parse),
    -32600 (invalid request), -32602 (missing name), -32601 (unknown
    method).
- `add_id` (line 21-): writes ID as number/string/null.
- `create_text_content` (line 62-): `{"type":"text","text":...}`
  inside an array of content under result.
- `send_error` uses standard JSON-RPC error form.

### 5.4 Session dispatch — `main/conversation/session_dispatch.{c,h}` (~490 LoC)

`nuwax_session_dispatch_command(ws, command, callbacks)`:
- Single dispatch entrypoint; ignores `command == NULL || !has_command`.
- Tracks `turn_id` via `callbacks->set_active_turn_id`.
- Switch on `command->command`:
  - `wifi_connect`: requires ssid; falls back to `wifi queue full` ack.
  - `wifi_scan`: ack `scanning`, kick `request_wifi_scan`.
  - `sleep_conversation`: cancel auto-followup; cancel speaker if
    pending; enter sleeping state.
  - `set_state`: validate against `parse_remote_state`; refuse idle if
    speaker pending or local active.
  - `set_emotion`: validate non-empty; calls `callbacks->set_emotion`.
  - `stop_playback`: cancel speaker, status card `("VOICE","CANCEL", text)`,
    `return_to_listening_after_cancel("cancel_followup")`.
  - `show_status`: `callbacks->show_status`.
  - `set_volume`: requires `has_volume_percent`, `0..100`,
    `persist = has_volume_persist ? volume_persist : false`. Calls
    `callbacks->apply_volume_percent`.
  - `set_audio_cleaner`: requires `has_audio_cleaner_enabled`. Calls
    `callbacks->set_audio_cleaner_enabled`.
  - `set_l3_interrupt`: defaults — `enabled` defaults true, `persist`
    defaults true. Calls `callbacks->set_l3_interrupt_enabled`.
  - `p3_ref_health`: `volume_db = has_volume_db ? volume_db : -18.0f`,
    `duration_ms = has_duration_ms ? duration_ms : 2000`. Calls
    `callbacks->run_p3_ref_health(... &passed)`. Ack ok iff `err ==
    ESP_OK && passed`.
  - `p3_softref_ab`: `delay_samples` defaults
    `NUWAX_P3_SOFTREF_AB_DEFAULT_DELAY_SAMPLES=240`; from `has_delay_ms`
    converts at `OUTPUT_SAMPLE_RATE`. Calls
    `callbacks->run_p3_softref_ab(... &ready)`.
  - `p3_hardref_ab`: hardref slot=2.
  - `p3_nearend_candidate`: same `delay_samples` derivation as softref_ab.
  - `p3_interrupt_timing`: `mode` defaults `"record_only"`.
  - `p5_fullduplex_overlap_probe`: ack ok iff
    `err == ESP_OK && passed`.
  - `p5_during_playback_candidate`: special-case for
    `gray_confirm` and `gray_cancel` modes — allow ack with `passed=true`
    or `cancel_sent=true` respectively. Default ack requires `passed=true`.
  - Fallback: `unknown command`.

### 5.5 Shell action dispatch — `main/conversation/shell_action_dispatch.{c,h}` (~80 LoC)

Tiny wiring layer:

- `clear_followup_for_shell_action`: when action is anything but
  `NONE` or `START_LISTEN`, calls `clear_auto_followup("shell_action")`.
- `nuwax_shell_action_dispatch(ws, action, callbacks)`:
  - `SCAN_WIFI` → `request_wifi_scan`.
  - `SHOW_SAVED_WIFI` → `refresh_saved_wifi_list_ui`.
  - `REFRESH_PC_STATUS` → `request_pc_status`.
  - `REFRESH_WORK_STATUS` → `request_status_app(..., "work")`.
  - `REFRESH_ISSUES_STATUS` → `request_status_app(..., "issues")`.
  - `CANCEL` → `request_cancel_turn(..., "touch_cancel")`.
  - `SET_VOLUME` → `apply_volume_percent(action->index)`.
  - `SELECT_WIFI/CONNECT_SAVED_WIFI` → `select_wifi/connect_saved_wifi`.
  - `NONE/START/STOP` no-op.
- `nuwax_shell_action_is_listen_control(action)` returns true for
  `START_LISTEN`/`STOP_LISTEN`.

### 5.6 Touch gesture — `main/conversation/touch_gesture.{c,h}` (~510 LoC)

Touch-driven UI controller:

- Thresholds: `HOLD_TO_TALK_START_MS=180`, `HOLD_TO_TALK_MOVE_LIMIT_PX=28`,
  `SCROLL_STEP_PX=56`, `DETAIL_DRAG_START_PX=18`, `HOME_SWIPE_EARLY_PX=44`.
- Drag dispatch intervals: `MENU_DRAG_INTERVAL_MS=16`,
  `DRAWER_DRAG_INTERVAL_MS=16`, `DRAWER_VOLUME_PREVIEW_MS=90`,
  `TOUCH_RELEASE_GUARD_MS=260`.
- `classify_touch(...)` (line 88-): returns
  `NUWAX_APP_SHELL_EVENT_*` based on absolute dx/dy with 50 px threshold
  and aspect ratio dominance (dx>dy → horizontal, dy>dx → vertical).
- Active session tracks `start`, `last`, `hold_blocked_logged`,
  `drag_dispatched`, `home_swipe_dispatched`, `menu_drag_active`,
  `drawer_opened_by_drag`, `drawer_panel_drag_active`,
  `drawer_volume_drag_active`, `drawer_volume_pending`,
  `drawer_volume_last_preview`, `drawer_panel_drag_start_y`,
  `drawer_panel_drag_base_y`, timestamp logs.
- Always posts events to `s_shell_actions` queue (callable from touch
  context). Hooked at boot via
  `nuwax_touch_gesture_set_action_queue(s_shell_actions)` and
  `nuwax_touch_gesture_set_volume_preview_callback(speaker_playback_preview_volume_percent_percent)`.

### 5.7 Time sync — `main/time_sync.{c,h}`

Tiny wrapper: `nuwax_time_sync_once()` performs a one-shot NTP sync
(`ntp.aliyun.com`, 12 s timeout).

### 5.8 Wi-Fi manager — `main/wifi_manager.{c,h}`

- `nuwax_wifi_init/_connect/_recover/_start_fallback_watcher/_is_connected/_notify_cloud_unreachable`
- `nuwax_wifi_scan`, `_reconnect_to`, `_get_saved_networks`,
  `_connect_saved_index`, `_get_status`.

## 6. State machines

### 6.1 Conversation state — `main/conversation/conversation_state.{c,h}`

Single `current` field with a generation counter that increments
on every transition. `idle_since_ms` set on entering IDLE, cleared
otherwise. `idle_sleep_due(now, timeout)` ⇒
`now − idle_since_ms ≥ timeout`.

### 6.2 Prelisten state — `main/conversation/prelisten_state.{c,h}`

- Window mode (`pending=true`): triggers auto-followup when
  `now ≥ ready_ms`, expires when `now ≥ deadline_ms`.
- Capture mode (`capture_active=true`): mic reading + frames enqueued
  in preroll.
- `mic_release_stops_capture`: when false, mic stays running across
  short releases (continuous listen).
- `cancel_capture` is a one-shot signal consumed by `_clear_cancel`.

### 6.3 Listen-resume — `main/conversation/listen_resume_state.{c,h}`

Two resumability flags:
- `no_speech_resumable`: armed after a long listen that ended in
  time-out (`arm_no_speech` sets `deadline_ms`).
- `short_audio_resumable`: capture total audio < 600 ms → set; ASR
  no-speech → resume, ASR successful → clear.

### 6.4 Mic-level helpers — `main/conversation/mic_level.{c,h}`

- `nuwax_mic_level_measure(samples, n)` → `{avg_abs, peak, clipped}`.
- `nuwax_mic_level_is_speech(level)`: heuristic threshold.
- `nuwax_mic_level_should_log(seq, important)` — periodic.
- `nuwax_mic_level_log(tag, level, seq)`.

### 6.5 D1 playback path — `main/conversation/d1_playback_path.c`

~50 lines referencing a `d1_playback_state` / `d1_state_*_t` enum that's
**declared via an `#include "..."` with broken macro** — scaffolding
only; the real state machine lives in `speaker_playback.c`.

## 7. UI layer — `main/nuwax_ui_v28/` and `main/nuwax_lvgl_port.c`

### 7.1 LVGL port — `main/nuwax_lvgl_port.{c,h}` (~24 KB)

- 360×360 round display; draws via `nuwax_display_*` / `display_*`.
- Internal SRAM draw buffer sized 32 KB
  (`DRAW_BUF_LINES=32`, `DISP_WIDTH=360`), trying to land in internal
  RAM with a 32 000 bytes reserve (`LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE`).
  Comment block documents the reasoning: PSRAM draw buffers made the
  animation-heavy UI render in ~88 ms/frame vs internal's single-digit
  ms.
- LVGL task pinned to core 1, 8 KB stack, priority 5, 2 ms tick
  (`#define LVGL_TASK_PRIORITY 5`, `LVGL_TICK_PERIOD_MS 2`).
- `nuwax_lvgl_port_set_pre_handler_cb(cb, user_data)` installs a hook
  that fires inside the LVGL task while the mutex is held — used by
  `app_shell.c` to drain its UI mailbox right before `lv_timer_handler()`.
- `nuwax_lvgl_port_lock(timeout_ms)` for any external LVGL API caller.
- Optional perf logging toggle `CONFIG_NUWAX_UI_PERF_LOG_ENABLE` —
  tracks `refr_*`, `render_*`, `flush_*`, `handler_*`, `over_16ms/33ms`
  counts.

### 7.2 V28 UI layering — `main/nuwax_ui_v28/nuwax_app.c`

`nuwax_app_create(lv_screen_active())`:

1. `nuwax_device_state_init` (defaults: emotion=NEUTRAL, wifi=DISCONNECTED,
   cloud=DISCONNECTED, dialog=SLEEP, power=EXTERNAL, volume=80).
2. `nuwax_theme_init` — colour palette.
3. `lv_obj_set_style_bg_color(screen, NUWAX_COLOR_BG_DARK, 0)`.
4. Layered creation (bottom-to-top):
   `face → status_bar → drawer → focus_menu → gesture`.
5. `nuwax_device_state_add_listener(state_listener, NULL)`.
6. `lv_timer_create(engine_tick_cb, 30 ms, NULL)` drives the face
   expression transition.

### 7.3 Device state + bridge — `nuwax_device_state.{c,h}`,
`nuwax_state_bridge.{c,h}`

- `nuwax_device_state_t` aggregates 25 emotion enum values,
  wifi/cloud/dialog/power enums, `volume`, `low_power`, `ssid`,
  `ip`. `nuwax_device_state_*` provides setters + an 8-entry listener
  table; every change notifies all listeners.
- The single registered `state_listener` in `nuwax_app.c`
  re-renders face, status bar, drawer, and focus menu on every
  state change.
- `nuwax_state_bridge_update(nuwax_app_state_t)` maps app_state into
  default emotion+dialog+power; `nuwax_state_bridge_set_semantic_emotion_name`
  overrides the default with a string. String→emotion mapping covers
  the 25 enum values plus catch-alls.

### 7.4 LVGL modules

- **`nuwax_face.{c,h}`** — drives the round-faced mascot
  (`face_engine`, `face_components`, `face_presets`, etc., 6 CUs).
  Ticked at 30 ms by `engine_tick_cb`.
- **`nuwax_status_bar.{c,h}`** — top status bar; rebuilds on every
  state change for wifi/cloud/power/volume icons.
- **`nuwax_drawer.{c,h}`** (~52 KB) — slide-down volume drawer
  with hit-test for volume positions.
- **`nuwax_focus_menu.{c,h}`** (~83 KB, 12 CUs: data, draw,
  style, status, wheel, etc.) — sidebar menu with 6 items:
  CLAW / WORK / ISSUES / WIFI / VOLUME / DEVICE.
- **`nuwax_gesture.{c,h}`** — gesture recognisers on the screen.
  Thresholds in header (`OPEN_MENU_DX=42`, `CLOSE_MENU_DX=-42`,
  `VERTICAL_DY=34`, `DRAWER_DY=24`, `TOP_BAND=96`, `TAP_MAX=12`,
  `VOLUME_STEP=5`).
- **`nuwax_theme.c`** — colour palette.
- **`nuwax_anim.c`** — animation helpers.
- **Asset CUs** — icons for cloud/power/wifi/volume state, dialog
  phases (sleep/listening/recognizing/thinking/speaking/error),
  menu icons (claw/work/issues/wifi/volume/device current+normal),
  face frames (a couple of 360×360 RGB565 face assets).

### 7.5 Shell bridge — `main/app_shell.c`

- Adapts `nuwax_app_shell_*` API to LVGL by translating
  shell events (wifi/cloud/volume/status card/conversation status)
  into mutations of `nuwax_device_state_t` /
  `nuwax_focus_menu_*` / `nuwax_drawer_*`.
- Drains a thread-safe UI mailbox (`portMUX_TYPE s_ui_mailbox_mux`)
  guarded by pending fields: menu scroll, drag, finish, detail adjust,
  drawer drag, finish, volume preview. Posted from touch task with
  zero allocation.
- Touch events from `nuwax_touch_gesture_task`:
  - Tap → menu adjust/detail adjust/volume touch.
  - Swipe up/down (top band, dy≥24) → drawer open/close.
  - Swipe left → next emotion (face).
  - Swipe right → open menu.
  - Press/long press → `START_LISTEN` action → queued into
    `s_shell_actions` for `wait_for_mic_start_action`.

### 7.6 Display driver — `main/display.c` (~35 KB)

QSPI driver for ST77916 (360×360). Header in `display.h` exposes
the `nuwax_display_init`, flush, region write, line draw, and
keepalive helpers (the latter re-asserts CS every 15 s, full refresh
every 120 s). LVGL flush callback uses `display_write_region_pixels`.

## 8. Misc support modules

### 8.1 `full_duplex_coord.c` (13 lines)

Stub `full_duplex_init()` returning 0. Placeholder for a future
full-duplex coordinator; not actually wired in.

### 8.2 `state_cmd_fast.c`

M1 fast command queue (volume/emotion/led/mute) using a
16-deep FreeRTOS queue. **Stale**: push API exists but **no consumer
task is started**. Code paths use
`apply_semantic_emotion_name` and
`nuwax_speaker_playback_apply_volume_percent` directly.

### 8.3 `audio_reliable_capture.c` (74 lines)

16 kHz/30 ms ring buffer (32 frames = 960 ms) scaffolding. The real
mic path goes through `mic_input.c → live_aec → audio_cleaner →
audio_uplink`.

### 8.4 `audio_reliable_playback.c` (46 lines)

24-frame stereo playback buffer scaffolding; not used.

### 8.5 `diagnostics.{c,h}`

`nuwax_diagnostics_log_heap(TAG)`, `_log_heap_label(TAG, label)`,
`_log_current_task_stack(TAG, label)`,
`_log_aec_ref_health(...)`. Used throughout `app_main` to tag
stages (`before_wifi_init`, `after_wifi_init`, `after_wake_init`,
`before_first_opus_encode`, `after_first_opus_encode`,
`p2_audio_smoke_done`, `p3-0 ref health`, …).

## 9. State / data flow summary

```
   Touch (CST816)               Wake word (esp-sr)
       │                              │
       ▼                              ▼
  touch_gesture_task            wake_word_events (queue)
       │                              │
       ▼                              ▼
  s_shell_actions (queue)  ◀───────────┘
       │
       ▼
  wait_for_mic_start_action (in stream_microphone_audio)
       │
       ▼
  nuwax_mic_input_read_vocat_mic12_ref_pcm16
       │
       ▼
  nuwax_live_aec_process_with_ref  ─── p3_software_ref (slot2 fallback)
       │
       ▼
  nuwax_audio_cleaner_process  ─── p3_software_ref (echo cancellation)
       │
       ▼
  enqueue_mic_uplink_frame ─▶ audio_uplink (send task)
                                          │
                                          ▼
                              nuwax_ws_send_binary ─── Cloud
                                              ▲
                                              │ (PONG / text)
                  on_ws_text ◀────  WebSocket rx task (ws_transport)
                                              ▲
                                              │
                              Server text protocol (session_protocol)
                                              │
                                              ▼
                          s_session_commands (queue, depth 8)
                                              │
                                              ▼
                          handle_session_event  →  set_state, set_emotion,
                                                  speaker_playback_*, ...
                          shell_action_dispatch  → wifi/scan/status/...
                          session_dispatch       → "device_command" routing

   Server binary (downlink NA-protocol frames)
       │
       ▼
  on_ws_binary ◀── ws_transport
       │
       ▼
  nuwax_speaker_playback_handle_audio_frame
       │
       ▼
  packets queue (depth 512)
       │
       ▼
  nuwax_speaker_playback_task → nuwax_speaker_output_write_pcm16
       │                              │
       │                              ▼
       │                audio_board shared TX (codec-managed I2S)
       │                              │
       │                              ▼
       │                          ES8311 → PA → speaker
       │                          └─▶ p3_software_ref interleaved feed
       ▼
   on_playback_done → state → auto-followup schedule / IDLE
```

## 10. Notable engineering / robustness observations

### 10.1 Owner mutex (mic)

Prevents wake-word and conversation capture from clobbering each
other's I2S config; aligns first-use via `portMUX_TYPE`
create-once pattern (`mic_input.c:52-101`).

### 10.2 V1.2 codec-power ordering (real-hw lesson)

GPIO 48 HIGH *before* any codec I2C, otherwise ES7210 produces zero
samples (`vocat_audio_board.c:543-553`).

### 10.3 ES7210 gain-after-enable order

Documented in source after a real-hw bug re-emerged
(`vocat_audio_board.c:412-422`); re-applied on every
`restart_es7210_after_i2s_ready` because `enable(mic_select)`
resets the gain nibble.

### 10.4 P3_REF_TDM_REFERENCE_STYLE

Single-Kconfig switch between 7.0-fixed and reference RX slot/BCLK
setup (`vocat_audio_board.c:336-349`).

### 10.5 P3-0 AFE MR guard

Deferred to P3-1; current build logs which path it took but never
creates the AFE. `s_afe_handle` and `s_afe_mr_enabled` are
zero-init globals commented as "TODO P3-1".

### 10.6 PSRAM-preferred queue/mutex creation

`create_queue_prefer_psram` at `main/nuwax_ai_partner_main.c:350-369`
for all major queues (`s_shell_actions`, `s_session_commands`, etc.).

### 10.7 Static task + PSRAM stack

Every long-lived task is `xTaskCreateStatic[PinnedToCore]` with
the stack in PSRAM and the TCB in internal RAM
(`create_app_task`, lines 1359-1407). Heap snapshots at every step.

### 10.8 State generation counter

`conversation_state_set` returns a generation; UI updates skip
stale generations (`main/nuwax_ai_partner_main.c:677-685`).

### 10.9 Long-lived UI mailbox

`app_shell` uses `portMUX_TYPE` critical section rather than a
queue, allowing zero-allocation posting from the touch task
(`app_shell.c:61-153`).

### 10.10 Activity timeout / ping loop

Every 25 s pings both protocol-layer `cjson ping` and ws-layer
`PING`. WS idle cutoff is 120 s
(`ws_check_activity_timeout`, `main/nuwax_ai_partner_main.c:2026-2039`).

### 10.11 Short-audio filter

Any auto-followup sub-600 ms capture is short-circuited via
`nuwax_listen_resume_state_set_short_audio`, keeping ASR no-speech
errors from sending the device to sleep
(`handle_session_event` short-circuit at lines 3255-3266).

### 10.12 `keep_initialized` on speaker

ES8311 handle retained across speaker-off periods
(`speaker_playback.c:71-74, 213-247`); `nuwax_speaker_output_stop`
just disables the channel.

### 10.13 Refuse remote idle while speaker active

`session_dispatch.c set_state` defers remote idle if
speaker pending OR local conversation active (lines 140-156).

### 10.14 Wake-handshake vs transport rejection

Only `nuwax_wifi_is_connected()` triggers SSID flip via
`nuwax_wifi_notify_cloud_unreachable()`. HTTP 401-class rejections
(`ESP_ERR_INVALID_RESPONSE`) keep the SSID.

### 10.15 Touch wake fallback

If `nuwax_wake_word_detector_init` fails, touch gesture remains
functional (`app_main:4976-4980`).

### 10.16 Generation-based stale packet drop

`speaker_note_cancelled_frame` clears `drop_until_end` exactly when
the END flag arrives in the new playback's `note_playback_begin`
context. Two cancellation windows: `CANCEL_DROP_GRACE_MS=1500` (idle),
`CANCEL_DROP_ACTIVE_MS=2500` (active playback).

### 10.17 Encoded frame growth policy

`nuwax_audio_uplink_ensure_encoded_frame_capacity` does
`heap_caps_realloc` first PSRAM-then-internal, with diagnostic
PSRAM counters on alloc failure.

### 10.18 Audio frame lossiness

`audio_uplink_enqueue_batch` distinguishes `final=true` (180 ms
slot wait) vs back-pressure (20 ms). On overflow, oldest dropped
once; on second failure, batch is dropped wholesale with WARN.

### 10.19 `live_aec` zero-cliff on playback_off

When `playback_active=false` at frame entry, `pending_count` is
zeroed, FIFO is cleared, and the function returns OK without
processing — the AEC is **parked during silence** to avoid
introducing residual noise.

### 10.20 AEC backend selection

`afe_aec` (preferred when `P3_SOFTWARE_REF_AEC_AB_ENABLE`) vs legacy
`esp_aec`, selected at compile time via macro fan-out
(`live_aec.c:106-162`). Both implement the same chunked interface
to the rest of the pipeline.

### 10.21 Software-reference gating

`audio_cleaner.process_softref_echo` is **a no-op when
`live_aec` is active and `playback_active`** (live AEC owns echo
suppression; `audio_cleaner.c:239-243`). The software-ref path is
the fallback for hardware-ref missing during mute/init.

### 10.22 Near-end blend raw-mix taper

When `mic_ref_corr_q15` falls between `full_corr=9830` and
`guard_corr=19661` (normal) or `guard_corr=22938` (strong),
`raw_mix_q15` is linearly tapered toward 0. With strong near-end +
high correlation, blending the raw mic would re-inject the echo.

### 10.23 State-bridge emotion precedence

`nuwax_state_bridge_update` chooses emotion via:
1. Power override (low battery/charging).
2. Semantic emotion name (string) if set by `set_emotion` command.
3. Default per app_state.

## 11. Where the spec does NOT match the code

A handful of values/constants defined but never functionally consumed:

| File / Symbol                              | Status                                                           |
| ------------------------------------------ | ---------------------------------------------------------------- |
| `state_cmd_fast.c`                         | Push API exists; **no consumer task is started.** No-op in app.  |
| `audio_reliable_capture.c`                 | Scaffolding; real path is `mic_input.c`.                         |
| `audio_reliable_playback.c`                | Scaffolding; real path is `speaker_playback.c`.                  |
| `full_duplex_coord.c`                      | `full_duplex_init()` returns 0 but isn't invoked from `app_main`. |
| `d1_playback_path.c`                       | Forward-declares `d1_playback_state` w/o defining it; unused.    |
| `nuwax_p3_l3_init / _process_chunk`        | Legacy, gated by NVS flag; bypassed when live_aec active.        |
| `p6_high_ref_strong_voice_ok`              | Code forces `false` (line 367); old shortcut kept for stability. |
| `p3_software_ref` when `live_aec` active   | Only the residual cancellation tier reads it; the main path owns echo. |
| `esp-tls` `CONFIG_NUWAX_TLS_TRYCLOUDFLARE_CA_ENABLE` | TLS CA bundle only on `*.trycloudflare.com` hosts.      |

## 12. Audio-fidelity deep-dive (summary of concrete behaviour)

End-to-end audio path observation per 20 ms frame at 16 kHz:

1. **Capture (1×TDM read)** — `i2s_channel_read` returns 4×320 int16
   samples interleaved `[mic1, mic2, aecref, nc]`.
2. **Downmix** — `apply_vocat_input_gain(mic1[i]) → mic[i]` at Q15.
3. **AEC** — `nuwax_live_aec_process_with_ref(samples, ref, n, playback_active)`:
   - Slices the frame into AEC chunks (e.g. 256/320).
   - On ref missing (< `chunk/2` copied or below
     `MIN_REF_AVG_ABS=20/PEAK=60`): scales mic by
     `REF_MISSING_ATTENUATE_Q15=0.125` (Q15 shift).
   - On bad chunks: clips + emits zeros to FIFO.
4. **Audio cleaner**:
   - HP biquad (c=32604/32768 ≈ 0.995).
   - Notch 100 Hz (Direct Form-II, 5 float coefficients).
   - Software-ref echo: when `live_aec` is OFF and
     `playback_active=true`, reads delayed software ref (240 samples
     behind), checks correlation, attenuates with
     `SOFTREF_ATTENUATE_Q15=0.5` or `HIGH_CORR_ATTENUATE_Q15=0.125`.
   - Quiet-frame scaling: 8192/32768 ≈ 0.25 (default) or
     4096/32768 ≈ 0.125 (playback).
   - Impulse attenuation: peak ≥ 10000 + avg ≤ 260 ⇒ divide by 4.
5. **Uplink** — `enqueue_mic_uplink_frame`:
   - PCM16: `nuwax_audio_uplink_build_pcm16_frame` builds 22-byte NA
     header + samples into `s_audio_upload_batch`.
   - Opus: `nuwax_opus_encoder_encode` →
     `nuwax_audio_frame_build(... NUWAX_AUDIO_CODEC_OPUS ...)` with
     bitrate=32 kbps, 16 kHz mono, FEC=off, DTX=on.
   - When `batch_count ≥ 4` or `final_frame=true`: enqueue.
   - Send task drains `ready_packets` and calls
     `nuwax_ws_send_binary(pool, len)` (WebSocket frame).
6. **Downlink** — server pushes NA-frame PCM16 → `on_ws_binary` →
   `nuwax_speaker_playback_handle_audio_frame(seq, flags, sr, ch, samples, n)`:
   - START flag ⇒ reset generation, clear queue, fire `on_tts_start`.
   - Enqueue into 512-slot PSRAM packet queue.
7. **Playback** — `nuwax_speaker_playback_task`:
   - Start flag ⇒ `playback_active=true`, `set_state(SPEAKING)`.
   - Lazy `nuwax_speaker_output_init` on first packet.
   - Volume-gain via `set_volume_percent(value)` Q15 multiply.
   - `i2s_channel_write` blocks ≤ `NUWAX_SPEAKER_WRITE_TIMEOUT_MS`.
   - END flag ⇒ `speaker_finish_playback_if_current(...)`: clear
     session, fire `on_playback_done(played, failures)`, mute idle
     output, schedule auto-followup or set IDLE.
   - On STOP/CANCEL: cancel_now drops the in-flight packets within
     1500-2500 ms grace window.

End-to-end **two-state playback envelope**:
- "speaking" ⇒ "after TTS_RESULT playback_start, during audio capture
  gated by `playback_upload_gate_required` (during_playback_mode &&
  playback_active)"
- "speaking" ⇒ "" once `speaker_cancel_now("touch_cancel")` fires
  and the queue empties.

---

### File / line anchors summary (cheat sheet)

| Topic                                        | Anchor                                               |
| -------------------------------------------- | ---------------------------------------------------- |
| Build & deps                                 | `main/CMakeLists.txt:122-134`                       |
| V1.2 pin map                                 | `main/board/esp_vocat_v1_0_pins.h:14-74`             |
| Shared I2C bus init                          | `main/board/i2c_bus_manager.c:30-78`                 |
| Audio board facade                           | `main/board/audio_board.c:71-234`                    |
| Vocat backend init + codec-power ordering    | `main/board/vocat_audio_board.c:526-567`             |
| ES7210 gain-after-enable                     | `main/board/vocat_audio_board.c:412-422`             |
| REF TDM probe                                | `main/board/vocat_audio_board.c:634-705`             |
| AEC ref health check signature               | `main/board/vocat_audio_board.h:132-133`             |
| Slot semantics macros                        | `main/board/vocat_audio_board.h:29-34`                |
| Owner mutex (mic)                            | `main/mic_input.c:52-101`                            |
| Vocat TDM read path                          | `main/mic_input.c:358-753`                           |
| NA protocol frame                            | `main/audio_frame.c`                                 |
| Opus encoder wrapper                         | `main/opus_encoder_wrapper.c:44-171`                 |
| Audio uplink two-queue producer              | `main/conversation/audio_uplink.c:200-489`           |
| Audio uplink enqueue + batch build           | `main/conversation/audio_uplink.c:323-403`           |
| Speech-protect + softref echo                | `main/conversation/audio_cleaner.c:136-351`          |
| HP / 100 Hz notch biquad                     | `main/conversation/audio_cleaner.c:353-419`          |
| Audio cleaner process                        | `main/conversation/audio_cleaner.c:455-576`          |
| Live AEC backend selection macros            | `main/conversation/live_aec.c:106-162`                |
| AEC FIFO ring                                | `main/conversation/live_aec.c:411-445`               |
| AEC chunk processing                         | `main/conversation/live_aec.c:641-786`               |
| Residual cancellation                        | `main/conversation/live_aec.c:299-409`               |
| Software reference ring buffer               | `main/p3_software_ref.c`                             |
| L3 interrupt-level prototype                 | `main/p3_afe_l3.c`                                   |
| P3 diagnostic softref A/B harness            | `main/p3_softref_aec_diag.c:544-841`                 |
| P3 near-end candidate evaluation             | `main/p3_nearend_candidate.c:289-465`                |
| P5 full-duplex overlap probe                 | `main/conversation/p5_fullduplex_overlap_probe.c`    |
| P6 gray-confirm pure-function state machine  | `main/p6_gray_confirm.c:34-101`                       |
| Wake word backend selection                  | `main/wake_word_detector.c:108-198`                  |
| Wake word detect loop                        | `main/wake_word_detector.c:200-335`                  |
| Speaker output TX I2S                         | `main/speaker_output.c:52-201`                       |
| Speaker output write                         | `main/speaker_output.c:226-282`                      |
| Speaker output stop/pause preload            | `main/speaker_output.c:284-365`                      |
| Speaker output deinit                        | `main/speaker_output.c:367-`                         |
| Speaker playback packet struct               | `main/conversation/speaker_playback.c:36-44`         |
| Speaker playback handle_audio_frame         | `main/conversation/speaker_playback.c:705-747`       |
| Speaker playback task body                   | `main/conversation/speaker_playback.c:749-844`       |
| Speaker playback cancel + drop window        | `main/conversation/speaker_playback.c:428-486`       |
| Speaker playback fade cancel                 | `main/conversation/speaker_playback.c:488-536`       |
| Speaker playback note_playback_begin         | `main/conversation/speaker_playback.c:602-630`       |
| Speaker playback note_playback_end           | `main/conversation/speaker_playback.c:632-`          |
| App main super-loop                          | `main/nuwax_ai_partner_main.c:5024-5107`             |
| Stream mic main loop                         | `main/nuwax_ai_partner_main.c:2348-3130`             |
| During-playback nearend candidate heuristic  | `main/nuwax_ai_partner_main.c:2222-2280`             |
| Stream auto-followup preroll ring            | `main/nuwax_ai_partner_main.c:2183-2196`              |
| Send auto-followup preroll                   | `main/nuwax_ai_partner_main.c:2282-2329`             |
| Prelisten state machine                      | `main/conversation/prelisten_state.c`                 |
| Listen resume state machine                  | `main/conversation/listen_resume_state.c`            |
| Touch gesture classification                 | `main/conversation/touch_gesture.c:88-100`           |
| WebSocket transport + handshake              | `main/ws_transport.c:355-573`                        |
| WebSocket TLS trycloudflare CA               | `main/ws_transport.c:43-69`                          |
| WebSocket frame send (chunked + masked)       | `main/ws_transport.c:416-468`                        |
| Session hello / audio params                 | `main/session_protocol.c:127-202`                     |
| Session text-frame parse                     | `main/session_protocol.c:407-881`                     |
| Session dispatch set_state / set_volume      | `main/conversation/session_dispatch.c:134-266`        |
| Session dispatch P3 / P5 / P6 commands        | `main/conversation/session_dispatch.c:268-489`        |
| MCP server (initialize/tools/list/tools/call) | `main/mcp_server.c`                                  |
| Shell action dispatch                        | `main/conversation/shell_action_dispatch.c`           |
| LVGL display port                            | `main/nuwax_lvgl_port.c:18-49`                       |
| V28 UI layers                                | `main/nuwax_ui_v28/nuwax_app.c:30-58`                |
| Device state + 8-entry listener              | `main/nuwax_ui_v28/nuwax_device_state.c`             |
| State bridge (app_state ⇄ emotion/dialog)    | `main/nuwax_ui_v28/nuwax_state_bridge.c`              |
| App_shell LVGL mailbox                       | `main/app_shell.c:61-153`                            |
