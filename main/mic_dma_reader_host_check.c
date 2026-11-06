#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef void *i2s_chan_handle_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_INVALID_RESPONSE 3
#define ESP_FAIL 4
#define CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM 240
#define NUWAX_VOCAT_MIC_SLOTS 4
typedef struct {
    void *raw_samples;
    void *rx_handle;
    size_t raw_capacity, rx_cached_offset, rx_cached_bytes;
} nuwax_mic_input_t;

static size_t cursor, skipped, calls;
static bool pressure;
static int inject;
static int64_t clock_us;
static int64_t esp_timer_get_time(void) { return clock_us; }
static bool is_i2s_read_error_usable(esp_err_t e, size_t n)
{ return e == ESP_OK || (e == ESP_ERR_TIMEOUT && n > 0); }

/* Model the SDK's near-full switch of a partly consumed 240-frame block.
 * No physical overflow is required for this skip. */
static esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *dst, size_t bytes,
                                  size_t *received, uint32_t timeout)
{
    (void)h; (void)timeout;
    calls++;
    *received = 0;
    if (inject == 1) { inject = 0; return ESP_ERR_TIMEOUT; }
    if (inject == 2) { inject = 0; return ESP_FAIL; }
    if (pressure && cursor % 240 != 0) {
        size_t lost = 240 - cursor % 240;
        cursor += lost;
        skipped += lost;
    }
    size_t frames = bytes / 8;
    if (inject == 3) { frames = 80; inject = 0; }
    int16_t *out = dst;
    for (size_t f = 0; f < frames; f++)
        for (size_t slot = 0; slot < 4; slot++)
            out[f * 4 + slot] = (int16_t)((cursor + f) * 4 + slot);
    cursor += frames;
    *received = frames * 8;
    clock_us += 100;
    return frames * 8 < bytes ? ESP_ERR_TIMEOUT : ESP_OK;
}
#include "mic_dma_reader.inc"

static int16_t raw[(960 + 240) * 4];
static nuwax_mic_input_t fresh(void)
{
    cursor = skipped = calls = 0;
    clock_us = 0; inject = 0; pressure = false;
    return (nuwax_mic_input_t){.raw_samples=raw,.raw_capacity=960*8};
}
static void check_samples(size_t first, size_t frames)
{
    for(size_t f=0; f<frames; f++)
        for(size_t slot=0; slot<4; slot++)
            assert(raw[f*4+slot] == (int16_t)((first+f)*4+slot));
}
int main(void)
{
    nuwax_mic_input_t mic = fresh(); size_t n;
    /* Establish the old path's counterexample before testing the remedy. */
    assert(i2s_channel_read(NULL,raw,320*8,&n,50)==ESP_OK);
    pressure=true;
    assert(i2s_channel_read(NULL,raw,320*8,&n,50)==ESP_OK);
    assert(skipped==160 && raw[0]==480*4);

    mic=fresh(); pressure=true;
    for(size_t frame=0; frame<12; frame++) {
        assert(mic_read_dma_blocks(&mic,320*8,50,&n)==ESP_OK);
        assert(n==320*8); check_samples(frame*320,320);
    }
    assert(skipped==0);
    /* Offline -> online moves the whole mic object without releasing RX.
     * Its software tail must move with it, not be discarded like teardown. */
    mic=fresh();
    assert(mic_read_dma_blocks(&mic,320*8,50,&n)==ESP_OK);
    nuwax_mic_input_t transferred=mic;
    memset(&mic,0,sizeof(mic));
    assert(mic_read_dma_blocks(&transferred,320*8,50,&n)==ESP_OK);
    assert(n==320*8); check_samples(320,320);
    mic=fresh();
    const size_t sizes[]={240,320,1,959,80,512,320};
    size_t total=0;
    for(size_t i=0; i<sizeof(sizes)/sizeof(sizes[0]); i++) {
        assert(mic_read_dma_blocks(&mic,sizes[i]*8,50,&n)==ESP_OK);
        assert(n==sizes[i]*8); check_samples(total,sizes[i]); total+=sizes[i];
    }
    mic=fresh();
    assert(mic_read_dma_blocks(&mic,320*8,50,&n)==ESP_OK);
    inject=1;
    assert(mic_read_dma_blocks(&mic,320*8,50,&n)==ESP_ERR_TIMEOUT);
    assert(n==160*8); check_samples(320,160);
    assert(mic_read_dma_blocks(&mic,320*8,50,&n)==ESP_OK);
    check_samples(480,320);
    inject=2;
    assert(mic_read_dma_blocks(&mic,320*8,50,&n)==ESP_FAIL);
    assert(n==0 && mic.rx_cached_bytes==0);
    mic=fresh(); inject=3;
    assert(mic_read_dma_blocks(&mic,320*8,50,&n)==ESP_ERR_TIMEOUT);
    assert(n==80*8); check_samples(0,80);
    assert(mic_read_dma_blocks(&mic,320*8,50,&n)==ESP_OK);
    check_samples(80,320);
    mic=fresh();
    assert(mic_read_dma_blocks(&mic,320*8,0,&n)==ESP_OK);
    assert(n==240*8 && calls==1);
    assert(mic_read_dma_blocks(&mic,961*8,50,&n)==ESP_ERR_INVALID_SIZE);
    /* The reset used on overflow/startup/teardown must not leak a cached
     * prefix into a recovered stream or a newly acquired microphone. */
    mic=fresh();
    assert(mic_read_dma_blocks(&mic,320*8,50,&n)==ESP_OK);
    assert(mic.rx_cached_bytes==160*8);
    mic_dma_cache_reset(&mic);
    assert(mic.rx_cached_bytes==0 && mic.rx_cached_offset==0);
    assert(mic_read_dma_blocks(&mic,320*8,50,&n)==ESP_OK);
    check_samples(480,320);
    puts("mic DMA reader: old-path skip reproduced; buffered continuity, slots, sizes, timeout and error checks passed");
    return 0;
}
