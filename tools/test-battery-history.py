"""Exercise the production ring and recorder across console loss and read errors."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/board/battery_service.c').read_text(encoding='utf-8-sig')
start = source.index('static void record_charge_history(')
end = source.index('static uint32_t saturating_double(', start)
recorder = source[start:end]
assert source.count('record_charge_history(&published, &raw,') == 2

program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include "board/battery_history.h"
#include "board/battery_service.h"
#include "board/board_drivers.h"
static nuwax_battery_history_t s_charge_history;
static uint64_t fake_ms;
static unsigned yields;
static bool console_connected;
static char output[20000];
static uint64_t now_ms(void) { return fake_ms; }
static void vTaskDelay(unsigned ticks) { assert(ticks==1); yields++; }
static void capture(const char *format, ...) {
    if (!console_connected) return;
    size_t n=strlen(output);
    assert(n < sizeof(output)-1024);
    va_list ap; va_start(ap, format);
    vsnprintf(output+n, sizeof(output)-n, format, ap); va_end(ap);
    strcat(output,"\n");
}
#define ESP_LOGI(tag,...) capture(__VA_ARGS__)
''' + recorder + r'''
int main(void) {
    nuwax_battery_history_t h;
    nuwax_battery_history_reset(&h);
    assert(sizeof(h)<2200);
    assert(!nuwax_battery_history_get(&h,0));
    assert(!nuwax_battery_history_dump_due(&h,100000));
    for (unsigned i=0;i<10000;i++) {
        nuwax_battery_history_sample_t s={.timestamp_ms=(uint64_t)UINT32_MAX+i*8000ULL,
            .current_ma=(int16_t)-(int)(i%300),.error=(int32_t)i};
        nuwax_battery_history_append(&h,s);
        const unsigned first=i<64?0:i-63;
        assert(h.count==(i<64?i+1:64));
        for (unsigned j=0;j<h.count;j++) {
            const nuwax_battery_history_sample_t *r=nuwax_battery_history_get(&h,j);
            assert(r->sequence==first+j && r->error==(int32_t)(first+j));
            assert(r->timestamp_ms==(uint64_t)UINT32_MAX+(first+j)*8000ULL);
        }
        assert(!nuwax_battery_history_get(&h,h.count));
    }
    nuwax_battery_history_reset(&h);
    nuwax_battery_history_append(&h,(nuwax_battery_history_sample_t){.timestamp_ms=0});
    assert(!nuwax_battery_history_dump_due(&h,63999));
    assert(nuwax_battery_history_dump_due(&h,64000));
    assert(!nuwax_battery_history_dump_due(&h,64000));
    assert(!nuwax_battery_history_dump_due(&h,1));
    assert(nuwax_battery_history_dump_due(&h,64001));

    nuwax_battery_service_snapshot_t s={.valid=true,.voltage_mv=4042,.current_ma=80,
        .percent=94,.display_percent=94,.remaining_mah=1067,.full_charge_mah=1140,
        .temperature_c=51,.charging=false};
    bq27220_battery_status_t raw={.telemetry_valid=true,.power_sample_valid=true,
        .battery_status_flags=0x5128};
    nuwax_battery_history_reset(&s_charge_history);
    for (unsigned i=0;i<=48;i++) {
        fake_ms=1000+i*8000ULL;
        s.current_ma=i<8?80:i<40?-120:0;
        console_connected=i>=40; /* 256 seconds of console loss. */
        record_charge_history(&s,&raw,0);
    }
    assert(strstr(output,"charge history begin revision=charge-history-v1"));
    assert(strstr(output,"charge history row=8,65000,4042,-120,94,94,51,1067,1140,5128,07,00,0"));
    assert(strstr(output,"charge history row=40,321000,4042,0,"));
    assert(strstr(output,"charge history end latest_seq=48"));
    assert(yields>0);
    raw.telemetry_valid=false; raw.power_sample_valid=false; s.stale=true;
    fake_ms+=8000; record_charge_history(&s,&raw,0x107);
    const nuwax_battery_history_sample_t *r=nuwax_battery_history_get(&s_charge_history,49);
    assert(r && r->error==0x107 && r->quality==12 && r->flags==0);
    raw.power_sample_valid=true; fake_ms+=8000;
    record_charge_history(&s,&raw,0x107);
    r=nuwax_battery_history_get(&s_charge_history,50);
    assert(r && r->quality==14 && r->flags==0x5128);
    nuwax_battery_history_reset(&s_charge_history);
    assert(s_charge_history.count==0 && s_charge_history.next_sequence==0);
    puts("PASS: history wrap/order, 64-bit time, dump cadence, console loss recovery, negative current, error validity, service restart and static memory bound");
}
'''
with tempfile.TemporaryDirectory(prefix='battery-history-') as folder:
    p=Path(folder)
    (p/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n', encoding='utf-8')
    (p/'test.c').write_text(program, encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',f'-I{p}',f'-I{root/"main"}',
                    str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
    subprocess.run([str(p/'test.exe')],check=True)
