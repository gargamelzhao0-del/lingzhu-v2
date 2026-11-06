#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static unsigned directory_reads, fail_directory_read;
static unsigned fail_directory_open;
static const char *fail_manifest;
static FILE *audit_manifest_open(const char *path,const char *mode) {
    if (fail_manifest&&!strcmp(path,fail_manifest)&&!strcmp(mode,"rb")) {
        fail_manifest=NULL;errno=EIO;return NULL;
    }
    return fopen(path,mode);
}
static DIR *audit_directory_open(const char *path) {
    if (fail_directory_open) { fail_directory_open--;errno=EIO;return NULL; }
    return opendir(path);
}
static struct dirent *audit_readdir(DIR *dir) {
    if (++directory_reads == fail_directory_read) { errno=EIO;return NULL; }
    return readdir(dir);
}
#define readdir audit_readdir
#define fopen audit_manifest_open
#define opendir audit_directory_open
#define main catalog_fixture_main
#include "sd_monitor_fifo_host_check.c"
#undef main
#undef readdir
#undef fopen
#undef opendir
int main(void) {
    enter();
    make_scene("io_young",1,1,20,1,100,true,false);
    make_scene("io_old",1,1,10,7,10000,true,false);
    sd_audio_session_t scene;sd_audio_segment_t part;bool found,file,wait;
    directory_reads=0;fail_directory_read=1;
    assert(sd_monitor_fifo_next(owner,&scene,&part,&found,&file,&wait)==ESP_FAIL);
    assert(!found&&!file);
    fail_directory_read=0;expect_file("io_old",0);
    assert(sd_audio_catalog_pin("io_old",0)==ESP_OK);
    assert(sd_audio_catalog_init()==ESP_ERR_NOT_FINISHED);
    sd_audio_catalog_unpin();expect_file("io_old",0);
    uint8_t foreign[32]={0x52};
    assert(sd_monitor_fifo_next(foreign,&scene,&part,&found,&file,&wait)==ESP_OK&&!found);
    expect_file("io_old",0);
    sd_monitor_fifo_reset();fail_manifest="catalog/io_old.session";
    assert(sd_monitor_fifo_next(owner,&scene,&part,&found,&file,&wait)==ESP_FAIL&&!found&&!file);
    expect_file("io_old",0); /* Load failure must not move the scan cursor past old. */
    puts("PASS FIFO I/O: directory error is not EOF or permission to skip; pinned init and owner switch preserve sources and correct queue");
    leave();enter();scene=make_scene("io_end",1,3,10,7,1000,true,true);
    assert(sd_audio_catalog_ack(scene.session_id,0)==ESP_OK&&sd_audio_catalog_ack(scene.session_id,2)==ESP_OK);
    assert(sd_audio_catalog_load(scene.session_id,&scene)==ESP_OK);scene.first_pending=3;
    char path[128];assert(catalog_path(path,scene.session_id,"session"));FILE *f=fopen(path,"r+b");assert(f);
    assert(write_pair(f,0,&scene,sizeof(scene),offsetof(sd_audio_session_t,crc))==ESP_OK&&fclose(f)==0);
    bool ready;
    fail_directory_open=1;
    assert(sd_monitor_fifo_end_ready(&scene,&ready)==ESP_FAIL&&!ready&&!monitor_fifo.end.active);
    assert(sd_monitor_fifo_end_ready(&scene,&ready)==ESP_ERR_INVALID_STATE&&!ready);
    sd_monitor_fifo_reset();directory_reads=0;fail_directory_read=1;
    assert(sd_monitor_fifo_end_ready(&scene,&ready)==ESP_FAIL&&!ready&&!monitor_fifo.end.active);
    fail_directory_read=0;
    assert(sd_monitor_fifo_end_ready(&scene,&ready)==ESP_ERR_INVALID_STATE&&!ready);
    assert(sd_audio_catalog_ack(scene.session_id,1)==ESP_OK);
    assert(sd_monitor_fifo_end_ready(&scene,&ready)==ESP_OK&&ready);
    assert(sd_audio_catalog_ack(NULL,0)==ESP_ERR_INVALID_ARG);
    assert(sd_audio_catalog_write(NULL,&part)==ESP_ERR_INVALID_ARG);
    puts("PASS FIFO I/O review: failed manifest retried before newer scene; failed open/readdir cannot publish end proof or hide unconfirmed middle; null IDs stay invalid arguments");
    leave();return 0;
}
