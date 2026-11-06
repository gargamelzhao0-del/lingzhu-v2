/* Production FIFO, selection, end JSON and catalog; wire ACK is the existing
 * bounded fixture. The separate streaming sender tests check actual bytes. */
#define SD_FIFO_REPLAY_HELPERS_ONLY 1
#include "sd_monitor_fifo_replay_host_check.c"
#include "audio_frame.h"

static sd_audio_segment_t append_file(sd_audio_session_t *writer, unsigned physical, bool sealed)
{
    sd_audio_segment_t part = {.start_ms=writer->start_ms + writer->segment_count*20U};
    part.end_ms = part.start_ms + 20U;
    snprintf(part.path,sizeof(part.path),"audio/card_%s_%08u.ready",writer->session_id,physical);
    assert(sd_audio_catalog_reserve(writer,&part)==ESP_OK);
    uint8_t bytes[794]={0};int16_t pcm[320];for(unsigned i=0;i<320;i++)pcm[i]=(int16_t)(i+part.index);
    memcpy(bytes,"NWXOFF1",7);bytes[8]=1;bytes[10]=96;bytes[13]=bytes[14]=1;
    put32(bytes+16,16000);bytes[20]=20;bytes[22]=2;put32(bytes+24,part.index);put64(bytes+28,part.start_ms);
    strcpy((char*)bytes+36,writer->session_id);put32(bytes+96,662);
    size_t frame_bytes=0;assert(nuwax_audio_frame_build_pcm16(bytes+100,662,pcm,320,part.index,
        (uint32_t)part.start_ms,0,16000,1,20,&frame_bytes)==ESP_OK&&frame_bytes==662);
    uint8_t *footer=bytes+762;memcpy(footer,"NWXEND1",7);footer[8]=1;footer[10]=32;
    put32(footer+12,1);put64(footer+16,662);put32(footer+24,checksum(bytes+100,662));footer[28]=1;
    FILE *f=fopen(part.path,"wb");assert(f&&fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes)&&!fclose(f));
    if(sealed){part.sealed=1;part.total_bytes=sizeof(bytes);part.payload_crc=checksum(bytes+100,662);
        assert(sd_audio_catalog_write(writer->session_id,&part)==ESP_OK);}
    atomic_store(&s_card_pending_upload,true);return part;
}

static void publish(sd_audio_session_t *scene,sd_audio_segment_t *part)
{
    FILE *f=fopen(part->path,"rb");assert(f);uint8_t bytes[794];
    assert(fread(bytes,1,sizeof(bytes),f)==sizeof(bytes)&&!fclose(f));
    part->sealed=1;part->total_bytes=sizeof(bytes);part->payload_crc=checksum(bytes+100,662);
    assert(sd_audio_catalog_write(scene->session_id,part)==ESP_OK);
    atomic_store(&s_card_pending_upload,true);
}

