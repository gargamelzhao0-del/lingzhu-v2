param(
    [Parameter(Mandatory = $true)][string]$Port,
    [int]$Baud = 460800,
    [string]$BuildDir = 'C:\tmp\nuwax-sd-test-20260911',
    [switch]$VerifyOnly,
    [ValidateSet('Test','Formal')][string]$ReplayProfile = 'Test'
)

$ErrorActionPreference = 'Stop'
function Get-ImageSha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
    finally { $hasher.Dispose(); $stream.Dispose() }
}
$pythonExe = 'C:\Espressif\python_env\idf6.0_py3.13_env\Scripts\python.exe'
$buildRoot = (Resolve-Path -LiteralPath $BuildDir).Path
$manifest = Get-Content -LiteralPath (Join-Path $buildRoot 'flasher_args.json') -Raw | ConvertFrom-Json
$configHeader = Get-Content -LiteralPath (Join-Path $buildRoot 'config/sdkconfig.h') -Raw
if ($ReplayProfile -eq 'Formal') {
    & $pythonExe (Join-Path $PSScriptRoot 'verify-sd-formal-build.py') $buildRoot
    if ($LASTEXITCODE -ne 0) { throw 'Formal route verification failed; serial port was not opened.' }
} elseif ($configHeader -notmatch '#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 1') {
    throw 'This build does not enable the SD replay test gateway.'
}
$elfPath = Join-Path $buildRoot 'NuwaxAIPartner.elf'
$appPath = [IO.Path]::GetFullPath((Join-Path $buildRoot 'NuwaxAIPartner.bin'))
$elfSha = Get-ImageSha256 $elfPath
$appBytes = [IO.File]::ReadAllBytes($appPath)
if ($appBytes.Length -lt 208) { throw 'Firmware image is too short for its ELF identity.' }
$embeddedSha = ([BitConverter]::ToString($appBytes,176,32)).Replace('-','').ToLowerInvariant()
if ($embeddedSha -ne $elfSha) { throw 'Firmware BIN and ELF do not belong to the same build.' }
$resultPath = Join-Path $buildRoot 'build-result.json'
$result = $null
if (Test-Path -LiteralPath $resultPath) {
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if ($result.elfSha256 -ne $elfSha -or
        $result.binSha256 -ne (Get-ImageSha256 $appPath)) {
        throw 'Snapshot identity differs from build-result.json.'
    }
    if (@($result.images).Count -ne @($manifest.flash_files.PSObject.Properties).Count) {
        throw 'Snapshot manifest is incomplete or contains unexpected images.'
    }
}
$profile = if ($configHeader -match '#define CONFIG_ESP_COREDUMP_ENABLE_TO_UART 1') { 'diagnostic (UART core dump)' } else { 'speed' }
Write-Host "Firmware directory: $buildRoot"
Write-Host "Firmware profile: $profile; image bytes: $($appBytes.Length)"
Write-Host "Expected boot ELF SHA256: $elfSha"
if ($result) { Write-Host "Firmware revision: $($result.revision)" }
$flashArgs = @('-m', 'esptool', '--chip', 'esp32s3', '--port', $Port,
    '--baud', [string]$Baud, '--before', 'default-reset', '--after', 'hard-reset', 'write-flash')
$flashArgs += @($manifest.write_flash_args)
$appInManifest = $false
foreach ($entry in $manifest.flash_files.PSObject.Properties) {
    $imagePath = [IO.Path]::GetFullPath((Join-Path $buildRoot ([string]$entry.Value)))
    if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
        throw "Missing flash image: $imagePath"
    }
    if ($imagePath -eq $appPath) { $appInManifest = $true }
    if ($result) {
        $record = @($result.images | Where-Object { $_.offset -eq $entry.Name -and $_.file -eq [string]$entry.Value })
        if ($record.Count -ne 1 -or $record[0].sha256 -ne (Get-ImageSha256 $imagePath)) {
            throw "Snapshot image checksum mismatch: $imagePath"
        }
    }
    $flashArgs += @($entry.Name, $imagePath)
}
if (-not $appInManifest) { throw 'Manifest does not contain the verified application image.' }
if ($VerifyOnly) {
    Write-Host 'PASS: firmware identity and all manifest images verified; serial port was not opened.'
    exit 0
}
# Flash all manifest images, including OTA selection, models and emotion assets.
# No erase-flash command is issued; NVS and the SD card are not erased.
& $pythonExe @flashArgs
exit $LASTEXITCODE
