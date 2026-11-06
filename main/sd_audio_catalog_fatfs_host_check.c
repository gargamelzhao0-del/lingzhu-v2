#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "ff.h"
#include "diskio.h"
#include "sd_audio_catalog.h"
#include "sd_audio_read_recovery_host.h"
typedef FF_DIR DIR;
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_CRC 0x109

#define SD_AUDIO_CATALOG_ROOT "0:/catalog"
#define CATALOG_MAGIC 0x3153584eU
#define SEGMENT_MAGIC 0x3147584eU
static bool metadata_io_qualified;
static uint32_t metadata_probe_ticket;
#define SECTORS 131072U
static uint8_t *disk_image;
static bool drop_sector_write;
static LBA_t dropped_sector;
static unsigned dropped_writes;
DSTATUS disk_initialize(BYTE p) { (void)p; return 0; }
DSTATUS disk_status(BYTE p) { (void)p; return 0; }
DRESULT disk_read(BYTE p, BYTE *out, LBA_t sector, UINT count) {
    (void)p; if (sector + count > SECTORS) return RES_PARERR;
    memcpy(out, disk_image + (size_t)sector * 512U, (size_t)count * 512U); return RES_OK;
}
DRESULT disk_write(BYTE p, const BYTE *in, LBA_t sector, UINT count) {
    (void)p; if (sector + count > SECTORS) return RES_PARERR;
    if (drop_sector_write && dropped_sector >= sector && dropped_sector < sector + count) {
        for (UINT i=0;i<count;i++) {
            if (sector+i == dropped_sector) { ++dropped_writes; continue; }
            memcpy(disk_image+(size_t)(sector+i)*512U,in+(size_t)i*512U,512U);
        }
        return RES_OK; /* Model a card reporting success without persisting one sector. */
    }
    memcpy(disk_image + (size_t)sector * 512U, in, (size_t)count * 512U); return RES_OK;
}
DRESULT disk_ioctl(BYTE p, BYTE cmd, void *data) {
    (void)p;
    if (cmd == CTRL_SYNC) return RES_OK;
    if (cmd == GET_SECTOR_COUNT) { *(LBA_t *)data = SECTORS; return RES_OK; }
    if (cmd == GET_SECTOR_SIZE) { *(WORD *)data = 512; return RES_OK; }
    if (cmd == GET_BLOCK_SIZE) { *(DWORD *)data = 1; return RES_OK; }
    return RES_PARERR;
}
static FIL *fat_fopen(const char *path, const char *mode) {
    FIL *f = calloc(1, sizeof(*f)); assert(f);
    BYTE flags = !strcmp(mode, "w+b") ? FA_READ | FA_WRITE | FA_CREATE_ALWAYS :
                 !strcmp(mode, "wb") ? FA_WRITE | FA_CREATE_ALWAYS :
                 !strcmp(mode, "rb") ? FA_READ : FA_READ | FA_WRITE;
    FRESULT r = f_open(f, path, flags);
    if (r != FR_OK) { errno = r == FR_NO_FILE || r == FR_NO_PATH ? ENOENT : EIO; free(f); return NULL; }
    return f;
}
static int fat_fclose(FIL *f) { FRESULT r = f_close(f); free(f); return r == FR_OK ? 0 : -1; }
static int fat_fseek(FIL *f, long pos, int mode) {
    assert(mode == SEEK_SET || mode == SEEK_END);
    long target = mode == SEEK_END ? (long)f_size(f) + pos : pos;
    assert(target >= 0); return f_lseek(f, (FSIZE_t)target) == FR_OK ? 0 : -1;
}
static long fat_ftell(FIL *f) { return (long)f_tell(f); }
static size_t fat_fread(void *out, size_t bytes, size_t n, FIL *f) {
    UINT got = 0; assert(f_read(f, out, (UINT)(bytes*n), &got) == FR_OK); return got / bytes;
}
static size_t fat_fwrite(const void *in, size_t bytes, size_t n, FIL *f) {
    UINT got = 0; assert(f_write(f, in, (UINT)(bytes*n), &got) == FR_OK); return got / bytes;
}
static int fat_fflush(FIL *f) { return f_sync(f) == FR_OK ? 0 : -1; }
static int fat_ferror(FIL *f) { return f_error(f); }
static int fat_fstat(FIL *f, struct stat *st) { memset(st,0,sizeof(*st)); st->st_size=f_size(f); return 0; }
static void fat_clearerr(FIL *f) { (void)f; }
static FIL *reservation;
static int fat_open(const char *path, int flags, int mode) {
    (void)mode; assert(!reservation && (flags & O_CREAT) && (flags & O_EXCL));
    reservation=calloc(1,sizeof(*reservation));assert(reservation);
    const FRESULT result=f_open(reservation,path,FA_READ|FA_WRITE|FA_CREATE_NEW);
    if(result!=FR_OK){free(reservation);reservation=NULL;errno=result==FR_EXIST?EEXIST:EIO;return -1;}
    return 1;
}
static int fat_close(int fd) { assert(fd==1&&reservation);FIL *file=reservation;reservation=NULL;return fat_fclose(file); }
static bool fail_fdopen;
static FIL *fat_fdopen(int fd,const char *mode) {
    assert(fd==1 && reservation && !strcmp(mode,"w+b"));
    if(fail_fdopen){errno=ENOMEM;return NULL;}
    FIL *file=reservation;reservation=NULL;return file;
}
static int fat_setvbuf(FIL *file,char *buffer,int mode,size_t bytes) {
    assert(file && buffer && mode==_IOFBF && bytes==8192U);return 0;
}
static int fat_stat(const char *path,struct stat *st) {
    FILINFO info;const FRESULT result=f_stat(path,&info);
    if(result!=FR_OK){errno=result==FR_NO_FILE||result==FR_NO_PATH?ENOENT:EIO;return -1;}
    memset(st,0,sizeof(*st));st->st_size=info.fsize;return 0;
}
static int fat_unlink(const char *path) { const FRESULT r=f_unlink(path);if(r!=FR_OK)errno=EIO;return r==FR_OK?0:-1; }
static int fat_rename(const char *a,const char *b) { const FRESULT r=f_rename(a,b);if(r!=FR_OK)errno=EIO;return r==FR_OK?0:-1; }
static esp_err_t ensure_catalog_directory(void) {const FRESULT r=f_mkdir(SD_AUDIO_CATALOG_ROOT);return r==FR_OK||r==FR_EXIST?ESP_OK:ESP_FAIL;}
bool sd_audio_catalog_replay_pinned(void) {return false;}
struct dirent { char d_name[256]; };
static DIR *fat_opendir(const char *path) {
    DIR *d = calloc(1, sizeof(*d)); assert(d); FRESULT r = f_opendir(d, path);
    if (r != FR_OK) { errno = r == FR_NO_PATH ? ENOENT : EIO; free(d); return NULL; } return d;
}
static struct dirent *fat_readdir(DIR *d) {
    static struct dirent e; FILINFO i;
    assert(f_readdir(d, &i) == FR_OK); if (!i.fname[0]) return NULL;
    snprintf(e.d_name, sizeof(e.d_name), "%s", i.fname); return &e;
}
static int fat_closedir(DIR *d) { FRESULT r = f_closedir(d); free(d); return r == FR_OK ? 0 : -1; }
static sd_audio_catalog_stats_t catalog_stats;
static bool done_cached;
static char done_id[48];
static void invalidate_selection(void) {}
static void invalidate_replay_picks(void) {}
static void done_cache_forget(const char *id) { if (!strcmp(id,done_id)) done_cached=false; }
static void done_cache_remember(const sd_audio_session_t *s) {
    if(s->done) { done_cached=true; strcpy(done_id,s->session_id); }
}
static bool done_cache_skip(const char *id) { return done_cached && !strcmp(id,done_id); }
static void catalog_prune_pages(const sd_audio_session_t *s, uint32_t n, bool all) { (void)s;(void)n;(void)all; }
static bool session_blocked(const char *id) { (void)id; return false; }
static esp_err_t load_replay_session(const char *id, sd_audio_session_t *s) { return sd_audio_catalog_load(id,s); }
#include "sd_catalog_fatfs_production.inc"

