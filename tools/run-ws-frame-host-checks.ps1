param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content "$root/main/ws_transport.c" -Raw
function Extract([string]$start, [string]$end) {
    $a=$source.IndexOf($start)
    $b=$source.IndexOf($end,$a+1)
    if ($a -lt 0 -or $b -le $a) { throw "Missing production block: $start" }
    return $source.Substring($a,$b-$a)
}
$code=Extract 'static esp_err_t send_all_until(' 'static esp_err_t send_all('
$code+=Extract 'static esp_err_t ensure_lifecycle_lock(void) {' 'static void lifecycle_store('
$code+=Extract 'void nuwax_ws_init(' '/* Caller owns tx_frame_lock'
$code+=Extract 'esp_err_t nuwax_ws_connect(' 'esp_err_t nuwax_ws_send_text('
$code+=Extract 'esp_err_t nuwax_ws_send_archive_chunk_if_epoch(' 'esp_err_t nuwax_ws_send_archive_text_if_epoch('
[IO.File]::WriteAllText("$out/ws_frame_production_host.inc",$code)
& $Gcc -std=c11 -O2 -fstrict-aliasing -fno-builtin-memcpy -Wall -Wextra -Werror -Wno-unused-function `
    "-I$root/main/audio_modes/meeting/host_stubs" "-I$root/main" "-I$out" `
    "$root/main/ws_frame_host_check.c" -o "$out/ws_frame_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/ws_frame_host_check.exe"
exit $LASTEXITCODE
