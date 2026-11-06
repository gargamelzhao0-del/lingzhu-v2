#include <assert.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <windows.h>
#include <direct.h>
static char legacy[80], current[80];
#define ROOT_DIR legacy
#define ARCHIVE_NEW_ROOT current
#define mkdir(path, mode) _mkdir(path)
#include "offline_archive_directory.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_SIZE -3
#define ESP_ERR_NOT_FOUND -4
#define ESP_ERR_NOT_FINISHED -5
#define ESP_ERR_INVALID_ARG -6
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static struct { atomic_bool degraded; atomic_uint ready_count; uint32_t next_index; } s;
static atomic_bool s_quarantine_recheck_pending = ATOMIC_VAR_INIT(true);
static atomic_bool s_quarantine_files_present, s_card_pending_upload;
static bool enabled(void) { return true; }
static bool runtime_initialized(void) { return true; }
static bool busy;
static bool nuwax_offline_audio_storage_busy(void) { return busy; }
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static void nuwax_sd_card_mark_io_fault(int err) { (void)err; }
#include "replay_directory_host.inc"
#include "archive_inventory_host.inc"
static void touch_file(const char *path) { FILE *f=fopen(path,"wb");assert(f);fputs("retained",f);assert(fclose(f)==0); }
int main(void)
{
    assert(archive_bucket_name("0000000a") && archive_bucket_name("0000000A"));
    snprintf(legacy,sizeof(legacy),"archive-legacy-%lu",(unsigned long)GetCurrentProcessId());
    snprintf(current,sizeof(current),"archive-new-%lu",(unsigned long)GetCurrentProcessId());
    assert(archive_make_directory(legacy)==0 && archive_make_directory(current)==0);
    // A fresh boot's quarantine recheck must not create an empty network job.
    assert(atomic_load(&s_quarantine_recheck_pending));
    assert(scan_ready_count()==0 && !nuwax_offline_audio_has_ready());
    char quarantine[160];
    assert(compose_child_path(quarantine,sizeof(quarantine),"boot_only.ready.bad"));
    touch_file(quarantine);
    assert(scan_ready_count()==0 && nuwax_offline_audio_has_ready());
    atomic_store(&s_quarantine_recheck_pending,false);
    assert(!nuwax_offline_audio_has_ready()); // Completed pass does not loop on a damaged file.
    assert(remove(quarantine)==0 && scan_ready_count()==0);
    atomic_store(&s_quarantine_recheck_pending,true);
    assert(!nuwax_offline_audio_has_ready());
    atomic_store(&s_card_pending_upload,true);
    assert(nuwax_offline_audio_has_ready()); // A pending session-end still needs an ACK.
    atomic_store(&s_card_pending_upload,false);
    char paths[74][160], part[160], ready[160];
    const char *old[]={"offline_old_00000090.ready","offline_old_00000091.part","offline_old_00000092.ready.bad"};
    for(unsigned i=0;i<3;i++) { assert(compose_child_path(paths[i],sizeof(paths[i]),old[i]));touch_file(paths[i]); }
    for(unsigned i=0;i<70;i++) {
        assert(archive_new_paths(i,"meeting_test",part,sizeof(part),ready,sizeof(ready)));
        strcpy(paths[i+3],ready);touch_file(ready);
    }
    assert(strstr(paths[34],"/00000000/") && strstr(paths[35],"/00000001/"));
    assert(archive_new_paths(70,"meeting_test",part,sizeof(part),ready,sizeof(ready)));
    strcpy(paths[73],part);touch_file(part); // Stale .part must participate in recovery traversal.
    char card[160];assert(compose_child_path(card,sizeof(card),"card_retained.ready"));touch_file(card);
    // Kept for diagnosis/another owner: count its space, never schedule legacy upload.
    assert(refresh_next_segment_index()==ESP_OK && s.next_index==93 && scan_used_bytes()==75U*8U && scan_ready_count()==71);
    assert(atomic_load(&s_quarantine_files_present));
    atomic_store(&s.ready_count,scan_ready_count());
    assert(nuwax_offline_audio_has_ready());
    archive_dir_t walk;assert(archive_dir_open(&walk));unsigned files=0,parts=0;
    struct dirent *e;
    while((e=archive_dir_read(&walk))) {
        if(e->d_name[0]=='.')continue;
        char full[160];assert(compose_child_path(full,sizeof(full),e->d_name));
        struct stat st;assert(stat(full,&st)==0 && st.st_size==8);files++;
        if(strstr(e->d_name,".part"))parts++;
    }
    assert(!walk.error && files==75 && parts==2);archive_dir_close(&walk);
    char after[128]="",name[128];bool found;unsigned count=0;
    do { assert(find_next_ready_name(after,name,sizeof(name),&found)==ESP_OK);
         if(found) {assert(strcmp(name,after)>0);strcpy(after,name);count++;} } while(found);
    assert(count==72); // 70 new, one legacy ready, one quarantined legacy ready.
    busy=true;assert(find_next_ready_name("",name,sizeof(name),&found)==ESP_ERR_NOT_FINISHED);busy=false;
    assert(!compose_child_path(part,sizeof(part),"v2/../../escape"));
    assert(!compose_child_path(part,sizeof(part),"v2/00000000/../escape"));
    assert(!compose_child_path(part,5,"v2/00000000/a.ready"));
    archive_remove_empty_bucket(paths[3]);assert(stat(paths[3],&(struct stat){0})==0); // No live file removal.
    for(unsigned i=0;i<74;i++) {assert(remove(paths[i])==0);archive_remove_empty_bucket(paths[i]);}
    assert(remove(card)==0);
    atomic_store(&s.ready_count,scan_ready_count());
    assert(!atomic_load(&s_quarantine_files_present) && !nuwax_offline_audio_has_ready());
    assert(rmdir(current)==0 && rmdir(legacy)==0);
    assert(!archive_dir_open(&walk) && walk.error); // Missing media is not an empty successful scan.
    atomic_store(&s.ready_count,2);
    atomic_store(&s_quarantine_files_present,true);
    assert(scan_ready_count()==2 && atomic_load(&s_quarantine_files_present));
    assert(atomic_load(&s.degraded) && nuwax_offline_audio_has_ready());
    puts("PASS: production mixed legacy/sharded traversal, bounded buckets, .part/.bad retention, paginated replay, cancellation, path bounds and empty-only cleanup");
}
