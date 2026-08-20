param(
    [string]$Gcc = "gcc"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$stubDir = Join-Path $projectRoot "main/session_protocol_host_stubs"
$cjsonDir = Join-Path $projectRoot "managed_components/espressif__cjson/cJSON"
$outputDir = Join-Path $projectRoot "build-host-checks/wake-word"
$exe = Join-Path $outputDir "session_wake_word_host_check.exe"

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

& $Gcc -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections `
    "-I$stubDir" `
    "-I$(Join-Path $projectRoot 'main')" `
    "-I$cjsonDir" `
    (Join-Path $projectRoot "main/session_protocol.c") `
    (Join-Path $projectRoot "main/conversation/session_dispatch.c") `
    (Join-Path $projectRoot "main/conversation/wake_word_config.c") `
    (Join-Path $cjsonDir "cJSON.c") `
    (Join-Path $projectRoot "main/session_wake_word_host_check.c") `
    "-Wl,--gc-sections" `
    -o $exe
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $exe
exit $LASTEXITCODE
