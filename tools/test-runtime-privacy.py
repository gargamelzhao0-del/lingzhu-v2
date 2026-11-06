"""Capture actual diagnostic functions and prove payloads stay off the log."""
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/runtime-privacy'
out.mkdir(parents=True,exist_ok=True)
def function(path, signature):
    s=(root/path).read_text(encoding='utf-8-sig')
    start=s.index(signature);end=s.index('{',start)+1;depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]
production=function('main/session_protocol.c','static void log_text_preview(')+'\n'+function('main/nuwax_ai_partner_main.c','static void log_cloud_connect_failure(')
common=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef int esp_err_t;
typedef int nuwax_ws_client_t;
typedef struct {char device_secret[64],cloud_url[128],device_id[48];} nuwax_cloud_config_t;
static char output[8192];
static void log_capture(const char *f,...){va_list ap;va_start(ap,f);size_t n=strlen(output);vsnprintf(output+n,sizeof(output)-n,f,ap);va_end(ap);}
#define ESP_LOGI(tag,...) log_capture(__VA_ARGS__)
#define ESP_LOGE(tag,...) log_capture(__VA_ARGS__)
static int nuwax_device_error_report_enqueue(const char*a,const char*b,const char*c,int e,void*p,unsigned n,void*q){return 0;}
static const char *esp_err_to_name(int e){return "ESP_FAIL";}
static int nuwax_ws_last_http_status(const nuwax_ws_client_t *ws){return 403;}
'''
tests=r'''
int main(void){
 nuwax_cloud_config_t c={.device_secret="PRIVATE_KEY_0123456789_END",.cloud_url="wss://cloud.invalid/path?token=PRIVATE_URL_TOKEN",.device_id="PRIVATE_DEVICE"};
 nuwax_cloud_config_t saved=c;
 log_text_preview("ASR final text","PRIVATE_USER_SPEECH");
 log_text_preview("Text response assistant","PRIVATE_ASSISTANT_RESPONSE");
 log_text_preview("empty",NULL);
 log_cloud_connect_failure("websocket_connect",&c,NULL,1);
 assert(memcmp(&c,&saved,sizeof(c))==0);
 assert(!strstr(output,"PRIVATE")&&!strstr(output,"token=")&&!strstr(output,"secret_hint")&&!strstr(output,"secret_fingerprint"));
 assert(strstr(output,"content=redacted")&&strstr(output,"secret_present=1")&&strstr(output,"secret_len=26"));
 output[0]=0;memset(c.device_secret,'Q',sizeof(c.device_secret));
 log_cloud_connect_failure("hello_send",&c,NULL,1);
 assert(strstr(output,"secret_present=0 secret_len=0")&&!strchr(output,'Q'));
 log_cloud_connect_failure("config_load",NULL,NULL,1);
 assert(strstr(output,"config=missing"));
 puts("PASS: log capture redacts speech/answer/key/URL; protocol buffers unchanged; null and unterminated inputs bounded");
}
'''
def run(name,body,negative=False):
    src=out/(name+'.c');exe=out/(name+'.exe');src.write_text(common+body+tests,encoding='utf-8')
    p=subprocess.run(['gcc','-std=c11',str(src),'-o',str(exe)],capture_output=True,text=True)
    assert p.returncode==0,p.stderr
    p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
    assert (p.returncode!=0)==negative,(name,p.stderr)
    print('PASS: sensitive logging mutant rejected' if negative else p.stdout.strip())
run('privacy',production)
run('leaking_text',production.replace('/* Keep event/length diagnostics', 'log_capture("%s", text ? text : "");\n    /* Keep event/length diagnostics'),True)
run('leaking_secret',production.replace('const size_t secret_length =', 'log_capture("%s", config->device_secret);\n    const size_t secret_length ='),True)
