param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content "$root/main/conversation/speaker_playback.c" -Raw
$a=$source.IndexOf('static bool save_speaker_volume_percent(')
$b=$source.IndexOf('static bool speaker_get_drop_state(', $a)
if ($a -lt 0 -or $b -le $a) { throw 'Production persistence function not found' }
[IO.File]::WriteAllText("$out/speaker_volume_persistence_host.inc",$source.Substring($a,$b-$a))
& $Gcc -std=c11 -Wall -Wextra -Werror "-I$out" "$root/main/speaker_volume_persistence_host_check.c" -o "$out/speaker_volume_persistence_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/speaker_volume_persistence_host_check.exe"
exit $LASTEXITCODE
