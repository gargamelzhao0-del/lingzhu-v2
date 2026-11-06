"""Exercise the real QMI sample entry under control-lock contention."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'main/board/qmi8658a_driver.c').read_text()
start = source.index('esp_err_t qmi8658a_read_data(')
end = source.index('\nbool qmi8658a_is_present', start)
function = source[start:end]
common = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NOT_FINISHED 4
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
#define QMI8658A_REG_AX_L 0x35
typedef struct {int16_t accel_x,accel_y,accel_z,gyro_x,gyro_y,gyro_z;
 float accel_scale,gyro_scale; uint64_t timestamp_us;} qmi8658a_sensor_data_t;
static struct {bool initialized; void *lock,*dev; float accel_scale,gyro_scale;} s_qmi;
static int busy,reads,releases,read_result;
static int xSemaphoreTake(void *lock,unsigned timeout){
 assert(lock); assert(timeout==0); return !busy;
}
static void xSemaphoreGive(void *lock){assert(lock);releases++;}
static esp_err_t read_regs(int reg,uint8_t *buf,size_t len){
 assert(reg==0x35 && len==12); reads++; memset(buf,0,len);buf[0]=42;return read_result;
}
static int16_t le16(const uint8_t *p){return (int16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint64_t esp_timer_get_time(void){return 1234;}
'''
tests = r'''
int main(void){
 qmi8658a_sensor_data_t v={0},zero={0};
 assert(qmi8658a_read_data(NULL)==ESP_ERR_INVALID_ARG);
 assert(qmi8658a_read_data(&v)==ESP_ERR_INVALID_STATE);
 s_qmi.initialized=true;s_qmi.lock=s_qmi.dev=&s_qmi;
 s_qmi.accel_scale=1;s_qmi.gyro_scale=2;
 busy=1;memset(&v,0x55,sizeof(v));
 assert(qmi8658a_read_data(&v)==ESP_ERR_NOT_FINISHED);
 assert(!memcmp(&v,&zero,sizeof(v)) && reads==0 && releases==0);
 busy=0;assert(qmi8658a_read_data(&v)==ESP_OK);
 assert(v.accel_x==42 && v.timestamp_us==1234 && reads==1 && releases==1);
 read_result=ESP_ERR_TIMEOUT;
 assert(qmi8658a_read_data(&v)==ESP_ERR_TIMEOUT);
 assert(!memcmp(&v,&zero,sizeof(v)) && reads==2 && releases==2);
 read_result=ESP_OK;assert(qmi8658a_read_data(&v)==ESP_OK);
 assert(v.accel_x==42 && reads==3 && releases==3);
 puts("PASS: QMI poll contention, zeroed failure, balanced unlock and recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='qmi-poll-') as directory:
    path=Path(directory)/'check.c'
    exe=path.with_suffix('.exe')
    for mutant in (False, True):
        body=function.replace('xSemaphoreTake(s_qmi.lock, 0)',
                              'xSemaphoreTake(s_qmi.lock, portMAX_DELAY)') if mutant else function
        path.write_text(common+body+tests)
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        if mutant:
            assert result.returncode!=0, 'Unbounded polling wait escaped'
            print('PASS: rejected unbounded QMI mutex wait')
        else:
            assert result.returncode==0,result.stderr
            print(result.stdout.strip())
