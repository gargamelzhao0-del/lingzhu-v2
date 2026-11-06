#include "sd_audio_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    assert(sd_audio_catalog_init()==ESP_OK);
    sd_audio_session_t old={.mode=1,.boot_id=7,.start_ms=1000,.complete=1};
    strcpy(old.session_id,"monitor_old");memset(old.owner,0x31,32);
    assert(sd_audio_catalog_create(&old)==ESP_OK);
    old.ended=1;old.ended_ms=2400000;
    assert(sd_audio_catalog_save(&old)==ESP_OK);
    sd_audio_session_t active={.mode=1,.boot_id=8,.start_ms=100,.complete=1};
    strcpy(active.session_id,"monitor_active");memset(active.owner,0x31,32);
    assert(sd_audio_catalog_create(&active)==ESP_OK);
    sd_audio_segment_t file={.start_ms=1000,.end_ms=1960,.realtime=1};
    strcpy(file.path,"retained.ready");
    assert(sd_audio_catalog_reserve(&active,&file)==ESP_OK);
    file.sealed=1;file.total_bytes=32096;
    assert(sd_audio_catalog_write(active.session_id,&file)==ESP_OK);
    sd_audio_session_t found;sd_audio_segment_t part;bool scene,data;
    assert(sd_audio_catalog_next(active.owner,8,2200,0,active.session_id,true,-1,&found,&part,&scene,&data)==ESP_OK && data);
    assert(sd_audio_catalog_defer(&old)==ESP_OK);
    assert(sd_audio_catalog_next(active.owner,8,2200,0,active.session_id,true,-1,&found,&part,&scene,&data)==ESP_OK && scene && data);
    assert(sd_audio_catalog_next(active.owner,8,5000,0,NULL,false,1,&found,&part,&scene,&data)==ESP_OK && scene && data);
    active.ended=1;active.ended_ms=422327;
    assert(sd_audio_catalog_save(&active)==ESP_OK);
    assert(sd_audio_catalog_next(active.owner,8,500000,0,NULL,false,1,&found,&part,&scene,&data)==ESP_OK && scene && data);
    sd_audio_catalog_replay_reset();
    assert(sd_audio_catalog_next(active.owner,8,500000,0,NULL,false,1,&found,&part,&scene,&data)==ESP_OK && data);
    assert(!strcmp(found.session_id,active.session_id));
    puts("PASS: failed old scene does not hide current live/history or later-boot scenes.");
    // Deferring the same scene is idempotent; reject overflow without broad skipping.
    for(unsigned i=0;i<16;i++) {
        snprintf(old.session_id,sizeof(old.session_id),"failed_%u",i);
        assert(sd_audio_catalog_defer(&old)==ESP_OK);
        assert(sd_audio_catalog_defer(&old)==ESP_OK);
    }
    strcpy(old.session_id,"failed_overflow");
    assert(sd_audio_catalog_defer(&old)==ESP_ERR_NO_MEM);
    /* replay_reset above made monitor_old eligible again. Overflow rotation
     * may select its empty session_end before active's file: complete that
     * legitimate transaction instead of assuming the next pick is a file. */
    for (unsigned attempts=0;attempts<3U;attempts++) {
        const esp_err_t err=sd_audio_catalog_next(active.owner,8,500000,0,NULL,false,1,&found,&part,&scene,&data);
        assert(err==ESP_OK || err==ESP_ERR_NOT_FINISHED);
        if(err==ESP_OK && data)break;
        if(err==ESP_OK && scene) {
            assert(!strcmp(found.session_id,"monitor_old") && found.ended && !found.segment_count);
            found.done=1;assert(sd_audio_catalog_save(&found)==ESP_OK);
        }
    }
    assert(data);
    assert(!strcmp(found.session_id,active.session_id));
    sd_audio_catalog_replay_reset();assert(sd_audio_catalog_defer(&old)==ESP_OK);
    return 0;
}
