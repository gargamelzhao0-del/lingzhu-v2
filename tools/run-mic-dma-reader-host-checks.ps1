param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
& $Gcc -std=c11 -Wall -Wextra -Werror "-I$root/main" "$root/main/mic_dma_reader_host_check.c" -o "$out/mic_dma_reader_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/mic_dma_reader_host_check.exe"
exit $LASTEXITCODE
