param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build-host-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$source=Get-Content "$root/main/wake_word_detector.c" -Raw
$a=$source.IndexOf('typedef struct {', $source.IndexOf('static nuwax_wake_detector_t s_detector;'))
$b=$source.IndexOf('} nuwax_wake_capture_context_t;', $a)+'} nuwax_wake_capture_context_t;'.Length
[IO.File]::WriteAllText("$out/wake_capture_types_host.inc",$source.Substring($a,$b-$a))
$a=$source.IndexOf('static void wake_capture_return_chunk(')
$b=$source.IndexOf('static void wake_capture_delete_queue(', $a)
[IO.File]::WriteAllText("$out/wake_capture_producer_host.inc",$source.Substring($a,$b-$a))
$a=$source.IndexOf('static esp_err_t wake_capture_read(')
$b=$source.IndexOf('static void wake_word_task(', $a)
[IO.File]::WriteAllText("$out/wake_capture_reader_host.inc",$source.Substring($a,$b-$a))
& $Gcc -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-variable "-I$out" "-I$root/main" "$root/main/wake_capture_host_check.c" -o "$out/wake_capture_host_check.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out/wake_capture_host_check.exe"
exit $LASTEXITCODE
