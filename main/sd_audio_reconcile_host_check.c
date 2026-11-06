/* Actual catalog/NA builder with temporary files and explicit clock/I/O faults. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_frame.h"
static unsigned live_dirs, header_reads, fail_open_dir, fail_read_dir;
static int64_t virtual_us = 120000000;
static unsigned header_cost_us=60000U;
static bool watch_reads, allocation_fail;
static int header_open_error;
static unsigned discard_write_fault;
static FILE *checked_open(const char *path, const char *mode)
{
    if(discard_write_fault && strstr(path,".index") && !strcmp(mode,"r+b")){
        --discard_write_fault;errno=ENOSPC;return NULL;
    }
    const size_t n = strlen(path);
    if (header_open_error && !strcmp(mode,"rb") && n >= 6U && !strcmp(path+n-6U,".ready")) {
        errno=header_open_error;header_open_error=0;return NULL;
    }
    if (watch_reads && !strcmp(mode,"rb") && n >= 6U && !strcmp(path+n-6U,".ready")) {
        ++header_reads; virtual_us += header_cost_us; /* Explicit, configurable read model. */
    }
    return fopen(path,mode);
}
static DIR *checked_opendir(const char *path)
{
    if (fail_open_dir) { --fail_open_dir; errno=EIO; return NULL; }
    DIR *dir=opendir(path);if(dir)++live_dirs;return dir;
}
static int checked_closedir(DIR *dir) { assert(live_dirs);--live_dirs;return closedir(dir); }
static struct dirent *checked_readdir(DIR *dir)
{
    if(fail_read_dir){--fail_read_dir;errno=EIO;return NULL;}return readdir(dir);
}
static void *checked_calloc(size_t n,size_t size)
{ if(allocation_fail){errno=ENOMEM;return NULL;}return calloc(n,size); }
#define SD_AUDIO_REPAIR_NOW_US() virtual_us
#define fopen checked_open
#define opendir checked_opendir
#define closedir checked_closedir
#define readdir checked_readdir
#define calloc checked_calloc
#ifdef CATALOG_RECONCILE_SOURCE
#include CATALOG_RECONCILE_SOURCE
#else
#include "sd_audio_catalog.c"
#endif
#undef fopen
#undef opendir
#undef closedir
#undef readdir
#undef calloc
static const uint8_t fixture_owner[32]={0x31};
static bool fixture_open_scene;
static void put32(uint8_t *p,uint32_t v){memcpy(p,&v,4U);}
static void put64(uint8_t *p,uint64_t v){memcpy(p,&v,8U);}
static sd_audio_session_t make_scene(const char *id,unsigned mode,unsigned count,unsigned physical)
{
    sd_audio_session_t scene={.mode=mode,.boot_id=7U,.start_ms=1000U,.complete=1U};
    strcpy(scene.session_id,id);memcpy(scene.owner,fixture_owner,32U);
    assert(sd_audio_catalog_create(&scene)==ESP_OK);
    for(unsigned i=0U;i<count;++i){
        uint8_t bytes[794]={0};memcpy(bytes,"NWXOFF1",7U);
        bytes[8]=1U;bytes[10]=96U;bytes[12]=(uint8_t)mode;bytes[13]=bytes[14]=1U;
        put32(bytes+16U,16000U);bytes[20]=20U;bytes[22]=2U;put32(bytes+24U,i);
        put64(bytes+28U,1000U+i*20U);strcpy((char*)bytes+36U,id);put32(bytes+96U,662U);
        int16_t pcm[320];for(unsigned k=0U;k<320U;++k)pcm[k]=(int16_t)(i*11U+k);
        size_t made=0U;assert(nuwax_audio_frame_build_pcm16(bytes+100U,662U,pcm,320U,i,
            1000U+i*20U,0U,16000U,1U,20U,&made)==ESP_OK&&made==662U);
        uint8_t *footer=bytes+762U;memcpy(footer,"NWXEND1",7U);footer[8]=1U;footer[10]=32U;
        put32(footer+12U,1U);put64(footer+16U,662U);put32(footer+24U,checksum(bytes+100U,662U));footer[28]=1U;
        sd_audio_segment_t part={.start_ms=1000U+i*20U,.end_ms=1020U+i*20U};
        snprintf(part.path,sizeof(part.path),"audio/card_%s_%08u.ready",id,physical+i);
        assert(sd_audio_catalog_reserve(&scene,&part)==ESP_OK);
        FILE *file=fopen(part.path,"wb");assert(file&&fwrite(bytes,1U,sizeof(bytes),file)==sizeof(bytes)&&!fclose(file));
        part.sealed=1U;part.total_bytes=sizeof(bytes);part.payload_crc=checksum(bytes+100U,662U);
        assert(sd_audio_catalog_write(id,&part)==ESP_OK);
    }
    scene.ended=fixture_open_scene?0U:1U;scene.ended_ms=fixture_open_scene?0U:1000U+count*20U;
    assert(sd_audio_catalog_save(&scene)==ESP_OK);return scene;
}
static void damage_index(const char *id,uint32_t index)
{
    char path[128];uint64_t offset;
    assert(catalog_index_location(id,index,path,&offset)==ESP_OK);
    FILE *file=fopen(path,"r+b");assert(file&&fseek(file,(long)offset,SEEK_SET)==0);
    uint8_t bytes[2*sizeof(sd_audio_segment_t)];memset(bytes,0xa5,sizeof(bytes));
    assert(fwrite(bytes,1U,sizeof(bytes),file)==sizeof(bytes)&&!fclose(file));
}
static void unchanged_index(const char *id,uint32_t index)
{
    char path[128];uint64_t offset;assert(catalog_index_location(id,index,path,&offset)==ESP_OK);
    FILE *file=fopen(path,"rb");assert(file&&fseek(file,(long)offset,SEEK_SET)==0);
    uint8_t bytes[2*sizeof(sd_audio_segment_t)];assert(fread(bytes,1U,sizeof(bytes),file)==sizeof(bytes)&&!fclose(file));
    for(unsigned k=0U;k<sizeof(bytes);++k)assert(bytes[k]==0xa5);
}
static void source_intact(const sd_audio_segment_t *part)
{
    FILE *file=fopen(part->path,"rb");assert(file);
    uint8_t bytes[794];assert(fread(bytes,1U,sizeof(bytes),file)==sizeof(bytes)&&!fclose(file));
    assert(!memcmp(bytes,"NWXOFF1",7U)&&checksum(bytes+100U,662U)==part->payload_crc);
}
static esp_err_t repair_until_done(const char *id,sd_audio_segment_t *part)
{
    esp_err_t err=ESP_ERR_NOT_FINISHED;
    for(unsigned i=0U;i<10000U&&err==ESP_ERR_NOT_FINISHED;++i)err=catalog_read_reconciled(id,0U,part);
    return err;
}
static void basic_guard(void)
{
    sd_audio_session_t scene=make_scene("guard",1U,2U,10U);
    assert(sd_audio_catalog_ack(scene.session_id,0U)==ESP_OK);
    assert(sd_audio_catalog_mark_missing(scene.session_id,0U)==ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_load(scene.session_id,&scene)==ESP_OK&&scene.complete);
    damage_index(scene.session_id,1U);
    assert(sd_audio_catalog_mark_missing(scene.session_id,1U)==ESP_ERR_INVALID_CRC);
    unchanged_index(scene.session_id,1U);
    assert(sd_audio_catalog_load(scene.session_id,&scene)==ESP_OK&&scene.complete);
}
static void fifo_head(void)
{
    sd_audio_session_t old=make_scene("old",1U,2U,10U);(void)make_scene("new",1U,1U,30U);
    sd_audio_session_t picked;sd_audio_segment_t part;bool found,file,wait;
    assert(sd_monitor_fifo_next(fixture_owner,&picked,&part,&found,&file,&wait)==ESP_OK&&found&&file&&!strcmp(picked.session_id,"old"));
    const sd_audio_segment_t original=part;char path[128];assert(catalog_path(path,old.session_id,"session"));
    FILE *f=fopen(path,"r+b");assert(f);uint8_t bad[2*sizeof(old)];memset(bad,0xa5,sizeof(bad));
    assert(fwrite(bad,1U,sizeof(bad),f)==sizeof(bad)&&!fclose(f));
    bool advanced=false;
    for(unsigned attempt=0U;attempt<100U&&!advanced;++attempt){
        sd_audio_catalog_replay_reset();
        esp_err_t err=sd_monitor_fifo_next(fixture_owner,&picked,&part,&found,&file,&wait);
        assert(err==ESP_OK||err==ESP_ERR_NOT_FINISHED||err==ESP_ERR_INVALID_CRC);
        advanced=err==ESP_OK&&found&&file&&!strcmp(picked.session_id,"new");
    }
    assert(advanced&&!part.acknowledged);source_intact(&original);
}
static void cursor_removed(void)
{
    (void)make_scene("cursor_old",1U,1U,10U);(void)make_scene("cursor_new",1U,1U,30U);
    monitor_fifo_state_t cursor={0};memcpy(cursor.owner,fixture_owner,32U);cursor.owner_valid=true;monitor_scan_reset(&cursor);
    unsigned budget=16U;bool done=false;
    assert(monitor_scan_session(&cursor,1U,&budget,&done)==ESP_OK&&cursor.after[0]);
    char path[128],moved[160];assert(catalog_path(path,cursor.after,"session"));
    snprintf(moved,sizeof(moved),"%s.bad.injected",path);assert(!rename(path,moved));
    bool finished=false;
    for(unsigned i=0U;i<100U&&!finished;++i){budget=16U;esp_err_t err=monitor_find_head(&cursor,1U,&budget);
        assert(err==ESP_OK||err==ESP_ERR_NOT_FINISHED);finished=err==ESP_OK&&cursor.scan_done;}
    assert(finished&&cursor.head[0]);
}
static void repair_case(const char *kind)
{
    const bool large=!strcmp(kind,"bounded")||!strcmp(kind,"fast");
    sd_audio_session_t scene=make_scene("repair",1U,large?256U:8U,10U);
    sd_audio_segment_t original,actual;assert(sd_audio_catalog_read(scene.session_id,0U,&original)==ESP_OK);
    damage_index(scene.session_id,0U);watch_reads=true;
    if(!strcmp(kind,"fast"))header_cost_us=0U;
    if(!strcmp(kind,"nomem")){
        allocation_fail=true;assert(catalog_read_reconciled(scene.session_id,0U,&actual)==ESP_ERR_NO_MEM&&!live_dirs&&!header_reads);
        unchanged_index(scene.session_id,0U);allocation_fail=false;
    }
    if(!strcmp(kind,"expired")){
        sd_audio_catalog_repair_set_deadline(virtual_us-1);
        assert(catalog_read_reconciled(scene.session_id,0U,&actual)==ESP_ERR_NOT_FINISHED&&!live_dirs&&!header_reads);
        sd_audio_catalog_repair_set_deadline(0);
    }
    if(!strcmp(kind,"file_nomem")||!strcmp(kind,"file_busy")){
        header_open_error=!strcmp(kind,"file_nomem")?ENOMEM:EMFILE;
        const esp_err_t expected=header_open_error==ENOMEM?ESP_ERR_NO_MEM:ESP_ERR_NOT_FINISHED;
        assert(catalog_read_reconciled(scene.session_id,0U,&actual)==expected&&!header_reads);
        unchanged_index(scene.session_id,0U);
        assert(!session_blocked(scene.session_id));
    }
    if(!strcmp(kind,"cancel")||!strcmp(kind,"io")){
        assert(catalog_read_reconciled(scene.session_id,0U,&actual)==ESP_ERR_NOT_FINISHED&&live_dirs);
        unchanged_index(scene.session_id,0U);
        if(!strcmp(kind,"io")){
            fail_read_dir=1U;assert(catalog_read_reconciled(scene.session_id,0U,&actual)==ESP_FAIL);
            unchanged_index(scene.session_id,0U);
        }
        assert(sd_audio_catalog_repair_reset()==ESP_OK&&!live_dirs);
        assert(sd_audio_catalog_prepare_remount()==ESP_OK&&!live_dirs);
        assert(sd_audio_catalog_init()==ESP_OK);
    }
    if(!strcmp(kind,"duplicate")){
        uint8_t bytes[794];FILE *f=fopen(original.path,"rb");assert(f&&fread(bytes,1U,sizeof(bytes),f)==sizeof(bytes)&&!fclose(f));
        const char *copy="audio/card_repair_00009999.ready";f=fopen(copy,"wb");assert(f&&fwrite(bytes,1U,sizeof(bytes),f)==sizeof(bytes)&&!fclose(f));
        assert(repair_until_done(scene.session_id,&actual)==ESP_OK&&!session_blocked(scene.session_id));
        assert(actual.missing==SD_AUDIO_SEGMENT_RETIRED&&!actual.acknowledged);
        source_intact(&original);assert(sd_audio_catalog_repair_reset()==ESP_OK&&!live_dirs);return;
    }
    esp_err_t err=ESP_ERR_NOT_FINISHED;unsigned slices=0U,max_reads=0U;
    for(;slices<10000U&&err==ESP_ERR_NOT_FINISHED;++slices){
        const unsigned before=header_reads;const int64_t began=virtual_us;
        err=catalog_read_reconciled(scene.session_id,0U,&actual);
        const unsigned delta=header_reads-before;if(delta>max_reads)max_reads=delta;
        assert(delta<=2U&&virtual_us-began<=120000);
        if(err==ESP_ERR_NOT_FINISHED){
            unchanged_index(scene.session_id,0U);
            sd_audio_session_t writer;assert(sd_audio_catalog_load(scene.session_id,&writer)==ESP_OK);
            assert(sd_audio_catalog_save(&writer)==ESP_OK); /* Concurrent writer metadata revision. */
        }
    }
    assert(err==ESP_OK&&!actual.acknowledged&&!actual.missing&&actual.payload_crc==original.payload_crc&&actual.total_bytes==original.total_bytes);
    source_intact(&original);assert(sd_audio_catalog_repair_reset()==ESP_OK&&!live_dirs);
    if(large)assert(slices>16U&&header_reads>=256U);
    printf("repair %s slices=%u max_header_reads_per_slice=%u original_tuple=1 no_early_write=1\n",kind,slices,max_reads);
}
static void two_modes(void)
{
    sd_audio_session_t a=make_scene("mode_a",0U,16U,10U),b=make_scene("mode_b",1U,16U,100U);
    sd_audio_segment_t original[2],actual;
    assert(sd_audio_catalog_read(a.session_id,0U,&original[0])==ESP_OK&&sd_audio_catalog_read(b.session_id,0U,&original[1])==ESP_OK);
    damage_index(a.session_id,0U);damage_index(b.session_id,0U);watch_reads=true;
    bool done[2]={false,false};
    for(unsigned i=0U;i<2000U&&(!done[0]||!done[1]);++i){unsigned mode=i&1U;
        if(done[mode])continue;const char *id=mode?b.session_id:a.session_id;
        esp_err_t err=catalog_read_reconciled(id,0U,&actual);assert(err==ESP_OK||err==ESP_ERR_NOT_FINISHED);
        if(err==ESP_OK){assert(actual.payload_crc==original[mode].payload_crc&&!actual.acknowledged);done[mode]=true;}
    }
    assert(done[0]&&done[1]);assert(sd_audio_catalog_repair_reset()==ESP_OK&&!live_dirs);
}
static void proven_absence(bool unparseable)
{
    sd_audio_session_t scene=make_scene("absence",1U,4U,10U);sd_audio_segment_t part;
    assert(sd_audio_catalog_read(scene.session_id,0U,&part)==ESP_OK);
    if(!unparseable){assert(sd_audio_catalog_ack(scene.session_id,0U)==ESP_OK&&!remove(part.path)&&sd_audio_catalog_drop_backup(part.path)==ESP_OK);}
    else {FILE *f=fopen(part.path,"r+b");assert(f&&fputc('X',f)!=EOF&&!fclose(f));assert(sd_audio_catalog_drop_backup(part.path)==ESP_OK);}
    damage_index(scene.session_id,0U);
    esp_err_t err=sd_audio_catalog_recover(8U);
    if(unparseable){assert(err==ESP_OK&&!session_blocked(scene.session_id));assert(sd_audio_catalog_read(scene.session_id,0U,&part)==ESP_OK&&part.missing&&!part.acknowledged);}
    else {assert(err==ESP_OK&&sd_audio_catalog_read(scene.session_id,0U,&part)==ESP_OK&&part.missing&&!part.acknowledged);}
}
static void uncertain_source_name(bool upper_case)
{
    sd_audio_session_t scene=make_scene("name_guard",1U,1U,10U);sd_audio_segment_t part;
    assert(sd_audio_catalog_read(scene.session_id,0U,&part)==ESP_OK);
    const uint32_t original_crc=part.payload_crc;
    const char *retained=upper_case?"audio/CARD_name_guard_00000010.ready":"audio/card_unparsed.partial";
    assert(!rename(part.path,retained));damage_index(scene.session_id,0U);
    assert(sd_audio_catalog_recover(8U)==ESP_OK&&!session_blocked(scene.session_id));
    assert(sd_audio_catalog_read(scene.session_id,0U,&part)==ESP_OK&&!part.acknowledged);
    assert(part.missing || (upper_case && part.payload_crc==original_crc));
    strcpy(part.path,retained);part.payload_crc=original_crc;source_intact(&part);
}
static void discard_flow(unsigned mode,bool live)
{
    fixture_open_scene=live;
    sd_audio_session_t scene=make_scene("discard_flow",mode,5U,10U);
    fixture_open_scene=false;
    (void)make_scene("discard_next",mode,2U,30U);
    sd_audio_segment_t original[5],actual;
    for(unsigned i=0U;i<5U;++i)assert(sd_audio_catalog_read(scene.session_id,i,&original[i])==ESP_OK);
    FILE *f=fopen(original[2].path,"r+b");assert(f&&fputc('X',f)!=EOF&&!fclose(f));
    assert(sd_audio_catalog_drop_backup(original[2].path)==ESP_OK);damage_index(scene.session_id,2U);
    if(!live){
        assert(catalog_block_reason(scene.session_id,"archive_metadata_unknown: old firmware local block\n")==ESP_OK);
        assert(sd_audio_catalog_recover(8U)==ESP_OK&&!session_blocked(scene.session_id));
    }else{
        esp_err_t err=ESP_ERR_NOT_FINISHED;
        for(unsigned i=0U;i<1000U&&err==ESP_ERR_NOT_FINISHED;++i)err=catalog_read_reconciled(scene.session_id,2U,&actual);
        assert(err==ESP_OK);
    }
    assert(sd_audio_catalog_read(scene.session_id,2U,&actual)==ESP_OK&&actual.missing==SD_AUDIO_SEGMENT_RETIRED&&!actual.acknowledged);
    const unsigned expected[]={0U,1U,3U,4U};
    unsigned received=0U;bool closed=false;
    for(unsigned attempts=0U;attempts<2000U&&!closed;++attempts){
        sd_audio_session_t picked;bool found=false,file=false,waiting=false;
        const esp_err_t err=mode?sd_monitor_fifo_next(fixture_owner,&picked,&actual,&found,&file,&waiting):
            sd_meeting_fifo_next(fixture_owner,&picked,&actual,&found,&file,&waiting);
        assert(err==ESP_OK||err==ESP_ERR_NOT_FINISHED);
        if(err!=ESP_OK||!found){
            if(received==4U&&live){
                assert(sd_audio_catalog_load(scene.session_id,&scene)==ESP_OK&&!scene.ended&&scene.segment_count==5U&&!scene.complete);
                scene.ended=1U;scene.ended_ms=1100U;assert(sd_audio_catalog_save(&scene)==ESP_OK);live=false;
            }
            continue;
        }
        assert(!strcmp(picked.session_id,scene.session_id));
        if(file){
            assert(received<4U&&actual.index==expected[received++]);
            assert(actual.payload_crc==original[actual.index].payload_crc&&actual.start_ms==original[actual.index].start_ms);
            source_intact(&actual);assert(sd_audio_catalog_ack(picked.session_id,actual.index)==ESP_OK);
        }else{
            bool ready=false;esp_err_t gate=mode?sd_monitor_fifo_end_ready(&picked,&ready):sd_meeting_fifo_end_ready(&picked,&ready);
            assert(gate==ESP_OK||gate==ESP_ERR_NOT_FINISHED);
            if(!ready)continue;
            assert(received==4U&&picked.segment_count==5U&&!picked.complete);
            assert((mode?sd_monitor_fifo_finish(&picked):sd_meeting_fifo_finish(&picked))==ESP_OK);closed=true;
        }
    }
    assert(closed&&sd_audio_catalog_recover(9U)==ESP_OK);
    bool next=false;
    for(unsigned i=0U;i<1000U&&!next;++i){
        sd_audio_session_t picked;bool found=false,file=false,waiting=false;
        esp_err_t err=mode?sd_monitor_fifo_next(fixture_owner,&picked,&actual,&found,&file,&waiting):
            sd_meeting_fifo_next(fixture_owner,&picked,&actual,&found,&file,&waiting);
        assert(err==ESP_OK||err==ESP_ERR_NOT_FINISHED);
        next=err==ESP_OK&&found&&file&&!strcmp(picked.session_id,"discard_next")&&actual.index==0U;
    }
    assert(next);
    puts("PASS discard flow: actual mode FIFO sends 0,1,3,4; count=5 complete=false; live capture retained; restart advances to next scene index=0");
}
static void discard_guards(void)
{
    sd_audio_session_t scene=make_scene("discard_guards",0U,3U,10U);sd_audio_segment_t part,actual;
    assert(sd_audio_catalog_read(scene.session_id,0U,&part)==ESP_OK);
    sd_audio_session_t foreign=scene;foreign.owner[0]^=1U;
    assert(sd_audio_catalog_discard_expected(&foreign,&part)==ESP_ERR_INVALID_STATE);source_intact(&part);
    discard_write_fault=1U;
    assert(sd_audio_catalog_discard_expected(&scene,&part)==ESP_FAIL);source_intact(&part);
    assert(sd_audio_catalog_read(scene.session_id,0U,&actual)==ESP_OK&&!actual.missing&&!actual.acknowledged);
    assert(sd_audio_catalog_discard_expected(&scene,&part)==ESP_OK);
    assert(sd_audio_catalog_read(scene.session_id,0U,&actual)==ESP_OK&&actual.missing==SD_AUDIO_SEGMENT_RETIRED&&!actual.acknowledged);
    /* Force the older dual slot: it must already contain the same discard. */
    char path[128];uint64_t offset;assert(catalog_index_location(scene.session_id,0U,path,&offset)==ESP_OK);
    FILE *f=fopen(path,"rb");assert(f&&fseek(f,(long)offset,SEEK_SET)==0);
    sd_audio_segment_t pair[2];assert(fread(pair,1U,sizeof(pair),f)==sizeof(pair)&&!fclose(f));
    assert(pair[0].missing==SD_AUDIO_SEGMENT_RETIRED&&pair[1].missing==SD_AUDIO_SEGMENT_RETIRED);
    assert(sd_audio_catalog_read(scene.session_id,1U,&part)==ESP_OK&&sd_audio_catalog_ack(scene.session_id,1U)==ESP_OK);
    assert(sd_audio_catalog_discard_expected(&scene,&part)==ESP_ERR_NOT_FINISHED);
    assert(sd_audio_catalog_read(scene.session_id,1U,&actual)==ESP_OK&&actual.acknowledged&&!actual.missing);
    puts("PASS discard guards: foreign binding/ACK protected, failed persistence retains original bytes, both fallback slots terminal before unlink");
}
static void discard_open_tail(bool interrupted)
{
    fixture_open_scene=true;
    sd_audio_session_t scene=make_scene("discard_tail",1U,1U,10U);sd_audio_segment_t part;
    fixture_open_scene=false;
    assert(sd_audio_catalog_read(scene.session_id,0U,&part)==ESP_OK&&sd_audio_catalog_drop_backup(part.path)==ESP_OK);
    FILE *f=fopen(part.path,"r+b");assert(f&&fputc('X',f)!=EOF&&!fclose(f));damage_index(scene.session_id,0U);
    for(unsigned i=0U;i<20U;++i)assert(catalog_read_reconciled(scene.session_id,0U,&part)==ESP_ERR_NOT_FINISHED);
    unchanged_index(scene.session_id,0U);
    if(interrupted){
        assert(sd_audio_catalog_recover(8U)==ESP_OK);
        assert(sd_audio_catalog_load(scene.session_id,&scene)==ESP_OK&&scene.ended&&!scene.complete);
    }else{scene.ended=1U;scene.ended_ms=1020U;assert(sd_audio_catalog_save(&scene)==ESP_OK);}
    assert(repair_until_done(scene.session_id,&part)==ESP_OK&&part.missing==SD_AUDIO_SEGMENT_RETIRED&&!part.acknowledged);
    puts("PASS active tail: missing ready source cannot discard an open writer reservation; only closed immutable slot can become a terminal gap");
}
static void discard_ack_backup(void)
{
    sd_audio_session_t scene=make_scene("ack_restore",0U,2U,10U);sd_audio_segment_t part;
    assert(sd_audio_catalog_read(scene.session_id,0U,&part)==ESP_OK);
    assert(sd_audio_catalog_ack(scene.session_id,0U)==ESP_OK);
    FILE *f=fopen(part.path,"r+b");assert(f&&fputc('X',f)!=EOF&&!fclose(f));damage_index(scene.session_id,0U);
    assert(repair_until_done(scene.session_id,&part)==ESP_OK&&part.acknowledged&&!part.missing);
    assert(sd_audio_catalog_load(scene.session_id,&scene)==ESP_OK&&scene.complete);
    puts("PASS ACK precedence: independent owner-bound ACK backup restores a corrupt index even when the local audio header is bad; no valid cloud data discarded");
}
static void discard_wrong_reference(void)
{
    sd_audio_session_t scene=make_scene("discard_reference",0U,3U,10U);
    sd_audio_segment_t wrong,healthy;
    assert(sd_audio_catalog_read(scene.session_id,0U,&wrong)==ESP_OK&&sd_audio_catalog_read(scene.session_id,2U,&healthy)==ESP_OK);
    strcpy(wrong.path,healthy.path);assert(sd_audio_catalog_write(scene.session_id,&wrong)==ESP_OK);
    assert(sd_audio_catalog_discard_expected(&scene,&wrong)==ESP_OK);source_intact(&healthy);
    sd_audio_segment_t actual;assert(catalog_read_reconciled(scene.session_id,0U,&actual)==ESP_OK&&actual.missing==SD_AUDIO_SEGMENT_RETIRED);
    source_intact(&healthy);
    assert(sd_audio_catalog_read(scene.session_id,2U,&actual)==ESP_OK&&!actual.missing&&!actual.acknowledged&&actual.payload_crc==healthy.payload_crc);
    puts("PASS cross-file deletion fence: bad index 0 pointing at healthy file 2 discards only index 0; independent header prevents deleting file 2");
}
#ifdef CATALOG_RECONCILE_NATIVE_TEST
static void native_guard(void)
{
    sd_audio_session_t scene=make_scene("native",1U,8U,10U);sd_audio_segment_t original,actual;
    assert(sd_audio_catalog_read(scene.session_id,0U,&original)==ESP_OK);damage_index(scene.session_id,0U);watch_reads=true;
    native_scope_busy=true;sd_audio_catalog_repair_set_deadline(virtual_us+1000000);
    assert(catalog_read_reconciled(scene.session_id,0U,&actual)==ESP_ERR_NOT_FINISHED&&!header_reads&&!live_dirs);
    native_scope_busy=false;sd_audio_catalog_repair_set_deadline(virtual_us+1000);
    assert(catalog_read_reconciled(scene.session_id,0U,&actual)==ESP_ERR_NOT_FINISHED&&!native_scope_until);
    unchanged_index(scene.session_id,0U);
    esp_err_t err=ESP_ERR_NOT_FINISHED;
    for(unsigned i=0U;i<1000U&&err==ESP_ERR_NOT_FINISHED;++i){
        sd_audio_catalog_repair_set_deadline(virtual_us+1000000);
        err=catalog_read_reconciled(scene.session_id,0U,&actual);
        assert(!native_scope_until&&native_scope_begin==native_scope_end);
    }
    assert(err==ESP_OK&&native_scope_begin>1U&&actual.payload_crc==original.payload_crc&&!actual.acknowledged);
    source_intact(&original);assert(sd_audio_catalog_repair_reset()==ESP_OK&&!live_dirs);
    puts("PASS actual native-repair branch: PSRAM-only workspace; busy/expired scopes defer without metadata mutation; every acquired scope released");
}
#endif
int main(int argc,char **argv)
{
    assert(argc==2&&mkdir("audio",0775)==0&&mkdir("audio2",0775)==0&&sd_audio_catalog_init()==ESP_OK);
    if(!strcmp(argv[1],"guard"))basic_guard();
#ifdef CATALOG_RECONCILE_NATIVE_TEST
    else if(!strcmp(argv[1],"native"))native_guard();
#endif
    else if(!strcmp(argv[1],"head"))fifo_head();
    else if(!strcmp(argv[1],"cursor"))cursor_removed();
    else if(!strcmp(argv[1],"modes"))two_modes();
    else if(!strcmp(argv[1],"absence"))proven_absence(false);
    else if(!strcmp(argv[1],"unknown"))proven_absence(true);
    else if(!strcmp(argv[1],"upper_name"))uncertain_source_name(true);
    else if(!strcmp(argv[1],"partial_name"))uncertain_source_name(false);
    else if(!strcmp(argv[1],"discard_meeting"))discard_flow(0U,false);
    else if(!strcmp(argv[1],"discard_monitor"))discard_flow(1U,false);
    else if(!strcmp(argv[1],"discard_live_meeting"))discard_flow(0U,true);
    else if(!strcmp(argv[1],"discard_live_monitor"))discard_flow(1U,true);
    else if(!strcmp(argv[1],"discard_guards"))discard_guards();
    else if(!strcmp(argv[1],"discard_tail"))discard_open_tail(false);
    else if(!strcmp(argv[1],"discard_interrupted"))discard_open_tail(true);
    else if(!strcmp(argv[1],"discard_ack_backup"))discard_ack_backup();
    else if(!strcmp(argv[1],"discard_reference"))discard_wrong_reference();
    else repair_case(argv[1]);
    assert(sd_audio_catalog_repair_reset()==ESP_OK&&!live_dirs);
    printf("PASS actual catalog reconcile: %s; no leaked DIR or forged ACK; unrecoverable slot discarded without renumbering\n",argv[1]);return 0;
}
