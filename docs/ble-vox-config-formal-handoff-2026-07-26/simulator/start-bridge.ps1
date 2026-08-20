param(
    [ValidateRange(1, 65535)]
    [int]$Port = 8765
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

python -c "import bleak, google.protobuf, cryptography, serial" 2>$null
if ($LASTEXITCODE -ne 0) {
    python -m pip install -r (Join-Path $simulatorDir "requirements.txt")
}

if (Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue) {
    throw "Simulator port $Port is already in use. Choose another value with -Port."
}

Write-Host "Provisioning simulator: http://127.0.0.1:$Port/simulator/"
python (Join-Path $simulatorDir "bridge.py") --host 127.0.0.1 --port $Port