#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define ARCHIVE_STDIO_BUFFER_BYTES 8192U
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static char writer_stdio[2U*ARCHIVE_STDIO_BUFFER_BYTES];
static struct {
    FIL *file;
    char *write_buffer;
    long last_good_offset, verify_offset;
    uint32_t verify_bytes, verify_crc32, verify_calls, segment_index;
    uint64_t verify_us;
    char session_id[48];
    char open_path[128];
} s;
static bool inject_readback;
static unsigned transient_readbacks;
static bool fail_readback_open, fail_readback_close, short_readback;
static FIL *readback_fat_fopen(const char *path,const char *mode) {
    if(fail_readback_open){errno=EIO;return NULL;}
    assert(s.file && !strcmp(path,s.open_path) && !strcmp(mode,"rb"));
    return fat_fopen(path,mode);
}
static int readback_fat_fclose(FIL *file) {
    assert(file!=s.file);const int result=fat_fclose(file);
    return fail_readback_close ? -1 : result;
}
static size_t readback_fat_fread(void *out,size_t size,size_t n,FIL *file) {
    if(short_readback)return 0;
    size_t got=fat_fread(out,size,n,file);
    if(inject_readback && got)((uint8_t*)out)[0]^=0x80U;
    if(transient_readbacks && got){--transient_readbacks;((uint8_t*)out)[0]^=0x40U;}
    return got;
}
static bool card_recording_locked(void) { return true; }
static int64_t esp_timer_get_time(void) { return 0; }
static void writer_note_fault(const char *name,int error) { (void)name;(void)error; }
static uint32_t crc32_update(uint32_t crc,const void *data,size_t n) {
    const uint8_t *p=data;crc=~crc;
    while(n--){crc^=*p++;for(unsigned b=0;b<8U;b++)crc=(crc>>1U)^((crc&1U)?0xedb88320U:0U);}
    return ~crc;
}
#include "sd_writer_readback_fatfs.inc"
#include "sd_writer_create_fatfs.inc"

