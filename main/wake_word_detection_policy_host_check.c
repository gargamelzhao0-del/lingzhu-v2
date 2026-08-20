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

    assert(nuwax_wake_detection_accept(
        100U, 0U, 0U, 0U, 0U, 0U));
    assert(!nuwax_wake_detection_accept(
        99U, 0U, 0U, 0U, 0U, 0U));
    assert(!nuwax_wake_detection_accept(
        79U, 100U, 200U, 250U, 0U, 16000U));
    assert(nuwax_wake_detection_accept(
        80U, 100U, 200U, 250U, 160U, 16000U));
    assert(nuwax_wake_detection_accept(
        99U, 100U, 200U, 3500U, 0U, 16000U));
    assert(nuwax_wake_detection_accept(
        80U, 100U, 250U, 160U, 0U, 16000U));
    assert(nuwax_wake_detection_accept(
        99U, 100U, 250U, 249U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        80U, 0U, 200U, 250U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        80U, 100U, 200U, 249U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        80U, 100U, 250U, 159U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        80U, 100U, 249U, 160U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        80U, 100U, 200U, 3501U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        80U, 100U, 199U, 250U, 0U, 16000U));
    assert(!nuwax_wake_detection_accept(
        80U, 100U, 200U, 250U, 161U, 16000U));
    assert(!nuwax_wake_detection_accept(
        80U, 100U, 250U, 160U, 161U, 16000U));

    assert(nuwax_wake_voice_threshold(58U) == 118U);
    assert(nuwax_wake_voice_frame_is_voice_like(118U, 400U, 58U));
    assert(!nuwax_wake_voice_frame_is_voice_like(117U, 400U, 58U));
    assert(!nuwax_wake_voice_frame_is_voice_like(118U, 399U, 58U));
    assert(!nuwax_wake_voice_tail_expired(1000U, 100U));
    assert(nuwax_wake_voice_tail_expired(1001U, 100U));
    assert(!nuwax_wake_voice_tail_expired(1001U, 0U));
    return 0;
}
