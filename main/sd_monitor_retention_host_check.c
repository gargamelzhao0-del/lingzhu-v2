/* Real catalog/files, with deterministic interruption at each durable step. */
#include <assert.h>
#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned fail_sync_at, sync_count, fail_unlink;
static int checked_sync(int fd)
{
    if (++sync_count == fail_sync_at) { errno = EIO; return -1; }
    return _commit(fd);
}
static int checked_remove(const char *path)
{
    if (fail_unlink && strstr(path, ".ready")) { --fail_unlink; errno = EIO; return -1; }
    return remove(path);
}
#define _commit checked_sync
#define remove checked_remove
#include "sd_audio_catalog.c"
#undef _commit
#undef remove
#include "sd_audio_retention_policy.h"

static unsigned file_order;
static bool exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
static sd_audio_session_t scene(const char *id, unsigned mode, unsigned owner, bool pages)
{
    sd_audio_session_t value = {.mode=mode,.complete=1,.boot_id=1};
    strcpy(value.session_id,id); memset(value.owner,owner,32);
    assert(sd_audio_catalog_create(&value)==ESP_OK);
    if(pages) assert(sd_audio_catalog_enable_retention(id)==ESP_OK);
    return value;
}
static sd_audio_segment_t audio(sd_audio_session_t *s, bool sealed)
{
    sd_audio_segment_t seg={.start_ms=1000U+s->segment_count*20U,.total_bytes=794};
    seg.end_ms=seg.start_ms+20;
    snprintf(seg.path,sizeof(seg.path),"audio/card_%s_%08u.ready",s->session_id,file_order++);
    assert(sd_audio_catalog_reserve(s,&seg)==ESP_OK);
    seg.sealed=sealed;
    assert(sd_audio_catalog_write(s->session_id,&seg)==ESP_OK);
    unsigned char data[794]={0};
    memcpy(data,"NWXOFF1",7); data[8]=1; data[10]=96; data[12]=s->mode; data[13]=1;
    data[22]=2; memcpy(data+24,&seg.index,4); strcpy((char *)data+36,s->session_id);
    uint32_t len=662, stamp=(uint32_t)seg.start_ms;
    memcpy(data+96,&len,4); memcpy(data+100,"NA",2); memcpy(data+114,&stamp,4);
    memcpy(data+762,"NWXEND1",7); data[790]=1;
    FILE *file=fopen(seg.path,"wb"); assert(file);
    assert(fwrite(data,1,sizeof(data),file)==sizeof(data) && fclose(file)==0);
    return seg;
}
static esp_err_t reclaim(const sd_audio_session_t *s, uint64_t *bytes, char path[128])
{
    esp_err_t err;
    unsigned calls=0;
    do { err=sd_audio_catalog_reclaim(s->owner,bytes,path); assert(++calls<1000); }
    while(err==ESP_ERR_NOT_FINISHED);
    return err;
}
static void protections(void)
{
    sd_audio_session_t meeting=scene("meeting",0,1,false), foreign=scene("foreign",1,2,false);
    sd_audio_session_t blocked=scene("blocked",1,1,true), current=scene("current",1,1,true);
    sd_audio_segment_t a=audio(&meeting,true), b=audio(&foreign,true), c=audio(&blocked,true);
    assert(sd_audio_catalog_block(blocked.session_id)==ESP_OK);
    sd_audio_segment_t old=audio(&current,true), pin=audio(&current,true), latest=audio(&current,true), open=audio(&current,false);
    assert(sd_audio_catalog_pin(current.session_id,pin.index)==ESP_OK);
    assert(sd_audio_catalog_init()==ESP_ERR_NOT_FINISHED && sd_audio_catalog_replay_pinned());
    uint64_t bytes; char path[128];
    assert(reclaim(&current,&bytes,path)==ESP_OK && bytes==794 && !strcmp(path,old.path));
    assert(!exists(old.path) && exists(pin.path));
    assert(reclaim(&current,&bytes,path)==ESP_OK && !strcmp(path,latest.path));
    assert(reclaim(&current,&bytes,path)==ESP_ERR_NOT_FOUND);
    assert(exists(a.path) && exists(b.path) && exists(c.path) && exists(pin.path) && exists(open.path));
    sd_audio_catalog_unpin();
    assert(reclaim(&current,&bytes,path)==ESP_OK && !strcmp(path,pin.path));
    sd_audio_session_t saved; assert(sd_audio_catalog_load(current.session_id,&saved)==ESP_OK && !saved.complete && !saved.ended);
    sd_audio_segment_t record; assert(sd_audio_catalog_read(current.session_id,0,&record)==ESP_OK && record.missing && !record.acknowledged);
    puts("PASS: oldest-first, meetings/foreign/blocked/open files protected, replay lease, missing not ACK, scene remains active");
}
static void fault_case(unsigned fail_at, bool unlink_failure)
{
    char id[48]; snprintf(id,sizeof(id),"fault_%u_%u",fail_at,unlink_failure);
    sd_audio_session_t s=scene(id,1,10+fail_at+unlink_failure*10,true);
    sd_audio_segment_t a=audio(&s,true);
    sync_count=0; fail_sync_at=fail_at; fail_unlink=unlink_failure;
    uint64_t bytes; char path[128];
    assert(reclaim(&s,&bytes,path)!=ESP_OK);
    fail_sync_at=fail_unlink=0;
    if(fail_at<=2 || unlink_failure) assert(exists(a.path));
    sd_audio_session_t saved;
    assert(sd_audio_catalog_load(id,&saved)==ESP_OK);
    if(!exists(a.path)) assert(!saved.complete);
    /* Reset volatile caches. Any interrupted unlink must finish before skip. */
    assert(sd_audio_catalog_recover(2)==ESP_OK);
    assert(sd_audio_catalog_load(id,&saved)==ESP_OK && !saved.complete);
    sd_audio_segment_t record; assert(sd_audio_catalog_read(id,0,&record)==ESP_OK);
    if(record.missing) assert(!record.acknowledged);
    if(exists(a.path)) assert(reclaim(&s,&bytes,path)==ESP_OK);
    assert(!exists(a.path));
}
static void bounded_metadata(void)
{
    sd_audio_session_t s=scene("long_monitor",1,77,true);
    uint64_t bytes; char path[128];
    for(unsigned i=0;i<1025;++i) {
        sd_audio_segment_t segment=audio(&s,true);
        assert(segment.index==i);
        if(i>=3) assert(reclaim(&s,&bytes,path)==ESP_OK && bytes==794);
    }
    sd_audio_session_t saved; assert(sd_audio_catalog_load(s.session_id,&saved)==ESP_OK);
    assert(saved.segment_count==1025 && saved.first_pending==1022 && !saved.complete && !saved.ended);
    for(unsigned i=0;i<7;++i) {
        snprintf(path,sizeof(path),"catalog/long_monitor.index.%08x",i); assert(!exists(path));
    }
    assert(exists("catalog/long_monitor.index.00000007") && exists("catalog/long_monitor.index.00000008"));
    /* A stale in-memory owner must not restore complete=true after retention. */
    s.complete=1; s.ended=1; assert(sd_audio_catalog_save(&s)==ESP_OK && !s.complete && s.first_pending==1022);
    assert(sd_audio_catalog_recover(2)==ESP_OK);
    bool found,file; sd_audio_segment_t part;
    assert(sd_audio_catalog_next(s.owner,2,99999,0,NULL,false,1,&saved,&part,&found,&file)==ESP_OK && found && file && part.index==1022);
    puts("PASS: 1025 files, latest three retained, monotonic indexes, bounded page files, restart and incomplete scene upload");
}
static void policies(void)
{
    const uint64_t gib=1024ULL*1024*1024;
    for(unsigned n=8;n<=64;n*=2) {
        uint64_t reserve=sd_monitor_reserve_bytes(n*gib);
        assert(reserve>=16U*1024*1024 && reserve<=128U*1024*1024);
        assert(sd_monitor_required_bytes(n*gib,4194304,false)==reserve+4194304+65536);
        assert(sd_monitor_required_bytes(n*gib,4194304,true)>sd_monitor_required_bytes(n*gib,4194304,false));
    }
    assert(sd_monitor_unlimited(true,true) && !sd_monitor_unlimited(true,false) && !sd_monitor_unlimited(false,true));
}
static void many_scenes(void)
{
    sd_audio_session_t list[24];
    for(unsigned i=0;i<24;++i) {
        char id[48]; snprintf(id,sizeof(id),"many_%02u",i);
        list[i]=scene(id,1,88,true);
    }
    sd_audio_segment_t oldest=audio(&list[23],true);
    for(unsigned i=0;i<23;++i) (void)audio(&list[i],true);
    uint64_t bytes; char path[128];
    assert(sd_audio_catalog_reclaim(list[0].owner,&bytes,path)==ESP_ERR_NOT_FINISHED && !bytes);
    assert(reclaim(&list[0],&bytes,path)==ESP_OK && !strcmp(path,oldest.path));
    puts("PASS: bounded multi-scene scan resumes across yields and still finds the globally oldest file");
}
static void wire_boundary(void)
{
    sd_audio_session_t s=scene("wire_limit",1,99,true);
    s.first_pending=s.segment_count=999999;
    assert(sd_audio_catalog_save(&s)==ESP_OK);
    sd_audio_segment_t part={0};
    assert(sd_audio_catalog_reserve(&s,&part)==ESP_OK && part.index==999999 && s.segment_count==1000000);
    assert(sd_audio_catalog_reserve(&s,&part)==ESP_ERR_INVALID_STATE && s.segment_count==1000000);
    s.done=s.ended=1; s.complete=0; assert(sd_audio_catalog_save(&s)==ESP_OK);
    puts("PASS: existing wire index ceiling never creates an unuploadable archive");
}
static void fallback_missing(void)
{
    sd_audio_session_t s=scene("fallback_missing",1,100,true);
    sd_audio_segment_t seg=audio(&s,true);
    char manifest[128]; assert(catalog_path(manifest,s.session_id,"session"));
    unsigned char prior[2*sizeof(s)];
    FILE *f=fopen(manifest,"rb"); assert(f);
    assert(fread(prior,1,sizeof(prior),f)==sizeof(prior) && fclose(f)==0);
    uint64_t bytes; char path[128];
    assert(reclaim(&s,&bytes,path)==ESP_OK && !exists(seg.path));
    /* Model recovery choosing the old valid manifest slot, while the file
     * retirement record survived. Never restore a false complete flag. */
    f=fopen(manifest,"wb"); assert(f);
    assert(fwrite(prior,1,sizeof(prior),f)==sizeof(prior) && fclose(f)==0);
    assert(sd_audio_catalog_recover(2)==ESP_OK);
    assert(sd_audio_catalog_load(s.session_id,&s)==ESP_OK && !s.complete);
    puts("PASS: older manifest fallback plus durable retired record remains incomplete after recovery");
}
static void corrupt_latest_manifest(const char *id)
{
    sd_audio_session_t current;
    assert(sd_audio_catalog_load(id,&current)==ESP_OK);
    char path[128]; assert(catalog_path(path,id,"session"));
    FILE *f=fopen(path,"r+b"); assert(f);
    const long position=(long)((current.generation&1U)*sizeof(current)+offsetof(sd_audio_session_t,crc));
    assert(fseek(f,position,SEEK_SET)==0);
    uint32_t bad=current.crc^1U;
    assert(fwrite(&bad,1,sizeof(bad),f)==sizeof(bad) && fclose(f)==0);
}
static void retirement_manifest_fallback(void)
{
    sd_audio_session_t s=scene("retirement_fallback",1,101,true);
    sd_audio_segment_t seg=audio(&s,true);
    s.ended=1;
    assert(sd_audio_catalog_save(&s)==ESP_OK);
    uint64_t bytes; char path[128];
    /* Persist the exact interrupted state: incomplete manifest and retirement
     * intent written, then reset before the unlink phase. This is also the
     * state retained by 7.0.172 after an unlink failure. */
    s.complete=0;
    assert(sd_audio_catalog_save(&s)==ESP_OK);
    seg.missing=SD_AUDIO_SEGMENT_RETIRING;
    assert(sd_audio_catalog_write(s.session_id,&seg)==ESP_OK && exists(seg.path));
    sd_audio_session_t current;
    assert(sd_audio_catalog_load(s.session_id,&current)==ESP_OK && !current.complete);
    /* Corrupt only the newest slot. The other valid manifest predates the
     * retirement intent, while the durable RETIRING segment survives. */
    corrupt_latest_manifest(s.session_id);
    assert(sd_audio_catalog_load(s.session_id,&current)==ESP_OK && current.complete);
    /* A failure while repairing the manifest must still protect the source. */
    sync_count=0; fail_sync_at=1;
    assert(reclaim(&s,&bytes,path)==ESP_FAIL && exists(seg.path));
    sync_count=0; /* A visible but unconfirmed bit is not durable proof. */
    assert(reclaim(&s,&bytes,path)==ESP_FAIL && exists(seg.path));
    fail_sync_at=0;
    assert(reclaim(&s,&bytes,path)==ESP_OK && !exists(seg.path));
    assert(sd_audio_catalog_load(s.session_id,&current)==ESP_OK && !current.complete);
    puts("PASS: runtime retry repairs old valid manifest before completing durable retirement");
}
static void missing_page_fallback(void)
{
    sd_audio_session_t s=scene("missing_page_fallback",1,102,true);
    sd_audio_segment_t last={0};
    for (unsigned i=0;i<128;++i) {
        last=audio(&s,true);
        if (i<127) {
            assert(sd_audio_catalog_ack(s.session_id,i)==ESP_OK);
            assert(remove(last.path)==0);
        }
    }
    s.first_pending=127; s.ended=1;
    assert(sd_audio_catalog_save(&s)==ESP_OK);
    assert(sd_audio_catalog_mark_missing(s.session_id,127)==ESP_OK);
    corrupt_latest_manifest(s.session_id);
    sd_audio_session_t current;
    assert(sd_audio_catalog_load(s.session_id,&current)==ESP_OK && current.complete);
    uint64_t bytes; char path[128];
    assert(reclaim(&s,&bytes,path)==ESP_ERR_NOT_FOUND && !bytes && exists(last.path));
    assert(sd_audio_catalog_load(s.session_id,&current)==ESP_OK && !current.complete && current.first_pending==128);
    assert(!exists("catalog/missing_page_fallback.index.00000000"));
    assert(sd_audio_catalog_recover(2)==ESP_OK);
    assert(sd_audio_catalog_load(s.session_id,&current)==ESP_OK && !current.complete);
    puts("PASS: missing evidence survives runtime cursor advance, page removal and restart after manifest fallback");
}
int main(void)
{
    assert(mkdir("audio",0775)==0);
    assert(sd_audio_catalog_init()==ESP_OK);
    policies(); protections();
    for(unsigned i=1;i<=5;++i) fault_case(i,false);
    fault_case(0,true);
    puts("PASS: failures before/after each fsync and unlink; reset never reports overwritten audio as complete");
    bounded_metadata();
    many_scenes();
    wire_boundary();
    fallback_missing();
    retirement_manifest_fallback();
    missing_page_fallback();
    return 0;
}
