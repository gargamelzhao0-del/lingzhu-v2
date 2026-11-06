"""Reconcile retained archives and sidecars without changing the cloud format."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-r196';out.mkdir(parents=True,exist_ok=True)
exe=out/'archive-recovery.exe'
args=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
 '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
 '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
 '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),
 str(root/'main/sd_audio_archive_recovery_host_check.c'),str(root/'main/audio_frame.c'),'-o',str(exe)]
subprocess.run(args,check=True)
with tempfile.TemporaryDirectory(prefix='sd-r196-archive-') as directory:
    subprocess.run([str(exe)],cwd=directory,check=True,timeout=90)
active=out/'active-recovery.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
 '-DESP_ERR_INVALID_CRC=0x109',
 '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),
 str(root/'main/sd_audio_active_recovery_host_check.c'),'-o',str(active)],check=True)
with tempfile.TemporaryDirectory(prefix='sd-r196-active-') as directory:
    subprocess.run([str(active)],cwd=directory,check=True,timeout=30)
directory_io=out/'directory-io.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
 '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
 '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
 '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),
 str(root/'main/sd_audio_directory_io_host_check.c'),str(root/'main/audio_frame.c'),'-o',str(directory_io)],check=True)
with tempfile.TemporaryDirectory(prefix='sd-r196-directory-') as directory:
    subprocess.run([str(directory_io)],cwd=directory,check=True,timeout=30)
