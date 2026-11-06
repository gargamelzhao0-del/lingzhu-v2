#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stddef.h>

static bool configured=true, initialized=true;
#define CONFIG_NUWAX_OTA_ENABLE 1
static bool ota_reserved;
static bool nuwax_ota_device_is_active(void) { return ota_reserved; }
static bool enabled(void) { return configured; }
static bool runtime_initialized(void) { return initialized; }
static atomic_uint s_mirror_closers;
static struct {
    atomic_bool media_recovery_requested, active, closing, capture_running, writer_busy;
    unsigned *queue;
} s;
static unsigned uxQueueMessagesWaiting(unsigned *queue) { return *queue; }
#include "offline_storage_host.inc"

static uint32_t reference_crc(uint32_t crc, const uint8_t *p, size_t len) {
    crc=~crc;
    while (len--) {
        crc^=*p++;
        for (unsigned bit=0;bit<8;bit++)
            crc=(crc>>1)^((crc&1) ? 0xedb88320U : 0);
    }
    return ~crc;
}
int main(void) {
    assert(!nuwax_offline_audio_storage_busy());
    ota_reserved=true;
    assert(nuwax_offline_audio_storage_busy());
    ota_reserved=false;
    assert(!nuwax_offline_audio_storage_busy());
    atomic_bool *gates[]={&s.media_recovery_requested,&s.active,&s.closing,
                         &s.capture_running,&s.writer_busy};
    for (unsigned i=0;i<sizeof(gates)/sizeof(gates[0]);i++) {
        atomic_store(gates[i],true);
        assert(nuwax_offline_audio_storage_busy());
        atomic_store(gates[i],false);
        assert(!nuwax_offline_audio_storage_busy());
    }
    atomic_store(&s_mirror_closers,1);
    assert(nuwax_offline_audio_storage_busy());
    atomic_store(&s_mirror_closers,0);
    unsigned queued=1; s.queue=&queued;
    assert(nuwax_offline_audio_storage_busy());
    queued=0; assert(!nuwax_offline_audio_storage_busy());
    atomic_store(&s.media_recovery_requested,true);
    initialized=false; assert(!nuwax_offline_audio_storage_busy());
    initialized=true; configured=false; assert(!nuwax_offline_audio_storage_busy());

    assert(crc32_update(0,"123456789",9)==0xcbf43926U);
    assert(crc32_update(0x12345678,NULL,0)==0x12345678U);
    static uint8_t payload[1024*1024];
    uint32_t random=0x12345678;
    for (size_t i=0;i<sizeof(payload);i++) {
        random=random*1664525U+1013904223U; payload[i]=(uint8_t)(random>>24);
    }
    const size_t steps[]={1,3,640,672,3968,8064,16384};
    for (unsigned seed=0;seed<2;seed++) {
        uint32_t initial=seed ? 0x13579bdfU : 0;
        uint32_t expected=reference_crc(initial,payload,sizeof(payload));
        assert(crc32_update(initial,payload,sizeof(payload))==expected);
        for (unsigned step=0;step<sizeof(steps)/sizeof(steps[0]);step++) {
            uint32_t crc=initial;
            for (size_t offset=0;offset<sizeof(payload);) {
                size_t len=steps[step];
                if (len>sizeof(payload)-offset) len=sizeof(payload)-offset;
                crc=crc32_update(crc,payload+offset,len); offset+=len;
            }
            assert(crc==expected);
        }
    }
    puts("offline storage recovery gate and archive CRC compatibility: PASS");
    return 0;
}
