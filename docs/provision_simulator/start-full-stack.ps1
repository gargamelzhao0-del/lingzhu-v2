param(
    [string]$LanAddress = $env:NUWAX_TEST_GATEWAY_LAN_ADDRESS,
    [ValidateRange(1, 65535)]
    [int]$SimulatorPort = 8765
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
if ($repoRoot) {
    $projectDescription = Join-Path $repoRoot "build\project_description.json"
    if (-not (Test-Path -LiteralPath $projectDescription)) {
        throw "Missing build/project_description.json; build the ESP-IDF project first."
    }
    $project = Get-Content -LiteralPath $projectDescription -Raw -Encoding UTF8 | ConvertFrom-Json
    $env:IDF_PATH = $project.idf_path
} elseif (-not $env:IDF_PATH -or
          -not (Test-Path -LiteralPath (Join-Path $env:IDF_PATH "tools\idf.py"))) {
    throw "Standalone package mode requires IDF_PATH to point to ESP-IDF v6.0."
}

$packagedGatewayDir = Join-Path (Split-Path -Parent $simulatorDir) "test-gateway"
$gatewayDir = if (Test-Path -LiteralPath (Join-Path $packagedGatewayDir "cloud-gateway.js")) {
    $packagedGatewayDir
} elseif ($repoRoot) {
    Join-Path (Split-Path -Parent $repoRoot) "cloud-gateway"
} else {
    $null
}
$gatewayScript = if ($gatewayDir) { Join-Path $gatewayDir "cloud-gateway.js" } else { $null }
if (-not $gatewayDir -or -not (Test-Path -LiteralPath $gatewayScript)) {
    throw "Test gateway is missing."
}

$deviceSecret = $env:NUWAX_TEST_DEVICE_SECRET
if (-not $deviceSecret) {
    $secureSecret = Read-Host "Device Secret (also enter this value in the simulator)" -AsSecureString
    $deviceSecret = [System.Net.NetworkCredential]::new("", $secureSecret).Password
}
$secretBytes = [System.Text.Encoding]::UTF8.GetBytes($deviceSecret)
if (-not $deviceSecret -or $secretBytes.Length -gt 95 -or $deviceSecret -notmatch '^[\x21-\x7E]+$') {
    throw "Device Secret must be 1..95 printable ASCII characters."
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
if (Get-NetTCPConnection -State Listen -LocalPort $SimulatorPort -ErrorAction SilentlyContinue) {
    throw "Simulator port $SimulatorPort is already in use. Choose another value with -SimulatorPort."
}
$env:CLOUD_GATEWAY_HOST = "0.0.0.0"
$env:CLOUD_GATEWAY_PORT = "$gatewayPort"
$env:CLOUD_DEVICE_ONBOARDING_ENABLE = "0"
$env:CLOUD_GATEWAY_TOKEN = $deviceSecret
$env:CLOUD_CHAT_PROVIDER = "mock"
$env:CLOUD_ASR_ENABLE = "true"
$env:CLOUD_ASR_PROVIDER = "mock"
$env:CLOUD_ASR_AUTO_RELAY = "true"
$env:VOLC_ASR_STREAMING_MODE = "bidirectional"
$env:CLOUD_CONVERSATION_MODE = "streaming"
$env:CLOUD_CONVERSATION_ENDPOINT_ENABLE = "true"
$env:CLOUD_CONVERSATION_ENDPOINT_SPEECH_RMS = "80"
$env:CLOUD_CONVERSATION_ENDPOINT_MIN_SPEECH_MS = "120"
$env:CLOUD_CONVERSATION_ENDPOINT_SILENCE_MS = "500"
$env:CLOUD_TTS_ENABLE = "true"
$env:CLOUD_TTS_PROVIDER = "mock"
$env:CLOUD_TTS_MODE = "streaming"
$env:CLOUD_TTS_AUTO_PLAYBACK = "true"
$env:NUWAX_TEST_GATEWAY_HTTP_URL = "http://$($lanAddressRecord.IPAddress):$gatewayPort"
$env:NUWAX_TEST_GATEWAY_TOKEN = $deviceSecret
$env:NUWAX_TEST_GATEWAY_WS_URL = "ws://$($lanAddressRecord.IPAddress):$gatewayPort/api/device/ws"

$logDir = Join-Path $simulatorDir "runtime-logs"
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$gatewayStdout = Join-Path $logDir "gateway-$timestamp.stdout.log"
$gatewayStderr = Join-Path $logDir "gateway-$timestamp.stderr.log"
$bridgeStdout = Join-Path $logDir "bridge-$timestamp.stdout.log"
$bridgeStderr = Join-Path $logDir "bridge-$timestamp.stderr.log"

python -c "import bleak, google.protobuf, cryptography, serial" 2>$null
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
    if ($health.gateway.device_auth_protocol -ne "x-bridge-token+hello.device_secret" -or
        $health.gateway.cloud_config_protocol -ne "vox-config-url-device-secret-v1" -or
        $health.gateway.signed_bundle_required -ne $false) {
        throw "Test gateway protocol does not match the current formal URL + deviceSecret protocol."
    }
    if ($health.gateway.asr.provider -ne "mock" -or
        $health.gateway.asr.streaming_enabled -ne $true -or
        $health.gateway.asr.auto_relay -ne $true -or
        $health.gateway.asr.conversation_endpoint.enabled -ne $true -or
        $health.gateway.tts.provider -ne "mock" -or
        $health.gateway.tts.auto_playback -ne $true) {
        throw "Test gateway conversation loop is incomplete; streaming ASR, auto relay, endpointing and TTS playback are required."
    }
    Write-Host "Test gateway for device: $env:NUWAX_TEST_GATEWAY_WS_URL"
    Write-Host "Provisioning simulator: http://127.0.0.1:$SimulatorPort/simulator/"
    Write-Host "Enter the same Device Secret in the simulator; it is not printed or persisted."
    Write-Host "Runtime logs: $logDir"
    $bridgeScript = Join-Path $simulatorDir "bridge.py"
    $bridgeArgs = "`"$bridgeScript`" --host 127.0.0.1 --port $SimulatorPort"
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
