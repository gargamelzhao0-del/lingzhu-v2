param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
& $Gcc -std=c11 -Wall -Wextra -Werror "-I$root/main" "$root/main/runtime_metrics_storage_host_check.c" "$root/main/conversation/conversation_metrics.c" -o "$out/runtime_metrics_storage_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/runtime_metrics_storage_host_check.exe"
exit $LASTEXITCODE
