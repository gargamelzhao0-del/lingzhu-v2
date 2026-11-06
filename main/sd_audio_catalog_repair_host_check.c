#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
static const char *repair_open_fault;
static FILE *repair_fopen(const char *path, const char *mode)
{
    if (repair_open_fault && strstr(path, repair_open_fault)) { errno = EIO; return NULL; }
    return fopen(path, mode);
}
#define fopen repair_fopen
#include "sd_audio_catalog.c"
#undef fopen
#include <assert.h>

static void put32(uint8_t *p, uint32_t n) { memcpy(p, &n, 4U); }
static void put64(uint8_t *p, uint64_t n) { memcpy(p, &n, 8U); }
static void enter_case(const char *name)
{
    assert(mkdir(name, 0775) == 0 && chdir(name) == 0);
    assert(sd_audio_catalog_init() == ESP_OK);
}
static sd_audio_session_t make(const char *id, unsigned mode, unsigned count, bool paged)
{
    sd_audio_session_t s = {.mode=mode,.boot_id=7,.start_ms=1000,.complete=1};
    snprintf(s.session_id,sizeof(s.session_id),"%s",id); memset(s.owner,0x31,32);
    assert(sd_audio_catalog_create(&s)==ESP_OK);
    if(paged) assert(sd_audio_catalog_enable_retention(id)==ESP_OK);
    for(unsigned i=0;i<count;i++) {
        uint8_t bytes[96+666+32]={0};
        memcpy(bytes,"NWXOFF1",7);bytes[8]=1;bytes[10]=96;bytes[12]=(uint8_t)mode;bytes[13]=1;
        put32(bytes+16,16000);bytes[20]=20;put32(bytes+24,i);put64(bytes+28,1000+i*20U);
        strcpy((char*)bytes+36,id);
        put32(bytes+96,662);bytes[100]='N';bytes[101]='A';put32(bytes+114,1000+i*20U);
        uint8_t *footer=bytes+96+666;memcpy(footer,"NWXEND1",7);footer[8]=1;footer[10]=32;
        put32(footer+12,1);put64(footer+16,662);put32(footer+24,checksum(bytes+100,662));footer[28]=1;
        sd_audio_segment_t part={.start_ms=1000+i*20U,.end_ms=1020+i*20U};
        snprintf(part.path,sizeof(part.path),"%s_%u.ready",id,i);
        assert(sd_audio_catalog_reserve(&s,&part)==ESP_OK);
        FILE *file=fopen(part.path,"wb");assert(file);
        assert(fwrite(bytes,1,sizeof(bytes),file)==sizeof(bytes) && fclose(file)==0);
        part.sealed=1;part.total_bytes=sizeof(bytes);part.payload_crc=checksum(bytes+100,662);
        assert(sd_audio_catalog_write(id,&part)==ESP_OK);
    }
    s.ended=1;s.ended_ms=1000+count*20U;assert(sd_audio_catalog_save(&s)==ESP_OK);
    return s;
}
static void poison(sd_audio_session_t *s, uint32_t cursor, bool done)
{
    s->complete=0;s->done=done;s->first_pending=cursor;
    char path[128];assert(catalog_path(path,s->session_id,"session"));
    FILE *f=fopen(path,"r+b");assert(f);
    assert(write_pair(f,0,s,sizeof(*s),offsetof(sd_audio_session_t,crc))==ESP_OK && fclose(f)==0);
}
static uint32_t audio_crc(const char *id, unsigned index)
{
    char path[128];snprintf(path,sizeof(path),"%s_%u.ready",id,index);
    uint8_t data[794];FILE *f=fopen(path,"rb");assert(f);
    assert(fread(data,1,sizeof(data),f)==sizeof(data) && fclose(f)==0);
    return checksum(data,sizeof(data));
}
static void recovered(const char *id, unsigned count, uint32_t cursor)
{
    sd_audio_session_t s;assert(sd_audio_catalog_load(id,&s)==ESP_OK);
    assert(!s.done && !s.complete && s.segment_count==count && s.first_pending==cursor);
    for(unsigned i=cursor;i<count;i++) {
        sd_audio_segment_t part;assert(sd_audio_catalog_read(id,i,&part)==ESP_OK);
        assert(part.sealed && !part.acknowledged && !part.missing);
    }
    bool pending=false;assert(sd_audio_catalog_has_pending(&pending)==ESP_OK && pending);
    sd_audio_session_t picked;sd_audio_segment_t part;bool have=false,file=false;
    assert(sd_audio_catalog_next(s.owner,8,99999,0,NULL,false,s.mode,&picked,&part,&have,&file)==ESP_OK);
    assert(have && file && part.index==cursor && !strcmp(picked.session_id,id));
    uint8_t other[32]={0};have=file=false;
    assert(sd_audio_catalog_next(other,8,99999,0,NULL,false,s.mode,&picked,&part,&have,&file)==ESP_OK && !have);
}
int main(void)
{
    enter_case("stale_snapshot");
    sd_audio_session_t old=make("meeting_stale",0,2,false),stale=old;
    stale.segment_count=1;assert(sd_audio_catalog_save(&stale)==ESP_ERR_INVALID_STATE);
    stale=old;stale.ended=0;assert(sd_audio_catalog_save(&stale)==ESP_ERR_INVALID_STATE);
    stale=old;stale.first_pending=3;assert(sd_audio_catalog_save(&stale)==ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_load(old.session_id,&stale)==ESP_OK && stale.segment_count==2 && stale.ended);
    assert(chdir("..")==0);
    for(unsigned variant=0;variant<4;variant++) {
        char name[24];snprintf(name,sizeof(name),"repair_%u",variant);enter_case(name);
        sd_audio_session_t s=make("meeting_fresh",0,10,false);
        uint32_t crc=audio_crc(s.session_id,0);
        poison(&s,variant==1?9999:variant==3?4:0,variant!=3);
        if(variant==2) assert(rename("catalog/meeting_fresh.session","catalog/meeting_fresh.session.bad.0")==0);
        unsigned before=catalog_stats.repaired_sessions;
        assert(sd_audio_catalog_recover(8)==ESP_OK);
        recovered(s.session_id,10,0);
        assert(audio_crc(s.session_id,0)==crc && catalog_stats.repaired_sessions==before+1);
        if(variant==2) {struct stat st;assert(stat("catalog/meeting_fresh.session.bad.0",&st)==0);}
        assert(sd_audio_catalog_recover(9)==ESP_OK && catalog_stats.repaired_sessions==before+1);
        assert(chdir("..")==0);
    }
    puts("PASS repair: done/cursor poisoning, quarantined evidence, exact audio bytes, immutable owner, zero fake ACKs and idempotent restart");
    enter_case("acked");sd_audio_session_t s=make("meeting_acked",0,2,false);
    assert(sd_audio_catalog_ack(s.session_id,0)==ESP_OK && sd_audio_catalog_ack(s.session_id,1)==ESP_OK);
    assert(sd_audio_catalog_load(s.session_id,&s)==ESP_OK);s.done=1;
    assert(sd_audio_catalog_save(&s)==ESP_OK);unsigned before=catalog_stats.repaired_sessions;
    assert(sd_audio_catalog_recover(8)==ESP_OK);
    assert(sd_audio_catalog_load(s.session_id,&s)==ESP_OK && s.done && s.complete);
    bool pending=true;assert(sd_audio_catalog_has_pending(&pending)==ESP_OK && !pending);
    assert(catalog_stats.repaired_sessions==before && chdir("..")==0);
    puts("PASS repair: genuinely ACKed scene remains done even when a source file still exists");
    enter_case("foreign_file");s=make("meeting_wrong",0,1,false);poison(&s,0,true);
    FILE *f=fopen("meeting_wrong_0.ready","r+b");assert(f);
    assert(fseek(f,36,SEEK_SET)==0 && fwrite("other_scene",1,12,f)==12 && fclose(f)==0);
    before=catalog_stats.repaired_sessions;assert(sd_audio_catalog_recover(8)==ESP_OK);
    assert(sd_audio_catalog_load(s.session_id,&s)==ESP_OK && s.done && catalog_stats.repaired_sessions==before);
    assert(chdir("..")==0);
    puts("PASS repair: another scene's archive cannot authorize unretiring metadata");
    enter_case("paged_monitor");s=make("monitor_paged",1,260,true);
    for(unsigned i=0;i<128;i++) assert(sd_audio_catalog_mark_missing(s.session_id,i)==ESP_OK);
    assert(sd_audio_catalog_load(s.session_id,&s)==ESP_OK);s.first_pending=128;
    assert(sd_audio_catalog_save(&s)==ESP_OK);struct stat st;
    assert(stat("catalog/monitor_paged.index.00000000",&st)!=0);
    poison(&s,128,true);assert(sd_audio_catalog_recover(8)==ESP_OK);
    recovered(s.session_id,260,128);assert(stat("catalog/monitor_paged.index.00000000",&st)!=0);
    puts("PASS repair: paged monitoring resumes at retained data and never recreates retired pages");
    for(unsigned variant=0;variant<5;variant++) {
        char name[32];snprintf(name,sizeof(name),"middle_control_%u",variant);enter_case(name);
        s=make("middle_evidence",1,8,false);
        const uint32_t original=audio_crc(s.session_id,3);
        for(unsigned i=0;i<8;i++)if(i!=3 || variant==4)
            assert(sd_audio_catalog_ack(s.session_id,i)==ESP_OK);
        assert(sd_audio_catalog_load(s.session_id,&s)==ESP_OK);
        if(variant==0){s.first_pending=3;assert(sd_audio_catalog_save(&s)==ESP_OK);}
        else if(variant==4){s.done=1;assert(sd_audio_catalog_save(&s)==ESP_OK);}
        else poison(&s,variant==2?6:8,variant!=2);
        if(variant==3)assert(rename("catalog/middle_evidence.session","catalog/middle_evidence.session.bad.0")==0);
        before=catalog_stats.repaired_sessions;
        assert(sd_audio_catalog_recover(8)==ESP_OK);
        assert(sd_audio_catalog_load(s.session_id,&s)==ESP_OK);
        assert(audio_crc(s.session_id,3)==original);
        bool pending=false;assert(sd_audio_catalog_has_pending(&pending)==ESP_OK);
        sd_audio_segment_t part;assert(sd_audio_catalog_read(s.session_id,3,&part)==ESP_OK);
        if(variant==4)assert(s.done && !pending && part.acknowledged && catalog_stats.repaired_sessions==before);
        else {
            assert(!s.done && pending && !part.acknowledged);
            sd_audio_session_t picked;bool scene=false,file=false;
            assert(sd_audio_catalog_next(s.owner,8,99999,0,NULL,false,1,&picked,&part,&scene,&file)==ESP_OK);
            assert(scene && file && part.index==3);
            if(variant)assert(catalog_stats.repaired_sessions==before+1 && !s.complete);
            else assert(catalog_stats.repaired_sessions==before && s.complete);
        }
        assert(chdir("..")==0);
    }
    puts("PASS repair: ACKed endpoints cannot hide the middle; normal and all-ACKed controls unchanged; cursor-only and quarantined recovery retain exact audio");
    for(unsigned variant=0;variant<2;variant++) {
        char name[32];snprintf(name,sizeof(name),"read_failure_%u",variant);enter_case(name);
        s=make("io_evidence",1,4,false);
        for(unsigned i=0;i<4;i++)if(i!=1)assert(sd_audio_catalog_ack(s.session_id,i)==ESP_OK);
        assert(sd_audio_catalog_load(s.session_id,&s)==ESP_OK);poison(&s,4,true);
        const uint32_t original=audio_crc(s.session_id,1);
        before=catalog_stats.repaired_sessions;
        repair_open_fault=variant ? "io_evidence_1.ready" : "io_evidence.index";
        assert(sd_audio_catalog_recover(8)==ESP_FAIL);
        repair_open_fault=NULL;
        assert(sd_audio_catalog_load(s.session_id,&s)==ESP_OK && s.done && s.first_pending==4);
        assert(catalog_stats.repaired_sessions==before && audio_crc(s.session_id,1)==original);
        sd_audio_segment_t part;assert(sd_audio_catalog_read(s.session_id,1,&part)==ESP_OK && !part.acknowledged);
        assert(sd_audio_catalog_recover(8)==ESP_OK);
        assert(sd_audio_catalog_load(s.session_id,&s)==ESP_OK && !s.done && !s.complete);
        assert(catalog_stats.repaired_sessions==before+1 && audio_crc(s.session_id,1)==original);
        assert(chdir("..")==0);
    }
    puts("PASS repair: index/audio I/O failures retain metadata and source; later recovery restores the unconfirmed file without a fake ACK");
    return 0;
}
