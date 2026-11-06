param(
    [string]$RepoRoot = '',
    [string]$Compiler = 'gcc'
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $searchPath = $PSScriptRoot
    while (-not (Test-Path -LiteralPath (Join-Path $searchPath 'main/nuwax_provision_new/nuwax_cloud_config.c'))) {
        $parentPath = Split-Path -Parent $searchPath
        if ([string]::IsNullOrWhiteSpace($parentPath) -or $parentPath -eq $searchPath) {
            throw 'Cannot locate production source; pass -RepoRoot explicitly.'
        }
        $searchPath = $parentPath
    }
    $RepoRoot = $searchPath
}
$rootPath = (Resolve-Path -LiteralPath $RepoRoot).Path
$artifactPath = $PSScriptRoot
$buildPath = Join-Path ([IO.Path]::GetTempPath()) ('nuwax-cloud-config-recheck-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $buildPath)
$allOutput = [Collections.Generic.List[string]]::new()
foreach ($mode in @(0, 1)) {
    $binaryPath = Join-Path $buildPath ('cloud-config-' + $mode + '.exe')
    & $Compiler '-std=c11' '-Wall' '-Wextra' '-Werror' "-DCONFIG_NUWAX_CLOUD_CONFIG_ALLOW_INSECURE_WS=$mode" "-I$artifactPath/stubs" "-I$rootPath/main/nuwax_provision_new" "$artifactPath/cloud_config_recheck.c" '-o' $binaryPath
    if ($LASTEXITCODE -ne 0) { throw "compile failed for mode $mode" }
    foreach ($scenario in @('legacy_ws', 'existing_wss')) {
        $output = & $binaryPath $scenario
        if ($LASTEXITCODE -ne 0) { throw "test failed for mode $mode scenario $scenario" }
        foreach ($line in $output) { $allOutput.Add($line); Write-Output $line }
    }
}
$allOutput | Set-Content -LiteralPath (Join-Path $artifactPath 'cloud-config-recheck.log') -Encoding UTF8
Write-Output "Only synthetic in-memory NVS was used. Executables: $buildPath"
