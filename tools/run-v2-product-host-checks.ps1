param(
    [string]$Gcc = "gcc"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot "build-host-checks\v2-product"
$cjsonDir = Join-Path $projectRoot "managed_components/espressif__cjson/cJSON"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

function Invoke-Checked([scriptblock]$Command, [string]$Label) {
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

Invoke-Checked {
    & py -3 (Join-Path $projectRoot "tools/verify-emotion-pack.py")
} "production emotion pack verification"

$emotionDeltaExe = Join-Path $outputDir "emotion_delta_format.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/emotion_delta_format_host_check.c") `
        -o $emotionDeltaExe
} "emotion delta format compile"
Invoke-Checked { & $emotionDeltaExe } "emotion delta format contract"

$emotionSegmentPolicyExe = Join-Path $outputDir "emotion_segment_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main/ui_host_stubs')" `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/emotion_segment_policy_host_check.c") `
        -o $emotionSegmentPolicyExe
} "emotion segment policy compile"
Invoke-Checked { & $emotionSegmentPolicyExe } "emotion segment policy contract"

$emotionComponentBridgeExe = Join-Path $outputDir "emotion_component_bridge_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main/ui_host_stubs')" `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/emotion_component_bridge_policy_host_check.c") `
        -o $emotionComponentBridgeExe
} "emotion component bridge policy compile"
Invoke-Checked { & $emotionComponentBridgeExe } "emotion component bridge policy contract"

$controlCenterTimingExe = Join-Path $outputDir "control_center_timing_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/control_center_timing_policy_host_check.c") `
        -o $controlCenterTimingExe
} "control-center timing policy compile"
Invoke-Checked { & $controlCenterTimingExe } "control-center timing policy"

$terminalExe = Join-Path $outputDir "audio_uplink_terminal.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'tools')" `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/audio_frame.c") `
        (Join-Path $projectRoot "main/conversation/audio_uplink_terminal.c") `
        (Join-Path $projectRoot "main/audio_frame_terminal_host_check.c") `
        -o $terminalExe
} "audio terminal compile"
Invoke-Checked { & $terminalExe } "audio terminal contract"

$uplinkLatencyExe = Join-Path $outputDir "audio_uplink_low_latency.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main/audio_modes/meeting/host_stubs')" `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/audio_uplink_low_latency_host_check.c") `
        -o $uplinkLatencyExe
} "audio uplink low-latency compile"
Invoke-Checked { & $uplinkLatencyExe } "audio uplink low-latency policy"

$dialogPolicyExe = Join-Path $outputDir "dialog_state_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/dialog_state_policy_host_check.c") `
        -o $dialogPolicyExe
} "dialog state policy compile"
Invoke-Checked { & $dialogPolicyExe } "dialog state policy"

$wifiSavedListExe = Join-Path $outputDir "wifi_saved_network_list.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/wifi_saved_network_list_host_check.c") `
        -o $wifiSavedListExe
} "Wi-Fi saved-list compile"
Invoke-Checked { & $wifiSavedListExe } "Wi-Fi saved-list compaction"

$otaPolicyExe = Join-Path $outputDir "ota_release_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/ota_release_policy_host_check.c") `
        -o $otaPolicyExe
} "OTA release policy compile"
Invoke-Checked { & $otaPolicyExe } "OTA release policy"

$firmwareReportPolicyExe = Join-Path $outputDir "firmware_report_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/firmware_report_policy_host_check.c") `
        -o $firmwareReportPolicyExe
} "firmware report policy compile"
Invoke-Checked { & $firmwareReportPolicyExe } "firmware report policy"

$firmwareIdentityPolicyExe = Join-Path $outputDir "firmware_identity_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/firmware_identity_policy_host_check.c") `
        -o $firmwareIdentityPolicyExe
} "firmware identity policy compile"
Invoke-Checked { & $firmwareIdentityPolicyExe } "firmware identity policy"

$otaRetryPolicyExe = Join-Path $outputDir "ota_retry_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/ota_retry_policy_host_check.c") `
        -o $otaRetryPolicyExe
} "OTA retry policy compile"
Invoke-Checked { & $otaRetryPolicyExe } "OTA retry policy"

$otaPowerPolicyExe = Join-Path $outputDir "ota_power_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/ota_power_policy_host_check.c") `
        -o $otaPowerPolicyExe
} "OTA power policy compile"
Invoke-Checked { & $otaPowerPolicyExe } "OTA power policy"

$otaUiProgressPolicyExe = Join-Path $outputDir "ota_ui_progress_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/ota_ui_progress_policy_host_check.c") `
        -o $otaUiProgressPolicyExe
} "OTA UI progress policy compile"
Invoke-Checked { & $otaUiProgressPolicyExe } "OTA UI progress policy"

$batteryPolicyExe = Join-Path $outputDir "battery_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/battery_policy_host_check.c") `
        -o $batteryPolicyExe
} "battery policy compile"
Invoke-Checked { & $batteryPolicyExe } "battery management policy"

$otaIdentityRecoveryPolicyExe = Join-Path $outputDir "ota_identity_recovery_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/ota_identity_recovery_policy_host_check.c") `
        -o $otaIdentityRecoveryPolicyExe
} "OTA identity recovery policy compile"
Invoke-Checked { & $otaIdentityRecoveryPolicyExe } "OTA identity recovery policy"

$cloudProtocolExe = Join-Path $outputDir "cloud_protocol_compat.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        "-I$cjsonDir" `
        (Join-Path $projectRoot "main/cloud_protocol_compat.c") `
        (Join-Path $cjsonDir "cJSON.c") `
        (Join-Path $projectRoot "main/cloud_protocol_compat_host_check.c") `
        -o $cloudProtocolExe
} "formal cloud compatibility compile"
Invoke-Checked { & $cloudProtocolExe } "formal cloud compatibility contract"

$cloudEpochPolicyExe = Join-Path $outputDir "cloud_connection_epoch_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/cloud_connection_epoch_policy_host_check.c") `
        -o $cloudEpochPolicyExe
} "cloud connection epoch policy compile"
Invoke-Checked { & $cloudEpochPolicyExe } "cloud connection epoch policy"

$mcpCapabilityExe = Join-Path $outputDir "mcp_capability.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main/audio_modes/meeting/host_stubs')" `
        "-I$(Join-Path $projectRoot 'main')" `
        "-I$cjsonDir" `
        (Join-Path $projectRoot "main/mcp_server.c") `
        (Join-Path $cjsonDir "cJSON.c") `
        (Join-Path $projectRoot "main/mcp_server_capability_host_check.c") `
        -o $mcpCapabilityExe
} "MCP capability compile"
Invoke-Checked { & $mcpCapabilityExe } "MCP capability contract"

$emotionFacePolicyExe = Join-Path $outputDir "emotion_face_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main/ui_host_stubs')" `
        "-I$(Join-Path $projectRoot 'main')" `
        "-I$(Join-Path $projectRoot 'main/nuwax_ui_v28')" `
        (Join-Path $projectRoot "main/nuwax_ui_v28/nuwax_face.c") `
        (Join-Path $projectRoot "main/emotion_face_policy_host_check.c") `
        -o $emotionFacePolicyExe
} "emotion face policy compile"
Invoke-Checked { & $emotionFacePolicyExe } "emotion face policy contract"

$emotionDisplayPolicyExe = Join-Path $outputDir "emotion_display_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/nuwax_ui_v28/emotion_display_policy.c") `
        (Join-Path $projectRoot "main/emotion_display_policy_host_check.c") `
        -o $emotionDisplayPolicyExe
} "emotion display policy compile"
Invoke-Checked { & $emotionDisplayPolicyExe } "emotion display policy contract"

$uiBridgeRevisionPolicyExe = Join-Path $outputDir "ui_bridge_revision_policy.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/ui_bridge_revision_policy_host_check.c") `
        -o $uiBridgeRevisionPolicyExe
} "UI bridge revision policy compile"
Invoke-Checked { & $uiBridgeRevisionPolicyExe } "UI bridge revision policy contract"

$wakeInterruptFilterExe = Join-Path $outputDir "wake_word_interrupt_filter.exe"
Invoke-Checked {
    & $Gcc -std=c11 -Wall -Wextra -Werror `
        "-I$(Join-Path $projectRoot 'main')" `
        (Join-Path $projectRoot "main/conversation/wake_word_interrupt_filter.c") `
        (Join-Path $projectRoot "main/wake_word_interrupt_filter_host_check.c") `
        -o $wakeInterruptFilterExe
} "wake word interrupt filter compile"
Invoke-Checked { & $wakeInterruptFilterExe } "wake word interrupt filter contract"

$kconfig = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/Kconfig.projbuild")
$defaults = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "sdkconfig.defaults")
$productDefaults = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "sdkconfig.product-ota.defaults")
$uiPerfDefaults = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "sdkconfig.ui-perf.defaults")
$bargeConfig = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "sdkconfig.barge-final")
$projectCmake = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "CMakeLists.txt")
$cmake = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/CMakeLists.txt")
$uplink = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/conversation/audio_uplink.c")
$audioTerminalRecovery = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/conversation/audio_terminal_recovery.c")
$uplinkHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/conversation/audio_uplink.h")
$latencyContract = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/conversation/conversation_latency_contract.h")
$bargeController = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/conversation/barge_in_controller.c")
$meeting = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/audio_modes/meeting/meeting_mode.c")
$monitoring = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/audio_modes/monitoring/monitoring_mode.c")
$session = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/session_protocol.c")
$cloudProtocol = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/cloud_protocol_compat.c")
$mcpServer = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/mcp_server.c")
$main = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ai_partner_main.c")
$wsTransport = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/ws_transport.c")
$wsTransportHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/ws_transport.h")
$wsClosePolicy = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/ws_close_policy.h")
$turnGuard = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/conversation/turn_guard.c")
$speakerPlayback = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/conversation/speaker_playback.c")
$provision = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_provision_new/nuwax_provision_main.c")
$otaDevice = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ota_new/nuwax_ota_device.c")
$otaDeviceHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ota_new/nuwax_ota_device.h")
$otaMain = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ota_new/nuwax_ota_main.c")
$otaReleasePolicy = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ota_new/nuwax_ota_release_policy.h")
$otaResultAckPolicy = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ota_new/nuwax_ota_result_ack_policy.h")
$firmwareReport = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ota_new/nuwax_firmware_report.c")
$deviceUid = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_provision_new/nuwax_device_uid.c")
$powerCtrl = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/board/nuwax_power_ctrl.c")
$boardProfile = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/board/nuwax_board.c")
$audioBoard = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/board/audio_board.c")
$vocatAudioBoard = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/board/vocat_audio_board.c")
$productReady = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/product_ready.c")
$productReadyHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/product_ready.h")
$timeSync = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/time_sync.c")
$wifiManager = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/wifi_manager.c")
$touchInput = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/touch_input.c")
$appShell = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/app_shell.c")
$display = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/display.c")
$displayHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/display.h")
$lvglPort = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_lvgl_port.c")
$lvglPortHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_lvgl_port.h")
$touchGesture = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/conversation/touch_gesture.c")
$headTouch = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/conversation/head_touch_control.c")
$boardInputs = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/board/board_gpio_inputs.c")
$shakeDetector = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/board/shake_detector.c")
$shakeDetectorHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/board/shake_detector.h")
$deviceState = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ui_v28/nuwax_device_state.h")
$drawer = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ui_v28/nuwax_drawer.c")
$drawerHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ui_v28/nuwax_drawer.h")
$controlCenterIcons = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ui_v28/assets/nuwax_control_center_icons.c")
$uiApp = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ui_v28/nuwax_app.c")
$faceAdapter = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ui_v28/nuwax_face.c")
$emotionPack = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ui_v28/face/emotion_asset_pack.c")
$emotionPlayer = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ui_v28/face/emotion_animation_player.c")
$stateBridge = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "main/nuwax_ui_v28/nuwax_state_bridge.c")
$partitionTable = Get-Content -Raw -LiteralPath (Join-Path $projectRoot "partitions.customwake.csv")