int main(void)
{
    enter();nuwax_ws_client_t ws={10,true};
    sd_audio_session_t writer=make_scene("stream_meeting",0,0,100,7,1000,false,false);
    append_file(&writer,100,true);append_file(&writer,101,true);
    sd_audio_segment_t tail=append_file(&writer,102,false);
    const unsigned initial_ends=end_sends;
    step(&ws);step(&ws);assert(meeting_sent==2&&end_sends==initial_ends);
    sd_audio_session_t current;assert(sd_audio_catalog_load(writer.session_id,&current)==ESP_OK);
    assert(!current.ended&&!current.done&&current.first_pending>=1&&current.segment_count==3);
    step(&ws);assert(meeting_sent==2&&!s_card_pending_upload);
    bool ready=true;assert(sd_meeting_fifo_end_ready(&current,&ready)==ESP_OK&&!ready);
    /* Stale writer state merges ACK progress; it cannot resurrect deleted files. */
    sd_audio_segment_t later=append_file(&writer,103,true);
    assert(writer.first_pending==2&&writer.segment_count==4);
    step(&ws);assert(meeting_sent==2); /* Later sealed index 3 cannot pass unsealed 2. */
    publish(&writer,&tail);step(&ws);assert(meeting_sent==3);
    step(&ws);assert(meeting_sent==4&&end_sends==initial_ends);
    step(&ws);assert(!s_card_pending_upload);
    assert(sd_audio_catalog_load(writer.session_id,&current)==ESP_OK&&!current.ended&&!current.done);
    current.ended=1;current.ended_ms=1080;assert(sd_audio_catalog_save(&current)==ESP_OK);
    atomic_store(&s_card_pending_upload,true);step(&ws);
    assert(end_sends==initial_ends+1&&sd_audio_catalog_load(writer.session_id,&current)==ESP_OK&&current.done);
    assert(!strcmp(last_end,writer.session_id));(void)later;
    puts("PASS meeting rolling files: active upload, unsealed barrier, stale writer/ACK merge, no premature end, final tail and durable scene end");leave();

    enter();ws.epoch=20;clock_ms=4000;s_card_pending_upload=true;
    sd_audio_session_t old=make_scene("offline_old",0,2,10,0xfffffff0U,900000,true,false);
    sd_audio_session_t young=make_scene("online_young",0,1,30,1,100,false,false);
    make_scene("monitor_independent",1,1,20,1,100,true,false);
    const unsigned before=meeting_sent;fail_meeting_file=true;
    for(unsigned i=0;i<8;i++)step(&ws);
    assert(s_meeting_retry_delay==1000&&meeting_sent==before+1);
    assert(sd_audio_catalog_load(young.session_id,&current)==ESP_OK&&current.first_pending==0&&!current.done);
    assert(!strcmp(last_end,"monitor_independent"));
    fail_meeting_file=false;ws.epoch++;
    for(unsigned i=0;i<6;i++)step(&ws);
    assert(sd_audio_catalog_load(old.session_id,&current)==ESP_OK&&current.done);
    assert(sd_audio_catalog_load(young.session_id,&current)==ESP_OK&&!current.done&&!current.ended);
    sd_audio_segment_t p;assert(sd_audio_catalog_read(young.session_id,0,&p)==ESP_OK&&p.acknowledged);
    puts("PASS meeting FIFO recovery: old boot/files first, failed head backoff, independent monitoring, reconnect retry, young active file without premature end");leave();

    enter();ws.epoch=30;s_card_pending_upload=true;
    old=make_scene("lost_end_old",0,1,10,7,1000,true,false);
    young=make_scene("lost_end_young",0,1,20,8,10,true,false);
    fail_end_ack=true;const unsigned prior=meeting_sent;step(&ws);step(&ws);step(&ws);
    assert(meeting_sent==prior+1&&sd_audio_catalog_load(young.session_id,&current)==ESP_OK&&!current.done);
    assert(sd_audio_catalog_read(young.session_id,0,&p)==ESP_OK&&!p.acknowledged);
    fail_end_ack=false;ws.epoch++;
    for(unsigned i=0;i<5;i++)step(&ws);
    assert(sd_audio_catalog_load(old.session_id,&current)==ESP_OK&&current.done);
    assert(sd_audio_catalog_load(young.session_id,&current)==ESP_OK&&current.done);
    puts("PASS meeting lost end ACK: later meeting cannot pass; retries preserve original scene identity and finish in order");
    leave();
    enter();ws.epoch=40;s_card_pending_upload=true;
    writer=make_scene("empty_reservation",0,0,10,7,1000,false,false);
    tail=append_file(&writer,10,false);assert(!remove(tail.path));
    step(&ws);assert(sd_audio_catalog_read(writer.session_id,0,&p)==ESP_OK&&!p.missing);
    writer.ended=1;writer.ended_ms=1020;assert(sd_audio_catalog_save(&writer)==ESP_OK);
    young=make_scene("after_empty",0,1,20,8,10,true,false);s_card_pending_upload=true;
    for(unsigned i=0;i<6;i++)step(&ws);
    assert(sd_audio_catalog_load(writer.session_id,&current)==ESP_OK&&current.done&&!current.complete);
    assert(sd_audio_catalog_read(writer.session_id,0,&p)==ESP_OK&&p.missing&&!p.acknowledged);
    assert(sd_audio_catalog_load(young.session_id,&current)==ESP_OK&&current.done);
    puts("PASS empty reservation: active tail never skipped; ended/proven-absent hole becomes incomplete, not an invented ACK, then younger meeting progresses");
    leave();return 0;
}
