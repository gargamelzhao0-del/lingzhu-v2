"""Execute production LED policy, PWM curve and battery listener with host stubs."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
read = lambda p: (root / p).read_text(encoding='utf-8-sig')


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


policy = re.sub(r'^#include.*$', '', read('main/board/led_status.c'), flags=re.M)
app = read('main/nuwax_ai_partner_main.c')
driver = read('main/board/led_driver.c')
init = function(driver, 'esp_err_t nuwax_led_init(')
assert init.index('set_brightness(0)') < init.index('xTaskCreateWithCaps')
program = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdatomic.h>
#include "board/board_drivers.h"
#include "board/led_status.h"
#include "board/battery_service.h"
typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portMAX_DELAY 0
static int *xSemaphoreCreateMutexStatic(int *s) { *s=1; return s; }
static void xSemaphoreTake(int *s, int t) { (void)t; assert(*s==1); *s=0; }
static void xSemaphoreGive(int *s) { assert(*s==0); *s=1; }
static int init_error, apply_error, applications;
static unsigned last_period;
esp_err_t nuwax_led_init(void) { return init_error; }
esp_err_t nuwax_led_deinit(void) { return ESP_OK; }
esp_err_t nuwax_led_off(led_color_t c) { (void)c; applications++; return apply_error; }
esp_err_t nuwax_led_set_state(led_color_t c, led_state_t s) {
    (void)c; (void)s; applications++; return apply_error;
}
esp_err_t nuwax_led_start_breath(led_color_t c, uint32_t p) {
    (void)c; applications++; last_period=p; return apply_error;
}
''' + policy + r'''
static unsigned brightness;
static unsigned pwm_duty, idle_level, updates;
static bool pwm_enabled;
static int stop_error, set_error, update_error;
#define LED_MODE 0
#define LED_CHANNEL 0
#define LED_DUTY_MAX 255u
#define LED_PWM_PERIOD_TICKS 256u
static int ledc_stop(int m, int c, unsigned idle) {
    assert(m==LED_MODE && c==LED_CHANNEL && idle<=1);
    if (stop_error) return stop_error;
    pwm_enabled=false; idle_level=idle; brightness=idle?0:255;
    return ESP_OK;
}
static int ledc_set_duty(int m, int c, unsigned duty) {
    assert(m==LED_MODE && c==LED_CHANNEL && duty>0 && duty<256);
    if (set_error) return set_error;
    pwm_duty=duty; return ESP_OK;
}
static int ledc_update_duty(int m, int c) {
    assert(m==LED_MODE && c==LED_CHANNEL);
    if (update_error) return update_error;
    pwm_enabled=true; updates++; brightness=(256-pwm_duty)*255/256;
    return ESP_OK;
}
''' + function(driver, 'static esp_err_t set_brightness(') + r'''
#define LED_DEFAULT_BREATH_MS 2000u
''' + function(driver, 'static esp_err_t apply_state(') + r'''
typedef int nuwax_power_state_t;
enum {NUWAX_POWER_UNKNOWN, NUWAX_POWER_EXTERNAL, NUWAX_POWER_BATTERY,
      NUWAX_POWER_CHARGING, NUWAX_POWER_LOW};
static bool ui_ready;
static int ui_updates, ui_power;
static bool ui_ready_snapshot(void) { return ui_ready; }
static void nuwax_app_shell_set_battery_state(bool v, bool t, int p,
                                             int soc, int mv, int ma) {
    (void)v; (void)t; (void)soc; (void)mv; (void)ma; ui_updates++; ui_power=p;
}
static void nuwax_state_bridge_refresh_power(void) {}
#define ESP_LOGW(...) ((void)0)
''' + function(app, 'static nuwax_power_state_t ui_power_state_from_battery(') + function(
    app, 'static void battery_service_listener(') + r'''
int main(void) {
    /* Inspect electrical output, not just the requested logical brightness. */
    assert(set_brightness(0)==ESP_OK && !pwm_enabled && idle_level==1);
    for (unsigned b=1;b<255;b++) {
        assert(set_brightness(b)==ESP_OK && pwm_enabled);
        assert(256-pwm_duty==b*256/255);
    }
    assert(set_brightness(255)==ESP_OK && !pwm_enabled && idle_level==0);
    assert(set_brightness(128)==ESP_OK && pwm_enabled);
    assert(set_brightness(0)==ESP_OK && !pwm_enabled && idle_level==1);
    unsigned old_updates=updates;
    for (unsigned i=0;i<100;i++) assert(set_brightness(0)==ESP_OK);
    assert(!pwm_enabled && idle_level==1 && updates==old_updates);
    assert(set_brightness(128)==ESP_OK && pwm_enabled);
    stop_error=ESP_FAIL;
    assert(set_brightness(0)==ESP_FAIL && pwm_enabled);
    stop_error=0;
    assert(set_brightness(0)==ESP_OK && !pwm_enabled);
    set_error=ESP_FAIL;
    assert(set_brightness(128)==ESP_FAIL && !pwm_enabled);
    set_error=0; update_error=ESP_FAIL;
    assert(set_brightness(128)==ESP_FAIL && !pwm_enabled);
    update_error=0;
    assert(set_brightness(128)==ESP_OK && pwm_enabled);
    assert(nuwax_led_status_set_charging(true)==ESP_ERR_INVALID_STATE);
    nuwax_battery_service_snapshot_t s={.valid=true, .power_valid=true,
        .charging=true, .power_state=NUWAX_BATTERY_POWER_CHARGING};
    init_error=ESP_ERR_NO_MEM;
    battery_service_listener(&s, NULL);
    assert(nuwax_led_status_get()==NUWAX_LED_STATUS_OFF);
    init_error=0;
    battery_service_listener(&s, NULL); /* Retry succeeds even before UI ready. */
    assert(nuwax_led_status_get()==NUWAX_LED_STATUS_CHARGING && last_period==1200);
    assert(ui_updates==0);
    int before=applications;
    for (int i=0;i<100;i++) battery_service_listener(&s, NULL);
    assert(applications==before); /* No PWM restarts on repeated telemetry. */
    ui_ready=true;
    battery_service_listener(&s, NULL);
    assert(ui_power==NUWAX_POWER_CHARGING);
    s.stale=true;
    battery_service_listener(&s,NULL);
    assert(nuwax_led_status_get()==NUWAX_LED_STATUS_OFF && ui_power==NUWAX_POWER_UNKNOWN);
    s.stale=false;
    for (int reason=0;reason<4;reason++) {
        battery_service_listener(&s,NULL);
        assert(nuwax_led_status_get()==NUWAX_LED_STATUS_CHARGING);
        nuwax_battery_service_snapshot_t bad=s;
        if (reason==0) bad.valid=false;
        if (reason==1) bad.power_stale=true;
        if (reason==2) bad.full=true;
        if (reason==3) bad.charging=false;
        battery_service_listener(&bad,NULL);
        assert(nuwax_led_status_get()==NUWAX_LED_STATUS_OFF);
    }
    assert(nuwax_led_status_set_sleeping(true)==ESP_OK);
    assert(nuwax_led_status_set_charging(true)==ESP_OK);
    assert(nuwax_led_status_get()==NUWAX_LED_STATUS_CHARGING);
    assert(nuwax_led_status_set_audio_rx(true)==ESP_OK);
    assert(nuwax_led_status_get()==NUWAX_LED_STATUS_AUDIO_RX);
    assert(nuwax_led_status_set_charging(false)==ESP_OK);
    assert(nuwax_led_status_get()==NUWAX_LED_STATUS_AUDIO_RX);
    assert(nuwax_led_status_set_audio_rx(false)==ESP_OK);
    assert(nuwax_led_status_get()==NUWAX_LED_STATUS_SLEEPING && last_period==2000);
    for (int mode=NUWAX_LED_STATUS_ERROR;mode<=NUWAX_LED_STATUS_PROVISIONING;mode++) {
        assert(nuwax_led_status_set(mode)==ESP_OK);
        assert(nuwax_led_status_set_charging(true)==ESP_OK);
        assert(nuwax_led_status_set_audio_rx(true)==ESP_OK);
        assert(nuwax_led_status_get()==(nuwax_led_status_t)mode);
        assert(nuwax_led_status_set(NUWAX_LED_STATUS_OFF)==ESP_OK);
        assert(nuwax_led_status_get()==NUWAX_LED_STATUS_CHARGING);
    }
    apply_error=ESP_FAIL;
    assert(nuwax_led_status_set_charging(false)==ESP_FAIL);
    assert(nuwax_led_status_get()==NUWAX_LED_STATUS_CHARGING);
    apply_error=ESP_OK;
    assert(nuwax_led_status_set_charging(false)==ESP_OK);
    assert(nuwax_led_status_get()==NUWAX_LED_STATUS_OFF);
    assert(nuwax_led_status_set_charging(true)==ESP_OK);
    assert(nuwax_led_status_deinit()==ESP_OK);
    assert(nuwax_led_status_init(NULL)==ESP_OK);
    assert(nuwax_led_status_get()==NUWAX_LED_STATUS_OFF);
    /* Exhaustive full periods: monotonic rise/fall, no peak wrap to zero. */
    for (unsigned period=200;period<=10000;period+=2) {
        unsigned prev=0;
        for (unsigned t=0;t<=period;t++) {
            assert(apply_state(LED_STATE_BREATH,period,t)==ESP_OK);
            if(t<=period/2) assert(brightness>=prev);
            else assert(brightness<=prev);
            if(t==period/2) assert(brightness==255);
            prev=brightness;
        }
        assert(brightness==0);
    }
    puts("PASS: charging LED binding, fault retry, freshness, priority restoration, PWM peak, static high OFF and PWM resume");
}
'''
with tempfile.TemporaryDirectory(prefix='nuwax-charging-led-') as folder:
    p = Path(folder)
    (p / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n' + '\n'.join(
        f'#define {name} {i}' for i, name in enumerate([
            'ESP_OK', 'ESP_FAIL', 'ESP_ERR_INVALID_STATE', 'ESP_ERR_INVALID_ARG',
            'ESP_ERR_NO_MEM'])))
    (p / 'test.c').write_text(program, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    f'-I{p}', f'-I{root / "main"}', str(p / 'test.c'),
                    '-o', str(p / 'test.exe')], check=True)
    subprocess.run([str(p / 'test.exe')], check=True)
