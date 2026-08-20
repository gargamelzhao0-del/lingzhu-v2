param(
    [string]$LanAddress = $env:NUWAX_TEST_GATEWAY_LAN_ADDRESS
)

$ErrorActionPreference = "Stop"
$simulatorDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$candidate = Get-Item -LiteralPath $simulatorDir
$repoRoot = $null
while ($candidate) {
    $contract = Join-Path $candidate.FullName "main\nuwax_provision_new\esp32s3-idf6-provisioning-contract.json"
    if (Test-Path -LiteralPath $contract) {
        $repoRoot = $candidate.FullName
        break
    }
    $candidate = $candidate.Parent
}
if (-not $repoRoot) {
    throw "Cannot locate the NuwaxAIPartner project root."
}

$projectDescription = Join-Path $repoRoot "build\project_description.json"
if (-not (Test-Path -LiteralPath $projectDescription)) {
    throw "Missing build/project_description.json; build the ESP-IDF project first."
}
$project = Get-Content -LiteralPath $projectDescription -Raw -Encoding UTF8 | ConvertFrom-Json
$env:IDF_PATH = $project.idf_path

$gatewayDir = Join-Path (Split-Path -Parent $repoRoot) "cloud-gateway"
$gatewayScript = Join-Path $gatewayDir "cloud-gateway.js"
$signingKey = Join-Path $gatewayDir "test-device-config-signing-private-key.pem"
if (-not (Test-Path -LiteralPath $gatewayScript) -or -not (Test-Path -LiteralPath $signingKey)) {
    throw "Test gateway or signing key is missing."
}

$lanCandidates = Get-NetIPAddress -AddressFamily IPv4 -AddressState Preferred -ErrorAction SilentlyContinue |
    Where-Object {
        $_.IPAddress -ne "127.0.0.1" -and
        $_.IPAddress -notlike "169.254.*" -and
        $_.InterfaceAlias -notmatch "Loopback|vEthernet|WSL|Docker|VMware|VirtualBox"
    }
if ($LanAddress) {
    $lanAddressRecord = $lanCandidates |
        Where-Object { $_.IPAddress -eq $LanAddress } |
        Select-Object -First 1
    if (-not $lanAddressRecord) {
        throw "Requested gateway LAN address is not active: $LanAddress"
    }
} else {
    $lanAddressRecord = $lanCandidates |
        Sort-Object `
            @{ Expression = { if ($_.InterfaceAlias -match 'WLAN|Wi-Fi|Wireless') { 0 } else { 1 } } },
            @{ Expression = { if ($_.IPAddress -match '^(192\.168\.|10\.|172\.(1[6-9]|2[0-9]|3[01])\.)') { 0 } else { 1 } } } |
        Select-Object -First 1
}
if (-not $lanAddressRecord) {
    throw "No usable LAN IPv4 address was found."
}

$gatewayPort = 8799..8810 | Where-Object {
    -not (Get-NetTCPConnection -State Listen -LocalPort $_ -ErrorAction SilentlyContinue)
} | Select-Object -First 1
if (-not $gatewayPort) {
    throw "No free test gateway port is available in 8799..8810."
}
$claimToken = [guid]::NewGuid().ToString("N") + [guid]::NewGuid().ToString("N")
$adminToken = [guid]::NewGuid().ToString("N") + [guid]::NewGuid().ToString("N")
$env:CLOUD_GATEWAY_HOST = "0.0.0.0"
$env:CLOUD_GATEWAY_PORT = "$gatewayPort"
$env:CLOUD_DEVICE_ONBOARDING_ENABLE = "1"
$env:CLOUD_DEVICE_ONBOARDING_ALLOW_INSECURE_WS = "1"
$env:CLOUD_DEVICE_PUBLIC_WS_URL = "ws://$($lanAddressRecord.IPAddress):$gatewayPort/api/device/ws"
$env:CLOUD_DEVICE_REGISTRY_FILE = Join-Path $gatewayDir "runtime\test-device-registry.json"
$env:CLOUD_DEVICE_SIGNING_KEY_FILE = $signingKey
$env:CLOUD_APP_CLAIM_TOKEN = $claimToken
$env:CLOUD_GATEWAY_TOKEN = $adminToken
$env:NUWAX_TEST_GATEWAY_HTTP_URL = "http://$($lanAddressRecord.IPAddress):$gatewayPort"
$env:NUWAX_TEST_GATEWAY_CLAIM_TOKEN = $claimToken

$logDir = Join-Path $simulatorDir "runtime-logs"
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$gatewayStdout = Join-Path $logDir "gateway-$timestamp.stdout.log"
$gatewayStderr = Join-Path $logDir "gateway-$timestamp.stderr.log"
$bridgeStdout = Join-Path $logDir "bridge-$timestamp.stdout.log"
$bridgeStderr = Join-Path $logDir "bridge-$timestamp.stderr.log"

python -c "import bleak, google.protobuf, cryptography" 2>$null
if ($LASTEXITCODE -ne 0) {
    python -m pip install -r (Join-Path $simulatorDir "requirements.txt")
}

$gateway = Start-Process -FilePath "node" -ArgumentList @("cloud-gateway.js") `
    -WorkingDirectory $gatewayDir -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $gatewayStdout -RedirectStandardError $gatewayStderr
try {
    $deadline = (Get-Date).AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 200
        try {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:$gatewayPort/api/status" -TimeoutSec 1
        } catch {
            $health = $null
        }
    } while (-not $health -and (Get-Date) -lt $deadline -and -not $gateway.HasExited)
    if (-not $health) {
        throw "Test gateway failed to start."
    }
    Write-Host "Test gateway for device: $env:CLOUD_DEVICE_PUBLIC_WS_URL"
    Write-Host "Provisioning simulator: http://127.0.0.1:8765/simulator/"
    Write-Host "Runtime logs: $logDir"
    $bridgeScript = Join-Path $simulatorDir "bridge.py"
    $bridgeArgs = "`"$bridgeScript`" --host 127.0.0.1 --port 8765"
    $bridge = Start-Process -FilePath "python" `
        -ArgumentList $bridgeArgs `
        -WorkingDirectory $simulatorDir -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $bridgeStdout -RedirectStandardError $bridgeStderr
    $bridge.WaitForExit()
    $bridge.Refresh()
    if ($bridge.ExitCode -ne 0) {
        throw "Provisioning bridge exited with code $($bridge.ExitCode); see $bridgeStderr"
    }
} finally {
    if ($gateway -and -not $gateway.HasExited) {
        Stop-Process -Id $gateway.Id -Force
    }
}