$canonicalEmotionNames = @(
    "dizziness", "happy", "excited", "sleeping", "provisioning",
    "disconnected", "thinking", "thought_complete", "meeting",
    "monitoring", "touch", "playful", "like", "angry", "question"
)
foreach ($emotionName in $canonicalEmotionNames) {
    $escapedEmotionName = [regex]::Escape($emotionName)
    if ($stateBridge -notmatch
        "emotion_name_equals\(name,\s*`"$escapedEmotionName`"\)") {
        throw "Canonical cloud emotion is not accepted by the device: $emotionName"
    }
}

$setEmotionCallback = [regex]::Match(
    $main,
    '(?s)static bool session_dispatch_set_emotion\(.*?\n\}').Value
if ($setEmotionCallback.Length -eq 0 -or
    $setEmotionCallback -notmatch
        'apply_semantic_emotion_name\(emotion,\s*"device_command"\)' -or
    $setEmotionCallback -match 'sleep_transaction_blocks_new_audio') {
    throw "set_emotion must remain a UI-only command and must not use the sleep audio gate."
}
$applySemanticEmotion = [regex]::Match(
    $main,
    '(?s)static bool apply_semantic_emotion_name\(.*?\n\}').Value
if ($applySemanticEmotion.Length -eq 0 -or
    $applySemanticEmotion -match '!ui_ready_snapshot\(\)' -or
    $stateBridge -notmatch
        'nuwax_emotion_display_policy_apply_cloud') {
    throw "Cloud emotions must queue across UI startup/rebuild and enter the bounded display policy."
}
if ($faceAdapter -notmatch
        '(?s)touch_visible\s*=\s*state->touch_emotion_active.*?if \(touch_visible\).*?nuwax_emotion_animation_player_trigger_touch\(\).*?else if \(asset == NUWAX_EMOTION_ASSET_TOUCH\).*?nuwax_emotion_animation_player_set_asset\(asset\)') {
    throw "Physical touch insertion and cloud touch display must remain separate policy paths."
}
if ($deviceState -notmatch 'bool\s+semantic_emotion_active' -or
    $faceAdapter -notmatch
        '(?s)if \(state->semantic_emotion_active\).*?semantic_asset\(state->emotion\)') {
    throw "Semantic emotion ownership must remain explicit below real system-state owners."
}

$appMainOffset = $main.IndexOf("void app_main(void)")
if ($appMainOffset -lt 0) {
    throw "app_main was not found."
}
$appMain = $main.Substring($appMainOffset)

if ($productReady -notmatch 'actionable_degraded_mask' -or
    $productReady -notmatch 'ESP_LOG_LEVEL_LOCAL\(' -or
    $timeSync -match 'ESP_LOGW\(TAG, "Waiting for SNTP time sync' -or
    $powerCtrl -match 'ESP_LOGW\(TAG, "mode=HOLD:' -or
    $powerCtrl -match 'ESP_LOGW\(TAG, "Miaopan power:' -or
    $boardProfile -match 'ESP_LOGW\(TAG, "  STRAP pins') {
    throw "Startup diagnostics regression: normal retries and validated board state must remain informational while actionable readiness failures stay warnings."
}

if ($main -notmatch '#define NUWAX_WS_APPLICATION_ACTIVITY_TIMEOUT_MS\s+45000' -or
    $main -notmatch '(?s)static void ws_mark_application_activity\(void\).*?s_ws_last_application_activity_ms\s*=\s*now_ms_i64\(\)' -or
    $main -notmatch '(?s)static void on_ws_text\(.*?ws_mark_application_activity\(\)' -or
    $main -notmatch '(?s)static void on_ws_text\(.*?nuwax_ota_device_handle_text\(payload, len,\s*ws_connection_epoch\).*?on_ws_text_non_ota\(payload, len, ws, ws_connection_epoch\)' -or
    $main -notmatch '(?s)static void on_ws_binary\(.*?ws_mark_application_activity\(\)' -or
    $main -notmatch '(?s)service_application_keepalive\(.*?ws_check_activity_timeout\(now_ms\).*?nuwax_ws_request_close_if_epoch' -or
    $main -notmatch '(?s)prepare_exclusive_audio_cloud_handoff\(.*?nuwax_meeting_mode_request_handoff_stop.*?nuwax_monitoring_mode_request_handoff_stop' -or
    $main -notmatch '(?s)begin_exclusive_audio_takeover\(.*?nuwax_sleep_transaction_reset_for_wake.*?clear_sleep_request' -or
    $main -notmatch '(?s)static void sync_exclusive_audio_mode_ui_state\(void\).*?s_ui_lifecycle_generation.*?nuwax_meeting_mode_state.*?nuwax_monitoring_mode_state.*?nuwax_app_shell_set_meeting_state.*?nuwax_app_shell_set_monitoring_state' -or
    $main -notmatch '(?s)static void shell_task\(void \*arg\).*?sync_pending_ui_state\(\);.*?sync_exclusive_audio_mode_ui_state\(\);.*?sync_pending_semantic_emotion\(\);' -or
    $main -notmatch '(?s)static bool meeting_or_monitoring_mode_blocks_sleep\(void\)\s*\{[^}]*nuwax_meeting_mode_has_audio_claim[^}]*nuwax_monitoring_mode_has_audio_claim[^}]*\}' -or
    $main -notmatch '(?s)static bool exclusive_audio_mode_blocks_normal_audio\(void\)\s*\{[^}]*nuwax_meeting_mode_has_pending[^}]*nuwax_monitoring_mode_has_pending[^}]*\}' -or
    $main -notmatch '(?s)static void service_pending_sleep_request\(void\)\s*\{.{0,1200}?meeting_or_monitoring_mode_blocks_sleep\(\).{0,800}?clear_sleep_request\(\).{0,400}?conversation_capture_busy_for_sleep\(\)' -or
    $main -notmatch '(?s)static void request_sleeping_conversation_state\(const char \*reason\)\s*\{.{0,600}?meeting_or_monitoring_mode_blocks_sleep\(\).{0,400}?clear_sleep_request\(\).{0,400}?return;' -or
    $main -notmatch '(?s)static bool commit_sleeping_conversation_state\(const char \*reason\)\s*\{.{0,600}?meeting_or_monitoring_mode_blocks_sleep\(\).{0,400}?return false;.{0,5000}?set_state\(NUWAX_APP_STATE_SLEEPING\).{0,800}?return true;' -or
    $main -notmatch '(?s)service_sleep_transaction\(void\).*?if \(!commit_sleeping_conversation_state\("remote_sleep_transaction"\)\) \{.*?return;.*?nuwax_sleep_transaction_commit_sleeping' -or
    $main -notmatch '(?s)static esp_err_t run_audio_source\(.*?nuwax_monitoring_mode_state.*?nuwax_meeting_mode_state.*?nuwax_monitoring_mode_has_pending.*?nuwax_meeting_mode_has_pending' -or
    $main -notmatch '(?s)conversation_was_in_flight.*?if \(!exclusive_mode_was_active\).*?preserve_awake_session_for_ws_reconnect\(.*?ws_disconnect_inflight_aborted.*?nuwax_speaker_playback_reset_runtime\(\)' -or
    $main -notmatch 'invalid inbound WebSocket text dropped.*transport retained and application liveness not refreshed' -or
    $main -match '(?s)if \(!nuwax_session_validate_text_frame\(payload, len\)\)\s*\{(?:(?!\n\s*return;).)*nuwax_ws_request_close_if_epoch' -or
    $speakerPlayback -notmatch 'esp_err_t nuwax_speaker_playback_reset_runtime\(void\)' -or
    $speakerPlayback -notmatch 'speaker runtime reset timed out waiting for reset owner' -or
    $meeting -notmatch 'nuwax_meeting_mode_request_handoff_stop' -or
    $monitoring -notmatch 'nuwax_monitoring_mode_request_handoff_stop') {
    throw "Cloud recovery and exclusive-audio takeover must remain application-liveness bound, ordered, and physically convergent."
}

if ($main -match 'reconnecting even if transport pong remains healthy' -or
    $main -notmatch 'transport heartbeat remains authoritative and WebSocket is retained' -or
    $main -notmatch '(?s)NUWAX_CLOUD_MESSAGE_PONG.*?nuwax_ws_note_heartbeat_if_epoch' -or
    $wsTransportHeader -notmatch 'last_heartbeat_epoch' -or
    $wsTransportHeader -notmatch 'nuwax_ws_advisory_close_if_epoch' -or
    $wsTransportHeader -notmatch 'nuwax_ws_request_close_if_epoch' -or
    $wsTransportHeader -notmatch 'nuwax_ws_close_if_epoch' -or
    $wsTransportHeader -notmatch 'nuwax_ws_force_close_if_epoch' -or
    $wsTransport -notmatch 'NUWAX_WS_HEARTBEAT_HEALTHY_MS\s+60000U' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_HEARTBEAT_EXPIRED' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_AUTH_REJECTED' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_OTA_STATE_REFRESH' -or
    $wsClosePolicy -notmatch 'NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER' -or
    $wsClosePolicy -notmatch '(?s)nuwax_ws_close_policy_admits.*?!heartbeat_healthy.*?nuwax_ws_close_cause_bypasses_heartbeat' -or
    $wsTransport -notmatch '(?s)nuwax_ws_close_policy_admits\(cause, heartbeat_healthy\).*?WebSocket close suppressed while heartbeat is healthy' -or
    $wsTransport -notmatch '(?s)nuwax_ws_request_close_if_epoch\(.*?NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY' -or
    $wsTransport -notmatch '(?s)nuwax_ws_force_close_if_epoch\(.*?NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER' -or
    $wsTransport -notmatch '(?s)nuwax_ws_owner_close\(.*?nuwax_ws_heartbeat_healthy_if_epoch.*?required or ownership close admission is required' -or
    $main -notmatch '(?s)cloud application liveness failed.*?NUWAX_WS_CLOSE_CAUSE_HEARTBEAT_EXPIRED' -or
    $main -notmatch '(?s)cloud rejected application authentication.*?NUWAX_WS_CLOSE_CAUSE_AUTH_REJECTED' -or
    $main -notmatch 'NUWAX_POST_END_RESPONSE_TIMEOUT_MS\s+15000U' -or
    $main -notmatch '(?s)post-END cloud response timed out.*?NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED' -or
    $main -notmatch '(?s)critical session queue full.*?nuwax_ws_request_close_if_epoch' -or
    $main -notmatch '(?s)cancel_turn send failed.*?nuwax_ws_request_close_if_epoch' -or
    $main -notmatch '(?s)forcing WebSocket reconnect for OTA state refresh.*?NUWAX_WS_CLOSE_CAUSE_OTA_STATE_REFRESH' -or
    $main -notmatch '(?s)wait_for_cloud_owner_to_park_for_reprovision.*?nuwax_ws_force_close_if_epoch' -or
    $main -notmatch '(?s)audio source yielded to BLE provisioning.*?nuwax_ws_force_close_if_epoch') {
    throw "WebSocket close-admission regression: heartbeat-clean business recovery must retain the connection and hard close remains explicit."
}

$runtimeReprovisionStart = $main.IndexOf(
    "static esp_err_t run_runtime_ble_reprovision(void)")
$runtimeReprovisionEnd = if ($runtimeReprovisionStart -ge 0) {
    $main.IndexOf(
        "static bool ota_continuous_audio_busy",
        $runtimeReprovisionStart)
} else {
    -1
}
$runtimeReprovisionBlock = if ($runtimeReprovisionStart -ge 0 -and
    $runtimeReprovisionEnd -gt $runtimeReprovisionStart) {
    $main.Substring(
        $runtimeReprovisionStart,
        $runtimeReprovisionEnd - $runtimeReprovisionStart)
} else {
    ""
}
$runtimeReprovisionOrderValid = $runtimeReprovisionBlock.Length -gt 0
$runtimeReprovisionCursor = -1
foreach ($step in @(
    'wait_for_cloud_owner_to_park_for_reprovision()',
    'nuwax_wifi_set_provisioning_active(true)',
    'nuwax_provision_main_reset_wifi_credentials()',
    'deinit_wake_model_serialized()',
    'conversation_live_aec_pool_suspend_for_provisioning()',
    'nuwax_speaker_playback_deinit_output()',
    'nuwax_provision_main_run_runtime_rebind(',
    'restore_audio_after_runtime_provisioning()',
    'nuwax_provision_main_rollback_wifi_credentials()',
    'clear_reprovision_reservation(true)'
)) {
    if (-not $runtimeReprovisionOrderValid) {
        break
    }
    $nextStep = $runtimeReprovisionBlock.IndexOf(
        $step,
        $runtimeReprovisionCursor + 1,
        [System.StringComparison]::Ordinal)
    if ($nextStep -lt 0) {
        $runtimeReprovisionOrderValid = $false
        break
    }
    $runtimeReprovisionCursor = $nextStep
}

if ($main -match 'return "audio capture active"' -or
    $main -match 'return "audio playback active"' -or
    $main -notmatch 's_reprovision_quiesce_requested' -or
    $main -notmatch 's_conversation_capture_starting' -or
    $main -notmatch 's_conversation_capture_runtime_active' -or
    $main -notmatch '(?s)wait_for_mic_start_action.*?s_reprovision_quiesce_requested.*?normal listen wait preempted by BLE reprovision' -or
    $main -notmatch '(?s)while \(nuwax_ws_is_connected\(ws\)\).*?s_reprovision_quiesce_requested.*?NUWAX_MIC_CAPTURE_STOP_REPROVISION' -or
    $main -notmatch '(?s)nuwax_mic_input_init_with_owner\(&mic, NUWAX_MIC_OWNER_CONVERSATION_CAPTURE\).*?s_conversation_capture_runtime_active.*?true' -or
    $main -notmatch '(?s)nuwax_mic_input_deinit\(&mic\).*?s_conversation_capture_runtime_active.*?false' -or
    $main -notmatch 'NUWAX_REPROVISION_AUDIO_QUIESCE_TIMEOUT_MS' -or
    $main -notmatch '(?s)BLE reprovision audio quiesce started.*?s_conversation_capture_starting.*?s_conversation_capture_runtime_active.*?nuwax_speaker_playback_has_pending.*?nuwax_wake_word_detector_is_running.*?BLE reprovision audio paths are idle.*?run_runtime_ble_reprovision\(\)' -or
    -not $runtimeReprovisionOrderValid -or
    $main -notmatch '(?s)static void __attribute__\(\(noreturn\)\) rt_capture_restart_preserving_ownership.*?controlled restart preserves task, I2S, queue and stack ownership.*?esp_restart\(\);' -or
    $main -notmatch '(?s)static esp_err_t rt_capture_stop\(.*?stop_acknowledged.*?if \(ctx->task && !stop_acknowledged\).*?rt_capture_restart_preserving_ownership.*?vTaskSuspend\(ctx->task\).*?task_state != eSuspended.*?rt_capture_restart_preserving_ownership.*?vTaskDelete\(ctx->task\);' -or
    $main -notmatch '(?s)conversation_live_aec_pool_init\(void\).*?s_conversation_live_aec_pool_initialized = false;.*?s_conversation_live_aec_pool_initializing = false;.*?return err;' -or
    $main -notmatch '(?s)service_conversation_live_aec_pool_recovery\(void\).*?s_reprovision_quiesce_requested.*?s_conversation_capture_runtime_active.*?conversation_live_aec_pool_init\(\)' -or
    $main -notmatch '(?s)service_conversation_live_aec_pool_recovery\(void\).*?!cloud_service_is_ready\(\).*?!nuwax_awake_session_state_is_active' -or
    $main -notmatch '(?s)service_offline_wake_word_detector\(.*?s_reprovision_quiesce_requested.*?conversation_capture_busy_for_sleep\(\).*?nuwax_awake_session_state_is_active.*?request_sleep_wake_word_detector' -or
    $main -notmatch '(?s)while \(true\).*?service_conversation_live_aec_pool_recovery\(\).*?service_offline_wake_word_detector\("network_offline"\).*?recover_wifi_if_needed\(\)' -or
    $main -notmatch '(?s)while \(nuwax_ws_is_connected\(&ws\)\).*?service_conversation_live_aec_pool_recovery\(\).*?run_audio_source\(&ws\)' -or
    $main -notmatch '(?s)start_sleep_wake_word_detector.*?start_wake_model_serialized\(&request\)' -or
    $main -notmatch '(?s)while \(true\).*?s_reprovision_quiesce_requested.*?s_cloud_owner_parked.*?cloud owner resumed after BLE provisioning' -or
    $wifiManager -notmatch '(?s)nuwax_wifi_set_provisioning_active\(bool active\).*?reconnect_armed.*?s_wifi_connect_requested.*?esp_wifi_connect\(\)' -or
    $main -notmatch 'Provisioning failed; previous Wi-Fi unavailable' -or
    $provision -notmatch 'NETWORK_PROV_EVENT_HANDLER_NONE' -or
    $provision -match 'NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE') {
    throw "BLE reprovision must stop physical audio and cloud owners, own STA in-process, retain restartable BLE memory, and restore runtime resources."
}

$wakeInitIndex = $main.IndexOf(
    "esp_err_t wake_init_err = init_wake_model_serialized()")
$wifiConnectIndex = $main.IndexOf(
    "boot_wifi_err = nuwax_wifi_connect()")
$wsDisconnectServiceIndex = $main.IndexOf(
    'service_offline_wake_word_detector("ws_disconnect")')
$wakeInit = [regex]::Match(
    $main,
    '(?s)static esp_err_t init_wake_model_serialized\(void\).*?(?=static esp_err_t start_wake_model_serialized)').Value
$wakeStart = [regex]::Match(
    $main,
    '(?s)static esp_err_t start_wake_model_serialized\(.*?(?=static esp_err_t deinit_wake_model_serialized)').Value
$wakeRestoreOwner = [regex]::Match(
    $main,
    '(?s)static bool service_wake_model_restore_owner\(void\).*?(?=static uint32_t next_wake_detector_token_locked)').Value
$wakeSleepStart = [regex]::Match(
    $main,
    '(?s)static bool start_sleep_wake_word_detector\(.*?(?=static void ensure_sleep_wake_word_detector)').Value
$wakeSupervisor = [regex]::Match(
    $main,
    '(?s)static void wake_word_supervisor_task\(void \*arg\).*?(?=static void remember_sleep_request)').Value
if ($wakeInitIndex -lt 0 -or
    $wifiConnectIndex -lt 0 -or
    $wakeInitIndex -gt $wifiConnectIndex -or
    $wsDisconnectServiceIndex -lt 0 -or
    $main -match 'stop_sleep_wake_word_detector\("ws_disconnect"\)' -or
    $wakeInit -notmatch '(?s)esp_ptr_in_dram\(.*?s_wake_model_operation_mutex.*?nuwax_wake_word_detector_init\(.*?xSemaphoreGive\(s_wake_model_operation_mutex\)' -or
    $wakeStart -notmatch '(?s)s_wake_model_operation_mutex.*?s_reprovision_quiesce_requested.*?nuwax_wake_word_detector_start_with_context\(context\).*?xSemaphoreGive\(s_wake_model_operation_mutex\)' -or
    $wakeStart -match 'nuwax_wake_word_detector_init\(' -or
    $main -notmatch '(?s)deinit_wake_model_serialized\(void\).*?s_wake_model_operation_mutex.*?nuwax_wake_word_detector_deinit\(\).*?nuwax_wake_word_detector_is_running\(\).*?xSemaphoreGive\(s_wake_model_operation_mutex\)' -or
    $wakeRestoreOwner -notmatch '(?s)s_conversation_owner_task.*?xTaskGetCurrentTaskHandle\(\).*?init_wake_model_serialized\(\).*?s_wake_model_restore_result.*?s_wake_model_restore_completed.*?CONFIG_ESP_MAIN_TASK_STACK_SIZE' -or
    $wakeSupervisor -match 'init_wake_model_serialized\(' -or
    $wakeSupervisor -notmatch '(?s)s_wake_detector_desired.*?conversation_capture_busy_for_sleep\(\).*?exclusive_audio_mode_blocks_normal_audio\(\).*?start_sleep_wake_word_detector\("wake_supervisor"\)' -or
    $wakeSleepStart -notmatch '(?s)err == ESP_ERR_INVALID_STATE.*?queue_wake_model_restore_nonblocking' -or
    $main -notmatch '(?s)static void ensure_sleep_wake_word_detector\(void\).*?nuwax_awake_session_state_is_active\(&s_awake_session\).*?s_turn_guard\.active' -or
    $main -notmatch '(?s)while \(atomic_load_explicit\(&s_reprovision_quiesce_requested,.*?service_wake_model_restore_owner\(\).*?vTaskDelay' -or
    $defaults -notmatch 'CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384' -or
    $main -notmatch '(?s)wake_word_detected_callback\(.*?cloud_service_is_ready\(\).*?offline_wake_rearm' -or
    $main -notmatch '(?s)wake_word_detected_callback\(.*?active_ws_epoch_snapshot\(\).*?event\.arm\.ws_epoch.*?xQueueSend\(s_wake_word_events' -or
    $main -notmatch '(?s)xQueueReceive\(s_wake_word_events.*?wake_event\.arm\.ws_epoch.*?active_epoch.*?wake_event\.detected_ms.*?NUWAX_WAKE_EVENT_MAX_AGE_MS.*?stale_wake_event' -or
    $main -notmatch '(?s)early local wake ready=.*?network_reserve_ok=.*?boot_network_memory_reserve' -or
    $main -notmatch '(?s)boot_wifi_connected.*?NUWAX_LIVE_AEC_PREP_MIN_INTERNAL_FREE_BYTES.*?boot_aec_memory_reserve.*?conversation_live_aec_pool_init\(\).*?boot_network_ready' -or
    $main -notmatch '(?s)boot Wi-Fi connection unavailable.*?continuing offline with background recovery.*?set_wifi_transport_connecting_state\("boot_wifi_unavailable"\)') {
    throw "Wake lifecycle regression: local wake must start before blocking Wi-Fi, fail closed on low heap, ignore offline hits, and survive WebSocket reconnects."
}
if ($main -notmatch 'conversation_live_aec_pool_suspend\("boot_preflight_complete"\)' -or
    $main -notmatch 'conversation_live_aec_pool_suspend\("sleep"\)' -or
    $main -notmatch 'conversation_live_aec_pool_suspend\("ws_disconnect"\)' -or
    $main -notmatch '(?s)wait_for_mic_start_action\(ws\).*?conversation_live_aec_pool_init\(\).*?conversation_live_aec_pool_acquire' -or
    $main -match '(?s)static esp_err_t restore_audio_after_runtime_provisioning\(void\).*?conversation_live_aec_pool_init\(\).*?static esp_err_t run_runtime_ble_reprovision' -or
    $otaDevice -notmatch '#define OTA_CONTROLLER_STACK_BYTES\s+12288U') {
    throw "Internal-RAM phase ownership regressed: AEC must not overlap sleep/TLS, and the measured OTA controller stack must remain bounded."
}
if ($main -notmatch 's_conversation_live_aec_post_reprovision_guard' -or
    $main -notmatch '(?s)static esp_err_t restore_audio_after_runtime_provisioning\(void\).*?heap_caps_malloc\(.*?NUWAX_LIVE_AEC_PREP_MIN_INTERNAL_LARGEST_BYTES.*?request_wake_model_restore.*?s_conversation_live_aec_post_reprovision_guard\s*=\s*aec_contiguous_guard' -or
    $main -notmatch '(?s)static esp_err_t conversation_live_aec_pool_init\(void\).*?s_conversation_live_aec_post_reprovision_guard\s*=\s*NULL.*?heap_caps_free\(post_reprovision_guard\).*?const size_t before_internal' -or
    $main -notmatch '(?s)conversation_live_aec_pool_suspend_for_provisioning\(void\).*?s_conversation_live_aec_post_reprovision_guard\s*=\s*NULL.*?heap_caps_free\(stale_guard\)' -or
    $main -notmatch 'post_provision_aec_prepare_restart') {
    throw "Post-provision AEC guard regression: wake reconstruction must not fragment the first normal capture, and a failed guarded prepare must recover by controlled restart."
}
if ($main -notmatch '#define NUWAX_SPEAKER_TASK_STACK_BYTES\s+12288U' -or
    $speakerPlayback -notmatch '(?s)void nuwax_speaker_playback_task\(void \*arg\).*?speaker_playback_packet_t \*packet_storage = s_speaker_task_packet;.*?speaker_playback_packet_t \*rebuffer_ref_storage =\s*s_speaker_task_rebuffer_ref;' -or
    $speakerPlayback -notmatch '(?s)s_speaker_task_packet = heap_caps_calloc\(.*?MALLOC_CAP_SPIRAM.*?s_speaker_task_rebuffer_ref = heap_caps_calloc' -or
    $speakerPlayback -notmatch 'speaker task resources stage=' -or
    $speakerPlayback -notmatch 'speaker_log_task_stack_watermark\("playback_end"\)') {
    throw "Speaker stack safety regressed: playback task needs a PSRAM-backed 12 KB stack, PSRAM packet storage, and runtime watermark logging."
}

if ($mcpServer -notmatch "nuwax_mcp_available_tool_mask" -or
    $main -notmatch '(?s)nuwax_mcp_callbacks_t callbacks = \{.*?\.servo_head_shake = NULL,.*?\.capability_flags = NUWAX_MCP_CALLBACK_CAP_NONE,.*?\};' -or
    $main -match "mcp_servo_head_shake") {
    throw "MCP capability wiring regressed: unavailable servo hardware must not be advertised or wired to a placeholder callback."
}
$beforeWifiOffset = $appMain.IndexOf('nuwax_diagnostics_log_heap_label(TAG, "before_wifi_init")')
$afterWifiConnectOffset = $appMain.IndexOf('nuwax_diagnostics_log_heap_label(TAG, "after_wifi_connect")')
$helloOffset = $appMain.IndexOf('nuwax_session_send_hello(')
$aecPoolOffset = $appMain.IndexOf('conversation_live_aec_pool_init()')
$displayOffset = $appMain.IndexOf('nuwax_display_init()')
$touchOffset = $appMain.IndexOf('nuwax_touch_init()')
$lvglOffset = $appMain.IndexOf('nuwax_lvgl_port_init()')
$speakerAnchorOffset = $appMain.IndexOf('nuwax_speaker_playback_prepare_for_tts("boot_preallocate")')
$micHealthOffset = $appMain.IndexOf('ota_validate_boot_mic_input(mic_detail,')
$bootBeforeHello = if ($helloOffset -gt 0) {
    $appMain.Substring(0, $helloOffset)
} else {
    $appMain
}
if ($beforeWifiOffset -lt 0 -or $afterWifiConnectOffset -lt 0 -or
    $helloOffset -lt 0 -or $aecPoolOffset -lt 0 -or
    $displayOffset -lt 0 -or $touchOffset -lt 0 -or $lvglOffset -lt 0 -or
    $speakerAnchorOffset -lt 0 -or $micHealthOffset -lt 0 -or
    $aecPoolOffset -lt $afterWifiConnectOffset -or
    $aecPoolOffset -gt $speakerAnchorOffset -or
    $aecPoolOffset -gt $helloOffset -or $micHealthOffset -lt $speakerAnchorOffset -or
    $main -notmatch 'OTA_BOOT_HEALTH_LIVE_AEC\s*=\s*1U\s*<<\s*9' -or
    $main -notmatch 'ota_boot_health_mark\(OTA_BOOT_HEALTH_LIVE_AEC\)' -or
    $main -notmatch 'ota_boot_health_has\(OTA_BOOT_HEALTH_LIVE_AEC\)' -or
    $appMain -match 'aec_ota_pending_verify' -or
    $bootBeforeHello -match 'nuwax_speaker_playback_deinit_output\(\)' -or
    $appMain -notmatch 'nuwax_speaker_playback_release_idle_output\("ws_disconnect"\)') {
    throw "Boot lifecycle regression: reusable AEC must be reserved after Wi-Fi but before speaker/TLS, required by OTA boot health, and codec handles must stay anchored across validation and reconnects."
}
$uiStartupStart = $main.IndexOf(
    'static esp_err_t initialize_v28_ui_with_retry(void)')
$uiStartupEnd = if ($uiStartupStart -ge 0) {
    $main.IndexOf('static nuwax_app_state_t head_touch_get_conversation_state',
                  $uiStartupStart)
} else { -1 }
$uiStartupBlock = if ($uiStartupStart -ge 0 -and
                           $uiStartupEnd -gt $uiStartupStart) {
    $main.Substring($uiStartupStart, $uiStartupEnd - $uiStartupStart)
} else { "" }
$emotionContractOffset = $projectCmake.IndexOf(
    'set(NUWAX_EMOTION_PACK_SHA256')
$projectConfigureOffset = $projectCmake.IndexOf(
    'project(NuwaxAIPartner)')
if ($defaults -notmatch '(?m)^CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y\s*$' -or
    $defaults -match '(?m)^CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y\s*$' -or
    $partitionTable -notmatch '(?m)^model,\s*data,\s*spiffs,\s*0x20000,\s*0x2A0000,\s*$' -or
    $partitionTable -notmatch '(?m)^ota_0,\s*app,\s*ota_0,\s*0x2C0000,\s*0x420000,\s*$' -or
    $partitionTable -notmatch '(?m)^ota_1,\s*app,\s*ota_1,\s*0x6E0000,\s*0x420000,\s*$' -or
    $partitionTable -notmatch '(?m)^emotion_0,data,\s*0x40,\s*0xB00000,\s*0x400000,\s*$' -or
    $partitionTable -notmatch '(?m)^storage,\s*data,\s*spiffs,\s*0xF00000,\s*0x100000,\s*$' -or
    $partitionTable -match '(?m)^emotion_1,' -or
    $projectCmake -notmatch 'esptool_py_flash_to_partition\(flash "emotion_0"' -or
    $projectCmake -notmatch '262cfa59cb200f7a0e147ed8ac25dd8659edc3e73c758365a209ffd3f53c367e' -or
    $projectCmake -notmatch 'de648186afa62dd7f6ca6e9a2743d5dac69dfe0117dc42e29147a57878666902' -or
    $projectCmake -notmatch '1c8461be24f2e2554b8a11d3c7b751cf05867dd511f66e05f2f4a358c24b44dc' -or
    $emotionContractOffset -lt 0 -or
    $projectConfigureOffset -lt 0 -or
    $emotionContractOffset -gt $projectConfigureOffset -or
    $cmake -notmatch 'nuwax_ui_v28/face/emotion_asset_pack\.c' -or
    $cmake -notmatch 'nuwax_ui_v28/face/emotion_animation_player\.c' -or
    $cmake -match '(?s)EMBED_FILES.*emotion-assets') {
    throw "N16R8 storage regression: dual app OTA, model, emotion_0 and storage partitions must remain inside the 16 MiB flash boundary."
}
if ($emotionPack -notmatch '#define PACK_PARTITION_LABEL "emotion_0"' -or
    $emotionPack -notmatch '#define PACK_PARTITION_SUBTYPE 0x40U' -or
    $emotionPack -notmatch '#define PACK_PARTITION_ADDRESS 0xB00000U' -or
    $emotionPack -notmatch '#define PACK_PARTITION_SIZE 0x400000U' -or
    $emotionPack -notmatch '#define PACK_REQUIRED_FLASH_SIZE 0x1000000U' -or
    $emotionPack -notmatch 'PACK_RELEASE_PAYLOAD_CRC32' -or
    $emotionPack -notmatch 'PACK_RELEASE_CATALOG_CRC32' -or
    $emotionPack -notmatch '(?s)validate_payload_crc\(void\).*?crc != s_header.payload_crc32' -or
    $projectCmake -notmatch 'NUWAX_EMOTION_COMPATIBLE_SHA256' -or
    $projectCmake -notmatch 'NUWAX_EMOTION_RELEASE_SHA_INDEX' -or
    $cmake -notmatch 'NUWAX_EMOTION_COMPATIBLE_SHA256_CSV' -or
    $emotionPack -notmatch '(?s)validate_pack_sha256\(void\).*?psa_crypto_init\(\).*?compatible_sha256.*?ESP_ERR_INVALID_CRC' -or
    $emotionPack -notmatch 'release_match' -or
    $emotionPack -notmatch 'integrity=sha256\+payload\+segment_crc abi=v%u' -or
    $emotionPack -notmatch 'PACK_VERSION_V1 NUWAX_EMOTION_ASSET_PACK_ABI_V1' -or
    $emotionPack -notmatch 'PACK_VERSION_V2 NUWAX_EMOTION_ASSET_PACK_ABI_V2' -or
    $emotionPack -notmatch 'PACK_VERSION_V3 NUWAX_EMOTION_ASSET_PACK_ABI_V3' -or
    $emotionPack -notmatch 'PACK_VERSION_V4 NUWAX_EMOTION_ASSET_PACK_ABI_V4' -or
    $emotionPack -notmatch 'nuwax_emotion_segment_raw_size_valid' -or
    $emotionPack -notmatch '(?s)validate_v3_segment\(.*?PACK_V3_SEGMENT_MAGIC.*?PACK_V3_SUPPORTED_FLAGS.*?source->compressed_size' -or
    $emotionPack -notmatch 'nuwax_emotion_asset_pack_v3_segment' -or
    $emotionPack -notmatch 'seen_assets != 0xffffU' -or
    $emotionPack -notmatch 'segment_references\[i\] != 1U' -or
    $emotionPack -notmatch 'source->compressed_size == 0' -or
    $emotionPack -notmatch 'source->flags != contract->flags') {
    throw "Emotion pack validation regression: exact partition identity, digest, unique assets, segment ownership and topology must fail closed."
}
if ($faceAdapter -notmatch 'FACE_BACKEND_ANIMATED' -or
    $faceAdapter -notmatch 'FACE_BACKEND_FAILSAFE' -or
    $faceAdapter -match 'FACE_BACKEND_LEGACY|face_engine_' -or
    $faceAdapter -match 'legacy face' -or
    $faceAdapter -notmatch '(?s)activate_failsafe.*?nuwax_emotion_animation_player_deinit\(\).*?create_failsafe_face\(\)' -or
    $faceAdapter -notmatch 'lv_obj_move_to_index\(s_failsafe_root, 0\)' -or
    $faceAdapter -notmatch '(?s)nuwax_face_create.*?nuwax_emotion_animation_player_init\(parent\).*?activate_failsafe\(err, "animation_startup"\).*?return ESP_OK;' -or
    $faceAdapter -notmatch '(?s)nuwax_face_tick.*?nuwax_emotion_animation_player_tick\(\).*?activate_failsafe\(err, "frame_decode"\)' -or
    $faceAdapter -notmatch 'state->wifi == NUWAX_WIFI_CONNECTING' -or
    $faceAdapter -match 'state->wifi != NUWAX_WIFI_CONNECTED' -or
    $faceAdapter -match 'cloud_unavailable' -or
    $stateBridge -notmatch '(?s)case NUWAX_APP_STATE_WS_CONNECTING:.*?return NUWAX_EMOTION_SLEEPING_FACE;' -or
    $stateBridge -notmatch '(?s)case NUWAX_APP_STATE_THINKING:\s*return NUWAX_EMOTION_THINKING;' -or
    $uiApp -notmatch '#define EMOTION_PLAYER_TICK_MS 5' -or
    $uiApp -notmatch 'nuwax_face_tick\(\)' -or
    $appShell -notmatch '(?s)NUWAX_APP_SHELL_EVENT_SWIPE_RIGHT\).*?nuwax_face_next_expression\(\).*?home swipe right -> next emotion expression' -or
    $appShell -match '(?s)NUWAX_APP_SHELL_EVENT_SWIPE_LEFT\).*?nuwax_face_next_expression\(\).*?home swipe left -> next emotion expression' -or
    $faceAdapter -notmatch '(?s)s_manual_preview_active.*?asset == s_automatic_asset.*?manual emotion preview released' -or
    $faceAdapter -notmatch 'CONFIG_NUWAX_FACE_TEST_MODE_INTERVAL_MS' -or
    $faceAdapter -notmatch 's_test_mode_active' -or
    $emotionPlayer -notmatch 'MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT' -or
    $emotionPlayer -notmatch '#define MIN_ANIMATED_FPS 40U' -or
    $emotionPlayer -match 'CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS' -or
    $emotionPack -notmatch '#define PACK_MIN_ANIMATED_FPS 40U' -or
    $defaults -notmatch '(?m)^CONFIG_LV_DEF_REFR_PERIOD=5\s*$') {
    throw "Emotion backend regression: production must use only deadline-driven animation plus a non-animated static failsafe, and preserve weak Wi-Fi as connected."
}
$legacyFacePaths = @(
    "main/nuwax_ui_v28/face/face_engine.c",
    "main/nuwax_ui_v28/face/face_engine.h",
    "main/nuwax_ui_v28/face/face_components.c",
    "main/nuwax_ui_v28/face/face_components.h",
    "main/nuwax_ui_v28/face/face_presets.c",
    "main/nuwax_ui_v28/face/face_presets.h",
    "main/nuwax_ui_v28/face/face_transition.c",
    "main/nuwax_ui_v28/face/face_transition.h",
    "main/nuwax_ui_v28/face/face_idle.c",
    "main/nuwax_ui_v28/face/face_idle.h",
    "main/nuwax_ui_v28/face/face_path_blob.c",
    "main/nuwax_ui_v28/face/face_path_blob.h",
    "main/nuwax_ui_v28/face/face_cache.c",
    "main/nuwax_ui_v28/face/face_cache.h",
    "main/nuwax_ui_v28/assets/face_diff_patches.c",
    "main/nuwax_ui_v28/assets/face_diff_patches.h",
    "main/nuwax_ui_v28/assets/face_paths.h",
    "main/nuwax_ui_v28/assets/face_paths_core.bin",
    "main/nuwax_ui_v28/assets/faces/face_05_thinking_360_rgb565.c",
    "main/nuwax_ui_v28/assets/faces/face_21_listening_360_rgb565.c",
    "main/nuwax_ui_v28/assets/faces/face_22_speaking_360_rgb565.c",
    "main/nuwax_ui_v28/assets/faces/face_23_low_battery_360_rgb565.c",
    "main/nuwax_ui_v28/assets/faces/face_24_charging_360_rgb565.c",
    "main/face_path_blob_host_check.c"
)
foreach ($legacyFacePath in $legacyFacePaths) {
    if (Test-Path -LiteralPath (Join-Path $projectRoot $legacyFacePath)) {
        throw "Retired face implementation remains in the product tree: $legacyFacePath"
    }
}
if ($cmake -match 'face_engine|face_components|face_presets|face_transition|face_idle|face_path_blob|face_cache|face_diff_patches|face_[0-9]+_.*rgb565|face_paths_core') {
    throw "Retired face implementation must not be linked or embedded."
}
if ($deviceState -notmatch '(?s)NUWAX_EMOTION_CHARGING,\s*/\* Appended values preserve.*?NUWAX_EMOTION_DIZZINESS,.*?NUWAX_EMOTION_QUESTION,\s*NUWAX_EMOTION_COUNT' -or
    $deviceState -notmatch 'bool provisioning_active;' -or
    $deviceState -notmatch 'uint32_t touch_sequence;' -or
    $deviceState -notmatch 'bool touch_emotion_active;' -or
    $deviceState -notmatch 'bool speaking_pcm_active;' -or
    $stateBridge -notmatch 'NUWAX_EMOTION_THOUGHT_COMPLETE: return "thought_complete"' -or
    $stateBridge -notmatch 'emotion_name_equals\(name, "provisioning"\)' -or
    $stateBridge -notmatch 'emotion_name_equals\(name, "monitoring"\)' -or
    $stateBridge -notmatch 'emotion_name_equals\(name, "touch"\)' -or
    $stateBridge -notmatch 'emotion_name_equals\(name, "thought_complete"\)' -or
    $stateBridge -notmatch 'emotion_name_equals\(name, "question"\)' -or
    $main -notmatch 'nuwax_app_shell_set_provisioning_active\(true\);' -or
    $main -notmatch 'nuwax_app_shell_set_provisioning_active\(false\);' -or
    $headTouch -notmatch '(?s)case NUWAX_HEAD_TOUCH_EVENT_PRESS:.*?nuwax_app_shell_trigger_touch_animation\(\);.*?if \(action ==') {
    throw "Emotion state regression: appended enum values, cloud names, provisioning ownership and accepted-touch sequencing must remain wired without renumbering legacy states."
}
if ($main -notmatch '(?s)if \(provisioning_boot_profile && ui_ready_snapshot\(\)\) \{.*?attempt <= 3U.*?nuwax_lvgl_port_set_runtime_provisioning_memory\(false\).*?UI continues in reduced-performance mode') {
    throw "First-boot provisioning must make a bounded attempt to restore the LVGL draw buffer to internal RAM."
}
if ($lvglPort -notmatch '(?s)if \(prefer_internal\).*?after_first_free >= \(DRAW_BUF_SIZE \+ LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE\).*?buf2 = heap_caps_aligned_alloc' -or
    $lvglPort -notmatch '(?s)if \(!buf1\) \{.*?MALLOC_CAP_SPIRAM.*?if \(DRAW_BUF_DOUBLE_BUFFER_ENABLE\).*?buf2 = heap_caps_aligned_alloc' -or
    $lvglPort -notmatch '(?s)if \(!buf2\) \{.*?Using single LVGL draw buffer' -or
    $lvglPort -notmatch '(?s)unsigned target_buffer_count = want_double \? 2U : 1U;.*?target_buffer_count = 1U;.*?uint8_t \*next2 = NULL;.*?if \(next1 && target_buffer_count == 2U\).*?if \(!next2\) \{.*?target_buffer_count = 1U;.*?continuing single-buffered.*?if \(!next1\)') {
    throw "LVGL draw-buffer allocation must prefer bounded double buffering and degrade to a valid single buffer under memory pressure."
}
if ($defaults -notmatch '(?m)^CONFIG_NUWAX_UI_DRAW_BUFFER_LINES=20\s*$' -or
    $kconfig -notmatch '(?s)config NUWAX_UI_DRAW_BUFFER_LINES.*?default 20.*?range 8 40' -or
    $lvglPort -notmatch '#define CONFIG_NUWAX_UI_DRAW_BUFFER_LINES 20' -or
    $lvglPort -notmatch 'A pair of 20-line buffers uses 28\.8 KiB total' -or
    $lvglPort -notmatch '(?s)Runtime pressure can still release.*?second 14\.4 KiB strip') {
    throw "LVGL performance contract requires two bounded 20-line internal buffers to halve object-tree strip traversal while retaining runtime single-buffer degradation."
}
if ($lvglPort -notmatch '(?s)static bool lvgl_async_flush_deinit\(void\).*?s_flush_task_exited.*?eSuspended.*?if \(!stopped\).*?return false;.*?vTaskDelete\(s_flush_task\)' -or
    $lvglPort -notmatch '(?s)static bool lvgl_port_init_cleanup\(bool tick_started\).*?if \(!lvgl_async_flush_deinit\(\)\).*?preserving display and draw-buffer ownership.*?return false;.*?lv_display_delete\(s_display\).*?heap_caps_free\(s_draw_buf1\).*?return true;' -or
    $lvglPort -notmatch '(?s)if \(!lvgl_port_init_cleanup\(tick_started\)\).*?restarting before dependent resources are released.*?esp_restart\(\);') {
    throw "LVGL rollback must never release display or draw-buffer resources before the asynchronous flush owner has quiesced."
}
if ($lvglPort -notmatch '#define LVGL_TASK_STACK_SIZE \(20 \* 1024\)' -or
    $lvglPort -notmatch '#define LVGL_TASK_STACK_GUARD_BYTES 512U' -or
    $lvglPort -notmatch '#define LVGL_TASK_STACK_WARN_BYTES \(4 \* 1024U\)' -or
    $lvglPort -notmatch '(?s)task_stack_allocation = heap_caps_aligned_alloc\(\s*16, LVGL_TASK_STACK_GUARD_BYTES \+ LVGL_TASK_STACK_SIZE,\s*MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT\);' -or
    $lvglPort -match 'task_stack_allocation = heap_caps_aligned_alloc\([^;]*MALLOC_CAP_SPIRAM' -or
    $lvglPort -notmatch '(?s)stack_cache_safe =.*?esp_ptr_in_dram\(task_stack_allocation\).*?esp_ptr_in_dram\(task_stack\).*?esp_ptr_in_dram\(\(const uint8_t \*\)task_stack \+.*?LVGL_TASK_STACK_SIZE - 1U\)' -or
    $lvglPort -notmatch '(?s)lvgl_task_stack_guard_intact\(void\).*?LVGL_TASK_STACK_GUARD_PATTERN' -or
    $lvglPort -notmatch '(?s)lvgl_check_task_stack\(int64_t now_us\).*?uxTaskGetStackHighWaterMark\(NULL\).*?LVGL task stack guard corrupted.*?esp_restart\(\).*?LVGL task stack reserve low' -or
    $defaults -notmatch 'CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y') {
    throw "LVGL task stack must retain a guarded 20 KiB internal reserve and hardware end-of-stack watchpoint because emotion partition reads disable the flash/PSRAM cache."
}
if ($lvglPort -notmatch '#define DOUBLE_BUFFER_RESTORE_STABLE_MS 2000' -or
    $lvglPort -notmatch '#define DOUBLE_BUFFER_RESTORE_HEADROOM 8000U' -or
    $lvglPort -notmatch '(?s)static void lvgl_maybe_rebalance_internal_buffers\(int64_t now_us\).*?lv_display_set_buffers\(s_display, s_draw_buf1, NULL.*?second internal draw buffer released.*?s_double_buffer_restore_candidate_us.*?heap_caps_aligned_alloc\(\s*16, DRAW_BUF_SIZE, MALLOC_CAP_INTERNAL.*?lv_display_set_buffers\(s_display, restored, NULL.*?internal draw buffer restored' -or
    $lvglPort -notmatch '(?s)lvgl_port_init_cleanup\(bool tick_started\).*?s_double_buffer_restore_candidate_us = 0;.*?s_internal_restore_last_log_us = 0;.*?s_runtime_internal_preferred = !s_provisioning_memory_profile;' -or
    $lvglPort -notmatch '(?s)nuwax_lvgl_port_set_runtime_provisioning_memory\(bool enabled\).*?s_runtime_internal_preferred = target_internal;') {
    throw "LVGL runtime buffer regression: memory pressure must degrade safely and later restore internal double buffering without inheriting rollback state."
}
if ($uiPerfDefaults -notmatch '(?m)^CONFIG_NUWAX_UI_PERF_LOG_ENABLE=y\s*$' -or
    $uiPerfDefaults -notmatch '(?m)^CONFIG_NUWAX_UI_PERF_LOG_INTERVAL_MS=2000\s*$' -or
    $uiPerfDefaults -notmatch '(?m)^CONFIG_NUWAX_UI_LONG_HANDLER_WARN_US=33000\s*$' -or
    $defaults -match '(?m)^CONFIG_NUWAX_UI_PERF_LOG_ENABLE=y\s*$') {
    throw "UI performance acceptance overlay must retain explicit 2 s telemetry and 33 ms handler limits without enabling production logging."
}
if ($main -notmatch 'static atomic_bool s_ui_ready = ATOMIC_VAR_INIT\(false\);' -or
    $main -notmatch '(?s)static bool ui_ready_snapshot\(void\).*?atomic_load_explicit\(&s_ui_ready, memory_order_acquire\)' -or
    $main -notmatch '(?s)static void ui_ready_store\(bool ready\).*?atomic_exchange_explicit\(\s*&s_ui_ready, ready, memory_order_acq_rel\).*?atomic_fetch_add_explicit\(&s_ui_lifecycle_generation,\s*1U,\s*memory_order_release\)' -or
    $main -notmatch '#define NUWAX_UI_STARTUP_MAX_ATTEMPTS 3U' -or
    $main -match 'ui_recovery_task' -or
    $appMain -notmatch '(?s)if \(display_err != ESP_OK\) \{.*?ui_ready_store\(false\);.*?\} else \{\s*esp_err_t lvgl_err = nuwax_lvgl_port_init\(\);' -or
    $appMain -notmatch '(?s)if \(lvgl_err == ESP_OK\) \{.*?esp_err_t ui_err = initialize_v28_ui_with_retry\(\);' -or
    $uiStartupBlock.Length -eq 0 -or
    $uiStartupBlock -notmatch 'ui_ready_store\(false\);' -or
    $uiStartupBlock -notmatch '(?s)attempt <= NUWAX_UI_STARTUP_MAX_ATTEMPTS.*?nuwax_lvgl_port_lock\(NUWAX_UI_STARTUP_LOCK_TIMEOUT_MS\).*?esp_err_t ui_err = nuwax_app_create\(lv_screen_active\(\)\);.*?esp_err_t shell_err = nuwax_app_shell_init\(\);' -or
    $uiStartupBlock -notmatch '(?s)if \(shell_err == ESP_OK\) \{.*?nuwax_lvgl_port_unlock\(\);\s*ui_ready_store\(true\);.*?return ESP_OK;' -or
    $uiStartupBlock -notmatch '(?s)V28 UI shell startup failed.*?nuwax_app_deinit\(\);' -or
    $uiStartupBlock -notmatch '(?s)attempt < NUWAX_UI_STARTUP_MAX_ATTEMPTS.*?vTaskDelay\(pdMS_TO_TICKS\(retry_delay_ms\)\);' -or
    $uiStartupBlock -notmatch '(?s)ui_ready_store\(false\);\s*ESP_LOGE\(TAG,\s*"V28 UI startup exhausted.*?return last_err;' -or
    $uiApp -notmatch 'esp_err_t nuwax_app_create\(lv_obj_t \*screen\)' -or
    $uiApp -notmatch '(?s)nuwax_app_create\(lv_obj_t \*screen\).*?!nuwax_lvgl_port_lock_is_held_by_current_task\(\).*?return ESP_ERR_INVALID_STATE;' -or
    $uiApp -notmatch '(?s)nuwax_app_deinit\(void\).*?!nuwax_lvgl_port_lock_is_held_by_current_task\(\).*?return ESP_ERR_INVALID_STATE;' -or
    $lvglPort -notmatch '(?s)bool nuwax_lvgl_port_lock_is_held_by_current_task\(void\).*?xSemaphoreGetMutexHolder\(s_lvgl_mutex\) == xTaskGetCurrentTaskHandle\(\)' -or
    $uiApp -notmatch '(?s)esp_err_t err = nuwax_face_create\(screen\);\s*if \(err != ESP_OK\) goto fail;.*?err = nuwax_drawer_create\(screen\);\s*if \(err != ESP_OK\) goto fail;.*?err = nuwax_gesture_init\(screen\);\s*if \(err != ESP_OK\) goto fail;' -or
    $uiApp -notmatch '(?s)fail:\s*ESP_LOGE\(TAG, "V28 UI creation failed: %s", esp_err_to_name\(err\)\);\s*esp_err_t cleanup_err = nuwax_app_deinit\(\);.*?return err;' -or
    $drawer -notmatch '(?s)if \(!control_center_memory_preflight\(\)\) \{\s*return ESP_ERR_NO_MEM;\s*\}.*?s_pending_timer = lv_timer_create\(pending_timer_cb, 250, NULL\);\s*if \(!s_pending_timer\) \{.*?nuwax_drawer_deinit\(\);.*?return ESP_ERR_NO_MEM;\s*\}') {
    throw "UI startup contract regressed: physical display readiness and fallible LVGL timers must gate s_ui_ready."
}
$legacyControlCenterPaths = @(
    "main/nuwax_ui_v28/nuwax_status_bar.c",
    "main/nuwax_ui_v28/nuwax_status_bar.h",
    "main/nuwax_ui_v28/nuwax_focus_menu.c",
    "main/nuwax_ui_v28/nuwax_focus_menu.h",
    "main/nuwax_ui_v28/assets/nuwax_menu_icon_assets.c",
    "main/nuwax_ui_v28/assets/nuwax_status_icon_assets.c",
    "main/nuwax_ui_v28/assets/nuwax_dialog_icon_assets.c"
)
foreach ($legacyControlCenterPath in $legacyControlCenterPaths) {
    if (Test-Path -LiteralPath (Join-Path $projectRoot $legacyControlCenterPath)) {
        throw "Retired control-center implementation remains in the product tree: $legacyControlCenterPath"
    }
}
if ($cmake -match 'nuwax_status_bar|nuwax_focus_menu|nuwax_menu_icon_assets|nuwax_status_icon_assets|nuwax_dialog_icon_assets' -or
    $cmake -notmatch 'nuwax_ui_v28/nuwax_drawer.c' -or
    $cmake -notmatch 'nuwax_ui_v28/assets/nuwax_control_center_icons.c' -or
    $controlCenterIcons -notmatch 'const lv_image_dsc_t \*nuwax_control_center_icon_get' -or
    $drawer -notmatch '#define CC_PSRAM_RUNTIME_RESERVE_BYTES' -or
    $drawer -notmatch '#define CC_PSRAM_LARGEST_BLOCK_MIN_BYTES' -or
    $drawer -notmatch '(?s)static bool control_center_memory_preflight\(void\).*?heap_caps_get_free_size.*?MALLOC_CAP_SPIRAM.*?heap_caps_get_largest_free_block.*?return false;' -or
    $drawer -notmatch '(?s)void nuwax_drawer_deinit\(void\).*?lv_timer_delete\(s_pending_timer\).*?hide_detail\(\);.*?lv_obj_delete\(s_root\);') {
    throw "UI lifecycle regression: the unified control center must be the sole overlay and must fail closed before exhausting PSRAM."
}
if ($drawer -notmatch '#define CC_MEETING_PENDING_TIMEOUT_MS 6000U' -or
    $drawer -notmatch '#define CC_PROVISION_PENDING_TIMEOUT_MS 15000U' -or
    $drawer -match '(?m)^#define CC_PENDING_TIMEOUT_MS' -or
    $drawer -notmatch '(?s)s_meeting_pending && now_us >= s_pending_deadline_us.*?s_meeting_pending = false;.*?s_pending_deadline_us = 0;' -or
    $drawer -notmatch '(?s)s_provision_pending &&\s*now_us >= s_provision_pending_deadline_us.*?s_provision_pending = false;' -or
    $drawer -notmatch '(?s)s_provision_pending_target = !provisioning_active\(\);\s*s_provision_pending = true;.*?s_provision_pending_deadline_us =.*?result\.value = s_provision_pending_target \? 1 : 0;' -or
    $drawer -notmatch '(?s)NUWAX_DRAWER_ACTION_SET_VOLUME\) \{\s*set_volume_visual\(s_have_state \? s_state\.volume : 60U\);') {
    throw "Control-center transaction regression: provisioning must settle from owner feedback and rejected volume commands must restore the canonical UI value."
}
if ($touchGesture -notmatch '(?s)static void restore_confirmed_volume_preview\(void\).*?nuwax_app_shell_get_device_state_snapshot\(&state, NULL\).*?s_volume_preview_cb\(state\.volume\);' -or
    $touchGesture -notmatch '(?s)static void queue_shell_action\(.*?nuwax_app_shell_notify_action_rejected\(result->action\);.*?NUWAX_APP_SHELL_ACTION_SET_VOLUME.*?restore_confirmed_volume_preview\(\);.*?nuwax_app_shell_set_volume_percent\(result->index\);' -or
    $appShell -notmatch '(?s)xQueueSend\(queue, result, wait_ticks\).*?result->action == NUWAX_APP_SHELL_ACTION_SET_VOLUME.*?app_shell_post_volume' -or
    $appShell -notmatch '(?s)result->action == NUWAX_APP_SHELL_ACTION_REPROVISION_WIFI.*?callback\(callback_ctx, result->index != 0\);.*?return true;.*?xQueueSend\(queue, result, wait_ticks\)' -or
    $main -notmatch '(?s)shell_dispatch_set_reprovisioning\(void \*ctx, bool enabled\).*?if \(!enabled\).*?nuwax_provision_main_request_runtime_cancel' -or
    $main -notmatch '(?s)Provisioning audio stop timeout.*?nuwax_app_shell_notify_action_rejected\(\s*NUWAX_APP_SHELL_ACTION_REPROVISION_WIFI\);') {
    throw "Control-center rollback regression: queue and provisioning-owner failures must converge optimistic UI and speaker preview state."
}
if ($appShell -notmatch '#define APP_SHELL_PROVISIONING_QR_RETRY_MS 500U' -or
    $appShell -notmatch '(?s)s_pending_provisioning_qr_deadline_us =\s*provisioning_qr_retry_deadline_us.*?if \(shown\).*?s_pending_provisioning_qr = false;' -or
    $appShell -notmatch '(?s)if \(was_active\).*?s_pending_provisioning_qr_close = true;' -or
    $drawerHeader -notmatch 'nuwax_drawer_hide_provisioning_qr' -or
    $drawer -notmatch '(?s)nuwax_drawer_hide_provisioning_qr\(void\).*?s_detail != CC_DETAIL_QR.*?nuwax_drawer_close\(\);' -or
    $drawer -notmatch '(?s)static void transition_cache_compose\(.*?s_detail != CC_DETAIL_NONE.*?return;') {
    throw "Provisioning QR lifecycle regression: auto-open retry, terminal close, and foreground detail isolation are required."
}
$appDeinitStart = $uiApp.IndexOf('esp_err_t nuwax_app_deinit(void)')
$appDeinitEnd = $uiApp.IndexOf('void nuwax_app_on_state_changed', $appDeinitStart + 1)
$appDeinit = if ($appDeinitStart -ge 0 -and $appDeinitEnd -gt $appDeinitStart) {
    $uiApp.Substring($appDeinitStart, $appDeinitEnd - $appDeinitStart)
} else {
    ''
}
$shellRelease = $appDeinit.IndexOf('nuwax_app_shell_deinit()')
$listenerRelease = $appDeinit.IndexOf('nuwax_device_state_remove_listener(')
$engineTimerRelease = $appDeinit.IndexOf('lv_timer_delete(s_emotion_timer)')
$gestureRelease = $appDeinit.IndexOf('nuwax_gesture_deinit(s_screen)')
$drawerRelease = $appDeinit.IndexOf('nuwax_drawer_deinit()')
$faceRelease = $appDeinit.IndexOf('nuwax_face_deinit()')
if ($shellRelease -lt 0 -or $listenerRelease -le $shellRelease -or
    $engineTimerRelease -le $listenerRelease -or
    $gestureRelease -le $engineTimerRelease -or
    $drawerRelease -le $gestureRelease -or
    $faceRelease -le $drawerRelease) {
    throw "UI teardown regression: callbacks and timers must stop before dependent LVGL modules are destroyed in reverse ownership order."
}
if ($appShell -notmatch '(?s)void nuwax_app_shell_deinit\(void\).*?app_shell_set_ui_ready\(false\);.*?nuwax_lvgl_port_set_pre_handler_cb\(NULL, NULL\);.*?app_shell_clear_transient_ui_locked\(\);.*?s_state_mailbox\.published_revision = 0U;' -or
    $appShell -notmatch '(?s)esp_err_t nuwax_app_shell_init\(void\).*?xQueueCreateStatic\(.*?app_shell_clear_transient_ui_locked\(\);.*?app_shell_next_state_revision_locked\(\);.*?nuwax_lvgl_port_set_pre_handler_cb\(app_shell_lvgl_pre_handler, NULL\);.*?app_shell_set_ui_ready\(true\);' -or
    $appShell -notmatch '(?s)static void app_shell_post_menu_drag\(int16_t delta_y\).*?if \(!s_ui_ready\).*?return;.*?s_pending_menu_drag = true;' -or
    $appShell -notmatch '(?s)static void app_shell_lvgl_pre_handler\(void \*user_data\).*?xQueueReceive\(s_touch_event_queue.*?app_shell_handle_touch_event_on_lvgl\(.*?app_shell_queue_result\(&result\);' -or
    $appShell -notmatch '(?s)nuwax_app_shell_handle_touch_event\(.*?xQueueSend\(s_touch_event_queue,.*?APP_SHELL_TOUCH_EVENT_QUEUE_WAIT_MS.*?return result;' -or
    $appShell -notmatch '(?s)app_shell_handle_touch_event_on_lvgl\(.*?nuwax_lvgl_port_lock_is_held_by_current_task\(\)' -or
    $appShell -match 'LVGL_TOUCH_LOCK_TIMEOUT_MS') {
    throw "App-shell lifecycle regression: terminal touch events must use the bounded LVGL-task queue while teardown discards transient input and preserves canonical state."
}
if ($emotionPlayer -notmatch '(?s)nuwax_emotion_animation_player_deinit\(void\).*?decode_worker_stop\(\).*?lv_obj_delete\(s_image\).*?s_cleanup_pending = true.*?reset_player_state\(\);' -or
    $emotionPlayer -notmatch '(?s)nuwax_emotion_animation_player_reap\(void\).*?decode_worker_reap\(\).*?reset_player_state\(\);' -or
    $emotionPlayer -notmatch '(?s)decode_worker_reap\(void\).*?s_decode_task_exited.*?eTaskGetState\(s_decode_task\) != eSuspended.*?vTaskDelete\(s_decode_task\)' -or
    $faceAdapter -notmatch '(?s)nuwax_face_tick\(void\).*?FACE_BACKEND_FAILSAFE.*?nuwax_emotion_animation_player_reap\(\)') {
    throw "LVGL image lifetime regression: image objects must detach before their animation buffers are released."
}
if ($drawer -notmatch '#define CC_REVEAL_DIRTY_BANDS 4' -or
    $drawer -notmatch '#define CC_REVEAL_CONTENT_DIRTY_AREAS 6' -or
    $drawer -notmatch '#define CC_REVEAL_RADIUS_Q4 2995' -or
    $drawer -notmatch '#define CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS 40' -or
    $drawer -notmatch '(?s)update_backdrop_progress\(int32_t progress\).*?CC_REVEAL_DIRTY_BANDS.*?invalidate_backdrop_band' -or
    $drawer -notmatch '(?s)reveal_timer_cb\(lv_timer_t \*timer\).*?nuwax_control_center_timeline_step\(.*?planned_step > s_reveal_anim_step \+ 1U.*?s_reveal_anim_step = planned_step;.*?apply_reveal_animation_step\(s_reveal_anim_step\).*?lv_timer_delete\(timer\).*?reveal_completed\(NULL\)' -or
    $drawer -notmatch '(?s)animate_reveal\(bool opening\).*?CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS.*?lv_timer_create\(reveal_timer_cb,\s*CC_REVEAL_FRAME_MS' -or
    $drawer -notmatch '(?s)open_control_center\(cc_page_t page\).*?nuwax_face_freeze_interaction\(\).*?nuwax_face_frozen_frame_ready\(\)' -or
    $drawer -notmatch '(?s)nuwax_drawer_begin_pull\(void\).*?nuwax_face_freeze_interaction\(\).*?nuwax_face_frozen_frame_ready\(\)' -or
    $drawer -notmatch '(?s)control_center_flush_filter\(.*?nuwax_face_frozen_rgb565\(&frozen_stride\).*?copy_frozen_span.*?blend_rgb565_q4' -or
    $drawer -notmatch '(?s)apply_content_reveal\(.*?one global circular mask.*?set_pos_if_changed\(clip, content_x, content_y\)' -or
    $drawer -match 'apply_circle_content_clip|nuwax_face_draw_frozen\(layer\)' -or
    $drawer -notmatch '(?s)nuwax_drawer_create\(lv_obj_t \*parent\).*?nuwax_lvgl_port_set_flush_filter\(control_center_flush_filter, NULL\)' -or
    $drawer -notmatch '(?s)nuwax_drawer_deinit\(void\).*?nuwax_lvgl_port_set_flush_filter\(NULL, NULL\)' -or
    $lvglPortHeader -notmatch 'nuwax_lvgl_port_flush_filter_cb_t' -or
    $lvglPort -notmatch '(?s)static void lvgl_flush_cb\(.*?flush_filter\(x1, y1, src_w, src_h, \(uint16_t \*\)px_map,.*?lvgl_flush_job_t job' -or
    $emotionPlayer -notmatch '(?s)nuwax_emotion_animation_player_frozen_rgb565\(.*?nuwax_emotion_animation_player_frozen_frame_ready\(\).*?return s_rgb565;' -or
    $drawer -notmatch '(?s)reveal_completed\(lv_anim_t \*anim\).*?s_reveal_anim_target == 0.*?nuwax_face_set_interaction_active\(false\).*?s_backdrop_frozen_face = false;' -or
    $drawer -notmatch '(?s)nuwax_drawer_deinit\(void\).*?nuwax_face_set_interaction_active\(false\)') {
    throw "Control-center animation regression: deadline-bound 40 FPS dirty-band reveal must freeze, RGB565-mask, skip expired poses, and release the published face frame."
}
if ($drawer -notmatch '#define CC_OPERATION_MIN_FPS 30' -or
    $drawer -notmatch '(?s)#ifndef CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS.*?#define CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS 40.*?#endif\s*#define CC_REVEAL_FRAME_MS \(1000U / CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS\)\s*_Static_assert\(CC_PAGE_ANIM_STEPS' -or
    $drawer -notmatch '#define CC_PAGE_ANIM_MS 230' -or
    $drawer -notmatch '#define CC_PAGE_ANIM_STEPS 10' -or
    $drawer -notmatch '(?s)_Static_assert\(CC_PAGE_ANIM_STEPS \* 1000U >=.*?CC_OPERATION_MIN_FPS.*?_Static_assert\(CC_PAGE_ANIM_STEPS \* 1000U >=.*?CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS' -or
    $drawer -notmatch '(?s)start_page_perf\(void\).*?s_page_perf_frames = 1U.*?finish_page_perf\(void\).*?control center page below minimum.*?CC_OPERATION_MIN_FPS' -or
    $drawer -notmatch '(?s)cancel_page_perf\(void\).*?s_page_perf_active = false;.*?s_page_perf_last_frame_us = 0;' -or
    $drawer -notmatch '(?s)page_transition_completed\(lv_anim_t \*anim\).*?finish_page_perf\(\).*?settle_page_transition') {
    throw "Control-center frame-rate regression: page operations must target 40 FPS, enforce a 30 FPS floor, and publish real device cadence telemetry."
}
if ($drawer -notmatch '#define CC_TRANSITION_CACHE_COMPONENTS' -or
    $drawer -notmatch '(?s)transition_cache_bind_sources\(void\).*?CC_PSRAM_RUNTIME_RESERVE_BYTES.*?heap_caps_aligned_alloc\(.*?MALLOC_CAP_SPIRAM.*?lv_draw_buf_init' -or
    $drawer -notmatch '(?s)transition_cache_capture_page\(cc_page_t page\).*?suspend_display_invalidation\(s_surface\).*?lv_snapshot_take_to_draw_buf.*?transition_cache_release\(\);' -or
    $drawer -notmatch '(?s)transition_cache_compose_component\(.*?s_reveal_mask_rows.*?memcpy\(destination, source.*?blend_rgb565_u8' -or
    $drawer -notmatch '(?s)control_center_flush_filter\(.*?transition_cache_compose\(x, y, width, height, pixels, stride_pixels\).*?nuwax_face_frozen_rgb565' -or
    $drawer -notmatch '(?s)pending_timer_cb\(lv_timer_t \*timer\).*?!s_open.*?transition_cache_prepare_page\(CC_PAGE_STATUS\).*?transition_cache_prepare_page\(CC_PAGE_MENU\)' -or
    $drawer -notmatch '(?s)transition_cache_mark_page_dirty\(cc_page_t page\).*?generation.*?\(\*generation\)\+\+;' -or
    $drawer -notmatch '(?s)transition_cache_capture_page\(cc_page_t page\).*?s_transition_cache_build_next.*?lv_snapshot_take_to_draw_buf.*?s_transition_cache_build_components\+\+.*?return false;.*?s_transition_cache_build_active = false;' -or
    $drawer -notmatch '(?s)transition_cache_activate\(bool both_pages\).*?current_ready.*?other_ready.*?if \(!s_transition_cache_available \|\| !current_ready \|\|.*?return false;.*?s_transition_cache_active = true;.*?set_page_tree_mode' -or
    $drawer -notmatch '(?s)transition_cache_deactivate\(void\).*?s_transition_cache_active = false;' -or
    $drawer -notmatch '(?s)apply_page_progress\(int32_t progress, bool quantize\).*?apply_reveal_progress\(s_reveal_raw, false, true\)' -or
    $drawer -notmatch '(?s)begin_page_transition\(cc_page_t target\).*?stop_local_animations\(\).*?s_page_transition_active = true;.*?s_page_transition_target = target;.*?set_page_tree_mode\(s_page, true\);' -or
    $drawer -notmatch '(?s)settle_page_transition\(cc_page_t page\).*?s_page = page;.*?s_page_progress = target_progress;.*?set_page_tree_mode\(page, false\);.*?s_page_transition_active = false;.*?start_local_animations\(\);' -or
    $drawer -notmatch '(?s)set_page_tree_mode\(cc_page_t active_page, bool transitioning\).*?suspend_display_invalidation\(s_surface\).*?LV_OBJ_FLAG_HIDDEN.*?resume_display_invalidation' -or
    $drawer -notmatch '(?s)nuwax_drawer_page_finish_drag\(bool commit\).*?s_page_transition_settle_page = target;.*?lv_anim_set_completed_cb\(&page_anim, page_transition_completed\)' -or
    $drawer -notmatch '(?s)nuwax_drawer_update\(const nuwax_device_state_t \*state\).*?s_status_dirty = s_status_dirty \|\| status_changed;.*?s_menu_dirty = s_menu_dirty \|\| menu_changed;.*?s_info_dirty = s_info_dirty \|\| info_changed;' -or
    $drawer -notmatch '(?s)nuwax_drawer_deinit\(void\).*?lv_anim_delete\(s_surface, page_progress_exec\).*?lv_obj_delete\(s_root\)') {
    throw "Control-center page transition regression: PSRAM component caches, bounded RGB565 composition, fallback live trees, field-level dirty state and transactional swipe settling must remain intact."
}
if ($emotionPlayer -notmatch '#define FRAME_PATCH_OBJECT_COUNT NUWAX_EMOTION_ASSET_V4_MAX_REGIONS' -or
    $emotionPlayer -notmatch '(?s)start_segment\(uint16_t segment_index\).*?nuwax_emotion_segment_raw_size_valid' -or
    $emotionPlayer -notmatch '#define COMPONENT_BRIDGE_FPS 40U' -or
    $emotionPlayer -notmatch '(?s)create_frame_patches\(.*?s_frame_patch_compositor = lv_obj_create\(parent\).*?lv_obj_set_size\(s_frame_patch_compositor, FRAME_WIDTH, FRAME_HEIGHT\)' -or
    $emotionPlayer -notmatch '(?s)update_frame_patch_state\(.*?FRAME_PATCH_OBJECT_COUNT.*?nuwax_emotion_component_regions_compact\(.*?FRAME_PATCH_OBJECT_COUNT\).*?invalidate_frame_patch_damage\(.*?lv_obj_invalidate_area\(s_frame_patch_compositor' -or
    $emotionPlayer -notmatch '(?s)begin_component_bridge\(.*?nuwax_emotion_component_bridge_allowed\(s_asset_id, target_asset\)' -or
    $emotionPlayer -notmatch '(?s)tick_component_bridge\(.*?nuwax_emotion_component_bridge_ready_to_animate.*?visual_elapsed_us.*?nuwax_emotion_component_regions_compact\(.*?FRAME_PATCH_OBJECT_COUNT\).*?present_frame_patches_direct\(regions, region_count.*?direct_present_frames.*?fallback_present_frames.*?nuwax_emotion_component_bridge_next_elapsed_us.*?bridge->next_frame_us = render_started_us \+ COMPONENT_BRIDGE_PERIOD_US' -or
    $defaults -notmatch 'CONFIG_NUWAX_UI_BACKGROUND_DECODE_ENABLE=y' -or
    $emotionPlayer -notmatch '(?s)prepare_next_frame\(decode_frame_slot_t \*slot\).*?NUWAX_EMOTION_ASSET_PACK_ABI_V4.*?decode_v4_frame_prepare\(slot\).*?NUWAX_EMOTION_ASSET_PACK_ABI_V3.*?decode_v3_frame_prepare\(slot\).*?NUWAX_EMOTION_ASSET_PACK_ABI_V2.*?decode_v2_frame_prepare\(slot\).*?decode_frame_prepare\(\)' -or
    $emotionPlayer -notmatch '(?s)decode_v3_frame_prepare\(.*?V3_BOOTSTRAP_TRANSITION.*?v3->transition_offset.*?V3_BOOTSTRAP_WRAP.*?v3->wrap_offset.*?v3_expand_key' -or
    $emotionPlayer -notmatch '(?s)advance_after_segment\(void\).*?V3_BOOTSTRAP_TRANSITION.*?V3_BOOTSTRAP_WRAP' -or
    $emotionPlayer -notmatch '(?s)decode_worker_task\(void \*argument\).*?find_free_decode_slot\(\).*?prepare_next_frame\(slot\).*?DECODE_SLOT_READY.*?DECODE_WORKER_READY' -or
    $emotionPlayer -notmatch '(?s)nuwax_emotion_animation_player_tick\(void\).*?s_background_decode.*?s_segment_advance_pending.*?DECODE_WORKER_RUNNING.*?advance_after_segment\(\).*?now_us < s_next_frame_us.*?find_ready_decode_slot\(.*?s_next_publish_sequence.*?publish_decoded_frame\(slot\).*?release_decode_slot\(slot\).*?s_segment_advance_pending = true;.*?s_next_frame_us = next_frame_us;' -or
    $emotionPlayer -notmatch '(?s)nuwax_emotion_animation_player_tick\(void\).*?now_us < s_next_frame_us.*?find_free_decode_slot\(\).*?prepare_next_frame\(slot\).*?publish_decoded_frame\(slot\).*?release_decode_slot\(slot\).*?s_next_frame_us = next_frame_us;' -or
    $emotionPlayer -notmatch '(?s)apply_queued_asset_if_stable\(void\).*?queued == \(unsigned\)s_component_bridge\.target_asset.*?atomic_compare_exchange_strong_explicit\(\s*&s_queued_asset_id, &expected, 0U' -or
    $emotionPlayer -notmatch '(?s)nuwax_emotion_animation_player_set_asset\(.*?nuwax_emotion_component_asset_request_is_satisfied\(' -or
    $emotionPlayer -notmatch '(?s)apply_queued_asset_if_stable\(void\).*?restored_bridge_source = false;.*?restore_component_bridge_source\(now_us\);.*?restored_bridge_source = true;.*?restored_bridge_source && queued == \(unsigned\)s_asset_id.*?atomic_compare_exchange_strong_explicit\(\s*&s_queued_asset_id, &expected, 0U' -or
    $emotionPlayer -notmatch '(?s)nuwax_emotion_animation_player_tick\(void\).*?apply_queued_asset_if_stable\(\).*?if \(s_interaction_active\) \{\s*return ESP_OK;\s*\}\s*if \(s_component_bridge\.active\).*?tick_component_bridge' -or
    $emotionPlayer -notmatch '(?s)nuwax_emotion_animation_player_set_interaction_active\(bool active\).*?if \(active\).*?nuwax_emotion_animation_player_freeze\(\).*?if \(!s_interaction_active\) return;.*?s_interaction_active = false;.*?s_component_bridge\.started_us \+= paused_us;.*?s_component_bridge\.next_frame_us \+= paused_us;.*?s_next_frame_us = resumed_us;') {
    throw "Emotion animation regression: bounded component patches, 40 FPS bridging, background decode with synchronous fallback, deadline pacing, and interaction resume must remain intact."
}
if ($displayHeader -notmatch 'display_write_region_pixels_strided' -or
    $display -notmatch '(?s)copy_pixels_byte_swapped\(.*?spi_write_pixels_strided\(' -or
    $display -notmatch '(?s)display_write_region_pixels_strided\(.*?source_stride_pixels.*?st77916_set_addr_window\(.*?spi_write_pixels_strided\(' -or
    $lvglPortHeader -notmatch 'nuwax_lvgl_port_present_rgb565_regions' -or
    $lvglPort -notmatch '(?s)nuwax_lvgl_port_present_rgb565_regions\(.*?nuwax_lvgl_port_lock_is_held_by_current_task\(\).*?lvgl_flush_pipeline_take\(timeout_ms\).*?display_write_region_pixels_strided\(.*?lvgl_flush_pipeline_give\(\)' -or
    $emotionPlayer -notmatch '#define COMPONENT_BRIDGE_DIRECT_PRESENT_TIMEOUT_MS 2U' -or
    $emotionPlayer -notmatch '(?s)present_frame_patches_direct\(.*?update_frame_patch_state\(.*?nuwax_lvgl_port_present_rgb565_regions\(.*?invalidate_frame_patch_damage\(damage, damage_count\)' -or
    $emotionPlayer -notmatch 'direct_avg_us=%u direct_max_us=%u') {
    throw "Emotion direct-present regression: transitions must serialize with LVGL flush ownership, stream strided RGB565 damage without another framebuffer, and retain bounded LVGL fallback telemetry."
}
if ($emotionPlayer -notmatch '#define COMPONENT_BRIDGE_PSRAM_RESERVE_BYTES \(1024U \* 1024U\)' -or
    $emotionPlayer -notmatch '(?s)ensure_component_bridge_work\(void\).*?COMPONENT_BRIDGE_PSRAM_RESERVE_BYTES.*?s_bridge_source_work = heap_caps_malloc.*?s_bridge_target_work = heap_caps_malloc.*?s_bridge_target_indices_work = heap_caps_malloc' -or
    $emotionPlayer -notmatch '(?s)nuwax_emotion_animation_player_init\(lv_obj_t \*parent\).*?pack_abi == NUWAX_EMOTION_ASSET_PACK_ABI_V4.*?!ensure_component_bridge_work\(\).*?workspace not prewarmed' -or
    $emotionPlayer -notmatch '(?s)decode_v4_handoff_frame\(.*?uint8_t \*raw = \(uint8_t \*\)rgb_out;.*?uint8_t \*compressed = raw \+ FRAME_PIXELS' -or
    $emotionPlayer -notmatch '(?s)publish_v4_frame\(.*?if \(s_rgb565\[pixel\] != rgb\).*?include_dirty_run' -or
    $emotionPlayer -match '(?s)publish_v4_frame\(.*?slot->absolute.*?lv_obj_invalidate\(s_image\)') {
    throw "Emotion transition regression: v4 switching must prewarm reusable PSRAM, reuse bounded scratch space, and derive absolute-frame invalidation from real pixel differences."
}
if ($emotionPlayer -match '=\s*tinfl_decompress_mem_to_mem\(' -or
    $emotionPlayer -notmatch '(?s)inflate_v4_record\(.*?tinfl_init\(s_inflater\).*?tinfl_decompress\(\s*s_inflater.*?TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF' -or
    $emotionPlayer -notmatch '(?s)s_inflater = allocate_decode_work\(sizeof\(\*s_inflater\), true\);\s*if \(pack_abi < NUWAX_EMOTION_ASSET_PACK_ABI_V4\)' -or
    $emotionPlayer -notmatch '(?s)!s_rgb565 \|\|\s*!s_inflater \|\|') {
    throw "Emotion inflate regression: v4 records must reuse the heap-backed inflater instead of placing the roughly 11 KiB ROM miniz workspace on an application task stack."
}
if ($emotionPlayer -notmatch '#define COMPONENT_BRIDGE_MIN_FPS 30U' -or
    $emotionPlayer -notmatch '#define DECODE_TASK_PRIORITY 4U' -or
    $emotionPlayer -notmatch '(?s)render_bridge_opaque_scaled_rows\(.*?memcpy\(&s_rgb565.*?render_bridge_eye\(.*?scale_rgb565_over_black' -or
    $emotionPlayer -notmatch '(?s)tick_component_bridge\(int64_t now_us\).*?render_started_us.*?max_gap_us.*?max_render_us.*?visual_elapsed_us.*?bridge->next_frame_us = render_started_us \+ COMPONENT_BRIDGE_PERIOD_US' -or
    $emotionPlayer -notmatch '(?s)commit_component_bridge_target\(int64_t now_us\).*?average_render_us.*?component bridge below minimum.*?COMPONENT_BRIDGE_MIN_FPS.*?COMPONENT_BRIDGE_FPS') {
    throw "Emotion transition telemetry regression: every component bridge must report its real cadence and render budget against the 30/40 FPS contract."
}
if ($faceAdapter -match 'SPEAKING_OVERLAY|speaking_draw_cb|create_speaking_overlay|set_speaking_visible|s_speaking_last_frame_us') {
    throw "Retired speaking bubble must not create an LVGL overlay or schedule periodic redraws."
}
if ($emotionPlayer -notmatch 's_health_frames_total\+\+;' -or
    $emotionPlayer -notmatch 'window_frames=%llu' -or
    $emotionPlayer -notmatch 's_health_deadline_resyncs\+\+;' -or
    $emotionPlayer -match '(?s)static esp_err_t load_asset\([^;]*?\)\s*\{.*?s_last_health_log_us\s*=.*?ESP_LOGI\(TAG,\s*"asset selected') {
    throw "Emotion health telemetry must report continuous frame pacing across state-driven asset switches."
}
if ($lvglPort -notmatch '#define LVGL_TASK_CORE\s+0' -or
    $lvglPort -notmatch '(?s)xTaskCreateStaticPinnedToCore\(\s*lvgl_flush_task,.*?LVGL_TASK_CORE\)' -or
    $lvglPort -notmatch '(?s)xTaskCreateStaticPinnedToCore\(\s*lvgl_task,.*?LVGL_TASK_CORE\)' -or
    $emotionPlayer -notmatch '#define DECODE_TASK_CORE 0') {
    throw "UI scheduling regression: emotion decode, LVGL rendering and display flush must share CPU0, isolated from CPU1 playback wake capture and MultiNet inference."
}
if ($lvglPort -match 'LVGL_MIN_BLOCK_TICKS' -or
    $lvglPort -notmatch '#define LVGL_EXTERNAL_WAKE_STREAK_LIMIT 8U' -or
    $lvglPort -notmatch '(?s)lvgl_wait_until_due\(uint32_t delay_ms\).*?ulTaskNotifyTake\(pdTRUE, 0\).*?s_wake_timer_fired.*?LVGL_EXTERNAL_WAKE_STREAK_LIMIT.*?vTaskDelay\(1\)' -or
    $lvglPort -notmatch '(?s)lvgl_block_between_handlers\(uint32_t next_ms\).*?lvgl_wait_until_due\(next_ms\)') {
    throw "LVGL pacing regression: high-resolution deadline sleeps must not charge every 40 FPS frame one 10 ms scheduler tick, while external wake storms remain bounded."
}

$scaledIntegerContracts = @(
    @{ Name = "NUWAX_P3_SOFTREF_AB_MAX_UNDERRUN_RATIO"; Default = "10" },
    @{ Name = "NUWAX_P3_SOFTREF_AB_MIN_ERLE_DB"; Default = "600" },
    @{ Name = "NUWAX_P3_SOFTREF_AB_MIN_ACTIVE_REF_RATIO"; Default = "600" },
    @{ Name = "NUWAX_P3_NEAREND_MIN_ACTIVE_REF_RATIO"; Default = "600" },
    @{ Name = "NUWAX_P3_NEAREND_MIN_OUT_TO_REF_DB"; Default = "-1600" },
    @{ Name = "NUWAX_P3_NEAREND_MAX_CORR_AFTER_ABS"; Default = "100" },
    @{ Name = "NUWAX_P6_NARROW_NORMALVOICE_REF_RATIO_LO"; Default = "600" },
    @{ Name = "NUWAX_P6_NARROW_NORMALVOICE_OUT_TO_REF_DB_LO"; Default = "-400" },
    @{ Name = "NUWAX_P6_NARROW_NORMALVOICE_ERLE_DB_LO"; Default = "1750" }
)
foreach ($contract in $scaledIntegerContracts) {
    $name = [regex]::Escape($contract.Name)
    $default = [regex]::Escape($contract.Default)
    if ($kconfig -notmatch "(?ms)^\s*config $name\s+int .*?^\s*default $default\s*$") {
        throw "Kconfig scaled-integer contract is missing for $($contract.Name)"
    }
}

if ($defaults -notmatch "CONFIG_NUWAX_BARGE_IN_ENABLE=n" -or
    $bargeConfig -notmatch "CONFIG_NUWAX_BARGE_IN_ENABLE=y" -or
    $bargeConfig -notmatch "CONFIG_NUWAX_BARGE_IN_LOCAL_EARLY_CONFIRM=y" -or
    $bargeConfig -notmatch "CONFIG_NUWAX_BARGE_IN_CLOUD_GUARDED_HIGH_RECALL=y" -or
    $kconfig -notmatch '(?s)config NUWAX_BARGE_IN_ENABLE.*?default n.*?Remote cancel remains' -or
    $kconfig -notmatch '(?s)config NUWAX_BARGE_IN_LOCAL_EARLY_CONFIRM.*?depends on NUWAX_BARGE_IN_ENABLE' -or
    $kconfig -notmatch '(?s)config NUWAX_BARGE_IN_CLOUD_GUARDED_HIGH_RECALL.*?depends on NUWAX_BARGE_IN_ENABLE' -or
    $main -notmatch 'CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC && CONFIG_NUWAX_BARGE_IN_ENABLE' -or
    $main -notmatch 'tts_start_voice_barge_disabled' -or
    $main -notmatch 'voice_barge_disabled_pre_mic' -or
    $main -notmatch 'tts_pending_voice_barge_disabled') {
    throw "Product voice barge-in must default off while preserving the explicit diagnostic profile."
}
if ($defaults -notmatch 'CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE=n' -or
    $defaults -notmatch 'CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE=y' -or
    $kconfig -notmatch '(?s)config NUWAX_TOUCH_WAKE_ACTION_ENABLE.*?default n' -or
    $kconfig -notmatch '(?s)config NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE.*?default n' -or
    $appShell -notmatch 'CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE' -or
    $appShell -notmatch 'CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE' -or
    $headTouch -notmatch 'NUWAX_HEAD_TOUCH_ACTION_CANCEL' -or
    $headTouch -notmatch 'CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE != 0') {
    throw "Product touch controls must keep touch wake disabled while active-turn interruption remains enabled."
}
if ($defaults -notmatch 'CONFIG_NUWAX_HEAD_TOUCH_LONG_PRESS_MS=1000' -or
    $defaults -notmatch 'CONFIG_NUWAX_HEAD_TOUCH_GAP_TOLERANCE_MS=750' -or
    $kconfig -notmatch '(?s)config NUWAX_HEAD_TOUCH_LONG_PRESS_MS.*?default 1000' -or
    $kconfig -notmatch '(?s)config NUWAX_HEAD_TOUCH_GAP_TOLERANCE_MS.*?default 750' -or
    $boardInputs -notmatch 's_head_touch_long_press_ms = 1000U' -or
    $boardInputs -notmatch 's_head_touch_gap_tolerance_ms = 750U' -or
    $headTouch -notmatch '#define CONFIG_NUWAX_HEAD_TOUCH_LONG_PRESS_MS 1000U' -or
    $boardInputs -notmatch '(?s)NUWAX_BOARD_INPUT_HEAD_TOUCH.*?\.active_level = 0' -or
    $boardInputs -notmatch 'ready head_touch=GPIO%d\(active-low\)' -or
    $boardInputs -notmatch '(?s)head_touch_raw_active_started_ms =.*?esp_timer_get_time.*?head_touch_raw_active_started_valid = true' -or
    $boardInputs -notmatch '(?s)activation_started_ms =.*?head_touch_raw_active_started_valid.*?head_touch_raw_active_started_ms.*?head_touch_press_started_ms = activation_started_ms' -or
    $boardInputs -match 'resume_session|head_touch_session_started_ms|session_duration_ms' -or
    $boardInputs -notmatch '(?s)if \(head_touch_active\).*?press_duration_ms.*?press_duration_ms >= long_press_ms.*?NUWAX_HEAD_TOUCH_EVENT_LONG_PRESS' -or
    $shakeDetectorHeader -notmatch '#define NUWAX_HEAD_TOUCH_INTERACTION_COOLDOWN_MS 2000U' -or
    $shakeDetectorHeader -notmatch '#define NUWAX_SHAKE_INTERACTION_COOLDOWN_MS 4000U' -or
    $shakeDetector -notmatch '(?s)interaction_cooldown_ms\(.*?NUWAX_HEAD_TOUCH_INTERACTION_COOLDOWN_MS.*?NUWAX_SHAKE_INTERACTION_COOLDOWN_MS' -or
    $shakeDetector -notmatch '(?s)const uint32_t cooldown_ms = interaction_cooldown_ms\(owner\);.*?if \(cooldown_ms > 0U\).*?s_interaction_cooldown_until_ms = 0U;' -or
    $headTouch -notmatch '(?s)static bool s_press_claimed_gate;.*?case NUWAX_HEAD_TOUCH_EVENT_LONG_PRESS:.*?s_press_claimed_gate = true;.*?case NUWAX_HEAD_TOUCH_EVENT_RELEASE:.*?if \(s_press_claimed_gate\).*?nuwax_shake_detector_release_head_touch\(\);') {
    throw "Head-touch debounce regression: GPIO8 must remain active-low, qualification must start at the first sampled active edge and remain 1 second, retriggers require a 2-second cooldown, and only the current press may release the shared interaction gate."
}
if ($defaults -notmatch 'CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=n' -or
    $defaults -notmatch 'CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM=y' -or
    $defaults -notmatch '# CONFIG_I2C_ISR_IRAM_SAFE is not set' -or
    $bargeConfig -notmatch '# CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is not set' -or
    $bargeConfig -notmatch 'CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM=y' -or
    $bargeConfig -notmatch '# CONFIG_I2C_ISR_IRAM_SAFE is not set') {
    throw "The product build must preserve the validated 7.0.13 system-BSS and I2C ISR layout."
}
if ($uplink -notmatch "formal cloud uplink requires a 16 kHz sample rate" -or
    $uplink -notmatch "formal cloud uplink requires mono audio" -or
    $uplink -notmatch "formal cloud uplink requires 20 ms audio frames" -or
    $uplink -notmatch "sample_count != NUWAX_AUDIO_UPLINK_PCM_FRAME_SAMPLES" -or
    $uplink -notmatch "nuwax_audio_uplink_build_terminal_marker" -or
    $uplink -notmatch "nuwax_audio_uplink_mark_last_frame_end" -or
    $uplink -notmatch '(?s)!ctx->end_enqueued.*?final_result != ESP_OK.*?!terminal_recovery_armed.*?!nuwax_audio_terminal_recovery_pending\(\).*?audio_uplink_request_integrity_close' -or
    $audioTerminalRecovery -notmatch '(?s)NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_RETRY_END.*?nuwax_ws_send_binary_with_timeout_if_epoch' -or
    $audioTerminalRecovery -notmatch '(?s)nuwax_audio_terminal_recovery_should_quarantine.*?nuwax_ws_close_if_epoch') {
    throw "Formal audio uplink framing or END contract is incomplete."
}
if ($latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_CAPTURE_SLOTS 12U" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_CAPTURE_PRE_DSP_MAX_AGE_MS 60U" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_BARGE_END_TO_END_MAX_AGE_MS 100U" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_UPLINK_PACKET_SLOTS 16U" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_UPLINK_STAGING_MAX_AGE_MS 160U" -or
    $latencyContract -notmatch "NUWAX_CONVERSATION_PROFILE_MIN_DSP_RESERVE_MS 40U" -or
    $latencyContract -notmatch "barge end-to-end age must reserve at least 40 ms for DSP" -or
    $kconfig -notmatch 'Realtime microphone uplink queue depth in packet slots' -or
    $kconfig -notmatch 'config NUWAX_BARGE_END_TO_END_MAX_FRAME_AGE_MS' -or
    $bargeConfig -notmatch 'CONFIG_NUWAX_VOICE_UPLINK_QUEUE_DEPTH=16' -or
    $bargeConfig -notmatch 'CONFIG_NUWAX_VOICE_UPLINK_REALTIME_MAX_AGE_MS=160' -or
    $bargeConfig -notmatch 'CONFIG_NUWAX_RT_CAPTURE_QUEUE_DEPTH=12' -or
    $bargeConfig -notmatch 'CONFIG_NUWAX_RT_CAPTURE_MAX_DECISION_AGE_MS=60' -or
    $bargeConfig -notmatch 'CONFIG_NUWAX_BARGE_END_TO_END_MAX_FRAME_AGE_MS=100') {
    throw "The normal-conversation latency contract or Kconfig semantics drifted."
}
if ($uplinkHeader -notmatch "NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES 4" -or
    $uplinkHeader -notmatch "NUWAX_AUDIO_UPLINK_LOW_LATENCY_FIRST_BATCH_FRAMES 1" -or
    $uplinkHeader -notmatch "NUWAX_AUDIO_UPLINK_LOW_LATENCY_STEADY_BATCH_FRAMES 1" -or
    $uplinkHeader -notmatch "NUWAX_CONVERSATION_UPLINK_PACKET_SLOT_DEPTH" -or
    $uplinkHeader -notmatch "NUWAX_CONVERSATION_UPLINK_STAGING_MAX_AGE_MS" -or
    $uplinkHeader -notmatch "(?s)#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS.*?NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH 32.*?#else.*?NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH 128" -or
    $uplinkHeader -notmatch "#define NUWAX_AUDIO_UPLINK_SEND_QUEUE_DEPTH NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH" -or
    $uplink -notmatch "(?s)profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME.*?NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH.*?NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH" -or
    $uplink -notmatch "(?s)nuwax_audio_uplink_start_if_epoch\(.*?NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE" -or
    $uplink -notmatch "(?s)const uint32_t queue_depth =.*?nuwax_audio_uplink_queue_depth_for_profile\(profile\).*?xQueueCreateWithCaps\(queue_depth" -or
    $uplink -notmatch "(?s)if \(audio_uplink_flag_load\(&ctx->send_failed\) \|\|\s*audio_uplink_flag_load\(&ctx->finished\)\).*?audio_uplink_release_unqueued_slot\(ctx, slot\).*?return send_failed" -or
    $uplink -notmatch "(?s)if \(send_err != ESP_OK\).*?audio_uplink_release_packet_slot\(ctx, packet\.slot\).*?break;" -or
    $uplinkHeader -notmatch "NUWAX_AUDIO_UPLINK_REALTIME_FINAL_ADMISSION_WAIT_MS 0U" -or
    $uplinkHeader -notmatch "NUWAX_AUDIO_UPLINK_REALTIME_TERMINAL_SEND_TIMEOUT_MS 2000U" -or
    $uplinkHeader -notmatch "nuwax_audio_uplink_can_skip_unsent_data_packet" -or
    $uplinkHeader -notmatch "nuwax_audio_uplink_reserves_terminal_slot" -or
    $uplink -notmatch "const uint32_t ready_queue_depth =\s*nuwax_audio_uplink_ready_queue_depth" -or
    $uplink -notmatch "const uint32_t pool_slot_count =\s*nuwax_audio_uplink_pool_slot_count" -or
    $uplink -notmatch "final && ctx->terminal_slot_reserved" -or
    $uplink -notmatch "realtime terminal packet admitted without capture-path wait" -or
    $uplink -notmatch "nuwax_audio_uplink_discard_uncommitted" -or
    $uplink -notmatch "nuwax_audio_uplink_request_finish_then" -or
    $uplinkHeader -notmatch "NUWAX_AUDIO_UPLINK_AFTER_END_CONTEXT_MAX_BYTES" -or
    $uplink -notmatch "memcpy\(ctx->after_end_user_ctx, user_ctx, user_ctx_size\)" -or
    $uplink -notmatch "ctx->after_end\(\(void \*\)ctx->after_end_user_ctx," -or
    $main -notmatch "send_seamless_listen_stop_after_end" -or
    $main -notmatch "nuwax_audio_uplink_request_finish_then" -or
    $uplink -notmatch "audio_uplink_flag_store\(&ctx->finished, true\);" -or
    $uplink -notmatch "nuwax_ws_send_binary_with_timeout" -or
    $uplink -notmatch "aborting current audio session" -or
    $uplink -notmatch "audio_uplink_log_realtime_drop" -or
    $uplink -match "nuwax_audio_uplink_recovery_send_budget_ms" -or
    $uplink -match "retrying same buffer within recovery grace" -or
    $uplink -notmatch 'packet\.final \? "terminal" : "reliable"' -or
    $uplink -notmatch "audio_uplink_ensure_start_on_first_sent_packet" -or
    $uplinkHeader -notmatch "_Atomic int send_result;" -or
    $uplink -notmatch "audio_uplink_result_store_first_error" -or
    $uplink -notmatch "(?s)audio_uplink_result_store_first_error\(ctx, ESP_ERR_TIMEOUT\);\s*audio_uplink_flag_store\(&ctx->send_failed, true\)" -or
    $uplink -notmatch "audio_uplink_ensure_reaper_ready" -or
    $uplink -notmatch "NUWAX_AUDIO_UPLINK_REAPER_FATAL_RESTART_MS" -or
    $uplink -notmatch "uplink sender did not release singleton" -or
    $uplink -notmatch "xQueueCreateStatic" -or
    $uplink -notmatch "caller retains session" -or
    $uplink -match '\babort\s*\(' -or
    $main -notmatch 'nuwax_audio_uplink_t \*send_ctx = nuwax_audio_uplink_create\(\)' -or
    $uplink -match "ctx->send_result\s*=") {
    throw "Audio uplink profile regression: isolate realtime/reliable queues and preserve heap-owned, non-aborting sender cleanup."
}
$wsTransport = Get-Content -LiteralPath (Join-Path $projectRoot "main\ws_transport.c") -Raw
if ($wsTransport -notmatch "effective_io_timeout_ms" -or
    $wsTransport -notmatch "total_budget_us" -or
    $wsTransport -notmatch "remaining_us" -or
    $wsTransport -notmatch "transport_wait_write_ready_until" -or
    $wsTransport -notmatch "transport_wait_write_ready_until\(client, soft_deadline_us\)" -or
    $wsTransport -match "transport_write_ready_now") {
    throw "Realtime WebSocket send timeout must cover lock wait and frame I/O."
}
if ($main -notmatch 'const uint32_t wire_sequence = \*recorded_frames;' -or
    $main -notmatch 'const uint32_t wire_sequence = recorded_frames;' -or
    $main -match 'const uint32_t seq = capture_sequence;' -or
    $main -notmatch 'NUWAX_CONVERSATION_CAPTURE_SLOT_DEPTH' -or
    $main -notmatch 'NUWAX_CONVERSATION_CAPTURE_PRE_DSP_MAX_AGE_MS' -or
    $bargeController -notmatch 'NUWAX_CONVERSATION_BARGE_END_TO_END_MAX_AGE_MS' -or
    $defaults -notmatch 'CONFIG_NUWAX_VOICE_UPLINK_QUEUE_DEPTH=16' -or
    $defaults -notmatch 'CONFIG_NUWAX_VOICE_UPLINK_REALTIME_MAX_AGE_MS=160' -or
    $defaults -notmatch 'CONFIG_NUWAX_RT_CAPTURE_QUEUE_DEPTH=12' -or
    $defaults -notmatch 'CONFIG_NUWAX_RT_CAPTURE_MAX_DECISION_AGE_MS=60' -or
    $defaults -notmatch 'CONFIG_NUWAX_BARGE_END_TO_END_MAX_FRAME_AGE_MS=100' -or
    $main -notmatch 'NUWAX_AUDIO_UPLINK_PROFILE_REALTIME' -or
    $main -notmatch 'nuwax_audio_uplink_set_low_latency\(send_ctx, true\)' -or
    $meeting -notmatch 'nuwax_audio_uplink_start_if_epoch\(\s*uplink\s*,\s*ws\s*,\s*ws_epoch\s*\)' -or
    $monitoring -notmatch 'nuwax_audio_uplink_start_if_epoch\(\s*stream->uplink\s*,\s*ws\s*,\s*ws_epoch\s*\)' -or
    $meeting -match 'NUWAX_AUDIO_UPLINK_PROFILE_REALTIME' -or
    $monitoring -match 'NUWAX_AUDIO_UPLINK_PROFILE_REALTIME' -or
    $meeting -match 'nuwax_audio_uplink_set_low_latency' -or
    $monitoring -match 'nuwax_audio_uplink_set_low_latency' -or
    $meeting -notmatch 'nuwax_audio_uplink_discard_uncommitted' -or
    $monitoring -notmatch 'nuwax_audio_uplink_discard_uncommitted') {
    throw "Normal-conversation latency isolation regressed: realtime capture must remain 12 slots and realtime uplink 12 packet slots while meeting and monitoring keep the reliable profile."
}
if ($meeting -notmatch "current_samples \+= samples_read" -or
    $monitoring -notmatch "current_samples \+= samples_read") {
    throw "Meeting/monitoring short mic reads are not accumulated into full frames."
}
if ($deviceState -notmatch "nuwax_monitoring_ui_state_t monitoring" -or
    $drawer -notmatch '(?s)static nuwax_control_center_icon_id_t dialog_icon\(.*?state->monitoring != NUWAX_MONITORING_UI_DISABLED.*?NUWAX_CC_ICON_MONITORING.*?state->meeting != NUWAX_MEETING_UI_DISABLED.*?NUWAX_CC_ICON_MEETING_STATE.*?NUWAX_DIALOG_SLEEP: return NUWAX_CC_ICON_SLEEPING;.*?NUWAX_DIALOG_READY:.*?NUWAX_CC_ICON_CONVERSATION;' -or
    $drawer -notmatch '(?s)static const char \*dialog_text\(.*?NUWAX_DIALOG_SLEEP.*?NUWAX_DIALOG_READY' -or
    $appShell -notmatch "app_shell_state_mailbox_t" -or
    $appShell -notmatch "nuwax_app_shell_set_monitoring_state" -or
    $main -notmatch "nuwax_app_shell_set_monitoring_state") {
    throw "Exclusive-mode UI synchronization, control-center state icon semantics, or retry mailbox is incomplete."
}
if ($session -notmatch '"meeting_mode_v1"' -or
    $session -notmatch '"monitoring_mode_v1"' -or
    $session -notmatch '"remote_ble_reprovision"' -or
    $session -notmatch '"ota_v1"' -or
    $session -notmatch '\{"reason", 64U\}' -or
    $session -notmatch 'cJSON_AddNumberToObject\(audio, "mic_channels", CONFIG_NUWAX_VOICE_CHANNELS\)' -or
    $session -notmatch "nuwax_session_classify_cloud_message") {
    throw "Formal application features, mono media metadata, or cloud compatibility are missing from hello."
}
if ($cloudProtocol -notmatch "NUWAX_CLOUD_MESSAGE_HELLO_REJECTED" -or
    $cloudProtocol -notmatch "NUWAX_CLOUD_MESSAGE_DEVICE_COMMAND" -or
    $cloudProtocol -notmatch "object_key_count" -or
    $main -notmatch "note_positive_cloud_activity" -or
    $main -notmatch '(?s)const int64_t ping_now_ms = now_ms_i64\(\);.*?nuwax_session_send_ping\(&ws, ping_now_ms\)' -or
    $main -notmatch '(?s)if \(!nuwax_ws_is_connected\(&ws\)\) \{.*?nuwax_barge_dispatcher_unbind\(&ws, barge_dispatch_binding\);.*?unpublish_active_ws\(\);.*?const esp_err_t close_err = nuwax_ws_owner_close\(&ws\);.*?continue;' -or
    $main -match "auth_deadline_ms" -or
    $main -match "ignoring cloud payload before authenticated hello ACK") {
    throw "Formal cloud optional-hello compatibility, transport cleanup, or OTA positive-activity fencing regressed."
}
if ($provision -notmatch 'cJSON_AddStringToObject\(root, "hardwareVersion",\s*nuwax_board_name\(\)\)' -or
    $provision -notmatch 'const esp_app_desc_t \*app = esp_app_get_description\(\)' -or
    $provision -notmatch 'cJSON_AddStringToObject\(root, "firmwareVersion",\s*firmware_version\)' -or
    $provision -match 'CyberVoc V1\.2' -or
    $provision -match 'cJSON_AddStringToObject\(root, "firmwareVersion", "7\.0\.0"\)') {
    throw "BLE device-info must report the selected board and running image version."
}
if ($otaDevice -notmatch 'static void request_firmware_metadata_refresh\(void\)' -or
    $otaDevice -notmatch '(?s)emit_result_reliably\((?:true,\s*)?"committed"(?:,\s*"ok")?.*?s_boot_pending_verify = false;.*?refresh_interlocks\(\);\s*if \(err == ESP_OK\) \{\s*/\*.*?\*/\s*request_firmware_metadata_refresh\(\);') {
    throw "A committed OTA image must reconnect once so the next hello reports ota_state=valid."
}
if ($main -notmatch 'static bool ota_blocks_wake_detector\(void\)' -or
    $main -notmatch '(?s)request_sleep_wake_word_detector\(const char \*reason\).*?ota_blocks_wake_detector\(\)' -or
    $main -notmatch '(?s)start_sleep_wake_word_detector\(const char \*reason\).*?ota_blocks_wake_detector\(\)' -or
    $main -notmatch '(?s)ensure_sleep_wake_word_detector\(void\).*?ota_blocks_wake_detector\(\)' -or
    $main -notmatch '(?s)wake_word_supervisor_task\(void \*arg\).*?ota_blocks_wake_detector\(\)' -or
    $main -notmatch '(?s)ota_validate_boot_mic_input\(.*?ESP_ERR_INVALID_STATE.*?ESP_ERR_NOT_FINISHED' -or
    $main -notmatch 'OTA audio isolation and retryable boot mic validation enabled') {
    throw "OTA audio isolation regression: wake recovery must stay inhibited and transient MIC ownership must remain retryable through pending verification."
}
if ($otaDevice -notmatch '(?s)if \(!begin_terminal_report\(\)\).*?emit_result_reliably\("staged",\s*"running image pending validation"\).*?emit_result_reliably\("committed", "health gate passed"\);\s*complete_terminal_report\(err\);') {
    throw "OTA boot confirmation regression: staged and committed ACKs must be matched while a terminal report transaction is in flight."
}
if ($projectCmake -notmatch 'NUWAX_EXPECTED_OTA_RELEASE_SEQUENCE' -or
    $projectCmake -notmatch '7000000 \+ \$\{CMAKE_MATCH_1\}' -or
    $cmake -notmatch 'NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE=\$\{NUWAX_OTA_RELEASE_SEQUENCE\}ULL' -or
    $otaReleasePolicy -notmatch '#ifndef NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE') {
    throw "OTA release identity regression: PROJECT_VER and the compiled release sequence must be derived and checked together."
}
if ($otaDeviceHeader -notmatch 'nuwax_ota_device_handle_text\(const char \*payload, int len,\s*uint32_t connection_epoch\)' -or
    $otaDeviceHeader -notmatch 'nuwax_ota_device_set_cloud_authenticated\(bool authenticated,\s*uint32_t connection_epoch\)' -or
    $otaDevice -notmatch 'uint32_t connection_epoch;' -or
    $otaDevice -notmatch 's_cloud_authenticated_epoch == connection_epoch' -or
    $otaDevice -notmatch 'nuwax_ota_device_cloud_authenticated\(item->connection_epoch\)' -or
    $main -notmatch 'nuwax_ota_device_handle_text\(payload, len,\s*ws_connection_epoch\)' -or
    $main -notmatch 'nuwax_ota_device_set_cloud_authenticated\(true, connection_epoch\)') {
    throw "OTA authorization regression: queued commands and cloud-ready events must remain bound to the authenticated WebSocket epoch that accepted them."
}
if ($otaDevice -notmatch '#define OTA_CONTROLLER_STACK_BYTES\s+12288U' -or
    $otaDevice -notmatch '#define OTA_CONTROLLER_STACK_WARN_BYTES\s+3072U' -or
    $otaDevice -match 'uxTaskGetStackHighWaterMark\(NULL\)\s*\*\s*sizeof\(StackType_t\)' -or
    $otaDevice -notmatch '__attribute__\(\(noinline\)\) void process_update' -or
    $otaDevice -notmatch '(?s)arm_result_ack\(ok\);\s*for \(unsigned attempt.*?if \(result_ack_received\(\)\)' -or
    $otaDevice -notmatch 'report_state == OTA_TERMINAL_REPORT_IN_FLIGHT' -or
    $otaDevice -notmatch 'nuwax_ota_result_ack_policy_decide' -or
    $otaDevice -notmatch 'accepted ota_result_ack persistence compatibility' -or
    $otaResultAckPolicy -notmatch '(?s)!expected_result_ok && received_ack_ok.*?NUWAX_OTA_RESULT_ACK_PERSISTED_SUCCESS_COMPAT' -or
    $otaDevice -notmatch 'ignoring unmatched ota_result_ack epoch_ok=') {
    throw "OTA controller stack regression: retain byte-accurate diagnostics, isolated update frames, and the validated internal-stack reserve."
}
if ($defaults -notmatch 'CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES=16384' -or
    $kconfig -notmatch '(?s)config NUWAX_WS_RX_TASK_STACK_BYTES.*?default 16384' -or
    $wsTransport -notmatch '#define CONFIG_NUWAX_WS_RX_TASK_STACK_BYTES 16384' -or
    $wsTransport -notmatch '#define NUWAX_WS_RX_STACK_WARN_BYTES 4096U' -or
    $wsTransport -match 'uxTaskGetStackHighWaterMark\(NULL\)\s*\*\s*sizeof\(StackType_t\)' -or
    $wsTransport -notmatch 'WebSocket RX stack reserve low' -or
    $main -notmatch 'static __attribute__\(\(noinline\)\) void on_ws_text_non_ota' -or
    $main -notmatch '(?s)static __attribute__\(\(noinline\)\) void on_ws_text_non_ota\(.*?nuwax_session_validate_text_frame\(payload, len\).*?nuwax_session_command_t command' -or
    $main -notmatch '(?s)static void on_ws_text\(.*?nuwax_ota_device_handle_text\(payload, len,\s*ws_connection_epoch\).*?return;.*?on_ws_text_non_ota\(payload, len, ws, ws_connection_epoch\)') {
    throw "WebSocket RX stack regression: OTA dispatch must bypass the ordinary command frame, retain byte-accurate diagnostics, and keep the PSRAM stack reserve."
}
if ($firmwareReport -notmatch 'cJSON_AddStringToObject\(root, "type", "firmware_report"\)' -or
    $firmwareReport -notmatch 'cJSON_AddStringToObject\(root, "report_id", s_report_id\)' -or
    $firmwareReport -notmatch 'cJSON_AddStringToObject\(root, "device_id"' -or
    $firmwareReport -notmatch 'cJSON_AddStringToObject\(root, "device_secret"' -or
    $firmwareReport -notmatch '#define NUWAX_FIRMWARE_TYPE "NuwaxAIPartner_V2"' -or
    $firmwareReport -notmatch 'cJSON_AddStringToObject\(firmware, "firmware_type"' -or
    $firmwareReport -notmatch 'cJSON_AddStringToObject\(firmware, "version"' -or
    $firmwareReport -notmatch 'cJSON_AddStringToObject\(firmware, "image_sha256"' -or
    $firmwareReport -match 'cJSON_AddStringToObject\(firmware, "project_name"' -or
    $firmwareReport -match '"boot_id"' -or
    $firmwareReport -match '"protocol_version"' -or
    $firmwareReport -match '"release_sequence"' -or
    $firmwareReport -match '"target_board"' -or
    $firmwareReport -match '"target_chip"' -or
    $firmwareReport -match '"partition_schema"' -or
    $firmwareReport -match '"build_flavor"' -or
    $firmwareReport -match '"running_partition"' -or
    $firmwareReport -match '"ota_state"') {
    throw "Boot firmware_report must retain the minimal version identity and must not reintroduce redundant OTA metadata."
}
if ($firmwareReport -notmatch '#define FIRMWARE_IDENTITY_TASK_STACK_BYTES 5120U' -or
    $firmwareReport -notmatch '#define FIRMWARE_IDENTITY_TASK_PRIORITY\s+1U' -or
    $firmwareReport -notmatch '#define FIRMWARE_IDENTITY_TASK_CORE\s+0' -or
    $firmwareReport -notmatch '(?s)firmware_identity_worker\(.*?nuwax_ota_main_get_running_image_sha256.*?nuwax_firmware_identity_policy_complete.*?vTaskSuspend\(NULL\)' -or
    $firmwareReport -notmatch '(?s)firmware_identity_ensure_async\(.*?firmware_identity_worker_reap\(\).*?nuwax_firmware_identity_policy_request.*?xTaskCreatePinnedToCoreWithCaps.*?MALLOC_CAP_INTERNAL.*?xTaskNotifyGive\(task\).*?ESP_ERR_NOT_FINISHED' -or
    $firmwareReport -notmatch '(?s)nuwax_firmware_report_service\(.*?firmware_identity_ensure_async\(\).*?nuwax_firmware_report_policy_note_attempt' -or
    $otaMain -notmatch '(?s)partition_sha256_hex\(.*?NUWAX_OTA_IDENTITY_HASH_YIELD_BYTES.*?vTaskDelay\(1\)') {
    throw "Firmware identity recovery must remain asynchronous, internal-stack safe, cooperatively chunked, and must not consume a report attempt before identity readiness."
}
if ($otaMain -notmatch '(?s)running_identity_condition\(esp_err_t err\).*?ESP_ERR_NVS_NOT_FOUND.*?NUWAX_OTA_IDENTITY_MISSING' -or
    $otaMain -notmatch '(?s)running_identity_condition\(esp_err_t err\).*?ESP_ERR_INVALID_CRC.*?NUWAX_OTA_IDENTITY_STALE' -or
    $otaMain -notmatch '(?s)nuwax_ota_main_get_running_image_sha256\(.*?nuwax_ota_running_image_pending_verify\(&pending_verify\).*?nuwax_ota_identity_partition_fallback_allowed\(\s*pending_verify, identity_condition\).*?esp_image_get_metadata.*?partition_sha256_hex') {
    throw "OTA running identity recovery regression: stale accepted metadata may fall back only for a non-pending running image."
}
$uidInitOffset = $deviceUid.IndexOf("esp_err_t nuwax_device_uid_init(void)")
$uidInit = if ($uidInitOffset -ge 0) { $deviceUid.Substring($uidInitOffset) } else { "" }
$deriveUidOffset = $uidInit.IndexOf("derive_uid_from_efuse(s_uid.raw)")
$loadUidOffset = $uidInit.IndexOf("load_uid_from_nvs(cached_raw)")
if ($uidInit.Length -eq 0 -or
    $deriveUidOffset -lt 0 -or $loadUidOffset -le $deriveUidOffset -or
    $uidInit -notmatch 'memcmp\(cached_raw, s_uid\.raw, sizeof\(s_uid\.raw\)\) == 0' -or
    $uidInit -notmatch 'persist_uid_to_nvs\(s_uid\.raw\)' -or
    $deviceUid -match 'device_uid persisted to NVS:') {
    throw "Device identity regression: derive from immutable hardware on every boot, verify/repair the NVS cache, and do not log the raw cached digest."
}
if ($audioBoard -notmatch '\.hardware_profile = "miaopan_v2_0_n32r8"' -or
    $audioBoard -match '\.hardware_profile = "esp_vocat_v1_0"') {
    throw "V2 audio capabilities must not advertise the legacy hardware profile."
}
if ($audioBoard -notmatch 'current->channels == candidate->channels' -or
    $audioBoard -notmatch 'stale or malformed audio lease release rejected' -or
    $audioBoard -notmatch 'audio board init retrying pending teardown before reinitialization' -or
    $vocatAudioBoard -notmatch 'static bool pa_output_latch_enabled\(void\)' -or
    $vocatAudioBoard -notmatch 'REG_READ\(GPIO_OUT_REG\)' -or
    $vocatAudioBoard -notmatch 'REG_READ\(GPIO_OUT1_REG\)' -or
    $vocatAudioBoard -match 'gpio_get_level\(NUWAX_VOCAT_PA_CTRL_GPIO\)' -or
    $vocatAudioBoard -notmatch '(?s)ESP_LOGI\(TAG,\s*"PA forced off GPIO%d=0 elapsed_ms=%lu feedback=output_latch"' -or
    $vocatAudioBoard -match '(?s)ESP_LOGE\(TAG,\s*"PA forced off GPIO%d=0 elapsed_ms=%lu feedback=output_latch"' -or
    $vocatAudioBoard -notmatch '(?s)if \(pa_output_latch_enabled\(\)\) \{\s*ESP_LOGE\(TAG, "force PA-off GPIO latch mismatch"\)' -or
    $vocatAudioBoard -notmatch '(?s)if \(elapsed_ms > CONFIG_NUWAX_VOCAT_PA_TRANSITION_MAX_MS\) \{\s*ESP_LOGE\(TAG,\s*"force PA-off exceeded deadline' -or
    $vocatAudioBoard -notmatch 'handle retained for retry' -or
    $vocatAudioBoard -match '\(void\)i2s_del_channel' -or
    $main -notmatch 'audio_diagnostic_teardown_retry' -or
    $main -notmatch 'result_err = ref_err != ESP_OK \? ref_err : health_err' -or
    $main -notmatch 'ref_err != ESP_OK \|\| health_err != ESP_OK' -or
    $main -notmatch 'const esp_err_t ref_gate_err' -or
    $productReadyHeader -notmatch 'bool hardware_ref_required;' -or
    $productReady -notmatch 'inputs->hardware_ref_required && !inputs->hardware_ref_released') {
    throw "Audio lifecycle regression: PA output-latch validation, lease identity, rollback retention, diagnostic result propagation, teardown retry, and optional hardware-reference readiness must remain fail-closed."
}
if ($wifiManager -notmatch 'WIFI_ONLINE_SCAN_ACTIVE_MAX_MS 80U' -or
    $wifiManager -notmatch 'WIFI_ONLINE_SCAN_HOME_DWELL_MS 30U' -or
    $wifiManager -notmatch 'WIFI_ONLINE_SCAN_TIMEOUT_MS 8000U' -or
    $wifiManager -notmatch '\.scan_time\.active = \{' -or
    $wifiManager -notmatch '\.home_chan_dwell_time = WIFI_ONLINE_SCAN_HOME_DWELL_MS' -or
    $wifiManager -notmatch 'esp_wifi_scan_start\(&scan_config, false\)' -or
    $wifiManager -notmatch 'xEventGroupWaitBits\(' -or
    $wifiManager -notmatch 'esp_wifi_scan_stop\(\)') {
    throw "Online Wi-Fi scans must be asynchronous, time-bounded, and limit off-channel dwell."
}
if ($touchInput -notmatch 'DRAM_ATTR uint8_t s_touch_i2c_tx_scratch\[2\]' -or
    $touchInput -notmatch 'DRAM_ATTR uint8_t s_touch_i2c_rx_scratch\[6\]' -or
    $touchInput -notmatch 'i2c_master_transmit_receive\(\s*s_touch_dev, s_touch_i2c_tx_scratch, 1, s_touch_i2c_rx_scratch' -or
    $touchInput -notmatch '(?s)xTaskCreateWithCaps\(touch_log_task,.*?MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT\)' -or
    $main -notmatch '(?s)create_app_task\(nuwax_touch_gesture_task,.*?CONFIG_NUWAX_TOUCH_GESTURE_TASK_STACK_SIZE' -or
    $main -match '(?s)create_app_task_internal\(nuwax_touch_gesture_task,.*?CONFIG_NUWAX_TOUCH_GESTURE_TASK_STACK_SIZE') {
    throw "Touch I2C buffers and the I2C worker stack must stay internal while the large gesture stack remains in PSRAM."
}
if ($kconfig -match 'config NUWAX_CONVERSATION_PLAYBACK_ABSOLUTE_TIMEOUT_MS' -or
    $defaults -match 'CONFIG_NUWAX_CONVERSATION_PLAYBACK_ABSOLUTE_TIMEOUT_MS' -or
    $main -match 'playback_absolute_timeout' -or
    $main -match 'NUWAX_CONVERSATION_PLAYBACK_ABSOLUTE_TIMEOUT_MS' -or
    $main -notmatch 'NUWAX_CONVERSATION_PLAYBACK_INGRESS_IDLE_TIMEOUT_MS 60000U' -or
    $main -notmatch 's_speaker_ingress_pending = true' -or
    $main -notmatch 'tts_no_audio_timeout' -or
    $main -notmatch 'nuwax_session_send_cancel_turn_until_if_epoch') {
    throw "Normal conversation playback must use a sliding 60-second cloud-ingress idle timeout with no absolute duration cap and an epoch-bound cancel path."
}
if ($cmake -notmatch 'audio_modes/meeting/meeting_mode.c' -or
    $cmake -notmatch 'audio_modes/monitoring/monitoring_mode.c' -or
    $cmake -notmatch 'nuwax_provision_new/nuwax_provision_main.c' -or
    $cmake -notmatch 'conversation/audio_uplink_terminal.c' -or
    $cmake -notmatch 'nuwax_ota_new/nuwax_ota_device.c' -or
    $cmake -notmatch 'cloud_protocol_compat.c') {
    throw "V2 component source registration is incomplete."
}
if ($defaults -notmatch 'CONFIG_NUWAX_POWER_CTRL_GPIO=7' -or
    $defaults -notmatch 'CONFIG_NUWAX_POWER_KEY_GPIO=16' -or
    $defaults -notmatch 'CONFIG_NUWAX_POWER_KEY_FORCE_OFF_HOLD_MS=5000' -or
    $defaults -notmatch 'CONFIG_NUWAX_MIC_I2S_BCLK_GPIO=40' -or
    $defaults -notmatch 'CONFIG_NUWAX_MIC_I2S_WS_GPIO=39' -or
    $defaults -notmatch 'CONFIG_NUWAX_MIC_I2S_DIN_GPIO=38' -or
    $defaults -notmatch 'CONFIG_NUWAX_SPEAKER_I2S_DOUT_GPIO=41' -or
    $defaults -notmatch 'CONFIG_NUWAX_DISPLAY_SPI_HZ=40000000') {
    throw "Miaopan V2.0 board defaults are incomplete."
}
$powerKeyTask = [regex]::Match(
    $powerCtrl,
    '(?s)static void power_key_task\(.*?(?=static esp_err_t start_power_key_task)').Value
if ($powerCtrl -notmatch 'NUWAX_POWER_KEY_TASK_PRIORITY\s+\(configMAX_PRIORITIES - 1U\)' -or
    $powerCtrl -notmatch 'xTaskCreateStatic\(' -or
    $powerCtrl -notmatch 'xTaskDelayUntil\(' -or
    $powerCtrl -notmatch 'CONFIG_NUWAX_POWER_KEY_FORCE_OFF_HOLD_MS' -or
    $powerCtrl -notmatch 'claim_shutdown_gate\(bool force' -or
    $powerCtrl -notmatch 'reset_press_is_shutdown_recovery' -or
    $powerKeyTask -match 'ESP_LOG[DIWEV]\(' -or
    $powerCtrl -match 'nuwax_display_backlight_force_off\(' -or
    $main -notmatch 'power control initialization failed') {
    throw "SW3 must use a static highest-priority worker, real-tick debounce, and a lock-independent emergency force-off path."
}
if ($productDefaults -notmatch 'CONFIG_NUWAX_OTA_ENABLE=y' -or
    $productDefaults -notmatch 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y' -or
    $productDefaults -notmatch 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.customwake.csv"' -or
    $productDefaults -notmatch '(?m)^# CONFIG_NUWAX_LIVE_AEC_MODE_HIGH_PERF is not set\s*$' -or
    $productDefaults -notmatch '(?m)^CONFIG_NUWAX_LIVE_AEC_MODE_LOW_COST=y\s*$' -or
    $productDefaults -notmatch '(?m)^# CONFIG_NUWAX_LIVE_AEC_SHADOW_DELAY_ENABLE is not set\s*$' -or
    $productDefaults -notmatch '(?m)^# CONFIG_NUWAX_LIVE_AEC_DYNAMIC_FIR_DELAY_ENABLE is not set\s*$' -or
    $productDefaults -notmatch '(?m)^# CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_ENABLE is not set\s*$' -or
    $productDefaults -notmatch '(?m)^CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_ENABLE=y\s*$' -or
    $productDefaults -notmatch '(?m)^# CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE is not set\s*$' -or
    $productDefaults -notmatch '(?m)^CONFIG_NUWAX_VOICE_UPLINK_QUEUE_DEPTH=16\s*$' -or
    $productDefaults -notmatch '(?m)^CONFIG_NUWAX_VOICE_UPLINK_REALTIME_MAX_AGE_MS=160\s*$' -or
    $productDefaults -notmatch '(?m)^CONFIG_NUWAX_RT_CAPTURE_QUEUE_DEPTH=12\s*$' -or
    $productDefaults -notmatch '(?m)^CONFIG_NUWAX_RT_CAPTURE_MAX_DECISION_AGE_MS=60\s*$' -or
    $productDefaults -notmatch '(?m)^CONFIG_NUWAX_BARGE_END_TO_END_MAX_FRAME_AGE_MS=100\s*$' -or
    $productDefaults -notmatch '(?m)^CONFIG_NUWAX_SPEAKER_BUFFER_BYTES=393216\s*$' -or
    $productDefaults -notmatch '(?m)^CONFIG_NUWAX_SPEAKER_BACKPRESSURE_POLL_MS=20\s*$' -or
    $defaults -notmatch '(?m)^CONFIG_NUWAX_LIVE_AEC_NEAREND_RAW_MIX_Q15=0\s*$' -or
    $defaults -notmatch '(?m)^CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_RAW_MIX_Q15=0\s*$') {
    throw "Product OTA defaults are incomplete."
}
if ($projectCmake -notmatch 'idf_component_get_property\(esp_audio_codec_component espressif__esp_audio_codec COMPONENT_LIB\)' -or
    $projectCmake -notmatch 'idf_component_get_property\(esp_audio_codec_dir espressif__esp_audio_codec COMPONENT_DIR\)' -or
    $projectCmake -notmatch 'add_prebuilt_library\(nuwax_esp_audio_codec_prebuilt "\$\{esp_audio_codec_archive\}"\)' -or
    $projectCmake -notmatch 'message\(FATAL_ERROR "esp_audio_codec 1\.0\.1 layout changed; update the Windows path fix"\)' -or
    $projectCmake -notmatch 'list\(FILTER esp_audio_codec_links EXCLUDE REGEX "\^-L"\)' -or
    $projectCmake -notmatch 'list\(FILTER esp_audio_codec_links EXCLUDE REGEX "\^esp_audio_codec\$"\)' -or
    $projectCmake -notmatch 'list\(APPEND esp_audio_codec_links nuwax_esp_audio_codec_prebuilt\)' -or
    $projectCmake -notmatch 'PROPERTY INTERFACE_LINK_LIBRARIES "\$\{esp_audio_codec_links\}"') {
    throw "The source-controlled esp_audio_codec path-with-spaces link fix is incomplete."
}

Write-Host "V2 product migration host contracts: PASS"
