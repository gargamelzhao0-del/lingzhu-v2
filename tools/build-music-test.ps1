param([string]$BuildDir='C:/tmp/nuwax-music-20261029-r202',
      [string]$GatewayOrigin='http://192.168.31.10:8798',
      [ValidatePattern('^7\.0\.[0-9]+$')][string]$Version='7.0.202')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$uri=[Uri]$GatewayOrigin
if ($uri.Scheme -ne 'http' -or $uri.AbsolutePath -ne '/' -or $uri.Query -or $uri.UserInfo) {
    throw 'GatewayOrigin must be a plain HTTP LAN origin.'
}
Push-Location $root
try {
    $config=Join-Path $root 'tmp/sdkconfig.music-test'
    $seed=Join-Path $root 'tmp/sdkconfig.sd-session-formal'
    if (-not (Test-Path -LiteralPath $seed)) { $seed=Join-Path $root 'sdkconfig' }
    $sdk=[IO.File]::ReadAllText($seed)
    # Generated configs override defaults: explicitly refresh the TLS record
    # size while leaving the test gateway and provisioned route untouched.
    $sdk=[regex]::Replace($sdk,'(?m)^CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=.*\r?\n?','')
    $sdk += "`nCONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=8192`n"
    $sdk=[regex]::Replace($sdk,'(?m)^(?:# )?CONFIG_NUWAX_SD_REPLAY_EARLY_TCP_WRITABLE(?:=.*| is not set)\r?\n?','')
    $sdk += "CONFIG_NUWAX_SD_REPLAY_EARLY_TCP_WRITABLE=y`n"
    $sdk=[regex]::Replace($sdk,'(?m)^(?:# )?CONFIG_NUWAX_SD_TLS_SCOPED_AES(?:=.*| is not set)\r?\n?','')
    $sdk += "CONFIG_NUWAX_SD_TLS_SCOPED_AES=y`n"
    $sdk=[regex]::Replace($sdk,'(?m)^(?:# )?CONFIG_LWIP_TCPIP_CORE_LOCKING(?:=.*| is not set)\r?\n?','')
    $sdk += "CONFIG_LWIP_TCPIP_CORE_LOCKING=y`n"
    $sdk=[regex]::Replace($sdk,'(?m)^CONFIG_NUWAX_SD_MEETING_MAX_HOURS=.*\r?\n?','')
    $sdk += "CONFIG_NUWAX_SD_MEETING_MAX_HOURS=8`n"
    $sdk=[regex]::Replace($sdk,'(?m)^(?:# )?CONFIG_NUWAX_MUSIC_[A-Z_]+(?:=.*| is not set)\r?\n?','')
    $sdk += "`nCONFIG_NUWAX_MUSIC_PLAYBACK_ENABLE=y`nCONFIG_NUWAX_MUSIC_MP3_ENABLE=y`n"
    $sdk += "CONFIG_NUWAX_MUSIC_CONTROL_TEST_ENABLE=y`nCONFIG_NUWAX_MUSIC_CONTROL_TEST_URL=`"$($GatewayOrigin.TrimEnd('/'))`"`n"
    $sdk += "# CONFIG_NUWAX_MUSIC_URL_TEST_ENABLE is not set`n"
    [IO.File]::WriteAllText($config,$sdk)
    . 'C:/esp/v6.0/esp-idf/export.ps1'
    & 'C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe' C:/esp/v6.0/esp-idf/tools/idf.py `
        -B $BuildDir -D "PROJECT_VER=$Version" -D SDKCONFIG=tmp/sdkconfig.music-test `
        -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.product-ota.defaults;sdkconfig.sd-replay-formal.defaults' build
    if ($LASTEXITCODE -ne 0) { throw 'Music firmware build failed' }
    $built=Get-Content -LiteralPath (Join-Path $BuildDir 'config/sdkconfig.h') -Raw
    if ($built -notmatch '#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 8192') { throw 'Stale TLS record size' }
    if ($built -notmatch '#define CONFIG_NUWAX_SD_REPLAY_EARLY_TCP_WRITABLE 1') { throw 'TCP writable optimization missing' }
    if ($built -notmatch '#define CONFIG_NUWAX_SD_TLS_SCOPED_AES 1') { throw 'Scoped SD AES missing' }
    if ($built -notmatch '#define CONFIG_LWIP_TCPIP_CORE_LOCKING 1') { throw 'TCP/IP core locking missing' }
    if ($built -notmatch '#define CONFIG_NUWAX_SD_MEETING_MAX_HOURS 8') { throw 'Card meeting total duration limit missing' }
    foreach ($setting in @('NUWAX_MUSIC_PLAYBACK_ENABLE','NUWAX_MUSIC_MP3_ENABLE','NUWAX_MUSIC_CONTROL_TEST_ENABLE')) {
        if ($built -notmatch "#define CONFIG_$setting 1") { throw "Missing feature: $setting" }
    }
} finally { Pop-Location }
