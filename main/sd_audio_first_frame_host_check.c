/* Actual archive writer + actual durable catalog, not a readiness mock. */
#define main previous_storage_controls
#include "sd_audio_storage_host_check.c"
#undef main
#include "offline_capture_health.h"
static spool_item_t item(unsigned sequence)
{
    int16_t pcm[320]={0};pcm[0]=(int16_t)sequence;
    spool_item_t result={.type=1U,.generation=1U,.realtime=true};
    assert(nuwax_audio_frame_build_pcm16(result.data,sizeof(result.data),pcm,320U,sequence,
        (uint32_t)clock_ms,0U,16000U,1U,20U,&result.bytes)==ESP_OK);
    return result;
}
int main(void)
{
    assert(sd_audio_catalog_init()==ESP_OK);
    begin("meeting_crc_first",0U);
    char path[128];snprintf(path,sizeof(path),"catalog/%s.session",s.session_id);
    FILE *file=fopen(path,"r+b");assert(file);uint8_t zero[2*sizeof(s_card_session)]={0};
    assert(fwrite(zero,1U,sizeof(zero),file)==sizeof(zero)&&!fclose(file));
    spool_item_t frame0=item(0U);
    assert(append_locked(&frame0)==ESP_ERR_INVALID_CRC && !s.file && !s.writer_frame_written && !s.archive_ready);
    s.degraded=true;s.capture_complete=false;s.writer_error=true;
    assert(nuwax_offline_capture_admission_health(NUWAX_OFFLINE_CAPTURE_BUFFERING,true,s.archive_ready)==NUWAX_OFFLINE_CAPTURE_STARTING);
    assert(sd_audio_catalog_prepare_active(&s_card_session)==ESP_OK);
    assert(sd_audio_catalog_recover_active(123U,s_card_session.session_id)==ESP_OK);
    assert(sd_audio_catalog_load(s.session_id,&s_card_session)==ESP_OK);
    s.writer_error=false;
    assert(append_locked(&frame0)==ESP_OK && s.writer_frame_written && s.archive_ready);
    assert(nuwax_offline_capture_admission_health(NUWAX_OFFLINE_CAPTURE_RECORDING,true,s.archive_ready)==NUWAX_OFFLINE_CAPTURE_RECORDING);
    assert(commit_segment_locked(false)==ESP_OK);sd_audio_segment_t saved;
    assert(sd_audio_catalog_read(s.session_id,1U,&saved)==ESP_OK);
    assert(saved.total_bytes==HEADER_BYTES+666U+FOOTER_BYTES);
    file=fopen(saved.path,"rb");assert(file);archive_header_t header;archive_footer_t footer;
    assert(fread(&header,1U,sizeof(header),file)==sizeof(header)&&header.segment_index==1U&&!strcmp(header.session_id,s.session_id));
    uint32_t length;uint8_t payload[662];assert(fread(&length,1U,4U,file)==4U&&length==662U);
    assert(fread(payload,1U,662U,file)==662U&&!memcmp(payload,frame0.data,662U));
    assert(fread(&footer,1U,sizeof(footer),file)==sizeof(footer)&&footer.frame_count==1U&&!footer.complete);
    assert(footer.crc32==crc32_update(0U,frame0.data,662U)&&!fclose(file));
    puts("PASS actual writer: first reserve CRC failure cannot activate; checkpoint recovery retries the same PCM once, with durable first frame and unchanged bytes/timestamps");
    begin("monitor_sync_first",1U);fail_first_frame_sync=true;frame0=item(0U);
    assert(append_locked(&frame0)==ESP_FAIL && s.writer_frame_written && !s.archive_ready);
    assert(nuwax_offline_capture_admission_health(NUWAX_OFFLINE_CAPTURE_BUFFERING,true,false)==NUWAX_OFFLINE_CAPTURE_STARTING);
    fail_first_frame_sync=false;assert(commit_segment_locked(false)==ESP_OK);
    assert(nuwax_offline_capture_admission_health(NUWAX_OFFLINE_CAPTURE_UNAVAILABLE,true,false)==NUWAX_OFFLINE_CAPTURE_UNAVAILABLE);
    assert(nuwax_offline_capture_admission_health(NUWAX_OFFLINE_CAPTURE_BUFFERING,false,false)==NUWAX_OFFLINE_CAPTURE_BUFFERING);
    puts("PASS admission: first-frame sync failure never publishes recording; unavailable deadline and cardless buffering behavior retained");
    return 0;
}
