param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content "$root/main/nuwax_ai_partner_main.c" -Raw -Encoding UTF8
$a=$source.IndexOf('/* Single-slot mailbox:')
$b=$source.IndexOf('static void drain_wifi_status_reports(', $a)
if($a -lt 0 -or $b -le $a){throw 'Production mailbox not found'}
[IO.File]::WriteAllText("$out/wifi_background_recovery_host.inc",$source.Substring($a,$b-$a))
$boot=$source.Substring($source.IndexOf('void app_main('))
if($boot.Contains('nuwax_wifi_connect();') -or $boot.Contains('nuwax_wifi_recover();')){throw 'Blocking Wi-Fi call on boot/main'}
& $Gcc -std=c11 -Wall -Wextra -Werror "-I$out" "-I$root/main/audio_modes/meeting/host_stubs" "$root/main/wifi_background_recovery_host_check.c" -o "$out/wifi_background_recovery_host_check.exe"
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& "$out/wifi_background_recovery_host_check.exe"
exit $LASTEXITCODE
