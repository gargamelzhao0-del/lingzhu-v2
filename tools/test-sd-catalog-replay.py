"""Exercise actual catalog selection reuse and durable ACK/delete boundaries."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
out = root/'build-host-checks/sd-catalog-replay'
out.mkdir(parents=True, exist_ok=True)
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <io.h>
static unsigned reads, updates, syncs, index_reads, manifest_reads;
static bool fail_open, fail_sync;
static FILE *observed_open(const char *p,const char *mode) {
    if(!strcmp(mode,"rb")) {
        reads++;if(strstr(p,".index"))index_reads++;
        if(strstr(p,".session"))manifest_reads++;
    }
    if(!strcmp(mode,"r+b")) {updates++;if(fail_open)return NULL;}
    return fopen(p,mode);
}
static int observed_sync(int fd) {syncs++;return fail_sync?-1:_commit(fd);}
#define fopen observed_open
#include "sd_audio_catalog.c"
#undef fopen
// Interpose _commit only in write/ACK production code via the generated source.
static sd_audio_session_t create_owned_mode(const char *id,unsigned count,unsigned mode,uint8_t owner) {
    sd_audio_session_t s={0};strcpy(s.session_id,id);memset(s.owner,owner,32);s.mode=mode;
    s.complete=1;s.start_ms=1000;s.ended_ms=2000;s.boot_id=7;
    assert(sd_audio_catalog_create(&s)==ESP_OK);
    for(unsigned i=0;i<count;i++) {
        sd_audio_segment_t p={0};snprintf(p.path,sizeof(p.path),"%s_%u.ready",id,i);
        assert(sd_audio_catalog_reserve(&s,&p)==ESP_OK);
        FILE *f=fopen(p.path,"wb");assert(f);fputs("audio",f);fclose(f);
        p.sealed=1;p.total_bytes=5;assert(sd_audio_catalog_write(id,&p)==ESP_OK);
    }
    s.ended=mode==0;assert(sd_audio_catalog_save(&s)==ESP_OK);return s;
}
static sd_audio_session_t create(const char *id,unsigned count) {return create_owned_mode(id,count,0,0x31);}
static sd_audio_segment_t select_file(sd_audio_session_t *s, unsigned index) {
    bool found,file;sd_audio_session_t out;sd_audio_segment_t p;
    assert(sd_audio_catalog_next(s->owner,7,10000,0,NULL,false,0,&out,&p,&found,&file)==ESP_OK);
    assert(found && file && p.index==index);return p;
}
int main(void) {
    assert(sd_audio_catalog_init()==ESP_OK);
    sd_audio_session_t s=create("meeting_test",3), bound;
    sd_audio_segment_t p=select_file(&s,0), part;bool reused;
    unsigned before=reads;
    assert(sd_audio_catalog_bind(s.session_id,0,&bound,&part,&reused)==ESP_OK && reused);
    assert(reads==before && !strcmp(part.path,p.path) && !memcmp(bound.owner,s.owner,32));
    assert(sd_audio_catalog_bind(s.session_id,0,&bound,&part,&reused)==ESP_OK && !reused && reads==before+2);
    puts("PASS: one-use selection removes two opens/CRC-record reads; retry reloads");

    select_file(&s,0);s.complete=0;assert(sd_audio_catalog_save(&s)==ESP_OK);
    assert(sd_audio_catalog_bind(s.session_id,0,&bound,&part,&reused)==ESP_OK && !reused && !bound.complete);
    select_file(&s,0);fail_open=true;assert(sd_audio_catalog_save(&s)!=ESP_OK);fail_open=false;
    assert(sd_audio_catalog_bind(s.session_id,0,&bound,&part,&reused)==ESP_OK && !reused);
    select_file(&s,0);sd_audio_catalog_replay_reset();
    assert(sd_audio_catalog_bind(s.session_id,0,&bound,&part,&reused)==ESP_OK && !reused);
    select_file(&s,0);s.owner[0]^=1;assert(sd_audio_catalog_save(&s)==ESP_ERR_INVALID_STATE);
    s.owner[0]^=1;
    assert(sd_audio_catalog_bind(s.session_id,0,&bound,&part,&reused)==ESP_OK && !reused && bound.owner[0]==s.owner[0]);
    select_file(&s,0);p.missing=1;assert(sd_audio_catalog_write(s.session_id,&p)==ESP_OK);
    assert(sd_audio_catalog_bind(s.session_id,0,&bound,&part,&reused)==ESP_OK && !reused && part.missing);
    p.missing=0;assert(sd_audio_catalog_write(s.session_id,&p)==ESP_OK);
    select_file(&s,0);assert(sd_audio_catalog_bind(s.session_id,1,&bound,&part,&reused)==ESP_OK && !reused && part.index==1);
    puts("PASS: owner changes rejected; metadata update, failed mutation, missing file, remount/reset and identity mismatch invalidate snapshot");

    select_file(&s,0);before=reads;unsigned previous_updates=updates;
    fail_sync=true;assert(sd_audio_catalog_ack(s.session_id,0)==ESP_FAIL);
    sd_audio_catalog_note_deleted(s.session_id,0);assert(replay_cursor[0]==0);
    // ACK visible in buffered data after failed sync still needs a successful sync.
    assert(sd_audio_catalog_ack(s.session_id,0)==ESP_FAIL);fail_sync=false;
    assert(sd_audio_catalog_ack(s.session_id,0)==ESP_OK);
    assert(reads==before && updates==previous_updates+3);
    assert(remove(p.path)==0);sd_audio_catalog_note_deleted(s.session_id,0);
    assert(replay_cursor[0]==1);before=reads;
    p=select_file(&s,1);assert(reads==before+2); // session + next segment; no old ACK read/unlink
    assert(sd_audio_catalog_ack(s.session_id,1)==ESP_OK);
    // No deletion notification: old acknowledged file must be cleaned on the next selection.
    p=select_file(&s,2);assert(access("meeting_test_1.ready",0)!=0);
    assert(sd_audio_catalog_ack(s.session_id,2)==ESP_OK);
    assert(remove(p.path)==0);sd_audio_catalog_note_deleted(s.session_id,2);
    sd_audio_catalog_replay_reset(); // Reboot loses hints, but durable ACKs suffice.
    bool found,file;
    assert(sd_audio_catalog_next(s.owner,7,10000,0,NULL,false,0,&bound,&p,&found,&file)==ESP_OK && found && !file);
    assert(bound.first_pending==3 && !bound.done);
    puts("PASS: ACK update uses one descriptor; failed fsync cannot authorize delete/cursor; failed/missing unlink and reboot retain durable recovery");

    /* Exhaust a slice at an unsealed monitor tail, then at a second scene's
     * ACK cursor. Neither changing candidates nor a stale cache may reset the
     * budget, and a short partial cursor must survive the next call. */
    sd_audio_session_t tail=create_owned_mode("budget_a_tail",17,1,0x62), next=create_owned_mode("budget_b_next",16,1,0x62);
    tail.ended_ms=1;next.ended_ms=2;
    assert(sd_audio_catalog_save(&tail)==ESP_OK && sd_audio_catalog_save(&next)==ESP_OK);
    for(unsigned i=0;i<16;i++) assert(sd_audio_catalog_ack(tail.session_id,i)==ESP_OK);
    for(unsigned i=0;i<15;i++) assert(sd_audio_catalog_ack(next.session_id,i)==ESP_OK);
    assert(sd_audio_catalog_read(tail.session_id,16,&part)==ESP_OK);part.sealed=0;
    assert(sd_audio_catalog_write(tail.session_id,&part)==ESP_OK);
    index_reads=0;
    assert(sd_audio_catalog_next(tail.owner,7,10000,0,NULL,false,1,&bound,&p,&found,&file)==ESP_ERR_NOT_FINISHED);
    assert(index_reads==16 && !found);
    index_reads=0;
    assert(sd_audio_catalog_next(tail.owner,7,10000,0,NULL,false,1,&bound,&p,&found,&file)==ESP_ERR_NOT_FINISHED);
    assert(index_reads==16 && !found && replay_cursor[1]==15);
    index_reads=0;
    assert(sd_audio_catalog_next(tail.owner,7,10000,0,NULL,false,1,&bound,&p,&found,&file)==ESP_OK);
    assert(index_reads==1 && found && file && !strcmp(bound.session_id,next.session_id) && p.index==15);
    assert(remove("catalog/budget_b_next.session")==0); // Stale pick, outside catalog invalidation.
    index_reads=0;
    assert(sd_audio_catalog_next(tail.owner,7,10000,0,NULL,false,1,&bound,&p,&found,&file)==ESP_OK && !found);
    assert(index_reads<=16);
    part.sealed=1;assert(sd_audio_catalog_write(tail.session_id,&part)==ESP_OK);
    index_reads=0;
    assert(sd_audio_catalog_next(tail.owner,7,10000,0,NULL,false,1,&bound,&p,&found,&file)==ESP_OK && found && file);
    assert(index_reads==1 && !strcmp(bound.session_id,tail.session_id) && p.index==16);
    puts("PASS: shared 16-record budget, unsealed tail, short partial cursor, stale cached manifest and sealing after empty hint");
    part.sealed=0;assert(sd_audio_catalog_write(tail.session_id,&part)==ESP_OK);
    sd_audio_session_t last=create_owned_mode("budget_z_good",1,1,0x62);
    last.ended_ms=3;
    assert(sd_audio_catalog_save(&last)==ESP_OK);
    for(unsigned i=0;i<40;i++) {char id[48];snprintf(id,sizeof(id),"foreign_scan_%02u",i);create(id,0);}
    unsigned calls=0;esp_err_t err;
    do {
        index_reads=manifest_reads=0;
        err=sd_audio_catalog_next(tail.owner,7,10000,0,NULL,false,1,&bound,&p,&found,&file);
        assert(index_reads<=16 && manifest_reads<=16 && ++calls<16);
        assert(err==ESP_OK || (err==ESP_ERR_NOT_FINISHED && !found && !replay_empty[1]));
    } while(err==ESP_ERR_NOT_FINISHED);
    assert(found && file && !strcmp(bound.session_id,last.session_id) && calls>3);
    sd_audio_catalog_replay_reset();
    assert(sd_audio_catalog_next(tail.owner,7,10000,0,NULL,false,1,&bound,&p,&found,&file)==ESP_ERR_NOT_FINISHED);
    // A writer mutation between slices invalidates the accumulated candidate.
    sd_audio_session_t earlier=create_owned_mode("budget_earlier_new",1,1,0x62);
    earlier.ended_ms=0;
    assert(sd_audio_catalog_save(&earlier)==ESP_OK);
    calls=0;
    do {
        manifest_reads=0;
        err=sd_audio_catalog_next(tail.owner,7,10000,0,NULL,false,1,&bound,&p,&found,&file);
        assert(manifest_reads<=16 && ++calls<16);
    } while(err==ESP_ERR_NOT_FINISHED);
    assert(err==ESP_OK && found && file && !strcmp(bound.session_id,earlier.session_id));
    sd_audio_catalog_replay_reset();
    assert(sd_audio_catalog_next(tail.owner,7,10000,0,NULL,false,1,&bound,&p,&found,&file)==ESP_ERR_NOT_FINISHED);
    const uint8_t foreign_owner[32]={0};calls=0;
    do {err=sd_audio_catalog_next(foreign_owner,7,10000,0,NULL,false,1,&bound,&p,&found,&file);assert(++calls<16);}while(err==ESP_ERR_NOT_FINISHED);
    assert(err==ESP_OK && !found); // Owner change cannot reuse a partial scan.
    puts("PASS: bounded manifest scan across yields; lower-key continuation; new scene invalidates partial selection; partial owner isolation");
}
'''
# Use the actual production source, with only its OS sync primitive intercepted.
source=(root/'main/sd_audio_catalog.c').read_text(encoding='utf-8').replace('#define fsync _commit','#define fsync observed_sync')
(out/'sd_audio_catalog.c').write_text(source,encoding='utf-8')
(out/'check.c').write_text(code,encoding='utf-8')
exe=out/'check.exe'
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-DSD_AUDIO_CATALOG_ROOT="catalog"',
    '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105','-I'+str(root/'main'),
    '-I'+str(root/'main/audio_modes/meeting/host_stubs'),str(out/'check.c'),'-o',str(exe)],check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-catalog-replay-') as d:
    subprocess.run([str(exe)],cwd=d,check=True)
