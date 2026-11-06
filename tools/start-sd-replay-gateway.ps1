param(
    [string]$HostAddress,
    [string]$ListenAddress = '0.0.0.0',
    [int]$Port = 8810,
    [string]$StorageDir,
    [string]$RunDir,
    [switch]$Stop
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$nodeExe = (Get-Command node.exe -ErrorAction Stop).Source
$settings = & $nodeExe (Join-Path $PSScriptRoot 'protocol/sd-test-gateway-config.js')
if ($LASTEXITCODE -ne 0) { throw 'Cannot read SD test gateway settings.' }
$settings = $settings | ConvertFrom-Json
if (-not $StorageDir) { $StorageDir = $settings.storageDir }
if (-not $RunDir) { $RunDir = $settings.runDir }
if (-not $HostAddress) { $HostAddress = @($settings.addresses)[0].address }
if (-not $HostAddress) { $HostAddress = '127.0.0.1' }
if ($Port -ne $settings.port) { throw 'Discovery and WebSocket ports must match the paired gateway settings.' }
$RunDir = [IO.Path]::GetFullPath($RunDir)
$StorageDir = [IO.Path]::GetFullPath($StorageDir)
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
$stopFile = Join-Path $RunDir 'stop.request'
if ($Stop) {
    Set-Content -LiteralPath $stopFile -Value 'stop' -Encoding ascii
    Write-Output "Stop requested: $RunDir"
    exit 0
}
$supervisorScript = Join-Path $PSScriptRoot 'protocol/sd-replay-supervisor.js'
$existing = Get-CimInstance Win32_Process -Filter "name='node.exe'" | Where-Object {
    $_.CommandLine -and $_.CommandLine.Contains($supervisorScript) -and $_.CommandLine.Contains($RunDir)
}
if ($existing) { Write-Output "Gateway supervisor already running: $($existing.ProcessId)"; exit 0 }
if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
    throw "Port $Port already has a listener; leaving it untouched."
}
if (Test-Path -LiteralPath $stopFile) { Remove-Item -LiteralPath $stopFile }
# Open only this test receiver and discovery ports on the local subnet.
foreach ($entry in @(@('Nuwax-SD-Test-TCP-8810','TCP',8810), @('Nuwax-SD-Test-UDP-8811','UDP',8811))) {
    if (-not (Get-NetFirewallRule -Name $entry[0] -ErrorAction SilentlyContinue)) {
        try {
            New-NetFirewallRule -Name $entry[0] -DisplayName $entry[0] -Direction Inbound -Action Allow `
                -Program $nodeExe -Protocol $entry[1] -LocalPort $entry[2] -RemoteAddress LocalSubnet `
                -Profile Private,Public | Out-Null
        } catch {
            # Existing Node allow rules can already cover these ports. Lack of
            # elevation must not prevent the receiver from starting at all.
            Write-Warning "Cannot add $($entry[0]): $($_.Exception.Message). Existing firewall rules remain in effect; run this launcher as administrator before starting if LAN access is blocked."
        }
    }
}
# Windows paths cannot contain quotes. Quote each complete argument so spaces
# in the project and user directory survive Start-Process command-line joining.
$launchArgs = @($supervisorScript, $RunDir, $StorageDir, $ListenAddress, [string]$Port, $HostAddress, $settings.configPath) |
    ForEach-Object { '"' + $_ + '"' }
$process = Start-Process -FilePath $nodeExe -ArgumentList $launchArgs -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $RunDir 'supervisor.log') `
    -RedirectStandardError (Join-Path $RunDir 'supervisor-error.log')
Write-Output "Gateway supervisor started: pid=$($process.Id) listen=${ListenAddress}:$Port endpoint=ws://${HostAddress}:$Port/api/device/ws"
Write-Output "Audio dashboard: http://${HostAddress}:$Port/ (local: http://127.0.0.1:$Port/)"
Write-Output "Paired LAN discovery: UDP $($settings.discoveryPort); gateway=$($settings.gatewayId)"
Write-Output "Archives: $StorageDir"
Write-Output "Logs: $RunDir"
