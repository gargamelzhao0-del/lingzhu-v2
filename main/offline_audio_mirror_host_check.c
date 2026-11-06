#include "offline_replay_policy.h"
/* Real staging/close code with only filesystem admission and RTOS scheduling faked. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "offline_audio_spool.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"

#define TAG "mirror_host"
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static void portENTER_CRITICAL(portMUX_TYPE *lock) { assert(!*lock); ++*lock; }
static void portEXIT_CRITICAL(portMUX_TYPE *lock) { assert(*lock == 1U); --*lock; }
static uint32_t clock_ms;
static atomic_uint s_mirror_closers;
static atomic_bool s_media_ready = true;
static atomic_bool s_space_blocked;
static bool available, writable, ended_complete;
static bool fail_once;
static uint32_t written, last_seq;
static void (*tail_fn)(void *);
static void *tail_arg;
static uint32_t offline_now_ms(void) { return clock_ms; }
static bool offline_deadline_elapsed(uint32_t now, uint32_t deadline) { return (int32_t)(now-deadline)>=0; }
static bool valid_session_id(const char *id) { return id && id[0]; }
void vTaskDelay(TickType_t ticks) { clock_ms += ticks; }
static void vTaskDeleteWithCaps(void *task) { (void)task; }
static int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name,
    unsigned stack, void *arg, unsigned priority, void *handle, int core, unsigned caps) {
    (void)name; (void)stack; (void)priority; (void)handle; (void)core; (void)caps;
    assert(!tail_fn); tail_fn=fn; tail_arg=arg; return pdPASS;
}
static esp_err_t begin_archive(nuwax_offline_audio_mode_t mode, const char *id,
    uint32_t rate, uint8_t ch, uint16_t ms, bool mirror, bool buffered) {
    (void)mode; (void)mirror; assert(id && rate==16000 && ch==1 && ms==20 && buffered);
    return available ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t nuwax_offline_audio_request_end(bool complete) { ended_complete=complete; return ESP_OK; }
void nuwax_offline_audio_supervisor_wake(void) {}
#include "offline_mirror_host.inc"
static esp_err_t append_frame(const nuwax_offline_audio_frame_t *f, TickType_t wait, bool loss) {
    assert(!wait && !loss && f->sample_count==320);
    if (fail_once) { fail_once=false; return ESP_ERR_INVALID_STATE; }
    if (!writable) return ESP_ERR_NOT_FINISHED;
    if (written) assert(f->sequence == last_seq+1U);
    assert(f->timestamp_ms==f->sequence*20U);
    assert(f->samples[0]==(int16_t)f->sequence);
    last_seq=f->sequence; ++written; return ESP_OK;
}
static void start(nuwax_offline_audio_mirror_t *m) {
    assert(nuwax_offline_audio_mirror_service(m, NUWAX_OFFLINE_AUDIO_MODE_MEETING,
        "original",16000,1,20,clock_ms)==ESP_OK);
}
static esp_err_t feed(nuwax_offline_audio_mirror_t *m, uint32_t seq) {
    int16_t samples[320] = {0}; samples[0]=(int16_t)seq;
    const nuwax_offline_audio_frame_t f={seq,seq*20U,0,samples,320};
    clock_ms+=20U;
    return nuwax_offline_audio_mirror_append(m,&f,clock_ms);
}
static void finish_tail(void) { void (*fn)(void*)=tail_fn; void *arg=tail_arg; tail_fn=NULL; fn(arg); }
int main(void) {
    nuwax_offline_audio_mirror_t m={0};
    /* A mounted but full card must not allocate a fresh staging ring. */
    atomic_store(&s_space_blocked,true);
    assert(nuwax_offline_audio_mirror_service(&m,NUWAX_OFFLINE_AUDIO_MODE_MEETING,
        "original",16000,1,20,clock_ms)==ESP_ERR_NOT_FINISHED);
    assert(!m.staging && !m.started);
    atomic_store(&s_space_blocked,false);
    nuwax_offline_audio_mirror_reset(&m);
    /* Boot without media: no large staging and no detached close task. */
    atomic_store(&s_media_ready, false);
    for (unsigned i=0;i<2000;i++) {
        assert(nuwax_offline_audio_mirror_service(&m, NUWAX_OFFLINE_AUDIO_MODE_MEETING,
            "original",16000,1,20,clock_ms)==ESP_ERR_NOT_FINISHED);
        assert(feed(&m,i)==ESP_ERR_NOT_FINISHED);
        assert(!m.staging && !m.started && !tail_fn);
    }
    assert(m.rejected_frames==2000 && !m.complete);
    nuwax_offline_audio_mirror_request_close(&m,true,clock_ms);
    assert(!tail_fn && !m.staging && !m.media_deferred);
    /* Card becomes ready within the same recording. Missing prefix must not
     * be advertised as a complete recording. */
    assert(nuwax_offline_audio_mirror_service(&m, NUWAX_OFFLINE_AUDIO_MODE_MEETING,
        "original",16000,1,20,clock_ms)==ESP_ERR_NOT_FINISHED);
    atomic_store(&s_media_ready, true); available=writable=true;
    start(&m); assert(!m.complete); feed(&m,2000);
    nuwax_offline_audio_mirror_request_close(&m,true,clock_ms);
    assert(!ended_complete && !m.staging);
    memset(&m,0,sizeof(m)); written=0; available=writable=false;
    start(&m);
    assert(atomic_load(&s_mirror_staging_rings)==1U);
    /* Reproduce the 153-frame archive-attachment gap without losing PCM. */
    for(unsigned i=0;i<153;i++) assert(feed(&m,i)==ESP_OK);
    assert(written==0 && m.staging_count==153 && !m.rejected_frames);
    available=writable=true;
    for(unsigned i=153;i<190;i++) assert(feed(&m,i)==ESP_OK);
    assert(written==190 && !m.staging_count);
    nuwax_offline_audio_mirror_request_close(&m,true,clock_ms);
    assert(ended_complete && !m.staging);
    assert(atomic_load(&s_mirror_staging_rings)==0U);
    /* Retain a pressured tail after the caller's stack/object goes away. */
    memset(&m,0,sizeof(m)); written=0; start(&m); writable=false;
    for(unsigned i=0;i<200;i++) assert(feed(&m,i)==ESP_OK);
    nuwax_offline_audio_mirror_request_close(&m,true,clock_ms);
    assert(tail_fn && !m.staging && atomic_load(&s_mirror_closers)==1);
    assert(atomic_load(&s_mirror_staging_rings)==1U);
    writable=true; finish_tail();
    assert(written==200 && ended_complete && atomic_load(&s_mirror_closers)==0);
    assert(atomic_load(&s_mirror_staging_rings)==0U);
    /* A failed generation is released, and the pending PCM survives reattach. */
    memset(&m,0,sizeof(m)); written=0; start(&m); fail_once=true;
    feed(&m,0);
    assert(m.staging_count==1 && !m.attached && !m.complete);
    feed(&m,1);
    assert(written==2 && !m.staging_count);
    nuwax_offline_audio_mirror_request_close(&m,true,clock_ms);
    assert(!ended_complete);
    /* Over-capacity loss is counted and invalidates completeness. */
    memset(&m,0,sizeof(m)); written=0; start(&m); writable=false;
    for(unsigned i=0;i<MIRROR_STAGING_FRAMES;i++) assert(feed(&m,i)==ESP_OK);
    for(unsigned i=MIRROR_STAGING_FRAMES;i<MIRROR_STAGING_FRAMES+4;i++) assert(feed(&m,i)==ESP_ERR_NOT_FINISHED);
    assert(m.rejected_frames==4);
    nuwax_offline_audio_mirror_request_close(&m,true,clock_ms);
    writable=true; finish_tail();
    assert(written==MIRROR_STAGING_FRAMES && !ended_complete);
    /* Measured 7.767 s stall, then a 12 s stall. No writer capacity is
     * credited here: retain every PCM sample/sequence in staging alone. */
    for(unsigned stalled_frames=389; stalled_frames<=600; stalled_frames+=211) {
        memset(&m,0,sizeof(m)); written=0; available=true; writable=false; start(&m);
        for(unsigned i=0;i<stalled_frames;i++) assert(feed(&m,i)==ESP_OK);
        assert(!m.rejected_frames);
        writable=true;
        for(unsigned i=stalled_frames;i<stalled_frames+100;i++) assert(feed(&m,i)==ESP_OK);
        nuwax_offline_audio_mirror_request_close(&m,true,clock_ms);
        assert(written==stalled_frames+100 && ended_complete);
    }
    /* Unavailable media cannot strand a close worker or silently report success. */
    memset(&m,0,sizeof(m)); available=false; written=0; start(&m); feed(&m,0);
    atomic_store(&s_media_ready, false); /* Existing PCM survives a media fault. */
    nuwax_offline_audio_mirror_request_close(&m,true,clock_ms);
    const uint32_t before=clock_ms; finish_tail();
    assert(clock_ms-before==MIRROR_CLOSE_TIMEOUT_MS && atomic_load(&s_mirror_closers)==0);
    assert(atomic_load(&s_mirror_staging_rings)==0U);
    puts("mirror staging: delayed attach, queue pressure, detached tail, overflow and close timeout PASS");
}
