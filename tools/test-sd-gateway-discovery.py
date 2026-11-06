"""Compile the actual firmware resolver, with deterministic socket/time/PSA edges.

HMAC fixture bytes come from Node's real SHA-256; the PSA stub checks exact
inputs, leaving hardware cryptography to ESP-IDF while testing all ownership.
"""
import json, pathlib, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[1]
out = pathlib.Path(sys.argv[1])
fixture = json.loads((out / 'fixture.json').read_text())
def put(name, text):
    file = out / name
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(text, encoding='utf-8')
put('sdkconfig.h', f'''#define CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_ENABLE 1
#define CONFIG_NUWAX_SD_GATEWAY_ID "{fixture['id']}"
#define CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_KEY "{fixture['key']}"
#define CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_PORT 8811
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL "ws://10.191.67.140:8810/api/device/ws"
''')
for name in ['esp_netif.h', 'esp_timer.h', 'esp_random.h', 'esp_log.h', 'lwip/sockets.h', 'lwip/inet.h', 'psa/crypto.h']:
    put(name, '#include "platform.h"\n')
put('platform.h', r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define socket fake_socket
#define setsockopt fake_setsockopt
#define bind fake_bind
#define sendto fake_sendto
#define recvfrom fake_recvfrom
#define close fake_close
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef struct { uint32_t addr; } ip_addr;
typedef struct { ip_addr ip, netmask, gw; } esp_netif_ip_info_t;
typedef int esp_netif_t;
typedef int psa_status_t;
typedef int mbedtls_svc_key_id_t;
typedef int psa_key_attributes_t;
#define PSA_KEY_ATTRIBUTES_INIT 0
#define PSA_KEY_TYPE_HMAC 1
#define PSA_ALG_SHA_256 2
#define PSA_ALG_HMAC(a) (a)
#define PSA_KEY_USAGE_SIGN_MESSAGE 1
#define PSA_KEY_USAGE_VERIFY_MESSAGE 2
#define PSA_SUCCESS 0
static int64_t clock_us;
static int scenario, receives, sends, sockets, keys, adapter;
static unsigned char fixture_query[72], fixture_reply[76];
static int64_t esp_timer_get_time(void) { return clock_us; }
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { assert(!strcmp(key, "WIFI_STA_DEF")); return &adapter; }
static int esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *info) {
    (void)n; info->ip.addr=inet_addr(scenario==4 && receives ? "10.1.1.5" : "10.191.67.54");
    info->netmask.addr=inet_addr("255.255.255.0"); return 0;
}
static void esp_fill_random(void *out, size_t count) { memset(out, 0xab, count); }
static int psa_crypto_init(void) { return 0; }
static void psa_set_key_type(int *a, int b) { (void)a; (void)b; }
static void psa_set_key_algorithm(int *a, int b) { (void)a; (void)b; }
static void psa_set_key_usage_flags(int *a, int b) { (void)a; (void)b; }
static void psa_reset_key_attributes(int *a) { (void)a; }
static int psa_import_key(int *a, const uint8_t *k, size_t len, int *id) { (void)a; (void)k; assert(len==32); *id=1; ++keys; return 0; }
static int psa_destroy_key(int id) { assert(id==1); --keys; return 0; }
static int psa_mac_compute(int key, int alg, const uint8_t *in, size_t len, uint8_t *mac, size_t size, size_t *bytes) {
    (void)key; (void)alg; assert(len==40 && size==32); assert(!memcmp(in,fixture_query,40));
    memcpy(mac,fixture_query+40,32); *bytes=32; return 0;
}
static int psa_mac_verify(int key, int alg, const uint8_t *in, size_t len, const uint8_t *mac, size_t size) {
    (void)key; (void)alg; assert(len==44 && size==32);
    return memcmp(in,fixture_reply,44) || memcmp(mac,fixture_reply+44,32);
}
static int fake_socket(int a, int b, int c) { (void)a; (void)b; (void)c; if(scenario==5) return -1; ++sockets; return 7; }
static int fake_setsockopt(int s,int l,int o,const void *v,int n) { (void)s;(void)l;(void)o;(void)v;(void)n;return scenario==6 ? -1 : 0; }
static int fake_bind(int s,const struct sockaddr *a,int n) { (void)s;(void)a;(void)n; return 0; }
static int fake_sendto(int s,const void *data,size_t len,int f,const struct sockaddr *a,int n) {
    (void)s;(void)f;(void)a;(void)n; assert(len==72 && !memcmp(data,fixture_query,72)); ++sends; return (int)len;
}
static int fake_recvfrom(int s,void *buf,size_t len,int f,struct sockaddr *addr,int *n) {
    (void)s;(void)f;(void)n; assert(len>=76); ++receives; clock_us+=100000;
    if(scenario==1 || scenario==3 || scenario==4) return -1;
    struct sockaddr_in *peer=(struct sockaddr_in *)addr;
    peer->sin_family=AF_INET; peer->sin_port=htons(8811); peer->sin_addr.s_addr=inet_addr("10.191.67.140");
    memcpy(buf,fixture_reply,76);
    if(scenario==2 && receives==1) ((uint8_t*)buf)[44]^=1;
    if(scenario==2 && receives==2) ((uint8_t*)buf)[8]^=1;
    if(scenario==2 && receives==3) peer->sin_addr.s_addr=inet_addr("10.1.1.140");
    return 76;
}
int fake_close(int s) { assert(s==7); --sockets; return 0; }
''')
put('harness.c', f'''#include "sd_gateway_discovery.c"
static bool cancel(void *ctx) {{ (void)ctx; return scenario==3 && clock_us>=200000; }}
int main(void) {{
    assert(sd_discovery_hex("{fixture['query']}", fixture_query, 72));
    assert(sd_discovery_hex("{fixture['reply']}", fixture_reply, 76));
    char url[128];
    for(scenario=0;scenario<7;scenario++) {{
        clock_us=0;receives=0;sends=0;
        int result=nuwax_sd_gateway_discover(url,sizeof(url),cancel,NULL);
        assert(sockets==0 && keys==0);
        if(scenario==0 || scenario==2) {{ assert(result==ESP_OK); assert(!strcmp(url,"ws://10.191.67.140:8810/api/device/ws")); }}
        if(scenario==1) {{ assert(result==ESP_ERR_TIMEOUT); assert(clock_us<=2100000 && sends<=12); }}
        if(scenario==2) assert(receives==4);
        if(scenario==3 || scenario==4) assert(result==ESP_ERR_NOT_FINISHED);
        if(scenario==5) assert(result==ESP_ERR_NO_MEM);
        if(scenario==6) assert(result==ESP_FAIL);
    }}
    scenario=0;clock_us=0;
    assert(nuwax_sd_gateway_discover(url,4,cancel,NULL)==ESP_ERR_INVALID_SIZE);
    assert(sockets==0 && keys==0);
    puts("PASS: actual device discovery: HMAC wire bytes, invalid reply/nonce/subnet, bounded timeout, cancellation, IP change, socket failure, buffer bounds and cleanup");
    return 0;
}}
''')
exe = out / 'device-discovery.exe'
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(out),
    '-I'+str(root/'main/audio_modes/meeting/host_stubs'), '-I'+str(root/'main'),
    str(out/'harness.c'),'-o',str(exe),'-lws2_32'], check=True)
subprocess.run([str(exe)], check=True)
