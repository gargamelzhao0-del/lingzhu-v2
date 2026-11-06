"""Run the production dual-slot routines on the SDK's real FAT32 implementation.

Unlike the host filesystem, FatFs leaves unwritten file gaps undefined. Seed
free data sectors with CRC-valid prior records to exercise reused SD media.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import re

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk', type=Path, default=Path('C:/esp/v6.0/esp-idf'))
parser.add_argument('--dynamic', action='store_true', help='Use target dynamic buffers, heap LFN and variable 512-4096 byte sectors')
parser.add_argument('--expect-cache-gap', action='store_true', help='Negative control for the old writer-handle readback')
args = parser.parse_args()
work = root / ('build-host-checks/sd-catalog-fatfs-dynamic' if args.dynamic else 'build-host-checks/sd-catalog-fatfs')
work.mkdir(parents=True, exist_ok=True)
for name in ['ff.c', 'ff.h', 'diskio.h', 'ffunicode.c', 'ffsystem.c']:
    shutil.copy2(args.sdk / 'components/fatfs/src' / name, work / name)
options = dict(FFCONF_DEF=80286, FF_FS_READONLY=0, FF_FS_MINIMIZE=0, FF_USE_FIND=0,
    FF_USE_MKFS=1, FF_USE_FASTSEEK=0, FF_USE_EXPAND=0, FF_USE_CHMOD=0, FF_USE_LABEL=0,
    FF_USE_FORWARD=0, FF_USE_STRFUNC=0, FF_PRINT_LLI=0, FF_PRINT_FLOAT=0, FF_STRF_ENCODE=3,
    FF_CODE_PAGE=437, FF_USE_LFN=2, FF_MAX_LFN=255, FF_LFN_UNICODE=2, FF_LFN_BUF=255,
    FF_SFN_BUF=12, FF_FS_RPATH=0, FF_VOLUMES=1, FF_STR_VOLUME_ID=0, FF_MULTI_PARTITION=0,
    FF_MIN_SS=512, FF_MAX_SS=512, FF_LBA64=0, FF_MIN_GPT='0x10000000', FF_USE_TRIM=0,
    FF_FS_TINY=0, FF_FS_EXFAT=0, FF_FS_NORTC=1, FF_NORTC_MON=1, FF_NORTC_MDAY=1,
    FF_NORTC_YEAR=2026, FF_FS_NOFSINFO=0, FF_FS_LOCK=0, FF_FS_REENTRANT=0,
    FF_FS_TIMEOUT=1000, FF_USE_DYN_BUFFER=0)
if args.dynamic:
    options.update(FF_USE_DYN_BUFFER=1, FF_MAX_SS=4096, FF_USE_LFN=3)
(work / 'ffconf.h').write_text('#pragma once\n' + '\n'.join(
    f'#define {key} {value}' for key, value in options.items()) + '\n', encoding='utf-8')
source = (root / 'main/sd_audio_catalog.c').read_text(encoding='utf-8')
def function(name):
    start = source.index(name)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
record_type = source[source.index('typedef bool (*catalog_record_match_t)'):source.index('\nstatic esp_err_t read_pair_matching')]
at = source.index('typedef struct {\n    const char *id;')
session_type = source[at:source.index('} catalog_session_key_t;', at) + len('} catalog_session_key_t;')]
names = ['static uint32_t checksum(', 'static bool valid_id(', 'static bool catalog_path(',
    'static bool valid_record(', 'static esp_err_t read_pair_matching(', 'static esp_err_t read_pair(',
    'static esp_err_t write_pair(', 'static bool session_record_matches(',
    'esp_err_t sd_audio_catalog_load(', 'esp_err_t sd_audio_catalog_save(',
    'esp_err_t sd_audio_catalog_has_pending(',
    'static esp_err_t catalog_verify_session_file(', 'static esp_err_t catalog_seed_session(']
production = record_type + '\n' + session_type + '\n' + '\n\n'.join(function(name) for name in names[:9])
production += '\n' + (root/'main/sd_audio_catalog_io.inc').read_text(encoding='utf-8')
production += '\n' + '\n\n'.join(function(name) for name in names[9:])
for old, new in [('FILE *', 'FIL *'), ('fopen(', 'fat_fopen('), ('fclose(', 'fat_fclose('),
    ('fseek(', 'fat_fseek('), ('fread(', 'fat_fread('), ('fwrite(', 'fat_fwrite('),
    ('fflush(', 'fat_fflush('), ('fsync(fileno(file))', 'fat_fflush(file)'),
    ('fstat(fileno(file), &st)', 'fat_fstat(file, &st)'), ('ferror(', 'fat_ferror('),
    ('opendir(', 'fat_opendir('), ('readdir(', 'fat_readdir('), ('closedir(', 'fat_closedir(')]:
    production = production.replace(old, new)
for name in ['open','close','stat','unlink','rename','clearerr']:
    production=re.sub(r'\b'+name+r'\(', 'fat_'+name+'(',production)
(work / 'sd_catalog_fatfs_production.inc').write_text(production, encoding='utf-8')
writer_source=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
verify_source=writer_source
if args.expect_cache_gap:
    verify_source=Path('C:/tmp/nuwax-sd-flow-release-20261030-r210/main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
start=verify_source.index('static esp_err_t writer_verify_records_locked(void)')
end=verify_source.index('\nstatic esp_err_t commit_segment_locked(',start)
writer=verify_source[start:end]
for old,new in [('FILE *','FIL *'),('fopen(', 'readback_fat_fopen('),('fclose(', 'readback_fat_fclose('),
    ('ftell(', 'fat_ftell('),('setvbuf(', 'fat_setvbuf('),('fseek(', 'fat_fseek('),
    ('fread(', 'readback_fat_fread('),('ferror(', 'fat_ferror('),('clearerr(', 'fat_clearerr(')]:
    writer=writer.replace(old,new)
(work/'sd_writer_readback_fatfs.inc').write_text(writer,encoding='utf-8')
start=writer_source.index('static FILE *archive_create_exclusive(')
end=writer_source.index('\nstatic esp_err_t open_segment_locked(',start)
create=writer_source[start:end]
for old,new in [('FILE *','FIL *'),('fdopen(', 'fat_fdopen(')]:create=create.replace(old,new)
for name in ['open','close']:create=re.sub(r'\b'+name+r'\(', 'fat_'+name+'(',create)
(work/'sd_writer_create_fatfs.inc').write_text(create,encoding='utf-8')
exe = work / 'fatfs.exe'
subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    *(['-DEXPECT_WRITER_CACHE_GAP=1'] if args.expect_cache_gap else []),
    '-Wno-unused-function', '-Wno-implicit-fallthrough',
    '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(root / 'main'),
    '-I' + str(work), str(root / 'main/sd_audio_catalog_fatfs_host_check.c'),
    str(work / 'ff.c'), str(work / 'ffunicode.c'), str(work / 'ffsystem.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True, timeout=30)
