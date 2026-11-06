#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef struct { uint32_t timeout_ms; } sdmmc_command_t;
static TaskHandle_t current = (TaskHandle_t)1;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }
static unsigned calls;
static bool return_late;
static esp_err_t sdmmc_host_do_transaction(int slot, sdmmc_command_t *cmd)
{
    assert(slot == 0 && cmd->timeout_ms); ++calls;
    if (return_late) now_us += 10000;
    return ESP_OK;
}
#include "board/sd_card_deadline.inc"
int main(void)
{
    assert(nuwax_sd_card_io_deadline_begin(0) == ESP_ERR_TIMEOUT);
    assert(nuwax_sd_card_io_deadline_begin(5000) == ESP_OK);
    assert(nuwax_sd_card_io_deadline_begin(9000) == ESP_ERR_NOT_FINISHED);
    sdmmc_command_t cmd = {.timeout_ms = 1000U};
    assert(sd_deadline_transaction(0, &cmd) == ESP_OK && cmd.timeout_ms == 5U && calls == 1U);
    current = (TaskHandle_t)2;
    nuwax_sd_card_io_deadline_end();
    assert(nuwax_sd_card_io_deadline_begin(9000) == ESP_ERR_NOT_FINISHED);
    current = (TaskHandle_t)1; now_us = 5000;
    assert(sd_deadline_transaction(0, &cmd) == ESP_ERR_TIMEOUT && calls == 1U);
    nuwax_sd_card_io_deadline_end(); now_us = 0;
    assert(nuwax_sd_card_io_deadline_begin(5000) == ESP_OK);
    return_late = true;cmd.timeout_ms = 0;
    assert(sd_deadline_transaction(0, &cmd) == ESP_ERR_TIMEOUT && cmd.timeout_ms == 5U && calls == 2U);
    nuwax_sd_card_io_deadline_end(); return_late = false;
    cmd.timeout_ms = 700U;
    assert(sd_deadline_transaction(0, &cmd) == ESP_OK && cmd.timeout_ms == 700U);
    puts("PASS actual SD deadline hook: residual native timeout, no I/O after expiry, late success refused, wrong owner/nested scopes cannot release deadline");
    return 0;
}
