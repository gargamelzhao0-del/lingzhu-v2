"""Compare production V4 publication to a scalar pixel oracle and damage coverage."""
from pathlib import Path
import re,subprocess,hashlib,json
root=Path(__file__).resolve().parents[1]
source_path=root/'main/nuwax_ui_v28/face/emotion_animation_player.c'
source=source_path.read_text(encoding='utf-8-sig')
out=root/'build-host-checks/emotion-v4-publish';out.mkdir(parents=True,exist_ok=True)
def extract(name):
    m=re.search(r'^static [\w* ]+\b'+name+r'\([^;{}]*\)\s*\{',source,re.M);assert m,name
    i=source.index('{',m.start())+1;depth=1
    while depth:depth+=(source[i]=='{')-(source[i]=='}');i+=1
    return source[m.start():i]
support=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define FRAME_WIDTH 360U
#define FRAME_HEIGHT 360U
#define FRAME_PIXELS (FRAME_WIDTH*FRAME_HEIGHT)
#define NUWAX_EMOTION_ASSET_V4_MAX_REGIONS 5U
typedef int esp_err_t;
enum {ESP_OK=0,ESP_ERR_INVALID_STATE=-1,ESP_ERR_INVALID_SIZE=-2,ESP_ERR_INVALID_ARG=-3};
typedef struct {uint16_t x,y,width,height;} nuwax_emotion_asset_region_t;
typedef struct {uint8_t *data;uint32_t size;bool v4_frame,regional_payload,absolute;
    uint8_t region_count;nuwax_emotion_asset_region_t regions[5];} decode_frame_slot_t;
static uint8_t indices[FRAME_PIXELS],oracle[FRAME_PIXELS],payload[FRAME_PIXELS],dirty[FRAME_PIXELS],expected_dirty[FRAME_PIXELS];
static uint8_t *s_indices=indices;
static uint16_t rgb[FRAME_PIXELS],s_palette[256];
static uint16_t *s_rgb565=rgb;
static struct {unsigned palette_count;} entry={16},*s_entry=&entry;
static void *s_frame_patch_compositor;
static uint32_t s_pending_dirty_pixels,resets,runs;
static void reset_dirty_areas(void){memset(dirty,0,sizeof(dirty));resets++;}
static void include_dirty_run(uint32_t off,uint32_t n){assert(off+n<=FRAME_PIXELS);memset(dirty+off,1,n);runs++;}
'''
tests=r'''
static uint32_t random_state=7;
static uint32_t rand32(void){random_state=random_state*1664525U+1013904223U;return random_state;}
int main(void){
    for(unsigned i=0;i<256;i++)s_palette[i]=(uint16_t)(i*31);
    for(unsigned test=0;test<1200;test++){
        bool absolute=test%17==0,compositor=(test&1)!=0,regional=(test%3)!=0;
        decode_frame_slot_t slot={.data=payload,.v4_frame=true,.absolute=absolute,.regional_payload=regional};
        s_frame_patch_compositor=compositor?(void*)1:NULL;
        for(unsigned i=0;i<FRAME_PIXELS;i++){indices[i]=oracle[i]=(uint8_t)(rand32()%16);rgb[i]=s_palette[indices[i]];payload[i]=0;}
        memset(dirty,0,sizeof(dirty));memset(expected_dirty,0,sizeof(expected_dirty));resets=runs=0;
        slot.region_count=absolute?0:(uint8_t)(test%6);
        for(unsigned r=0;r<slot.region_count;r++){
            unsigned w=1+rand32()%60,h=1+rand32()%40;
            slot.regions[r]=(nuwax_emotion_asset_region_t){rand32()%(361-w),rand32()%(361-h),w,h};
        }
        unsigned cursor=0,expected_changes=0;
        unsigned count=absolute?1:slot.region_count;
        for(unsigned r=0;r<count;r++){
            nuwax_emotion_asset_region_t area=absolute?(nuwax_emotion_asset_region_t){0,0,360,360}:slot.regions[r];
            for(unsigned y=area.y;y<area.y+area.height;y++)for(unsigned x=area.x;x<area.x+area.width;x++){
                unsigned pixel=y*360+x,at=regional?cursor++:pixel;
                payload[at]=(uint8_t)(rand32()%16);
            }
        }
        slot.size=regional?cursor:FRAME_PIXELS;cursor=0;
        for(unsigned r=0;r<count;r++){
            nuwax_emotion_asset_region_t area=absolute?(nuwax_emotion_asset_region_t){0,0,360,360}:slot.regions[r];
            for(unsigned y=area.y;y<area.y+area.height;y++)for(unsigned x=area.x;x<area.x+area.width;x++){
                unsigned pixel=y*360+x;uint8_t v=payload[regional?cursor++:pixel];
                uint8_t next=absolute?v:(oracle[pixel]^v);
                if(next!=oracle[pixel]){expected_changes++;expected_dirty[pixel]=1;}
                oracle[pixel]=next;
            }
        }
        assert(publish_v4_frame(&slot)==ESP_OK);
        assert(slot.size==0&&s_pending_dirty_pixels==expected_changes);
        assert(memcmp(indices,oracle,sizeof(indices))==0);
        for(unsigned i=0;i<FRAME_PIXELS;i++)assert(rgb[i]==s_palette[oracle[i]]);
        if(compositor&&!absolute)assert(resets==0&&runs==0);
        else {assert(resets==1);assert(memcmp(dirty,expected_dirty,sizeof(dirty))==0);}
    }
    // State/geometry/payload/palette errors must retain their rejection paths.
    decode_frame_slot_t invalid={.data=payload,.v4_frame=true,.regional_payload=true,.region_count=1,.size=1,
        .regions={{359,359,1,1}}};
    assert(publish_v4_frame(NULL)==ESP_ERR_INVALID_STATE);
    invalid.v4_frame=false;assert(publish_v4_frame(&invalid)==ESP_ERR_INVALID_STATE);invalid.v4_frame=true;
    invalid.regions[0].width=2;assert(publish_v4_frame(&invalid)==ESP_ERR_INVALID_SIZE);invalid.regions[0].width=1;
    invalid.size=2;payload[0]=0;assert(publish_v4_frame(&invalid)==ESP_ERR_INVALID_SIZE);invalid.size=1;
    payload[0]=255;assert(publish_v4_frame(&invalid)==ESP_ERR_INVALID_ARG);
    puts("PASS production V4: 1200 full-size frames vs scalar pixel oracle, absolute/regional/full delta/overlap/hold, changed counts, damage-path selection and invalid input rejection");
}
'''
path=out/'publish.c';path.write_text(support+extract('publish_v4_frame')+tests,encoding='utf-8')
exe=out/'publish.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
(out/'report.json').write_text(json.dumps({'frames':1200,'hardwareValidated':False,
    'sourceSha256':hashlib.sha256(source_path.read_bytes()).hexdigest()},indent=2),encoding='utf-8')
