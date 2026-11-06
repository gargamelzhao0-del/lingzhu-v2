"""Production MIC admission: rejected init must preserve live storage."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'main/mic_input.c').read_text(encoding='utf-8-sig')
def function(signature):
    start=source.index(signature); end=source.index('{',start)+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
admission=function('static esp_err_t acquire_owner(')
release=function('static void release_owner(')
entry=source[source.index('esp_err_t nuwax_mic_input_init_with_owner('):]
entry=entry[:entry.index('#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0')]+'return ESP_OK;\n}\n'
common=r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
typedef int nuwax_mic_owner_t;
typedef struct {int owner;bool owner_acquired;void *raw_samples;unsigned token;} nuwax_mic_input_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_INVALID_STATE 3
#define NUWAX_MIC_OWNER_NONE 0
#define NUWAX_MIC_OWNER_UNSPECIFIED 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffU
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr,...) do{int e=(expr);if(e)return e;}while(0)
static int s_active_owner,s_mic_owner_lock=1,fail_lock;
static int ensure_owner_lock(void){return 0;}
static int xSemaphoreTake(int lock,unsigned ticks){(void)lock;(void)ticks;return !fail_lock;}
static void xSemaphoreGive(int lock){(void)lock;}
static void report_mic_warning(const char *s,int e,int o){(void)s;(void)e;(void)o;}
'''
tests=r'''
int main(void){
 nuwax_mic_input_t a={0},b={0},saved;
 assert(nuwax_mic_input_init_with_owner(NULL,2)==ESP_ERR_INVALID_ARG);
 assert(nuwax_mic_input_init_with_owner(&a,2)==ESP_OK);
 a.raw_samples=&a;a.token=123;saved=a;
 assert(nuwax_mic_input_init_with_owner(&a,2)==ESP_ERR_INVALID_STATE);
 assert(!memcmp(&a,&saved,sizeof(a)));
 assert(nuwax_mic_input_init_with_owner(&a,3)==ESP_ERR_INVALID_STATE);
 assert(!memcmp(&a,&saved,sizeof(a)));
 assert(nuwax_mic_input_init_with_owner(&b,3)==ESP_ERR_INVALID_STATE);
 assert(!b.owner_acquired && !b.raw_samples && s_active_owner==2);
 fail_lock=1;
 assert(nuwax_mic_input_init_with_owner(&a,2)==ESP_ERR_TIMEOUT);
 assert(!memcmp(&a,&saved,sizeof(a)));fail_lock=0;
 release_owner(&a);assert(s_active_owner==0);
 assert(nuwax_mic_input_init_with_owner(&b,3)==ESP_OK);
 release_owner(&b);
 assert(nuwax_mic_input_init_with_owner(&a,0)==ESP_OK);
 assert(a.owner==NUWAX_MIC_OWNER_UNSPECIFIED && !a.token && !a.raw_samples);
 release_owner(&a);
 puts("PASS: live duplicate/changed-owner init, competitor, lock timeout, release and reinit");
}
'''
with tempfile.TemporaryDirectory(prefix='mic-init-owner-') as directory:
    path=Path(directory)/'check.c';exe=path.with_suffix('.exe')
    code=common+admission+release+entry+tests
    for mutant in (False, True):
        body=code.replace('esp_err_t owner_err = acquire_owner(mic, owner);',
            'memset(mic,0,sizeof(*mic)); esp_err_t owner_err = acquire_owner(mic, owner);') if mutant else code
        path.write_text(body,encoding='utf-8')
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        if mutant:
            assert result.returncode!=0,'Old destructive init escaped'
            print('PASS: rejected old clear-before-admission defect')
        else:
            assert result.returncode==0,result.stderr
            print(result.stdout.strip())
