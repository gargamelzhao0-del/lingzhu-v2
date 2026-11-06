/* Historical failing implementation used only as a regression negative. */
static bool audio_uplink_queue_detached_cleanup(nuwax_audio_uplink_t *ctx) {
    return ctx && s_audio_uplink_reaper_queue &&
           xQueueSend(s_audio_uplink_reaper_queue, &ctx, 0) == pdTRUE;
}
esp_err_t nuwax_audio_uplink_destroy_owned(nuwax_audio_uplink_t **owner) {
    if (!owner || !*owner) {
        return ESP_OK;
    }
    nuwax_audio_uplink_t *ctx = *owner;
    if (!ctx->task) {
        *owner = NULL;
        nuwax_audio_uplink_destroy(ctx);
        heap_caps_free(ctx);
        return ESP_OK;
    }
    if (audio_uplink_flag_load(&ctx->finished)) {
        /* A finished task still owns its static TCB/stack until deletion
         * succeeds.  Do not free the context on a transient scheduler miss. */
        if (!audio_uplink_delete_task(ctx)) {
            if (!audio_uplink_queue_detached_cleanup(ctx)) {
                ESP_LOGE(TAG,
                         "finished uplink cleanup handoff failed; caller retains session");
                report_audio_uplink_fault("cleanup_handoff", ESP_ERR_TIMEOUT);
                return ESP_ERR_TIMEOUT;
            }
            *owner = NULL;
            ESP_LOGW(TAG,
                     "finished uplink detached for deferred task deletion epoch=%lu",
                     (unsigned long)ctx->ws_connection_epoch);
            return ESP_ERR_NOT_FINISHED;
        }
        *owner = NULL;
        nuwax_audio_uplink_destroy(ctx);
        heap_caps_free(ctx);
        return ESP_OK;
    }
    audio_uplink_flag_store(&ctx->capture_done, true);
    audio_uplink_flag_store(&ctx->send_failed, true);
    audio_uplink_notify_sender(ctx);
    if (!audio_uplink_queue_detached_cleanup(ctx)) {
        /* The caller retains ownership and may retry; never orphan a sender
         * context just because the cleanup handoff could not be queued. */
        ESP_LOGE(TAG,
                 "uplink detached cleanup handoff failed; caller retains session");
        report_audio_uplink_fault("cleanup_handoff", ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    *owner = NULL;
    ESP_LOGW(TAG,
             "uplink session detached for asynchronous cleanup with bounded cancellation grace epoch=%lu grace_ms=%u",
             (unsigned long)ctx->ws_connection_epoch,
             (unsigned)NUWAX_AUDIO_UPLINK_REAPER_CANCEL_GRACE_MS);
    return ESP_ERR_NOT_FINISHED;
}