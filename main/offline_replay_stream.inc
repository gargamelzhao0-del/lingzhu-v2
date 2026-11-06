/* Included by the spool and its host regression harness. No FILE operations:
 * validate exactly the bytes being uploaded, including resumed prefixes. */
typedef struct {
    uint32_t position, length, filled, frames, crc, last_sequence;
    uint64_t payload_bytes;
    uint32_t direct_frames, copied_bytes;
    bool sequence_seen;
    uint8_t length_bytes[4];
    uint8_t frame_header[NUWAX_AUDIO_FRAME_HEADER_BYTES];
} replay_stream_t;

_Static_assert(sizeof(replay_stream_t) <= 128U, "replay hot state must remain bounded");

/* Metadata acceptance matches validate_archive_frame without constructing a
 * PCM pointer or assembling payload. Timestamp/duration/reserved policy stays
 * unchanged. Byte decoding accepts any input address alignment. */
static esp_err_t replay_stream_frame_header(replay_stream_t *v, const uint8_t *h,
    const archive_header_t *archive, uint32_t expected_payload)
{
    const uint32_t payload = (uint32_t)h[18] | ((uint32_t)h[19] << 8U);
    const uint32_t rate = (uint32_t)h[6] | ((uint32_t)h[7] << 8U);
    const uint32_t sequence = (uint32_t)h[10] | ((uint32_t)h[11] << 8U) |
        ((uint32_t)h[12] << 16U) | ((uint32_t)h[13] << 24U);
    if (h[0] != 'N' || h[1] != 'A' || h[2] != 1U ||
        h[3] != NUWAX_AUDIO_CODEC_PCM16 ||
        (h[4] & (NUWAX_AUDIO_FLAG_START | NUWAX_AUDIO_FLAG_END)) ||
        h[5] != archive->channels || rate != archive->sample_rate ||
        payload != expected_payload || payload + NUWAX_AUDIO_FRAME_HEADER_BYTES != v->length ||
        (v->sequence_seen && sequence <= v->last_sequence)) return ESP_ERR_INVALID_SIZE;
    v->last_sequence = sequence;
    v->sequence_seen = true;
    return ESP_OK;
}

static esp_err_t replay_stream_feed(replay_stream_t *v, const uint8_t *data,
                                    size_t bytes, uint32_t total,
                                    const archive_header_t *header,
                                    const archive_footer_t *footer)
{
    if (total < HEADER_BYTES + FOOTER_BYTES ||
        v->position > total || bytes > total - v->position)
        return ESP_ERR_INVALID_SIZE;
    /* Compute the format invariant once per read, not once per 20 ms frame. */
    const uint64_t samples = ((uint64_t)header->sample_rate * header->frame_duration_ms) / 1000U;
    if (header->codec != NUWAX_AUDIO_CODEC_PCM16 || header->channels != 1U ||
        !samples || samples > (MAX_FRAME_BYTES - NUWAX_AUDIO_FRAME_HEADER_BYTES) / 2U)
        return ESP_ERR_INVALID_SIZE;
    const uint32_t expected_payload = (uint32_t)samples * 2U;
    while (bytes) {
        const uint32_t footer_at = total - FOOTER_BYTES;
        if (v->position < HEADER_BYTES || v->position >= footer_at) {
            const bool head = v->position < HEADER_BYTES;
            const uint32_t at = head ? v->position : v->position - footer_at;
            size_t n = (head ? HEADER_BYTES : FOOTER_BYTES) - at;
            if (n > bytes) n = bytes;
            const uint8_t *expected = head ? (const uint8_t *)header : (const uint8_t *)footer;
            if (memcmp(data, expected + at, n) != 0) return ESP_ERR_INVALID_CRC;
            v->position += n; data += n; bytes -= n;
            continue;
        }
        if (!v->length) {
            if (footer_at - v->position < 4U - v->filled) return ESP_ERR_INVALID_SIZE;
            size_t n = 4U - v->filled;
            if (n > bytes) n = bytes;
            memcpy(v->length_bytes + v->filled, data, n);
            v->filled += n; v->position += n; data += n; bytes -= n;
            if (v->filled == 4U) {
                memcpy(&v->length, v->length_bytes, 4U);
                v->filled = 0U;
                if (v->length < NUWAX_AUDIO_FRAME_HEADER_BYTES || v->length > MAX_FRAME_BYTES ||
                    v->length > footer_at - v->position) return ESP_ERR_INVALID_SIZE;
            }
        } else {
            size_t n = v->length - v->filled;
            if (n > bytes) n = bytes;
            /* CRC consumes pieces in place, excluding archive length words.
             * Only a fragmented frame's header is retained, never its PCM. */
            const bool direct = v->filled == 0U && n == v->length;
            const uint8_t *frame = direct ? data : v->frame_header;
            if (direct) v->direct_frames++;
            else if (v->filled < NUWAX_AUDIO_FRAME_HEADER_BYTES) {
                size_t header_bytes = NUWAX_AUDIO_FRAME_HEADER_BYTES - v->filled;
                if (header_bytes > n) header_bytes = n;
                memcpy(v->frame_header + v->filled, data, header_bytes);
                v->copied_bytes += (uint32_t)header_bytes;
            }
            v->crc = crc32_update(v->crc, data, n);
            v->filled += n; v->position += n; data += n; bytes -= n;
            if (v->filled == v->length) {
                if (replay_stream_frame_header(v, frame, header, expected_payload) != ESP_OK)
                    return ESP_ERR_INVALID_SIZE;
                v->payload_bytes += v->length;
                v->frames++;
                v->length = 0U; v->filled = 0U;
            }
        }
    }
    if (v->position == total && (v->filled || v->length ||
        v->frames != footer->frame_count || v->payload_bytes != footer->payload_bytes ||
        v->crc != footer->crc32)) return ESP_ERR_INVALID_CRC;
    return ESP_OK;
}