static void writer_exclusive_create(void) {
    FIL *file=archive_create_exclusive("0:/exclusive.raw");assert(file);
    const uint8_t saved[]={0xaa,0x55,0x11,0xee};
    assert(fat_fwrite(saved,1,sizeof(saved),file)==sizeof(saved) && !fat_fclose(file));
    errno=0;assert(!archive_create_exclusive("0:/exclusive.raw") && errno==EEXIST);
    uint8_t check[sizeof(saved)];file=fat_fopen("0:/exclusive.raw","rb");assert(file);
    assert(fat_fread(check,1,sizeof(check),file)==sizeof(check) && !memcmp(saved,check,sizeof(saved)) && !fat_fclose(file));
    fail_fdopen=true;errno=0;assert(!archive_create_exclusive("0:/allocation.raw") && errno==ENOMEM && !reservation);
    fail_fdopen=false;
    puts("PASS actual FatFs exclusive create: existing audio unchanged; fdopen failure closes owned descriptor and preserves errno");
}

static void writer_cache_fault(void) {
    memset(&s,0,sizeof(s));s.file=fat_fopen("0:/cache-fault.raw","w+b");assert(s.file);
    s.write_buffer=writer_stdio;
    strcpy(s.open_path,"0:/cache-fault.raw");
    uint8_t bytes[96+666];for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]=(uint8_t)(i*7U+23U);
    assert(fat_fwrite(bytes,1,sizeof(bytes),s.file)==sizeof(bytes));
    s.verify_offset=0;s.verify_bytes=sizeof(bytes);s.last_good_offset=sizeof(bytes);
    s.verify_crc32=crc32_update(0U,bytes,sizeof(bytes));
    dropped_sector=s.file->sect;dropped_writes=0;drop_sector_write=true;
    assert(!fat_fflush(s.file));drop_sector_write=false;assert(dropped_writes==1);
    const esp_err_t checked=writer_verify_records_locked();
#ifdef EXPECT_WRITER_CACHE_GAP
    assert(checked==ESP_OK); /* Negative control: current writer cache hides the lost sector. */
#else
    assert(checked==ESP_ERR_INVALID_CRC && s.verify_bytes==sizeof(bytes));
#endif
    assert(!fat_fclose(s.file));s.file=NULL;
    FIL *fresh=fat_fopen("0:/cache-fault.raw","rb");assert(fresh);
    uint8_t observed[sizeof(bytes)];assert(fat_fread(observed,1,sizeof(observed),fresh)==sizeof(observed));
    assert(memcmp(bytes,observed,sizeof(bytes)) && !fat_fclose(fresh));
    printf("PASS actual FatFs dropped-sector control: checked=%d persisted_crc=%08lx expected=%08lx\n",
        checked,(unsigned long)crc32_update(0U,observed,sizeof(observed)),
        (unsigned long)crc32_update(0U,bytes,sizeof(bytes)));
}

