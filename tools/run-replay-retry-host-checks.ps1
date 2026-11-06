param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content "$root/main/offline_audio_spool.c" -Raw
$a=$source.IndexOf('static esp_err_t replay_exchange_chunk(')
$b=$source.IndexOf('static esp_err_t validate_archive_records(', $a)
if ($a -lt 0 -or $b -le $a) { throw 'Missing production chunk exchange' }
$define=([regex]::Match($source,'(?m)^#define REPLAY_CHUNK_ACK_ATTEMPTS .*')).Value
[IO.File]::WriteAllText("$out/replay_retry_host.inc",$define+"`n"+$source.Substring($a,$b-$a))
& $Gcc -std=c11 -Wall -Wextra -Werror "-I$root/main/audio_modes/meeting/host_stubs" "-I$out" "$root/main/replay_retry_host_check.c" -o "$out/replay_retry_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/replay_retry_host_check.exe"
exit $LASTEXITCODE
