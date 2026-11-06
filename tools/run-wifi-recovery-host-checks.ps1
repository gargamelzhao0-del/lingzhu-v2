param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content "$root/main/wifi_manager.c" -Raw
$a=$source.IndexOf('static esp_err_t wifi_driver_connect_once(')
$b=$source.IndexOf('/* Exclude missing AP', $a)
$code=$source.Substring($a,$b-$a)
$a=$source.IndexOf('/* Protected by s_wifi_operation_mutex. Remember driver normalization')
$b=$source.IndexOf('static void wifi_make_station_config(', $a)
$code+=$source.Substring($a,$b-$a)
$a=$source.IndexOf('esp_err_t nuwax_wifi_recover(void)')
$b=$source.IndexOf('bool nuwax_wifi_simulated_offline(', $a)
$code+=$source.Substring($a,$b-$a)
[IO.File]::WriteAllText("$out/wifi_recovery_host.inc",$code)
& $Gcc -std=c11 -Wall -Wextra -Werror "-I$out" "-I$root/main/audio_modes/meeting/host_stubs" "$root/main/wifi_recovery_host_check.c" -o "$out/wifi_recovery_host_check.exe"
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& "$out/wifi_recovery_host_check.exe"
exit $LASTEXITCODE
