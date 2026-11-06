param(
    [Parameter(Mandatory = $true)][string]$Port,
    [int]$Baud = 0,
    [string]$BuildDir = 'C:\tmp\nuwax-sd-test-20260911'
)

$ErrorActionPreference = 'Stop'
$pythonExe = 'C:\Espressif\python_env\idf6.0_py3.13_env\Scripts\python.exe'
$toolchain = 'C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin'
$buildRoot = (Resolve-Path -LiteralPath $BuildDir).Path
$elfPath = Join-Path $buildRoot 'NuwaxAIPartner.elf'
if (-not (Test-Path -LiteralPath $elfPath -PathType Leaf)) { throw "Missing matching ELF: $elfPath" }
if ($Baud -le 0) {
    $configHeader = Get-Content -LiteralPath (Join-Path $buildRoot 'config/sdkconfig.h') -Raw
    $baudMatch = [regex]::Match($configHeader, '(?m)^#define CONFIG_ESP_CONSOLE_UART_BAUDRATE (\d+)')
    if (-not $baudMatch.Success) { throw 'Console baud rate not found; specify -Baud explicitly.' }
    $Baud = [int]$baudMatch.Groups[1].Value
}
Write-Host "Monitor ELF: $elfPath; baud: $Baud"
$stream = [IO.File]::OpenRead($elfPath)
$hasher = [Security.Cryptography.SHA256]::Create()
try { $elfSha = ([BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
finally { $hasher.Dispose(); $stream.Dispose() }
Write-Host "Expected boot ELF SHA256: $elfSha"
if (-not (Test-Path -LiteralPath (Join-Path $toolchain 'xtensa-esp32s3-elf-addr2line.exe'))) {
    throw "Missing toolchain: $toolchain"
}
$previousPath = $env:PATH
$previousIdf = $env:IDF_PATH
try {
    $env:PATH = "$toolchain;" + (Split-Path -Parent $pythonExe) + ";$previousPath"
    $env:IDF_PATH = 'C:\esp\v6.0\esp-idf'
    # Keep UART core dump blocks intact in the serial log for offline analysis.
    # --no-reset lets logging attach to a long-running test without restarting it.
    & $pythonExe -m esp_idf_monitor --port $Port --baud $Baud --no-reset `
        --target esp32s3 --toolchain-prefix xtensa-esp32s3-elf- `
        --decode-coredumps disable --timestamps $elfPath
    $monitorCode = $LASTEXITCODE
} finally {
    $env:PATH = $previousPath
    $env:IDF_PATH = $previousIdf
}
exit $monitorCode
