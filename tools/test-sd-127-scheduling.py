"""Production maintenance policy, pressure notification and ping wire checks."""
from pathlib import Path
import subprocess, re
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks';out.mkdir(exist_ok=True)
session=(root/'main/session_protocol.c').read_text(encoding='utf-8')
ping=re.search(r'(?ms)^esp_err_t nuwax_session_send_ping_until_if_epoch\(.*?^}',session).group()
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "display_maintenance_policy.h"
typedef unsigned TickType_t;
typedef unsigned StaticEventGroup_t;
typedef unsigned *EventGroupHandle_t;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define pdMS_TO_TICKS(x) ((x)/10U)
#define pdTRUE 1
#define pdFALSE 0
static unsigned age, scenario, delays, waits, elapsed;
static uint32_t nuwax_wake_word_capture_backlog_ms(void){return age;}
static EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *p){*p=0;return p;}
static void vTaskDelay(unsigned ticks){assert(ticks==1);delays++;elapsed+=10;}
static void wake_capture_signal_drained(void);
static void xEventGroupClearBits(EventGroupHandle_t p,unsigned bits){
    *p &= ~bits;
    if(scenario==1){age=30;wake_capture_signal_drained();}
}
static unsigned xEventGroupSetBits(EventGroupHandle_t p,unsigned bits){return *p |= bits;}
static unsigned xEventGroupWaitBits(EventGroupHandle_t p,unsigned bits,int clear,int all,unsigned ticks){
    assert(clear && !all && ticks==1);waits++;
    if(scenario==2){age=30;wake_capture_signal_drained();elapsed+=2;}
    if(!(*p&bits))elapsed+=10;
    unsigned got=*p;*p&=~bits;return got;
}
#include "wake_capture_pressure_signal.inc"
typedef int esp_err_t;
typedef int nuwax_ws_client_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_ERR_INVALID_SIZE -2
static char captured[64];
static int nuwax_ws_send_keepalive_until_if_epoch(nuwax_ws_client_t *w,const char *p,int64_t d,uint32_t e){
    assert(w && d==123 && e==7);strcpy(captured,p);return 0;
}
''' + ping + r'''
int main(void){
    nuwax_display_maintenance_t m={0};uint16_t first,last;
    assert(!nuwax_display_maintenance_next(&m,1,0,false,360,&first,&last));
    assert(!nuwax_display_maintenance_next(&m,1,119999999,false,360,&first,&last));
    assert(!nuwax_display_maintenance_next(&m,1,120000000,true,360,&first,&last));
    unsigned covered=0, calls=0;
    for(int64_t now=120000000;covered<360;now+=50000){
        assert(nuwax_display_maintenance_next(&m,1,now,false,360,&first,&last));
        assert(first==covered && last-first<8);covered=last+1;calls++;
        assert(!nuwax_display_maintenance_next(&m,1,now+1,false,360,&first,&last));
    }
    assert(calls==45 && !m.active);
    assert(!nuwax_display_maintenance_next(&m,2,300000000,false,360,&first,&last));
    assert(!nuwax_display_maintenance_next(&m,2,420000000,true,360,&first,&last));
    assert(nuwax_display_maintenance_next(&m,2,420100000,false,360,&first,&last) && first==0);
    assert(!nuwax_display_maintenance_next(&m,3,420200000,false,360,&first,&last) && m.row==0);
    assert(!nuwax_display_maintenance_next(&m,0,420300000,false,360,&first,&last) && !m.initialized);
    /* Odd heights are clipped, no overflow or last-row omission. */
    for(unsigned height=1;height<600;height++){
        m=(nuwax_display_maintenance_t){0};covered=0;
        assert(!nuwax_display_maintenance_next(&m,1,0,false,height,&first,&last));
        for(int64_t now=120000000;covered<height;now+=50000){
            assert(nuwax_display_maintenance_next(&m,1,now,false,height,&first,&last));
            assert(first==covered && last<height && last-first<8);covered=last+1;
        }
    }
    age=90;nuwax_wake_word_wait_capture_headroom(10);assert(delays==1);
    wake_capture_pressure_signal_init();EventGroupHandle_t original=s_pressure_event;
    wake_capture_pressure_signal_init();assert(original==s_pressure_event);
    age=30;nuwax_wake_word_wait_capture_headroom(10);assert(waits==0);
    age=90;scenario=1;nuwax_wake_word_wait_capture_headroom(10);assert(age==30 && waits==0);
    age=90;scenario=2;elapsed=0;nuwax_wake_word_wait_capture_headroom(10);assert(age==30 && elapsed==2 && waits==1);
    /* Timeout/missed notification and stale hints never admit work. */
    age=90;scenario=0;elapsed=0;wake_capture_signal_drained();
    nuwax_wake_word_wait_capture_headroom(10);assert(age==90 && elapsed==10);
    age=0;elapsed=0;nuwax_wake_word_wait_capture_headroom(10);assert(!elapsed);
    nuwax_ws_client_t ws=0;
    int64_t values[]={0,12345,INT64_MIN,INT64_MAX};char expected[64];
    for(unsigned i=0;i<4;i++){
        assert(nuwax_session_send_ping_until_if_epoch(&ws,values[i],123,7)==ESP_OK);
        snprintf(expected,sizeof(expected),"{\"type\":\"ping\",\"now_ms\":%lld}",(long long)values[i]);
        assert(!strcmp(expected,captured));
    }
    assert(nuwax_session_send_ping_until_if_epoch(NULL,0,123,7)==ESP_ERR_INVALID_ARG);
    assert(nuwax_session_send_ping_until_if_epoch(&ws,0,0,7)==ESP_ERR_INVALID_ARG);
    assert(nuwax_session_send_ping_until_if_epoch(&ws,0,123,0)==ESP_ERR_INVALID_ARG);
    puts("PASS: 45 paced screen strips instead of one 129600px invalidation; pause/resume, screen replacement, clipped coverage; drain-before/after-check, timeout/stale notification, static lifetime; exact allocation-free ping fields and integer limits");
}
'''
p=out/'sd_127_scheduling.c';p.write_text(code,encoding='utf-8');exe=p.with_suffix('.exe')
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'main'),str(p),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
