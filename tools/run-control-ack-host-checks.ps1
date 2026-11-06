param([string]$Gcc = 'gcc')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
& $Gcc -std=c11 -O2 -Wall -Wextra -Werror -pthread `
    "-I$root/main/control_ack_tx_host_stubs" `
    "-I$root/main/session_protocol_host_stubs" "-I$root/main" `
    "$root/main/control_ack_tx_host_check.c" `
    "$root/main/conversation/session_dispatch.c" `
    "$root/main/conversation/wake_word_config.c" `
    -o "$out/control_ack_tx_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/control_ack_tx_host_check.exe"
exit $LASTEXITCODE
