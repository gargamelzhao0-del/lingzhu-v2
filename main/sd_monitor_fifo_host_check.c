#include <assert.h>
#include "sd_audio_catalog.c"
#include "sd_audio_monitor_fifo.h"

static unsigned serial_case;
static const uint8_t owner[32]={0x31};
static void enter(void)
{
    char name[40];snprintf(name,sizeof(name),"case_%u",serial_case++);
    assert(mkdir(name,0775)==0 && chdir(name)==0);
    assert(mkdir("audio",0775)==0 && mkdir("audio2",0775)==0);
    assert(sd_audio_catalog_init()==ESP_OK);
}
static void leave(void){assert(chdir("..")==0);}
static void put32(uint8_t *p,uint32_t v){memcpy(p,&v,4);}
static void put64(uint8_t *p,uint64_t v){memcpy(p,&v,8);}
static sd_audio_session_t make_scene(const char *id,unsigned mode,unsigned count,
    uint32_t physical,uint32_t boot,uint64_t started,bool ended,bool pages)
{
    sd_audio_session_t s={.mode=mode,.boot_id=boot,.start_ms=started,.complete=1};
    strcpy(s.session_id,id);memcpy(s.owner,owner,32);
    assert(sd_audio_catalog_create(&s)==ESP_OK);
    if(pages)assert(sd_audio_catalog_enable_retention(id)==ESP_OK);
    for(unsigned i=0;i<count;i++){
        uint8_t bytes[794]={0};memcpy(bytes,"NWXOFF1",7);bytes[8]=1;bytes[10]=96;bytes[12]=mode;bytes[13]=1;
        put32(bytes+16,16000);bytes[20]=20;bytes[22]=2;put32(bytes+24,i);put64(bytes+28,started+i*20);
        strcpy((char*)bytes+36,id);put32(bytes+96,662);bytes[100]='N';bytes[101]='A';
        put32(bytes+114,(uint32_t)started+i*20);
        uint8_t *footer=bytes+96+666;memcpy(footer,"NWXEND1",7);footer[8]=1;footer[10]=32;
        put32(footer+12,1);put64(footer+16,662);put32(footer+24,checksum(bytes+100,662));footer[28]=1;
        sd_audio_segment_t part={.start_ms=started+i*20,.end_ms=started+i*20+20};
        snprintf(part.path,sizeof(part.path),"audio/card_%s_%08u.ready",id,physical+i);
        assert(sd_audio_catalog_reserve(&s,&part)==ESP_OK);
        FILE *f=fopen(part.path,"wb");assert(f && fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes) && fclose(f)==0);
        part.sealed=1;part.total_bytes=sizeof(bytes);part.payload_crc=checksum(bytes+100,662);part.realtime=i%2==0;
        assert(sd_audio_catalog_write(id,&part)==ESP_OK);
    }
    s.ended=ended;s.ended_ms=ended?started+count*20:0;
    assert(sd_audio_catalog_save(&s)==ESP_OK);return s;
}
static esp_err_t next(sd_audio_session_t *s,sd_audio_segment_t *p,bool *found,bool *file,bool *wait)
{
    for(unsigned i=0;i<10000;i++){
        esp_err_t err=sd_monitor_fifo_next(owner,s,p,found,file,wait);
        if(err!=ESP_ERR_NOT_FINISHED)return err;
    }
    assert(!"bounded selection failed to complete");return ESP_FAIL;
}
static void expect_file(const char *id,unsigned index)
{
    sd_audio_session_t s;sd_audio_segment_t p;bool found,file,wait;
    assert(next(&s,&p,&found,&file,&wait)==ESP_OK && found && file && !wait);
    assert(!strcmp(s.session_id,id) && p.index==index);
    assert(sd_audio_catalog_pin(id,index)==ESP_OK);sd_audio_catalog_unpin();
}
static void expect_end(const char *id, bool finish)
{
    sd_audio_session_t s;sd_audio_segment_t p;bool found,file,wait;
    assert(next(&s,&p,&found,&file,&wait)==ESP_OK && found && !file && !wait && !strcmp(s.session_id,id));
    bool ready=false;esp_err_t err;
    do{err=sd_monitor_fifo_end_ready(&s,&ready);}while(err==ESP_ERR_NOT_FINISHED);
    assert(err==ESP_OK && ready);
    if(finish)assert(sd_monitor_fifo_finish(&s)==ESP_OK && s.done);
}
int main(void)
{
    enter();
    sd_audio_session_t a=make_scene("older",1,4,100,0xfffffff0U,900000,true,false);
    sd_audio_session_t b=make_scene("newer",1,2,200,1,1000,false,false);
    for(unsigned i=0;i<4;i++){
        expect_file(a.session_id,i);
        sd_audio_segment_t p;assert(sd_audio_catalog_read(a.session_id,i,&p)==ESP_OK);
        assert(sd_audio_segment_realtime(&a,&p,123,9999999999ULL,9999999999ULL)==(i%2==0));
        assert(sd_audio_catalog_ack(a.session_id,i)==ESP_OK);
    }
    expect_end(a.session_id,false); /* lost end ACK: B cannot overtake. */
    expect_end(a.session_id,false);
    sd_monitor_fifo_reset();expect_end(a.session_id,true);
    expect_file(b.session_id,0);assert(sd_audio_catalog_ack(b.session_id,0)==ESP_OK);
    expect_file(b.session_id,1);assert(sd_audio_catalog_ack(b.session_id,1)==ESP_OK);
    sd_audio_session_t s;sd_audio_segment_t p;bool found,file,wait;
    assert(next(&s,&p,&found,&file,&wait)==ESP_OK && !found && wait);
    bool ready=true;assert(sd_monitor_fifo_end_ready(&b,&ready)==ESP_OK && !ready);
    assert(sd_audio_catalog_load(b.session_id,&b)==ESP_OK);b.ended=1;b.ended_ms=2000;
    assert(sd_audio_catalog_save(&b)==ESP_OK);expect_end(b.session_id,true);
    puts("PASS FIFO: old before active-new across boot clocks; file ACK order; stable source class; end ACK gate; active no-file is not ended; reboot");leave();

    enter();a=make_scene("blocked_old",1,2,10,5,8000,true,false);
    b=make_scene("young",1,1,20,6,100,true,false);
    sd_audio_session_t meeting=make_scene("meeting_independent",0,1,15,5,9000,true,false);
    expect_file(a.session_id,0);assert(sd_audio_catalog_defer(&a)==ESP_OK);
    expect_file(a.session_id,0);assert(sd_audio_catalog_block(a.session_id)==ESP_OK);
    assert(next(&s,&p,&found,&file,&wait)==ESP_OK && !found && wait);
    assert(sd_audio_catalog_next(owner,6,10000,0,NULL,false,0,&s,&p,&found,&file)==ESP_OK && found && file);
    assert(!strcmp(s.session_id,meeting.session_id));
    puts("PASS FIFO: old retry/defer and permanent conflict never release newer monitor; meeting remains eligible");leave();

    enter();a=make_scene("unsealed_old",1,2,10,7,1000,false,false);
    b=make_scene("unsealed_young",1,1,20,8,100,true,false);
    assert(sd_audio_catalog_read(a.session_id,0,&p)==ESP_OK);p.sealed=0;
    assert(sd_audio_catalog_write(a.session_id,&p)==ESP_OK);
    assert(next(&s,&p,&found,&file,&wait)==ESP_OK && !found && wait);
    assert(sd_audio_catalog_read(a.session_id,0,&p)==ESP_OK);p.sealed=SD_AUDIO_SEGMENT_ATTEMPTED;
    assert(sd_audio_catalog_write(a.session_id,&p)==ESP_OK);
    assert(next(&s,&p,&found,&file,&wait)==ESP_OK && !found && wait);
    assert(sd_audio_catalog_read(a.session_id,0,&p)==ESP_OK);p.sealed=1;
    assert(sd_audio_catalog_write(a.session_id,&p)==ESP_OK);expect_file(a.session_id,0);
    puts("PASS FIFO: unsealed earlier file waits; sealed later files and younger scene cannot pass");leave();

    enter();a=make_scene("poisoned_end",1,3,10,7,1000,true,false);
    assert(sd_audio_catalog_ack(a.session_id,0)==ESP_OK && sd_audio_catalog_ack(a.session_id,2)==ESP_OK);
    assert(sd_audio_catalog_load(a.session_id,&a)==ESP_OK);a.first_pending=3;
    char manifest[128];assert(catalog_path(manifest,a.session_id,"session"));FILE *f=fopen(manifest,"r+b");assert(f);
    assert(write_pair(f,0,&a,sizeof(a),offsetof(sd_audio_session_t,crc))==ESP_OK && fclose(f)==0);
    ready=false;assert(sd_monitor_fifo_end_ready(&a,&ready)==ESP_ERR_INVALID_STATE && !ready);
    assert(sd_audio_catalog_load(a.session_id,&s)==ESP_OK && !s.done);
    puts("PASS FIFO end: poisoned cursor cannot hide an unconfirmed middle file");leave();

    enter();a=make_scene("late_mutation",1,1,10,7,1000,true,false);
    assert(sd_audio_catalog_ack(a.session_id,0)==ESP_OK);expect_end(a.session_id,false);
    assert(sd_audio_catalog_load(a.session_id,&a)==ESP_OK);
    assert(sd_audio_catalog_read(a.session_id,0,&p)==ESP_OK);p.acknowledged=0;
    assert(sd_audio_catalog_write(a.session_id,&p)==ESP_OK);
    assert(sd_monitor_fifo_finish(&a)==ESP_ERR_INVALID_STATE);
    puts("PASS FIFO end: metadata mutation invalidates end proof before done persistence");leave();

    enter();a=make_scene("large_paged",1,129,10,7,1000,true,true);
    b=make_scene("paged_new",1,1,500,8,1,true,false);
    for(unsigned i=0;i<129;i++){expect_file(a.session_id,i);assert(sd_audio_catalog_ack(a.session_id,i)==ESP_OK);}
    expect_end(a.session_id,true);expect_file(b.session_id,0);
    struct stat st;assert(stat("catalog/large_paged.index.00000000",&st)!=0);
    puts("PASS FIFO: 129-file paged scene, durable ACK cleanup and pruned pages; no resurrection or younger overtaking");leave();

    enter();for(unsigned i=0;i<40;i++){
        char id[40];snprintf(id,sizeof(id),"many_%02u",i);
        make_scene(id,1,1,1000-i*10,7,1000+i*20,true,false);
    }
    expect_file("many_39",0);
    puts("PASS FIFO: bounded multi-slice scene scan chooses physical creation order, not directory name or boot duration");leave();

    enter();a=make_scene("partial_old",1,3,10,7,1000,true,false);
    b=make_scene("partial_new",1,1,20,8,1,true,false);
    assert(sd_audio_catalog_mark_missing(a.session_id,1)==ESP_OK);
    expect_file(a.session_id,0);assert(sd_audio_catalog_ack(a.session_id,0)==ESP_OK);
    expect_file(a.session_id,2);assert(sd_audio_catalog_ack(a.session_id,2)==ESP_OK);
    expect_end(a.session_id,false);
    assert(sd_audio_catalog_load(a.session_id,&a)==ESP_OK&&!a.complete&&!a.done);
    expect_end(a.session_id,true);expect_file(b.session_id,0);
    puts("PASS FIFO partial: original missing-file evidence remains incomplete; remaining files ACK before end and newer scene");leave();
    return 0;
}
