param([string]$BuildDir = '',
      [ValidateSet(16,32)][int]$WifiCacheTxBuffers = 32,
      [ValidateSet('cpu0','unbound')][string]$TcpipAffinity = 'cpu0',
      [switch]$UseTestGateway)
$ErrorActionPreference = 'Stop'
if (-not $UseTestGateway) {
    if (-not $BuildDir) { $BuildDir = 'C:/tmp/nuwax-sd-session-formal-20261023' }
    Write-Host 'SD uploads now default to the formal cloud; the PC gateway requires -UseTestGateway.'
    & (Join-Path $PSScriptRoot 'build-sd-replay-formal.ps1') -BuildDir $BuildDir `
        -WifiCacheTxBuffers $WifiCacheTxBuffers -TcpipAffinity $TcpipAffinity
    exit $LASTEXITCODE
}
if (-not $BuildDir) { $BuildDir = 'C:/tmp/nuwax-sd-session-test-20261013' }
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    & node (Join-Path $PSScriptRoot 'protocol/sd-test-gateway-config.js') --prepare-build (Join-Path $projectRoot 'tmp/sdkconfig.sd-session-test')
    if ($LASTEXITCODE -ne 0) { throw 'SD gateway pairing configuration failed.' }
    # Existing SDKCONFIG values override defaults. Apply this test profile
    # explicitly as well, so rebuilding an older checkout cannot retain 1MiB.
    $sdkPath = Join-Path $projectRoot 'tmp/sdkconfig.sd-session-test'
    $sdk = [IO.File]::ReadAllText($sdkPath)
    $sdk = [regex]::Replace($sdk, '(?m)^CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES=.*\r?\n?', '')
    [IO.File]::WriteAllText($sdkPath, $sdk + "`nCONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES=4194304`n")
    # A reused SDKCONFIG overrides defaults; enforce the bounded Wi-Fi queue.
    $sdk = [IO.File]::ReadAllText($sdkPath)
    $sdk = [regex]::Replace($sdk, '(?m)^CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=.*\r?\n?', '')
    [IO.File]::WriteAllText($sdkPath, $sdk + "`nCONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=$WifiCacheTxBuffers`n")
    # Keep the high-priority TCP/IP worker off the wake inference core. Permit
    # a controlled unbound comparison, but never silently reuse old values.
    $sdk = [IO.File]::ReadAllText($sdkPath)
    $sdk = [regex]::Replace($sdk, '(?m)^(?:# )?CONFIG_LWIP_TCPIP_TASK_AFFINITY(?:_NO_AFFINITY|_CPU0|_CPU1)?(?:=.*| is not set)\r?\n?', '')
    $coreChoice = if ($TcpipAffinity -eq 'cpu0') { 'CPU0' } else { 'NO_AFFINITY' }
    [IO.File]::WriteAllText($sdkPath, $sdk + "`nCONFIG_LWIP_TCPIP_TASK_AFFINITY_$coreChoice=y`n")
    . 'C:/esp/v6.0/esp-idf/export.ps1'
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' `
        C:/esp/v6.0/esp-idf/tools/idf.py -B $BuildDir `
        -D PROJECT_VER=7.0.177 `
        -D SDKCONFIG=tmp/sdkconfig.sd-session-test `
        -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.product-ota.defaults;sdkconfig.sd-replay-test.defaults;sdkconfig.sd-session-test.defaults' build
    if ($LASTEXITCODE -ne 0) { throw 'SD session test firmware build failed.' }
    $config = Get-Content -LiteralPath (Join-Path $BuildDir 'config/sdkconfig.h') -Raw
    foreach ($name in @('NUWAX_SD_SESSION_ENABLE','NUWAX_SD_LEGACY_CONTROL_COMPAT','NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE','NUWAX_SD_GATEWAY_DISCOVERY_ENABLE')) {
        if ($config -notmatch "#define CONFIG_$name 1") { throw "Missing test setting: $name" }
    }
    foreach ($name in @('NUWAX_TEST_PANEL_ENABLE','NUWAX_TEST_NETWORK_ENABLE')) {
        if ($config -match "#define CONFIG_$name 1") { throw "Unexpected developer UI enabled: $name" }
    }
    if ($config -match '#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1') { throw 'Unexpected formal upload route.' }
    if ($config -notmatch '#define CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES 4194304') { throw 'Unexpected SD archive size.' }
    if ($config -notmatch '#define CONFIG_NUWAX_SD_REPLAY_EARLY_TCP_WRITABLE 1') { throw 'Expected early TCP writable test profile; use an explicit A/B build for the default policy.' }
    foreach ($expected in @("ESP_WIFI_CACHE_TX_BUFFER_NUM $WifiCacheTxBuffers",'ESP_WIFI_STATIC_TX_BUFFER_NUM 6','SPIRAM_TRY_ALLOCATE_WIFI_LWIP 1','LWIP_TCP_SND_BUF_DEFAULT 46080')) {
        if (-not $config.Contains('#define CONFIG_' + $expected)) { throw "Unexpected network memory profile: $expected" }
    }
    if (-not $config.Contains('#define CONFIG_LWIP_TCPIP_TASK_AFFINITY_' + $coreChoice + ' 1')) {
        throw 'Unexpected TCP/IP task affinity.'
    }
    $gateway = Get-Content -LiteralPath (Join-Path $projectRoot 'runtime-logs/sd-test-gateway/settings.json') -Raw | ConvertFrom-Json
    if (-not $config.Contains('#define CONFIG_NUWAX_SD_GATEWAY_ID "' + $gateway.gatewayId + '"') -or
        -not $config.Contains('#define CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_KEY "' + $gateway.discoveryKey + '"')) {
        throw 'Firmware belongs to a different PC test gateway.'
    }
} finally { Pop-Location }
