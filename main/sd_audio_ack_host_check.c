#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "sd_audio_scene_protocol.h"
#include "offline_replay_policy.h"
#define REPLAY_ACK_BIT 4U
#define REPLAY_CHUNK_ACK_BIT 8U
#define REPLAY_CHUNK_MAX_PAYLOAD_BYTES 8064U
#define REPLAY_WINDOW_MAX_BYTES 524288U
#define TAG "test"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef unsigned EventBits_t;
static unsigned signals;
static struct {
    unsigned events;
    struct { bool waiting,ok,chunk; uint32_t epoch,next_offset,window_bytes,received_ms;
        sd_scene_ack_t scene; char request_id[48],segment_id[48],expected_type[40]; } replay_ack;
} s;
static bool enabled(void) {return true;}
static bool runtime_initialized(void) {return true;}
static cJSON *nuwax_json_parse_with_length(const char *p,int n) {return cJSON_ParseWithLength(p,n);}
static unsigned xEventGroupSetBits(unsigned group,unsigned bits) {(void)group;signals|=bits;return signals;}
static uint32_t offline_now_ms(void){return 123U;}
#include "ack.inc"
static void arm(unsigned route) {
    memset(&s,0,sizeof(s));signals=0;s.replay_ack.waiting=true;s.replay_ack.epoch=7;
    strcpy(s.replay_ack.request_id,"req");strcpy(s.replay_ack.segment_id,"part");
    strcpy(s.replay_ack.expected_type,sd_audio_type(route,SD_BEGIN_ACK));
}
static void reply(unsigned route,unsigned epoch,const char *req,const char *part,
                  const char *offset,const char *window) {
    char json[400];snprintf(json,sizeof(json),"{\"type\":\"%s\",\"request_id\":\"%s\","
        "\"segment_id\":\"%s\",\"ok\":true,\"next_offset\":%s,\"replay_version\":2,\"window_bytes\":%s}",
        sd_audio_type(route,SD_BEGIN_ACK),req,part,offset,window);
    assert(nuwax_offline_audio_handle_protocol_text(json,(int)strlen(json),epoch));
}
int main(void) {
    for(unsigned route=0;route<1;route++) {
        arm(route);reply(route,7,"req","part","12345","524288");
        assert(s.replay_ack.received_ms==123U);
        assert(signals==REPLAY_ACK_BIT && s.replay_ack.window_bytes==524288 && s.replay_ack.next_offset==12345);
        arm(route);reply(route,8,"req","part","0","524288");assert(!signals && !s.replay_ack.window_bytes);
        arm(route);reply(route,7,"old","part","0","524288");assert(!signals);
        arm(route);reply(route,7,"req","other","0","524288");assert(!signals);
        arm(route);reply((route+1)%4,7,"req","part","0","524288");assert(!signals);
        for(unsigned j=0;j<4;j++) {
            const char *bad[]={"-1","0.5","4294967296","null"};
            arm(route);reply(route,7,"req","part",bad[j],"524288");assert(!signals);
        }
        const char *bad_windows[]={"0","8063","524289","65536.5","null"};
        for(unsigned j=0;j<5;j++) {
            arm(route);reply(route,7,"req","part","0",bad_windows[j]);
            assert(signals && !s.replay_ack.window_bytes); // safely stays on v1
        }
        for(unsigned version=0;version<3;version++) {
            char json[400];arm(route);
            snprintf(json,sizeof(json),"{\"type\":\"%s\",\"request_id\":\"req\",\"segment_id\":\"part\",\"ok\":true,\"next_offset\":0,\"window_bytes\":524288%s}",
                sd_audio_type(route,SD_BEGIN_ACK),version==0?"":version==1?",\"replay_version\":1":",\"replay_version\":null");
            assert(nuwax_offline_audio_handle_protocol_text(json,(int)strlen(json),7));
            assert(signals && s.replay_ack.window_bytes==
                ((version==0 && replay_compact_protocol_allowed(true,route))?524288U:0U));
        }
        assert(!replay_bulk_window_allowed(false,route,false));
        assert(!replay_bulk_window_allowed(true,route,true));
        assert(replay_bulk_window_allowed(true,route,false)==(route!=SD_UPLOAD_LIVE));
    }
    for (unsigned route = 1; route < 4; ++route) {
        for (unsigned commit = 1; commit < 2; ++commit) {
            const char *type = sd_audio_type(route, commit ? SD_COMMIT_ACK : SD_BEGIN_ACK);
            const char *bad[] = {"-1", "0.5", "1000000", "null", "\"0\""};
            for (unsigned trial = 0; trial < 12; ++trial) {
                char json[600], key[48]; arm(route);
                assert(sd_audio_file_key(key, "meeting_one", 0));
                strcpy(s.replay_ack.segment_id, key); strcpy(s.replay_ack.expected_type, type);
                snprintf(json, sizeof(json), "{\"type\":\"%s\",\"request_id\":\"%s\","
                    "\"audio_session_id\":\"%s\",\"segment_index\":%s,\"ok\":%s%s}",
                    type, trial == 1 ? "stale" : "req", trial == 2 ? "other" : "meeting_one",
                    trial >= 3 && trial < 8 ? bad[trial - 3] : trial == 8 ? "1" : "0",
                    trial == 11 ? "false" : "true", commit ? "" : ",\"next_offset\":12345");
                assert(nuwax_offline_audio_handle_protocol_text(json, (int)strlen(json), trial == 9 ? 8 : 7));
                const bool valid = trial == 0 || trial == 10 || trial == 11;
                assert((signals != 0) == valid);
                if (valid) {
                    assert(s.replay_ack.ok == (trial != 11));
                    assert(s.replay_ack.next_offset == (commit ? 0U : 12345U));
                    assert(!s.replay_ack.window_bytes);
                }
            }
        }
        arm(route); reply(route,7,"req","part","0","524288"); assert(!signals);
    }
    for (unsigned route=1; route<4; ++route) for(unsigned trial=0;trial<12;trial++) {
        arm(route);strcpy(s.replay_ack.segment_id,"meeting_one");
        s.replay_ack.scene.count=1;s.replay_ack.scene.items[0]=(sd_scene_resume_t){2,40000,123,0};
        char json[600];
        const char *arrays[]={"[{\"segment_index\":2,\"next_offset\":12345}]","[]","null",
          "[{\"segment_index\":3,\"next_offset\":0}]","[{\"segment_index\":2,\"next_offset\":40001}]",
          "[{\"segment_index\":2,\"next_offset\":0.5}]","[{\"segment_index\":2,\"next_offset\":-1}]"};
        snprintf(json,sizeof(json),"{\"type\":\"%s\",\"request_id\":\"%s\",\"audio_session_id\":\"%s\",\"ok\":%s,\"error_code\":\"conflict\",\"resume\":%s}",
          sd_audio_type(route,SD_BEGIN_ACK),trial==7?"stale":"req",trial==8?"other":"meeting_one",trial==10?"false":"true", arrays[trial<7?trial:0]);
        assert(nuwax_offline_audio_handle_protocol_text(json,(int)strlen(json),trial==9?8:7));
        assert((signals!=0)==(trial==0||trial==10||trial==11));
        if(trial==0||trial==11)assert(s.replay_ack.scene.items[0].offset==12345);
        if(trial==10)assert(!s.replay_ack.ok && s.replay_ack.scene.error==SD_SCENE_PERMANENT);
    }
    arm(SD_UPLOAD_MEETING);strcpy(s.replay_ack.segment_id,"meeting_one");
    const char *normal="{\"type\":\"meeting_audio_begin_ack\",\"request_id\":\"req\",\"audio_session_id\":\"meeting_one\",\"ok\":true}";
    assert(nuwax_offline_audio_handle_protocol_text(normal,(int)strlen(normal),7) && signals);
    arm(SD_UPLOAD_MEETING); strcpy(s.replay_ack.segment_id, "meeting_one");
    strcpy(s.replay_ack.expected_type, "meeting_audio_session_end_ack");
    const char *end = "{\"type\":\"meeting_audio_session_end_ack\",\"request_id\":\"req\",\"audio_session_id\":\"meeting_one\",\"ok\":true}";
    assert(nuwax_offline_audio_handle_protocol_text(end, (int)strlen(end),7) && signals);
    char key[48];
    assert(!sd_audio_file_key(key,"",0) && !sd_audio_file_key(key,"../bad",0));
    assert(!sd_audio_file_key(key,"123456789012345678901234567890123456",0));
    assert(sd_audio_file_key(key,"12345678901234567890123456789012345",999999));
    puts("PASS: file ACK epoch/type/request/session/index fences; rejected stale/legacy/malformed identities; session end; legacy window compatibility");
}
