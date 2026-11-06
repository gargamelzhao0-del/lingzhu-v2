param([string]$Gcc = 'gcc')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source = Get-Content "$root/main/touch_input.c" -Raw
$start = $source.IndexOf('static void touch_log_task(')
$end = $source.IndexOf('esp_err_t nuwax_touch_init(', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Production touch task not found' }
[IO.File]::WriteAllText("$out/touch_task_host.inc", $source.Substring($start, $end-$start))
foreach ($test in @('touch_read_policy', 'touch_task')) {
    $exe = Join-Path $out "$($test)_host_check.exe"
    & $Gcc -std=c11 -Wall -Wextra -Werror "-I$out" "-I$root/main" `
        "-I$root/main/session_protocol_host_stubs" "$root/main/$($test)_host_check.c" -o $exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
