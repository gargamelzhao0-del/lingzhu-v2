param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content "$root/main/offline_audio_spool.c" -Raw
$a=$source.IndexOf('static esp_err_t find_next_ready_name(')
$b=$source.IndexOf('static bool replay_ota_active(', $a)
if ($a -lt 0 -or $b -le $a) { throw 'Ready-directory extraction boundaries not found' }
[IO.File]::WriteAllText("$out/replay_directory_host.inc",$source.Substring($a,$b-$a))
$a=$source.IndexOf('static esp_err_t refresh_next_segment_index(')
if ($a -lt 0) { throw 'Production filename scanner not found' }
$b=$source.IndexOf('static esp_err_t recover_open_segments(', $a)
$indexSource=$source.Substring($a,$b-$a)
$a=$source.IndexOf('static uint64_t scan_used_bytes(')
$b=$source.IndexOf('static void log_sd_step(', $a)
$ready=[regex]::Match($source,'(?ms)^bool nuwax_offline_audio_has_ready\(void\)\s*\{.*?^\}')
if (!$ready.Success) { throw 'Production ready predicate missing' }
[IO.File]::WriteAllText("$out/archive_inventory_host.inc",$indexSource+$source.Substring($a,$b-$a)+$ready.Value)
& $Gcc -std=c11 -Wall -Wextra -Werror -DCONFIG_NUWAX_SD_SESSION_ENABLE=1 "-I$out" "-I$root/main" "$root/main/archive_directory_host_check.c" -o "$out/archive_directory_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location $out
try { & "$out/archive_directory_host_check.exe" } finally { Pop-Location }
exit $LASTEXITCODE
