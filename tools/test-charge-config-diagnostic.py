"""Execute production diagnostic/profile bodies with read failures injected."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/board/bq27220_driver.c').read_text(encoding='utf-8-sig')

def body(signature):
    start = source.index(signature)
    cursor = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor]

stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_RESPONSE 3
#define BQ27220_REG_TEMPERATURE 6
#define BQ27220_REG_OPERATION_STATUS 0x3a
#define BQ27220_DM_U16 2
typedef struct { uint16_t address, value; int width; } bq27220_profile_parameter_t;
static const bq27220_profile_parameter_t s_vendor_profile[27];
static uint16_t config_a;
static int reads, fail_at, seals, unseals, writes, profile_reads;
static bool initially_matches;
static char logs[4096];
static void log_line(const char *fmt, ...) {
    size_t n = strlen(logs);
    va_list ap; va_start(ap, fmt);
    vsnprintf(logs+n, sizeof(logs)-n, fmt, ap); va_end(ap);
}
#define ESP_LOGI(tag,...) log_line(__VA_ARGS__)
#define ESP_LOGW(tag,...) log_line(__VA_ARGS__)
static const char *esp_err_to_name(int e) { (void)e; return "injected"; }
static int read_data_memory_value(const bq27220_profile_parameter_t *p, uint16_t *v) {
    const uint16_t addresses[] = {0x9206,0x9208,0x91f5,0x91f7,0x91f9,0x91fb,0x91fd,0x9201};
    const uint16_t values[] = {config_a,0x1000,(uint16_t)-50,450,50,200,4200,100};
    assert(reads < 8 && p->address == addresses[reads] && p->width == 2);
    int i = reads++;
    if (i == fail_at) return 9;
    *v = values[i]; return 0;
}
static int read_word(uint8_t reg, uint16_t *v) {
    if (reg == 0x3a) { *v = 0xb6; return 0; }
    assert((reads == 8 && reg == 6) || (reads == 9 && reg == 0x28));
    int i = reads++;
    if (i == fail_at) return 9;
    *v = 3232; return 0;
}
static int restore_normal_sealed_state(bool *ok) { seals++; *ok = true; return 0; }
static int security_state_from_operation(uint16_t op) { (void)op; return 3; }
static int temporarily_unseal(int state, bool *restore) { assert(state==3); unseals++; *restore=true; return 0; }
static int read_vendor_profile(bool *matches) { profile_reads++; *matches=initially_matches || writes>0; return 0; }
static int write_vendor_profile(void) { writes++; return 0; }
'''
checks = r'''
static void reset(void) {
    reads=seals=unseals=writes=profile_reads=0; fail_at=-1;
    config_a=0x0484; initially_matches=true; logs[0]=0;
}
int main(void) {
    bool verified, restored;
    for (int failure=-1; failure<10; failure++) {
        reset(); fail_at=failure;
        assert(verify_vendor_cedv_profile(&verified,&restored)==0);
        assert(verified && restored && seals==2 && unseals==1 && writes==0);
        assert(reads==(failure<0?10:failure+1));
        if (failure>=0) assert(strstr(logs,"diagnostic only"));
        if (failure<0) {
            assert(strstr(logs,"temp_source=internal"));
            assert(strstr(logs,"inhibit_low_dC=-50 inhibit_high_dC=450 hysteresis_dC=50"));
            assert(strstr(logs,"temperature_dK=3232 internal_dK=3232"));
        }
    }
    reset(); config_a=0xb08c;
    assert(verify_vendor_cedv_profile(&verified,&restored)==0);
    assert(strstr(logs,"temp_source=external TEMPS=1 WRTEMP=0 BATG_EN=1 BATG_POL=1 PFC=1"));
    reset(); config_a=0x8584;
    assert(verify_vendor_cedv_profile(&verified,&restored)==0);
    assert(strstr(logs,"temp_source=host TEMPS=1 WRTEMP=1"));
    reset(); initially_matches=false;
    assert(verify_vendor_cedv_profile(&verified,&restored)==0);
    assert(verified && restored && writes==1 && seals==3 && unseals==2 && reads==10);
    puts("PASS: production charge diagnostics, signed thresholds, temperature source, all 10 read failures, sealed cleanup and profile repair unchanged");
}
'''
with tempfile.TemporaryDirectory(prefix='charge-config-') as folder:
    code = Path(folder) / 'test.c'
    exe = Path(folder) / 'test.exe'
    code.write_text(stub + body('static void log_charge_configuration(') +
                    body('static esp_err_t verify_vendor_cedv_profile(') + checks, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(code), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
