#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "conversation/speaker_stream_policy.h"

#define START_FLAG 0x01U
#define END_FLAG 0x02U
#define VAD_FLAG 0x04U
#define ALLOWED_FLAGS (START_FLAG | END_FLAG | VAD_FLAG)

static nuwax_speaker_stream_result_t validate(
    const nuwax_speaker_stream_policy_t *policy,
    uint32_t sequence,
    uint8_t flags,
    size_t samples) {
    return nuwax_speaker_stream_validate(policy,
                                         policy->generation,
                                         sequence,
                                         flags,
                                         ALLOWED_FLAGS,
                                         START_FLAG,
                                         END_FLAG,
                                         samples);
}

int main(void) {
    assert(nuwax_speaker_stream_frame_is_orphan(false, false));
    assert(!nuwax_speaker_stream_frame_is_orphan(true, false));
    assert(!nuwax_speaker_stream_frame_is_orphan(false, true));

    nuwax_speaker_stream_policy_t stream = {0};
    nuwax_speaker_stream_begin_generation(&stream, 7U, 3U, 1600U);
    assert(validate(&stream, 0U, START_FLAG, 320U) ==
           NUWAX_SPEAKER_STREAM_OK);
    nuwax_speaker_stream_commit(&stream, 0U, START_FLAG, END_FLAG, 320U);
    assert(validate(&stream, 0U, 0U, 320U) ==
           NUWAX_SPEAKER_STREAM_BAD_SEQUENCE);
    assert(validate(&stream, 2U, 0U, 320U) ==
           NUWAX_SPEAKER_STREAM_BAD_SEQUENCE);
    assert(validate(&stream, 1U, 0U, 160U) ==
           NUWAX_SPEAKER_STREAM_BAD_FRAME_SAMPLES);
    assert(validate(&stream, 1U, 0U, 320U) ==
           NUWAX_SPEAKER_STREAM_OK);
    nuwax_speaker_stream_commit(&stream, 1U, 0U, END_FLAG, 320U);
    assert(validate(&stream, 2U, END_FLAG, 160U) ==
           NUWAX_SPEAKER_STREAM_OK);
    nuwax_speaker_stream_commit(&stream, 2U, END_FLAG, END_FLAG, 160U);
    assert(stream.pcm_frames == 3U && stream.pcm_bytes == 1600U &&
           stream.end_accepted);
    assert(validate(&stream, 3U, 0U, 320U) ==
           NUWAX_SPEAKER_STREAM_POST_END);

    nuwax_speaker_stream_begin_generation(&stream, 8U, 2U, 1280U);
    assert(validate(&stream, 1U, START_FLAG, 320U) ==
           NUWAX_SPEAKER_STREAM_INVALID_START);
    assert(validate(&stream, 0U, START_FLAG, 0U) ==
           NUWAX_SPEAKER_STREAM_INVALID_START);
    assert(validate(&stream, 0U, START_FLAG | 0x80U, 320U) ==
           NUWAX_SPEAKER_STREAM_INVALID_FLAGS);

    nuwax_speaker_stream_begin_generation(&stream, 9U, 2U, 1280U);
    assert(validate(&stream, 0U, START_FLAG, 320U) ==
           NUWAX_SPEAKER_STREAM_OK);
    nuwax_speaker_stream_commit(&stream, 0U, START_FLAG, END_FLAG, 320U);
    assert(validate(&stream, 1U, END_FLAG, 0U) ==
           NUWAX_SPEAKER_STREAM_TOTAL_MISMATCH);

    nuwax_speaker_stream_begin_generation(&stream, 10U, 1U, 640U);
    assert(validate(&stream, 0U, START_FLAG | END_FLAG, 320U) ==
           NUWAX_SPEAKER_STREAM_OK);
    nuwax_speaker_stream_commit(&stream,
                                0U,
                                START_FLAG | END_FLAG,
                                END_FLAG,
                                320U);
    assert(nuwax_speaker_stream_bind_expected(&stream, 10U, 1U, 640U));
    assert(!nuwax_speaker_stream_bind_expected(&stream, 10U, 2U, 640U));

    nuwax_speaker_stream_begin_generation(&stream, 11U, 0U, 0U);
    assert(validate(&stream, 0U, START_FLAG, 320U) ==
           NUWAX_SPEAKER_STREAM_OK);
    nuwax_speaker_stream_commit(&stream, 0U, START_FLAG, END_FLAG, 320U);
    stream.next_sequence = UINT32_MAX;
    assert(validate(&stream, UINT32_MAX, 0U, 320U) ==
           NUWAX_SPEAKER_STREAM_OK);
    nuwax_speaker_stream_commit(&stream, UINT32_MAX, 0U, END_FLAG, 320U);
    assert(stream.next_sequence == 0U);
    assert(validate(&stream, 0U, END_FLAG, 0U) ==
           NUWAX_SPEAKER_STREAM_OK);

    /* A new generation must discard every sequence, size and END bit from
     * the prior generation. */
    nuwax_speaker_stream_begin_generation(&stream, 12U, 0U, 0U);
    assert(stream.active && !stream.started && !stream.end_accepted);
    assert(stream.generation == 12U && stream.next_sequence == 0U);
    assert(stream.frame_samples == 0U && stream.pcm_frames == 0U &&
           stream.pcm_bytes == 0U);
    assert(validate(&stream, 0U, START_FLAG, 320U) ==
           NUWAX_SPEAKER_STREAM_OK);
    nuwax_speaker_stream_commit(&stream, 0U, START_FLAG, END_FLAG, 320U);
    assert(validate(&stream, 1U, 0U, 320U) ==
           NUWAX_SPEAKER_STREAM_OK);
    nuwax_speaker_stream_commit(&stream, 1U, 0U, END_FLAG, 320U);

    /* playback_begin can arrive after START/PCM. Binding exact future totals
     * must succeed, while totals below already accepted PCM must fail. */
    assert(nuwax_speaker_stream_bind_expected(&stream, 12U, 3U, 1600U));
    assert(!nuwax_speaker_stream_bind_expected(&stream, 12U, 1U, 640U));
    assert(!nuwax_speaker_stream_bind_expected(&stream, 13U, 3U, 1600U));
    assert(validate(&stream, 2U, END_FLAG, 160U) ==
           NUWAX_SPEAKER_STREAM_OK);
    nuwax_speaker_stream_commit(&stream, 2U, END_FLAG, END_FLAG, 160U);
    assert(nuwax_speaker_stream_bind_expected(&stream, 12U, 3U, 1600U));
    assert(!nuwax_speaker_stream_bind_expected(&stream, 12U, 4U, 1600U));
    assert(validate(&stream, 3U, 0U, 320U) ==
           NUWAX_SPEAKER_STREAM_POST_END);

    nuwax_speaker_stream_reset(&stream);
    assert(!stream.active && stream.generation == 0U &&
           validate(&stream, 0U, START_FLAG, 320U) ==
               NUWAX_SPEAKER_STREAM_INACTIVE);

    puts("speaker stream policy host check: PASS");
    return 0;
}
