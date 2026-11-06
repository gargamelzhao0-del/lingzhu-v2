/* Actual SDK command/status/wait bodies, with a host-transaction boundary.
 * This verifies error propagation/order, not a physical SD controller. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <inttypes.h>
#include "sd_protocol_defs.h"
#include "board/sd_probe_trace.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_INVALID_STATE 0x103
#define SDMMC_HOST_FLAG_SPI 0x10000
#define SCF_CMD_ADTC 1
#define SCF_RSP_R1 2
#define SCF_CMD_READ 4
#define SCF_CMD_AC 8
#define SDMMC_DEFAULT_CMD_TIMEOUT_MS 1000
#define SDMMC_WRITE_CMD_TIMEOUT_MS 1000
#define SDMMC_READY_FOR_DATA_TIMEOUT_US 5000000
static unsigned error_logs;
#define ESP_LOGE(...) ((void)++error_logs)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGV(...) ((void)0)
typedef struct command {unsigned flags;size_t blklen,datalen,buflen;void*data;
    unsigned timeout_ms,opcode;size_t arg;uint32_t response[4];esp_err_t error;} sdmmc_command_t;
typedef struct {struct {size_t sector_size,capacity;}csd;uint32_t ocr;unsigned rca;
    struct {unsigned flags,command_timeout_ms;int slot;
    esp_err_t(*do_transaction)(int,sdmmc_command_t*);}host;} sdmmc_card_t;
static unsigned status_calls,scenario,commands,observer_calls;
static bool observer_enabled;
bool nuwax_sd_probe_trace_active(void) { return observer_enabled; }
static int64_t tick;
static int64_t esp_timer_get_time(void){tick+=1000000;return tick;}
static void vTaskDelay(int n){(void)n;}
static esp_err_t sdmmc_send_cmd_num_of_written_blocks(sdmmc_card_t*c,size_t*n){(void)c;*n=1;return ESP_OK;}
void nuwax_sd_probe_trace_command(const void*c,uint32_t op,uint32_t arg,uint32_t response,
    int err,const void*buffer,size_t bytes)
{ (void)c;(void)op;(void)arg;(void)buffer;(void)bytes;assert(observer_enabled);if(err!=ESP_OK)assert(!response);++observer_calls; }
static esp_err_t test_host(int slot,sdmmc_command_t*cmd)
{
    (void)slot;++commands;
    if(cmd->opcode==MMC_SEND_STATUS){
        ++status_calls;
        if(scenario==1){cmd->response[0]=0;if(status_calls==2)cmd->error=ESP_ERR_INVALID_CRC;}
        else if(scenario==2)cmd->response[0]=0;
        else cmd->response[0]=(1U<<8)|(4U<<9)|(scenario==4?(1U<<26):0U);
        return ESP_OK;
    }
    assert(cmd->opcode==MMC_READ_BLOCK_SINGLE||cmd->opcode==MMC_WRITE_BLOCK_SINGLE);
    assert(commands==1&&cmd->arg==1492384&&cmd->datalen==512&&cmd->blklen==512);
    if(scenario==3)cmd->error=ESP_ERR_INVALID_CRC;
    return ESP_OK;
}
#include "sd_status_sdk_host.inc"
int main(void)
{
    sdmmc_card_t card={.csd={512,10000000},.ocr=SD_OCR_SDHC_CAP,.rca=1,
        .host={.do_transaction=test_host}};uint8_t buffer[512]={0};
    for(unsigned scope=0;scope<2;scope++)for(unsigned write=0;write<2;write++)for(scenario=0;scenario<5;scenario++){
        observer_enabled=scope!=0;
        status_calls=commands=observer_calls=error_logs=0;tick=0;
        int err=write?sdmmc_write_sectors_dma(&card,buffer,1492384,1,512):sdmmc_read_sectors_dma(&card,buffer,1492384,1,512);
        int expected=scenario==2?ESP_ERR_TIMEOUT:scenario==3?ESP_ERR_INVALID_CRC:ESP_OK;
#ifdef EXPECT_FIXED
        if(scenario==1)expected=ESP_ERR_INVALID_CRC;
        assert(observer_calls==(observer_enabled?commands:0U));
#else
        assert(!observer_calls);
#endif
        assert(err==expected&&commands==1+status_calls);
        if(scenario==1)assert(error_logs==1&&status_calls==2);
        if(scenario==0||scenario==4)assert(!error_logs&&status_calls==1&&commands==2);
    }
    puts("PASS actual SDK: normal order, data/wait CRC, timeout propagation; observer adds no card commands; no new R1 rejection policy");
}
