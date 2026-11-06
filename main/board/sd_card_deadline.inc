/* Included by the board driver after SDK/task declarations. A storage owner
 * keeps this scope through recovery and catalog reconciliation. Never cancel
 * a native transaction by deleting its task or freeing its DMA memory. */
static portMUX_TYPE s_io_deadline_mux = portMUX_INITIALIZER_UNLOCKED;
static struct { TaskHandle_t owner; int64_t until_us; } s_io_deadline;

static int64_t sd_io_deadline_current(void)
{
    const TaskHandle_t owner = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_io_deadline_mux);
    const int64_t until = s_io_deadline.owner == owner ? s_io_deadline.until_us : 0;
    portEXIT_CRITICAL(&s_io_deadline_mux);
    return until;
}

esp_err_t nuwax_sd_card_io_deadline_begin(int64_t deadline_us)
{
    if (deadline_us <= esp_timer_get_time()) return ESP_ERR_TIMEOUT;
    const TaskHandle_t owner = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_io_deadline_mux);
    const bool available = s_io_deadline.owner == NULL;
    if (available) { s_io_deadline.owner = owner; s_io_deadline.until_us = deadline_us; }
    portEXIT_CRITICAL(&s_io_deadline_mux);
    return available ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

void nuwax_sd_card_io_deadline_end(void)
{
    const TaskHandle_t owner = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_io_deadline_mux);
    if (s_io_deadline.owner == owner) { s_io_deadline.owner = NULL; s_io_deadline.until_us = 0; }
    portEXIT_CRITICAL(&s_io_deadline_mux);
}

static bool sd_io_deadline_expired(void)
{
    const int64_t until = sd_io_deadline_current();
    return until && esp_timer_get_time() >= until;
}

static esp_err_t sd_deadline_transaction(int slot, sdmmc_command_t *command)
{
    const int64_t until = sd_io_deadline_current();
    if (until) {
        const int64_t remaining = until - esp_timer_get_time();
        if (remaining <= 0) return ESP_ERR_TIMEOUT;
        const uint64_t remaining_ms = (uint64_t)remaining / 1000U + (remaining % 1000 != 0);
        const uint32_t ms = remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms;
        if (!command->timeout_ms || command->timeout_ms > ms) command->timeout_ms = ms;
    }
    const esp_err_t err = sdmmc_host_do_transaction(slot, command);
    /* Success after expiry can have written bytes: callers retain their
     * uncertain-frame proof and must not blindly resend or publish success. */
    return sd_io_deadline_expired() ? ESP_ERR_TIMEOUT : err;
}
