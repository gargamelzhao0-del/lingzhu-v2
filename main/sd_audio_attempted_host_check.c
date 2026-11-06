/* Exercise production scene_prepare -> marker -> ACK -> cleanup -> next pick,
 * including I/O faults. Directory counters prove the repeated scan is gone. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <io.h>
#include <dirent.h>
static unsigned directory_opens, manifest_reads, index_writes;
static unsigned fail_sync, fail_close, fail_write;
static FILE *index_writer;
static DIR *audit_directory(const char *p) { ++directory_opens; return opendir(p); }
static FILE *audit_open(const char *p,const char *mode) {
    FILE *file=fopen(p,mode);
    if(strstr(p,".session") && !strcmp(mode,"rb")) ++manifest_reads;
    if(strstr(p,".index") && (strchr(mode,'+') || strchr(mode,'w')))index_writer=file;
    return file;
}
static int audit_close(FILE *file) {
    const int fail=file==index_writer && fail_close;
    if(file==index_writer)index_writer=NULL;
    const int result=fclose(file);
    if(fail){--fail_close;errno=EIO;return EOF;}return result;
}
static int audit_sync(int fd) {
    if(fail_sync && index_writer && fd==_fileno(index_writer)){--fail_sync;errno=EIO;return -1;}
    return _commit(fd);
}
static size_t audit_write(const void *p,size_t size,size_t n,FILE *file) {
    if(file==index_writer) {
        ++index_writes;
        if(fail_write){--fail_write;return fwrite(p,1,size*n/2,file)/size;}
    }
    return fwrite(p,size,n,file);
}
#define opendir audit_directory
#define fopen audit_open
#define fclose audit_close
#define _commit audit_sync
#define fwrite audit_write
#define SD_AUDIO_ARCHIVE_ROOT "."
#define SD_AUDIO_ARCHIVE_NEW_ROOT "."
#include "sd_audio_catalog.c"
#undef opendir
#undef fopen
#undef fclose
#undef _commit
#undef fwrite
#define main existing_scene_checks
#include "sd_audio_scene_host_check.c"
#undef main

static void audio_file(const sd_audio_segment_t *part) {
    FILE *f=fopen(part->path,"wb");assert(f);assert(fputc(0,f)==0);assert(!fclose(f));
}
static void pick_mode(const uint8_t owner[32],unsigned mode,
    sd_audio_session_t *session,sd_audio_segment_t *part,bool *have,bool *file) {
    assert(sd_audio_catalog_next(owner,7,10000,0,NULL,false,(int)mode,
        session,part,have,file)==ESP_OK);
}
static void check_first_upload(void) {
    for(unsigned i=0;i<40;i++) {
        char id[48];snprintf(id,sizeof(id),"history_%02u",i);
        sd_audio_session_t old=make(id,0,7);old.done=1;assert(sd_audio_catalog_save(&old)==ESP_OK);
    }
    sd_audio_session_t scene=make("fresh",5,7),selected;
    sd_audio_segment_t part;bool have,file;uint32_t offset;
    nuwax_ws_client_t ws={0};card_scene_reset();sd_audio_catalog_replay_reset();
    directory_opens=manifest_reads=0;
    for(unsigned i=0;i<5;i++) {
        pick_mode(scene.owner,i%2,&selected,&part,&have,&file);
        if(!have)pick_mode(scene.owner,1-i%2,&selected,&part,&have,&file);
        assert(have && file && part.index==i && !strcmp(selected.session_id,scene.session_id));
        assert(directory_opens==1 && replay_pick[0][0] && replay_empty[1]);
        audio_file(&part);
        assert(card_scene_prepare(&ws,1,true,&selected,&part,&offset)==ESP_OK && offset==0);
        assert(directory_opens==1 && replay_pick[0][0] && replay_empty[1] && !selected_valid);
        sd_audio_segment_t saved;assert(sd_audio_catalog_read(scene.session_id,i,&saved)==ESP_OK);
        assert(saved.sealed & SD_AUDIO_SEGMENT_ATTEMPTED);
        assert(saved.total_bytes==part.total_bytes && saved.payload_crc==part.payload_crc && !saved.acknowledged);
        assert(sd_audio_catalog_ack(scene.session_id,i)==ESP_OK);
        assert(!remove(part.path));sd_audio_catalog_note_deleted(scene.session_id,i);card_scene_file_done();
    }
    pick_mode(scene.owner,0,&selected,&part,&have,&file);
    assert(have && !file && directory_opens==1 && begins==1);
    printf("PASS: 40 historical scenes + 5 first-upload files + session end: directory_scans=%u manifest_reads=%u begin_messages=%u\n",
        directory_opens,manifest_reads,begins);
    assert(checked_scene_end(&ws,1,true,&selected)==ESP_OK);
    assert(!replay_pick[0][0] && !replay_empty[1]); // Done changes eligibility.
}
static void check_faults(void) {
    for(unsigned fault=0;fault<3;fault++) {
        char id[48];snprintf(id,sizeof(id),"fault_%u",fault);
        sd_audio_session_t scene=make(id,1,7),selected;
        sd_audio_segment_t part;bool have,file;uint32_t offset=0;
        nuwax_ws_client_t ws={0};card_scene_reset();sd_audio_catalog_replay_reset();
        pick_mode(scene.owner,0,&selected,&part,&have,&file);assert(have && file);
        audio_file(&part);
        if(fault==0)fail_write=1;else if(fault==1)fail_sync=1;else fail_close=1;
        assert(card_scene_prepare(&ws,1,true,&selected,&part,&offset)==ESP_FAIL);
        assert(!selected_valid && !acked_id[0] && replay_pick[0][0]);
        struct stat st;assert(!stat(part.path,&st));
        unsigned before=index_writes;
        assert(sd_audio_catalog_mark_attempted(scene.session_id,0)==ESP_OK && index_writes==before+1);
        assert(sd_audio_catalog_read(scene.session_id,0,&part)==ESP_OK && !part.acknowledged && !part.missing);
        assert(part.sealed & SD_AUDIO_SEGMENT_ATTEMPTED);
        assert(sd_audio_catalog_load(scene.session_id,&scene)==ESP_OK && scene.complete && !scene.done);
        assert(sd_audio_catalog_ack(scene.session_id,0)==ESP_OK);assert(!remove(part.path));
        sd_audio_catalog_note_deleted(scene.session_id,0);
        scene.done=1;assert(sd_audio_catalog_save(&scene)==ESP_OK);
    }
    puts("PASS: partial write/sync/close failure blocks preparation, retains audio, invalidates stale proof; fresh durable retry succeeds");
}
static void check_invalidation(void) {
    sd_audio_session_t scene=make("eligible",1,7),selected;
    sd_audio_segment_t part;bool have,file;
    sd_audio_catalog_replay_reset();pick_mode(scene.owner,0,&selected,&part,&have,&file);assert(have && file);
    assert(sd_audio_catalog_mark_attempted(scene.session_id,0)==ESP_OK && replay_pick[0][0]);
    assert(sd_audio_catalog_read(scene.session_id,0,&part)==ESP_OK);
    part.end_ms+=20;assert(sd_audio_catalog_write(scene.session_id,&part)==ESP_OK);
    assert(!replay_pick[0][0]); // General mutations must still invalidate.
    pick_mode(scene.owner,0,&selected,&part,&have,&file);assert(have && file && replay_pick[0][0]);
    sd_audio_session_t earlier=make("a_earlier",1,7);assert(!replay_pick[0][0]);
    pick_mode(scene.owner,0,&selected,&part,&have,&file);assert(have && !strcmp(selected.session_id,earlier.session_id));
    assert(sd_audio_catalog_defer(&selected)==ESP_OK && !replay_pick[0][0]);
    pick_mode(scene.owner,0,&selected,&part,&have,&file);assert(have && !strcmp(selected.session_id,scene.session_id));
    part.missing=1;assert(sd_audio_catalog_write(scene.session_id,&part)==ESP_OK);
    assert(sd_audio_catalog_mark_attempted(scene.session_id,0)==ESP_ERR_INVALID_STATE);
    part.missing=0;part.sealed=0;assert(sd_audio_catalog_write(scene.session_id,&part)==ESP_OK);
    assert(sd_audio_catalog_mark_attempted(scene.session_id,0)==ESP_ERR_INVALID_STATE);
    part.sealed=1;part.acknowledged=1;assert(sd_audio_catalog_write(scene.session_id,&part)==ESP_OK);
    assert(sd_audio_catalog_mark_attempted(scene.session_id,0)==ESP_ERR_INVALID_STATE);
    assert(sd_audio_catalog_mark_attempted("../invalid",0)==ESP_ERR_INVALID_ARG);
    assert(sd_audio_catalog_mark_attempted(scene.session_id,UINT32_MAX)==ESP_ERR_INVALID_SIZE);
    puts("PASS: new/changed/deferred/finished scenes still invalidate hints; chronological selection and invalid-record guards retained");
}
int main(void) {
    assert(sd_audio_catalog_init()==ESP_OK);
    check_first_upload();check_faults();check_invalidation();card_scene_reset();
    return 0;
}
