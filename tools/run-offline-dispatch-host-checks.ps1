param([string]$Gcc='gcc', [switch]$Formal)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$main=Get-Content "$root/main/nuwax_ai_partner_main.c" -Raw
$a=$main.IndexOf('static bool offline_replay_control_busy(void)')
$b=$main.IndexOf('static esp_err_t service_offline_audio_handoff(nuwax_ws_client_t *ws)', $a)
$c=$main.IndexOf('static esp_err_t service_idle_wait_application_keepalive(')
$d=$main.IndexOf('static void store_cloud_connection_state(', $c)
if ($a -lt 0 -or $b -lt $a -or $c -lt 0 -or $d -lt $c) { throw 'Production dispatch functions missing' }
[IO.File]::WriteAllText("$out/offline_dispatch_host.inc", $main.Substring($a,$b-$a)+$main.Substring($c,$d-$c))
$spool=Get-Content "$root/main/offline_audio_spool.c" -Raw
$crcStart=$spool.IndexOf('static uint32_t crc32_update(')
$crcEnd=$spool.IndexOf('static esp_err_t validate_archive_frame(', $crcStart)
# Extract only storage_busy: prepare_shutdown was added before replay_send_json
# and belongs to the separate board/power lifecycle checks, not these stubs.
$busyFunction=[regex]::Match($spool,'(?ms)^bool nuwax_offline_audio_storage_busy\(void\)\s*\{.*?^\}')
if ($crcStart -lt 0 -or $crcEnd -lt $crcStart -or !$busyFunction.Success) { throw 'Production storage functions missing' }
[IO.File]::WriteAllText("$out/offline_storage_host.inc",$spool.Substring($crcStart,$crcEnd-$crcStart)+$busyFunction.Value)
& $Gcc -std=c11 -Wall -Wextra -Werror "-I$out" "$root/main/offline_storage_host_check.c" -o "$out/offline_storage_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/offline_storage_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$a=$spool.IndexOf('static atomic_uint s_mic_transfer =')
$b=$spool.IndexOf('uint32_t nuwax_offline_audio_next_capture_sequence(void)', $a)
if ($a -lt 0 -or $b -lt $a) { throw 'Production transfer functions missing' }
[IO.File]::WriteAllText("$out/offline_mic_transfer_host.inc",$spool.Substring($a,$b-$a))
# Check that the production idle loop actually reaches the tested service.
$a=$main.IndexOf('static esp_err_t wait_for_mic_start_action(')
$b=$main.IndexOf('static ', $a+20)
if (!$main.Substring($a,$b-$a).Contains('service_idle_wait_application_keepalive(ws)')) { throw 'Idle dispatch disconnected' }
$dispatchFlags=@()
$dispatchExe="$out/offline_audio_dispatch_host_check.exe"
if ($Formal) {
    $startFunction=[regex]::Match($spool,'(?ms)^static esp_err_t replay_formal_start_locked\(.*?^\}')
    $urlFunction=[regex]::Match($spool,'(?ms)^static esp_err_t replay_test_build_url\(.*?^\}')
    if (!$startFunction.Success -or !$urlFunction.Success) { throw 'Production direct replay functions missing' }
    [IO.File]::WriteAllText("$out/offline_formal_start_host.inc",$startFunction.Value+"`n"+$urlFunction.Value)
    & $Gcc -std=c11 -Wall -Wextra -Werror "-I$out" `
        "$root/main/offline_formal_start_host_check.c" `
        -o "$out/offline_formal_start_host_check.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & "$out/offline_formal_start_host_check.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $dispatchFlags=@('-DCONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE=0')
    $dispatchExe="$out/offline_audio_formal_dispatch_host_check.exe"
}
& $Gcc @dispatchFlags -std=c11 -Wall -Wextra -Werror -Wno-unused-function "-I$out" "$root/main/offline_audio_dispatch_host_check.c" -o $dispatchExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $dispatchExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$a=$spool.IndexOf('typedef struct __attribute__((packed))')
$b=$spool.IndexOf('typedef struct {', $a)
[IO.File]::WriteAllText("$out/offline_archive_types_host.inc",$spool.Substring($a,$b-$a))
$a=$spool.IndexOf('static esp_err_t commit_segment_locked(bool complete)')
$b=$spool.IndexOf('static esp_err_t append_locked(', $a)
[IO.File]::WriteAllText("$out/offline_archive_commit_host.inc",$spool.Substring($a,$b-$a))
& $Gcc -std=c11 -Wall -Wextra -Werror "-I$out" "$root/main/offline_archive_commit_host_check.c" -o "$out/offline_archive_commit_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location $out
try { & "$out/offline_archive_commit_host_check.exe" } finally { Pop-Location }
exit $LASTEXITCODE
