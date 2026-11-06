"""Fault-inject the actual provisioning response hooks and generated transport."""
from pathlib import Path
import importlib.util
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/provision-finish'
out.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('patcher', root / 'tools/patch-protocomm-lifecycle.py')
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)
sdk = Path('C:/esp/v6.0/esp-idf/components/protocomm/src/transports/protocomm_nimble.c')
transport = patcher.patch(sdk.read_text(encoding='utf-8'))
app = (root / 'main/nuwax_provision_new/nuwax_provision_main.c').read_text(encoding='utf-8-sig')
main = (root / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')


def function(source, signature):
    start = source.rindex(signature)
    pos = source.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "nuwax_provision_policy.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define TAG "host"
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static unsigned errors;
#define ESP_LOGE(...) ((void)++errors)
'''

hooks = common + r'''
typedef int portMUX_TYPE;
typedef int provision_wifi_reset_kind_t;
typedef int nuwax_provision_outcome_t;
static int s_state_lock, locked;
#define portENTER_CRITICAL(p) do {assert((p)==&s_state_lock && !locked);locked=1;}while(0)
#define portEXIT_CRITICAL(p) do {assert((p)==&s_state_lock && locked);locked=0;}while(0)
static int64_t now_us;
static int64_t esp_timer_get_time(void){return now_us;}
static int done_error;
static int __real_network_prov_mgr_done(void){return done_error;}
static uint32_t s_success_query_sequence;
''' + app[app.index('typedef struct {\n    bool got_ip;'):app.index('static portMUX_TYPE s_state_lock')] .split('static provision_state_t')[0] + r'''
static provision_state_t s_state;
''' + '\n'.join(function(app, sig) for sig in [
    'esp_err_t __wrap_network_prov_mgr_done(void)',
    'uint32_t nuwax_provision_success_query_revision(void)',
    'void nuwax_provision_success_response_read(uint32_t revision,'])

gatt_stubs = r'''
#define BLE_UUID_STR_LEN 40
#define BLE_UUID128_VAL_LENGTH 16
#define BLE_GATT_ACCESS_OP_READ_CHR 1
#define BLE_GATT_ACCESS_OP_WRITE_CHR 2
#define BLE_ATT_ERR_UNLIKELY 10
#define BLE_ATT_ERR_INSUFFICIENT_RES 11
#define BLE_ATT_ERR_INVALID_PDU 12
struct om {unsigned om_len;};
struct chr {void *uuid;};
struct ble_gatt_access_ctxt {int op;struct om *om;struct chr *chr;};
#define OS_MBUF_PKTLEN(p) ((p)->om_len)
struct data_mbuf {uint32_t success_query_revision;};
static struct data_mbuf response;
static bool transport_active=true;
static bool protocomm_ble_transport_active(void){return transport_active;}
static struct {void *pc_ble;uint16_t gatt_mtu;} internal={NULL,247}, *protoble_internal=&internal;
static struct data_mbuf *find_attr_with_handle(uint16_t h){return h==1?&response:NULL;}
static int append_error, set_error, request_error;
static bool connected_status;
static const char *endpoint="prov-config";
static uint8_t response_bytes[100];
static int simple_ble_gatts_get_attr_value(uint16_t h,ssize_t *n,uint8_t **p){*n=100;*p=response_bytes;return h==1?0:-1;}
static int simple_ble_gatts_set_attr_value(uint16_t h,ssize_t n,uint8_t *p){assert(h==1 && n==100);if(!set_error)free(p);return set_error;}
static int os_mbuf_append(struct om *m,const void *p,ssize_t n){return append_error;}
static int ble_uuid_flat(void *u,void *v){return 0;}
static int ble_hs_mbuf_to_flat(struct om *m,void *p,uint16_t n,uint16_t *len){*len=n;return 0;}
static const char *uuid128_to_handler(void *uuid){return endpoint;}
static int protocomm_req_handle(void *pc,const char *e,uint16_t conn,void *data,uint16_t len,uint8_t **p,ssize_t *n){
 if(connected_status)__wrap_network_prov_mgr_done();
 if(request_error)return request_error;
 *p=malloc(100);*n=100;return 0;
}
'''
# Keep optional-hook address checks legal under -Werror with weak declarations.
hooks += '\nextern uint32_t nuwax_provision_success_query_revision(void) __attribute__((weak));\n'
hooks += 'extern void nuwax_provision_success_response_read(uint32_t,uint16_t) __attribute__((weak));\n'
hooks += gatt_stubs + function(transport, 'static int\ngatt_svr_chr_access(uint16_t conn_handle,')
hooks += r'''
static void ready(void){
 memset(&s_state,0,sizeof(s_state));s_state.ble_connected=true;s_state.security_secured=true;
 s_state.wifi_owner=NUWAX_PROVISION_WIFI_OWNER_APP;s_state.got_ip=true;
 s_state.owner_generation=4;s_state.ble_conn_handle=7;
 response.success_query_revision=0;done_error=append_error=request_error=set_error=0;
 endpoint="prov-config";connected_status=true;now_us=1000000;
}
int main(void){
 struct om om={1};struct chr chr={0};struct ble_gatt_access_ctxt ctx={2,&om,&chr};
 ready();assert(!gatt_svr_chr_access(7,1,&ctx,NULL));
 assert(response.success_query_revision && !s_state.success_response_read_us);
 // Generated success response alone is not evidence of an APP read.
 assert(!nuwax_provision_success_read_grace_elapsed(1000000,0,14000000));
 ctx.op=1;protoble_internal->gatt_mtu=23;gatt_svr_chr_access(7,1,&ctx,NULL);
 assert(!s_state.success_response_read_us);protoble_internal->gatt_mtu=247;
 append_error=1;assert(gatt_svr_chr_access(7,1,&ctx,NULL)==11);
 assert(!s_state.success_response_read_us);append_error=0;
 assert(!gatt_svr_chr_access(8,1,&ctx,NULL));assert(!s_state.success_response_read_us);
 assert(!gatt_svr_chr_access(7,1,&ctx,NULL));assert(s_state.success_response_read_us==1000000);
 now_us=1500000;gatt_svr_chr_access(7,1,&ctx,NULL);
 assert(s_state.success_response_read_us==1000000); // polling cannot extend grace
 assert(!nuwax_provision_success_read_grace_elapsed(2000000,1000000,3999999));
 assert(nuwax_provision_success_read_grace_elapsed(2000000,1000000,4000000));
 assert(!nuwax_provision_success_read_grace_elapsed(1000000,3000000,4999999));
 assert(nuwax_provision_success_read_grace_elapsed(1000000,3000000,5000000));
 assert(!nuwax_provision_success_read_grace_elapsed(1000000,6000000,5000000));
 uint32_t stale=response.success_query_revision;
 ready();ctx.op=2;gatt_svr_chr_access(7,1,&ctx,NULL);
 assert(response.success_query_revision!=stale);
 nuwax_provision_success_response_read(stale,7);assert(!s_state.success_response_read_us);
 for(int mode=0;mode<6;mode++){
  ready();ctx.op=2;
  if(mode==0)connected_status=false;
  if(mode==1)endpoint="vox-config";
  if(mode==2)done_error=-1;
  if(mode==3)s_state.app_abort_pending=true;
  if(mode==4)s_state.security_secured=false;
  if(mode==5)set_error=-1;
  gatt_svr_chr_access(7,1,&ctx,NULL);ctx.op=1;gatt_svr_chr_access(7,1,&ctx,NULL);
  assert(!s_state.success_response_read_us);
 }
 ready();ctx.op=2;gatt_svr_chr_access(7,1,&ctx,NULL);
 assert(response.success_query_revision);request_error=-1;gatt_svr_chr_access(7,1,&ctx,NULL);
 ctx.op=1;gatt_svr_chr_access(7,1,&ctx,NULL);assert(!s_state.success_response_read_us);
 puts("PASS: actual GATT write/read + APP hooks reject stale/wrong/failed responses; commit/read grace boundaries");
}
'''

advertise = common + r'''
static atomic_bool s_nuwax_ble_stopping=true;
static bool active=true,enabled=true;
static bool protocomm_ble_transport_active(void){return active;}
static bool ble_hs_is_enabled(void){return enabled;}
#define BLE_HS_ADV_F_DISC_GEN 1
#define BLE_HS_ADV_F_BREDR_UNSUP 2
#define BLE_GAP_CONN_MODE_UND 1
#define BLE_GAP_DISC_MODE_GEN 1
#define BLE_HS_FOREVER -1
#define BLE_HS_EDISABLED 30
static int own_addr_type;
static void *simple_ble_gap_event;
struct ble_hs_adv_fields {int flags,num_uuids128,uuids128_is_complete;};
static struct ble_hs_adv_fields adv_data,resp_data;
static struct {int conn_mode,disc_mode,itvl_min,itvl_max;} adv_params;
static int calls,stop_at,error_at;
static int call(void){calls++;if(calls==stop_at)atomic_store(&s_nuwax_ble_stopping,true);return calls==error_at?30:0;}
static int ble_gap_adv_set_fields(void *p){return call();}
static int ble_gap_adv_rsp_set_fields(const void *p){return call();}
static int ble_gap_adv_start(int a,void *b,int c,void *d,void *e,void *f){return call();}
''' + function(transport, 'static void\nsimple_ble_advertise(void)') + r'''
int main(void){
 simple_ble_advertise();assert(calls==0);
 for(int stage=1;stage<=3;stage++){
  atomic_store(&s_nuwax_ble_stopping,false);calls=errors=0;stop_at=error_at=stage;
  simple_ble_advertise();assert(calls==stage && errors==0);
  simple_ble_advertise();assert(calls==stage);
 }
 atomic_store(&s_nuwax_ble_stopping,false);calls=errors=stop_at=error_at=0;
 simple_ble_advertise();assert(calls==3); // next session advertises normally
 calls=0;error_at=1;simple_ble_advertise();assert(calls==1 && errors==1);
 calls=0;enabled=false;simple_ble_advertise();assert(!calls);
 enabled=true;active=false;simple_ble_advertise();assert(!calls);
 puts("PASS: no advertising after stop; stop between GAP calls; next session; normal failures preserved");
}
'''

# Execute the actual disconnect tail with all cleanup owners enabled.
start = main.index('        bool awake_session_preserved = false;')
tail = main[start:main.index('\n    }\n}', start)]
handoff = common + r'''
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 1
#define CONFIG_NUWAX_SPEAKER_ENABLE 1
#define CONFIG_NUWAX_WAKE_WORD_ENABLE 1
#define CONFIG_NUWAX_OTA_ENABLE 0
#define NUWAX_APP_STATE_ERROR 1
#define NUWAX_CLOUD_DISCONNECTED 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 99999
static atomic_bool s_reprovision_quiesce_requested;
static bool binding_required;
static bool nuwax_provision_main_binding_required(void){return binding_required;}
''' + function(main, 'static bool cloud_connect_cancelled_for_reprovision(') + r'''
static bool exclusive_mode_was_active,conversation_was_in_flight,late_handoff;
static int ws,barge_dispatch_binding;
static unsigned sleeps,wakes,retries,closed,aec,queues,replay,speaker,preserves;
static bool preserve_awake_session_for_ws_reconnect(const char *r){preserves++;return false;}
static void clear_awake_session(const char *r){}
static void nuwax_barge_dispatcher_unbind(void *w,int b){}
static void unpublish_active_ws(void){}
static int nuwax_speaker_playback_reset_runtime(void){speaker++;return 0;}
static void set_state(int s){}
static void nuwax_diagnostics_log_heap_label(const char *t,const char *s){}
static void vTaskDelay(int n){}
static void esp_restart(void){abort();}
static void clear_auto_followup(const char *r){}
static void nuwax_speaker_playback_release_idle_output(const char *r){}
static void discard_connection_bound_queues(const char *r){queues++;}
static bool nuwax_offline_audio_replay_formal_running(void){return true;}
static unsigned nuwax_ws_connection_epoch(void *w){return 3;}
static int nuwax_offline_audio_replay_formal_cancel(unsigned e,unsigned t){assert(e==3 && t==6000);replay++;return 0;}
static bool offline_mode_lease_pending(void){return false;}
static bool nuwax_offline_audio_capture_running(void){return false;}
static bool nuwax_offline_audio_is_active(void){return false;}
static void enter_sleeping_conversation_state(const char *r){sleeps++;}
static int nuwax_ws_owner_close(void *w){closed++;if(late_handoff)atomic_store(&s_reprovision_quiesce_requested,true);return 0;}
static void observe_ws_owner_cleanup_result(void *w,int e,const char *r){}
static int conversation_live_aec_pool_suspend(const char *r){aec++;return 0;}
static void nuwax_app_shell_set_cloud_state(int s){}
static void service_offline_wake_word_detector(const char *r){wakes++;}
static bool nuwax_wifi_is_connected(void){return false;}
static int recover_wifi_if_needed(void){retries++;return 0;}
static void set_cloud_transport_connecting_state(const char *r){}
static void delay_before_cloud_reconnect(const char *r,int h,int e){retries++;}
static void cleanup(void){do {
''' + tail + r'''
}while(0);}
int main(void){
 for(int scenario=0;scenario<4;scenario++){
  sleeps=wakes=retries=closed=aec=queues=replay=speaker=preserves=0;
  atomic_store(&s_reprovision_quiesce_requested,scenario==1);late_handoff=scenario==2;
  binding_required=scenario==3;
  cleanup();assert(closed==1 && aec==1 && queues==1 && replay==1 && speaker==1);
  if(scenario==0)assert(sleeps==1 && wakes==1 && retries==2);
  else assert(wakes==0 && retries==0);
  if(scenario==1 || scenario==3)assert(sleeps==0 && preserves==0);
  else assert(preserves==1);
 }
 puts("PASS: handoff preserves speaker/replay/WS/AEC cleanup and bypasses wake/reconnect, including late handoff");
}
'''

for name, code in [('response', hooks), ('advertise', advertise), ('handoff', handoff)]:
    src, exe = out / (name + '.c'), out / (name + '.exe')
    src.write_text(code, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-variable',
                    '-Wno-address',  # host fixture links optional weak hooks in the same TU
                    '-I' + str(root / 'main/nuwax_provision_new'),
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# Source-order checks cover the outer handoff ACK and its failed-close path.
park = main[main.index('const esp_err_t park_close_err'):main.index('cloud owner parked; BLE')]
assert park.index('if (park_close_err != ESP_OK)') < park.index('&s_cloud_owner_parked, true')
assert 'continue;' in park[:park.index('&s_cloud_owner_parked, true')]
assert 'CONFIG_NUWAX_PROVISION_SUCCESS_BLE_HOLD_MS * 1000' in app
assert transport.index('atomic_store_explicit(&s_nuwax_ble_stopping, true', transport.index('esp_err_t protocomm_ble_stop')) < transport.index('esp_err_t rc = ble_gap_adv_stop();')
print('PASS: failed close cannot acknowledge network handoff; success fallback window retained; stop fence precedes GAP stop')

mutants = [
    ('failed_read_accepted', hooks.replace('rc == 0 && temp_outlen > 0', 'temp_outlen > 0', 1)),
    ('stale_response_accepted', hooks.replace('revision == s_state.success_query_revision', 'true', 1)),
    ('advertise_after_stop', advertise.replace(
        'atomic_load_explicit(&s_nuwax_ble_stopping, memory_order_acquire) ||', 'false ||', 1)),
    ('handoff_reconnect', handoff.replace(
        'if (cloud_connect_cancelled_for_reprovision(NULL))',
        'if (false)', 1)),
]
for name, code in mutants:
    src, exe = out / (name + '.c'), out / (name + '.exe')
    src.write_text(code, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-address',
                    '-I' + str(root / 'main/nuwax_provision_new'),
                    str(src), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True)
    assert result.returncode != 0, 'Regression escaped: ' + name
    print('PASS: negative mutant rejected ' + name)
