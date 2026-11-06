param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$s=Get-Content "$root/main/nuwax_ai_partner_main.c" -Raw -Encoding UTF8
$a=$s.IndexOf('static bool reconcile_cloud_ready_sleep_state(const char *reason) {')
$b=$s.IndexOf('static void schedule_volume_report(', $a)
if($a -lt 0 -or $b -le $a){throw 'Missing production convergence function'}
[IO.File]::WriteAllText("$out/cloud_ready_sleep_host.inc",$s.Substring($a,$b-$a))
$a=$s.IndexOf('static esp_err_t wait_for_cloud_service_ready(nuwax_ws_client_t *ws) {')
$b=$s.IndexOf('static void on_ws_rx_activity(', $a)
if(!$s.Substring($a,$b-$a).Contains('reconcile_cloud_ready_sleep_state("cloud_ready_received")')) {throw 'Missing ready-time convergence'}
& $Gcc -std=c11 -Wall -Wextra -Werror "-I$out" "$root/main/cloud_ready_sleep_host_check.c" -o "$out/cloud_ready_sleep_host_check.exe"
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& "$out/cloud_ready_sleep_host_check.exe"
exit $LASTEXITCODE
