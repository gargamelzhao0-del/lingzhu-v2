param([string]$BuildDir='C:/tmp/nuwax-sd-session-formal-20261027-r188',
      [ValidatePattern('^7\.0\.[0-9]+$')][string]$Version='7.0.188',
      [ValidateSet(16,32)][int]$WifiCacheTxBuffers=32,
      [ValidateSet('cpu0','unbound')][string]$TcpipAffinity='cpu0')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    # Reused sdkconfig values override defaults. Clear a stale LAN route on
    # every build while retaining the current product's unrelated settings.
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'prepare-sd-formal-config.py') `
        (Join-Path $root 'tmp/sdkconfig.sd-session-formal') `
        --seed (Join-Path $root 'tmp/sdkconfig.sd-session-test') `
        --wifi-cache $WifiCacheTxBuffers --tcpip-affinity $TcpipAffinity
    if ($LASTEXITCODE -ne 0) { throw 'Formal SD configuration failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'apply-network-provision-reset-fix.py')
    if ($LASTEXITCODE -ne 0) { throw 'Provisioning dependency repair failed.' }
    . 'C:/esp/v6.0/esp-idf/export.ps1'
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        C:/esp/v6.0/esp-idf/tools/idf.py -B $BuildDir `
        -D "PROJECT_VER=$Version" `
        -D SDKCONFIG=tmp/sdkconfig.sd-session-formal `
        -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.product-ota.defaults;sdkconfig.sd-replay-formal.defaults' build
    if ($LASTEXITCODE -ne 0) { throw 'Formal firmware build failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-sd-formal-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Formal firmware verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-sd-replay-scheduling-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'SD replay scheduling verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-board-power-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Board power integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-startup-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Startup integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-interaction-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Interaction integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-audio-owner-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Audio ownership integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-network-recovery-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Network recovery integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-provision-finish-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Provisioning finish integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-cloud-resume-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Cloud resume integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-link-convergence-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Link convergence integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-shake-gap-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Shake gap integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-playback-startup-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Playback startup integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-runtime-hardening-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Runtime hardening integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-wake-evidence-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Wake evidence integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-jitter-stability-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Jitter stability integration verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-rejected-playback-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Rejected playback verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-uplink-handoff-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Uplink handoff verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-sd-media-admission-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'SD media admission verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-uplink-reaper-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Uplink reaper budget verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-json-resource-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'JSON resource budget verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-ota-identity-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'OTA boot identity verification failed.' }
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        (Join-Path $PSScriptRoot 'verify-speaker-pa-build.py') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'Speaker PA sequencing verification failed.' }
} finally { Pop-Location }
