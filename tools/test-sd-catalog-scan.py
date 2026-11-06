"""Real catalog I/O: cross-mode miss reuse, invalidation and scan bounds."""
from pathlib import Path
import subprocess, tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-catalog-scan'
out.mkdir(parents=True, exist_ok=True)
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned manifests;
static FILE *observed_open(const char *p,const char *mode) {
    if(strstr(p,".session") && !strcmp(mode,"rb")) manifests++;
    return fopen(p,mode);
}
#define fopen observed_open
#include "sd_audio_catalog.c"
#undef fopen
static sd_audio_session_t make(const char *id,unsigned mode,bool done) {
    sd_audio_session_t s={0};strcpy(s.session_id,id);memset(s.owner,0x31,32);
    s.mode=mode;s.complete=1;s.ended=done;s.done=done;s.boot_id=7;
    s.start_ms=1000;s.ended_ms=2000;assert(sd_audio_catalog_create(&s)==ESP_OK);return s;
}
static void append(sd_audio_session_t *s,bool sealed) {
    sd_audio_segment_t p={0};strcpy(p.path,"absent-but-not-acked.ready");
    assert(sd_audio_catalog_reserve(s,&p)==ESP_OK);p.sealed=sealed;p.total_bytes=32096;
    assert(sd_audio_catalog_write(s->session_id,&p)==ESP_OK);
}
static void lookup(const uint8_t *owner,int mode,bool expected) {
    sd_audio_session_t s;sd_audio_segment_t p;bool have,file;
    esp_err_t err;unsigned steps=0;
    do {
        err=sd_audio_catalog_next(owner,7,10000,0,NULL,false,mode,&s,&p,&have,&file);
        assert(++steps<1024U);
    } while(err==ESP_ERR_NOT_FINISHED);
    assert(err==ESP_OK);
    assert(have==expected);
}
int main(void) {
    assert(sd_audio_catalog_init()==ESP_OK);
    for(unsigned i=0;i<30;i++){char id[48];sprintf(id,"done_%u",i);make(id,i%2,true);}
    sd_audio_session_t m=make("meeting",0,false);
    append(&m,true);append(&m,true);
    m.ended=1;assert(sd_audio_catalog_save(&m)==ESP_OK);
    sd_audio_catalog_replay_reset();manifests=0;lookup(m.owner,0,true);
    unsigned full=manifests;assert(full==31);
    lookup(m.owner,1,false);assert(manifests==full); // old code re-read 31 manifests
    lookup(m.owner,0,true);assert(manifests==full+1);
    printf("PASS: alternating empty-mode scan eliminated %u manifest reads; selected scene still revalidated\n",full);

    // A new monitor invalidates the negative hint, even before sealing.
    sd_audio_session_t mon=make("monitor",1,false);append(&mon,false);
    sd_audio_catalog_replay_reset();lookup(m.owner,0,true);
    assert(!replay_empty[1]);lookup(m.owner,1,false);
    sd_audio_segment_t p;assert(sd_audio_catalog_read(mon.session_id,0,&p)==ESP_OK);
    p.sealed=1;assert(sd_audio_catalog_write(mon.session_id,&p)==ESP_OK);
    lookup(m.owner,1,true);
    // A miss for another account must not conceal this account's files.
    uint8_t other[32]={0};lookup(other,0,false);lookup(m.owner,1,true);
    sd_audio_catalog_defer(&mon);lookup(m.owner,1,false);
    sd_audio_catalog_replay_reset();lookup(m.owner,1,true);

    // A sliced index walk cannot claim to have exhausted all manifests.
    for(unsigned i=0;i<m.segment_count;i++) assert(sd_audio_catalog_ack(m.session_id,i)==ESP_OK);
    m.done=1;assert(sd_audio_catalog_save(&m)==ESP_OK);
    m=make("meeting_sliced",0,false);
    for(unsigned i=0;i<20;i++) append(&m,true);
    for(unsigned i=0;i<m.segment_count;i++) assert(sd_audio_catalog_ack(m.session_id,i)==ESP_OK);
    m.ended=1;assert(sd_audio_catalog_save(&m)==ESP_OK);
    sd_audio_catalog_replay_reset();bool found,file;sd_audio_session_t selected;
    assert(sd_audio_catalog_next(m.owner,7,10000,0,NULL,false,0,&selected,&p,&found,&file)==ESP_ERR_NOT_FINISHED);
    assert(!replay_empty[1]);lookup(m.owner,1,true);
    puts("PASS: new/unsealed/sealed scene, account change, defer/reset and incomplete scan preserve eligibility");
}
'''
(out/'scan.c').write_text(code, encoding='utf-8')
exe = out/'scan.exe'
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
    '-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),
    str(out/'scan.c'),'-o',str(exe)],check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-sd-scan-') as directory:
    subprocess.run([str(exe)],cwd=directory,check=True)
