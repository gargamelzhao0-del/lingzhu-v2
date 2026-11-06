"""Compile production connection/OTA helpers with deterministic failure injection."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/network-join-handoff'
out.mkdir(parents=True, exist_ok=True)

def extract(path, start, end):
    text = (root / path).read_text(encoding='utf-8-sig')
    a = text.index(start)
    return text[a:text.index(end, a + len(start))]

common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_INVALID_ARG 4
#define ESP_ERR_WIFI_NOT_CONNECT 5
#define ESP_ERR_WIFI_STATE 6
#define ESP_ERR_NOT_FOUND 7
static void log_ignore(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
static const char *TAG="host";
#define ESP_LOGI(...) log_ignore(__VA_ARGS__)
#define ESP_LOGW(...) log_ignore(__VA_ARGS__)
#define ESP_LOGE(...) log_ignore(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr,...) do {int r=(expr);if(r) return r;}while(0)
static const char *esp_err_to_name(int e){(void)e;return "stub";}
static int64_t now_us;
static int64_t esp_timer_get_time(void){return now_us;}
'''

tcp = common + r'''
typedef int socklen_t;
struct sockaddr {int unused;};
struct timeval {long tv_sec,tv_usec;};
typedef struct {int fd;} fd_set;
#define FD_ZERO(p) ((p)->fd=-1)
#define FD_SET(s,p) ((p)->fd=(s))
#define AF_UNSPEC 0
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_ERROR 1
#define NUWAX_DEVICE_ERROR_RESULT_BYTES 256
#define nuwax_device_error_report_enqueue_detail(...) (++durable_reports)
static unsigned durable_reports;
struct addrinfo {int ai_family,ai_socktype,ai_protocol; struct sockaddr *ai_addr;int ai_addrlen;struct addrinfo *ai_next;};
typedef struct {const char *host;int port;} parsed_ws_url_t;
typedef struct {bool (*connect_cancelled)(void *);void *user_ctx;uint32_t tcp_connect_timeout_ms;bool local_connect_diagnostics_only;} nuwax_ws_config_t;
static unsigned opens,closes,polls,freed;
static int scenario;
static int64_t cancel_at;
static bool cancelled(void *p){(void)p;return cancel_at>=0 && now_us>=cancel_at;}
static struct addrinfo addresses[2];
static int getaddrinfo(const char *host,const char *port, const struct addrinfo *h,struct addrinfo **r){
 (void)host;(void)port;(void)h;memset(addresses,0,sizeof addresses);
 if(scenario==5){addresses[0].ai_next=&addresses[1];}
 *r=addresses;return 0;
}
static void freeaddrinfo(struct addrinfo *p){assert(p==addresses);freed++;}
static int socket(int a,int b,int c){(void)a;(void)b;(void)c;opens++;return 3;}
static void close(int s){assert(s==3);closes++;}
static int configure_socket_nonblocking(int s){assert(s==3);return 0;}
static void configure_socket_timeouts(int s){assert(s==3);}
static int connect(int s,struct sockaddr *a,int n){(void)s;(void)a;(void)n;errno=EINPROGRESS;return -1;}
static int select(int n,void *r,fd_set *w,fd_set *e,struct timeval *t){
 assert(n==4 && !r && w->fd==3 && e->fd==3);assert(t->tv_usec<=100000 && t->tv_usec>0);
 now_us+=t->tv_usec;polls++;
 if(scenario==4 && polls==1){errno=EINTR;return -1;}
 return scenario==1 || scenario==2 ? 0 : 1;
}
static int getsockopt(int s,int level,int opt,void *p,socklen_t *n){
 assert(s==3 && level==SOL_SOCKET && opt==SO_ERROR && *n==(int)sizeof(int));
 *(int *)p=(scenario==3 || (scenario==5 && opens==1))?ECONNREFUSED:0;return 0;
}
''' + extract('main/ws_transport.c', 'static int wait_tcp_connect(', 'static esp_err_t connect_tls(') + r'''
int main(void){
 parsed_ws_url_t url={"gateway",8810};nuwax_ws_config_t cfg={cancelled,0,3000,false};
 for(scenario=0;scenario<6;scenario++){
  now_us=0;opens=closes=polls=freed=0;cancel_at=scenario==2?150000:-1;
  int fd=connect_tcp(&url,&cfg);assert(freed==1);
  if(scenario==1){assert(fd<0 && now_us==3000000 && polls==30);}
  else if(scenario==2){assert(fd<0 && now_us==200000);}
  else if(scenario==3){assert(fd<0);}
  else {assert(fd==3);close(fd);}
  assert(opens==closes);if(scenario==5)assert(opens==2);
 }
 scenario=2;now_us=0;cancel_at=0;opens=closes=polls=freed=0;
 assert(connect_tcp(&url,&cfg)<0 && polls==0 && opens==closes);
 scenario=1;cancel_at=-1;now_us=0;cfg.tcp_connect_timeout_ms=0;
 assert(connect_tcp(&url,&cfg)<0 && now_us==15000000);
 scenario=1;now_us=0;cfg.tcp_connect_timeout_ms=3000;cfg.local_connect_diagnostics_only=true;durable_reports=0;
 assert(connect_tcp(&url,&cfg)<0 && durable_reports==0);
 now_us=0;cfg.local_connect_diagnostics_only=false;
 assert(connect_tcp(&url,&cfg)<0 && durable_reports==1);
 puts("PASS: local test endpoint timeouts do not persist; formal endpoint diagnostics preserved");
 puts("PASS: production TCP success/refusal/address fallback/EINTR/3s timeout/default timeout/cancellation; all sockets closed");
}
'''

wifi = common + r'''
typedef unsigned EventBits_t;
typedef struct {struct {char ssid[33],password[65];int scan_method,sort_method;}sta;}wifi_config_t;
typedef struct {int reason; int rssi; unsigned char bssid[6];} wifi_event_sta_disconnected_t;
#define WIFI_FAST_SCAN 1
#define WIFI_CONNECT_AP_BY_SIGNAL 1
#define ESP_IDF_VERSION 0
#define ESP_IDF_VERSION_VAL(a,b,c) 1
#define WIFI_CONNECTED_BIT 1U
#define WIFI_FAIL_BIT 2U
#define WIFI_FALLBACK_BIT 4U
#define WIFI_STA_STOPPED_BIT 32U
#define WIFI_STA_STARTED_BIT 64U
#define MAX_WIFI_RETRY 8
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WIFI_REASON_AUTH_EXPIRE 2
#define WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY 4
#define WIFI_REASON_ASSOC_FAIL 203
#define WIFI_REASON_CONNECTION_FAIL 205
#define WIFI_REASON_NO_AP_FOUND 201
#define WIFI_REASON_STA_LEAVING 36
#define CONFIG_NUWAX_WIFI_SSID_B ""
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
static atomic_bool s_join_refresh_allowed,s_join_refresh_pending,s_wifi_connect_requested,s_wifi_driver_connecting;
static atomic_bool s_sta_restart_needs_start;
static atomic_bool s_wifi_simulated_offline,s_synchronous_connect_active,s_wifi_scan_in_progress,s_reconnect_after_scan,s_fallback_in_progress;
static atomic_uint s_join_failure_streak,s_wifi_intent_revision;
static atomic_int s_wifi_retry_count;
static int s_wifi_event_group=1;
static char s_current_ssid[33],s_current_ip[16];
static unsigned bits,configs,driver_calls,disconnects,events;
static unsigned stops,starts;
static int scenario;
static bool provisioning;
static size_t strlcpy(char *dst,const char *src,size_t n){size_t len=strlen(src);snprintf(dst,n,"%s",src);return len;}
static bool nuwax_wifi_provisioning_active(void){return provisioning;}
static int wifi_init_once(void){return 0;}
static int wifi_operation_lock(void){return 0;}
static void wifi_operation_unlock(void){}
static int wifi_driver_disconnect(void){disconnects++;return 0;}
static int wifi_driver_connect_once(void){driver_calls++;return 0;}
static int wifi_set_config_guarded(wifi_config_t *c){assert(strcmp(c->sta.ssid,"ssid")==0);configs++;return 0;}
static void wifi_make_station_config(wifi_config_t *c,const char *ssid,const char *password){strlcpy(c->sta.ssid,ssid,sizeof c->sta.ssid);strlcpy(c->sta.password,password,sizeof c->sta.password);}
static int esp_wifi_stop(void){assert(!s_wifi_connect_requested);stops++;if(scenario==7)return ESP_FAIL;if(scenario!=9)bits|=WIFI_STA_STOPPED_BIT;return ESP_OK;}
static int esp_wifi_start(void){assert(!s_wifi_connect_requested);starts++;if(scenario==8)return ESP_FAIL;bits|=WIFI_STA_STARTED_BIT;if(scenario==10)provisioning=true;return ESP_OK;}
static void vTaskDelay(unsigned ms){now_us+=(int64_t)ms*1000;}
static void xEventGroupClearBits(int g,unsigned b){assert(g==1);bits&=~b;}
static void xEventGroupSetBits(int g,unsigned b){assert(g==1);bits|=b;}
''' + extract('main/wifi_manager.c','static bool wifi_join_refresh_needed(', 'static uint32_t wifi_reconnect_backoff_ms(')
disconnect = extract('main/wifi_manager.c', '    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {', '    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {')
wifi += '\nstatic void emit_disconnect(int reason){wifi_event_sta_disconnected_t e={.reason=reason};void *event_data=&e;\n' + disconnect.split('{',1)[1] + '\n}\n'
wifi += r'''
static EventBits_t xEventGroupWaitBits(int g,unsigned mask,int clear,int all,unsigned ms){
 if(mask==WIFI_STA_STOPPED_BIT || mask==WIFI_STA_STARTED_BIT){assert(g==1 && !clear && !all && ms==1000);if(!(bits&mask))vTaskDelay(ms);return bits&mask;}
 assert(g==1 && mask==3 && !clear && !all && ms<=100);vTaskDelay(ms);
 if(scenario==4){atomic_store(&s_wifi_simulated_offline,true);atomic_fetch_add(&s_wifi_intent_revision,1);return bits;}
 if(scenario==5){provisioning=true;return bits;}
 if(scenario==6){atomic_fetch_add(&s_wifi_intent_revision,1);return bits;}
 if(scenario==3)return bits; /* silence: exhaust shared deadline */
 if(scenario==0 && configs==2){bits|=WIFI_CONNECTED_BIT;return bits;}
 if(now_us%1000000==0){events++;emit_disconnect(scenario==11?201:(scenario==2?202:(events%2?4:205)));}
 return bits;
}
''' + extract('main/wifi_manager.c','static esp_err_t wifi_connect_with_credentials_locked(', 'esp_err_t nuwax_wifi_connect(void)') + r'''
int main(void){
 for(scenario=0;scenario<12;scenario++){
  configs=events=driver_calls=disconnects=bits=stops=starts=0;now_us=0;provisioning=false;s_sta_restart_needs_start=false;
  atomic_store(&s_wifi_simulated_offline,false);atomic_store(&s_wifi_intent_revision,0);
  int r=wifi_connect_with_credentials("ssid","password");
  assert(!atomic_load(&s_join_refresh_allowed) && !atomic_load(&s_synchronous_connect_active));
  if(scenario==0){assert(r==0 && configs==2 && now_us==2100000);}
  if(scenario==1){assert(r==ESP_FAIL && configs==2 && now_us<=30000000);}
  if(scenario==2){assert(r==ESP_FAIL && configs==1);}
  if(scenario==3){assert(r==ESP_ERR_TIMEOUT && now_us==30000000 && configs==1);}
  if(scenario>=4 && scenario<=6){assert(r==ESP_ERR_INVALID_STATE && configs==1 && now_us==100000);}
  if(scenario==7){assert(r==ESP_FAIL && stops==1 && starts==0 && configs==1);}
  if(scenario==8){assert(r==ESP_FAIL && stops==1 && starts==1 && configs==1 && s_sta_restart_needs_start);}
  if(scenario==9){assert(r==ESP_ERR_TIMEOUT && stops==1 && starts==1 && configs==1 && !s_sta_restart_needs_start);}
  if(scenario==10){assert(r==ESP_ERR_INVALID_STATE && stops==1 && starts==1 && configs==1);}
  if(scenario==11){assert(r==ESP_FAIL && !stops && !starts && configs==1);}
  assert(stops<=1 && starts<=1 && now_us<=30000000);
 }
 atomic_store(&s_join_refresh_allowed,true);atomic_store(&s_join_failure_streak,0);
 assert(!wifi_join_refresh_needed(2));assert(!wifi_join_refresh_needed(201));
 assert(wifi_join_refresh_needed(205));assert(!wifi_join_refresh_needed(4));assert(!wifi_join_refresh_needed(205));
 const int logged_reasons[]={201,201,201,201,201,2,201,2};
 s_join_refresh_allowed=true;s_join_failure_streak=0;unsigned refreshes=0;
 for(unsigned i=0;i<sizeof logged_reasons/sizeof *logged_reasons;i++)refreshes+=wifi_join_refresh_needed(logged_reasons[i]);
 assert(refreshes==1);
 provisioning=false;s_wifi_connect_requested=true;s_wifi_driver_connecting=true;s_wifi_retry_count=0;
 unsigned calls=driver_calls;bits=WIFI_CONNECTED_BIT;emit_disconnect(36);
 assert(!s_wifi_retry_count && s_wifi_driver_connecting && driver_calls==calls && bits==WIFI_CONNECTED_BIT);
 puts("PASS: logged mixed AP/auth failures trigger exactly one real restart; local disconnect cannot consume a newer attempt; STOP/START errors, event timeout and handoff covered");
 puts("PASS: production Wi-Fi callback/owner transient refresh; one refresh maximum; rejection/missing-AP exclusions; shared deadline; provisioning/offline/intent cancellation");
}
'''

ota = common + r'''
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 1
#define pdMS_TO_TICKS(x) (x)
static struct {atomic_bool replay_running;}s;
static atomic_bool s_replay_test_transport_quarantined;
static bool active,worker,formal,initialized=true;
static int scenario;
static bool enabled(void){return true;}
static bool runtime_initialized(void){return initialized;}
static bool replay_ota_active(void){return active;}
static bool nuwax_offline_audio_replay_formal_running(void){return formal;}
static bool nuwax_offline_audio_replay_test_running(void){return worker||atomic_load(&s_replay_test_transport_quarantined);}
static bool nuwax_offline_audio_replay_running(void){return atomic_load(&s.replay_running)||formal||nuwax_offline_audio_replay_test_running();}
static uint32_t offline_now_ms(void){return (uint32_t)(now_us/1000);}
static void vTaskDelay(unsigned ms){
 now_us+=(int64_t)ms*1000;
 if(scenario==0){worker=formal=false;atomic_store(&s.replay_running,false);}
 if(scenario==2){atomic_store(&s.replay_running,now_us<40000);if(now_us>=40000)worker=false;}
 if(scenario==3)atomic_store(&s_replay_test_transport_quarantined,true);
}
''' + extract('main/offline_audio_spool.c','bool nuwax_offline_audio_replay_blocks_ota(void)', '\nbool nuwax_offline_audio_')
# Keep exactly the two functions under test if the next public symbol changes.
assert 'nuwax_offline_audio_replay_prepare_ota' in ota
ota += r'''
int main(void){
 (void)TAG;active=false;assert(nuwax_offline_audio_replay_prepare_ota(2500)==ESP_ERR_INVALID_STATE);
 active=true;worker=true;assert(!nuwax_offline_audio_replay_blocks_ota());
 assert(nuwax_offline_audio_replay_prepare_ota(2500)==ESP_OK && !worker);
 worker=true;scenario=1;now_us=0;assert(nuwax_offline_audio_replay_prepare_ota(2500)==ESP_ERR_TIMEOUT && now_us==2500000);
 scenario=2;now_us=0;assert(nuwax_offline_audio_replay_prepare_ota(2500)==ESP_OK && now_us==40000);
 worker=false;formal=true;scenario=0;assert(!nuwax_offline_audio_replay_blocks_ota());
 assert(nuwax_offline_audio_replay_prepare_ota(2500)==ESP_OK && !formal);
 atomic_store(&s.replay_running,true);scenario=0;
 assert(!nuwax_offline_audio_replay_blocks_ota());
 assert(nuwax_offline_audio_replay_prepare_ota(2500)==ESP_OK && !atomic_load(&s.replay_running));
 worker=true;scenario=3;assert(nuwax_offline_audio_replay_prepare_ota(2500)==ESP_ERR_INVALID_STATE);
 atomic_store(&s_replay_test_transport_quarantined,true);assert(nuwax_offline_audio_replay_prepare_ota(2500)==ESP_ERR_INVALID_STATE);
 puts("PASS: production OTA waits for test/formal replay and late worker publication; timeout/quarantine refuse firmware write; queued reservation required");
}
'''

for name, code in [('tcp', tcp), ('wifi', wifi), ('ota', ota)]:
    src = out / (name + '.c')
    exe = out / (name + '.exe')
    src.write_text(code, encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
