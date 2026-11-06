#include <assert.h>
#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE *index_writer;
static unsigned fail_sync, fail_close, syncs, writes, deletes;
static int audit_sync(int fd) {
    ++syncs;
    if (fail_sync && index_writer && fd == _fileno(index_writer)) { --fail_sync; errno=EIO; return -1; }
    return _commit(fd);
}
static FILE *audit_open(const char *path, const char *mode) {
    FILE *f=fopen(path,mode);
    if (strstr(path,".index") && (strchr(mode,'+') || strchr(mode,'w'))) index_writer=f;
    return f;
}
static int audit_close(FILE *f) {
    const int inject = f==index_writer && fail_close;
    if (f==index_writer) index_writer=NULL;
    const int result=fclose(f);
    if (inject) { --fail_close; errno=EIO; return EOF; }
    return result;
}
static size_t audit_write(const void *p,size_t size,size_t count,FILE *f) {
    if (f==index_writer) ++writes;
    return fwrite(p,size,count,f);
}
static int audit_remove(const char *path) { ++deletes; return remove(path); }
#define _commit audit_sync
#define fopen audit_open
#define fclose audit_close
#define fwrite audit_write
#define remove audit_remove
#include "sd_audio_catalog.c"
#undef _commit
#undef fopen
#undef fclose
#undef fwrite
#undef remove

static sd_audio_session_t create(const char *id) {
    sd_audio_session_t scene={.boot_id=1,.ended=1,.complete=1,.segment_count=1};
    strcpy(scene.session_id,id);memset(scene.owner,1,32);assert(sd_audio_catalog_create(&scene)==ESP_OK);
    sd_audio_segment_t part={.sealed=1,.total_bytes=794};snprintf(part.path,sizeof(part.path),"%s.ready",id);
    assert(sd_audio_catalog_write(id,&part)==ESP_OK);
    unsigned char data[794]={0};memcpy(data+96,"\x96\x02\x00\x00NA",6);
    memcpy(data+762,"NWXEND1",7);data[790]=1;
    FILE *f=fopen(part.path,"wb");assert(f);assert(fwrite(data,1,sizeof(data),f)==sizeof(data));assert(!fclose(f));
    return scene;
}
static esp_err_t select_scene(sd_audio_session_t *scene,bool *end) {
    sd_audio_session_t out;sd_audio_segment_t part;bool have=false,file=false;
    const esp_err_t err=sd_audio_catalog_next(scene->owner,1,100,0,NULL,false,0,&out,&part,&have,&file);
    *end=err==ESP_OK && have && !file;return err;
}
static bool exists(const char *id) {
    char path[128];snprintf(path,sizeof(path),"%s.ready",id);struct stat st;return stat(path,&st)==0;
}
int main(void) {
    /* Model a reset losing the unsynced index update while the source stays
     * present. Recovery must retain a retryable, complete scene. */
    sd_audio_session_t reset=create("reset_before_retry");
    unsigned char prior[2*sizeof(sd_audio_segment_t)];
    FILE *f=fopen("catalog/reset_before_retry.index","rb");assert(f);
    const size_t saved=fread(prior,1,sizeof(prior),f);assert(saved==sizeof(prior));fclose(f);
    fail_sync=1;assert(sd_audio_catalog_ack(reset.session_id,0)==ESP_FAIL);
    fail_sync=1;bool reset_end=false;assert(select_scene(&reset,&reset_end)==ESP_FAIL && !reset_end);
    f=fopen("catalog/reset_before_retry.index","wb");assert(f);assert(fwrite(prior,1,saved,f)==saved);fclose(f);
    assert(sd_audio_catalog_recover(2)==ESP_OK && exists(reset.session_id));
    sd_audio_segment_t recovered;
    assert(sd_audio_catalog_read(reset.session_id,0,&recovered)==ESP_OK && !recovered.missing && !recovered.acknowledged);
    assert(sd_audio_catalog_load(reset.session_id,&reset)==ESP_OK && reset.complete);
    assert(sd_audio_catalog_ack(reset.session_id,0)==ESP_OK);
    assert(select_scene(&reset,&reset_end)==ESP_OK && reset_end && !exists(reset.session_id));
    reset.done=1;assert(sd_audio_catalog_save(&reset)==ESP_OK);
    for(unsigned fault=0;fault<2;fault++) {
        sd_audio_session_t scene=create(fault?"close_failure":"sync_failure");
        if(fault)fail_close=1;else fail_sync=1;
        assert(sd_audio_catalog_ack(scene.session_id,0)==ESP_FAIL && exists(scene.session_id));
        sd_audio_segment_t part;assert(sd_audio_catalog_read(scene.session_id,0,&part)==ESP_OK && part.acknowledged);
        const unsigned before_writes=writes,before_deletes=deletes;
        if(fault)fail_close=1;else fail_sync=1;
        bool end;
        assert(select_scene(&scene,&end)==ESP_FAIL && !end && exists(scene.session_id));
        assert(writes>before_writes && deletes==before_deletes);
        /* A healthy retry must rewrite even an already visible ACK before unlink. */
        const unsigned retry_writes=writes;
        assert(select_scene(&scene,&end)==ESP_OK && end && !exists(scene.session_id));
        assert(writes==retry_writes+1);
        sd_audio_catalog_replay_reset();assert(sd_audio_catalog_recover(2)==ESP_OK);
        assert(sd_audio_catalog_load(scene.session_id,&scene)==ESP_OK && scene.complete);
        assert(sd_audio_catalog_read(scene.session_id,0,&part)==ESP_OK && part.acknowledged && !part.missing);
        scene.done=1;assert(sd_audio_catalog_save(&scene)==ESP_OK);
    }
    sd_audio_session_t normal=create("durable_normal");
    assert(sd_audio_catalog_ack(normal.session_id,0)==ESP_OK);
    const unsigned before=syncs;bool end;
    assert(select_scene(&normal,&end)==ESP_OK && end && !exists(normal.session_id));
    assert(syncs==before); // A valid same-process proof does not add a sync.
    sd_audio_catalog_replay_reset();
    assert(select_scene(&normal,&end)==ESP_OK && end && syncs==before); // Already deleted: no rewrite.
    puts("PASS: failed sync/close retain audio and withhold session end; retry rewrites cached ACK; recovery stays complete; durable normal/deleted paths add no sync");
    return 0;
}
