param(
    [string]$Gcc = "gcc"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$meetingDir = Join-Path $projectRoot "main/audio_modes/meeting"
$hostStubDir = Join-Path $meetingDir "host_stubs"
$cjsonDir = Join-Path $projectRoot "managed_components/espressif__cjson/cJSON"
$deviceErrorHostStub = Join-Path $hostStubDir "device_error_report.c"
$outputDir = Join-Path $projectRoot "build-host-checks"

New-Item -ItemType Directory -Force $outputDir | Out-Null
& "$PSScriptRoot/prepare-audio-uplink-producer-host.ps1" -OutputDir $outputDir

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$meetingDir" `
    (Join-Path $meetingDir "meeting_processor.c") `
    (Join-Path $meetingDir "meeting_processor_host_check.c") `
    -o (Join-Path $outputDir "meeting_processor_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $outputDir "meeting_processor_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$hostStubDir" `
    "-I$meetingDir" `
    "-I$(Join-Path $projectRoot 'main')" `
    "-I$cjsonDir" `
    (Join-Path $meetingDir "meeting_protocol.c") `
    (Join-Path $cjsonDir "cJSON.c") `
    (Join-Path $meetingDir "meeting_protocol_host_check.c") `
    -o (Join-Path $outputDir "meeting_protocol_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $outputDir "meeting_protocol_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function `
    "-I$hostStubDir" `
    "-I$meetingDir" `
    "-I$(Join-Path $projectRoot 'main/conversation')" `
    "-I$(Join-Path $projectRoot 'main')" `
    "-I$cjsonDir" `
    (Join-Path $meetingDir "meeting_mode.c") `
    (Join-Path $meetingDir "meeting_processor.c") `
    (Join-Path $meetingDir "meeting_protocol.c") `
    (Join-Path $cjsonDir "cJSON.c") `
    $deviceErrorHostStub `
    (Join-Path $meetingDir "meeting_mode_host_check.c") `
    -o (Join-Path $outputDir "meeting_mode_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $outputDir "meeting_mode_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $Gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function `
    "-I$(Join-Path $projectRoot 'main/audio_modes/monitoring/host_stubs')" `
    "-I$hostStubDir" `
    "-I$meetingDir" `
    "-I$(Join-Path $projectRoot 'main/conversation')" `
    "-I$(Join-Path $projectRoot 'main')" `
    "-I$cjsonDir" `
    (Join-Path $meetingDir "meeting_mode.c") `
    (Join-Path $meetingDir "meeting_processor.c") `
    (Join-Path $meetingDir "meeting_protocol.c") `
    (Join-Path $cjsonDir "cJSON.c") `
    $deviceErrorHostStub `
    (Join-Path $meetingDir "meeting_capture_transaction_host_check.c") `
    "-I$outputDir" `
    (Join-Path $projectRoot 'main/audio_frame.c') `
    (Join-Path $projectRoot 'main/conversation/audio_uplink_terminal.c') `
    -o (Join-Path $outputDir "meeting_capture_transaction_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $outputDir "meeting_capture_transaction_host_check.exe")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$meetingModeSource = Get-Content (Join-Path $meetingDir "meeting_mode.c") -Raw
$meetingBind = $meetingModeSource.IndexOf('nuwax_audio_uplink_bind_terminal_recovery(')
$meetingListen = $meetingModeSource.IndexOf('nuwax_session_send_listen_with_audio_session_until_if_epoch(')
if ($meetingBind -lt 0 -or $meetingListen -le $meetingBind -or
    $meetingModeSource -notmatch '(?s)nuwax_audio_uplink_bind_terminal_recovery\(\s*uplink,\s*"meeting",\s*mode->audio_session_id,\s*NULL,' -or
    $meetingModeSource -notmatch '(?s)nuwax_audio_terminal_recovery_arm\(\s*ws,\s*ws_epoch,\s*NULL,\s*0U,\s*true,\s*"meeting",\s*mode->audio_session_id,\s*NULL,') {
    throw "Meeting terminal recovery regression: recovery must bind before listen start and recovered listen-stop must preserve request_id=NULL."
}

exit 0
