param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
# Compile the real driver with bounded fake host/VFS operations.
$source=Get-Content "$root/main/board/sd_card_driver.c" -Raw
$source=[regex]::Replace($source,'(?m)^#include[^\r\n]*','')
[IO.File]::WriteAllText("$out/sd_speed_driver_host.inc",$source)
foreach ($enabled in @(0,1)) {
    $exe="$out/sd_speed_host_$enabled.exe"
    & $Gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function "-DCONFIG_NUWAX_SD_CARD_HIGHSPEED_ENABLE=$enabled" "-I$out" "-I$root/main" "$root/main/sd_speed_host_check.c" -o $exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
