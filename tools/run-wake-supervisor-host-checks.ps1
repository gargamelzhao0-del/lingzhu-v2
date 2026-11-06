param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content "$root/main/nuwax_ai_partner_main.c" -Raw
$a=$source.IndexOf('static void wake_word_supervisor_task(void *arg) {')
$b=$source.IndexOf('static void remember_sleep_request(', $a)
if ($a -lt 0 -or $b -le $a) { throw 'Production wake supervisor not found' }
[IO.File]::WriteAllText("$out/wake_supervisor_host.inc",$source.Substring($a,$b-$a))
& $Gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-variable "-I$out" "$root/main/wake_supervisor_host_check.c" -o "$out/wake_supervisor_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/wake_supervisor_host_check.exe"
exit $LASTEXITCODE
