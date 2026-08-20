#include <assert.h>

#include "conversation/conversation_endpoint_policy.h"

int main(void) {
    assert(nuwax_conversation_endpoint_decide(
               true, true, false, 900U, 449U, 700U, 450U, 650U) ==
           NUWAX_CONVERSATION_ENDPOINT_NONE);
    assert(nuwax_conversation_endpoint_decide(
               true, true, false, 900U, 450U, 700U, 450U, 650U) ==
           NUWAX_CONVERSATION_ENDPOINT_CANDIDATE);
    assert(nuwax_conversation_endpoint_decide(
               true, true, false, 900U, 650U, 700U, 450U, 650U) ==
           NUWAX_CONVERSATION_ENDPOINT_COMMIT);
    assert(nuwax_conversation_endpoint_decide(
               true, true, true, 900U, 650U, 700U, 450U, 650U) ==
           NUWAX_CONVERSATION_ENDPOINT_NONE);
    assert(nuwax_conversation_endpoint_decide(
               true, true, false, 699U, 650U, 700U, 450U, 650U) ==
           NUWAX_CONVERSATION_ENDPOINT_NONE);
    assert(nuwax_conversation_endpoint_decide(
               false, true, false, 900U, 650U, 700U, 450U, 650U) ==
           NUWAX_CONVERSATION_ENDPOINT_NONE);

    nuwax_conversation_weak_continuation_t weak = {0};
    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, true, false, 3U, 20U));
    assert(nuwax_conversation_weak_continuation_pending(&weak));
    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, true, false, 3U, 20U));
    assert(nuwax_conversation_weak_continuation_observe(
        &weak, true, true, false, 3U, 20U));
    assert(nuwax_conversation_weak_continuation_observe(
        &weak, true, true, false, 3U, 20U));

    for (unsigned i = 0U; i < 100U; ++i) {
        assert(nuwax_conversation_weak_continuation_observe(
            &weak, true, true, false, 3U, 20U));
    }
    assert(weak.weak_only_frames == 20U);
    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, false, false, 3U, 20U));

    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, true, true, 3U, 20U));
    assert(weak.weak_only_frames == 0U);
    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, true, true, 3U, 20U));
    assert(nuwax_conversation_weak_continuation_observe(
        &weak, true, true, true, 3U, 20U));
    for (unsigned i = 0U; i < 25U; ++i) {
        assert(nuwax_conversation_weak_continuation_observe(
            &weak, true, true, true, 3U, 20U));
    }

    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, false, false, 3U, 20U));
    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, true, false, 3U, 20U));
    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, true, false, 3U, 20U));
    assert(nuwax_conversation_weak_continuation_observe(
        &weak, true, true, false, 3U, 20U));
    for (unsigned i = 0U; i < 40U; ++i) {
        assert(nuwax_conversation_weak_continuation_observe(
            &weak, true, true, false, 3U, 20U));
    }
    assert(weak.weak_only_frames == 20U);

    nuwax_conversation_weak_continuation_rearm(&weak);
    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, true, false, 3U, 20U));
    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, true, false, 3U, 20U));
    assert(nuwax_conversation_weak_continuation_observe(
        &weak, true, true, false, 3U, 20U));

    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, true, false, false, 3U, 20U));
    assert(!nuwax_conversation_weak_continuation_pending(&weak));
    assert(!nuwax_conversation_weak_continuation_observe(
        &weak, false, true, false, 3U, 20U));
    assert(!nuwax_conversation_weak_continuation_pending(&weak));

    assert(nuwax_conversation_weak_level_evidence(
        140U, 297U, 140U, 70U, 300U, 1200U));
    assert(nuwax_conversation_weak_level_evidence(
        138U, 505U, 138U, 80U, 300U, 1200U));
    assert(!nuwax_conversation_weak_level_evidence(
        82U, 152U, 83U, 70U, 300U, 1200U));
    assert(!nuwax_conversation_weak_level_evidence(
        83U, 152U, 82U, 70U, 300U, 1200U));
    assert(!nuwax_conversation_weak_level_evidence(
        83U, 151U, 83U, 70U, 300U, 1200U));
    assert(!nuwax_conversation_weak_level_evidence(
        82U, 151U, 82U, 70U, 300U, 1200U));
    assert(nuwax_conversation_weak_level_evidence(
        83U, 152U, 83U, 70U, 300U, 1200U));
    assert(!nuwax_conversation_weak_level_evidence(
        215U, 864U, 215U, 0U, 300U, 1200U));
    assert(!nuwax_conversation_weak_level_evidence(
        216U, 863U, 216U, 0U, 300U, 1200U));
    assert(nuwax_conversation_weak_level_evidence(
        216U, 864U, 0U, 0U, 300U, 1200U));
    assert(!nuwax_conversation_weak_level_evidence(
        120U, 500U, 120U, 0U, 300U, 1200U));
    assert(!nuwax_conversation_weak_start_level_evidence(
        86U, 360U, 86U, 0U, 300U, 1200U));
    assert(!nuwax_conversation_weak_start_level_evidence(
        87U, 359U, 87U, 0U, 300U, 1200U));
    assert(!nuwax_conversation_weak_start_level_evidence(
        87U, 360U, 86U, 0U, 300U, 1200U));
    assert(nuwax_conversation_weak_start_level_evidence(
        87U, 360U, 87U, 0U, 300U, 1200U));
    assert(!nuwax_conversation_weak_start_level_evidence(
        93U, 198U, 93U, 55U, 300U, 1200U));
    assert(!nuwax_conversation_weak_start_level_evidence(
        94U, 197U, 94U, 55U, 300U, 1200U));
    assert(nuwax_conversation_weak_start_level_evidence(
        94U, 198U, 94U, 55U, 300U, 1200U));
    assert(!nuwax_conversation_weak_start_level_evidence(
        115U, 252U, 115U, 70U, 300U, 1200U));
    assert(nuwax_conversation_weak_start_level_evidence(
        116U, 252U, 116U, 70U, 300U, 1200U));

    assert(nuwax_conversation_weak_start_frame_evidence(
        false, false, false, false, false, false, true));
    assert(nuwax_conversation_weak_start_frame_evidence(
        false, false, true, false, false, false, false));
    assert(!nuwax_conversation_weak_start_frame_evidence(
        true, false, true, true, true, true, true));
    assert(!nuwax_conversation_weak_start_frame_evidence(
        false, true, true, true, true, true, true));

    assert(nuwax_conversation_weak_start_eligible(
        true, false, false, false, false, false));
    assert(!nuwax_conversation_weak_start_eligible(
        false, false, false, false, false, false));
    assert(!nuwax_conversation_weak_start_eligible(
        true, true, false, false, false, false));
    assert(!nuwax_conversation_weak_start_eligible(
        true, false, true, false, false, false));
    assert(!nuwax_conversation_weak_start_eligible(
        true, false, false, true, false, false));
    assert(!nuwax_conversation_weak_start_eligible(
        true, false, false, false, true, false));
    assert(!nuwax_conversation_weak_start_eligible(
        true, false, false, false, false, true));

    nuwax_conversation_correlated_weak_t correlated_weak = {0};
    for (unsigned elapsed_ms = 20U;
         elapsed_ms < NUWAX_CONVERSATION_CORRELATED_WEAK_CONFIRM_MS;
         elapsed_ms += 20U) {
        assert(!nuwax_conversation_correlated_weak_observe(
            &correlated_weak, true, true, true, true, true, 20U));
    }
    assert(correlated_weak.consecutive_ms == 220U);
    assert(nuwax_conversation_correlated_weak_observe(
        &correlated_weak, true, true, true, true, true, 20U));
    assert(correlated_weak.consecutive_ms ==
           NUWAX_CONVERSATION_CORRELATED_WEAK_CONFIRM_MS);

    assert(!nuwax_conversation_correlated_weak_observe(
        &correlated_weak, true, true, false, true, true, 20U));
    assert(correlated_weak.consecutive_ms == 0U);
    assert(!nuwax_conversation_correlated_weak_observe(
        &correlated_weak, true, true, true, true, true, 20U));
    assert(!nuwax_conversation_correlated_weak_observe(
        &correlated_weak, true, true, true, false, true, 20U));
    assert(correlated_weak.consecutive_ms == 0U);
    assert(!nuwax_conversation_correlated_weak_observe(
        &correlated_weak, true, true, true, true, true, 20U));
    assert(!nuwax_conversation_correlated_weak_observe(
        &correlated_weak, true, true, true, true, false, 20U));
    assert(correlated_weak.consecutive_ms == 0U);
    assert(!nuwax_conversation_correlated_weak_observe(
        &correlated_weak, true, true, true, true, true, 20U));
    assert(!nuwax_conversation_correlated_weak_observe(
        &correlated_weak, false, true, true, true, true, 20U));
    assert(correlated_weak.consecutive_ms == 0U);

    assert(nuwax_conversation_utterance_next_limit_ms(
               NUWAX_CONVERSATION_UTTERANCE_SOFT_LIMIT_MS,
               20U,
               true,
               true) == 60000U);
    assert(nuwax_conversation_utterance_next_limit_ms(
               60000U,
               NUWAX_CONVERSATION_UTTERANCE_RECENT_CONFIRMED_MS,
               true,
               true) == 75000U);
    assert(nuwax_conversation_utterance_next_limit_ms(
               75000U,
               20U,
               true,
               true) == 90000U);
    assert(nuwax_conversation_utterance_next_limit_ms(
               90000U,
               20U,
               true,
               true) == 105000U);
    assert(nuwax_conversation_utterance_next_limit_ms(
               NUWAX_CONVERSATION_UTTERANCE_SOFT_LIMIT_MS,
               NUWAX_CONVERSATION_UTTERANCE_RECENT_CONFIRMED_MS + 1U,
               true,
               true) == NUWAX_CONVERSATION_UTTERANCE_SOFT_LIMIT_MS);
    assert(nuwax_conversation_utterance_next_limit_ms(
               NUWAX_CONVERSATION_UTTERANCE_SOFT_LIMIT_MS,
               20U,
               false,
               true) == NUWAX_CONVERSATION_UTTERANCE_SOFT_LIMIT_MS);
    assert(nuwax_conversation_utterance_next_limit_ms(
               NUWAX_CONVERSATION_UTTERANCE_SOFT_LIMIT_MS,
               20U,
               true,
               false) == NUWAX_CONVERSATION_UTTERANCE_SOFT_LIMIT_MS);
    assert(nuwax_conversation_utterance_next_limit_ms(
               UINT32_MAX - 1000U,
               20U,
               true,
               true) == UINT32_MAX);
    return 0;
}
