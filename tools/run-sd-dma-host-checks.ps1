param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content "$root/main/board/sd_dma_bulk.c" -Raw
$a=$source.IndexOf('static atomic_uintptr_t s_recovery_reader;')
if ($a -lt 0) { throw 'Production DMA adapter not found' }
[IO.File]::WriteAllText("$out/sd_dma_bulk_host.inc",$source.Substring($a))
& $Gcc -std=c11 -Wall -Wextra -Werror "-I$out" "-I$root/main" "$root/main/sd_dma_bulk_host_check.c" -o "$out/sd_dma_bulk_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/sd_dma_bulk_host_check.exe"
exit $LASTEXITCODE
