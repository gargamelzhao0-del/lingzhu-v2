param(
    [string]$Gcc = "gcc"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot "build-host-checks"

New-Item -ItemType Directory -Force $outputDir | Out-Null

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/ws_receive_deadline_policy_host_check.c") `
    -o (Join-Path $outputDir "ws_receive_deadline_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "ws_receive_deadline_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/audio_runtime_memory_policy_host_check.c") `
    -o (Join-Path $outputDir "audio_runtime_memory_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "audio_runtime_memory_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/sleep_transaction.c") `
    (Join-Path $projectRoot "main/sleep_transaction_host_check.c") `
    -o (Join-Path $outputDir "sleep_transaction_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "sleep_transaction_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/notification_playback.c") `
    (Join-Path $projectRoot "main/notification_playback_host_check.c") `
    -o (Join-Path $outputDir "notification_playback_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "notification_playback_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/notification_rx_barrier.c") `
    (Join-Path $projectRoot "main/notification_rx_barrier_host_check.c") `
    -o (Join-Path $outputDir "notification_rx_barrier_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "notification_rx_barrier_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/ws_send_failure_policy_host_check.c") `
    -o (Join-Path $outputDir "ws_send_failure_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "ws_send_failure_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/audio_terminal_recovery_host_check.c") `
    -o (Join-Path $outputDir "audio_terminal_recovery_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "audio_terminal_recovery_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/ws_close_policy_host_check.c") `
    -o (Join-Path $outputDir "ws_close_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "ws_close_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/app_state.c") `
    (Join-Path $projectRoot "main/conversation/head_touch_control_policy.c") `
    (Join-Path $projectRoot "main/head_touch_control_policy_host_check.c") `
    -o (Join-Path $outputDir "head_touch_control_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "head_touch_control_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/head_touch_agent_intent.c") `
    (Join-Path $projectRoot "main/head_touch_agent_intent_host_check.c") `
    -o (Join-Path $outputDir "head_touch_agent_intent_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "head_touch_agent_intent_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/capture_phase_handoff_host_check.c") `
    -o (Join-Path $outputDir "capture_phase_handoff_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "capture_phase_handoff_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    -I (Join-Path $projectRoot "main") `
    (Join-Path $projectRoot "main/rt_capture_wait_policy_host_check.c") `
    -o (Join-Path $outputDir "rt_capture_wait_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $outputDir "rt_capture_wait_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/prelisten_state.c") `
    (Join-Path $projectRoot "main/prelisten_state_host_check.c") `
    -o (Join-Path $outputDir "prelisten_state_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "prelisten_state_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/awake_session_state.c") `
    (Join-Path $projectRoot "main/awake_session_state_host_check.c") `
    -o (Join-Path $outputDir "awake_session_state_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "awake_session_state_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/listen_resume_state.c") `
    (Join-Path $projectRoot "main/listen_resume_state_host_check.c") `
    -o (Join-Path $outputDir "listen_resume_state_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "listen_resume_state_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/playback_terminal_guard.c") `
    (Join-Path $projectRoot "main/playback_terminal_guard_host_check.c") `
    -o (Join-Path $outputDir "playback_terminal_guard_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "playback_terminal_guard_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/turn_guard.c") `
    (Join-Path $projectRoot "main/conversation/turn_guard_host_check.c") `
    -o (Join-Path $outputDir "turn_guard_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "turn_guard_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/preroll_window.c") `
    (Join-Path $projectRoot "main/preroll_window_host_check.c") `
    -o (Join-Path $outputDir "preroll_window_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "preroll_window_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/speaker_backpressure_policy_host_check.c") `
    -o (Join-Path $outputDir "speaker_backpressure_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "speaker_backpressure_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/aec_reference_provenance_host_check.c") `
    -o (Join-Path $outputDir "aec_reference_provenance_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "aec_reference_provenance_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/speaker_jitter_policy_host_check.c") `
    -o (Join-Path $outputDir "speaker_jitter_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "speaker_jitter_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/wake_word_input_policy_host_check.c") `
    -o (Join-Path $outputDir "wake_word_input_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "wake_word_input_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/wake_word_detection_policy_host_check.c") `
    -o (Join-Path $outputDir "wake_word_detection_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "wake_word_detection_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/wake_word_probe_policy_host_check.c") `
    -o (Join-Path $outputDir "wake_word_probe_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "wake_word_probe_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation_endpoint_policy_host_check.c") `
    -o (Join-Path $outputDir "conversation_endpoint_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "conversation_endpoint_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/adaptive_endpoint.c") `
    (Join-Path $projectRoot "main/adaptive_endpoint_host_check.c") `
    -o (Join-Path $outputDir "adaptive_endpoint_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "adaptive_endpoint_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/speech_confirmation.c") `
    (Join-Path $projectRoot "main/speech_confirmation_host_check.c") `
    -o (Join-Path $outputDir "speech_confirmation_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "speech_confirmation_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/short_speech_tracker.c") `
    (Join-Path $projectRoot "main/short_speech_tracker_host_check.c") `
    -o (Join-Path $outputDir "short_speech_tracker_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "short_speech_tracker_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/speaker_generation_policy_host_check.c") `
    -o (Join-Path $outputDir "speaker_generation_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "speaker_generation_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/speaker_stream_policy_host_check.c") `
    -o (Join-Path $outputDir "speaker_stream_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "speaker_stream_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/ui_sync_pending_host_check.c") `
    -o (Join-Path $outputDir "ui_sync_pending_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "ui_sync_pending_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/deferred_state_pending_host_check.c") `
    -o (Join-Path $outputDir "deferred_state_pending_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "deferred_state_pending_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/touch_cancel_recovery.c") `
    (Join-Path $projectRoot "main/touch_cancel_recovery_host_check.c") `
    -o (Join-Path $outputDir "touch_cancel_recovery_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "touch_cancel_recovery_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/emotion_ui_pending_host_check.c") `
    -o (Join-Path $outputDir "emotion_ui_pending_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "emotion_ui_pending_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation_visual_round_host_check.c") `
    -o (Join-Path $outputDir "conversation_visual_round_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "conversation_visual_round_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/barge_in_controller.c") `
    (Join-Path $projectRoot "main/barge_in_controller_host_check.c") `
    -o (Join-Path $outputDir "barge_in_controller_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "barge_in_controller_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/conversation_audio_health.c") `
    (Join-Path $projectRoot "main/conversation/conversation_audio_health_host_check.c") `
    -o (Join-Path $outputDir "conversation_audio_health_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "conversation_audio_health_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/live_aec_output_provenance.c") `
    (Join-Path $projectRoot "main/live_aec_output_provenance_host_check.c") `
    -o (Join-Path $outputDir "live_aec_output_provenance_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "live_aec_output_provenance_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main')" `
    (Join-Path $projectRoot "main/conversation/conversation_metrics.c") `
    (Join-Path $projectRoot "main/conversation/conversation_metrics_host_check.c") `
    -o (Join-Path $outputDir "conversation_metrics_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $outputDir "conversation_metrics_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$mainSource = Get-Content (Join-Path $projectRoot "main/nuwax_ai_partner_main.c") -Raw
$endpointPolicyHeader = Get-Content (Join-Path $projectRoot "main/conversation/conversation_endpoint_policy.h") -Raw
$shortSpeechTrackerHeader = Get-Content (Join-Path $projectRoot "main/conversation/short_speech_tracker.h") -Raw
$turnGuardSource = Get-Content (Join-Path $projectRoot "main/conversation/turn_guard.c") -Raw
$turnGuardHeader = Get-Content (Join-Path $projectRoot "main/conversation/turn_guard.h") -Raw
$speakerSource = Get-Content (Join-Path $projectRoot "main/conversation/speaker_playback.c") -Raw
$speakerHeader = Get-Content (Join-Path $projectRoot "main/conversation/speaker_playback.h") -Raw
$speakerOutputSource = Get-Content (Join-Path $projectRoot "main/speaker_output.c") -Raw
$speakerOutputHeader = Get-Content (Join-Path $projectRoot "main/speaker_output.h") -Raw
$speakerBackpressureSource = Get-Content (Join-Path $projectRoot "main/conversation/speaker_backpressure_policy.h") -Raw
$speakerJitterPolicy = Get-Content (Join-Path $projectRoot "main/conversation/speaker_jitter_policy.h") -Raw
$speakerGenerationPolicy = Get-Content (Join-Path $projectRoot "main/conversation/speaker_generation_policy.h") -Raw
$speakerStreamPolicy = Get-Content (Join-Path $projectRoot "main/conversation/speaker_stream_policy.h") -Raw
$dispatcherSource = Get-Content (Join-Path $projectRoot "main/conversation/barge_in_dispatcher.c") -Raw
$dispatcherHeader = Get-Content (Join-Path $projectRoot "main/conversation/barge_in_dispatcher.h") -Raw
$sessionDispatchHeader = Get-Content (Join-Path $projectRoot "main/conversation/session_dispatch.h") -Raw
$sessionDispatchSource = Get-Content (Join-Path $projectRoot "main/conversation/session_dispatch.c") -Raw
$sessionProtocolSource = Get-Content (Join-Path $projectRoot "main/session_protocol.c") -Raw
$kconfigSource = Get-Content (Join-Path $projectRoot "main/Kconfig.projbuild") -Raw
$audioUplinkSource = Get-Content (Join-Path $projectRoot "main/conversation/audio_uplink.c") -Raw
$audioUplinkHeader = Get-Content (Join-Path $projectRoot "main/conversation/audio_uplink.h") -Raw
$audioTerminalRecoverySource = Get-Content (Join-Path $projectRoot "main/conversation/audio_terminal_recovery.c") -Raw
$adaptiveEndpointSource = Get-Content (Join-Path $projectRoot "main/conversation/adaptive_endpoint.c") -Raw
$adaptiveEndpointHeader = Get-Content (Join-Path $projectRoot "main/conversation/adaptive_endpoint.h") -Raw
$speechConfirmationSource = Get-Content (Join-Path $projectRoot "main/conversation/speech_confirmation.c") -Raw
$speechConfirmationHeader = Get-Content (Join-Path $projectRoot "main/conversation/speech_confirmation.h") -Raw
$micLevelSource = Get-Content (Join-Path $projectRoot "main/conversation/mic_level.c") -Raw
$realtimeLogHeader = Get-Content (Join-Path $projectRoot "main/conversation/realtime_log.h") -Raw
$projectKconfig = Get-Content (Join-Path $projectRoot "main/Kconfig.projbuild") -Raw
$p5ProbeSource = Get-Content (Join-Path $projectRoot "main/conversation/p5_fullduplex_overlap_probe.c") -Raw
$bargeControllerSource = Get-Content (Join-Path $projectRoot "main/conversation/barge_in_controller.c") -Raw
$latencyContract = Get-Content (Join-Path $projectRoot "main/conversation/conversation_latency_contract.h") -Raw
$liveAecSource = Get-Content (Join-Path $projectRoot "main/conversation/live_aec.c") -Raw
$liveAecHeader = Get-Content (Join-Path $projectRoot "main/conversation/live_aec.h") -Raw
$wsSource = Get-Content (Join-Path $projectRoot "main/ws_transport.c") -Raw
$wsHeader = Get-Content (Join-Path $projectRoot "main/ws_transport.h") -Raw
$wsClosePolicy = Get-Content (Join-Path $projectRoot "main/ws_close_policy.h") -Raw
$wsFailurePolicyHeader = Get-Content (Join-Path $projectRoot "main/ws_send_failure_policy.h") -Raw
$wifiSource = Get-Content (Join-Path $projectRoot "main/wifi_manager.c") -Raw
$wakeSource = Get-Content (Join-Path $projectRoot "main/wake_word_detector.c") -Raw
$wakeHeader = Get-Content (Join-Path $projectRoot "main/wake_word_detector.h") -Raw
$wakeProbePolicy = Get-Content (Join-Path $projectRoot "main/conversation/wake_word_probe_policy.h") -Raw
$wakeConfigSource = Get-Content (Join-Path $projectRoot "main/conversation/wake_word_config.c") -Raw
$micSource = Get-Content (Join-Path $projectRoot "main/mic_input.c") -Raw
$micHeader = Get-Content (Join-Path $projectRoot "main/mic_input.h") -Raw
$wakeAliasPolicy = Get-Content (Join-Path $projectRoot "main/conversation/wake_word_alias_policy.h") -Raw
$prelistenSource = Get-Content (Join-Path $projectRoot "main/conversation/prelisten_state.c") -Raw
$prerollWindowHeader = Get-Content (Join-Path $projectRoot "main/conversation/preroll_window.h") -Raw
$bridgeSource = Get-Content (Join-Path $projectRoot "main/nuwax_ui_v28/nuwax_state_bridge.c") -Raw
$shellSource = Get-Content (Join-Path $projectRoot "main/app_shell.c") -Raw
$shellHeader = Get-Content (Join-Path $projectRoot "main/app_shell.h") -Raw
$shellDispatchSource = Get-Content (Join-Path $projectRoot "main/conversation/shell_action_dispatch.c") -Raw
$touchGestureSource = Get-Content (Join-Path $projectRoot "main/conversation/touch_gesture.c") -Raw
$headTouchSource = Get-Content (Join-Path $projectRoot "main/conversation/head_touch_control.c") -Raw
$awakeStateSource = Get-Content (Join-Path $projectRoot "main/conversation/awake_session_state.c") -Raw
$listenResumeSource = Get-Content (Join-Path $projectRoot "main/conversation/listen_resume_state.c") -Raw
$capturePhaseSource = Get-Content (Join-Path $projectRoot "main/conversation/capture_phase_handoff.h") -Raw
$rtCaptureWaitPolicy = Get-Content (Join-Path $projectRoot "main/conversation/rt_capture_wait_policy.h") -Raw
$sdkconfigDefaults = Get-Content (Join-Path $projectRoot "sdkconfig.defaults") -Raw
$productOtaDefaults = Get-Content (Join-Path $projectRoot "sdkconfig.product-ota.defaults") -Raw
$bargeConfig = Get-Content (Join-Path $projectRoot "sdkconfig.barge-final") -Raw
$sdkconfig = Get-Content (Join-Path $projectRoot "sdkconfig") -Raw
$sleepTransactionConfig = Get-Content (Join-Path $projectRoot "sdkconfig.sleep-transaction") -Raw
$sleepTransactionSource = Get-Content (Join-Path $projectRoot "main/conversation/sleep_transaction.c") -Raw
$notificationSource = Get-Content (Join-Path $projectRoot "main/conversation/notification_playback.c") -Raw
$notificationHeader = Get-Content (Join-Path $projectRoot "main/conversation/notification_playback.h") -Raw
$touchCancelSource = Get-Content (Join-Path $projectRoot "main/conversation/touch_cancel_recovery.c") -Raw
$touchCancelHeader = Get-Content (Join-Path $projectRoot "main/conversation/touch_cancel_recovery.h") -Raw

$wakeLifetimeBlock = [regex]::Match(
    $wakeSource,
    '(?s)if \(!lifetime_refresh_deferred && mn_created_ms != 0U.*?(?=\n#endif)').Value
if ($wakeLifetimeBlock.Length -eq 0 -or
    $wakeLifetimeBlock -notmatch 's_detector\.mn_maintenance_due = true' -or
    $wakeLifetimeBlock -notmatch 'healthy backend retained and refresh deferred' -or
    $wakeLifetimeBlock -match 's_detector\.mn_rebuild_pending\s*=\s*true' -or
    $wakeLifetimeBlock -match '\bbreak\s*;') {
    throw "Wake backend lifetime regression: soft expiry must retain the healthy backend and defer maintenance."
}

$wakeStartBlock = [regex]::Match(
    $wakeSource,
    '(?s)esp_err_t nuwax_wake_word_detector_start_with_context\(.*?(?=\nesp_err_t nuwax_wake_word_detector_start\()').Value
if ($wakeStartBlock.Length -eq 0 -or
    $wakeStartBlock -notmatch 'backend_missing\s*\|\|\s*s_detector\.mn_rebuild_pending' -or
    $wakeStartBlock -match 'lifetime_expired|cycle_limit_reached' -or
    $wakeStartBlock -notmatch 'healthy backend can be cleaned and re-armed') {
    throw "Wake backend start regression: periodic maintenance must not block the sleep wake-up critical path."
}

$wakeCommandConfigBlock = [regex]::Match(
    $wakeSource,
    '(?s)static esp_err_t configure_multinet_commands\(.*?(?=#if CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET)').Value
if ($wakeCommandConfigBlock.Length -eq 0 -or
    $wakeCommandConfigBlock -notmatch 'commands_reused' -or
    $wakeCommandConfigBlock -notmatch '(?s)if \(commands_reused\).*?esp_mn_commands_clear\(\)') {
    throw "Wake command lifecycle regression: a fresh MultiNet command tree must not be cleared before its first command is added."
}

$wakeBackendDestroyBlock = [regex]::Match(
    $wakeSource,
    '(?s)static void destroy_multinet_backend_locked\(void\).*?(?=static esp_err_t rebuild_multinet_backend_locked)').Value
if ($wakeBackendDestroyBlock.Length -eq 0 -or
    $wakeBackendDestroyBlock -notmatch '(?s)mn_iface->destroy\(s_detector\.mn_data\).*?mn_commands_allocated = false' -or
    $wakeBackendDestroyBlock -match '(?s)if \(s_detector\.mn_commands_allocated\).*?mn_iface->destroy') {
    throw "Wake command teardown regression: MultiNet destroy owns its global command tree; the application must not free it first."
}

if ($latencyContract -notmatch 'NUWAX_CONVERSATION_CAPTURE_OWNER_POLL_MS\s+20U' -or
    $rtCaptureWaitPolicy -notmatch 'NUWAX_RT_CAPTURE_WAIT_CONTROL_POLL_IDLE' -or
    $rtCaptureWaitPolicy -notmatch 'wait_result == NUWAX_RT_CAPTURE_WAIT_FRAME_READY' -or
    $mainSource -notmatch '(?s)rt_capture_receive\(\s*&\(work->capture_ctx\),\s*&capture_frame,\s*NUWAX_CONVERSATION_CAPTURE_OWNER_POLL_MS\s*\).*?NUWAX_RT_CAPTURE_WAIT_CONTROL_POLL_IDLE.*?continue;' -or
    $mainSource -notmatch 'nuwax_rt_capture_wait_counts_mic_timeout\(' -or
    $mainSource -match 'CONFIG_NUWAX_MIC_READ_TIMEOUT_MS\s*\+\s*50U') {
    throw "RT capture responsiveness regression: queue polling must stay within one 20 ms frame and queue-idle polls must not become MIC timeouts."
}

if ($turnGuardHeader -notmatch "NUWAX_TURN_PHASE_WAIT_TTS_RETRY" -or
    $turnGuardHeader -notmatch "nuwax_turn_guard_retire_metadata_playback" -or
    $turnGuardSource -notmatch "playback_id_retired" -or
    $turnGuardSource -notmatch "metadata_retry_count >= max_retries" -or
    $mainSource -notmatch "NUWAX_TTS_METADATA_RETRY_GRACE_MS" -or
    $mainSource -notmatch "metadata-only TTS playback retired; waiting for bounded same-turn retry" -or
    $mainSource -notmatch '(?s)arm_metadata_playback_retry.*?start_gate_pending.*?rx_stream_active.*?queue_waiting.*?stop_in_progress' -or
    $mainSource -notmatch '(?s)playback_reason_allows_metadata_retry.*?arm_metadata_playback_retry\(command,\s*"cloud_playback_end_error"\)' -or
    $mainSource -notmatch '(?s)case NUWAX_SESSION_EVENT_TTS_RESULT:.*?arm_metadata_playback_retry\(command,\s*"tts_result_error"\)' -or
    $mainSource -notmatch '(?s)case NUWAX_SESSION_EVENT_PLAYBACK_BEGIN:.*?NUWAX_TURN_PHASE_WAIT_TTS_RETRY' -or
    $mainSource -notmatch 'strcmp\(reason, "completed"\).*?strcmp\(reason, "complete"\)') {
    throw "Metadata-only TTS retry regression: retries must remain bounded, same-turn, retired-ID aware, and forbidden after PCM/START ownership begins."
}

if ($turnGuardHeader -notmatch "nuwax_turn_guard_accepts_tts_skip" -or
    $turnGuardSource -notmatch '(?s)nuwax_turn_guard_accepts_tts_skip.*?NUWAX_TURN_PHASE_WAIT_TTS.*?NUWAX_TURN_PHASE_WAIT_TTS_RETRY.*?speaker_generation != 0U' -or
    $sessionProtocolSource -notmatch 'strcmp\(type->valuestring, "tts_skip"\) == 0' -or
    $sessionProtocolSource -notmatch 'NUWAX_SESSION_EVENT_TTS_SKIP' -or
    $sessionProtocolSource -notmatch 'voice_broadcast_disabled' -or
     $mainSource -notmatch 'case NUWAX_SESSION_EVENT_TTS_SKIP:' -or
     $mainSource -notmatch 'nuwax_turn_guard_accepts_tts_skip' -or
     $mainSource -notmatch 'clear_head_touch_agent_intent\("tts_skip"\)' -or
     $mainSource -notmatch 'clear_active_turn_guard\("tts_skip"\)' -or
     $mainSource -notmatch 'arm_tts_skip_followup' -or
     $mainSource -notmatch 'TTS skip waiting for cloud emotion' -or
     $mainSource -notmatch 'static void service_tts_skip_followup' -or
     $mainSource -notmatch 'schedule_auto_followup_after_cloud_output' -or
     $sessionDispatchHeader -notmatch 'set_emotion_with_context' -or
     $sessionDispatchSource -notmatch 'set_emotion_with_context' -or
     $mainSource -notmatch 'take_tts_skip_prebound_emotion' -or
     $mainSource -notmatch 'tts_skip prebound emotion stored') {
    throw "TTS-skip regression: voice-broadcast disable must wait for the owned cloud emotion hold, including reverse-order set_emotion binding, before starting the next listening round."
}

if ($touchCancelHeader -notmatch "nuwax_touch_cancel_recovery_mark_physical_stop" -or
    $touchCancelSource -notmatch "nuwax_touch_cancel_recovery_owner_matches" -or
    $mainSource -notmatch "duplicate touch cancel ignored while recovery is active" -or
    $mainSource -notmatch "nuwax_touch_cancel_recovery_arm" -or
    $mainSource -notmatch "touch cancel listening held for physical stop" -or
    $mainSource -notmatch "A during-playback/barge-in capture may still own MIC/I2S" -or
    $shellDispatchSource -match 'callbacks->clear_auto_followup\(' -or
    $shellDispatchSource -notmatch '(?s)case NUWAX_APP_SHELL_ACTION_CANCEL:.*?callbacks->request_cancel_turn' -or
    $mainSource -notmatch '(?s)service_touch_cancel_recovery.*?sleep_lifecycle_blocks_new_capture\(\).*?audio_lifecycle_takeover.*?exclusive_audio_mode_blocks_normal_audio\(\).*?exclusive_audio_takeover' -or
    $mainSource -notmatch '(?s)NUWAX_SPEAKER_OWNER_EVENT_PLAYBACK_DONE.*?nuwax_touch_cancel_recovery_mark_physical_stop' -or
    $mainSource -notmatch '(?s)NUWAX_SPEAKER_OWNER_EVENT_CANCEL_RECOVERY.*?nuwax_touch_cancel_recovery_mark_physical_stop' -or
    $mainSource -notmatch '(?s)NUWAX_SESSION_EVENT_CANCEL_TURN_ACK.*?nuwax_touch_cancel_recovery_mark_ack' -or
    $mainSource -notmatch '(?s)drain_speaker_owner_events\(\);.*?service_touch_cancel_recovery\(ws\);' -or
    $mainSource -notmatch '(?s)service_touch_cancel_recovery.*?physical_stop_done.*?detector_released.*?recover_normal_conversation_after_playback_abort') {
    throw "Touch-cancel recovery regression: cloud quarantine, physical speaker stop, wake-detector release, ACK binding, and the single listening handoff must remain generation-bound."
}

if ($notificationHeader -notmatch "nuwax_notification_refresh_stream_deadline" -or
    $notificationSource -notmatch "NUWAX_NOTIFICATION_STATE_STREAMING" -or
    $notificationSource -notmatch "NUWAX_NOTIFICATION_STATE_DRAINING" -or
    $mainSource -notmatch "duplicate notification begin payload conflict" -or
    $mainSource -notmatch "duplicate notification end payload conflict" -or
    $mainSource -notmatch "reset_notification_transaction_runtime" -or
    $mainSource -notmatch "notification playback active" -or
    $mainSource -notmatch "sleep command rejected while notification transaction owns audio" -or
    $mainSource -notmatch '(?s)static bool ota_continuous_audio_busy.*?nuwax_notification_blocks_conversation' -or
    $mainSource -notmatch '(?s)static bool meeting_can_start.*?nuwax_notification_blocks_conversation' -or
    $mainSource -notmatch '(?s)static bool monitoring_can_start.*?nuwax_notification_blocks_conversation') {
    throw "Notification playback isolation regression: deadlines, duplicate payloads, connection cleanup, and cross-mode admission must remain transaction-bound."
}

if ($mainSource -notmatch 'NUWAX_SESSION_NOTIFICATION_QUEUE_DEPTH' -or
    $mainSource -notmatch '(?s)static bool session_command_is_notification_control.*?stop_playback.*?prepare_notify_playback' -or
    $mainSource -notmatch '(?s)QueueHandle_t target_queue = is_notification_control.*?s_session_notification_commands.*?is_low_priority_progress' -or
    $mainSource -notmatch '(?s)while \(handled < max_commands(?:\s*&&[^)]*)?\).*?xQueueReceive\(s_session_notification_commands.*?xQueueReceive\(s_session_critical_commands.*?xQueueReceive\(s_session_commands' -or
    $mainSource -notmatch 'create_queue_prefer_psram\(\s*"session_notification_commands"' -or
    [regex]::Matches($mainSource, 'xQueueReset\(s_session_notification_commands\)').Count -lt 2) {
    throw "Notification control scheduling regression: prepare/begin/end/stop must retain an independent highest-priority FIFO that is cleared at OTA and connection boundaries."
}

if ($mainSource -notmatch 'log_cloud_connect_failure\("websocket_connect"' -or
    $mainSource -notmatch 'log_cloud_connect_failure\("config_load"' -or
    $mainSource -notmatch 'log_cloud_connect_failure\("hello_send"' -or
    $mainSource -notmatch "endpoint_configured=%d secret_present=%d secret_len=%u" -or
    $mainSource -match "secret_fingerprint=%s|secret_hint=%s" -or
    $mainSource -match 'cloud connect diagnostic[^\r\n]*device_secret=%s' -or
    $wsSource -notmatch 'WebSocket handshake rejected http_status=%d status_line=') {
    throw "Cloud diagnostics must retain status/presence/length without credentials, fingerprint or raw endpoint."
}

if ($sdkconfigDefaults -notmatch '(?m)^CONFIG_NUWAX_LIVE_AEC_ENABLE=n\s*$' -or
    $productOtaDefaults -notmatch '(?m)^CONFIG_NUWAX_LIVE_AEC_ENABLE=n\s*$' -or
    $sleepTransactionConfig -notmatch '(?m)^(CONFIG_NUWAX_LIVE_AEC_ENABLE=n|# CONFIG_NUWAX_LIVE_AEC_ENABLE is not set)\s*$' -or
    $sleepTransactionConfig -match '(?m)^CONFIG_NUWAX_LIVE_AEC_ENABLE=y\s*$' -or
    $mainSource -notmatch '(?s)static void.*?request_playback_wake_word_detector\(.*?#if CONFIG_NUWAX_WAKE_WORD_ENABLE\s+&& CONFIG_NUWAX_WAKE_WORD_INTERRUPT_ENABLE' -or
    $mainSource -notmatch '(?s)case NUWAX_SPEAKER_OWNER_EVENT_TTS_START:.*?!CONFIG_NUWAX_BARGE_IN_ENABLE.*?request_playback_wake_word_detector\(' -or
    $mainSource -notmatch '(?s)case NUWAX_SPEAKER_OWNER_EVENT_FIRST_PCM_WRITTEN:.*?!CONFIG_NUWAX_BARGE_IN_ENABLE.*?if \(!arm_playback_wake_word_detector\(' -or
    $mainSource -notmatch 'playback wake detector suppressed epoch=%lu generation=%lu reason=tts_start_playback_wake_disabled' -or
    $mainSource -notmatch '(?s)playback wake detector suppressed.*?#endif\s+break;') {
    throw "Playback audio protection regression: product builds must keep Live AEC disabled while the dedicated playback wake detector remains independent of ordinary voice barge-in."
}

if ($wakeHeader -notmatch 'bool probe_requested;' -or
    $wakeProbePolicy -notmatch 'NUWAX_WAKE_PROBE_REQUIRED_SAFE_FRAMES 3U' -or
    $wakeProbePolicy -notmatch 'NUWAX_WAKE_PROBE_COOLDOWN_MS 15000U' -or
    $wakeSource -notmatch 'nuwax_wake_probe_policy_observe' -or
    $wakeSource -notmatch 'filter_result.nearfield_safe && filter_result.reference_active' -or
    $wakeSource -notmatch '\.probe_requested = true' -or
    $mainSource -notmatch '!result->probe_requested' -or
    $mainSource -notmatch 'nuwax_speaker_playback_request_probe_duck_generation' -or
    $mainSource -notmatch 'speaker probe duck applied=%d') {
    throw "Playback wake probe regression: three safe near-field frames must request a rate-limited, generation-bound duck without evicting final wake events."
}

if ($wakeSource -notmatch '#define NUWAX_WAKE_AUDIO_TASK_CORE 1' -or
    $wakeSource -notmatch '(?s)xTaskCreateStaticPinnedToCore\(\s*wake_word_task,.*?NUWAX_WAKE_AUDIO_TASK_CORE\)' -or
    $wakeSource -notmatch '(?s)xTaskCreatePinnedToCoreWithCaps\(\s*wake_capture_task,.*?NUWAX_WAKE_AUDIO_TASK_CORE,.*?MALLOC_CAP_SPIRAM') {
    throw "Wake capture scheduling regression: sleep and playback wake readers must stay on core 1, isolated from the core-0 Wi-Fi task."
}
if ($wakeSource -notmatch 'NUWAX_WAKE_INFERENCE_COOPERATE_INTERVAL_MS 1000U' -or
    $wakeSource -notmatch '(?s)nuwax_wake_input_cooperate_due\(\s*capture != NULL,.*?NUWAX_WAKE_INFERENCE_COOPERATE_INTERVAL_MS\).*?vTaskDelay\(1\);.*?cooperative_blocks\+\+;' -or
    $wakeSource -notmatch 'inference_avg_us=%lu inference_max_us=%lu cooperative_blocks=%lu') {
    throw "Playback wake scheduling regression: continuous MultiNet inference must block one tick every second and publish inference/cooperation health so CPU1 IDLE remains watchdog-safe."
}

if ($mainSource -notmatch 'playback terminal cloud grace expired.*WebSocket retained' -or
    $mainSource -notmatch 'playback_terminal_local_quarantine' -or
    $mainSource -notmatch 'cancel_turn failed.*local recovery continues and WebSocket is retained' -or
    $mainSource -match 'matching cancel_turn_ack rejected[^\r\n]*reconnecting' -or
    $mainSource -match 'playback terminal barrier timed out[^\r\n]*reconnecting') {
    throw "Playback transport retention regression: slow/missing cloud audio terminal messages must quarantine only the old generation and retain a healthy WebSocket."
}

if ($sleepTransactionSource -notmatch "nuwax_sleep_transaction_requires_local_sleep_reconcile" -or
    $mainSource -notmatch "cloud transport connecting preserved conversation state" -or
    $mainSource -notmatch 'set_cloud_transport_connecting_state\("websocket_disconnect"\)' -or
    $mainSource -notmatch "reconciling local sleeping state after transport recovery" -or
    $mainSource -notmatch "idle_wait_sleep_state_reconcile" -or
    $mainSource -notmatch "remote transport state isolated while sleep lifecycle owns device" -or
    $sessionDispatchSource -notmatch "state isolated by local lifecycle" -or
    $mainSource -notmatch "type == NUWAX_SPEAKER_OWNER_EVENT_FIRST_PCM_WRITTEN" -or
    $mainSource -notmatch '(?s)case NUWAX_SPEAKER_OWNER_EVENT_FIRST_PCM_WRITTEN:.*?set_state_deferred_ui\(NUWAX_APP_STATE_SPEAKING\)' -or
    $mainSource -match 'set_conversation_status_guarded\("NUWAX",\s*"SPEAKING"') {
    throw "Conversation lifecycle regression: reconnects must preserve/reconcile sleeping state and SPEAKING must be published only by a generation-bound first PCM event."
}

if ($wakeConfigSource -notmatch 'nuwax_wake_command_variant_count' -or
    $wakeConfigSource -notmatch 'nuwax_wake_command_variant_at' -or
    $wakeSource -notmatch 'configure_multinet_commands\(&configured\)' -or
    $wakeSource -notmatch 'esp_mn_commands_add\(1, variant\)' -or
    $wakeSource -notmatch 'NUWAX_WAKE_WORD_CONFIG_LEGACY_VERSION 2U' -or
    $wakeSource -notmatch 'NUWAX_WAKE_WORD_LEGACY_MAX_VARIANTS 4U' -or
    $wakeSource -notmatch 'legacy_persisted_to_wake_config' -or
    $wakeAliasPolicy -notmatch '"xiao zhu xiao zhu"' -or
    $wakeAliasPolicy -notmatch '"xiao zu xiao zu"' -or
    $wakeAliasPolicy -notmatch '"xiao zhu xiao zu"' -or
    $wakeAliasPolicy -notmatch '"xiao zu xiao zhu"') {
    throw "Wake alias regression: legacy strings must retain factory expansion and four-slot NVS arrays must migrate without expansion."
}

if ($speakerSource -notmatch '(?s)static TickType_t speaker_positive_delay_ticks\(uint32_t delay_ms\).*?const TickType_t ticks = pdMS_TO_TICKS\(delay_ms\);.*?return ticks == 0 \? 1 : ticks;' -or
    $speakerSource -match 'vTaskDelay\(pdMS_TO_TICKS\(1\)\)' -or
    [regex]::Matches($speakerSource, 'vTaskDelay\(speaker_positive_delay_ticks\(').Count -lt 9) {
    throw "Speaker wait contract regressed: every positive sub-tick delay must block for at least one RTOS tick."
}

$wakeGreetingStart = $mainSource.IndexOf("static esp_err_t send_wake_word_greeting")
$wakeGreetingEnd = $mainSource.IndexOf("static esp_err_t wait_for_mic_start_action", $wakeGreetingStart)
$wakeGreetingBlock = if ($wakeGreetingStart -ge 0 -and
                         $wakeGreetingEnd -gt $wakeGreetingStart) {
    $mainSource.Substring($wakeGreetingStart,
                          $wakeGreetingEnd - $wakeGreetingStart)
} else { "" }
$captureStart = $mainSource.IndexOf("static esp_err_t stream_microphone_audio")
$captureCleanup = $mainSource.IndexOf("cleanup:", $captureStart)
$captureInitBlock = if ($captureStart -ge 0 -and
                        $captureCleanup -gt $captureStart) {
    $mainSource.Substring($captureStart,
                          $captureCleanup - $captureStart)
} else { "" }
$postPlaybackResumeStart = $mainSource.IndexOf(
    "static void complete_post_playback_resume")
$postPlaybackResumeEnd = $mainSource.IndexOf(
    "static bool schedule_auto_followup_after_playback",
    $postPlaybackResumeStart)
$postPlaybackResumeBlock = if ($postPlaybackResumeStart -ge 0 -and
                                   $postPlaybackResumeEnd -gt
                                       $postPlaybackResumeStart) {
    $mainSource.Substring(
        $postPlaybackResumeStart,
        $postPlaybackResumeEnd - $postPlaybackResumeStart)
} else { "" }
$autoFollowupStart = $mainSource.IndexOf(
    "static bool schedule_auto_followup_after_playback")
$autoFollowupEnd = $mainSource.IndexOf(
    "static void return_to_listening_after_cancel",
    $autoFollowupStart)
$autoFollowupBlock = if ($autoFollowupStart -ge 0 -and
                             $autoFollowupEnd -gt $autoFollowupStart) {
    $mainSource.Substring($autoFollowupStart,
                          $autoFollowupEnd - $autoFollowupStart)
} else { "" }
$waitCloudStart = $mainSource.IndexOf(
    "static void wait_for_cloud_updates")
$waitCloudEnd = $mainSource.IndexOf(
    "static bool mcp_set_emotion",
    $waitCloudStart)
$waitCloudBlock = if ($waitCloudStart -ge 0 -and
                          $waitCloudEnd -gt $waitCloudStart) {
    $mainSource.Substring($waitCloudStart,
                          $waitCloudEnd - $waitCloudStart)
} else { "" }
$preserveListenStart = $mainSource.LastIndexOf(
    "static bool drain_shell_actions_preserving_listen_control")
$preserveListenEnd = if ($preserveListenStart -ge 0) {
    $mainSource.IndexOf("static esp_err_t run_p3_aec_ref_health_gate",
                        $preserveListenStart)
} else { -1 }
$preserveListenBlock = if ($preserveListenStart -ge 0 -and
                               $preserveListenEnd -gt $preserveListenStart) {
    $mainSource.Substring($preserveListenStart,
                          $preserveListenEnd - $preserveListenStart)
} else { "" }

if ($wakeGreetingBlock.Length -eq 0 -or
    $wakeGreetingBlock.IndexOf("begin_active_turn_guard") -lt 0 -or
    $wakeGreetingBlock.IndexOf("nuwax_session_send_text_request") -lt 0 -or
    $wakeGreetingBlock.IndexOf("begin_active_turn_guard") -gt
        $wakeGreetingBlock.IndexOf("nuwax_session_send_text_request") -or
    $wakeGreetingBlock -notmatch "wake greeting turn guard could not be established; request not sent" -or
    $wakeGreetingBlock -notmatch 'begin_active_turn_guard[\s\S]*?wake_greeting_metrics_cancel\("wake_greeting_guard_failed"\)[\s\S]*?return ESP_ERR_INVALID_STATE' -or
    $wakeGreetingBlock -notmatch 'err != ESP_OK[\s\S]*?clear_active_turn_guard\("wake_greeting_send_failed"\)') {
    throw "Wake greeting ownership regression: establish the turn guard before sending and clear it when send fails."
}

if ($captureInitBlock.Length -eq 0 -or
    $captureInitBlock -notmatch "Configured microphone record length is zero" -or
    $captureInitBlock -notmatch "Configured microphone record length is zero[\s\S]*?goto cleanup" -or
    $captureInitBlock -notmatch "barge-in controller initialization failed[\s\S]*?goto cleanup" -or
    $captureInitBlock -notmatch "conversation audio health initialization failed[\s\S]*?goto cleanup") {
    throw "Capture cleanup regression: early capture setup failures must enter the unified cleanup path."
}

if ($postPlaybackResumeBlock.Length -eq 0 -or
    $postPlaybackResumeBlock -notmatch 'start_prelisten_capture_now\(\s*"auto_followup"' -or
    $postPlaybackResumeBlock -notmatch 'start_prelisten_capture_now\(\s*"awake_session"' -or
    $postPlaybackResumeBlock -match 'start_prelisten_window\(' -or
    $autoFollowupBlock.Length -eq 0 -or
    $autoFollowupBlock -notmatch 'start_prelisten_capture_now\(\s*"auto_followup"' -or
    $autoFollowupBlock -match 'start_prelisten_window\(' -or
    $autoFollowupBlock -match 'CONFIG_NUWAX_CONVERSATION_AUTO_FOLLOWUP_GRACE_MS') {
    throw "Post-playback capture regression: physical playback completion must start the preroll capture immediately; only VAD admission may be delayed."
}

if ($waitCloudBlock.Length -eq 0 -or
    $waitCloudBlock -notmatch 'while \(nuwax_ws_is_connected\(ws\)' -or
    $waitCloudBlock -notmatch 'listen_err != ESP_OK[\s\S]*?if \(!nuwax_ws_is_connected\(ws\)\) \{[\s\S]*?return;' -or
    $mainSource -notmatch 'cleanup:[\s\S]*?nuwax_prelisten_state_finish_capture\(&s_prelisten\);[\s\S]*?return result;') {
    throw "Disconnect cleanup regression: background capture must leave cloud wait immediately after WebSocket loss and clear capture ownership through unified cleanup."
}

if ($waitCloudBlock -notmatch 'drain_shell_actions_preserving_listen_control\(ws\)[\s\S]*?return;' -or
    $preserveListenBlock.Length -eq 0 -or
    $preserveListenBlock -notmatch 'queued_at_entry = uxQueueMessagesWaiting\(s_shell_actions\)' -or
    $preserveListenBlock -notmatch 'xQueueReceive\(s_shell_actions, &action, 0\)' -or
    $preserveListenBlock -notmatch 'nuwax_shell_action_is_listen_control\(action\.action\)[\s\S]*?s_deferred_shell_listen_actions[\s\S]*?continue;' -or
    $preserveListenBlock -notmatch 'return s_deferred_shell_listen_action_count > 0U;' -or
    $mainSource -notmatch 'take_deferred_shell_listen_action\(&action\)[\s\S]*?xQueueReceive\(s_shell_actions, &action, 0\)' -or
    $mainSource -notmatch 'commit_sleeping_conversation_state[\s\S]*?clear_deferred_shell_listen_actions' -or
    $mainSource -notmatch 'begin_exclusive_audio_takeover[\s\S]*?clear_deferred_shell_listen_actions' -or
    $shellHeader -notmatch 'bool nuwax_app_shell_request_action' -or
    $shellSource -notmatch 'static bool app_shell_queue_result' -or
    $shellSource -notmatch 'APP_SHELL_CLEANUP_ACTION_QUEUE_WAIT_MS\s+100' -or
    $shellSource -notmatch '(?s)cleanup_action\s*=.*?NUWAX_APP_SHELL_ACTION_STOP_LISTEN.*?NUWAX_APP_SHELL_ACTION_CANCEL.*?wait_ticks\s*=\s*cleanup_action.*?pdMS_TO_TICKS\(\s*APP_SHELL_CLEANUP_ACTION_QUEUE_WAIT_MS\s*\).*?xQueueSend\(queue, result, wait_ticks\) != pdTRUE.*?return false;.*?return true;' -or
    $headTouchSource -notmatch 's_press_started_listen = nuwax_app_shell_request_action' -or
    $headTouchSource -notmatch 'if \(!s_press_started_listen\)[\s\S]*?rejected reason=shell_action_queue' -or
    $mainSource -notmatch 'head_touch_interaction_allowed[\s\S]*?s_reprovision_quiesce_requested[\s\S]*?nuwax_ota_device_blocks_audio\(\)[\s\S]*?exclusive_audio_mode_blocks_normal_audio\(\)' -or
    $mainSource -notmatch '(?s)static bool head_touch_agent_is_blocked\(void\).*?s_reprovision_quiesce_requested.*?sleep_lifecycle_blocks_new_capture\(\).*?exclusive_audio_mode_blocks_normal_audio\(\).*?nuwax_ota_device_blocks_audio\(\)' -or
    $mainSource -notmatch '(?s)static void service_touch_cancel_recovery\(.*?s_reprovision_quiesce_requested.*?clear_touch_cancel_recovery\("reprovision_takeover"\)' -or
    $mainSource -notmatch 'action\.action == NUWAX_APP_SHELL_ACTION_CANCEL &&[\s\S]*?exclusive_audio_mode_blocks_normal_audio\(\)[\s\S]*?dropped stale shell cancel action') {
    throw "Head-touch queue ownership regression: listen controls must survive the cloud-settle window, enqueue failure must not arm release, and provisioning/exclusive/OTA modes must reject or cancel normal touch control."
}

if ($sessionProtocolSource -notmatch 'nuwax_session_send_ping_until_if_epoch' -or
    $sessionProtocolSource -notmatch 'nuwax_ws_send_keepalive_until_if_epoch\(ws, json, deadline_us, expected_epoch\)' -or
    $mainSource -notmatch '#define NUWAX_WS_APPLICATION_KEEPALIVE_INTERVAL_MS 15000' -or
    $mainSource -notmatch 'service_application_keepalive\(ws, "idle_wait"\)' -or
    $captureInitBlock -notmatch 'service_application_keepalive\(ws, "conversation_capture"\)' -or
    $mainSource -notmatch 'service_application_keepalive\(ws, "exclusive_audio"\)' -or
    $mainSource -notmatch 'service_application_keepalive\(\s*&ws, "terminal_recovery"\)' -or
    $mainSource -notmatch 'application keepalive sent context=%s epoch=%lu') {
    throw "Application presence regression: bounded application ping must run during idle, normal capture/playback, exclusive audio, and terminal recovery modes."
}

$reconnectRecoveryStart = $mainSource.IndexOf(
    "recover_awake_conversation_after_ws_reconnect(")
$reconnectRecoveryEnd = if ($reconnectRecoveryStart -ge 0) {
    $mainSource.IndexOf("static bool resume_prelisten_after_no_speech",
                        $reconnectRecoveryStart)
} else { -1 }
$reconnectRecoveryBlock = if ($reconnectRecoveryStart -ge 0 -and
                                  $reconnectRecoveryEnd -gt
                                      $reconnectRecoveryStart) {
    $mainSource.Substring($reconnectRecoveryStart,
                          $reconnectRecoveryEnd - $reconnectRecoveryStart)
} else { "" }
if ($reconnectRecoveryBlock.Length -eq 0 -or
    $reconnectRecoveryBlock -notmatch 's_ws_audio_recovery\.ws_epoch' -or
    $reconnectRecoveryBlock -notmatch 's_ws_audio_recovery\.awake_generation' -or
    $reconnectRecoveryBlock -notmatch 'ws_epoch != active_epoch' -or
    $reconnectRecoveryBlock -notmatch 'NUWAX_WS_RECOVERY_FALLBACK_SLEEP' -or
    $reconnectRecoveryBlock -notmatch 'ws_reconnect_handoff_to_exclusive' -or
    $reconnectRecoveryBlock -notmatch 'nuwax_monitoring_mode_abort_pending_controls' -or
    $reconnectRecoveryBlock -notmatch 'nuwax_meeting_mode_abort_pending_controls' -or
    $reconnectRecoveryBlock -notmatch 'nuwax_prelisten_state_clear\(&s_prelisten\)' -or
    $reconnectRecoveryBlock -notmatch 'start_prelisten_window\("awake_session", window_ms, 0U\)' -or
    $reconnectRecoveryBlock -notmatch '!s_prelisten.pending && !s_prelisten.capture_active' -or
    $mainSource -notmatch 'recover_awake_conversation_after_ws_reconnect\([\s\S]*?"ws_reconnect"' -or
    $mainSource -notmatch 'ws_reconnect_recovery_invariant' -or
    $mainSource -notmatch 'WebSocket reconnect recovery lost its audio target; forcing sleeping wake-ready state') {
    throw "WebSocket recovery regression: an old-epoch prelisten must be replaced by a verified awake window or forced into sleeping wake-ready state."
}

if ($sessionProtocolSource -notmatch '(?s)strcmp\(type->valuestring, "audio_stats"\) == 0.*?cJSON_GetObjectItem\(audio_session,\s*"audio_session_id"\).*?NUWAX_SESSION_EVENT_AUDIO_PROGRESS.*?utf8_copy_trunc\(out_command->audio_session_id' -or
    $mainSource -notmatch '(?s)is_low_priority_progress.*?NUWAX_SESSION_EVENT_AUDIO_PROGRESS.*?is_notification_control.*?target_queue = is_notification_control.*?s_session_notification_commands.*?is_low_priority_progress.*?s_session_commands.*?s_session_critical_commands' -or
    $mainSource -notmatch '(?s)audio_input_event.*?NUWAX_SESSION_EVENT_AUDIO_PROGRESS.*?nuwax_listen_resume_state_event_matches' -or
    $mainSource -notmatch '(?s)case NUWAX_SESSION_EVENT_AUDIO_PROGRESS:.*?note_conversation_cloud_progress\(command->event\);.*?break;') {
    throw "Cloud progress regression: session-bound audio_stats must refresh the ASR watchdog without consuming critical queue capacity."
}

if ($listenResumeSource -notmatch 'nuwax_listen_resume_state_can_bootstrap_cloud_turn' -or
    $mainSource -notmatch 'ensure_active_turn_guard_for_cloud_output' -or
    $mainSource -notmatch '(?s)case NUWAX_SESSION_EVENT_TEXT_RESPONSE:.*?ensure_active_turn_guard_for_cloud_output.*?note_conversation_cloud_turn_bootstrap' -or
    $mainSource -notmatch '(?s)case NUWAX_SESSION_EVENT_PLAYBACK_BEGIN:.*?ensure_active_turn_guard_for_cloud_output.*?note_conversation_cloud_turn_bootstrap' -or
    $mainSource -notmatch 's_conversation_cloud_progress_closes_capture') {
    throw "Cloud-output bootstrap regression: text/playback output must replace the removed asr_result only for the current bound capture round."
}

$aecEmitGateIndex = $mainSource.IndexOf("if (!nuwax_live_aec_output_emitted(&(work->live_aec)))")
$rawAnalysisIndex = $mainSource.IndexOf("const nuwax_mic_level_t raw_level", $aecEmitGateIndex)
$alignedPopIndex = $mainSource.IndexOf("nuwax_aec_alignment_context_t *aligned_context", $aecEmitGateIndex)
if ($aecEmitGateIndex -lt 0 -or
    $alignedPopIndex -le $aecEmitGateIndex -or
    $rawAnalysisIndex -le $alignedPopIndex -or
    $mainSource -notmatch "AEC stream end flush" -or
    $mainSource -notmatch "nuwax_live_aec_flush_pending" -or
    $liveAecSource -notmatch "human_rescue_queued\.sample_count != aec->human_rescue_count" -or
    $liveAecSource -notmatch "aec->human_rescue_count != aec->output_count" -or
    $liveAecSource -notmatch "aec->output_count < sample_count \|\|\s+aec->human_rescue_count < sample_count" -or
    $liveAecHeader -notmatch "NUWAX_LIVE_AEC_RESCUE_SEGMENT_CAPACITY 8U") {
    throw "AEC frame alignment regression: AEC/rescue/provenance and capture metadata must be emitted together as a complete 320-sample frame."
}

if ($mainSource -notmatch "memcpy\(frame->hw_ref,\s+frame->ref" -or
    $mainSource -notmatch "memcpy\(staged_alignment_context->hw_ref,\s+capture_frame->hw_ref" -or
    $mainSource -notmatch "memcpy\(s_audio_hw_ref_samples,\s+aligned_context->hw_ref" -or
    $mainSource -notmatch "raw_mic1_hw_ref_8k_interleaved" -or
    $mainSource -notmatch "hw_ref_health_passed=%d") {
    throw "Hardware-reference evidence regression: raw MIC3 must survive software-reference fallback and remain exportable without changing AEC selection."
}

$rxTaskStart = $wsSource.IndexOf("static void websocket_receive_task")
# cleanup_rx_task returns the owner-visible join error. Keep this anchor tied
# to the actual signature so the static lifecycle check does not fail before
# evaluating the RX/owner ordering assertions.
$rxTaskEnd = $wsSource.IndexOf("static esp_err_t cleanup_rx_task", $rxTaskStart)
$rxTaskBlock = if ($rxTaskStart -ge 0 -and $rxTaskEnd -gt $rxTaskStart) {
    $wsSource.Substring($rxTaskStart, $rxTaskEnd - $rxTaskStart)
} else { "" }
$waitIndex = $wsSource.IndexOf("wait_for_rx_task_stop(", $rxTaskEnd)
$deleteIndex = $wsSource.IndexOf("vTaskDeleteWithCaps(rx_task)", $rxTaskEnd)
$cleanupRxEnd = $wsSource.IndexOf("static void owner_close_status_store", $rxTaskEnd)
$cleanupRxBlock = if ($rxTaskEnd -ge 0 -and
                          $cleanupRxEnd -gt $rxTaskEnd) {
    $wsSource.Substring($rxTaskEnd, $cleanupRxEnd - $rxTaskEnd)
} else { "" }
$ownerCloseStart = $wsSource.IndexOf("esp_err_t nuwax_ws_owner_close(")
$ownerCloseEnd = $wsSource.IndexOf("void nuwax_ws_close(", $ownerCloseStart)
$ownerCloseBlock = if ($ownerCloseStart -ge 0 -and
                           $ownerCloseEnd -gt $ownerCloseStart) {
    $wsSource.Substring($ownerCloseStart,
                        $ownerCloseEnd - $ownerCloseStart)
} else { "" }
$closeSerializedStart = $wsSource.IndexOf("static esp_err_t transport_close_serialized")
$closeSerializedEnd = $wsSource.IndexOf("static bool wait_for_rx_task_stop", $closeSerializedStart)
$closeSerializedBlock = if ($closeSerializedStart -ge 0 -and
                                $closeSerializedEnd -gt $closeSerializedStart) {
    $wsSource.Substring($closeSerializedStart,
                        $closeSerializedEnd - $closeSerializedStart)
} else { "" }
$transportCloseStart = $wsSource.IndexOf(
    "static esp_err_t transport_close(nuwax_ws_client_t *client)")
$transportCloseEnd = $wsSource.IndexOf(
    "static void transport_interrupt",
    $transportCloseStart)
$transportCloseBlock = if ($transportCloseStart -ge 0 -and
                               $transportCloseEnd -gt $transportCloseStart) {
    $wsSource.Substring($transportCloseStart,
                        $transportCloseEnd - $transportCloseStart)
} else { "" }
$waitStopStart = $wsSource.IndexOf("static bool wait_for_rx_task_stop")
$waitStopEnd = $wsSource.IndexOf("static void websocket_send_close_code",
                                 $waitStopStart)
$waitStopBlock = if ($waitStopStart -ge 0 -and
                         $waitStopEnd -gt $waitStopStart) {
    $wsSource.Substring($waitStopStart, $waitStopEnd - $waitStopStart)
} else { "" }
$connectStart = $wsSource.IndexOf("esp_err_t nuwax_ws_connect(")
$connectEnd = $wsSource.IndexOf("esp_err_t nuwax_ws_send_text(", $connectStart)
$connectBlock = if ($connectStart -ge 0 -and $connectEnd -gt $connectStart) {
    $wsSource.Substring($connectStart, $connectEnd - $connectStart)
} else { "" }
# Connection publication is now isolated under per-client frame ownership.
$connectTransportStart = $wsSource.IndexOf("static esp_err_t connect_transport_tx_locked(")
$connectTransportBlock = if ($connectTransportStart -ge 0 -and $connectStart -gt $connectTransportStart) {
    $wsSource.Substring($connectTransportStart, $connectStart - $connectTransportStart)
} else { "" }
$rxStartGateIndex = $rxTaskBlock.IndexOf("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)")
$publishRxTaskIndex = $connectTransportBlock.IndexOf("client->rx_task = rx_task")
$publishOpenIndex = $connectTransportBlock.IndexOf(
    "lifecycle_store(client, NUWAX_WS_LIFECYCLE_OPEN)")
$releaseRxTaskIndex = $connectTransportBlock.IndexOf("xTaskNotifyGive(rx_task)")
if ($rxTaskBlock.Length -eq 0 -or
    $connectBlock.Length -eq 0 -or
    $connectTransportBlock.Length -eq 0 -or
    $connectBlock -notmatch '(?s)nuwax_ws_owner_close\(client\).*?take_mutex_until\(\(SemaphoreHandle_t\)client->tx_frame_lock,.*?connect_transport_tx_locked\(client, config, url\).*?xSemaphoreGive\(\(SemaphoreHandle_t\)client->tx_frame_lock\)' -or
    $rxStartGateIndex -lt 0 -or
    $publishRxTaskIndex -lt 0 -or
    $publishOpenIndex -le $publishRxTaskIndex -or
    $releaseRxTaskIndex -le $publishOpenIndex -or
    $rxTaskBlock -match "transport_close\(" -or
    $rxTaskBlock -match "esp_tls_conn_destroy" -or
    $rxTaskBlock -notmatch "NUWAX_WS_LIFECYCLE_RX_EXITED" -or
    $rxTaskBlock -notmatch "lifecycle_store\(client,\s+NUWAX_WS_LIFECYCLE_RX_EXITED\);(?s:.*?)atomic_store_explicit\(&client->rx_task_running,\s+false" -or
    # CLOSE_REQUESTED may overwrite RX_EXITED before the owner joins. The RX
    # running flag is therefore the authoritative bounded join predicate.
    $waitStopBlock -notmatch "rx_task_running" -or
    $rxTaskBlock -notmatch 'atomic_store_explicit\(&client->rx_task_running,\s+false[\s\S]*?vTaskSuspend\(NULL\)' -or
    $cleanupRxBlock.Length -eq 0 -or
    $cleanupRxBlock -notmatch 'if \(!wait_for_rx_task_stop\([\s\S]*?\)\) \{[\s\S]*?return ESP_ERR_TIMEOUT;[\s\S]*?\}\s*vTaskDeleteWithCaps\(rx_task\)' -or
    $waitIndex -lt 0 -or $deleteIndex -le $waitIndex -or
    $wsSource -match "vTaskDeleteWithCaps\(rx_task\)(?s:.*?)Timed out waiting" -or
    $closeSerializedBlock -match "portMAX_DELAY" -or
    $closeSerializedBlock -notmatch "NUWAX_WS_LIFECYCLE_LOCK_TIMEOUT_MS" -or
    $transportCloseBlock.Length -eq 0 -or
    $transportCloseBlock -match "portMAX_DELAY" -or
    $transportCloseBlock -notmatch "take_io_lock_with_timeout" -or
    $transportCloseBlock -notmatch "handle retained" -or
    $transportCloseBlock -notmatch "ESP_ERR_NOT_FINISHED" -or
    $ownerCloseBlock.Length -eq 0 -or
    $ownerCloseBlock -notmatch 'transport_interrupt\(client\);[\s\S]*?cleanup_rx_task\(client\)[\s\S]*?transport_close_serialized\(client\)' -or
    $ownerCloseBlock -match 'vTaskDeleteWithCaps' -or
    $wsSource -notmatch "previous transport cleanup failed:.*refusing handle reuse" -or
    $wsSource -notmatch "previous transport still owns resources; refusing handle reuse") {
    throw "WebSocket lifecycle regression: publish the RX handle and OPEN state before releasing the worker, then let the owner join before one bounded transport destroy."
}

foreach ($configText in @($sdkconfigDefaults, $sdkconfig)) {
    if ($configText -notmatch "CONFIG_NUWAX_VOICE_UPLINK_QUEUE_DEPTH=64" -or
        $configText -notmatch "CONFIG_NUWAX_VOICE_UPLINK_REALTIME_MAX_AGE_MS=1000" -or
        $configText -notmatch "CONFIG_NUWAX_RT_CAPTURE_QUEUE_DEPTH=12" -or
        $configText -notmatch "CONFIG_NUWAX_RT_CAPTURE_MAX_DECISION_AGE_MS=60" -or
        $configText -notmatch "CONFIG_NUWAX_BARGE_END_TO_END_MAX_FRAME_AGE_MS=100" -or
        $configText -notmatch "CONFIG_NUWAX_MIC_LEVEL_LOG_INTERVAL_FRAMES=0" -or
        $configText -notmatch "# CONFIG_NUWAX_REALTIME_AUDIO_DIAGNOSTIC_LOG_ENABLE is not set" -or
        $configText -notmatch "CONFIG_ESPTOOLPY_MONITOR_BAUD=921600" -or
        $configText -notmatch "# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set" -or
        $configText -notmatch "CONFIG_ESP_CONSOLE_UART_CUSTOM=y" -or
        $configText -notmatch "CONFIG_ESP_CONSOLE_UART_CUSTOM_NUM_0=y" -or
        $configText -notmatch "CONFIG_ESP_CONSOLE_UART_TX_GPIO=43" -or
        $configText -notmatch "CONFIG_ESP_CONSOLE_UART_RX_GPIO=44" -or
        $configText -notmatch "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600" -or
        $configText -notmatch "CONFIG_NUWAX_PROTOCOL_LISTEN_STOP_ENABLE=y" -or
        $configText -notmatch "CONFIG_NUWAX_WIFI_REALTIME_DISABLE_POWER_SAVE=y") {
        throw "Normal-conversation latency configuration drifted from the production contract."
    }
}

if ($bargeConfig -notmatch "CONFIG_NUWAX_PROTOCOL_LISTEN_STOP_ENABLE=y" -or
    $kconfigSource -notmatch '(?s)config NUWAX_PROTOCOL_LISTEN_STOP_ENABLE.*?default y' -or
    $sessionProtocolSource -notmatch '#define CONFIG_NUWAX_PROTOCOL_LISTEN_STOP_ENABLE 1' -or
    [regex]::Matches($sessionProtocolSource,
        'if \(suppress_listen_stop\(state, mode, audio_session_id\)\) \{\s*return ESP_ERR_NOT_SUPPORTED;').Count -ne 3 -or
    $mainSource -notmatch 'post-END cloud response watchdog armed' -or
    $mainSource -notmatch 'NUWAX_CONVERSATION_ASR_PROGRESS_TIMEOUT_MS\s+60000U' -or
    $mainSource -notmatch 'NUWAX_POST_END_RESPONSE_TIMEOUT_MS\s+15000U' -or
    $mainSource -notmatch 'nuwax_listen_resume_state_arm_post_end_asr' -or
    $mainSource -notmatch 'cloud_waiting_question_has_liveness_owner' -or
    $mainSource -notmatch 'cloud wait rejected without liveness owner' -or
    $mainSource -notmatch '(?s)if \(!enter_cloud_waiting_question\("audio_submitted"\).*?resume_awake_conversation_after_inactivity' -or
    $mainSource -notmatch 'recover_post_end_cloud_response_timeout\(ws, now_ms\)' -or
    $mainSource -notmatch 'post-END cloud response timed out for %u consecutive rounds; rebuilding stalled business channel' -or
    $mainSource -notmatch '(?s)const bool close_queued =\s*nuwax_ws_close_if_epoch\(ws, expected_ws_epoch,\s*NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED\);\s*if \(close_queued\) \{\s*return true;\s*\}.*?resuming local conversation' -or
    $sessionProtocolSource -notmatch 'nuwax_session_send_listen_stop_retry_if_epoch' -or
    $sessionProtocolSource -notmatch 'nuwax_ws_safe_unsent_retry_allowed' -or
    $listenResumeSource -notmatch 'nuwax_listen_resume_state_post_end_asr_expired') {
    throw "Listen transaction regression: END must be followed by a safely retryable stop and a bounded, session-scoped post-END cloud-response recovery."
}

if ($wsSource -notmatch "frame_bytes_written" -or
    $wsSource -notmatch "transport_write_attempted" -or
    $wsSource -notmatch "secure_transport" -or
    $wsSource -notmatch "nuwax_ws_send_failure_requires_disconnect" -or
    $wsSource -notmatch "nuwax_ws_reliable_send_failure_requires_disconnect" -or
    $wsSource -notmatch "reliable WebSocket frame deferred before transport progress" -or
    $wsSource -notmatch "reliable WebSocket frame failure requires reconnect" -or
    $wsSource -notmatch 'inflight_drain_enabled \? "realtime" : "reliable"' -or
    $wsSource -notmatch "unsent %s WebSocket frame expired before unsafe transport progress" -or
    $wsSource -notmatch "transport_wait_write_ready_until" -or
    $wsSource -notmatch "transport_wait_write_ready_until\(client, soft_deadline_us\)" -or
    $wsSource -match "transport_write_ready_now" -or
    $wsSource -notmatch "WebSocket RX callback slow" -or
    $wsSource -notmatch "WebSocket peer close frame received code=%u reason_bytes=%u reason=%s" -or
    $wsSource -notmatch "WebSocket receive task stopped reason=%s" -or
    $wsFailurePolicyHeader -notmatch "secure_transport && transport_write_attempted" -or
    $wsSource -notmatch "SO_KEEPALIVE" -or
    $mainSource -notmatch "wait_for_cloud_service_ready" -or
    $mainSource -notmatch "authenticated cloud application ready; audio may resume" -or
    $mainSource -notmatch "s_conversation_capture_runtime_active(?s:.*?)set_state_deferred_ui\(state\)" -or
    $wifiSource -notmatch "esp_wifi_set_ps\(WIFI_PS_NONE\)") {
    throw "Realtime cloud stability regression: unsent expiry, cloud-ready gating, deferred capture UI, TCP keepalive, and Wi-Fi no-sleep must remain enabled."
}

if ($mainSource -match 'notification BEGIN conflicts with RX ordering barrier(?s:.*?)closing bound connection' -or
    $mainSource -match 'normal playback BEGIN overlaps an unresolved notification stream(?s:.*?)closing bound connection' -or
    $mainSource -notmatch 'selected cloud audio owner retained' -or
    $mainSource -notmatch 'notification RX ownership commit failed' -or
    $mainSource -notmatch '(?s)if \(!notification_rx_barrier_admit_epoch\(.*?notification RX ownership commit failed.*?return;\s*\}\s*const esp_err_t ack_err = send_notification_event_ack') {
    throw "Cloud audio ownership regression: control-plane overlap must select or quarantine audio without tearing down a healthy WebSocket."
}

if ($speakerJitterPolicy -notmatch "NUWAX_SPEAKER_STALL_WARN" -or
    $speakerJitterPolicy -notmatch "NUWAX_SPEAKER_STALL_ABORT" -or
    $projectKconfig -notmatch 'config NUWAX_SPEAKER_STREAM_STALL_WARN_MS(?s:.*?)default 12000' -or
    $projectKconfig -notmatch 'config NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS(?s:.*?)default 60000' -or
    $speakerSource -notmatch "keeping generation recoverable" -or
    $mainSource -notmatch "transport attempt explicitly") {
    throw "Long TTS recovery regression: a recoverable warning must precede the hard PCM stall boundary and every WebSocket retry must republish cloud connecting."
}

if ($projectKconfig -notmatch 'config NUWAX_REALTIME_AUDIO_DIAGNOSTIC_LOG_ENABLE(?s:.*?)default n' -or
    $projectKconfig -notmatch 'config NUWAX_MIC_LEVEL_LOG_INTERVAL_FRAMES(?s:.*?)default 0' -or
    $realtimeLogHeader -notmatch 'NUWAX_RT_DIAG_LOGI' -or
    $realtimeLogHeader -notmatch 'NUWAX_RT_DIAG_LOGW' -or
    $micLevelSource -notmatch 'if \(important\) \{\s+return true;' -or
    $micLevelSource -notmatch '#if CONFIG_NUWAX_REALTIME_AUDIO_DIAGNOSTIC_LOG_ENABLE' -or
    $mainSource -notmatch 'NUWAX_RT_DIAG_LOGI\(\s*TAG,\s*"audio clean seq=' -or
    $mainSource -notmatch 'NUWAX_RT_DIAG_LOGI\(TAG,\s*"conversation audio health state=' -or
    $mainSource -notmatch 'NUWAX_RT_DIAG_LOGW\(TAG,\s*"barge controller fail-closed' -or
    $mainSource -notmatch 'NUWAX_RT_DIAG_LOGI\(TAG,\s*"local VAD speech confirmed' -or
    $liveAecSource -notmatch 'NUWAX_RT_DIAG_LOGI\(TAG,\s*"live AEC realtime chunks=' -or
    $audioUplinkSource -notmatch 'NUWAX_RT_DIAG_LOGI\(TAG,\s*"mic first batch sent' -or
    $mainSource -match 'ESP_LOGI\(\s*TAG,\s*"audio clean seq=' -or
    $liveAecSource -match 'ESP_LOGI\(TAG,\s*"live AEC realtime chunks=' -or
    $audioUplinkSource -match 'ESP_LOGI\(TAG,\s*"mic first batch sent') {
    throw "Realtime logging regression: production capture diagnostics must remain behind the disabled diagnostic gate."
}

if ($mainSource -notmatch "continuous local VAD endpoint committed" -or
    $mainSource -notmatch "NUWAX_CONTINUOUS_ENDPOINT_CANDIDATE_SILENCE_MS 450U" -or
    $mainSource -notmatch "nuwax_conversation_endpoint_decide" -or
    $mainSource -notmatch "binary END then listen stop, WebSocket retained" -or
    $mainSource -notmatch "persistent_awake_timeout" -or
    $mainSource -notmatch "if \(!persistent_conversation_mode &&\s+upload_started && elapsed_since_upload_ms >= max_record_ms\)" -or
    $mainSource -notmatch "if \(persistent_conversation_mode\) \{\s+s_prelisten\.mic_release_stops_capture = false;\s+max_record_samples = SIZE_MAX") {
    throw "Continuous conversation contract regression: a reliable local sentence endpoint must close only its audio session while retaining the awake session and WebSocket."
}
$shortTrackingStart = $mainSource.IndexOf(
    "const bool short_speech_tracking_allowed")
$correlatedWeakStart = if ($shortTrackingStart -ge 0) {
    $mainSource.IndexOf("const bool correlated_weak_confirmed",
                        $shortTrackingStart)
} else { -1 }
$trustedShortStart = if ($correlatedWeakStart -ge 0) {
    $mainSource.IndexOf("const bool trusted_short_speech_evidence",
                        $correlatedWeakStart)
} else { -1 }
$trustedShortEnd = if ($trustedShortStart -ge 0) {
    $mainSource.IndexOf("short_speech_observation =",
                        $trustedShortStart)
} else { -1 }
$correlatedWeakBlock = if ($correlatedWeakStart -ge 0 -and
                                $trustedShortStart -gt $correlatedWeakStart) {
    $mainSource.Substring($correlatedWeakStart,
                          $trustedShortStart - $correlatedWeakStart)
} else { "" }
$trustedShortBlock = if ($trustedShortStart -ge 0 -and
                             $trustedShortEnd -gt $trustedShortStart) {
    $mainSource.Substring($trustedShortStart,
                          $trustedShortEnd - $trustedShortStart)
} else { "" }
if ($mainSource -notmatch 'NUWAX_ORDINARY_WEAK_START_WINDOW_MS 480U' -or
    $mainSource -notmatch 'NUWAX_ORDINARY_WEAK_START_REQUIRED_MS 180U' -or
    $mainSource -notmatch 'NUWAX_ORDINARY_WEAK_START_RESET_SILENCE_MS 200U' -or
    $shortSpeechTrackerHeader -notmatch 'NUWAX_SHORT_SPEECH_UNTRUSTED_WEAK_MS 180U' -or
    $mainSource -notmatch 'nuwax_conversation_weak_start_eligible' -or
    $mainSource -notmatch 'nuwax_conversation_weak_start_level_evidence' -or
    $mainSource -notmatch 'nuwax_conversation_weak_start_frame_evidence' -or
    $mainSource -notmatch 'dual_mic_human_first_hint' -or
    $endpointPolicyHeader -notmatch 'NUWAX_CONVERSATION_CORRELATED_WEAK_CONFIRM_MS 240U' -or
    $endpointPolicyHeader -notmatch 'nuwax_conversation_correlated_weak_observe' -or
    $correlatedWeakBlock.Length -eq 0 -or
    $correlatedWeakBlock -notmatch 'weak_start_level_evidence' -or
    $correlatedWeakBlock -notmatch 'dual_mic_human_first_hint' -or
    $correlatedWeakBlock -notmatch 'capture_sequence_contiguous' -or
    $correlatedWeakBlock -notmatch '!playback_echo_veto && !aec_playback_gate_active' -or
    $trustedShortBlock.Length -eq 0 -or
    $trustedShortBlock -match 'dual_mic_human_first_hint' -or
    $trustedShortBlock -notmatch 'correlated_weak_confirmed' -or
    $trustedShortBlock -notmatch 'dual_mic_aec_hint' -or
    $trustedShortBlock -notmatch 'dual_mic_cleaner_hint' -or
    $trustedShortBlock -notmatch 'dual_mic_voice_hint' -or
    $mainSource -notmatch 'trusted_weak_speech_evidence' -or
    $mainSource -notmatch 'frame_is_speech =(?s:.*?)weak_continuation_confirmed;' -or
    $mainSource -notmatch 'weak_far_field_window' -or
    $mainSource -notmatch 'nuwax_short_speech_tracker_observe' -or
    $mainSource -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_FRAME_WEAK_SPEECH' -or
    $mainSource -notmatch 'energy_only_weak_speech_evidence' -or
    $mainSource -notmatch 'nuwax_adaptive_endpoint_observe_v3' -or
    $mainSource -notmatch 'else if \(weak_continuation_confirmed\)') {
    throw "Weak far-field start regression: ordinary listening must admit unified short speech and retain confirmed weak continuation until reliable silence."
}
$persistentUploadStart = $mainSource.IndexOf(
    "} else if (persistent_conversation_mode) {")
$persistentUploadEnd = if ($persistentUploadStart -ge 0) {
    $mainSource.IndexOf("} else {", $persistentUploadStart + 1)
} else { -1 }
$persistentUploadBlock = if ($persistentUploadStart -ge 0 -and
                                  $persistentUploadEnd -gt $persistentUploadStart) {
    $mainSource.Substring($persistentUploadStart,
                          $persistentUploadEnd - $persistentUploadStart)
} else { "" }
if ($persistentUploadBlock.Length -eq 0 -or
    $persistentUploadBlock -match 'start_mic_cloud_upload\(' -or
    $persistentUploadBlock -match 'upload_started = true;' -or
    $persistentUploadBlock -notmatch 'nuwax_capture_phase_handoff_begin_awake_prelisten\(' -or
    $persistentUploadBlock -notmatch 'upload_gate=speech_confirmation' -or
    $mainSource -notmatch 'if \(preserve_post_playback_onset\) \{(?s:.*?)start_mic_cloud_upload\(' -or
    $mainSource -notmatch 'post-playback local prelisten resumed upload_gate=speech_confirmation' -or
    $mainSource -notmatch 'if \(!upload_started && auto_followup_mode && vad_speech_seen' -or
    $mainSource -notmatch 'nuwax_capture_phase_handoff_promote_awake_uplink' -or
    $mainSource -notmatch 'if \(!upload_started && auto_followup_mode && during_playback_mode &&\s+vad_speech_seen') {
    throw "Speech-gated uplink regression: ordinary awake listening must remain local until confirmed speech, while preserved post-playback speech and barge-in still promote an ordered cloud uplink."
}
if ($projectKconfig -notmatch 'choice NUWAX_ADAPTIVE_ENDPOINT_MODE(?s:.*?)default NUWAX_ADAPTIVE_ENDPOINT_MODE_SHADOW' -or
    $projectKconfig -notmatch 'config NUWAX_VOICE_LOCAL_VAD_SPEECH_AVG_ABS(?s:.*?)default 281 if NUWAX_BOARD_ESP_VOCAT_V1_0' -or
    $projectKconfig -notmatch 'config NUWAX_VOICE_LOCAL_VAD_SPEECH_PEAK(?s:.*?)default 1105 if NUWAX_BOARD_ESP_VOCAT_V1_0' -or
    $projectKconfig -notmatch 'config NUWAX_VOICE_LOCAL_VAD_SILENCE_AVG_ABS(?s:.*?)default 130 if NUWAX_BOARD_ESP_VOCAT_V1_0' -or
    $projectKconfig -notmatch 'config NUWAX_VOICE_LOCAL_VAD_SILENCE_PEAK(?s:.*?)default 520 if NUWAX_BOARD_ESP_VOCAT_V1_0' -or
    $projectKconfig -notmatch 'config NUWAX_VOICE_LOCAL_VAD_TRAILING_SILENCE_MS(?s:.*?)default 1200' -or
    $adaptiveEndpointHeader -notmatch 'sizeof\(nuwax_adaptive_endpoint_t\) <= 64U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_MIN_COMMIT_MS 1200U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_TRUSTED_SHORT_COMMIT_MS 1200U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_SHORT_COMMIT_MS 1200U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_MEDIUM_COMMIT_MS 1700U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_MEDIUM_FIRST_PAUSE_COMMIT_MS 1800U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_LONG_COMMIT_MS 2200U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_VERY_LONG_COMMIT_MS 2500U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_MAX_COMMIT_MS 2500U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_UNKNOWN_HARD_COMMIT_MS 2600U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_MIXED_NOISE_HARD_COMMIT_MS 3200U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_LONG_VOICED_MAX_MS 12000U' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_LONG_SPAN_MAX_MS 12000U' -or
    $adaptiveEndpointSource -match 'malloc|calloc|realloc|free|xTaskCreate|xQueueCreate|xSemaphoreCreate|float|double' -or
    $adaptiveEndpointSource -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_STRICT_TAIL_MS' -or
    $adaptiveEndpointSource -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_RESUME_CONFIRM_MS' -or
    $adaptiveEndpointSource -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_ENERGY_RESUME_CONFIRM_MS' -or
    $adaptiveEndpointSource -notmatch 'maximum_frame_aligned_commit_ms' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_FRAME_WEAK_SPEECH' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE' -or
    $adaptiveEndpointHeader -notmatch 'NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN' -or
    $adaptiveEndpointSource -notmatch 'effective_pause_commit_ms' -or
    $adaptiveEndpointSource -match 'NUWAX_ADAPTIVE_ENDPOINT_WEAK_PROTECTION_MS' -or
    $adaptiveEndpointSource -notmatch 'if \(evidence->energy_weak_speech\)' -or
    $adaptiveEndpointSource -notmatch 'nuwax_adaptive_endpoint_seed' -or
    $mainSource -notmatch 'clean_result\.after\.avg_abs(?s:.*?)clean_result\.before\.avg_abs' -or
    $mainSource -notmatch 'adaptive_active_and_eligible(?s:.*?)else(?s:.*?)nuwax_conversation_endpoint_decide' -or
    $mainSource -notmatch 'else if \(!persistent_conversation_mode &&\s+!stop_requested' -or
    $mainSource -notmatch 'adaptive endpoint shadow summary predicted=' -or
    $mainSource -notmatch 'nuwax_adaptive_endpoint_reset\(&adaptive_endpoint\);(?s:.*?)adaptive_session_eligible = false' -or
    $mainSource -notmatch 'frame_is_speech =(?s:.*?)unified_weak_confirmed_now(?s:.*?)weak_continuation_confirmed;' -or
    $mainSource -notmatch 'else if \(weak_continuation_confirmed\)' -or
    $mainSource -notmatch 'energy_weak_speech = energy_only_weak_speech_evidence' -or
    $mainSource -notmatch 'nuwax_preroll_choose_confirmed_onset\(' -or
    $mainSource -notmatch 'if \(adaptive_observation\.resumed_pause_learned\) \{\s*last_confirmed_speech_us = esp_timer_get_time\(\);' -or
    $prerollWindowHeader -notmatch 'nuwax_preroll_choose_confirmed_onset' -or
    $endpointPolicyHeader -notmatch 'NUWAX_CONVERSATION_UTTERANCE_SOFT_LIMIT_MS 45000U' -or
    $endpointPolicyHeader -notmatch 'NUWAX_CONVERSATION_UTTERANCE_EXTENSION_MS 15000U' -or
    $endpointPolicyHeader -notmatch 'NUWAX_CONVERSATION_UTTERANCE_RECENT_CONFIRMED_MS 4000U' -or
    $endpointPolicyHeader -match 'NUWAX_CONVERSATION_UTTERANCE_HARD_LIMIT_MS' -or
    $endpointPolicyHeader -notmatch 'nuwax_conversation_utterance_next_limit_ms' -or
    $mainSource -notmatch 'continuous_utterance_limit_ms' -or
    $mainSource -notmatch 'nuwax_audio_uplink_context_queue_depth\(send_ctx\)' -or
    $sdkconfigDefaults -notmatch 'CONFIG_NUWAX_VOICE_LOCAL_VAD_SPEECH_AVG_ABS=281' -or
    $sdkconfigDefaults -notmatch 'CONFIG_NUWAX_VOICE_LOCAL_VAD_SPEECH_PEAK=1105' -or
    $sdkconfigDefaults -notmatch 'CONFIG_NUWAX_VOICE_LOCAL_VAD_SILENCE_AVG_ABS=130' -or
    $sdkconfigDefaults -notmatch 'CONFIG_NUWAX_VOICE_LOCAL_VAD_SILENCE_PEAK=520' -or
    $sdkconfigDefaults -notmatch 'CONFIG_NUWAX_VOICE_LOCAL_VAD_TRAILING_SILENCE_MS=1200' -or
    $sdkconfigDefaults -notmatch 'CONFIG_NUWAX_ADAPTIVE_ENDPOINT_MODE_ACTIVE=y' -or
    $sdkconfigDefaults -match 'CONFIG_NUWAX_ADAPTIVE_ENDPOINT_MODE_SHADOW=y' -or
    $sdkconfigDefaults -notmatch 'CONFIG_NUWAX_SHORT_SPEECH_POLICY_MODE_ACTIVE=y' -or
    $sdkconfigDefaults -match 'CONFIG_NUWAX_SHORT_SPEECH_POLICY_MODE_SHADOW=y' -or
    $productOtaDefaults -notmatch 'CONFIG_NUWAX_ADAPTIVE_ENDPOINT_MODE_ACTIVE=y' -or
    $productOtaDefaults -match 'CONFIG_NUWAX_ADAPTIVE_ENDPOINT_MODE_SHADOW=y' -or
    $productOtaDefaults -notmatch 'CONFIG_NUWAX_SHORT_SPEECH_POLICY_MODE_ACTIVE=y' -or
    $productOtaDefaults -match 'CONFIG_NUWAX_SHORT_SPEECH_POLICY_MODE_SHADOW=y') {
    throw "Adaptive endpoint regression: product profiles must retain bounded four-state endpointing, seeded short-speech history, and fixed fallback for ineligible sessions."
}

$normalTerminalStart = $mainSource.IndexOf(
    "if (listen_started && send_ctx && send_ctx->task &&")
$normalTerminalEnd = if ($normalTerminalStart -ge 0) {
    $mainSource.IndexOf("(void)nuwax_audio_uplink_destroy_owned(&send_ctx);",
                        $normalTerminalStart)
} else { -1 }
$normalTerminalBlock = if ($normalTerminalStart -ge 0 -and
                                $normalTerminalEnd -gt $normalTerminalStart) {
    $mainSource.Substring($normalTerminalStart,
                          $normalTerminalEnd - $normalTerminalStart)
} else { "" }
$normalFinishIndex = $normalTerminalBlock.IndexOf(
    "nuwax_audio_uplink_finish(send_ctx)")
$normalEndCommittedIndex = $normalTerminalBlock.IndexOf(
    "nuwax_audio_uplink_end_committed(send_ctx)")
$normalListenStopIndex = $normalTerminalBlock.IndexOf(
    "nuwax_session_send_listen_stop_retry_if_epoch(")
if ($normalTerminalBlock.Length -eq 0 -or
    $normalFinishIndex -lt 0 -or
    $normalEndCommittedIndex -le $normalFinishIndex -or
    $normalListenStopIndex -le $normalEndCommittedIndex -or
    $normalTerminalBlock -notmatch 'listen_stop_protocol_safe' -or
    $normalTerminalBlock -notmatch 'send_ctx->terminal_recovery_bound' -or
    $normalTerminalBlock -notmatch 'const char \*stop_mode = send_ctx->terminal_recovery_mode' -or
    $normalTerminalBlock -notmatch 'send_ctx->terminal_recovery_audio_session_id' -or
    $normalTerminalBlock -notmatch 'send_ctx->terminal_recovery_request_id' -or
    $normalTerminalBlock -match '(?s)const char \*stop_mode =.*?nuwax_prelisten_state_mode_or' -or
    $normalTerminalBlock -match '(?s)const char \*stop_audio_session_id =.*?barge_audio_session_id' -or
    $normalTerminalBlock -notmatch 'listen stop sent after binary END') {
    throw "Normal listen terminal regression: the accepted utterance must commit binary END before sending stop from the immutable start-bound mode, session, and request identifiers."
}
if ($projectKconfig -notmatch 'choice NUWAX_SPEECH_CONFIRMATION_MODE(?s:.*?)default NUWAX_SPEECH_CONFIRMATION_MODE_SHADOW' -or
    $speechConfirmationHeader -notmatch 'sizeof\(nuwax_speech_confirmation_t\) <= 24U' -or
    $speechConfirmationSource -match 'malloc|calloc|realloc|free|xTaskCreate|xQueueCreate|xSemaphoreCreate|float|double' -or
    $mainSource -notmatch 'persistent_conversation_mode &&\s*!during_playback_mode &&\s*!playback_active &&\s*!acceptance_capture_enabled' -or
    $mainSource -notmatch 'legacy_speech_confirmed \|\| window_speech_confirmed' -or
    $mainSource -notmatch 'speech confirmation policy invalid; legacy consecutive gate restored' -or
    $sdkconfigDefaults -notmatch 'CONFIG_NUWAX_SPEECH_CONFIRMATION_MODE_LEGACY=y' -or
    $sdkconfigDefaults -match 'CONFIG_NUWAX_SPEECH_CONFIRMATION_MODE_ACTIVE=y' -or
    $productOtaDefaults -notmatch 'CONFIG_NUWAX_SPEECH_CONFIRMATION_MODE_LEGACY=y' -or
    $productOtaDefaults -match 'CONFIG_NUWAX_SPEECH_CONFIRMATION_MODE_ACTIVE=y') {
    throw "Speech confirmation regression: product profiles must use the consecutive-speech legacy owner; window confirmation may not admit ordinary conversation audio."
}
if ($mainSource -notmatch "NUWAX_MIC_CAPTURE_STOP_CAPTURE_SEQUENCE_GAP" -or
    $mainSource -notmatch 'capture sequence gap after uplink start; terminating session' -or
    $mainSource -notmatch '(?s)if \(capture_sequence != expected_sequence\).*?if \(upload_started\).*?ESP_ERR_INVALID_STATE.*?NUWAX_MIC_CAPTURE_STOP_CAPTURE_SEQUENCE_GAP.*?break;') {
    throw "Capture-integrity regression: a sequence gap after uplink commit must terminate the session instead of being hidden by wire renumbering."
}
if ($awakeStateSource -notmatch "state->waiting = false;(?s:.*?)state->paused_remaining_ms" -or
    $awakeStateSource -notmatch "nuwax_awake_session_state_reset_paused" -or
    $mainSource -notmatch 'reset_awake_wait_budget_paused\("natural_playback_complete"\)' -or
    $mainSource -notmatch 'resume_awake_waiting\("stable_listening_capture"\)' -or
    $mainSource -notmatch '(?s)if \(!vad_speech_seen.*?reset_awake_wait_budget_paused\(\s*during_playback_mode\s*\? "confirmed_human_speech_during_playback"\s*: "confirmed_human_speech"\)' -or
    $awakeStateSource -notmatch 'nuwax_awake_session_state_can_enter_idle_sleep' -or
    $awakeStateSource -notmatch 'nuwax_awake_session_state_can_commit_deferred_idle_sleep' -or
    $mainSource -notmatch 'deferred local sleep request discarded without idle authority' -or
    $mainSource -notmatch 'stale post-END timeout ignored' -or
    $mainSource -notmatch "recoverable_normal_conversation_error" -or
    $mainSource -notmatch '(?s)const bool ws_connected_after_source =\s*nuwax_ws_is_connected\(&ws\);.*?if \(!ws_connected_after_source\).*?break;.*?recoverable_normal_conversation_error.*?ws_connected_after_source' -or
    $mainSource -notmatch "normal conversation retained after local audio error" -or
    $mainSource -notmatch "stale awake timeout ignored" -or
    $listenResumeSource -notmatch "nuwax_listen_resume_state_event_matches" -or
    $listenResumeSource -notmatch "nuwax_listen_resume_state_holds_idle_expiry_boundary" -or
    $listenResumeSource -notmatch '(?s)nuwax_listen_resume_state_bind_round.*?idle_expiry_pending = false' -or
    $mainSource -notmatch '(?s)awake_idle_sleep_admitted.*?nuwax_listen_resume_state_holds_idle_expiry_boundary' -or
    $mainSource -notmatch '(?s)persistent_awake_timeout.*?nuwax_state_bridge_clear_pending_sleep_visual.*?enter_cloud_waiting_question\(\s*"awake_idle_timeout_post_end"\).*?awake idle timeout deferred after committed END; visual=question awaiting cloud turn' -or
    $mainSource -notmatch '(?s)else if \(auto_followup_mode &&\s*\(persistent_awake_timeout \|\| vad_no_speech_timeout\)\).*?metrics_cancel\(persistent_awake_timeout \? "awake_no_speech_timeout"' -or
    $mainSource -notmatch '(?s)idle_expiry_pending.*?awake idle boundary received no cloud turn before post-END deadline; entering sleep' -or
    $mainSource -notmatch "stale ASR event ignored" -or
    $mainSource -notmatch "conversation_audio_session_id") {
    throw "Awake-idle regression: count only stable user-wait time, reset after playback, retain recoverable captures, and isolate stale round events."
}
if ($capturePhaseSource -notmatch "nuwax_capture_phase_preserve_stale_transition" -or
    $capturePhaseSource -notmatch "nuwax_capture_phase_may_discard_uncommitted_candidate" -or
    $mainSource -notmatch "preserve_pending_handoff_frame" -or
    $mainSource -notmatch "const bool preserve_stale_capture_frame" -or
    $mainSource -notmatch "preserve_confirmed_barge_frame \|\|" -or
    $mainSource -notmatch "preserve_pending_handoff_frame;") {
    throw "Seamless playback handoff regression: retain the transition frame across bounded control/UI stalls."
}
if ($mainSource -notmatch "playback prelisten sequence boundary reset speculative candidate" -or
    $mainSource -notmatch "post-playback onset aged out or crossed a capture gap" -or
    $mainSource -notmatch 'preserve_post_playback_onset = false;\s+result = ESP_OK;' -or
    $capturePhaseSource -notmatch 'return during_playback_mode && !cloud_transaction_started;' -or
    $mainSource -notmatch 'nuwax_capture_phase_may_discard_uncommitted_candidate\(\s*during_playback_mode,\s*upload_started \|\| barge_protocol_enqueued \|\|\s*barge_protocol_listen_committed\)' -or
    $mainSource -match 'nuwax_capture_phase_may_discard_uncommitted_candidate\([^)]*barge_controller_confirmed') {
    throw "Playback-preroll recovery regression: an uncommitted stale candidate must restart from a fresh in-place boundary without stopping capture or WebSocket."
}
if ($speakerSource -match "speaker playback queue full, dropped oldest" -or
    $speakerSource -notmatch "CONFIG_NUWAX_SPEAKER_BUFFER_BYTES" -or
    $speakerSource -notmatch "xRingbufferCreateWithCaps" -or
    $speakerSource -notmatch "xRingbufferSendAcquire" -or
    $speakerSource -notmatch "speaker ingress backpressure" -or
    $speakerSource -notmatch "speaker ingress backpressure timed out" -or
    $speakerSource -notmatch "nuwax_speaker_backpressure_deadline_reached" -or
    $speakerSource -notmatch '"ingress_backpressure_timeout"' -or
    $speakerSource -notmatch "NUWAX_SPEAKER_RX_ADMISSION_BUDGET_MS 40U" -or
    $speakerSource -notmatch "speaker admission must not stall WebSocket RX" -or
    $speakerSource -notmatch "preserving PCM" -or
    $speakerSource -match "ingress_queue_full" -or
    $speakerSource -notmatch "speaker_schedule_protocol_fault" -or
    $speakerBackpressureSource -notmatch "nuwax_speaker_backpressure_poll_wait_ms" -or
    $speakerSource -notmatch "speaker_cancel_now_if_current" -or
    $speakerSource -notmatch "nuwax_speaker_cancel_expected_matches" -or
    $speakerSource -notmatch "nuwax_speaker_playback_request_cancel_generation" -or
    $sdkconfigDefaults -notmatch "CONFIG_NUWAX_SPEAKER_BUFFER_BYTES=393216" -or
    $speakerSource -notmatch "return ESP_ERR_NOT_FOUND") {
    throw "Speaker playback contract regression: require compact PSRAM buffering, bounded RX-safe admission, and generation-isolated cancellation."
}
if ($mainSource -notmatch "nuwax_aec_reference_select" -or
    $mainSource -notmatch "nuwax_aec_reference_epoch_contiguous" -or
    $mainSource -notmatch "frame_reference_provenance" -or
    $mainSource -notmatch "live_aec_reference_provenance") {
    throw "AEC reference provenance regression: hardware and software references must share generation-bound health metadata."
}
if ($speakerSource -notmatch "NUWAX_SPEAKER_JITTER_STARTUP_MIN_MS 480U" -or
    $speakerSource -notmatch "NUWAX_SPEAKER_JITTER_STARTUP_BASE_MS 800U" -or
    $speakerSource -notmatch "NUWAX_SPEAKER_JITTER_STARTUP_MAX_MS 1600U" -or
     $speakerSource -notmatch "NUWAX_SPEAKER_JITTER_RESUME_MIN_MS 640U" -or
     $speakerSource -notmatch "NUWAX_SPEAKER_JITTER_RESUME_MAX_MS 960U" -or
    $speakerSource -notmatch "NUWAX_SPEAKER_JITTER_EMPTY_GRACE_MS 80U" -or
    $speakerSource -notmatch "speaker_wait_for_jitter_watermark" -or
    $speakerSource -notmatch "speaker_packet_ring_samples" -or
    $speakerSource -notmatch "xTaskNotifyGive" -or
    $speakerSource -notmatch "ulTaskNotifyTake" -or
    $speakerSource -notmatch "last_ingress_progress_ms" -or
    $speakerSource -notmatch "jitter_rebuffer_events" -or
    $speakerSource -match "gap_silence_output_failure" -or
    $speakerJitterPolicy -notmatch "nuwax_speaker_jitter_audio_ms" -or
    $speakerJitterPolicy -notmatch "nuwax_speaker_jitter_ready_ms" -or
    $speakerJitterPolicy -notmatch "nuwax_speaker_jitter_should_enter_rebuffer" -or
    $speakerJitterPolicy -notmatch "nuwax_speaker_jitter_rebuffer_is_midstream" -or
    $speakerJitterPolicy -notmatch "nuwax_speaker_jitter_resume_target_ms" -or
    $speakerJitterPolicy -notmatch "nuwax_speaker_jitter_adapt_startup_ms" -or
    $speakerSource -notmatch "jitter_tail_wait_events" -or
    $speakerSource -notmatch "speaker latency breakdown" -or
    $speakerSource -notmatch "jitter_resume_fade_pending" -or
    $speakerSource -notmatch "on_ingress_progress" -or
    $speakerSource -notmatch "nuwax_speaker_playback_abort_metadata_only_session" -or
    $mainSource -notmatch "NUWAX_SPEAKER_BINARY_BATCH_ADMISSION_BUDGET_MS 40U" -or
    $mainSource -notmatch "binary_batch_admission_deadline_ms" -or
    $mainSource -notmatch "nuwax_speaker_playback_handle_audio_frame_admitted_with_timeout" -or
    $speakerSource -notmatch "bounded_admission_timeout_ms" -or
    $mainSource -notmatch "NUWAX_CONVERSATION_TTS_TIMEOUT_MS 30000U" -or
    $mainSource -notmatch "NUWAX_CONVERSATION_PLAYBACK_INGRESS_IDLE_TIMEOUT_MS 60000U" -or
    $mainSource -notmatch "tts_no_audio_timeout" -or
    $speakerSource -match "jitter_near_underrun_confirmed") {
     throw "Speaker jitter regression: require sample-duration adaptive 480..1600 ms startup buffering with an 800 ms safe default, 640..960 ms gap-aware recovery, a bounded binary-batch admission deadline, event wakeup, tail-wait isolation, recovery fade-in, ingress-progress stall timing, and no synthetic gap-silence writes."
}
if ($speakerSource -notmatch "nuwax_speaker_output_drain_and_stop" -or
    $speakerOutputSource -notmatch "nuwax_speaker_output_drain_and_stop" -or
    $speakerOutputHeader -notmatch "nuwax_speaker_output_drain_and_stop" -or
    $speakerOutputSource -notmatch "speaker natural drain complete" -or
    $speakerOutputHeader -notmatch "stop_settled" -or
    $speakerOutputSource -notmatch '(?s)if \(speaker->stop_settled &&\s*!speaker->shared_tx_recovery_required\).*?return ESP_OK;' -or
    $speakerOutputSource -notmatch '(?s)speaker->stop_settled = false;\s*if \(speaker->vocat_shared_tx && speaker->muted\)' -or
    $speakerOutputSource -notmatch '(?s)if \(preload_err == ESP_OK\).*?speaker->stop_settled = true;') {
    throw "Speaker terminal regression: natural END must drain the DMA tail before stop, while cancel/fault keeps the existing immediate stop path."
}
if ($speakerSource -notmatch "speaker_wait_for_start_authorization" -or
    $speakerSource -notmatch "nuwax_speaker_playback_authorize_generation" -or
    $speakerSource -notmatch "speaker_start_gate_authorized_locked" -or
    $speakerSource -notmatch "xRingbufferSendComplete" -or
    $speakerSource -match 'nuwax_speaker_playback_cancel_now\("queue_backpressure_timeout"\)' -or
    $mainSource -notmatch "speaker_callback_on_start_pending" -or
    $mainSource -notmatch "authorize_speaker_generation_for_active_turn" -or
    $mainSource -notmatch "NUWAX_SPEAKER_OWNER_EVENT_START_GATE_TIMEOUT" -or
    $mainSource -notmatch "NUWAX_SPEAKER_OWNER_EVENT_PLAYBACK_FAULT") {
    throw "Speaker START ownership regression: binary START must remain physically gated by validated turn/playback metadata, and RX backpressure must be owner-recovered without a synchronous speaker stop."
}
if ($speakerHeader -notmatch 'on_start_pending\)\(uint32_t connection_epoch,\s+uint32_t generation\)' -or
    $speakerHeader -notmatch 'on_playback_fault\)\(uint32_t connection_epoch,\s+uint32_t generation' -or
    $speakerHeader -notmatch 'failed_stop_connection_epoch' -or
    $speakerSource -notmatch 'submitted_connection_epoch' -or
    $speakerSource -notmatch 'cancel_stopped_connection_epoch' -or
    $speakerSource -notmatch 'protocol_fault_pending_connection_epoch' -or
    $speakerSource -notmatch 'speaker_generation_source_matches_locked' -or
    $speakerSource -notmatch 'callbacks\.on_playback_fault\(connection_epoch,' -or
    $mainSource -notmatch 'nuwax_speaker_playback_handle_audio_frame_admitted_with_timeout\(\s*ws_connection_epoch,\s*\(uint32_t\)binary_batch_epoch' -or
    $mainSource -notmatch 'stats\.failed_stop_connection_epoch' -or
    $mainSource -notmatch 'deferred\.ws_connection_epoch' -or
    $mainSource -match 'event\.ws_connection_epoch\s*=\s*active_ws_epoch_snapshot\(\)' -or
    $mainSource -notmatch 'speaker owner event missing source WebSocket epoch') {
    throw "Speaker WebSocket epoch regression: packets, generation state, terminal callbacks, deferred events, and stop-fault recovery must preserve their original connection epoch without sampling the current active connection."
}
$speakerProgressCallback = [regex]::Match(
    $mainSource,
    '(?s)static void speaker_callback_on_playback_progress.*?(?=\r?\nstatic void speaker_callback_on_ingress_progress)')
$speakerIngressCallback = [regex]::Match(
    $mainSource,
    '(?s)static void speaker_callback_on_ingress_progress.*?(?=\r?\nstatic bool speaker_callback_schedule_auto_followup)')
if ($speakerStreamPolicy -notmatch 'nuwax_speaker_stream_frame_is_orphan' -or
    $speakerSource -notmatch 'return ESP_ERR_NOT_FOUND;' -or
    $mainSource -notmatch 'orphan playback tail dropped' -or
    $mainSource -notmatch 'report_current_speaker_ingress_fault' -or
    $mainSource -notmatch 'speaker_owner_event_loss_is_critical' -or
    $speakerGenerationPolicy -notmatch 'nuwax_speaker_owner_loss_matches_turn' -or
    $mainSource -notmatch 's_speaker_owner_event_overflow_generation' -or
    $mainSource -notmatch 's_speaker_ingress_pending' -or
    $mainSource -notmatch 'Collapse accepted PCM ingress into a latest-wins slot' -or
    !$speakerProgressCallback.Success -or
    $speakerProgressCallback.Value -match 's_speaker_ingress_pending = true;' -or
    !$speakerIngressCallback.Success -or
    $speakerIngressCallback.Value -notmatch 's_speaker_ingress_pending = true;' -or
    $speakerIngressCallback.Value -match 'enqueue_speaker_owner_event') {
    throw "Speaker cross-turn isolation regression: orphan canceled-playback tails must be dropped, accepted ingress must be coalesced without using speaker output as cloud progress, and real owner loss must remain generation-bound."
}
if ($speakerSource -notmatch "nuwax_speaker_playback_request_probe_duck_generation" -or
    $speakerSource -notmatch "nuwax_speaker_probe_duck_allowed" -or
    $mainSource -notmatch "NUWAX_BARGE_EVENT_PROBE_REQUESTED" -or
    $mainSource -notmatch "NUWAX_BARGE_PROBE_HUMAN_PERSISTS" -or
    $mainSource -notmatch "NUWAX_BARGE_PROBE_ECHO_TRACKS_DUCK" -or
    $mainSource -notmatch "conversation_audio_health_process") {
    throw "Adaptive barge-in regression: generation-bound ambiguity duck and stateful AEC/reference health gating are required."
}
if ($bargeControllerSource -notmatch "high_volume_probe_required" -or
    $bargeControllerSource -notmatch "!high_volume_probe_required") {
    throw "High-volume barge-in regression: 76-100% playback candidates must pass the generation-bound duck probe before local stop."
}
if ($bargeControllerSource -notmatch "\.active_aec_lock_frames = 32U" -or
    $bargeControllerSource -notmatch "frame->aec_frame_processed" -or
    $bargeControllerSource -notmatch "active_aec_stable_frames" -or
    $mainSource -notmatch "\.aec_frame_processed = frame_aec_processed") {
    throw "Barge-in startup regression: local stop requires generation-bound active AEC/reference stabilization."
}
if ($bargeControllerSource -notmatch "\.minimum_guard_frames = 4U" -or
    $mainSource -match "nuwax_barge_in_playback_startup_guard_elapsed" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_CAPTURE_SLOTS 12U" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_CAPTURE_PRE_DSP_MAX_AGE_MS 60U" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_BARGE_END_TO_END_MAX_AGE_MS 100U" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_UPLINK_PACKET_SLOTS 64U" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_UPLINK_STAGING_MAX_AGE_MS 1000U" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_MIN_DSP_RESERVE_MS 40U" -or
    $latencyContract -notmatch "barge end-to-end age must reserve at least 40 ms for DSP" -or
    $mainSource -notmatch "NUWAX_CONVERSATION_CAPTURE_SLOT_DEPTH" -or
    $mainSource -notmatch "NUWAX_CONVERSATION_CAPTURE_PRE_DSP_MAX_AGE_MS" -or
    $bargeControllerSource -notmatch "NUWAX_CONVERSATION_BARGE_END_TO_END_MAX_AGE_MS" -or
    $mainSource -notmatch "stale capture frame dropped before DSP" -or
    $mainSource -notmatch "preserve_confirmed_barge_frame" -or
    $mainSource -notmatch "stale capture frame retained for %s transition" -or
    $mainSource -notmatch "NUWAX_CONVERSATION_EVENT_STALE_CAPTURE_FRAME" -or
    $sdkconfigDefaults -notmatch "CONFIG_NUWAX_RT_CAPTURE_QUEUE_DEPTH=12" -or
    $sdkconfigDefaults -notmatch "CONFIG_NUWAX_RT_CAPTURE_MAX_DECISION_AGE_MS=60" -or
    $sdkconfigDefaults -notmatch "CONFIG_NUWAX_BARGE_END_TO_END_MAX_FRAME_AGE_MS=100") {
    throw "Realtime decision-latency regression: preserve 12 capture slots, 60 ms pre-DSP admission, 40 ms DSP reserve, and a 100 ms barge decision limit."
}
if ($bargeControllerSource -notmatch "\.candidate_max_gap_frames = 1U" -or
    $bargeControllerSource -notmatch "NUWAX_CONVERSATION_BARGE_END_TO_END_MAX_AGE_MS" -or
    $mainSource -notmatch "symmetric_continuity_was_armed &&" -or
    $mainSource -notmatch "barge_spatial_symmetric_evidence = true" -or
    $mainSource -notmatch "!during_playback_mode && !playback_human_seed_active") {
    throw "Barge-in continuity regression: tolerate one clean evidence gap, align controller evidence with symmetric support, and defer speculative preroll repair."
}
if ($mainSource -notmatch "conversation_live_aec_pool_init" -or
    $mainSource -notmatch "conversation_live_aec_pool_acquire" -or
    $mainSource -notmatch "conversation_live_aec_pool_release" -or
    $mainSource -notmatch "conversation_live_aec_restore_production_state" -or
    $mainSource -notmatch "live_aec_custom_delay_exercised" -or
    $mainSource -notmatch "live_aec_runtime_faulted" -or
    $mainSource -notmatch "live_aec_raw_diagnostic_exercised" -or
    $mainSource -notmatch "acceptance_capture\.has_delay_samples" -or
    $mainSource -notmatch "next\.has_delay_samples = command->has_delay_samples" -or
    $mainSource -notmatch "live AEC production rebuild failed after diagnostic capture" -or
    $mainSource -notmatch "NUWAX_LIVE_AEC_POST_MIN_DMA_LARGEST_BYTES" -or
    $mainSource -notmatch "conversation live AEC pool invalidated" -or
    $mainSource -notmatch "nuwax_live_aec_begin_session\(&\(work->live_aec\)" -or
    $liveAecSource -notmatch "esp_err_t\s+nuwax_live_aec_begin_session" -or
    $liveAecSource -notmatch "!aec->human_rescue_source_chunk" -or
    $mainSource -match "live AEC preflight enabled") {
    throw "AEC lifecycle regression: reserve and reuse the ESP-SR backend, require the rescue buffer, and memory-check rebuilt production adaptation after raw or custom-delay diagnostics."
}
if ($speakerSource -notmatch "speaker_packet_ring_clear_locked") {
    throw "TTS START latency regression: stale playback generations must be cleared without copying every queued PCM packet."
}
$wakeStopCompletionReferences =
    [regex]::Matches($mainSource, "complete_during_playback_wake_stop\(").Count
if ($mainSource -notmatch "nuwax_wake_word_detector_request_stop" -or
    $wakeStopCompletionReferences -ne 3 -or
    $mainSource -notmatch "s_during_playback_wake_stop_playback_started" -or
    $wakeSource -notmatch "read_timeout_ms > 100U" -or
    $wakeSource -notmatch "nuwax_mic_input_read_pcm16\([\s\S]*?read_timeout_ms" -or
    $speakerSource -notmatch "submitted_generation" -or
    $speakerSource -notmatch "speaker_activate_start_packet" -or
    $speakerSource -notmatch "speaker_packet_is_submitted" -or
    $speakerSource -notmatch "speaker_claim_packet_io" -or
    $speakerSource -notmatch "output_stop_owner_generation" -or
    $speakerSource -notmatch "completion_claim_generation" -or
    $speakerSource -notmatch "nuwax_speaker_finish_claim_allowed" -or
    $speakerSource -notmatch "nuwax_speaker_stop_claim_decide" -or
    $speakerSource -notmatch "speaker_cancel_now_if_current" -or
    $speakerSource -notmatch "(?s)nuwax_speaker_start_submission_allowed\(\s*s_speaker_ctx\.io_claim_generation,\s*s_speaker_ctx\.completion_claim_generation,\s*s_speaker_ctx\.maintenance_claim_generation,\s*s_speaker_ctx\.cancel_pending_generation,\s*s_speaker_ctx\.output_stop_owner_generation,\s*s_speaker_ctx\.runtime_reset_in_progress\)" -or
    $speakerSource -notmatch "speaker_claim_idle_output_action" -or
    $speakerSource -notmatch "(?s)const bool io_claimed = speaker_claim_packet_io\(\s*packet\.connection_epoch,\s*packet\.generation\);.*?if \(io_claimed\).*?callbacks\.on_tts_start.*?packet\.connection_epoch.*?packet\.generation.*?nuwax_p3_software_ref_reset\(\).*?nuwax_speaker_output_write_pcm16") {
    throw "TTS START latency/generation regression: activation and reset must be speaker-task owned, cancel/write must be generation gated, and wake stop must be single-consumer and chunk bounded."
}
if ($mainSource -notmatch "s_conversation_state_lock" -or
    $mainSource -notmatch "nuwax_ui_sync_submit" -or
    $mainSource -notmatch "nuwax_ui_sync_complete" -or
    $mainSource -match "s_ui_state_sync\.pending\s*=\s*false" -or
    $mainSource -match "s_ui_speaking_status_sync") {
    throw "Conversation state/UI generation regression: state transitions and conditional UI completion must share the generation lock."
}
if ($mainSource -notmatch 'static bool invalidate_all_pending_semantic_emotions' -or
    $mainSource -notmatch '(?s)static bool app_state_blocks_semantic_emotion.*?NUWAX_APP_STATE_BOOTING.*?NUWAX_APP_STATE_WIFI_CONNECTING.*?NUWAX_APP_STATE_WS_CONNECTING.*?NUWAX_APP_STATE_LISTENING.*?NUWAX_APP_STATE_SLEEPING.*?NUWAX_APP_STATE_ERROR' -or
    $mainSource -notmatch '(?s)cancel_pending_semantic_emotion_for_state.*?invalidate_all_pending_semantic_emotions' -or
    $mainSource -notmatch '(?s)session_dispatch_set_emotion.*?app_state_blocks_semantic_emotion\(current_state\)' -or
    $mainSource -notmatch '(?s)nuwax_head_touch_agent_intent_mark_sent.*?invalidate_all_pending_semantic_emotions\(\s*is_shake \? "shake_round_committed" : "touch_round_committed"\).*?nuwax_state_bridge_pin_' -or
    $mainSource -notmatch '(?s)begin_conversation_visual_round\(.*?nuwax_conversation_visual_round_is_active.*?clear_conversation_visual_round\("visual_round_replaced"\).*?nuwax_conversation_visual_round_begin') {
    throw "Conversation visual ownership regression: strong lifecycle and interaction commits must fence queued semantic emotions, and a residual visual owner must be released before replacement."
}
if ($mainSource -notmatch '#define NUWAX_SLEEP_CLOUD_EMOTION_HOLD_MS 4000U' -or
    $mainSource -notmatch '(?s)static bool apply_sleep_cloud_emotion_name.*?NUWAX_SLEEP_CLOUD_EMOTION_HOLD_MS.*?sleep_owner_current.*?nuwax_state_bridge_invalidate_semantic_revisions_through.*?nuwax_state_bridge_clear_semantic_emotion_if_owner' -or
    $mainSource -notmatch '(?s)session queue full but semantic emotion was preserved.*?sleep_lease' -or
    $mainSource -notmatch '(?s)if \(current_state == NUWAX_APP_STATE_SLEEPING\).*?apply_sleep_cloud_emotion_name') {
    throw "Sleep cloud-emotion regression: normal and queue-full delivery must use a four-second finite lease bound to the current sleeping lifecycle."
}
if ($bridgeSource -notmatch "case NUWAX_APP_STATE_SLEEPING:\s+dialog = NUWAX_DIALOG_SLEEP" -or
    $bridgeSource -notmatch '(?s)case NUWAX_APP_STATE_THINKING:\s*return NUWAX_EMOTION_THINKING;' -or
    $bridgeSource -notmatch '(?s)nuwax_emotion_display_policy_enter_lifecycle\(.*?dialog\)' -or
    $shellSource -notmatch "static void app_shell_post_dialog" -or
    $shellSource -notmatch "s_state_mailbox\.app_state ==\s+NUWAX_APP_STATE_SLEEPING" -or
    $shellSource -notmatch "s_state_mailbox\.snapshot\.meeting == NUWAX_MEETING_UI_DISABLED" -or
    $shellSource -notmatch "s_state_mailbox\.snapshot\.monitoring ==\s+NUWAX_MONITORING_UI_DISABLED") {
    throw "UI contract regression: the moon icon must be controlled by the actual sleeping state only."
}
if ($mainSource -notmatch "conversation capture handoff requested from continuous uplink to during-playback prelisten" -or
    $mainSource -notmatch "complete_during_playback_capture_handoff" -or
    $mainSource -notmatch "nuwax_barge_dispatcher_current_binding\(ws\)" -or
    $mainSource -notmatch "nuwax_barge_dispatcher_enqueue_confirmed" -or
    $mainSource -notmatch "nuwax_barge_dispatcher_get_status" -or
    $mainSource -notmatch "barge_protocol_listen_committed" -or
    $dispatcherSource -notmatch "uint32_t ws_connection_epoch" -or
    $dispatcherSource -notmatch "s_bound_ws_epoch" -or
    $dispatcherSource -notmatch "snapshot_binding\(item\.binding_generation,\s+item\.ws_connection_epoch" -or
    $dispatcherSource -notmatch "s_binding_generation == expected_generation" -or
    $dispatcherSource -notmatch "s_bound_ws_epoch == expected_ws_epoch" -or
    $dispatcherSource -notmatch "failed_binding_is_current" -or
    $dispatcherSource -match "request_close_failed_binding_if_current" -or
    $dispatcherSource -match "nuwax_ws_request_close_if_epoch" -or
    $dispatcherSource -notmatch '(?s)binding_still_current.*?nuwax_session_send_barge_in_until_if_epoch\(\s*confirmed_ws,\s*"confirmed".*?item\.ws_connection_epoch\)' -or
    $dispatcherSource -notmatch 'nuwax_session_send_listen_with_audio_session_until_if_epoch' -or
    $sessionProtocolSource -notmatch 'nuwax_session_send_barge_in_until_if_epoch' -or
    $sessionProtocolSource -notmatch 'nuwax_session_send_listen_with_audio_session_until_if_epoch' -or
    $sessionProtocolSource -notmatch 'nuwax_ws_send_text_until_if_epoch' -or
    $dispatcherSource -notmatch 'nuwax_barge_dispatcher_cancel' -or
    $sdkconfigDefaults -notmatch "CONFIG_NUWAX_BARGE_IN_ENABLE=n" -or
    $bargeConfig -notmatch "CONFIG_NUWAX_BARGE_IN_ENABLE=y" -or
    $bargeConfig -notmatch "CONFIG_NUWAX_BARGE_IN_LOCAL_EARLY_CONFIRM=y" -or
    $bargeConfig -notmatch "CONFIG_NUWAX_BARGE_IN_CLOUD_GUARDED_HIGH_RECALL=y" -or
    $mainSource -notmatch 'tts_start_voice_barge_disabled' -or
    $mainSource -notmatch 'voice_barge_disabled_pre_mic' -or
    $mainSource -notmatch 'tts_pending_voice_barge_disabled') {
    throw "Device-owned voice barge-in must remain available behind the explicit diagnostic switch while product defaults keep it disabled."
}
if ($sdkconfigDefaults -notmatch 'CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE=n' -or
    $sdkconfigDefaults -notmatch 'CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE=y' -or
    $kconfigSource -notmatch '(?s)config NUWAX_TOUCH_WAKE_ACTION_ENABLE.*?default n' -or
    $kconfigSource -notmatch '(?s)config NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE.*?default n' -or
    $shellSource -notmatch 'static nuwax_app_shell_action_t app_shell_hold_action' -or
    $shellSource -notmatch 'CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE' -or
    $shellSource -notmatch 'CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE' -or
    $mainSource -notmatch 'head touch conversation control disabled wake_action=0 interrupt_action=0' -or
    $mainSource -notmatch '(?s)CONFIG_NUWAX_HEAD_TOUCH_ENABLE.*?CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE.*?CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE') {
    throw "Touch conversation control regression: product defaults must keep touch wake disabled and active-turn touch interruption enabled while ordinary UI gestures remain available."
}
if ($audioUplinkSource -notmatch "nuwax_audio_uplink_request_finish" -or
    $audioUplinkSource -notmatch "nuwax_audio_uplink_finish_is_complete" -or
    $mainSource -notmatch "seamless playback handoff phase switched at frame boundary" -or
    $mainSource -notmatch "post-playback seamless resume committed at frame boundary" -or
    $mainSource -notmatch "barge-to-awake old session draining after END frame" -or
    $mainSource -notmatch "rt_capture_reused=1 mic_reused=1 aec_reused=1" -or
    $mainSource -notmatch "nuwax_capture_phase_handoff_request_awake" -or
    $mainSource -notmatch "nuwax_capture_phase_handoff_request_playback_session_close" -or
    $mainSource -notmatch "nuwax_capture_phase_allows_new_session" -or
    $autoFollowupBlock -match 'cancel_auto_followup_capture\("natural_playback_complete"\)') {
    throw "Seamless capture phase regression: both awake->playback and playback->awake transitions must reuse RT capture/MIC/AEC, drain the old protocol session, and switch only at a processed frame boundary."
}
if ($audioUplinkHeader -notmatch "NUWAX_AUDIO_UPLINK_REALTIME_FINAL_ADMISSION_WAIT_MS 0U" -or
    $audioUplinkHeader -notmatch "NUWAX_AUDIO_UPLINK_REALTIME_TERMINAL_SEND_TIMEOUT_MS 2000U" -or
    $audioUplinkHeader -notmatch "nuwax_audio_uplink_can_skip_unsent_data_packet" -or
    $audioUplinkHeader -notmatch "nuwax_audio_uplink_reserves_terminal_slot" -or
    $audioUplinkSource -notmatch "NUWAX_AUDIO_UPLINK_IDLE_FALLBACK_WAIT_MS" -or
    $audioUplinkSource -notmatch "audio_uplink_notify_sender" -or
    $audioUplinkSource -notmatch '(?s)xQueueReceive\(ctx->ready_packets,\s*&packet,\s*0\).*?ulTaskNotifyTake' -or
    $audioUplinkSource -notmatch '(?s)xQueueSend\(ctx->ready_packets,\s*&packet,\s*0\).*?audio_uplink_notify_sender\(ctx\)' -or
    $audioUplinkSource -match 'xQueueReceive\(ctx->ready_packets,\s*&packet,\s*pdMS_TO_TICKS\(100\)\)' -or
    $audioUplinkSource -notmatch "const uint32_t ready_queue_depth =\s*nuwax_audio_uplink_ready_queue_depth" -or
    $audioUplinkSource -notmatch "const uint32_t pool_slot_count =\s*nuwax_audio_uplink_pool_slot_count" -or
    $audioUplinkSource -notmatch "final && ctx->terminal_slot_reserved" -or
    $audioUplinkSource -notmatch "realtime terminal packet admitted without capture-path wait" -or
    $audioUplinkSource -notmatch "audio_uplink_ensure_start_on_first_sent_packet" -or
    $audioUplinkSource -notmatch "nuwax_audio_uplink_can_skip_unsent_data_packet" -or
    $audioUplinkSource -notmatch "nuwax_audio_uplink_admission_wait_ms\(\s*ctx->profile,\s*true\)" -or
    $audioUplinkSource -notmatch "nuwax_audio_uplink_discard_uncommitted" -or
    $audioUplinkSource -notmatch "nuwax_audio_uplink_request_finish_then" -or
    $audioUplinkHeader -notmatch "NUWAX_AUDIO_UPLINK_AFTER_END_CONTEXT_MAX_BYTES" -or
    $audioUplinkSource -notmatch "memcpy\(ctx->after_end_user_ctx, user_ctx, user_ctx_size\)" -or
    $audioUplinkSource -notmatch "ctx->after_end\(\(void \*\)ctx->after_end_user_ctx" -or
    $mainSource -notmatch "send_seamless_listen_stop_after_end" -or
    $mainSource -notmatch "nuwax_audio_uplink_request_finish_then" -or
    $mainSource -notmatch "NUWAX_SEAMLESS_HANDOFF_DRAIN_TIMEOUT_MS" -or
    $mainSource -notmatch "seamless handoff END/listen-stop transaction timed out" -or
    $mainSource -notmatch "s_during_playback_handoff_seamless" -or
    $mainSource -notmatch "nuwax_capture_phase_resolve_early_playback_complete" -or
    $mainSource -notmatch "playback completed before cancel/rearm handoff cleanup" -or
    $mainSource -notmatch "nuwax_capture_phase_cancel_recovery_needs_resume" -or
    $mainSource -notmatch "speaker cancel recovery kept active %s capture and requested post-playback resume") {
    throw "Realtime terminal admission regression: queue-full END must use a dedicated packet/ready slot with zero capture-path wait, and an uncommitted sender must be discardable without binary output."
}

$handoffPollStart = $mainSource.IndexOf("if (handoff_old_session_closing) {")
$handoffPollEnd = if ($handoffPollStart -ge 0) {
    $mainSource.IndexOf("if (cloud_progress_watchdog_armed)", $handoffPollStart)
} else { -1 }
$handoffPollBlock = if ($handoffPollStart -ge 0 -and
                            $handoffPollEnd -gt $handoffPollStart) {
    $mainSource.Substring($handoffPollStart,
                          $handoffPollEnd - $handoffPollStart)
} else { "" }
if ($handoffPollBlock -match "nuwax_session_send_listen_with_audio_session") {
    throw "Seamless listen-stop regression: the capture loop must only poll the END/stop transaction; JSON stop belongs to the uplink terminal callback."
}

$micUploadPrepareStart = $mainSource.IndexOf(
    "static esp_err_t prepare_mic_cloud_upload(")
$micUploadStart = if ($micUploadPrepareStart -ge 0) {
    $mainSource.IndexOf("static esp_err_t start_mic_cloud_upload(",
                        $micUploadPrepareStart)
} else { -1 }
$micUploadPrepareBlock = if ($micUploadPrepareStart -ge 0 -and
                                  $micUploadStart -gt $micUploadPrepareStart) {
    $mainSource.Substring($micUploadPrepareStart,
                          $micUploadStart - $micUploadPrepareStart)
} else { "" }
$micUploadEnd = if ($micUploadStart -ge 0) {
    $mainSource.IndexOf("static esp_err_t enqueue_mic_uplink_frame(", $micUploadStart)
} else { -1 }
$micUploadBlock = if ($micUploadStart -ge 0 -and $micUploadEnd -gt $micUploadStart) {
    $mainSource.Substring($micUploadStart, $micUploadEnd - $micUploadStart)
} else { "" }
$micUploadPrepare = $micUploadPrepareBlock.IndexOf(
    "nuwax_audio_uplink_start_with_profile(")
$micUploadRecoveryBind = $micUploadPrepareBlock.IndexOf(
    "nuwax_audio_uplink_bind_terminal_recovery(")
$micUploadPrepareCall = $micUploadBlock.IndexOf("prepare_mic_cloud_upload(")
$micUploadListen = $micUploadBlock.IndexOf("nuwax_audio_uplink_schedule_listen_start(")
if ($micUploadPrepare -lt 0 -or
    $micUploadRecoveryBind -le $micUploadPrepare -or
    $micUploadPrepareCall -lt 0 -or
    $micUploadListen -le $micUploadPrepareCall -or
    $micUploadPrepareBlock -notmatch "nuwax_audio_uplink_discard_uncommitted" -or
    $micUploadPrepareBlock -match "nuwax_session_send_listen" -or
    $micUploadPrepareBlock -match "nuwax_ws_request_close" -or
    $micUploadBlock -notmatch "NUWAX_INTERACTIVE_CONTROL_SEND_TIMEOUT_MS" -or
    $micUploadBlock -notmatch "active_ws_epoch_snapshot\(\)" -or
    $micUploadBlock -notmatch "nuwax_ws_connection_epoch\(ws\) != expected_epoch" -or
    $micUploadBlock -notmatch "send_ctx->ws_connection_epoch != expected_epoch" -or
    $micUploadBlock -match "nuwax_session_send_listen_with_audio_session_until_if_epoch" -or
    $micUploadBlock -notmatch "nuwax_audio_uplink_discard_uncommitted" -or
    $micUploadBlock -notmatch "retaining WebSocket" -or
    $micUploadBlock -match "nuwax_ws_request_close") {
    throw "Listen-start transaction regression: local uplink resources and terminal recovery must be ready before bounded epoch-bound sender admission; the capture owner must not send control synchronously, and failed admission must discard the idle worker."
}
if ($audioUplinkSource -notmatch "esp_timer_get_time\(\) >= ctx->listen_start_deadline_us" -or
    $audioUplinkSource -notmatch "ctx->listen_start_deadline_us, ctx->ws_connection_epoch" -or
    $audioUplinkSource -notmatch "packet_start_err = audio_uplink_commit_listen_start\(ctx\)" -or
    $mainSource -notmatch "nuwax_audio_uplink_finish_is_complete\(send_ctx, NULL\)(?s:.*?)!nuwax_audio_uplink_listen_start_committed\(send_ctx\)") {
    throw "Ordered listen-start regression: sender deadline, frozen epoch, post-dequeue barrier and committed-only terminal cleanup must all remain present."
}
if ($dispatcherHeader -notmatch "NUWAX_BARGE_DISPATCH_LISTEN_ATTEMPTED" -or
    $dispatcherHeader -notmatch "NUWAX_BARGE_DISPATCH_COMMIT_UNCERTAIN" -or
    $dispatcherHeader -notmatch "nuwax_barge_dispatch_listen_may_be_committed" -or
    $dispatcherSource -notmatch "mark_listen_attempted" -or
    $dispatcherSource -notmatch "s_status_result = ESP_ERR_NOT_FINISHED" -or
    $dispatcherSource -notmatch '(?s)listen_attempted\s*\? nuwax_session_send_listen_with_audio_session_until_if_epoch.*?NUWAX_BARGE_DISPATCH_COMMIT_UNCERTAIN' -or
    $dispatcherSource -match '(?s)listen_attempted\s*\?\s*NUWAX_BARGE_DISPATCH_FAILED') {
    throw "Barge-in listen commit regression: once listen send is attempted, an error must remain commit-uncertain and require stop/close cleanup."
}
if ($audioUplinkSource -notmatch "nuwax_audio_uplink_start_if_epoch" -or
    $audioUplinkSource -notmatch "nuwax_audio_uplink_start_with_profile_if_epoch" -or
    $audioUplinkSource -notmatch "nuwax_ws_connection_epoch\(ws\) != expected_ws_epoch" -or
    $audioUplinkSource -notmatch "ctx->ws_connection_epoch = expected_ws_epoch" -or
    $p5ProbeSource -notmatch "nuwax_p5_fullduplex_overlap_probe_run_ex_if_epoch" -or
    $p5ProbeSource -notmatch "nuwax_session_send_listen_with_audio_session_until_if_epoch" -or
    $p5ProbeSource -notmatch "nuwax_audio_uplink_start_if_epoch" -or
    $p5ProbeSource -notmatch "nuwax_ws_connection_epoch\(ws\) == expected_ws_epoch" -or
    $mainSource -notmatch "run_p5_during_playback_candidate_manual\(ws,\s+request\.ws_connection_epoch" -or
    $mainSource -notmatch "nuwax_session_send_command_ack_until_if_epoch" -or
    $mainSource -notmatch "nuwax_session_send_cancel_turn_until_if_epoch") {
    throw "Async P5 WebSocket binding regression: listen, uplink, cancel, and ACK must remain bound to the request connection epoch."
}
$p5RunStart = $p5ProbeSource.IndexOf(
    "esp_err_t nuwax_p5_fullduplex_overlap_probe_run_ex_if_epoch(")
$p5RunEnd = if ($p5RunStart -ge 0) {
    $p5ProbeSource.IndexOf(
        "esp_err_t nuwax_p5_fullduplex_overlap_probe_run_ex(",
        $p5RunStart + 1)
} else { -1 }
$p5RunBlock = if ($p5RunStart -ge 0 -and $p5RunEnd -gt $p5RunStart) {
    $p5ProbeSource.Substring($p5RunStart, $p5RunEnd - $p5RunStart)
} else { "" }
$p5PrepareIndex = $p5RunBlock.IndexOf("nuwax_audio_uplink_start_if_epoch(")
$p5ListenStartIndex = $p5RunBlock.IndexOf(
    "nuwax_session_send_listen_with_audio_session_until_if_epoch(")
if ($p5RunBlock.Length -eq 0 -or
    $p5PrepareIndex -lt 0 -or
    $p5ListenStartIndex -le $p5PrepareIndex -or
    $p5RunBlock -notmatch "nuwax_audio_uplink_discard_uncommitted" -or
    $p5RunBlock -notmatch "nuwax_ws_request_close_if_epoch" -or
    $p5RunBlock -notmatch "listen_started && end_committed" -or
    $p5RunBlock -notmatch "listen stop suppressed because binary END was not committed") {
    throw "P5 reliable stream transaction regression: prepare uplink before listen-start, discard uncommitted starts, and send stop only after a proven binary END."
}
if ($mainSource -notmatch "NUWAX_BARGE_DISPATCH_COMMIT_UNCERTAIN" -or
    $mainSource -notmatch "barge listen transaction unresolved during cleanup" -or
    $mainSource -notmatch "completing END/stop when possible and retaining healthy WebSocket" -or
    $mainSource -notmatch "barge_listen_commit_uncertain" -or
    $mainSource -match "closing WebSocket without JSON stop because binary END cannot be proven" -or
    $mainSource -match "barge dispatcher status unavailable during cleanup") {
    throw "Barge-in uncertain listen cleanup regression: attempted or unknown commits must abort only the current transaction and retain WebSocket."
}

$cancelIndex = $mainSource.IndexOf("nuwax_speaker_playback_request_cancel_generation_ex(")
$stopArmedIndex = if ($cancelIndex -ge 0) {
    $mainSource.IndexOf("barge_stop_request_armed = true;", $cancelIndex)
} else { -1 }
$protocolEnqueueIndex = if ($stopArmedIndex -ge 0) {
    $mainSource.IndexOf("nuwax_barge_dispatcher_enqueue_confirmed(", $stopArmedIndex)
} else { -1 }
$candidateDispatchIndex = $dispatcherSource.IndexOf('"candidate"')
$confirmedDispatchIndex = if ($candidateDispatchIndex -ge 0) {
    $dispatcherSource.IndexOf('"confirmed"', $candidateDispatchIndex + 1)
} else { -1 }
$listenDispatchIndex = if ($confirmedDispatchIndex -ge 0) {
    $dispatcherSource.IndexOf('nuwax_session_send_listen_with_audio_session',
                              $confirmedDispatchIndex + 1)
} else { -1 }
if ($cancelIndex -lt 0 -or $stopArmedIndex -le $cancelIndex -or
    $protocolEnqueueIndex -le $stopArmedIndex -or
    $candidateDispatchIndex -lt 0 -or
    $confirmedDispatchIndex -le $candidateDispatchIndex -or
    $listenDispatchIndex -le $confirmedDispatchIndex) {
    throw "Barge-in ordering regression: generation-bound local stop must be armed before protocol enqueue, and the dispatcher must send candidate, confirmed, then listen start."
}
$trustedOnsetValidationIndex = $mainSource.IndexOf(
    "esp_err_t preroll_validation_err = ESP_OK;")
$deferredProtocolEnqueueIndex = if ($trustedOnsetValidationIndex -ge 0) {
    $mainSource.IndexOf("nuwax_barge_dispatcher_enqueue_confirmed(",
                        $trustedOnsetValidationIndex)
} else { -1 }
$bargePrepareIndex = if ($trustedOnsetValidationIndex -ge 0) {
    $mainSource.IndexOf("prepare_mic_cloud_upload(",
                        $trustedOnsetValidationIndex)
} else { -1 }
if ($trustedOnsetValidationIndex -lt 0 -or
    $bargePrepareIndex -le $trustedOnsetValidationIndex -or
    $deferredProtocolEnqueueIndex -le $trustedOnsetValidationIndex -or
    $deferredProtocolEnqueueIndex -le $bargePrepareIndex -or
    $mainSource.IndexOf("nuwax_barge_dispatcher_enqueue_confirmed(") -ne
        $deferredProtocolEnqueueIndex -or
    $mainSource -notmatch "protocol_deferred_until_preroll_valid=1" -or
    $mainSource -notmatch "barge-in degraded before protocol commit" -or
    $mainSource -notmatch "barge_audio_untrusted") {
    throw "Barge-in trusted-onset regression: candidate/confirmed/listen must remain deferred until the cleaned onset-to-confirmed span is validated."
}

$bargeCleanupStart = $mainSource.IndexOf(
    "if (barge_protocol_enqueued && !listen_started) {")
$bargeCleanupEnd = if ($bargeCleanupStart -ge 0) {
    $mainSource.IndexOf("#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS",
                        $bargeCleanupStart)
} else { -1 }
$bargeCleanupBlock = if ($bargeCleanupStart -ge 0 -and
                              $bargeCleanupEnd -gt $bargeCleanupStart) {
    $mainSource.Substring($bargeCleanupStart,
                          $bargeCleanupEnd - $bargeCleanupStart)
} else { "" }
$bargeCleanupResolveIndex = $bargeCleanupBlock.IndexOf(
    "nuwax_barge_dispatch_listen_may_be_committed(")
$bargeCleanupFinishIndex = $bargeCleanupBlock.IndexOf(
    "nuwax_audio_uplink_finish(send_ctx)")
if ($bargeCleanupBlock.Length -eq 0 -or
    $bargeCleanupResolveIndex -lt 0 -or
    $bargeCleanupFinishIndex -le $bargeCleanupResolveIndex -or
    $bargeCleanupBlock -notmatch "nuwax_audio_uplink_discard_uncommitted" -or
    $bargeCleanupBlock -notmatch "listen_stop_protocol_safe") {
    throw "Barge-in cleanup ordering regression: dispatcher commit ownership must resolve before prepared uplink END/discard and JSON listen-stop eligibility."
}

$physicalStopCallIndex = $speakerSource.IndexOf(
    "stop_err = nuwax_speaker_output_stop(&s_speaker_ctx.output);")
$physicalStopPublishIndex = if ($physicalStopCallIndex -ge 0) {
    $speakerSource.IndexOf("s_speaker_ctx.completed_stop_generation =",
                           $physicalStopCallIndex)
} else { -1 }
if ($physicalStopCallIndex -lt 0 -or
    $physicalStopPublishIndex -le $physicalStopCallIndex -or
    $mainSource -notmatch "speaker_completed_stop_generation" -or
    $mainSource -notmatch "speaker_completed_stop_timestamp_us" -or
    $mainSource -notmatch "NUWAX_BARGE_PHYSICAL_STOP_TIMEOUT_MS" -or
    $mainSource -notmatch "barge_physical_stop_observed" -or
    $mainSource -match "playback_already_stopped" -or
    $mainSource -match '(?s)speaker_completed_stop_timestamp_us\s*>=\s*barge_stop_requested_timestamp_us' -or
    $mainSource -notmatch "NUWAX_CONVERSATION_LATENCY_BARGE_CONFIRM_TO_LOCAL_STOP(?s:.*?)barge_physical_stop_timestamp_us") {
    throw "Physical stop timing regression: publish the generation-bound output-stop completion and use that timestamp for the guard and latency metrics."
}
if ($speakerSource -match "speaker_publish_cancel_timeout" -or
    $speakerSource -notmatch "nuwax_speaker_unconditional_cancel_join_target" -or
    $speakerSource -notmatch "speaker unconditional cancel joined existing stop" -or
    $speakerSource -notmatch "SPEAKER_JOINED_STOP_FAILED" -or
    $speakerSource -notmatch "speaker unconditional cancel retrying joined failed stop" -or
    $speakerSource -notmatch "speaker cancel recovered output-stop ownership" -or
    $speakerSource -notmatch "NUWAX_SPEAKER_RUNTIME_RESET_MAX_STOP_RETRIES" -or
    $speakerSource -notmatch "speaker runtime reset aborted with unresolved stop" -or
    $speakerSource -notmatch "stop_fault_active" -or
    $speakerSource -notmatch "failed_stop_generation") {
    throw "Speaker stop transaction regression: unconditional cancel must join the active generation, timeout recovery must perform a real output stop, and failed stop attempts must remain distinct from completion."
}

$uploadFailureLogIndex = $mainSource.IndexOf("confirmation is not reverted")
$uploadStartIndex = if ($uploadFailureLogIndex -ge 0) {
    $mainSource.LastIndexOf("result = start_mic_cloud_upload(ws,", $uploadFailureLogIndex)
} else { -1 }
$uploadCommittedIndex = if ($uploadFailureLogIndex -ge 0) {
    $mainSource.IndexOf("upload_started = true;", $uploadFailureLogIndex)
} else { -1 }
if ($uploadStartIndex -lt 0 -or $uploadCommittedIndex -le $uploadFailureLogIndex) {
    throw "Confirmed barge-in upload failure contract is missing."
}
$confirmedUploadBlock = $mainSource.Substring(
    $uploadStartIndex,
    $uploadCommittedIndex + "upload_started = true;".Length - $uploadStartIndex)
if ($confirmedUploadBlock -match '"rejected"' -or
    $confirmedUploadBlock -match "nuwax_session_send_barge_in" -or
    $dispatcherSource -match '"rejected"') {
    throw "Terminal barge-in regression: an upload-start failure must not emit rejected after confirmed."
}

$localCancelIndex = $mainSource.IndexOf("barge_local_cancel_requested = true;")
$postCancelUploadIndex = if ($localCancelIndex -ge 0) {
    $mainSource.IndexOf("result = start_mic_cloud_upload(ws,", $localCancelIndex)
} else { -1 }
$postCancelGateBlock = if ($localCancelIndex -ge 0 -and
                              $postCancelUploadIndex -gt $localCancelIndex) {
    $mainSource.Substring($localCancelIndex,
                          $postCancelUploadIndex - $localCancelIndex)
} else { "" }
if ($mainSource -notmatch "#define NUWAX_BARGE_IN_LOCAL_FADE_MS 0U" -or
    $mainSource -notmatch "#define NUWAX_BARGE_IN_POST_CANCEL_GUARD_MS 96U" -or
    $mainSource -notmatch "#define NUWAX_BARGE_IN_PREROLL_LEAD_FRAMES 4U" -or
    $mainSource -notmatch "#define NUWAX_BARGE_IN_OVERLAP_MAX_REF_CORR_Q15 12000U" -or
    $mainSource -notmatch "persistent_conversation_mode &&\s+s_during_playback_handoff_pending" -or
    $mainSource -notmatch "seamless playback handoff phase switched at frame boundary" -or
    $mainSource -notmatch "(?s)barge_local_cancel_requested &&\s+barge_physical_stop_observed.*?barge_post_cancel_guard_frames--.*?barge_post_stop_guard_frame = true" -or
    $mainSource -notmatch "sanitize_barge_overlap_preroll" -or
    $mainSource -notmatch "repair_auto_followup_human_preroll\((?s:.*?)NUWAX_BARGE_IN_PREROLL_LEAD_FRAMES" -or
    $mainSource -notmatch "sanitize_barge_overlap_preroll\((?s:.*?)repair_actual_start_sequence" -or
    $mainSource -notmatch "nuwax_preroll_overlap_frame_should_silence" -or
    $mainSource -notmatch "nuwax_preroll_audio_trust_allows_preserve" -or
    $mainSource -notmatch "verified_silent_passthrough" -or
    $mainSource -notmatch "NUWAX_BARGE_SILENT_REFERENCE_STABLE_FRAMES" -or
    $mainSource -notmatch "speaker_stats_before" -or
    $mainSource -notmatch "speaker_playback_snapshot_stable" -or
    $mainSource -notmatch "speaker_volume_snapshot_stable" -or
    $mainSource -notmatch "silence_barge_preroll_range" -or
    $mainSource -notmatch "fail-closed silence failed" -or
    $mainSource -notmatch "forced_silence" -or
    $prerollWindowHeader -notmatch "NUWAX_PREROLL_CONFIRMED_GAP_MAX_FRAMES 8U" -or
    $mainSource -notmatch "repair_confirmed_barge_preroll_gap" -or
    $mainSource -notmatch "confirmed barge-in capture gap repaired with forced silence" -or
    $mainSource -notmatch "audio session terminated and WebSocket retained" -or
    $mainSource -notmatch "frame->speech = false;(?s:.*?)frame->barge_silenced = true;(?s:.*?)frame->forced_silence = true;" -or
    $mainSource -notmatch "checking_confirmed_span = false;" -or
    $mainSource -notmatch "if \(upload_started\) \{(?s:.*?)NUWAX_MIC_CAPTURE_STOP_CAPTURE_SEQUENCE_GAP" -or
    $mainSource -notmatch "!window\.onset_found \|\| !window\.onset_included" -or
    $mainSource -notmatch "validate_auto_followup_preroll_for_upload" -or
    $mainSource -notmatch "output_underrun_delta" -or
    $mainSource -notmatch "frame->sequence != expected_sequence" -or
    $mainSource -notmatch "memset\(frame->samples, 0, frame->sample_count \* sizeof\(frame->samples\[0\]\)\)" -or
    $mainSource -notmatch "(?s)barge_local_cancel_requested &&\s+barge_post_stop_guard_frame.*?store_auto_followup_preroll_frame.*?sanitize_barge_overlap_preroll" -or
    $mainSource -notmatch "barge-in post-stop guard resolved" -or
    $mainSource -notmatch "speaker_pending \|\|(?s:.*?)!barge_physical_stop_observed \|\|(?s:.*?)barge_post_cancel_guard_frames > 0" -or
    $mainSource -notmatch "barge_post_cancel_guard_frames\s*=\s*NUWAX_BARGE_IN_POST_CANCEL_GUARD_FRAMES" -or
    $mainSource -notmatch "speaker_completed_stop_generation" -or
    $mainSource -notmatch "barge_physical_stop_timestamp_us" -or
    $mainSource -notmatch "preserves only frames whose cleanup can be proven" -or
    $postCancelGateBlock.Length -eq 0 -or
    $postCancelGateBlock -match "vad_speech_seen = false" -or
    $mainSource -match "preroll_write = 0;\s+preroll_count = 0;\s+playback_human_preroll_repair_pending = false") {
    throw "Clean barge-in uplink regression: confirmation must not wait for a second VAD; preserve sequence/sample duration while zeroing unsafe overlap and tail frames."
}
if ($mainSource -notmatch "ASR no-speech playback guard; keep during-playback capture" -or
    $mainSource -notmatch 'request_during_playback_capture\("asr_no_speech_playback_guard"' -or
    $mainSource -notmatch "normal prelisten blocked during speaker playback" -or
    $mainSource -notmatch 'nuwax_prelisten_state_start_capture\(&s_prelisten,\s+"during_playback"' -or
    $prelistenSource -notmatch "if \(!state \|\| !state->capture_active\)") {
    throw "Playback prelisten ownership regression: a late ASR no-speech result must not replace the active during-playback capture or start a new normal uplink."
}

if ($mainSource -notmatch '(?s)static bool publish_listening_after_current_asr_failure.*?nuwax_state_bridge_clear_waiting_question\(\);.*?set_conversation_status_guarded\("VOICE", "LISTENING", "listening", true\);.*?set_state\(NUWAX_APP_STATE_LISTENING\);' -or
    $mainSource -notmatch '(?s)publish_listening_after_current_asr_failure.*?playback_owner_active.*?during_playback_capture.*?exclusive_owner_active.*?sleep_owner_active.*?turn_owner_active.*?interaction_owner_active' -or
    $mainSource -notmatch '(?s)resume_prelisten_after_no_speech\(void\).*?ASR no-speech playback guard.*?if \(!publish_listening_after_current_asr_failure\("asr_no_speech"\)\).*?return true;' -or
    $mainSource -notmatch '(?s)set_conversation_status_guarded\("ASR ERROR", "ERROR",.*?listening_published =\s*publish_listening_after_current_asr_failure\("asr_error"\);.*?if \(!listening_published\).*?break;.*?resume_awake_conversation_after_inactivity\("asr_error"\)') {
    throw "ASR failure recovery regression: the current failed round must return to listening without overriding playback or interaction owners."
}

if ($mainSource -notmatch '(?s)shake_listening_bypasses_chat_guard.*?NUWAX_AGENT_INTERACTION_SHAKE.*?NUWAX_HEAD_TOUCH_AGENT_SOURCE_LISTENING' -or
    $mainSource -notmatch '(?s)shake_listening_bypasses_chat_guard.*?nuwax_head_touch_agent_intent_begin_quiesce' -or
    $mainSource -notmatch '(?s)shake_listening_bypasses_chat_guard.*?touch keeps the speech-protection policy below' -or
    $mainSource -notmatch '(?s)shake_listening_bypasses_chat_guard.*?NUWAX_HEAD_TOUCH_AGENT_FRAME_BEGIN_QUIESCE') {
    throw "Shake listening policy regression: only the SHAKE/LISTENING source may bypass chat speech arbitration; touch and all other sources must retain the quiet/user-speech guard."
}

if ($mainSource -notmatch '(?s)Diagnostics share the serialized WebSocket TX path with microphone.*?if \(!nuwax_audio_uplink_sender_busy\(\) \|\|\s*exclusive_audio_mode_blocks_normal_audio\(\)\).*?nuwax_device_error_report_service\(ws\)') {
    throw "Realtime uplink arbitration regression: diagnostic transmission must remain deferred while the microphone sender owns WebSocket TX."
}

if ($micSource -notmatch "recovery read forced" -or
    $micSource -notmatch "Always consume one DMA buffer" -or
    $micSource -notmatch "mic_rx_overflow_begin\(mic, &overflow_before\)" -or
    $micSource -notmatch "mic_rx_overflow_end\(mic,\s*samples_read,\s*overflow_before,\s*discontinuous_before_read,\s*is_i2s_read_error_usable" -or
    $micHeader -notmatch "rx_overflow_recovery_pending" -or
    $wakeSource -notmatch "mic_input consumed and rejected a discontinuous DMA frame" -or
    $wakeSource -notmatch "Retry immediately so RX catches up") {
    throw "RX DMA overflow recovery regression: consume one discontinuous frame and retry without a delay."
}

if ($mainSource -notmatch 'NUWAX_CONVERSATION_PLAYBACK_INGRESS_IDLE_TIMEOUT_MS 60000U' -or
    $mainSource -notmatch 'speaker_callback_on_ingress_progress' -or
    $mainSource -notmatch 's_speaker_ingress_pending = true' -or
    $mainSource -notmatch 'Consuming already-buffered PCM does not prove that the cloud is still' -or
    $mainSource -notmatch 'tts_no_audio_timeout' -or
    $mainSource -match 'NUWAX_CONVERSATION_PLAYBACK_ABSOLUTE_TIMEOUT_MS' -or
    $mainSource -match 'playback_absolute_timeout' -or
    $kconfigSource -match 'config NUWAX_CONVERSATION_PLAYBACK_ABSOLUTE_TIMEOUT_MS' -or
    $sdkconfigDefaults -match 'CONFIG_NUWAX_CONVERSATION_PLAYBACK_ABSOLUTE_TIMEOUT_MS' -or
    $sdkconfig -match 'CONFIG_NUWAX_CONVERSATION_PLAYBACK_ABSOLUTE_TIMEOUT_MS' -or
    $mainSource -notmatch 'update_normal_downlink_admission\(false\)[\s\S]*?nuwax_session_send_cancel_turn_until_if_epoch' -or
    $mainSource -notmatch 'nuwax_playback_terminal_guard_mark_fault\([\s\S]*?nuwax_speaker_playback_request_cancel_generation\([\s\S]*?timeout_reason') {
    throw "Playback ingress-idle watchdog regression: accepted cloud PCM alone must refresh a 60-second sliding deadline, with no absolute duration cap and bounded terminal convergence."
}

if ($speakerSource -notmatch '(?s)nuwax_speaker_playback_note_playback_end.*?if \(matched\).*?active_owner = NUWAX_SPEAKER_PLAYBACK_OWNER_NONE' -or
    $speakerSource -notmatch '(?s)nuwax_speaker_playback_note_notification_end.*?if \(matched\).*?active_owner = NUWAX_SPEAKER_PLAYBACK_OWNER_NONE') {
    throw "Speaker metadata ownership regression: matching terminal metadata must clear a cancelled or rejected owner once no stream work remains."
}

$wakeReaperStart = $wakeSource.IndexOf(
    "static bool detector_reap_exited_task(void)")
$wakeReaperEnd = if ($wakeReaperStart -ge 0) {
    $wakeSource.IndexOf("#if CONFIG_NUWAX_WAKE_WORD_BACKEND_WAKENET",
                        $wakeReaperStart)
} else { -1 }
$wakeReaperBlock = if ($wakeReaperStart -ge 0 -and
                          $wakeReaperEnd -gt $wakeReaperStart) {
    $wakeSource.Substring($wakeReaperStart,
                          $wakeReaperEnd - $wakeReaperStart)
} else { "" }
$wakeFinalLogIndex = $wakeSource.IndexOf('"wake input final frames=')
$wakeFinalFreeIndex = if ($wakeFinalLogIndex -ge 0) {
    $wakeSource.IndexOf("free(samples);", $wakeFinalLogIndex)
} else { -1 }
$wakeExitPublishIndex = if ($wakeFinalFreeIndex -ge 0) {
    $wakeSource.IndexOf(
        "s_detector.task_state = NUWAX_WAKE_TASK_EXITING;",
        $wakeFinalFreeIndex)
} else { -1 }
$wakeSelfSuspendIndex = if ($wakeExitPublishIndex -ge 0) {
    $wakeSource.IndexOf("vTaskSuspend(NULL);", $wakeExitPublishIndex)
} else { -1 }
$wakePostPublishBlock = if ($wakeExitPublishIndex -ge 0 -and
                               $wakeSelfSuspendIndex -gt
                                   $wakeExitPublishIndex) {
    $wakeSource.Substring(
        $wakeExitPublishIndex,
        $wakeSelfSuspendIndex - $wakeExitPublishIndex)
} else { "" }
if ($wakeReaperBlock.Length -eq 0 -or
    $wakeReaperBlock -match 'vTaskSuspend\(task\)' -or
    $wakeReaperBlock -notmatch 'task_state != eSuspended' -or
    $wakeReaperBlock -notmatch 'did not self-suspend' -or
    $wakeFinalLogIndex -lt 0 -or
    $wakeFinalFreeIndex -le $wakeFinalLogIndex -or
    $wakeExitPublishIndex -le $wakeFinalFreeIndex -or
    $wakeSelfSuspendIndex -le $wakeExitPublishIndex -or
    $wakePostPublishBlock -match 'ESP_LOG[A-Z]*\(' -or
    $wakePostPublishBlock -match '(heap_caps_)?free\(') {
    throw "Wake detector lifecycle regression: finish logging and temporary-buffer cleanup before publishing EXITING, then self-suspend without asynchronous owner suspension."
}

if ($wakeHeader -notmatch "nuwax_wake_word_detector_stop_with_timeout" -or
    $wakeSource -notmatch "NUWAX_WAKE_WORD_STOP_TIMEOUT_MS" -or
    $wakeSource -notmatch "NUWAX_WAKE_CAPTURE_START_TIMEOUT_MS" -or
    $wakeSource -notmatch "NUWAX_WAKE_TASK_SUSPEND_SETTLE_TIMEOUT_MS" -or
    $wakeSource -notmatch "wake capture did not release microphone" -or
    $audioUplinkSource -notmatch "NUWAX_AUDIO_UPLINK_REAPER_CANCEL_GRACE_MS" -or
    $audioUplinkSource -notmatch "NUWAX_AUDIO_UPLINK_REAPER_FATAL_RESTART_MS" -or
    $audioUplinkSource -notmatch "NUWAX_AUDIO_UPLINK_TASK_SUSPEND_SETTLE_MS" -or
    $audioUplinkSource -notmatch "uplink sender did not release singleton" -or
    $audioUplinkSource -notmatch "audio_uplink_log_stalled_sender" -or
    $audioUplinkSource -notmatch "NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY" -or
    $mainSource -notmatch "NUWAX_WAKE_SUPERVISOR_READY_TIMEOUT_MS" -or
    $mainSource -notmatch "nuwax_deferred_state_submit" -or
    $mainSource -notmatch "service_deferred_speaker_state" -or
    $micSource -notmatch "teardown remains retryable" -or
    $micSource -notmatch "controlled restart preserves generation isolation" -or
    $micSource -notmatch "release_owner\(mic\);\s*if \(mic->board_ref.token == 0\)" -or
    $mainSource -notmatch "static portMUX_TYPE s_ws_activity_lock" -or
    $mainSource -notmatch "portENTER_CRITICAL\(&s_ws_activity_lock\)" -or
    $mainSource -notmatch "esp_task_wdt_add\(NULL\)" -or
    $mainSource -notmatch "app_task_watchdog_checkpoint\(\)" -or
    $sdkconfigDefaults -notmatch '(?m)^CONFIG_ESP_TASK_WDT_TIMEOUT_S=30\s*$' -or
    $sdkconfigDefaults -notmatch '(?m)^CONFIG_ESP_TASK_WDT_PANIC=y\s*$' -or
    $sdkconfig -notmatch '(?m)^CONFIG_ESP_TASK_WDT_TIMEOUT_S=30\s*$' -or
    $sdkconfig -notmatch '(?m)^CONFIG_ESP_TASK_WDT_PANIC=y\s*$') {
    throw "Runtime liveness regression: bounded wake/uplink cleanup, synchronized WebSocket activity, and the product application-task watchdog must remain enabled."
}

if ($mainSource -match 'reconnecting even if transport pong remains healthy' -or
    $mainSource -notmatch 'transport heartbeat remains authoritative and WebSocket is retained' -or
    $mainSource -notmatch '(?s)NUWAX_CLOUD_MESSAGE_PONG.*?nuwax_ws_note_heartbeat_if_epoch' -or
    $wsHeader -notmatch 'last_heartbeat_epoch' -or
    $wsHeader -notmatch 'nuwax_ws_advisory_close_if_epoch' -or
    $wsHeader -notmatch 'nuwax_ws_request_close_if_epoch' -or
    $wsHeader -notmatch 'nuwax_ws_close_if_epoch' -or
    $wsHeader -notmatch 'nuwax_ws_force_close_if_epoch' -or
    $wsSource -notmatch 'NUWAX_WS_HEARTBEAT_HEALTHY_MS\s+60000U' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_HEARTBEAT_EXPIRED' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_AUTH_REJECTED' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER' -or
    $wsClosePolicy -notmatch '(?s)nuwax_ws_close_policy_admits.*?!heartbeat_healthy.*?nuwax_ws_close_cause_bypasses_heartbeat' -or
    $wsSource -notmatch '(?s)nuwax_ws_close_policy_admits\(cause, heartbeat_healthy\).*?WebSocket close suppressed while heartbeat is healthy' -or
    $wsSource -notmatch '(?s)nuwax_ws_advisory_close_if_epoch\(.*?NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY' -or
    $wsSource -notmatch '(?s)nuwax_ws_request_close_if_epoch\(.*?NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY' -or
    $wsSource -notmatch '(?s)nuwax_ws_force_close_if_epoch\(.*?NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER' -or
    $wsSource -notmatch '(?s)nuwax_ws_owner_close\(.*?nuwax_ws_heartbeat_healthy_if_epoch.*?required or ownership close admission is required' -or
    $mainSource -notmatch '(?s)cloud application liveness failed.*?NUWAX_WS_CLOSE_CAUSE_HEARTBEAT_EXPIRED' -or
    $mainSource -notmatch '(?s)cloud rejected application authentication.*?NUWAX_WS_CLOSE_CAUSE_AUTH_REJECTED' -or
    $mainSource -notmatch 'NUWAX_POST_END_RESPONSE_TIMEOUT_MS\s+15000U' -or
    $mainSource -notmatch '(?s)post-END cloud response timed out.*?NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED' -or
    $mainSource -notmatch '(?s)"%s session queue full event=%d command=%s; closing bound epoch so transaction can be retried".*?is_notification_control \? "notification" : "critical".*?nuwax_ws_request_close_if_epoch' -or
    $mainSource -notmatch '(?s)cancel_turn send failed.*?nuwax_ws_request_close_if_epoch' -or
    $mainSource -notmatch '(?s)forcing WebSocket reconnect for OTA state refresh.*?nuwax_ws_request_close_if_epoch' -or
    $mainSource -notmatch '(?s)wait_for_cloud_owner_to_park_for_reprovision.*?nuwax_ws_force_close_if_epoch' -or
    $mainSource -notmatch '(?s)audio source yielded to BLE provisioning.*?nuwax_ws_force_close_if_epoch' -or
    $audioUplinkSource -notmatch 'safe_unsent_terminal_timeout' -or
    $audioUplinkSource -notmatch '(?s)bool terminal_recovery_armed = false;.*?recovery_err == ESP_OK.*?terminal_recovery_armed = true;.*?if \(!transport_ended && !terminal_recovery_armed && ctx->ws' -or
    $audioUplinkSource -notmatch '(?s)mark_err == ESP_OK.*?audio_uplink_arm_terminal_recovery.*?audio_uplink_arm_synthesized_terminal_recovery.*?ctx->batch_start_seq' -or
    $audioUplinkSource -notmatch '(?s)if \(end_committed\).*?audio_uplink_arm_terminal_recovery\(\s*ctx, NULL, 0U, true\).*?terminal_snapshot_len > 0U.*?audio_uplink_arm_synthesized_terminal_recovery' -or
    $audioUplinkSource -notmatch '(?s)audio_uplink_ensure_start_on_first_sent_packet\(ctx, &packet\).*?audio_uplink_arm_synthesized_terminal_recovery\(\s*ctx, packet\.start_seq\)' -or
    $audioUplinkSource -notmatch '(?s)!terminal_recovery_armed &&\s*!nuwax_audio_terminal_recovery_pending\(\).*?audio_uplink_request_integrity_close' -or
    $audioUplinkSource -notmatch 'nuwax_audio_terminal_recovery_arm' -or
    $audioUplinkSource -notmatch 'nuwax_audio_terminal_recovery_complete_if_matches' -or
    $audioUplinkSource -notmatch 'nuwax_audio_uplink_sender_busy' -or
    $audioTerminalRecoverySource -notmatch 'RECOVERY_PHASE_ARMING' -or
    $audioTerminalRecoverySource -notmatch 'nuwax_audio_uplink_sender_busy' -or
    $audioTerminalRecoverySource -notmatch 'nuwax_ws_send_binary_with_timeout_if_epoch' -or
    $audioTerminalRecoverySource -notmatch 'nuwax_session_send_listen_with_audio_session_until_if_epoch' -or
    $audioTerminalRecoverySource -notmatch '(?s)nuwax_audio_terminal_recovery_should_quarantine.*?NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY' -or
    $wsSource -notmatch '(?s)send_all_until.*?close_requested_for_epoch\(client, tx_epoch\).*?ESP_ERR_INVALID_STATE') {
    throw "WebSocket close-admission regression: heartbeat-clean business recovery must retain the connection, terminal sends must retry only safe-unsent work, and hard close remains explicit."
}

Write-Host "conversation source contracts: PASS"
& py -3 (Join-Path $PSScriptRoot 'test-uplink-listen-start.py')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'run-control-ack-host-checks.ps1') -Gcc $Gcc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& py -3 (Join-Path $PSScriptRoot 'test-control-ack-routing.py')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'run-ws-frame-host-checks.ps1') -Gcc $Gcc
exit $LASTEXITCODE
