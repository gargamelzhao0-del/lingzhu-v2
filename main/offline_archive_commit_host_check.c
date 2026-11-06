#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <io.h>
#include <process.h>
#define HEADER_BYTES 96U
#define FOOTER_BYTES 32U
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;
#include "offline_archive_types_host.inc"
static struct {
    FILE *file;
    uint32_t frame_count, payload_crc32;
    uint64_t payload_bytes, used_bytes;
    long last_good_offset;
    atomic_bool degraded;
    atomic_uint ready_count;
    char open_path[128], ready_path[128];
} s;
static atomic_bool s_directory_dirty;
static unsigned truncations;
static void writer_note_fault(const char *stage, int saved_errno) { (void)stage; (void)saved_errno; }
static int tracked_truncate(int fd,long len) {
    ++truncations;
    /* Match the POSIX fd operation; callers must position/flush the stream. */
    return _chsize(fd,len);
}
#define ftruncate tracked_truncate
static bool sync_archive_file(FILE *file,const char *step) {
    (void)step;
    return fflush(file)==0 && _commit(_fileno(file))==0;
}
static int64_t esp_timer_get_time(void) { return 0; }
static void log_sd_step(const char *step,int64_t time) { (void)step;(void)time; }
#include "offline_archive_commit_host.inc"
static void check_commit(bool torn_tail) {
    memset(&s,0,sizeof(s)); atomic_init(&s.degraded,torn_tail); atomic_init(&s.ready_count,0);
    snprintf(s.open_path,sizeof(s.open_path),"commit-check-%d.part",_getpid());
    snprintf(s.ready_path,sizeof(s.ready_path),"commit-check-%d.ready",_getpid());
    char result[128]; strcpy(result,s.ready_path);
    s.file=fopen(s.open_path,"w+b"); assert(s.file);
    unsigned char bytes[116]; memset(bytes,0x5a,sizeof(bytes));
    assert(fwrite(bytes,1,sizeof(bytes),s.file)==sizeof(bytes));
    s.last_good_offset=sizeof(bytes); s.frame_count=1; s.payload_bytes=16; s.payload_crc32=123;
    if (torn_tail) {
        assert(fwrite(bytes,1,50,s.file)==50);
        assert(fflush(s.file)==0);
        assert(fseek(s.file,s.last_good_offset,SEEK_SET)==0);
    }
    unsigned before=truncations;
    assert(commit_segment_locked(!torn_tail)==ESP_OK);
    assert(truncations-before==(torn_tail?1U:0U));
    assert(atomic_load(&s.ready_count)==1);
    FILE *file=fopen(result,"rb"); assert(file);
    unsigned char saved[116]; assert(fread(saved,1,sizeof(saved),file)==sizeof(saved));
    assert(memcmp(saved,bytes,sizeof(bytes))==0);
    archive_footer_t footer; assert(fread(&footer,1,sizeof(footer),file)==sizeof(footer));
    assert(memcmp(footer.magic,"NWXEND1",7)==0 && footer.frame_count==1);
    assert(footer.complete==!torn_tail && footer.crc32==123);
    assert(fgetc(file)==EOF); fclose(file); assert(remove(result)==0);
}
int main(void) {
    check_commit(false); check_commit(true);
    puts("archive commit: clean append and torn-tail truncation PASS");
    return 0;
}
