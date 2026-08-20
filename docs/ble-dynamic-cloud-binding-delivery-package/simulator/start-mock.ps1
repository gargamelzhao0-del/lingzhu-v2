$ErrorActionPreference = "Stop"
$simulatorDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$packageDir = Split-Path -Parent $simulatorDir
$contract = Join-Path $packageDir "provisioning-contract.json"

if (-not (Test-Path -LiteralPath $contract)) {
    throw "Missing provisioning-contract.json in the delivery package."
}

Write-Host "Mock provisioning simulator: http://127.0.0.1:8765/simulator/"
Write-Host "Press Ctrl+C to stop."
python -m http.server 8765 --bind 127.0.0.1 --directory $packageDir
