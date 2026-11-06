#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include "sd_audio_catalog.c"
#include "audio_frame.h"
#define ESP_LOGW(...) ((void)0)
static struct {atomic_bool capture_complete;} s;
#include "sd_audio_file_fault_host.inc"

static void put16(uint8_t *p,uint16_t value) {memcpy(p,&value,sizeof(value));}
static void put32(uint8_t *p,uint32_t value) {memcpy(p,&value,sizeof(value));}
static void put64(uint8_t *p,uint64_t value) {memcpy(p,&value,sizeof(value));}
int main(void)
{
    assert(sd_audio_catalog_init()==ESP_OK);
    sd_audio_session_t scene={.mode=0,.complete=1,.ended=1,.segment_count=1,
        .boot_id=7,.start_ms=1000,.ended_ms=1060};
    strcpy(scene.session_id,"meeting_integrity");memset(scene.owner,0x31,sizeof(scene.owner));
    assert(sd_audio_catalog_create(&scene)==ESP_OK);
    sd_audio_segment_t part={.index=0,.sealed=1,.start_ms=1000,.end_ms=1060,
        .total_bytes=96+3*666+32};
    strcpy(part.path,"./card_meeting_integrity_00000000.ready");
    uint8_t archive[96+3*666+32]={0},payloads[3*662];
    memcpy(archive,"NWXOFF1",7);put16(archive+8,1);put16(archive+10,96);
    archive[13]=1;put16(archive+14,1);put32(archive+16,16000);
    put16(archive+20,20);put16(archive+22,2);put64(archive+28,1000);
    strcpy((char*)archive+36,scene.session_id);put32(archive+92,60);
    int16_t pcm[320];for(unsigned i=0;i<320;i++)pcm[i]=(int16_t)(i*17U);
    for(unsigned i=0;i<3;i++) {
        uint8_t *record=archive+96+i*666;size_t bytes=0;put32(record,662);
        assert(nuwax_audio_frame_build_pcm16(record+4,662,pcm,320,i,1000+i*20,
            i==0?NUWAX_AUDIO_FLAG_START:0,16000,1,20,&bytes)==ESP_OK && bytes==662);
        memcpy(payloads+i*662,record+4,662);
    }
    part.payload_crc=checksum(payloads,sizeof(payloads));
    uint8_t *footer=archive+sizeof(archive)-32;memcpy(footer,"NWXEND1",7);
    put16(footer+8,1);put16(footer+10,32);put32(footer+12,3);
    put64(footer+16,sizeof(payloads));put32(footer+24,part.payload_crc);footer[28]=1;
    /* Damage only PCM in the middle: all immutable metadata/last frame stays
     * valid, exactly the case metadata-only repair cannot distinguish. */
    archive[96+4+22+100]^=0x80U;
    FILE *file=fopen(part.path,"wb");assert(file);
    assert(fwrite(archive,1,sizeof(archive),file)==sizeof(archive) && !fclose(file));
    assert(sd_audio_catalog_write(scene.session_id,&part)==ESP_OK);
#ifdef NEGATIVE_KEEP_BAD_READY
    assert(sd_audio_catalog_mark_missing(scene.session_id,part.index)==ESP_OK);
#else
    assert(card_confirm_missing_locked(&scene,&part,true)==ESP_ERR_NOT_FINISHED);
    fault_media_healthy=true;atomic_fetch_add(&s_media_generation,1U);
    assert(card_confirm_missing_locked(&scene,&part,true)==ESP_OK);
#endif
    assert(sd_audio_catalog_recover(8U)==ESP_OK);
    sd_audio_segment_t checked;
    assert(sd_audio_catalog_read(scene.session_id,0,&checked)==ESP_OK && !checked.acknowledged);
    sd_audio_session_t restored;assert(sd_audio_catalog_load(scene.session_id,&restored)==ESP_OK && !restored.complete);
#ifdef NEGATIVE_KEEP_BAD_READY
    assert(!checked.missing);
    puts("PASS negative control: real catalog resurrects a CRC-damaged .ready after missing was persisted");
#else
    assert(checked.missing==SD_AUDIO_SEGMENT_RETIRED);
    struct stat st;assert(stat(part.path,&st)!=0&&errno==ENOENT);
    /* Model power loss after discard persistence, before physical unlink.
     * The original bad body must never be resurrected by archive recovery. */
    file=fopen(part.path,"wb");assert(file&&fwrite(archive,1,sizeof(archive),file)==sizeof(archive)&&!fclose(file));
    assert(sd_audio_catalog_recover(9U)==ESP_OK);
    assert(sd_audio_catalog_read(scene.session_id,0,&checked)==ESP_OK && checked.missing==SD_AUDIO_SEGMENT_RETIRED && !checked.acknowledged);
    assert(stat(part.path,&st)!=0&&errno==ENOENT);
    puts("PASS production discard + real catalog: bad PCM removed after terminal non-ACK marker; reset before unlink never resurrects it");
    part.index=1;strcpy(part.path,"./card_meeting_integrity_00000001.ready");
    scene.segment_count=2U;assert(sd_audio_catalog_save(&scene)==ESP_OK);
    assert(sd_audio_catalog_write(scene.session_id,&part)==ESP_OK);
    file=fopen(part.path,"wb");assert(file);assert(fputc(0xaa,file)!=EOF && !fclose(file));
    file=fopen("./card_meeting_integrity_00000001.ready.bad","wb");assert(file);assert(fputc(0x55,file)!=EOF && !fclose(file));
    fault_active_capture=true;s_card_session=scene;s.capture_complete=true;fault_stop_requested=false;
    assert(card_confirm_missing_locked(&scene,&part,true)==ESP_ERR_NOT_FINISHED);
    fault_media_healthy=true;atomic_fetch_add(&s_media_generation,1U);
    assert(card_confirm_missing_locked(&scene,&part,true)==ESP_OK && !s_card_failed && s.capture_complete && !fault_stop_requested);
    assert(stat(part.path,&st)!=0&&errno==ENOENT);
    file=fopen("./card_meeting_integrity_00000001.ready.bad","rb");assert(file);assert(fgetc(file)==0x55 && !fclose(file));
    assert(sd_audio_catalog_read(scene.session_id,1U,&checked)==ESP_OK&&checked.missing==SD_AUDIO_SEGMENT_RETIRED&&!checked.acknowledged);
    puts("PASS preexisting diagnostic collision: exact corrupt source discarded, other bytes untouched, active recording continues");
#endif
    return 0;
}
