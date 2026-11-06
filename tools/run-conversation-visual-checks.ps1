param([string]$Gcc='C:/Users/Administrator/scoop/apps/gcc/current/bin/gcc.exe')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$build=Join-Path $PSScriptRoot 'ui-speaking-capture/conversation-v1'
Push-Location $root
try {
    & (Join-Path $PSScriptRoot 'ui-speaking-capture/build-capture.ps1') -BuildDir conversation-v1 -Gcc $Gcc
    if ($LASTEXITCODE -ne 0) { throw 'Native conversation visual build failed' }
    $savedPath=$env:PATH
    $env:PATH=(Split-Path -Parent $Gcc)+';'+$env:PATH
    Push-Location $build
    try {
        & './nuwax_speaking_capture.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Native conversation visual checks failed' }
    } finally { Pop-Location; $env:PATH=$savedPath }
    & $Gcc -std=c11 -Wall -Wextra -Werror -Imain/ui_host_stubs -Imain -Imain/nuwax_ui_v28 `
        main/nuwax_ui_v28/nuwax_face.c main/emotion_face_policy_host_check.c `
        -o (Join-Path $build 'face-policy.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Face policy build failed' }
    & (Join-Path $build 'face-policy.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Face policy checks failed' }
} finally { Pop-Location }
