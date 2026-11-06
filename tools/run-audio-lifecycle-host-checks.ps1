param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content -LiteralPath "$root/main/offline_audio_spool.c" -Raw
$start=$source.IndexOf('/* The real-time producer retains PCM')
$end=$source.IndexOf('static TickType_t recovery_remaining_ticks(', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Missing production mirror block' }
$defines=([regex]::Matches($source,'(?m)^#define (?:OFFLINE_MIRROR_ATTACH_RETRY_MS|MIRROR_STAGING_FRAMES|MIRROR_CLOSE_TIMEOUT_MS) .*$') | ForEach-Object Value) -join "`n"
$spaceStart=$source.IndexOf('static bool recording_space_blocked(void)')
$spaceEnd=$source.IndexOf('static bool enabled(void);', $spaceStart)
if ($spaceStart -lt 0 -or $spaceEnd -le $spaceStart) { throw 'Missing production space admission' }
$space=$source.Substring($spaceStart,$spaceEnd-$spaceStart)
[IO.File]::WriteAllText("$out/offline_mirror_host.inc", "$defines`n$space`n"+$source.Substring($start,$end-$start))
& $Gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function `
    "-I$root/main/audio_modes/meeting/host_stubs" "-I$root/main" "-I$out" `
    "$root/main/offline_audio_mirror_host_check.c" -o "$out/offline_audio_mirror_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/offline_audio_mirror_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$main=Get-Content -LiteralPath "$root/main/nuwax_ai_partner_main.c" -Raw
$start=$main.IndexOf('static void sync_exclusive_audio_mode_ui_state(void) {')
$end=$main.IndexOf('static void sync_pending_semantic_emotion(', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Missing production UI function' }
[IO.File]::WriteAllText("$out/audio_mode_ui_host.inc",$main.Substring($start,$end-$start))
& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$root/main/audio_modes/meeting/host_stubs" "-I$out" `
    "$root/main/audio_mode_ui_lifecycle_host_check.c" -o "$out/audio_mode_ui_lifecycle_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/audio_mode_ui_lifecycle_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# Ensure neither dispatch loop nor the final MIC dispatcher skips the recovery controller.
if ($main -notmatch '(?s)static esp_err_t run_audio_source\(nuwax_ws_client_t \*ws\) \{.*?offline_mode_lease_pending\(\) && !controller_ready' -or
    ([regex]::Matches($main,'"audio_owner_wait"')).Count -lt 2) { throw 'Lifecycle dispatch gates regressed' }
& "$PSScriptRoot/run-offline-dispatch-host-checks.ps1" -Gcc $Gcc
exit $LASTEXITCODE
