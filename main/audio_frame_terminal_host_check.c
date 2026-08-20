/* Host-only uplink terminal contract check. Not part of firmware sources. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "audio_frame.h"
#include "conversation/audio_uplink_terminal.h"

#define TEST_BATCH_FRAMES 4U
#define TEST_STREAM_CAPACITY 512U

static uint16_t read_u16le(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t read_u32le(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static int check_stream(size_t data_frame_count)
{
    uint8_t stream[TEST_STREAM_CAPACITY] = {0};
    size_t stream_len = 0;
    size_t last_frame_offset = 0;
    int failures = 0;

    for (size_t i = 0; i < data_frame_count; i++) {
        const int16_t sample = (int16_t)(100 + i);
        size_t frame_len = 0;
        last_frame_offset = stream_len;
        esp_err_t err = nuwax_audio_frame_build_pcm16(
            &stream[stream_len], sizeof(stream) - stream_len, &sample, 1,
            (uint32_t)i, (uint32_t)i * 20U,
            i == 0 ? NUWAX_AUDIO_FLAG_START : 0, 16000, 1, 20,
            &frame_len);
        if (err != ESP_OK) {
            printf("[FAIL] frames=%u data build index=%u err=%d\n",
                   (unsigned)data_frame_count, (unsigned)i, err);
            return 1;
        }
        stream_len += frame_len;
    }

    const bool marker_expected =
        data_frame_count == 0 || (data_frame_count % TEST_BATCH_FRAMES) == 0;
    if (marker_expected) {
        size_t marker_len = 0;
        esp_err_t err = nuwax_audio_uplink_build_terminal_marker(
            &stream[stream_len], sizeof(stream) - stream_len,
            NUWAX_AUDIO_CODEC_PCM16, (uint32_t)data_frame_count,
            data_frame_count != 0, 16000, 1, 20, &marker_len);
        if (err != ESP_OK) {
            printf("[FAIL] frames=%u marker build err=%d\n",
                   (unsigned)data_frame_count, err);
            return 1;
        }
        stream_len += marker_len;
    } else {
        esp_err_t err = nuwax_audio_uplink_mark_last_frame_end(
            stream, stream_len, last_frame_offset);
        if (err != ESP_OK) {
            printf("[FAIL] frames=%u tail mark err=%d\n",
                   (unsigned)data_frame_count, err);
            return 1;
        }
    }

    size_t offset = 0;
    size_t parsed_frames = 0;
    unsigned start_count = 0;
    unsigned end_count = 0;
    uint8_t last_flags = 0;
    uint16_t last_payload_bytes = UINT16_MAX;
    while (offset < stream_len) {
        uint8_t codec = 0;
        uint8_t flags = 0;
        size_t frame_len = 0;
        esp_err_t err = nuwax_audio_frame_peek(
            &stream[offset], stream_len - offset, &codec, &flags, &frame_len);
        if (err != ESP_OK || codec != NUWAX_AUDIO_CODEC_PCM16) {
            failures++;
            break;
        }

        const uint32_t sequence = read_u32le(&stream[offset + 10U]);
        const uint16_t payload_bytes = read_u16le(&stream[offset + 18U]);
        if (sequence != (uint32_t)parsed_frames) {
            failures++;
        }
        if (parsed_frames < data_frame_count) {
            if (payload_bytes != sizeof(int16_t)) {
                failures++;
            }
        } else if (!marker_expected || payload_bytes != 0) {
            failures++;
        }
        if ((flags & NUWAX_AUDIO_FLAG_START) != 0) {
            start_count++;
        }
        if ((flags & NUWAX_AUDIO_FLAG_END) != 0) {
            end_count++;
            if (offset + frame_len != stream_len) {
                failures++;
            }
        }

        last_flags = flags;
        last_payload_bytes = payload_bytes;
        parsed_frames++;
        offset += frame_len;
    }

    const size_t expected_frames =
        data_frame_count + (marker_expected ? 1U : 0U);
    if (offset != stream_len || parsed_frames != expected_frames ||
        start_count != 1 || end_count != 1 ||
        (last_flags & NUWAX_AUDIO_FLAG_END) == 0) {
        failures++;
    }
    if (marker_expected) {
        const uint8_t expected_flags =
            NUWAX_AUDIO_FLAG_END |
            (data_frame_count == 0 ? NUWAX_AUDIO_FLAG_START : 0U);
        if (last_payload_bytes != 0 || last_flags != expected_flags) {
            failures++;
        }
    } else if (last_payload_bytes != sizeof(int16_t)) {
        failures++;
    }

    printf("[%s] uplink terminal frames=%u parsed=%u marker=%d start=%u end=%u\n",
           failures == 0 ? "PASS" : "FAIL", (unsigned)data_frame_count,
           (unsigned)parsed_frames, marker_expected ? 1 : 0,
           start_count, end_count);
    return failures;
}

static int check_start_transfer(void)
{
    uint8_t frame[64] = {0};
    const int16_t sample = 123;
    size_t frame_len = 0U;
    if (nuwax_audio_frame_build_pcm16(
            frame, sizeof(frame), &sample, 1U, 7U, 140U,
            NUWAX_AUDIO_FLAG_END, 16000U, 1U, 20U, &frame_len) != ESP_OK ||
        nuwax_audio_uplink_ensure_batch_start(frame, frame_len) != ESP_OK) {
        puts("[FAIL] uplink START transfer could not update a valid frame");
        return 1;
    }
    uint8_t flags = 0U;
    size_t parsed_len = 0U;
    if (nuwax_audio_frame_peek(frame, frame_len, NULL, &flags,
                               &parsed_len) != ESP_OK ||
        parsed_len != frame_len ||
        flags != (NUWAX_AUDIO_FLAG_START | NUWAX_AUDIO_FLAG_END)) {
        puts("[FAIL] uplink START transfer did not preserve END");
        return 1;
    }
    if (nuwax_audio_uplink_ensure_batch_start(frame, 4U) !=
        ESP_ERR_INVALID_SIZE) {
        puts("[FAIL] uplink START transfer accepted a truncated frame");
        return 1;
    }
    puts("[PASS] uplink START transfers to first delivered frame and preserves END");
    return 0;
}

static int check_pcm_parser_guards(void)
{
    _Alignas(int16_t) uint8_t frame[64] = {0};
    const int16_t sample = 321;
    size_t frame_len = 0U;
    const int16_t *parsed_samples = NULL;
    size_t sample_count = 0U;
    if (nuwax_audio_frame_build_pcm16(
            frame, sizeof(frame), &sample, 1U, 0U, 0U,
            NUWAX_AUDIO_FLAG_START, 16000U, 1U, 20U, &frame_len) != ESP_OK ||
        nuwax_audio_frame_parse_pcm16(frame, frame_len, &parsed_samples,
                                      &sample_count, NULL, NULL, NULL) !=
            ESP_OK ||
        sample_count != 1U || parsed_samples[0] != sample) {
        puts("[FAIL] PCM parser rejected a valid aligned frame");
        return 1;
    }

    frame[2] = 2U;
    if (nuwax_audio_frame_parse_pcm16(frame, frame_len, &parsed_samples,
                                      &sample_count, NULL, NULL, NULL) !=
        ESP_ERR_INVALID_ARG) {
        puts("[FAIL] PCM parser accepted an unsupported protocol version");
        return 1;
    }

    _Alignas(int16_t) uint8_t unaligned_storage[65] = {0};
    uint8_t *unaligned_frame = &unaligned_storage[1];
    if (nuwax_audio_frame_build_pcm16(
            unaligned_frame, 64U, &sample, 1U, 0U, 0U,
            NUWAX_AUDIO_FLAG_START, 16000U, 1U, 20U, &frame_len) != ESP_OK ||
        nuwax_audio_frame_parse_pcm16(unaligned_frame, frame_len,
                                      &parsed_samples, &sample_count,
                                      NULL, NULL, NULL) !=
            ESP_ERR_INVALID_SIZE) {
        puts("[FAIL] PCM parser accepted an unaligned native sample view");
        return 1;
    }

    if (nuwax_audio_frame_build_pcm16(
            frame, sizeof(frame), &sample,
            SIZE_MAX / sizeof(int16_t) + 1U,
            0U, 0U, 0U, 16000U, 1U, 20U, &frame_len) !=
        ESP_ERR_INVALID_SIZE) {
        puts("[FAIL] PCM builder accepted a wrapped sample byte count");
        return 1;
    }
    puts("[PASS] PCM parser version, alignment, and size guards");
    return 0;
}

int main(void)
{
    static const size_t cases[] = {0, 1, 3, 4, 5, 8};
    int failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        failures += check_stream(cases[i]);
    }
    failures += check_start_transfer();
    failures += check_pcm_parser_guards();

    printf("audio uplink terminal host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
