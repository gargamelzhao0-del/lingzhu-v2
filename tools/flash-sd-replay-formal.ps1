param(
    [Parameter(Mandatory=$true)][string]$Port,
    [int]$Baud=460800,
    [string]$BuildDir='C:/tmp/nuwax-sd-session-formal-20261026-r179',
    [switch]$VerifyOnly
)
$ErrorActionPreference='Stop'
& (Join-Path $PSScriptRoot 'flash-sd-replay-test.ps1') -Port $Port -Baud $Baud `
    -BuildDir $BuildDir -ReplayProfile Formal -VerifyOnly:$VerifyOnly
exit $LASTEXITCODE
