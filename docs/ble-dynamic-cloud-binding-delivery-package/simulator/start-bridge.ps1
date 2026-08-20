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

python -c "import bleak, google.protobuf, cryptography" 2>$null
if ($LASTEXITCODE -ne 0) {
    python -m pip install -r (Join-Path $simulatorDir "requirements.txt")
}

python (Join-Path $simulatorDir "bridge.py") --host 127.0.0.1 --port 8765
