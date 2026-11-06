"""Exercise production PSRAM admission against recording handoff and low memory."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
shared = (root/'main/offline_replay_shared.inc').read_text(encoding='utf-8')
start = shared.index('static size_t replay_shared_memory_reserve(')
end = shared.index('static esp_err_t replay_send_shared_file(', start)
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM 32
#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 8192
#define CONFIG_LWIP_TCP_SND_BUF_DEFAULT 131072U
#include "offline_replay_policy.h"
#define MIRROR_STAGING_FRAMES 768U
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
typedef struct { uint32_t sequence,timestamp_ms; uint8_t flags; int16_t samples[320]; } mirror_staged_frame_t;
static atomic_uint s_mirror_staging_rings;
static size_t available, allocated;
static unsigned allocations, frees, scenario;
static size_t heap_caps_get_free_size(unsigned c) { (void)c; return available; }
static void *heap_caps_malloc(size_t n,unsigned c) {
    (void)c; allocations++; assert(n<=available && !allocated);
    if(scenario==1)return NULL; // Fragmentation or concurrent allocation failure.
    allocated=n; available-=n;
    if(scenario==2)atomic_store(&s_mirror_staging_rings,0); // Close removed the credit before its free.
    return malloc(n);
}
static void heap_caps_free(void *p) {
    assert(p && allocated); frees++;available+=allocated;allocated=0;free(p);
}
''' + shared[start:end] + r'''
int main(void) {
    const size_t ring=MIRROR_STAGING_FRAMES*sizeof(mirror_staged_frame_t);
    const size_t full=REPLAY_PSRAM_RESERVE_BYTES+REPLAY_WORKER_HEADROOM_BYTES;
    const size_t credited=full-ring;
    assert(replay_shared_reserve_bytes(0,ring)==full);
    assert(replay_shared_reserve_bytes(1,ring)==credited);
    assert(replay_shared_reserve_bytes(2,ring)==credited); // Close + new capture, credit only one.
    assert(replay_shared_reserve_bytes(100,SIZE_MAX)==256U*1024U+REPLAY_WORKER_HEADROOM_BYTES);
    assert(replay_shared_payload_limit(false)==8064U);
    assert(replay_shared_payload_limit(true)==3896U);
    size_t capacity=0; void *buffer;
    // Latest trace: ~575 KiB free during capture used to fall back entirely.
    available=575U*1024U;s_mirror_staging_rings=1;
    buffer=replay_shared_allocate(8142,&capacity);
    assert(buffer && capacity==32768U && available>=credited);heap_caps_free(buffer);
    // With no capture, preserve the entire future capture startup reserve.
    s_mirror_staging_rings=0;allocations=0;
    assert(!replay_shared_allocate(8142,&capacity) && !allocations);
    available=full+2U*32768U+8142U;
    buffer=replay_shared_allocate(8142,&capacity);
    assert(buffer && capacity==32768U && available==full);heap_caps_free(buffer);
    // Retain the prior 8KiB admission, then cover the smaller 4KiB slots.
    available=credited+2U*8192U+8142U;s_mirror_staging_rings=1;
    buffer=replay_shared_allocate(8142,&capacity);
    assert(buffer && capacity==8192U && available==credited);heap_caps_free(buffer);
    available=credited+2U*4096U+8142U;
    buffer=replay_shared_allocate(8142,&capacity);
    assert(buffer && capacity==4096U && available==credited);heap_caps_free(buffer);
    available--;allocations=0;
    assert(!replay_shared_allocate(8142,&capacity) && !allocations);
    scenario=1;available=575U*1024U;allocations=0;
    assert(!replay_shared_allocate(8142,&capacity) && allocations==4 && !allocated);
    scenario=2;available=575U*1024U;s_mirror_staging_rings=1;frees=0;
    assert(!replay_shared_allocate(8142,&capacity) && frees==1 && !allocated);
    assert(available==575U*1024U); // No leak on credit expiration.
    puts("PASS: capture reserve credit, two owners, 575 KiB prefetch, low-memory fallback, fragmentation and close-during-allocation");
}
'''
with tempfile.TemporaryDirectory(prefix='sd-formal-speed-') as directory:
    path = Path(directory)
    (path/'check.c').write_text(code,encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'main'),
                    str(path/'check.c'),'-o',str(path/'check.exe')],check=True)
    subprocess.run([str(path/'check.exe')],check=True)
