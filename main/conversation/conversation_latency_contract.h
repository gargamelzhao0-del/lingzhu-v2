#pragma once

/* Product latency contract for the 20 ms normal-conversation path. Kconfig
 * exposes the effective values, while these assertions prevent any build from
 * silently drifting away from the jointly budgeted product profile. */
#define NUWAX_CONVERSATION_PROFILE_FRAME_MS 20U
#define NUWAX_CONVERSATION_PROFILE_CAPTURE_SLOTS 12U
#define NUWAX_CONVERSATION_PROFILE_CAPTURE_PRE_DSP_MAX_AGE_MS 60U
#define NUWAX_CONVERSATION_PROFILE_BARGE_END_TO_END_MAX_AGE_MS 100U
#define NUWAX_CONVERSATION_PROFILE_UPLINK_PACKET_SLOTS 16U
#define NUWAX_CONVERSATION_PROFILE_UPLINK_STAGING_MAX_AGE_MS 160U
#define NUWAX_CONVERSATION_PROFILE_UPLINK_RECOVERY_MAX_AGE_MS 320U
#define NUWAX_CONVERSATION_PROFILE_MIN_DSP_RESERVE_MS 40U
#define NUWAX_CONVERSATION_NO_SPEECH_RESUME_GRACE_MS 160U
#define NUWAX_CONVERSATION_CAPTURE_OWNER_POLL_MS 20U

/* The pre-DSP ceiling applies to ordinary decision frames. The existing
 * confirmed-barge preservation path remains an explicit, logged exception. */

#ifndef CONFIG_NUWAX_VOICE_FRAME_DURATION_MS
#define CONFIG_NUWAX_VOICE_FRAME_DURATION_MS \
    NUWAX_CONVERSATION_PROFILE_FRAME_MS
#endif
#ifndef CONFIG_NUWAX_RT_CAPTURE_QUEUE_DEPTH
#define CONFIG_NUWAX_RT_CAPTURE_QUEUE_DEPTH \
    NUWAX_CONVERSATION_PROFILE_CAPTURE_SLOTS
#endif
#ifndef CONFIG_NUWAX_RT_CAPTURE_MAX_DECISION_AGE_MS
#define CONFIG_NUWAX_RT_CAPTURE_MAX_DECISION_AGE_MS \
    NUWAX_CONVERSATION_PROFILE_CAPTURE_PRE_DSP_MAX_AGE_MS
#endif
#ifndef CONFIG_NUWAX_BARGE_END_TO_END_MAX_FRAME_AGE_MS
#define CONFIG_NUWAX_BARGE_END_TO_END_MAX_FRAME_AGE_MS \
    NUWAX_CONVERSATION_PROFILE_BARGE_END_TO_END_MAX_AGE_MS
#endif
#ifndef CONFIG_NUWAX_VOICE_UPLINK_QUEUE_DEPTH
#define CONFIG_NUWAX_VOICE_UPLINK_QUEUE_DEPTH \
    NUWAX_CONVERSATION_PROFILE_UPLINK_PACKET_SLOTS
#endif
#ifndef CONFIG_NUWAX_VOICE_UPLINK_REALTIME_MAX_AGE_MS
#define CONFIG_NUWAX_VOICE_UPLINK_REALTIME_MAX_AGE_MS \
    NUWAX_CONVERSATION_PROFILE_UPLINK_STAGING_MAX_AGE_MS
#endif

#define NUWAX_CONVERSATION_CAPTURE_SLOT_DEPTH \
    CONFIG_NUWAX_RT_CAPTURE_QUEUE_DEPTH
#define NUWAX_CONVERSATION_CAPTURE_PRE_DSP_MAX_AGE_MS \
    CONFIG_NUWAX_RT_CAPTURE_MAX_DECISION_AGE_MS
#define NUWAX_CONVERSATION_BARGE_END_TO_END_MAX_AGE_MS \
    CONFIG_NUWAX_BARGE_END_TO_END_MAX_FRAME_AGE_MS
#define NUWAX_CONVERSATION_UPLINK_PACKET_SLOT_DEPTH \
    CONFIG_NUWAX_VOICE_UPLINK_QUEUE_DEPTH
