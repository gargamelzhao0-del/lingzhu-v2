#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include "sd_audio_catalog.h"
#include "sd_audio_time.h"
static atomic_bool s_card_online;
static uint64_t s_card_online_since, fake_ms;
static int s_card_network_lock;
static bool held;
static void enter(int *mux) { (void)mux;assert(!held);held=true; }
static void leave(int *mux) { (void)mux;assert(held);held=false; }
static int64_t esp_timer_get_time(void) { assert(!held);return (int64_t)fake_ms*1000; }
#define portENTER_CRITICAL(mux) enter(mux)
#define portEXIT_CRITICAL(mux) leave(mux)
#include "sd_network_host.inc"

int main(void) {
    fake_ms=1000;nuwax_sd_session_network(true);
    const uint64_t elapsed[]={0,2147483647ULL,2147483648ULL,4294967295ULL,4294967296ULL,8589934592ULL};
    for(unsigned i=0;i<sizeof(elapsed)/sizeof(elapsed[0]);i++) {
        fake_ms=2000+elapsed[i];nuwax_sd_session_network(true);
        assert(card_online_since_ms()==1000);
        assert(card_frame_realtime((uint32_t)(fake_ms-100)));
        sd_audio_session_t scene={.mode=1,.boot_id=7};
        sd_audio_segment_t file={.sealed=1,.realtime=1,.start_ms=fake_ms-960,.end_ms=fake_ms};
        assert(sd_audio_segment_realtime(&scene,&file,7,fake_ms+100,card_online_since_ms()));
        assert(sd_audio_segment_realtime(&scene,&file,8,fake_ms+100,card_online_since_ms()));
        assert(sd_audio_segment_realtime(&scene,&file,7,fake_ms+3001,card_online_since_ms()));
        nuwax_sd_session_network(false);assert(!card_frame_realtime((uint32_t)fake_ms));
        nuwax_sd_session_network(true);assert(card_online_since_ms()==fake_ms);
        assert(!card_frame_realtime((uint32_t)(fake_ms-100)));
        assert(card_frame_realtime((uint32_t)fake_ms));
        assert(sd_audio_segment_realtime(&scene,&file,7,fake_ms+100,card_online_since_ms()));
        nuwax_sd_session_network(false);fake_ms=1000;nuwax_sd_session_network(true);
    }
    fake_ms=4294967296ULL+20;nuwax_sd_session_network(false);nuwax_sd_session_network(true);
    fake_ms+=100;
    assert(!card_frame_realtime(0xfffffff0U)); // Old preroll across low-32 wrap.
    assert(card_frame_realtime(20));
    assert(!sd_audio_frame_after_online(0xfffffff0U,5,0)); // Not a pre-boot time.
    puts("PASS: actual online transitions and capture classification across 2^31/2^32/multiple wraps; old preroll excluded during capture; persisted online-file class survives age/reconnect/reboot");
    return 0;
}
