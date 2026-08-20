param(
    [string]$Gcc = "gcc"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$monitoringDir = Join-Path $projectRoot "main/audio_modes/monitoring"
$hostStubDir = Join-Path $projectRoot "main/audio_modes/meeting/host_stubs"
$cjsonDir = Join-Path $projectRoot "managed_components/espressif__cjson/cJSON"
$outputDir = Join-Path $projectRoot "build-host-checks"

New-Item -ItemType Directory -Force $outputDir | Out-Null

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$monitoringDir" `
    (Join-Path $monitoringDir "monitoring_speech_policy_host_check.c") `
    -o (Join-Path $outputDir "monitoring_speech_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $outputDir "monitoring_speech_policy_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$hostStubDir" `
    "-I$monitoringDir" `
    "-I$(Join-Path $projectRoot 'main')" `
    "-I$cjsonDir" `
    (Join-Path $monitoringDir "monitoring_protocol.c") `
    (Join-Path $cjsonDir "cJSON.c") `
    (Join-Path $monitoringDir "monitoring_protocol_host_check.c") `
    -o (Join-Path $outputDir "monitoring_protocol_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $outputDir "monitoring_protocol_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function `
    "-I$hostStubDir" `
    "-I$monitoringDir" `
    "-I$(Join-Path $projectRoot 'main/conversation')" `
    "-I$(Join-Path $projectRoot 'main')" `
    "-I$cjsonDir" `
    (Join-Path $monitoringDir "monitoring_mode.c") `
    (Join-Path $monitoringDir "monitoring_protocol.c") `
    (Join-Path $cjsonDir "cJSON.c") `
    (Join-Path $monitoringDir "monitoring_mode_host_check.c") `
    -o (Join-Path $outputDir "monitoring_mode_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $outputDir "monitoring_mode_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function `
    "-I$(Join-Path $monitoringDir 'host_stubs')" `
    "-I$hostStubDir" `
    "-I$monitoringDir" `
    "-I$(Join-Path $projectRoot 'main/conversation')" `
    "-I$(Join-Path $projectRoot 'main')" `
    "-I$cjsonDir" `
    (Join-Path $monitoringDir "monitoring_mode.c") `
    (Join-Path $monitoringDir "monitoring_protocol.c") `
    (Join-Path $cjsonDir "cJSON.c") `
    (Join-Path $monitoringDir "monitoring_capture_host_check.c") `
    -o (Join-Path $outputDir "monitoring_capture_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $outputDir "monitoring_capture_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$monitoringModeSource = Get-Content (Join-Path $monitoringDir "monitoring_mode.c") -Raw
$monitoringBind = $monitoringModeSource.IndexOf('nuwax_audio_uplink_bind_terminal_recovery(')
$monitoringListen = $monitoringModeSource.IndexOf('nuwax_session_send_listen_with_audio_session_until_if_epoch(')
if ($monitoringBind -lt 0 -or $monitoringListen -le $monitoringBind -or
    $monitoringModeSource -notmatch '(?s)nuwax_audio_uplink_bind_terminal_recovery\(\s*stream->uplink,\s*"monitoring",\s*mode->audio_session_id,\s*NULL,' -or
    $monitoringModeSource -notmatch '(?s)nuwax_audio_terminal_recovery_arm\(\s*ws,\s*ws_epoch,\s*NULL,\s*0U,\s*true,\s*"monitoring",\s*mode->audio_session_id,\s*NULL,') {
    throw "Monitoring terminal recovery regression: recovery must bind before listen start and recovered listen-stop must preserve request_id=NULL."
}

exit 0
