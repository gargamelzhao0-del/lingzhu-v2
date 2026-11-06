param(
    [string]$Port = 'COM6',
    [ValidateRange(1, 10000000)][int]$Baud = 115200,
    [string]$OutputDir = '',
    [string]$PythonExe = 'C:\Espressif\tools\python\v6.0\venv\Scripts\python.exe',
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $projectRoot 'runtime-logs\device-serial'
}
if (-not (Test-Path -LiteralPath $PythonExe -PathType Leaf)) {
    throw "ESP-IDF Python not found: $PythonExe. Use -PythonExe to specify its path."
}
$captureScript = Join-Path $PSScriptRoot 'capture-device-log.py'
$captureArgs = @('-u', $captureScript, '--port', $Port, '--baud', "$Baud", '--output-dir', $OutputDir)
if ($Quiet) { $captureArgs += '--quiet' }
& $PythonExe @captureArgs
exit $LASTEXITCODE
