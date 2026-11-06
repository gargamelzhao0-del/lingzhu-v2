/* Included by mic_input.c and the host contract test. raw_samples has
 * raw_capacity + MIC_DMA_BLOCK_BYTES storage; raw_capacity remains the public
 * per-call limit. Never retain a DMA pointer outside i2s_channel_read(). */
#define MIC_DMA_BLOCK_BYTES ((size_t)CONFIG_NUWAX_VOCAT_AUDIO_DMA_FRAME_NUM * \
                            NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t))

static void mic_dma_cache_reset(nuwax_mic_input_t *mic)
{
    mic->rx_cached_bytes = 0U;
    mic->rx_cached_offset = 0U;
}

static esp_err_t mic_read_dma_blocks(nuwax_mic_input_t *mic, size_t wanted,
                                    uint32_t timeout_ms, size_t *bytes_read)
{
    *bytes_read = 0U;
    if (wanted > mic->raw_capacity || MIC_DMA_BLOCK_BYTES == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *raw = (uint8_t *)mic->raw_samples;
    size_t available = mic->rx_cached_bytes;
    if (available > 0U) {
        memmove(raw, raw + mic->rx_cached_offset, available);
    }
    mic_dma_cache_reset(mic);
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t result = ESP_OK;
    while (available < wanted) {
        const int64_t remaining = deadline - esp_timer_get_time();
        const uint32_t wait_ms = remaining > 0
            ? (uint32_t)((remaining + 999) / 1000) : 0U;
        size_t received = 0U;
        result = i2s_channel_read((i2s_chan_handle_t)mic->rx_handle,
                                  raw + available, MIC_DMA_BLOCK_BYTES,
                                  &received, wait_ms);
        if (received > MIC_DMA_BLOCK_BYTES ||
            received % (NUWAX_VOCAT_MIC_SLOTS * sizeof(int16_t)) != 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (!is_i2s_read_error_usable(result, received)) {
            if (result != ESP_ERR_TIMEOUT) {
                return result;
            }
            break;
        }
        available += received;
        if (result == ESP_ERR_TIMEOUT || received == 0U ||
            esp_timer_get_time() >= deadline) {
            break;
        }
    }
    *bytes_read = available < wanted ? available : wanted;
    mic->rx_cached_offset = *bytes_read;
    mic->rx_cached_bytes = available - *bytes_read;
    return result;
}
