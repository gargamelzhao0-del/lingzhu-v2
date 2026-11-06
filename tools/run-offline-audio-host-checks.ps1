param([string]$Gcc = "gcc")
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'build-host-checks'
New-Item -ItemType Directory -Force $outputDir | Out-Null
& "$PSScriptRoot/prepare-audio-uplink-producer-host.ps1" -OutputDir $outputDir
$voiceExe = Join-Path $outputDir 'offline_audio_voice_host_check.exe'
& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$projectRoot/main/audio_modes/meeting/host_stubs" `
    "-I$projectRoot/main" `
    "-I$projectRoot/managed_components/espressif__cjson/cJSON" `
    "$projectRoot/main/offline_audio_voice.c" `
    "$projectRoot/main/offline_audio_voice_host_check.c" -o $voiceExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $voiceExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Compile the real monitoring capture loop and real voice recorder together.
# The harness rejects cloud resume and checks SD still captures those frames.
$captureExe = Join-Path $outputDir 'monitoring_sd_capture_host_check.exe'
& $Gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function `
    -DCONFIG_NUWAX_OFFLINE_AUDIO_ENABLE=1 `
    -DNUWAX_HOST_HEAP_CAPS_FAULT_INJECT=1 `
    "-I$projectRoot/main/audio_modes/monitoring/host_stubs" `
    "-I$projectRoot/main/audio_modes/meeting/host_stubs" `
    "-I$projectRoot/main/audio_modes/monitoring" `
    "-I$projectRoot/main/conversation" "-I$projectRoot/main" `
    "-I$projectRoot/managed_components/espressif__cjson/cJSON" `
    "$projectRoot/main/audio_modes/monitoring/monitoring_mode.c" `
    "$projectRoot/main/audio_modes/monitoring/monitoring_protocol.c" `
    "$projectRoot/main/offline_audio_voice.c" `
    "$projectRoot/managed_components/espressif__cjson/cJSON/cJSON.c" `
    "$projectRoot/main/audio_modes/meeting/host_stubs/device_error_report.c" `
    "$projectRoot/main/audio_modes/monitoring/monitoring_capture_host_check.c" `
    "-I$outputDir" `
    "$projectRoot/main/audio_frame.c" `
    "$projectRoot/main/conversation/audio_uplink_terminal.c" `
    -o $captureExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $captureExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'run-replay-retry-host-checks.ps1') -Gcc $Gcc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'run-replay-window-host-checks.ps1') -Gcc $Gcc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'run-sd-dma-host-checks.ps1') -Gcc $Gcc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'run-sd-event-host-checks.ps1') -Gcc $Gcc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'run-sd-speed-host-checks.ps1') -Gcc $Gcc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'run-archive-directory-host-checks.ps1') -Gcc $Gcc
exit $LASTEXITCODE
