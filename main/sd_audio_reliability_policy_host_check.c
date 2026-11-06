#include <assert.h>
#include <stdio.h>
#include "offline_capture_health.h"
#include "sd_audio_file_fault.h"

int main(void)
{
    nuwax_offline_capture_watch_t watch = {.buffering = true, .buffering_since_ms = 100U};
    assert(nuwax_offline_recovery_budget_ms(&watch, 14100U, 768U, 20U, 2000U, 10000U) == 1000U);
    assert(nuwax_offline_recovery_budget_ms(&watch, 15100U, 768U, 20U, 2000U, 10000U) == 0U);
    watch.buffering = false;
    assert(nuwax_offline_recovery_budget_ms(&watch, 0U, 100U, 20U, 2000U, 10000U) == 0U);
    assert(nuwax_offline_recovery_budget_ms(&watch, 0U, 101U, 20U, 2000U, 10000U) == 20U);
    assert(nuwax_offline_recovery_budget_ms(&watch, 0U, UINT32_MAX, UINT32_MAX, 2000U, 10000U) == 10000U);
    watch.buffering = true; watch.buffering_since_ms = UINT32_MAX - 999U;
    assert(nuwax_offline_recovery_budget_ms(&watch, 13000U, 768U, 20U, 2000U, 10000U) == 1000U);
    for (unsigned elapsed = 0U; elapsed <= 16000U; elapsed += 20U) {
        watch.buffering_since_ms = 37U;
        for (unsigned free = 0U; free <= 768U; free += 7U) {
            const unsigned budget = nuwax_offline_recovery_budget_ms(&watch, 37U + elapsed, free, 20U, 2000U, 10000U);
            assert(budget <= 10000U);
            if (budget) assert(elapsed + budget <= 15000U && budget + 2000U <= free * 20U);
        }
    }
    sd_audio_damage_window_t window = {0};
    assert(!sd_audio_damage_note(&window, 75U));
    assert(!sd_audio_damage_note(&window, 75U));
    sd_audio_damage_healthy(&window, 74U);
    assert(window.armed && sd_audio_damage_note(&window, 76U));
    window = (sd_audio_damage_window_t){0};
    assert(!sd_audio_damage_note(&window, 75U));
    sd_audio_damage_healthy(&window, 76U);
    assert(!window.armed && !sd_audio_damage_note(&window, 77U));
    puts("PASS recovery budget: existing 15s grace, real capacity reserve, zero budget, clock wrap and 88k pressure combinations");
    puts("PASS damage window: duplicate failure/old healthy result cannot hide damage; later full healthy file resets window");
    return 0;
}