#define NUWAX_CONVERSATION_UPLINK_STAGING_MAX_AGE_MS \
    CONFIG_NUWAX_VOICE_UPLINK_REALTIME_MAX_AGE_MS

_Static_assert(CONFIG_NUWAX_VOICE_FRAME_DURATION_MS ==
                   NUWAX_CONVERSATION_PROFILE_FRAME_MS,
               "normal conversation requires 20 ms frames");
_Static_assert(NUWAX_CONVERSATION_CAPTURE_SLOT_DEPTH ==
                   NUWAX_CONVERSATION_PROFILE_CAPTURE_SLOTS,
               "normal-conversation capture slot contract drifted");
_Static_assert(NUWAX_CONVERSATION_CAPTURE_PRE_DSP_MAX_AGE_MS ==
                   NUWAX_CONVERSATION_PROFILE_CAPTURE_PRE_DSP_MAX_AGE_MS,
               "normal-conversation pre-DSP age contract drifted");
_Static_assert(NUWAX_CONVERSATION_BARGE_END_TO_END_MAX_AGE_MS ==
                   NUWAX_CONVERSATION_PROFILE_BARGE_END_TO_END_MAX_AGE_MS,
               "normal-conversation barge age contract drifted");
_Static_assert(NUWAX_CONVERSATION_UPLINK_PACKET_SLOT_DEPTH ==
                   NUWAX_CONVERSATION_PROFILE_UPLINK_PACKET_SLOTS,
               "normal-conversation uplink packet-slot contract drifted");
_Static_assert(NUWAX_CONVERSATION_UPLINK_STAGING_MAX_AGE_MS ==
                   NUWAX_CONVERSATION_PROFILE_UPLINK_STAGING_MAX_AGE_MS,
               "normal-conversation uplink age contract drifted");
_Static_assert(NUWAX_CONVERSATION_PROFILE_UPLINK_RECOVERY_MAX_AGE_MS >=
                   NUWAX_CONVERSATION_PROFILE_UPLINK_STAGING_MAX_AGE_MS,
               "uplink recovery bound must preserve the realtime soft target");
_Static_assert(NUWAX_CONVERSATION_PROFILE_UPLINK_RECOVERY_MAX_AGE_MS <=
                   NUWAX_CONVERSATION_PROFILE_UPLINK_PACKET_SLOTS *
                       NUWAX_CONVERSATION_PROFILE_FRAME_MS,
               "uplink recovery bound must fit the fixed realtime queue");
_Static_assert(NUWAX_CONVERSATION_CAPTURE_PRE_DSP_MAX_AGE_MS +
                       NUWAX_CONVERSATION_PROFILE_MIN_DSP_RESERVE_MS <=
                   NUWAX_CONVERSATION_BARGE_END_TO_END_MAX_AGE_MS,
               "barge end-to-end age must reserve at least 40 ms for DSP");
_Static_assert(NUWAX_CONVERSATION_CAPTURE_SLOT_DEPTH *
                       CONFIG_NUWAX_VOICE_FRAME_DURATION_MS >=
                   NUWAX_CONVERSATION_CAPTURE_PRE_DSP_MAX_AGE_MS,
               "capture slots must cover the pre-DSP admission budget");
_Static_assert(NUWAX_CONVERSATION_NO_SPEECH_RESUME_GRACE_MS %
                       CONFIG_NUWAX_VOICE_FRAME_DURATION_MS == 0U,
               "no-speech resume grace must align to whole audio frames");
_Static_assert(NUWAX_CONVERSATION_NO_SPEECH_RESUME_GRACE_MS >=
                       CONFIG_NUWAX_VOICE_FRAME_DURATION_MS * 4U &&
                   NUWAX_CONVERSATION_NO_SPEECH_RESUME_GRACE_MS <= 250U,
               "no-speech resume grace must remain bounded and tail-safe");
_Static_assert(NUWAX_CONVERSATION_CAPTURE_OWNER_POLL_MS > 0U &&
                   NUWAX_CONVERSATION_CAPTURE_OWNER_POLL_MS <=
                       CONFIG_NUWAX_VOICE_FRAME_DURATION_MS,
               "capture owner control polling must fit one audio frame");
