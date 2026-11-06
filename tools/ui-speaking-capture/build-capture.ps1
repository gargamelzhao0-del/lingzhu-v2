param(
    [string]$BuildDir = "build",
    [string]$Gcc = "C:\Users\Administrator\scoop\apps\gcc\current\bin\gcc.exe"
)

$ErrorActionPreference = "Stop"
$CaptureDir = $PSScriptRoot
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $CaptureDir "..\.."))
$WorkspaceRoot = Split-Path $ProjectRoot -Parent
$SupportDir = Join-Path $WorkspaceRoot "tools\lvgl-simulator"
$LvglDir = Join-Path $ProjectRoot "lib\lvgl"
$OutDir = Join-Path $CaptureDir $BuildDir
$ObjDir = Join-Path $OutDir "obj"
$Exe = Join-Path $OutDir "nuwax_speaking_capture.exe"

if (!(Test-Path -LiteralPath $Gcc)) { throw "GCC not found: $Gcc" }
New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

$lvglSources = Get-ChildItem -LiteralPath (Join-Path $LvglDir "src") -Recurse -File -Filter "*.c" |
    Where-Object {
        $_.FullName -notmatch '\\src\\libs\\thorvg\\' -and
        $_.FullName -notmatch '\\src\\others\\vg_lite_tvg\\'
    } | Select-Object -ExpandProperty FullName

$sources = @(
    (Join-Path $CaptureDir "capture_main.c"),
    (Join-Path $CaptureDir "sim_stubs.c"),
    (Join-Path $ProjectRoot "main\nuwax_ui_v28\face\conversation_visual.c"),
    (Join-Path $ProjectRoot "main\nuwax_ui_v28\ota_progress.c"),
    (Join-Path $ProjectRoot "main\nuwax_ui_v28\low_battery_warning.c"),
    (Join-Path $ProjectRoot "main\nuwax_ui_v28\assets\low_battery_assets.c"),
    (Join-Path $ProjectRoot "main\nuwax_ui_v28\assets\ota_logo.c"),
    (Join-Path $ProjectRoot "main\nuwax_ui_v28\assets\ota_capsule.c"),
    (Join-Path $ProjectRoot "main\nuwax_ui_v28\assets\fonts\nuwax_font_ota.c"),
    (Join-Path $ProjectRoot "main\nuwax_ui_v28\nuwax_face.c")
) + $lvglSources

$commonArgs = @(
    "-std=gnu11", "-O2", "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_KCONFIG_IGNORE",
    "-D_WIN32_WINNT=0x0601", "-DWINVER=0x0601",
    "-I$CaptureDir", "-I$SupportDir", "-I$LvglDir",
    "-I$(Join-Path $LvglDir 'src')", "-I$(Join-Path $ProjectRoot 'main')",
    "-I$(Join-Path $ProjectRoot 'main\nuwax_ui_v28')",
    "-I$(Join-Path $ProjectRoot 'main\nuwax_ui_v28\face')"
)

function Path-Token([string]$Path) {
    return ([IO.Path]::GetFullPath($Path) -replace '[^A-Za-z0-9_.-]', '_')
}

$objects = [System.Collections.Generic.List[string]]::new()
$headerStamp = (Get-ChildItem -LiteralPath $LvglDir,$CaptureDir,$SupportDir -Recurse -File -Filter '*.h' |
    Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
$faceHeaderStamp = (Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'main\nuwax_ui_v28') -Recurse -File -Filter '*.h' |
    Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
foreach ($source in $sources) {
    $dependencyStamp=$headerStamp
    if (-not $source.StartsWith($LvglDir,[StringComparison]::OrdinalIgnoreCase) -and $faceHeaderStamp -gt $dependencyStamp) {
        $dependencyStamp=$faceHeaderStamp
    }
    $object = Join-Path $ObjDir ((Path-Token $source) + ".obj")
    $objects.Add($object)
    if ((Test-Path -LiteralPath $object) -and
        ((Get-Item -LiteralPath $object).LastWriteTimeUtc -ge
         (Get-Item -LiteralPath $source).LastWriteTimeUtc) -and
        ((Get-Item -LiteralPath $object).LastWriteTimeUtc -ge $dependencyStamp)) {
        continue
    }
    $compileArgs=@($commonArgs)
    & $Gcc @compileArgs -c $source -o $object
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $source" }
}

$response = Join-Path $OutDir "objects.rsp"
$lines = @($objects | ForEach-Object { '"' + $_ + '"' }) +
    @("-luser32", "-lgdi32", "-lshcore", "-lwinmm")
[IO.File]::WriteAllLines($response, $lines, [Text.UTF8Encoding]::new($false))
& $Gcc -o $Exe "@$response"
if ($LASTEXITCODE -ne 0) { throw "link failed" }
Write-Output $Exe
