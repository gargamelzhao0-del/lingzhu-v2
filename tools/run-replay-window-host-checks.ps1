param([string]$Gcc='gcc', [switch]$RomCrc, [switch]$NoLaneProbe, [switch]$LockedValidation, [ValidateSet(16,32)][int]$WifiCacheTxBuffers=32, [ValidateSet(4096,8192)][int]$TlsOutBytes=8192, [string]$IdfPath='C:/esp/v6.0/esp-idf')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content "$root/main/offline_audio_spool.c" -Raw
$a=$source.IndexOf('static uint32_t crc32_update(')
$b=$source.IndexOf('static esp_err_t validate_archive_frame(', $a)
[IO.File]::WriteAllText("$out/replay_crc_host.inc",$source.Substring($a,$b-$a))
$a=$b
$b=$source.IndexOf('static ', $a+20)
[IO.File]::WriteAllText("$out/replay_frame_validator_host.inc",$source.Substring($a,$b-$a))
$a=$source.IndexOf('typedef struct __attribute__((packed))')
$b=$source.IndexOf('typedef struct {', $a)
[IO.File]::WriteAllText("$out/offline_archive_types_host.inc",$source.Substring($a,$b-$a))
$a=$source.IndexOf('static esp_err_t replay_send_parts(')
$b=$source.IndexOf('static esp_err_t replay_send_frame(', $a)
[IO.File]::WriteAllText("$out/replay_parts_host.inc",$source.Substring($a,$b-$a))
$a=$source.IndexOf('static bool replay_bulk_cancelled(')
$b=$source.IndexOf('static esp_err_t replay_one_ready_file(', $a)
[IO.File]::WriteAllText("$out/replay_window_host.inc",$source.Substring($a,$b-$a).Replace('#include "offline_replay_shared.inc"','').Replace('#include "sd_audio_pending_commit.inc"',''))
$a=$b
$b=$source.IndexOf('static esp_err_t replay_one_ready(', $a)
[IO.File]::WriteAllText("$out/replay_commit_host.inc",$source.Substring($a,$b-$a))
$a=$source.IndexOf('static esp_err_t validate_archive_records(')
$b=$source.IndexOf('#include "offline_replay_stream.inc"', $a)
[IO.File]::WriteAllText("$out/replay_records_host.inc",$source.Substring($a,$b-$a))
$a=$source.IndexOf('static esp_err_t recheck_archive_path_once(')
$b=$source.IndexOf('#include "wake_word_detector.h"', $a)
[IO.File]::WriteAllText("$out/replay_recovery_host.inc",$source.Substring($a,$b-$a))
$a=$source.IndexOf('static esp_err_t find_next_ready_name(')
$b=$source.IndexOf('static esp_err_t replay_ready_impl(', $a)
[IO.File]::WriteAllText("$out/replay_directory_host.inc",$source.Substring($a,$b-$a))
$extraArgs=@()
$exe="$out/replay_window_host_check.exe"
if ($RomCrc) {
    $crcSource=Join-Path $IdfPath 'components/esp_rom/linux/esp_rom_crc.c'
    if (-not (Test-Path -LiteralPath $crcSource)) { throw "Missing ESP-IDF CRC reference: $crcSource" }
    $extraArgs=@('-DNUWAX_CRC_ROM_HOST_TEST=1',"-I$IdfPath/components/esp_rom/include",$crcSource)
    $exe="$out/replay_window_rom_host_check.exe"
}
if ($NoLaneProbe) {
    $extraArgs += '-DCONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_LANE_PROBE=0'
    $exe=$exe.Replace('.exe','_no_probe.exe')
}
$extraArgs += "-DCONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=$WifiCacheTxBuffers"
$extraArgs += "-DCONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=$TlsOutBytes"
$unlock = if ($LockedValidation) { 0 } else { 1 }
$extraArgs += "-DCONFIG_NUWAX_SD_READ_UNLOCK_VALIDATE=$unlock"
$extraArgs += "$root/main/cloud_time.c"
& $Gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function "-I$out" "-I$root/main" "-I$root/main/audio_modes/meeting/host_stubs" "-I$root/managed_components/espressif__cjson/cJSON" "$root/main/replay_window_host_check.c" "$root/main/audio_frame.c" "$root/managed_components/espressif__cjson/cJSON/cJSON.c" @extraArgs -o $exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location $out
try { & $exe } finally { Pop-Location }
exit $LASTEXITCODE
