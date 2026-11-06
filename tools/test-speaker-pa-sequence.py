"""Exercise production PA/init/PCM/stop bodies with GPIO/I2S fault injection.

These checks prove software ordering, not physical pop suppression. The latter
requires the target board. Negative mutants must compile and fail at runtime.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/speaker-pa-sequence'
out.mkdir(parents=True, exist_ok=True)
board = (root / 'main/board/vocat_audio_board.c').read_text(encoding='utf-8-sig')
speaker = (root / 'main/speaker_output.c').read_text(encoding='utf-8-sig')
header = (root / 'main/speaker_output.h').read_text(encoding='utf-8-sig')


def function(source, signature):
    start = source.index(signature)
    while ';' in source[start:source.index('{', start)]:
        start = source.index(signature, start + len(signature))
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_TIMEOUT -4
#define ESP_ERR_NO_MEM -5
#define ESP_ERR_INVALID_SIZE -6
#define ESP_ERR_INVALID_RESPONSE -7
#define ESP_CODEC_DEV_OK 0
#define CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0 1
#define CONFIG_NUWAX_SPEAKER_SD_GPIO -1
#define CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE 1
#define CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE 16000
#define CONFIG_NUWAX_VOCAT_AUDIO_DMA_DESC_NUM 6
#define CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM 240
#define CONFIG_NUWAX_SPEAKER_DMA_DESC_NUM 6
#define CONFIG_NUWAX_SPEAKER_DMA_FRAME_NUM 240
#define CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT 80
#define NUWAX_SPEAKER_DISCRETE_STOP_PRELOAD_MS 20U
#define NUWAX_SPEAKER_DISCRETE_STOP_MUTE_MS 20U
#define NUWAX_SPEAKER_SHARED_STOP_SETTLE_MARGIN_MS 30U
#define NUWAX_SPEAKER_OUTPUT_LOCK_TIMEOUT_MS 250U
#define NUWAX_SPEAKER_STOP_LOCK_TIMEOUT_MS 1250U
#define CONFIG_NUWAX_VOCAT_PA_ENABLE_SETTLE_MS 20
#define CONFIG_NUWAX_VOCAT_PA_DISABLE_SETTLE_MS 5
#define CONFIG_NUWAX_VOCAT_PA_TRANSITION_MAX_MS 100
#define NUWAX_VOCAT_PA_CTRL_GPIO 4
#define NUWAX_VOCAT_CODEC_PWR_GPIO -1
#define NUWAX_VOCAT_CODEC_DEV_ADDR_ES8311 48
#define VOCAT_AUDIO_SAMPLE_RATE_HZ 16000
#define VOCAT_ES8311_DEFAULT_VOLUME_DB -20.0f
#define NUWAX_VOCAT_I2S_MCLK_GPIO 1
#define NUWAX_VOCAT_I2S_BCK_GPIO 2
#define NUWAX_VOCAT_I2S_WS_GPIO 3
#define NUWAX_VOCAT_I2S_DOUT_GPIO 5
#define NUWAX_VOCAT_I2S_DIN_GPIO 6
#define AUDIO_CODEC_DEV_WORK_MODE_DAC 1
#define ESP_CODEC_DEV_WORK_MODE_DAC 1
#define I2C_NUM_0 0
#define GPIO_MODE_OUTPUT 1
#define MALLOC_CAP_8BIT 1
#define pdTRUE 1
#define portTICK_PERIOD_MS 10U
#define pdMS_TO_TICKS(ms) ((ms)/portTICK_PERIOD_MS)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr,...) do{int rc=(expr);if(rc)return rc;}while(0)
#define ESP_RETURN_ON_FALSE(expr,code,...) do{if(!(expr))return code;}while(0)
typedef int esp_err_t;
typedef unsigned TickType_t;
typedef int *SemaphoreHandle_t;
typedef void *i2s_chan_handle_t;
typedef void *i2c_master_bus_handle_t;
typedef struct {uint32_t token;} nuwax_audio_board_lease_t;
typedef struct {uint32_t token;} nuwax_audio_board_ref_t;
static uint64_t time_us, pa_ready_us;
static bool pa, silent_ring, tx_enabled, rx_enabled;
static bool fail_alloc, fail_mute, fail_prepare, fail_disable, fail_preload, short_preload, fail_enable, fail_write;
static bool fail_codec, fail_lock, early_tick, late_tick, audit_reference;
static unsigned reference_cursor;
static size_t partial_write_frames;
static unsigned tx_disables, tx_enables, pa_enables, pcm_count, ref_count, board_cleanups;
static int16_t pcm[4096];
static int tx_object, rx_object, mutex;
static i2s_chan_handle_t s_tx, s_rx;
static void *s_es8311_ctrl, *s_es7210_ctrl, *s_es7210_if;
static int s_es8311_ctrl_guard;
static bool s_initialized, s_pa_forced_off;
static int64_t esp_timer_get_time(void){return (int64_t)time_us;}
static void vTaskDelay(unsigned ticks){assert(ticks>0);time_us+=(uint64_t)ticks*10000;if(early_tick){time_us-=9000;early_tick=false;}if(late_tick){time_us+=200000;late_tick=false;}}
static bool pa_output_latch_enabled(void){return pa;}
static int gpio_set_level(int pin,int level){assert(pin==4);if(fail_mute&&!level)return ESP_FAIL;pa=level;return 0;}
static int gpio_set_direction(int pin,int direction){assert(pin==4&&direction==1);assert(!pa);return 0;}
static int codec_gpio_set(int pin,int level){
 assert(pin==4);pa=level;if(level){pa_enables++;assert(tx_enabled);pa_ready_us=time_us+20000;}
 if(fail_prepare&&level)return ESP_FAIL;return 0;
}
typedef struct {int (*set)(int,int);} gpio_if_t;
static const gpio_if_t gpio_if={codec_gpio_set};
static const gpio_if_t *s_gpio_if;
static int codec_dev_ret_to_err(int rc,const char *label){(void)label;return rc==0?0:ESP_FAIL;}
static const gpio_if_t *audio_codec_new_gpio(void){return &gpio_if;}
typedef struct {int port,addr;void *bus_handle;} audio_codec_i2c_cfg_t;
typedef void audio_codec_ctrl_if_t;
typedef struct {const void *ctrl_if;const gpio_if_t *gpio_if;int codec_mode,pa_pin;bool use_mclk;struct {float pa_voltage,codec_dac_voltage;} hw_gain;} es8311_codec_cfg_t;
typedef struct {int bits_per_sample,channel,channel_mask;unsigned sample_rate;} esp_codec_dev_sample_info_t;
struct codec_if;
typedef struct codec_if {int (*set_fs)(const struct codec_if*,esp_codec_dev_sample_info_t*);int (*enable)(const struct codec_if*,bool);int (*mute)(const struct codec_if*,bool);} codec_if_t;
static int codec_pa_pin;
static int codec_fs(const codec_if_t *c,esp_codec_dev_sample_info_t *fs){(void)c;(void)fs;assert(!pa);return 0;}
static int codec_enable(const codec_if_t *c,bool enable){(void)c;assert(!pa);if(enable&&codec_pa_pin>=0)pa=true;return fail_codec?ESP_FAIL:0;}
static int codec_mute(const codec_if_t *c,bool mute){(void)c;(void)mute;return 0;}
static const codec_if_t codec={codec_fs,codec_enable,codec_mute};
static const codec_if_t *s_es8311_if;
/* Clock-error aggregation is exercised in test-music-failures.py. */
static int set_es8311_sample_info(esp_codec_dev_sample_info_t *fs,const char *label){return codec_dev_ret_to_err(s_es8311_if->set_fs(s_es8311_if,fs),label);}
static const codec_if_t *es8311_codec_new(es8311_codec_cfg_t *cfg){codec_pa_pin=cfg->pa_pin;return &codec;}
static int nuwax_i2c_bus_get(void **p){*p=&tx_object;return 0;}
static void *new_codec_i2c_ctrl_guarded(audio_codec_i2c_cfg_t *cfg){(void)cfg;return &tx_object;}
static void *codec_ctrl_guard_bind(int *g,void *c){(void)g;return c;}
static int set_es8311_volume_db(float db,const char *label){(void)db;(void)label;assert(!pa);return 0;}
static int init_i2s_duplex(void){assert(!pa);s_tx=&tx_object;s_rx=&rx_object;silent_ring=true;return 0;}
static int init_es7210(void){assert(!pa);return 0;}
static int enable_i2s_channel(const char *name,void *h){(void)name;assert(!pa);if(h==s_tx)tx_enabled=true;else rx_enabled=true;return 0;}
static int restart_es7210_after_i2s_ready(void){assert(!pa&&tx_enabled&&rx_enabled);return 0;}
static void log_i2s_channel(const char *name,void *h){(void)name;(void)h;}
static int nuwax_vocat_audio_board_deinit(void){pa=false;s_tx=s_rx=NULL;s_gpio_if=NULL;s_es8311_if=NULL;s_es7210_if=NULL;s_es8311_ctrl=s_es7210_ctrl=NULL;s_initialized=false;board_cleanups++;return 0;}
static int nuwax_vocat_audio_board_force_speaker_mute(void);
static int nuwax_vocat_audio_board_prepare_speaker_playback(void);
static int nuwax_vocat_audio_board_init(void);
static SemaphoreHandle_t xSemaphoreCreateMutex(void){mutex=1;return &mutex;}
static void vSemaphoreDelete(SemaphoreHandle_t m){assert(m==&mutex&&mutex);}
static int xSemaphoreTake(SemaphoreHandle_t m,unsigned ticks){(void)ticks;if(fail_lock)return 0;assert(*m);*m=0;return 1;}
static void xSemaphoreGive(SemaphoreHandle_t m){assert(!*m);*m=1;}
static void *heap_caps_realloc(void *p,size_t n,int caps){(void)caps;return fail_alloc?NULL:realloc(p,n);}
static int nuwax_audio_board_init(nuwax_audio_board_ref_t *r){int e=nuwax_vocat_audio_board_init();if(!e)r->token=1;return e;}
static int nuwax_audio_board_deinit(nuwax_audio_board_ref_t *r){r->token=0;return 0;}
static int nuwax_audio_board_acquire_tx(void **tx,nuwax_audio_board_lease_t *l){*tx=s_tx;l->token=1;return 0;}
static int nuwax_audio_board_force_speaker_mute(void){return nuwax_vocat_audio_board_force_speaker_mute();}
static int nuwax_audio_board_prepare_speaker_playback(void){assert(silent_ring&&tx_enabled);return nuwax_vocat_audio_board_prepare_speaker_playback();}
static int i2s_channel_disable(void *h){assert(h==s_tx&&!pa);tx_disables++;if(fail_disable)return ESP_FAIL;tx_enabled=false;return 0;}
static int i2s_channel_preload_data(void *h,const void *buf,size_t bytes,size_t *done){
 assert(h==s_tx&&!tx_enabled&&!pa);const unsigned char *p=buf;for(size_t i=0;i<bytes;i++)assert(p[i]==0);
 *done=fail_preload?0:(short_preload?bytes-4:bytes);silent_ring=(*done==bytes);return fail_preload?ESP_FAIL:0;
}
static int i2s_channel_enable(void *h){assert(h==s_tx&&!pa&&silent_ring);tx_enables++;if(fail_enable)return ESP_FAIL;tx_enabled=true;return 0;}
static int i2s_channel_write(void *h,const void *buf,size_t bytes,size_t *done,unsigned timeout){
 (void)timeout;assert(h==s_tx&&tx_enabled&&pa&&time_us>=pa_ready_us);assert(bytes%4==0);
 if(fail_write){*done=0;return ESP_FAIL;}const int16_t *p=buf;
 size_t frames=bytes/4;
 if(partial_write_frames&&frames>partial_write_frames)frames=partial_write_frames;
 for(size_t i=0;i<frames;i++){assert(p[i*2]==p[i*2+1]);assert(pcm_count<4096);pcm[pcm_count++]=p[i*2];}
 *done=frames*4;silent_ring=false;return partial_write_frames?ESP_ERR_TIMEOUT:0;
}
static void nuwax_p3_software_ref_write_interleaved_lr(const int16_t *p,size_t n){
 if(audit_reference)for(size_t i=0;i<n;i++){
   assert(reference_cursor<pcm_count&&p[i*2]==pcm[reference_cursor]&&p[i*2+1]==pcm[reference_cursor]);reference_cursor++;
 }
 ref_count+=n;
}
#define nuwax_device_warning_report_enqueue(...) (0)
'''

prefix += speaker[speaker.index('#define NUWAX_SPEAKER_STARTUP_FADE_MS'):speaker.index('#define NUWAX_SPEAKER_STOP_LOCK_TIMEOUT_MS')]

struct = header[header.index('typedef struct {'):header.index('} nuwax_speaker_output_t;') + len('} nuwax_speaker_output_t;')]
glue = r'''
static int init_p3_software_ref_if_enabled(nuwax_speaker_output_t *s){s->software_ref_ready=true;return 0;}
static int nuwax_speaker_output_deinit(nuwax_speaker_output_t *s){pa=false;free(s->stereo_buffer);memset(s,0,sizeof(*s));return 0;}
int nuwax_speaker_output_stop(nuwax_speaker_output_t *speaker);
'''
board_bodies = '\n'.join(function(board, sig) for sig in [
    'static void wait_pa_settle_ms(',
    'static esp_err_t set_external_pa_enabled(',
    'esp_err_t nuwax_vocat_audio_board_force_speaker_mute(',
    'esp_err_t nuwax_vocat_audio_board_prepare_speaker_playback(',
    'static esp_err_t init_es8311(',
    'esp_err_t nuwax_vocat_audio_board_init('])
speaker_bodies = '\n'.join(function(speaker, sig) for sig in [
    'static uint8_t clamp_volume_percent(', 'static uint32_t speaker_output_startup_gain_q15(',
    'static bool speaker_output_lock(',
    'static size_t speaker_output_dma_chunk_frames(',
    'static size_t speaker_output_stop_silence_samples(',
    'static uint32_t speaker_output_stop_settle_ms(',
    'static void speaker_output_write_ref_silence(',
    'esp_err_t nuwax_speaker_output_init(',
    'esp_err_t nuwax_speaker_output_set_volume_percent(',
    'esp_err_t nuwax_speaker_output_write_pcm16(',
    'esp_err_t nuwax_speaker_output_stop('])
checks = r'''
static void stop_ok(nuwax_speaker_output_t *s){
 assert(nuwax_speaker_output_stop(s)==0);assert(!pa&&s->muted&&s->stop_settled&&!s->shared_tx_recovery_required);
 assert(s->startup_fade_position==0);
 assert(tx_enabled&&rx_enabled&&silent_ring);
}

static void check_startup_envelope(void){
 nuwax_speaker_output_t s={0};assert(nuwax_speaker_output_init(&s)==0);
 int16_t source[800],copy[800],whole[800];
 for(size_t i=0;i<800;i++)source[i]=i<64?0:(i%2?INT16_MIN:INT16_MAX);
 memcpy(copy,source,sizeof(source));
 const unsigned volumes[]={1,40,80,100};
 for(unsigned v=0;v<4;v++){
   assert(nuwax_speaker_output_set_volume_percent(&s,(uint8_t)volumes[v])==0);
   pcm_count=reference_cursor=0;audit_reference=true;
   assert(nuwax_speaker_output_write_pcm16(&s,source,800,500)==0);
   audit_reference=false;assert(pcm_count==800&&reference_cursor==800&&!memcmp(copy,source,sizeof(source)));
   memcpy(whole,pcm,sizeof(whole));
   for(unsigned i=0;i<=64;i++)assert(whole[i]==0);
   assert(whole[65]>=-8&&whole[65]<=8); /* Low endpoint slope, even at full scale. */
   int32_t before=0;
   for(unsigned i=64;i<64+NUWAX_SPEAKER_STARTUP_FADE_FRAMES;i++){
     int32_t amplitude=whole[i]<0?-(int32_t)whole[i]:whole[i];
     assert(amplitude>=before&&amplitude<=32768);before=amplitude;
   }
   for(unsigned i=64+NUWAX_SPEAKER_STARTUP_FADE_FRAMES-1;i<800;i++)
     assert(whole[i]==(source[i]*(int32_t)volumes[v])/100);
   stop_ok(&s);pcm_count=reference_cursor=0;audit_reference=true;
   const size_t pieces[]={1,7,31,2,97,13,41,320,3,80,205};size_t at=0;
   for(unsigned i=0;i<sizeof(pieces)/sizeof(pieces[0])&&at<800;i++){
     size_t n=pieces[i];if(n>800-at)n=800-at;
     assert(nuwax_speaker_output_write_pcm16(&s,source+at,n,500)==0);at+=n;
   }
   assert(at==800&&pcm_count==800&&reference_cursor==800&&!memcmp(whole,pcm,sizeof(whole)));
   audit_reference=false;stop_ok(&s);
 }
 /* Arbitrarily long digital silence must not spend the audible ramp. */
 int16_t zeros[640]={0},pulse[480];for(unsigned i=0;i<480;i++)pulse[i]=30000;
 pcm_count=0;assert(nuwax_speaker_output_set_volume_percent(&s,100)==0);
 assert(nuwax_speaker_output_write_pcm16(&s,zeros,640,500)==0&&s.startup_fade_position==0);
 unsigned opens=pa_enables;
 assert(nuwax_speaker_output_write_pcm16(&s,pulse,480,500)==0&&pcm[640]==0&&pa_enables==opens);
 assert(pcm[640+NUWAX_SPEAKER_STARTUP_FADE_FRAMES-1]==30000);stop_ok(&s);
 /* Muted PCM and a zero-sized write are not audible progress. */
 pcm_count=0;assert(nuwax_speaker_output_set_volume_percent(&s,0)==0);
 assert(nuwax_speaker_output_write_pcm16(&s,pulse,480,500)==0&&s.startup_fade_position==0);
 assert(nuwax_speaker_output_set_volume_percent(&s,100)==0);
 assert(nuwax_speaker_output_write_pcm16(&s,pulse,0,500)==0&&s.startup_fade_position==0);
 assert(nuwax_speaker_output_write_pcm16(&s,pulse,16,500)==0&&s.startup_fade_position==16&&pcm[480]==0);
 assert(nuwax_speaker_output_write_pcm16(&s,pulse+16,464,500)==0&&pcm_count==960);
 assert(pcm[480+NUWAX_SPEAKER_STARTUP_FADE_FRAMES-1]==30000);stop_ok(&s);
 /* PA preparation failure must not commit the precomputed gain position. */
 pcm_count=0;fail_prepare=true;
 assert(nuwax_speaker_output_write_pcm16(&s,pulse,480,500)!=0&&pcm_count==0&&s.startup_fade_position==0);
 fail_prepare=false;stop_ok(&s);
 /* Only a prefix accepted by I2S advances the envelope or AEC reference. */
 pcm_count=reference_cursor=0;audit_reference=true;partial_write_frames=17;
 assert(nuwax_speaker_output_write_pcm16(&s,pulse,480,500)==ESP_ERR_TIMEOUT);
 audit_reference=false;partial_write_frames=0;
 assert(pcm_count==17&&reference_cursor==17&&s.startup_fade_position==17&&!pa&&s.shared_tx_recovery_required);
 assert(nuwax_speaker_output_write_pcm16(&s,pulse,480,500)==ESP_ERR_INVALID_STATE);
 stop_ok(&s);pcm_count=0;
 assert(nuwax_speaker_output_write_pcm16(&s,pulse,480,500)==0&&pcm[0]==0);stop_ok(&s);
 free(s.stereo_buffer);
 puts("PASS: 15ms smooth startup, unchanged full-volume tail/count, packet/DMA split invariance, leading zeros, volumes 0/1/40/80/100, short packets, restart, partial I2S and exact AEC reference");
}
int main(void){
 nuwax_speaker_output_t s={0};
 pa=true; /* A stale high latch must not be driven before setting it low. */
 fail_mute=true;assert(nuwax_vocat_audio_board_init()!=0&&s_tx==NULL&&s_es8311_if==NULL);fail_mute=false;
 fail_codec=true;assert(nuwax_vocat_audio_board_init()!=0);assert(!pa&&!s_initialized&&board_cleanups==1);
 fail_codec=false;assert(nuwax_speaker_output_init(&s)==0);
 assert(!pa&&s.muted&&s.stop_settled&&silent_ring&&rx_enabled&&tx_enabled&&codec_pa_pin==-1);
 unsigned stops=tx_disables;stop_ok(&s);assert(tx_disables==stops); /* Idempotent idle. */
 int16_t speech[320];for(int i=0;i<320;i++)speech[i]=(int16_t)(1000+i);
 assert(nuwax_speaker_output_write_pcm16(&s,speech,0,500)==0&&!pa);
 unsigned ref_before=ref_count;
 early_tick=true;
 assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)==0);
 assert(pa&&pcm_count==320&&ref_count-ref_before==320);
 assert(pcm[0]==0);
 for(unsigned i=1;i<NUWAX_SPEAKER_STARTUP_FADE_FRAMES;i++)assert(pcm[i]>=pcm[i-1]&&pcm[i]<=(speech[i]*80)/100);
 for(unsigned i=NUWAX_SPEAKER_STARTUP_FADE_FRAMES-1;i<320;i++)assert(pcm[i]==(speech[i]*80)/100);
 unsigned opens=pa_enables;
 assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)==0&&pa_enables==opens);
 for(unsigned i=0;i<320;i++)assert(pcm[320+i]==(speech[i]*80)/100);
 stop_ok(&s);
 early_tick=true;uint64_t before=time_us;
 assert(nuwax_vocat_audio_board_force_speaker_mute()==0&&time_us-before>=5000);
 /* An allocation failure must not open PA or invalidate settled silence. */
 fail_alloc=true;size_t capacity=s.stereo_capacity;s.stereo_capacity=0;
 assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)==ESP_ERR_NO_MEM);
 assert(!pa&&s.stop_settled&&pa_enables==opens);s.stereo_capacity=capacity;fail_alloc=false;
 /* Enable failure after GPIO goes high must roll back, then require recovery. */
 fail_prepare=true;assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)!=0);
 assert(!pa&&s.shared_tx_recovery_required);fail_prepare=false;
 assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)==ESP_ERR_INVALID_STATE);stop_ok(&s);
 late_tick=true;assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)==ESP_ERR_TIMEOUT);
 assert(!pa&&s.shared_tx_recovery_required);stop_ok(&s);
 /* No DMA reset is allowed if physical PA mute failed. */
 assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)==0);stops=tx_disables;
 fail_mute=true;assert(nuwax_speaker_output_stop(&s)!=0&&tx_disables==stops&&s.shared_tx_recovery_required);
 fail_mute=false;stop_ok(&s);
 for(int fault=0;fault<5;fault++){
   assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)==0);
   capacity=s.stereo_capacity;
   fail_disable=fault==0;fail_preload=fault==1;short_preload=fault==2;fail_enable=fault==3;fail_alloc=fault==4;
   if(fail_alloc)s.stereo_capacity=0;
   assert(nuwax_speaker_output_stop(&s)!=0&&!pa&&!s.stop_settled&&s.shared_tx_recovery_required);
   assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)==ESP_ERR_INVALID_STATE);
   fail_disable=fail_preload=short_preload=fail_enable=fail_alloc=false;s.stereo_capacity=capacity;stop_ok(&s);
 }
 fail_write=true;assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)!=0&&!pa);
 fail_write=false;stop_ok(&s);
 fail_write=fail_mute=true;assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)!=0);
 assert(s.shared_tx_recovery_required);fail_write=fail_mute=false;
 assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)==ESP_ERR_INVALID_STATE);stop_ok(&s);
 fail_lock=true;assert(nuwax_speaker_output_write_pcm16(&s,speech,320,500)==ESP_ERR_TIMEOUT&&!pa);fail_lock=false;
 free(s.stereo_buffer);memset(&s,0,sizeof(s));
 /* TX reacquisition with RX still live: erase old DMA before opening PA. */
 silent_ring=false;assert(nuwax_speaker_output_init(&s)==0);assert(silent_ring&&!pa&&rx_enabled);
 free(s.stereo_buffer);
 memset(&s,0,sizeof(s));fail_alloc=true;
 assert(nuwax_speaker_output_init(&s)==ESP_ERR_NO_MEM&&!s.initialized&&!pa);fail_alloc=false;
 assert(nuwax_speaker_output_init(&s)==0&&s.stop_settled&&!pa);free(s.stereo_buffer);
 check_startup_envelope();
 puts("PASS: boot/reprovision mute, full DMA silence, first PCM, repeated idle, PA/I2S/heap failure recovery and live RX");
}
'''


def run(name, b=board_bodies, s=speaker_bodies, negative=False):
    c = out / (name + '.c')
    exe = out / (name + '.exe')
    c.write_text(prefix + struct + glue + b + s + checks, encoding='utf-8')
    build = subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', '-Wno-unused-variable',
                            '-Wno-misleading-indentation', str(c), '-o', str(exe)],
                           text=True, capture_output=True)
    if build.returncode:
        raise RuntimeError(build.stderr)
    result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=10)
    assert (result.returncode != 0) == negative, (name, result.stdout, result.stderr)
    print('PASS:', name, '(mutant rejected)' if negative else result.stdout.strip())


run('production')
run('codec_reopens_pa', b=board_bodies.replace('.pa_pin = -1,', '.pa_pin = NUWAX_VOCAT_PA_CTRL_GPIO,'), negative=True)
run('stop_without_physical_mute', s=speaker_bodies.replace(
    'const esp_err_t mute_err = nuwax_audio_board_force_speaker_mute();',
    'const esp_err_t mute_err = ESP_OK;'), negative=True)
run('no_first_dma_quieting', s=speaker_bodies.replace(
    'const esp_err_t quiet_err = nuwax_speaker_output_stop(speaker);',
    'const esp_err_t quiet_err = ESP_OK;'), negative=True)
run('failed_prepare_not_muted', b=board_bodies.replace(
    'const esp_err_t mute_err = nuwax_vocat_audio_board_force_speaker_mute();',
    'const esp_err_t mute_err = ESP_OK;'), negative=True)
run('tick_rounding_shortens_settle', b=board_bodies.replace(
    function(board, 'static void wait_pa_settle_ms('),
    'static void wait_pa_settle_ms(uint32_t ms){vTaskDelay((ms+9)/10);}'), negative=True)
run('startup_without_fade', s=speaker_bodies.replace(
    function(speaker, 'static uint32_t speaker_output_startup_gain_q15('),
    'static uint32_t speaker_output_startup_gain_q15(uint32_t p){return NUWAX_SPEAKER_GAIN_ONE_Q15;}'), negative=True)
run('startup_linear_step', s=speaker_bodies.replace(
    function(speaker, 'static uint32_t speaker_output_startup_gain_q15('),
    'static uint32_t speaker_output_startup_gain_q15(uint32_t p){return p>=NUWAX_SPEAKER_STARTUP_FADE_FRAMES-1U?32768U:(uint32_t)((uint64_t)p*32768U/(NUWAX_SPEAKER_STARTUP_FADE_FRAMES-1U));}'), negative=True)
run('startup_consumed_by_silence', s=speaker_bodies.replace(
    'fade_position == 0U && scaled == 0', 'false'), negative=True)
run('startup_reset_each_packet', s=speaker_bodies.replace(
    'const uint32_t fade_start = speaker->muted ? 0U :',
    'const uint32_t fade_start = true ? 0U :'), negative=True)
run('startup_committed_before_prepare', s=speaker_bodies.replace(
    '    /* Allocate/prepare PCM while still muted.',
    '    speaker->startup_fade_position = fade_position;\n    /* Allocate/prepare PCM while still muted.'), negative=True)
run('startup_not_reset_on_stop', s=speaker_bodies.replace(
    'speaker->startup_fade_position = 0U;', '(void)speaker->startup_fade_position;'), negative=True)
run('startup_commits_unsent_tail', s=speaker_bodies.replace(
    'speaker->startup_fade_position =\n        fade_written >= NUWAX_SPEAKER_STARTUP_FADE_FRAMES - fade_start\n            ? NUWAX_SPEAKER_STARTUP_FADE_FRAMES : fade_start + (uint32_t)fade_written;',
    'speaker->startup_fade_position = fade_position;'), negative=True)
print('PASS: software sequencing checks complete; physical pop/first-syllable acceptance still requires hardware')
