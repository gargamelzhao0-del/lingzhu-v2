"""Execute production provisioning reset and PG2 restart bodies with faults."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/network-recovery-boundaries'
out.mkdir(parents=True, exist_ok=True)


def body(path, signature):
    source = (root / path).read_text(encoding='utf-8-sig')
    start = source.index(signature)
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
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_WIFI_NOT_CONNECT 3
#define ESP_ERR_WIFI_STATE 4
#define TAG "test"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
'''

manager = common + r'''
#define WIFI_STORAGE_RAM 1
#define WIFI_STORAGE_FLASH 2
#define WIFI_IF_STA 0
#define WIFI_PROV_STORAGE_BIT 1
#define NETWORK_PROV_STATE_STARTED 1
#define NETWORK_PROV_START 1
typedef struct {int unused;} wifi_config_t;
static struct {
    struct {struct {bool no_auto_stop;}capabilities;}mgr_info;
    int prov_state;
    unsigned connection_attempts_completed;
} ctx, *prov_ctx=&ctx;
static int prov_ctx_lock=1, locked, stage, storage;
static bool busy;
static int disconnect_error,config_error,storage_error;
static unsigned starts;
#define ACQUIRE_LOCK(x) do {assert(x && !locked);locked=1;}while(0)
#define RELEASE_LOCK(x) do {assert(x && locked);locked=0;}while(0)
static int esp_wifi_set_storage(int value){assert(locked);storage=value;return value==WIFI_STORAGE_FLASH?storage_error:0;}
static int esp_wifi_disconnect(void){assert(locked);stage=1;if(disconnect_error)return disconnect_error;busy=false;return 0;}
static int esp_wifi_set_config(int iface,wifi_config_t *c){
    assert(locked && iface==WIFI_IF_STA && !c->unused && storage==WIFI_STORAGE_RAM);
    assert(stage==1 && !busy && prov_ctx->prov_state==NETWORK_PROV_STATE_STARTED);
    stage=2;return config_error;
}
static void execute_event_cb(int e,void *data,unsigned size){assert(e==NETWORK_PROV_START && !data && !size && stage==2);starts++;}
''' + body('managed_components/espressif__network_provisioning/src/manager.c',
           'esp_err_t network_prov_mgr_reset_wifi_sm_state_for_reprovision(void)') + r'''
int main(void){
 for(unsigned scenario=0;scenario<4;scenario++){
  ctx.mgr_info.capabilities.no_auto_stop=true;ctx.prov_state=3;ctx.connection_attempts_completed=7;
  busy=true;stage=0;starts=0;storage=WIFI_STORAGE_FLASH;
  disconnect_error=scenario==1?ESP_FAIL:0;
  config_error=scenario==2?ESP_ERR_WIFI_STATE:0;
  storage_error=scenario==3?ESP_FAIL:0;
  int result=network_prov_mgr_reset_wifi_sm_state_for_reprovision();
  assert(!locked && storage==WIFI_STORAGE_FLASH);
  if(scenario==0)assert(!result && starts==1 && !busy && !ctx.connection_attempts_completed);
  else assert(result!=0);
  if(scenario==1)assert(stage==1 && starts==0 && ctx.prov_state==3);
  if(scenario==2)assert(stage==2 && starts==0);
 }
 puts("PASS: active authentication is stopped before config reset; driver/config/storage faults propagate; lock and storage restored");
}
'''

reset = common + r'''
typedef enum {PROVISION_WIFI_RESET_FAILED_ATTEMPT,PROVISION_WIFI_RESET_ABORT_ATTEMPT} provision_wifi_reset_kind_t;
static int failure_result,abort_result;
static unsigned failures,aborts;
static int network_prov_mgr_reset_wifi_sm_state_on_failure(void){failures++;return failure_result;}
static int network_prov_mgr_reset_wifi_sm_state_for_reprovision(void){aborts++;return abort_result;}
''' + body('main/nuwax_provision_new/nuwax_provision_main.c',
           'static esp_err_t reset_manager_wifi_state(') + r'''
int main(void){
 abort_result=ESP_ERR_WIFI_STATE;failure_result=ESP_OK;
 assert(reset_manager_wifi_state(PROVISION_WIFI_RESET_ABORT_ATTEMPT,"cancel")==ESP_ERR_WIFI_STATE);
 assert(aborts==1 && failures==0);
 failure_result=ESP_FAIL;abort_result=ESP_OK;
 assert(reset_manager_wifi_state(PROVISION_WIFI_RESET_FAILED_ATTEMPT,"failure")==ESP_FAIL && aborts==1);
 failure_result=ESP_ERR_INVALID_STATE;
 assert(reset_manager_wifi_state(PROVISION_WIFI_RESET_FAILED_ATTEMPT,"failure")==ESP_OK && aborts==2);
 puts("PASS: partial abort cannot be falsely acknowledged by alternate STARTED reset; only failed-state mismatch falls back");
}
'''

power = common + r'''
typedef int gpio_num_t;
typedef struct {uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;}gpio_config_t;
#define CONFIG_NUWAX_POWER_CTRL_GPIO 7
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
static bool shutting_down,held;
static int level,error;
static unsigned hold_calls;
static bool nuwax_power_ctrl_shutdown_preparing(void){return shutting_down;}
static int power_hold_level(void){return 1;}
static int gpio_set_level(int pin,int value){assert(pin==7 && value==1);if(error)return error;level=value;return 0;}
static int gpio_hold_en(int pin){assert(pin==7 && level==1);held=true;hold_calls++;return 0;}
static int gpio_config(const gpio_config_t *c){assert(c->pin_bit_mask==(1ULL<<7) && level==1);return 0;}
static void gpio_deep_sleep_hold_dis(void){}
static int gpio_hold_dis(int pin){assert(pin==7 && level==1);held=false;return 0;}
''' + body('main/board/nuwax_power_ctrl.c','static esp_err_t configure_power_hold_output(') + '\n' + body(
    'main/board/nuwax_power_ctrl.c','static void preserve_power_on_restart(void)') + r'''
int main(void){
 preserve_power_on_restart();assert(held && hold_calls==1 && level==1);
 assert(configure_power_hold_output(7,1)==ESP_OK && !held && level==1);
 held=true;error=ESP_FAIL;
 assert(configure_power_hold_output(7,1)==ESP_FAIL && held);
 held=false;error=ESP_OK;
 shutting_down=true;preserve_power_on_restart();assert(hold_calls==1 && !held);
 shutting_down=false;error=ESP_FAIL;preserve_power_on_restart();assert(hold_calls==1 && !held);
 puts("PASS: restart latches PG2 HIGH; boot preloads HIGH before release; shutdown and failed level drive do not latch");
}
'''

for name, code in [('manager', manager), ('reset', reset), ('power', power)]:
    src, exe = out / (name + '.c'), out / (name + '.exe')
    src.write_text(code, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# Confirm the checks reject a missing disconnect, swallowed cleanup error and
# missing power hold, rather than merely accepting the current implementation.
mutants = [
    ('manager_without_disconnect', manager.replace('ret = esp_wifi_disconnect();',
        'ret = ((void)esp_wifi_disconnect, ESP_OK);')),
    ('reset_swallow_error', reset.replace('if (!failed_attempt || reset_err != ESP_ERR_INVALID_STATE)',
        'if (false)')),
    ('restart_without_hold', power.replace('gpio_hold_en(pin) : level_err',
        '((void)gpio_hold_en, ESP_OK) : level_err')),
]
for name, code in mutants:
    src, exe = out / (name + '.c'), out / (name + '.exe')
    src.write_text(code, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(src), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True)
    assert result.returncode != 0, f'Regression escaped: {name}'
    print('PASS: negative mutant rejected ' + name)
