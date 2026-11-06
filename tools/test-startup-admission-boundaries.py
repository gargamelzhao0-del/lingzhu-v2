"""Execute production admission/configuration/lock accounting with fault stubs."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser()
parser.add_argument('--gcc', default='gcc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
read = lambda p: (root / p).read_text(encoding='utf-8-sig')
ota = read('main/nuwax_ota_new/nuwax_ota_device.c')
manifest = read('main/nuwax_ota_new/nuwax_ota_manifest.c')
wifi = read('main/wifi_manager.c')
bus = read('main/board/i2c_bus_manager.c')
app = read('main/nuwax_ai_partner_main.c')

common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <ctype.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_TIMEOUT 2
#define ESP_RETURN_ON_ERROR(expr, ...) do { int e=(expr); if(e) return e; } while(0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l=strlen(s); if(n) { size_t c=l<n-1?l:n-1; memcpy(d,s,c); d[c]=0; } return l;
}
'''

ota_code = common + r'''
typedef struct { char version[64]; } esp_app_desc_t;
typedef struct { char version[64]; } nuwax_ota_manifest_t;
static esp_app_desc_t running={"7.0.98"};
static bool descriptor_available=true;
static const esp_app_desc_t *esp_app_get_description(void) {
    return descriptor_available ? &running : NULL;
}
'''
ota_code += manifest[manifest.index('static bool numeric_core(const char *version)\n{'):manifest.index('esp_err_t nuwax_ota_sha256_init(')]
ota_code += function(ota, 'static bool ota_version_is_newer(')
ota_code += r'''
int main(void) {
    nuwax_ota_manifest_t m={"7.0.98"}; assert(!ota_version_is_newer(&m));
    strcpy(m.version,"7.0.97"); assert(!ota_version_is_newer(&m));
    strcpy(m.version,"7.0.98+different-image"); assert(!ota_version_is_newer(&m));
    strcpy(m.version,"7.0.98-rc.1"); assert(!ota_version_is_newer(&m));
    strcpy(m.version,"7.0.99"); assert(ota_version_is_newer(&m));
    strcpy(m.version,"7.0.100"); assert(ota_version_is_newer(&m));
    descriptor_available=false; assert(!ota_version_is_newer(&m));
    puts("PASS: OTA same/older/newer/prerelease/missing descriptor admission");
}
'''

wifi_code = common + r'''
typedef struct { struct { unsigned char ssid[32], password[64];
    int scan_method,sort_method,sae_pwe_h2e,disable_wpa3_compatible_mode;
    struct { int authmode; } threshold; } sta; } wifi_config_t;
#define CONFIG_ESP_WIFI_ENABLE_WPA3_SAE 1
#define CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT 1
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_IDF_VERSION 60000
#define WIFI_FAST_SCAN 1
#define WIFI_CONNECT_AP_BY_SIGNAL 2
#define WPA3_SAE_PWE_BOTH 3
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 2
#define WIFI_IF_STA 0
#define WIFI_NVS_FLASH_GUARD_TIMEOUT_MS 500
static wifi_config_t driver, s_last_requested_wifi_config, s_last_applied_wifi_config;
static bool s_last_wifi_config_valid, connected, provisioning, locked;
static atomic_bool s_wifi_connect_requested, s_wifi_simulated_offline;
static int get_error, set_error, guard_error, lock_error, writes, guards, ends;
static int wifi_init_once(void) { return ESP_OK; }
static int wifi_operation_lock(void) { if(lock_error) return lock_error; assert(!locked); locked=true; return 0; }
static void wifi_operation_unlock(void) { assert(locked); locked=false; }
static bool nuwax_wifi_is_connected(void) { return connected; }
static bool nuwax_wifi_provisioning_active(void) { return provisioning; }
static int esp_wifi_get_config(int i, wifi_config_t *c) { (void)i; *c=driver; return get_error; }
static int esp_wifi_set_config(int i, wifi_config_t *c) {
    (void)i; assert(guards>ends); ++writes; if(set_error) return set_error; driver=*c;
    if(strlen((const char*)driver.sta.password)>=8) driver.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    return 0;
}
static int nuwax_wake_word_detector_begin_flash_operation(int t) {
    (void)t; if(guard_error) return guard_error; ++guards; return 0;
}
static void nuwax_wake_word_detector_end_flash_operation(void) { ++ends; }
'''
for signature in ['static esp_err_t wifi_set_config_guarded(', 'static void wifi_make_station_config(', 'esp_err_t nuwax_wifi_prepare_boot_config(']:
    wifi_code += function(wifi, signature)
wifi_code += r'''
int main(void) {
    strcpy((char*)driver.sta.ssid,"saved"); strcpy((char*)driver.sta.password,"12345678");
    assert(nuwax_wifi_prepare_boot_config()==0); assert(writes==1 && guards==ends && !locked);
    wifi_config_t expected; wifi_make_station_config(&expected,"saved","12345678");
    assert(wifi_set_config_guarded(&expected)==0); assert(writes==1 && guards==1);
    assert(nuwax_wifi_prepare_boot_config()==0); assert(writes==1);
    driver.sta.sort_method=99; connected=true;
    assert(nuwax_wifi_prepare_boot_config()==0 && writes==1); connected=false;
    provisioning=true; assert(nuwax_wifi_prepare_boot_config()==0 && writes==1); provisioning=false;
    atomic_store(&s_wifi_connect_requested,true);
    assert(nuwax_wifi_prepare_boot_config()==0 && writes==1); atomic_store(&s_wifi_connect_requested,false);
    atomic_store(&s_wifi_simulated_offline,true);
    assert(nuwax_wifi_prepare_boot_config()==0 && writes==1); atomic_store(&s_wifi_simulated_offline,false);
    guard_error=ESP_ERR_TIMEOUT; assert(nuwax_wifi_prepare_boot_config()==ESP_ERR_TIMEOUT && !locked && writes==1);
    guard_error=0; set_error=ESP_FAIL; assert(nuwax_wifi_prepare_boot_config()==ESP_FAIL && !locked && guards==ends);
    set_error=0; assert(nuwax_wifi_prepare_boot_config()==0 && guards==ends);
    get_error=ESP_FAIL; assert(nuwax_wifi_prepare_boot_config()==ESP_FAIL && !locked); get_error=0;
    lock_error=ESP_ERR_TIMEOUT; assert(nuwax_wifi_prepare_boot_config()==ESP_ERR_TIMEOUT && !locked); lock_error=0;
    driver.sta.ssid[0]=0; int before=writes;
    assert(nuwax_wifi_prepare_boot_config()==0 && writes==before);
    puts("PASS: STA normalization/reuse, live-link/BLE/offline guards, write/lock failures");
}
'''

header = read('main/board/i2c_bus_manager.h')
struct = header[header.index('typedef enum {'):header.index('void nuwax_i2c_bus_get_stats')]
bus_code = common + struct + r'''
typedef unsigned TickType_t;
#define pdTRUE 1
#define taskENTER_CRITICAL(x) ((void)0)
#define taskEXIT_CRITICAL(x) ((void)0)
static nuwax_i2c_bus_stats_t s_stats;
static int s_transaction_lock=1;
static int64_t s_transaction_started_us, clock_us;
static int64_t s_sample_driver_done_us;
static esp_err_t s_sample_result;
static nuwax_i2c_client_t s_transaction_client;
static int64_t s_driver_done_us;
static esp_err_t s_driver_result;
static unsigned taken_timeout;
static bool take_ok=true, held;
static const char *task_name="battery";
static int nuwax_i2c_bus_init(void) { return 0; }
static int ensure_transaction_lock(void) { return 0; }
static unsigned delay_ticks_ceil(unsigned n) { return n; }
static int64_t esp_timer_get_time(void) { return clock_us++; }
static const char *pcTaskGetName(void *p) { (void)p; return task_name; }
static int xSemaphoreTake(int s,unsigned t) { (void)s;taken_timeout=t; clock_us+=40; held=take_ok; return take_ok; }
static void xSemaphoreGive(int s) { (void)s; assert(held); held=false; }
'''
for signature in ['esp_err_t nuwax_i2c_bus_transaction_begin(', 'void nuwax_i2c_bus_transfer_done(', 'void nuwax_i2c_bus_sample_transfer_done(', 'void nuwax_i2c_bus_transaction_end(', 'void nuwax_i2c_bus_get_stats(']:
    bus_code += function(bus, signature)
bus_code += r'''
int main(void) {
    assert(nuwax_i2c_bus_transaction_begin(10)==0); clock_us+=160000;
    nuwax_i2c_bus_transaction_end(); assert(!held && s_stats.long_holds==1);
    assert(strcmp(s_stats.max_hold_task,"battery")==0);
    task_name="touch"; take_ok=false;
    assert(nuwax_i2c_bus_transaction_begin(10)==ESP_ERR_TIMEOUT);
    assert(s_stats.lock_timeouts==1 && s_stats.transactions==1 && !held);
    assert(strcmp(s_stats.last_timeout_task,"touch")==0);
    take_ok=true; assert(nuwax_i2c_bus_transaction_begin(10)==0);
    nuwax_i2c_bus_transaction_end(); assert(s_stats.long_holds==1);
    assert(strcmp(s_stats.max_hold_task,"battery")==0);
    nuwax_i2c_bus_stats_t copy; nuwax_i2c_bus_get_stats(&copy);
    assert(copy.transactions==2 && copy.total_hold_us>=160000);
    assert(copy.clients[NUWAX_I2C_CLIENT_OTHER].calls==2);
    assert(copy.clients[NUWAX_I2C_CLIENT_OTHER].long_holds==1);
    take_ok=false;
    assert(nuwax_i2c_bus_transaction_begin(0)==ESP_ERR_TIMEOUT && taken_timeout==0);
    assert(s_stats.try_skips==1 && s_stats.lock_timeouts==1);
    take_ok=true;assert(nuwax_i2c_bus_transaction_begin(0)==ESP_OK);
    clock_us+=2300;nuwax_i2c_bus_sample_transfer_done(ESP_ERR_TIMEOUT);
    clock_us+=450;nuwax_i2c_bus_transaction_end();
    nuwax_i2c_bus_get_stats(&copy);
    assert(copy.sample_calls==1 && copy.sample_errors==1);
    assert(copy.sample_driver_max_us>=2300 && copy.sample_release_max_us>=450);
    assert(copy.sample_window_driver_max_us==copy.sample_driver_max_us);
    nuwax_i2c_bus_get_stats(&copy);
    assert(copy.sample_window_driver_max_us==0 && copy.sample_window_release_max_us==0);
    assert(copy.sample_driver_max_us>=2300 && !held);
    assert(copy.clients[NUWAX_I2C_CLIENT_QMI].calls==0);
    assert(nuwax_i2c_bus_transaction_begin(50)==ESP_OK);
    clock_us+=2000;nuwax_i2c_bus_transfer_done(NUWAX_I2C_CLIENT_TOUCH,ESP_OK);
    clock_us+=15000;nuwax_i2c_bus_transaction_end();
    assert(nuwax_i2c_bus_transaction_begin(50)==ESP_OK);
    clock_us+=12000;nuwax_i2c_bus_transfer_done(NUWAX_I2C_CLIENT_BATTERY,ESP_ERR_TIMEOUT);
    nuwax_i2c_bus_transaction_end();
    nuwax_i2c_bus_get_stats(&copy);
    assert(copy.clients[NUWAX_I2C_CLIENT_TOUCH].calls==1);
    assert(copy.clients[NUWAX_I2C_CLIENT_TOUCH].long_holds==1);
    assert(copy.clients[NUWAX_I2C_CLIENT_TOUCH].driver_max_us>=2000);
    assert(copy.clients[NUWAX_I2C_CLIENT_TOUCH].release_max_us>=15000);
    assert(copy.clients[NUWAX_I2C_CLIENT_BATTERY].errors==1);
    assert(copy.clients[NUWAX_I2C_CLIENT_BATTERY].long_holds==1);
    assert(copy.clients[NUWAX_I2C_CLIENT_OTHER].calls==0);
    nuwax_i2c_bus_get_stats(&copy);
    for(unsigned i=0;i<NUWAX_I2C_CLIENT_COUNT;i++)assert(copy.clients[i].calls==0);
    assert(copy.long_holds==3 && !held);
    puts("PASS: I2C long-holder attribution, timeout accounting and lock release");
}
'''

# Check integration around the executed helpers as well as boot ordering.
receive = function(ota, 'bool nuwax_ota_device_handle_text(')
accept = function(ota, 'static bool accept_queued_update(')
process = function(ota, 'static __attribute__((noinline)) void process_update(')
assert receive.index('if (duplicate)') < receive.index('ota_version_is_newer') < receive.index('allocate_command_item()')
assert accept.index('ota_version_is_newer') < accept.index('persist_report_context')
assert process.index('accept_queued_update') < process.index('show_local_progress') < process.index('prepare_audio_for_update')
boot = app[app.index('void app_main('):]
assert boot.count('create_app_task(nuwax_speaker_playback_task,') == 1
assert boot.count('refresh_saved_wifi_list_ui();') == 1
assert boot.index('refresh_saved_wifi_list_ui();') < boot.index('init_wake_model_serialized();')
assert boot.index('nuwax_wifi_prepare_boot_config();') < boot.index('init_wake_model_serialized();')
assert boot.index('init_wake_model_serialized();') < boot.index('prepare_for_tts("boot_preallocate")') < boot.index('request_sleep_wake_word_detector("boot_local_wake")')
assert boot.index('init_wake_model_serialized();') < boot.index('conversation_live_aec_pool_init()') < boot.index('conversation_live_aec_pool_suspend("boot_preflight_complete")') < boot.index('prepare_for_tts("boot_preallocate")')
assert 'nuwax_wifi_connect();' not in boot and 'nuwax_wifi_recover();' not in boot

with tempfile.TemporaryDirectory(prefix='nuwax-startup-check-') as tmp:
    for name, code in [('ota',ota_code),('wifi',wifi_code),('i2c',bus_code)]:
        src=Path(tmp)/f'{name}.c'; exe=Path(tmp)/f'{name}.exe'
        src.write_text(code,encoding='utf-8')
        subprocess.run([args.gcc,'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
print('Startup admission production-boundary checks: PASS')