static void writer_batches(FATFS *fs) {
    memset(&s,0,sizeof(s));s.file=fat_fopen("0:/readback.raw","w+b");assert(s.file);
    s.write_buffer=writer_stdio;
    strcpy(s.open_path,"0:/readback.raw");
    uint8_t header[96]={0},record[666];
    assert(fat_fwrite(header,1,sizeof(header),s.file)==sizeof(header));
    s.verify_offset=s.last_good_offset=sizeof(header);
    for(unsigned batch=0;batch<256U;batch++) {
        for(unsigned frame=0;frame<100U;frame++) {
            for(unsigned b=0;b<sizeof(record);b++)record[b]=(uint8_t)(batch*17U+frame*13U+b);
            assert(fat_fwrite(record,1,sizeof(record),s.file)==sizeof(record));
            s.verify_crc32=crc32_update(s.verify_crc32,record,sizeof(record));
            s.verify_bytes+=sizeof(record);s.last_good_offset+=sizeof(record);
        }
        assert(fat_fflush(s.file)==0);
        assert(writer_verify_records_locked()==ESP_OK&&!s.verify_bytes&&fat_ftell(s.file)==s.last_good_offset);
        if(batch==127U) {
            assert(!fat_fclose(s.file)&&f_mount(NULL,"0:",0)==FR_OK&&f_mount(fs,"0:",1)==FR_OK);
            s.file=fat_fopen("0:/readback.raw","r+b");assert(s.file);
            assert(!fat_fseek(s.file,s.last_good_offset,SEEK_SET));
        }
    }
    /* A byte mismatch on the real FatFs stream is never committed as verified. */
    assert(fat_fwrite(record,1,sizeof(record),s.file)==sizeof(record)&&!fat_fflush(s.file));
    s.verify_crc32=crc32_update(0U,record,sizeof(record));s.verify_bytes=sizeof(record);s.last_good_offset+=sizeof(record);
    inject_readback=true;assert(writer_verify_records_locked()==ESP_ERR_INVALID_CRC&&s.verify_bytes==sizeof(record));
    inject_readback=false;assert(writer_verify_records_locked()==ESP_OK&&!s.verify_bytes);
    assert(fat_fwrite(record,1,sizeof(record),s.file)==sizeof(record)&&!fat_fflush(s.file));
    s.verify_crc32=crc32_update(0U,record,sizeof(record));s.verify_bytes=sizeof(record);s.last_good_offset+=sizeof(record);
#ifndef EXPECT_WRITER_CACHE_GAP
    const uint32_t calls=s.verify_calls;transient_readbacks=1;
    assert(writer_verify_records_locked()==ESP_OK&&!s.verify_bytes&&s.verify_calls==calls+2U&&!transient_readbacks);
    assert(fat_fwrite(record,1,sizeof(record),s.file)==sizeof(record)&&!fat_fflush(s.file));
    s.verify_crc32=crc32_update(0U,record,sizeof(record));s.verify_bytes=sizeof(record);s.last_good_offset+=sizeof(record);
    fail_readback_open=true;assert(writer_verify_records_locked()==ESP_FAIL&&s.verify_bytes==sizeof(record));fail_readback_open=false;
    short_readback=true;assert(writer_verify_records_locked()==ESP_FAIL&&s.verify_bytes==sizeof(record));short_readback=false;
    fail_readback_close=true;assert(writer_verify_records_locked()==ESP_FAIL&&s.verify_bytes==sizeof(record));fail_readback_close=false;
#endif
    assert(writer_verify_records_locked()==ESP_OK&&!s.verify_bytes&&fat_ftell(s.file)==s.last_good_offset);
    assert(!fat_fclose(s.file));s.file=NULL;
    puts("PASS real SDK FatFs: 25,600 mixed write/sync/readback frames across remount; exact CRC/cursor, injected mismatch retained");
}

