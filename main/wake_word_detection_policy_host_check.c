#include <assert.h>

#include "conversation/wake_word_alias_policy.h"
#include "conversation/wake_word_detection_policy.h"

int main(void) {
    const char *const aliases[NUWAX_WAKE_FACTORY_ALIAS_COUNT] = {
        "xiao zhu xiao zhu",
        "xiao zu xiao zu",
        "xiao zhu xiao zu",
        "xiao zu xiao zhu",
        "ni hao ling zhu",
        "ni hao lin zhu",
        "ni hao ling zu",
        "ni hao lin zu",
        "ni hao xiao zu",
        "ni hao xiao zhu",
    };
    assert(nuwax_wake_command_variant_count(aliases[0]) ==
           NUWAX_WAKE_FACTORY_ALIAS_COUNT);
    for (size_t i = 0; i < NUWAX_WAKE_FACTORY_ALIAS_COUNT; ++i) {
        assert(strcmp(nuwax_wake_command_variant_at(aliases[0], i), aliases[i]) == 0);
        if (i < NUWAX_WAKE_XIAOZHU_ALIAS_COUNT) {
            assert(nuwax_wake_command_is_xiaozhu_family(aliases[i]));
        } else {
            assert(nuwax_wake_command_is_lingzhu_family(aliases[i]));
        }
    }
    assert(nuwax_wake_command_variant_at(aliases[0],
                                         NUWAX_WAKE_FACTORY_ALIAS_COUNT) == NULL);
    assert(nuwax_wake_command_variant_count("xiao nu") == 1U);
    assert(strcmp(nuwax_wake_command_variant_at("xiao nu", 0U), "xiao nu") == 0);
    assert(nuwax_wake_command_variant_at("xiao nu", 1U) == NULL);

    assert(!nuwax_wake_detection_accept(
        100U, 0U, 0U, 0U, 0U, 0U));
    /* Exact score/voice tuple from the environmental chatter incident. */
    assert(!nuwax_wake_detection_accept(
        187U, 125U, 0U, 0U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        990U, 125U, 0U, 0U, 0U, 16000U));
    assert(nuwax_wake_detection_accept(
        187U, 100U, 250U, 160U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        187U, 100U, 199U, 250U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        187U, 100U, 250U, 250U, 161U, 16000U));
    assert(!nuwax_wake_detection_accept(
        99U, 0U, 0U, 0U, 0U, 0U));
    assert(!nuwax_wake_detection_accept(
        89U, 100U, 200U, 250U, 0U, 16000U));
    /* The requested 10% reduction is relative: 0.10 -> 0.09, not 10 points. */
    for (uint32_t score = 0U; score < 90U; ++score) {
        assert(!nuwax_wake_detection_accept(
            score, 100U, 1000U, 1000U, 0U, 16000U));
    }
    for (uint32_t score = 90U; score <= 100U; ++score) {
        assert(nuwax_wake_detection_accept(
            score, 100U, 1000U, 1000U, 0U, 16000U));
        assert(!nuwax_wake_detection_accept(
            score, 100U, 1000U, 159U, 0U, 16000U));
        assert(!nuwax_wake_detection_accept(
            score, 100U, 199U, 250U, 0U, 16000U));
        assert(!nuwax_wake_detection_accept(
            score, 100U, 1000U, 1000U, 161U, 16000U));
    }
    /* 2026-10-08 chatter: measured score/noise/signal/voiced duration.
     * With valid unclipped counters these old chatter tuples now pass the
     * lowered score floor. Keep this explicit sensitivity tradeoff visible;
     * the incident logs did not include exact clipping/sample counters. */
    assert(nuwax_wake_detection_accept(
        96U, 51U, 286U, 704U, 0U, 16000U));
    assert(nuwax_wake_detection_accept(
        98U, 35U, 354U, 448U, 0U, 16000U));
    assert(nuwax_wake_detection_accept(
        99U, 51U, 286U, 704U, 0U, 16000U));
    assert(nuwax_wake_detection_accept(
        100U, 51U, 286U, 704U, 0U, 16000U));
    assert(nuwax_wake_detection_accept(
        100U, 100U, 200U, 250U, 160U, 16000U));
    assert(nuwax_wake_detection_accept(
        100U, 100U, 200U, 3500U, 0U, 16000U));
    assert(nuwax_wake_detection_accept(
        100U, 100U, 250U, 160U, 0U, 16000U));
    assert(nuwax_wake_detection_accept(
        100U, 100U, 250U, 249U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        100U, 0U, 200U, 250U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        100U, 100U, 200U, 249U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        100U, 100U, 250U, 159U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        100U, 100U, 249U, 160U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        100U, 100U, 200U, 3501U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        100U, 100U, 199U, 250U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        100U, 100U, 200U, 250U, 161U, 16000U));
    assert(!nuwax_wake_detection_accept(
        100U, 100U, 250U, 160U, 161U, 16000U));

    assert(nuwax_wake_voice_threshold(58U) == 118U);
    assert(nuwax_wake_voice_frame_is_voice_like(118U, 400U, 58U));
    assert(!nuwax_wake_voice_frame_is_voice_like(117U, 400U, 58U));
    assert(!nuwax_wake_voice_frame_is_voice_like(118U, 399U, 58U));
    assert(!nuwax_wake_voice_tail_expired(1000U, 100U));
    assert(nuwax_wake_voice_tail_expired(1001U, 100U));
    assert(!nuwax_wake_voice_tail_expired(1001U, 0U));

    nuwax_wake_evidence_t e = {0};
    nuwax_wake_evidence_snapshot_t ev = nuwax_wake_evidence_snapshot(&e, 0, 16000);
    assert(!ev.voiced_ms && !ev.total_samples);
    for (unsigned i = 0; i < 5; ++i) {
        nuwax_wake_evidence_observe(&e, i * 32U, 512, 0, 250, true);
    }
    ev = nuwax_wake_evidence_snapshot(&e, 128, 16000);
    assert(ev.voiced_ms == 160 && ev.age_ms == 0 && ev.total_samples == 2560);
    assert(nuwax_wake_detection_accept(187, 100, ev.signal_max, ev.voiced_ms,
                                       ev.clipped_samples, ev.total_samples));
    /* A model search timeout does not reset evidence. Late results still need
     * recent voice; silent tail contributes no voiced duration. */
    nuwax_wake_evidence_observe(&e, 800, 512, 0, 20, false);
    ev = nuwax_wake_evidence_snapshot(&e, 1028, 16000);
    assert(ev.voiced_ms == 160 && ev.age_ms == 900);
    ev = nuwax_wake_evidence_snapshot(&e, 1029, 16000);
    assert(!ev.voiced_ms && !ev.signal_max);
    /* A capture gap, candidate consumption or re-arm discards old evidence. */
    nuwax_wake_evidence_reset(&e);
    ev = nuwax_wake_evidence_snapshot(&e, 128, 16000);
    assert(!ev.voiced_ms && !ev.total_samples);
    /* Isolated loud impulse followed by silence is not 160 ms of voice. */
    nuwax_wake_evidence_observe(&e, 2000, 512, 0, 2000, true);
    for (unsigned i = 1; i < 20; ++i)
        nuwax_wake_evidence_observe(&e, 2000 + i * 32, 512, 0, 10, false);
    ev = nuwax_wake_evidence_snapshot(&e, 2608, 16000);
    assert(ev.voiced_ms == 32);
    assert(!nuwax_wake_detection_accept(990, 100, ev.signal_max, ev.voiced_ms,
                                        ev.clipped_samples, ev.total_samples));
    /* Ring rollover and long speech remain bounded, not permanently rejected
     * by a span that grows for the whole conversation. */
    nuwax_wake_evidence_reset(&e);
    for (unsigned i = 0; i < 10000; ++i) {
        nuwax_wake_evidence_observe(&e, i * 32U, 512, 0, 250, true);
        if (i >= 120) {
            ev = nuwax_wake_evidence_snapshot(&e, i * 32U, 16000);
            assert(nuwax_wake_detection_accept(187, 100, ev.signal_max, ev.voiced_ms,
                                               ev.clipped_samples, ev.total_samples));
        }
    }
    ev = nuwax_wake_evidence_snapshot(&e, 9999U * 32U, 16000);
    assert(ev.voiced_ms >= 3300 && ev.voiced_ms <= 3500);
    assert(nuwax_wake_detection_accept(187, 100, ev.signal_max, ev.voiced_ms,
                                       ev.clipped_samples, ev.total_samples));
    /* Bucket times remain correct through the uint32 millisecond wrap. */
    nuwax_wake_evidence_reset(&e);
    for (unsigned i = 0; i < 10; ++i)
        nuwax_wake_evidence_observe(&e, UINT32_MAX - 127U + i * 32U, 512, 0, 250, true);
    ev = nuwax_wake_evidence_snapshot(&e, 160, 16000);
    assert(ev.voiced_ms == 320 && ev.age_ms == 0);
    ev = nuwax_wake_evidence_snapshot(&e, 5000, 16000);
    assert(!ev.voiced_ms && !ev.total_samples);
    /* A valid voiced frame at millisecond zero is not a sentinel. */
    nuwax_wake_evidence_reset(&e);
    nuwax_wake_evidence_observe(&e, 0, 512, 0, 250, true);
    assert(nuwax_wake_evidence_snapshot(&e, 0, 16000).voiced_ms == 32);
    assert(nuwax_wake_evidence_snapshot(&e, 0, 0).voiced_ms == 0);
    nuwax_wake_evidence_observe(&e, 32, 512, 513, 250, true);
    assert(nuwax_wake_evidence_snapshot(&e, 32, 16000).voiced_ms == 0);
    return 0;
}