static void scenario(bool recycled) {
    FATFS fs; MKFS_PARM mk = {.fmt=FM_FAT32|FM_SFD,.n_fat=2,.align=1,.n_root=0,.au_size=512};
    uint8_t work[4096];
    disk_image=calloc(SECTORS,512); assert(disk_image);
    done_cached=false;done_id[0]=0;
    if (recycled) {
        /* Model allocated SD sectors retaining a CRC-valid former manifest.
         * A real quick format/allocation does not erase the data clusters. */
        sd_audio_session_t old={.generation=900,.magic=CATALOG_MAGIC,.complete=0,.done=1,.ended=1};
        strcpy(old.session_id,"old_completed");
        old.crc=checksum(&old,offsetof(sd_audio_session_t,crc));
        for (size_t sector=0;sector<SECTORS;sector++) memcpy(disk_image+sector*512,&old,sizeof(old));
    }
    assert(f_mkfs("0:",&mk,work,sizeof(work))==FR_OK);
    assert(f_mount(&fs,"0:",1)==FR_OK && fs.fs_type==FS_FAT32);
    assert(f_mkdir("0:/catalog")==FR_OK);
    metadata_io_qualified=false;metadata_probe_ticket=0;
    assert(sd_audio_catalog_qualify_storage()==ESP_OK && metadata_io_qualified);
    sd_audio_session_t fresh={.magic=CATALOG_MAGIC,.complete=1,.done=0,.boot_id=7,.start_ms=38000};
    strcpy(fresh.session_id,"meeting_fresh");
    /* Execute the actual two-slot seed/readback, not a shortened model. */
    FIL *f=NULL;
    assert(catalog_seed_session("0:/catalog/meeting_fresh.session",&fresh)==ESP_OK && fresh.generation==2U);
    f=fat_fopen("0:/catalog/meeting_fresh.session","rb"); assert(f);
    sd_audio_session_t picked;
    assert(read_pair(f,0,&picked,sizeof(picked),offsetof(sd_audio_session_t,crc),CATALOG_MAGIC)==ESP_OK);
    assert(!fat_fclose(f));
    printf("FatFs FAT32 recycled=%d first_read_id=%s generation=%u complete=%u done=%u\n",
           recycled,picked.session_id,picked.generation,picked.complete,picked.done);
    fresh.segment_count=1;
    assert(sd_audio_catalog_save(&fresh)==ESP_OK); /* Reserve first audio file. */
    printf("after_real_save current_id=%s complete=%u done=%u\n",fresh.session_id,fresh.complete,fresh.done);
    fresh.segment_count=10;fresh.ended=1;fresh.ended_ms=1194000;
    assert(sd_audio_catalog_save(&fresh)==ESP_OK);
    bool pending=true;assert(sd_audio_catalog_has_pending(&pending)==ESP_OK);
    printf("after_clean_capture_and_stop complete=%u done=%u has_pending=%u\n",fresh.complete,fresh.done,pending);
    assert(fresh.complete && !fresh.done && pending);
    /* A CRC-valid foreign slot must not outrank the matching predecessor. */
    sd_audio_session_t alien=fresh;
    strcpy(alien.session_id,"old_completed");alien.generation=900;alien.complete=0;alien.done=1;
    alien.crc=checksum(&alien,offsetof(sd_audio_session_t,crc));
    f=fat_fopen("0:/catalog/meeting_fresh.session","r+b");assert(f);
    assert(fat_fseek(f,0,SEEK_SET)==0 && fat_fwrite(&alien,1,sizeof(alien),f)==sizeof(alien));
    assert(!fat_fclose(f));
    sd_audio_session_t matching;
    assert(sd_audio_catalog_load(fresh.session_id,&matching)==ESP_OK && matching.complete && !matching.done);
    assert(sd_audio_catalog_save(&fresh)==ESP_OK && fresh.complete && !fresh.done);
    /* FatFs index growth must initialize intervening pairs as well. */
    f=fat_fopen("0:/catalog/growth.index","wb");assert(f);
    sd_audio_session_t growth=fresh;growth.generation=0;
    assert(write_pair(f,1024U,&growth,sizeof(growth),offsetof(sd_audio_session_t,crc))==ESP_OK);
    assert(!fat_fclose(f));
    f=fat_fopen("0:/catalog/growth.index","rb");assert(f);
    uint8_t gap[1024];assert(fat_fread(gap,1,sizeof(gap),f)==sizeof(gap));
    for(unsigned i=0;i<sizeof(gap);i++)assert(!gap[i]);
    assert(read_pair(f,1024U,&matching,sizeof(matching),offsetof(sd_audio_session_t,crc),CATALOG_MAGIC)==ESP_OK);
    assert(!matching.done && matching.complete && !fat_fclose(f));

    /* An already allocated foreign pair must be rejected before merging
     * flags, even if the CRC and generation are both otherwise valid. */
    sd_audio_session_t foreign={.magic=CATALOG_MAGIC,.generation=200,.complete=0,.done=1};
    strcpy(foreign.session_id,"foreign_scene");
    f=fat_fopen("0:/catalog/meeting_guard.session","wb");assert(f);
    assert(write_pair(f,0,&foreign,sizeof(foreign),offsetof(sd_audio_session_t,crc))==ESP_OK);
    assert(!fat_fclose(f));
    sd_audio_session_t guard={.complete=1,.done=0};strcpy(guard.session_id,"meeting_guard");
    esp_err_t guarded=sd_audio_catalog_save(&guard);
    printf("foreign_identity_save err=%d complete=%u done=%u\n",guarded,guard.complete,guard.done);
    assert(guarded==ESP_ERR_INVALID_STATE && guard.complete && !guard.done);

    writer_batches(&fs);
    writer_cache_fault();
    writer_exclusive_create();
    assert(f_mount(NULL,"0:",0)==FR_OK);free(disk_image);
}
int main(void) {
    scenario(false);scenario(true);
    puts("FIX VERIFIED: explicit slot initialization prevents inherited complete/done flags on the same real FatFs fixture.");

}
