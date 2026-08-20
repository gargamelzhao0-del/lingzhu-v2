/*
 * P5-LIVEBARGEIN stage 0 diagnostic: live fast gate A/B host-only check
 *
 * Goal (per Codex 2026-07-02 12:20 任务 P6-LIVEBARGEIN-004-3C-HOSTCHECK-SEMANTICS):
 *   - Mirror CURRENT FIRMWARE P3/P5 semantics for live fast gate.
 * This is a LOGIC-only check. It does NOT predict real mic/AEC output
 * nor real hit probability. Real hit rate must come from 004-3A +
 * later stage 5/6 logs.
 *
 * Build (strict, must succeed with no warnings):
 *   gcc -Wall -Wextra -Wpedantic -Werror -std=c11 \
 *       main/p5_live_fast_gate_host_check.c -o p5lf_host
 *
 * Run:
 *   ./p5lf_host   (delete exe after run; do not commit)
 *
 * Out of CMake build: p5_live_fast_gate_host_check.c is NOT added to
 * CMakeLists.txt SRCS, so the firmware build is unaffected.
 */

#include <stdio.h>
#include <stdbool.h>

/* ---------- Mocked P3 result (mirror nuwax_p3_nearend_candidate_result_t) ---- */
typedef struct {
    /* voice ok flags (the 4 'shape' inputs) */
    bool p6_low_ref_normal_voice_ok;
    bool p6_high_ref_voice_ok;
    bool p6_mid_ref_strong_voice_ok;
    bool p6_normal_voice_ok;
    /* narrow normalvoice (P6-LIVEBARGEIN-004-4C) */
    bool p6_mid_ref_high_erle_voice_ok;
    /* nearend-only broad evidence: record/diagnostic only, never cancel */
    bool p6_low_ref_nearend_ok;
    /* explicit short-circuit danger: always false in real firmware, used to
     * gate down anything that would otherwise look 'strong + high ref'. */
    bool p6_high_ref_strong_voice_ok;
    /* inter-mic correlation evidence */
    bool active_dual_mic_voice_ok;
    /* top-level P3 candidate flag (mirrors firmware r->candidate). */
    bool candidate;
    /* raw acoustic inputs (only used by fixture-driven cases for diagnostic) */
    float active_ref_ratio;
    float active_mic_rms;
    float active_ref_rms;
    float active_out_rms;
    float active_out_to_ref_db;
    float active_erle_db;
    float active_dual_mic_corr;
} p3_mock_t;

/* ---------- Narrow normalvoice cancel class (host mock, Codex 004-4B) ----- *
 * Eval with raw acoustic inputs from real 140947/140244 device frames:
 *   - ref_ratio 0.60..0.75
 *   - mic_rms  >= 3800
 *   - out_rms  550..900
 *   - out_to_ref_db -4.0..0.0
 *   - erle     >= 17.5
 * This is a NARROW class on purpose. It is gated by the existing
 * resource/data quality/gate_ok soft-checks at the caller side.
 */
static bool p6_narrow_normalvoice_ok(const p3_mock_t *c) {
    if (c == NULL) return false;
    return c->active_ref_ratio >= 0.60f && c->active_ref_ratio <= 0.75f &&
           c->active_mic_rms >= 3800.0f &&
           c->active_out_rms >= 550.0f && c->active_out_rms <= 900.0f &&
           c->active_out_to_ref_db >= -4.0f && c->active_out_to_ref_db <= 0.0f &&
           c->active_erle_db >= 17.5f;
}

/* ---------- P3 cancel candidate (host mirror of 004-2A) ------------------- */
static bool p3_p6_cancel_candidate(const p3_mock_t *c) {
    if (c == NULL) return false;
    /* Per Codex 004-4C: DANGER FIRST. high_ref_strong_voice is the
     * "playback + ref dominant" lock; suppress cancel even if any other
     * voice class is true, matching firmware line 367 (`false` short-cut).
     * Strict danger: high_ref_strong_voice_ok MUST NOT pass. */
    if (c->p6_high_ref_strong_voice_ok) return false;
    return c->p6_low_ref_normal_voice_ok ||
           c->p6_mid_ref_strong_voice_ok ||
           c->p6_normal_voice_ok ||
           c->p6_high_ref_voice_ok ||
           p6_narrow_normalvoice_ok(c);
}

/* P3 record candidate: mirror firmware line 430 — broader path including
 * p6_low_ref_nearend_ok and cancel-candidate trigger. */
static bool p3_p6_record_candidate(const p3_mock_t *c) {
    if (c == NULL) return false;
    /* Mirror firmware P3 record path:
     * record = candidate || p6_low_ref_nearend_ok || p3_p6_cancel_candidate. */
    return c->candidate || c->p6_low_ref_nearend_ok || p3_p6_cancel_candidate(c);
}

/* ---------- P5 live fast gate: A (current strict) vs B (allow single-mic fallback) */
static bool live_cancel_voice_class_ok_A(const p3_mock_t *c) {
    /* CURRENT firmware: requires active_dual_mic_voice_ok AND one of 4 voice_ok.
     * DANGER-FIRST: high_ref_strong_voice suppresses BOTH A and B. */
    if (c == NULL) return false;
    if (c->p6_high_ref_strong_voice_ok) return false;
    if (!c->active_dual_mic_voice_ok) return false;
    return c->p6_low_ref_normal_voice_ok ||
           c->p6_high_ref_voice_ok ||
           c->p6_mid_ref_strong_voice_ok ||
           c->p6_normal_voice_ok ||
           p6_narrow_normalvoice_ok(c);
}

static bool live_cancel_voice_class_ok_B(const p3_mock_t *c) {
    /* Strategy B: allow single-mic fallback. Treated as a 004-2A-style
     * 'dual-mic-bypass' = any of the 4 voice_ok flags, without the
     * active_dual_mic_voice_ok gate. Still NOT including high_ref_strong
     * (the explicit danger short-circuit). Codex 004-4B: ALSO OR-in the
     * narrow normalvoice class so F1 140947 may hit B. */
    if (c == NULL) return false;
    /* DANGER-FIRST: high_ref_strong must veto B as well as A. */
    if (c->p6_high_ref_strong_voice_ok) return false;
    return c->p6_low_ref_normal_voice_ok ||
           c->p6_high_ref_voice_ok ||
           c->p6_mid_ref_strong_voice_ok ||
           c->p6_normal_voice_ok ||
           p6_narrow_normalvoice_ok(c);
}

/* Combined: live fast pass = P3 allows cancel AND live gate ok. */
static bool live_fast_pass_A(const p3_mock_t *c) {
    return p3_p6_cancel_candidate(c) && live_cancel_voice_class_ok_A(c);
}

static bool live_fast_pass_B(const p3_mock_t *c) {
    return p3_p6_cancel_candidate(c) && live_cancel_voice_class_ok_B(c);
}

/* ---------- Test cases (designated initializers) -------------------------- */
typedef struct {
    const char *label;
    p3_mock_t c;
    /* Expected outcomes (one per strategy). */
    bool p3_cancel;   /* expected: P3 allows cancel */
    bool p3_record;   /* expected: P3 allows record */
    bool A_pass;      /* expected: live fast pass under A (current strict) */
    bool B_pass;      /* expected: live fast pass under B (allow single-mic) */
    bool danger;      /* true if this case is explicitly dangerous; B must NOT pass */
} test_case_t;

static const test_case_t cases[] = {
    /* T1: normalvoice 50cm + dual mic  -> A and B both pass */
    {
        .label = "T1 normalvoice_50cm+dual",
        .c      = { .p6_normal_voice_ok = true, .active_dual_mic_voice_ok = true },
        .p3_cancel = true, .p3_record = true, .A_pass = true, .B_pass = true, .danger = false
    },
    /* T2: normalvoice 50cm + single mic  -> A suppress, B pass (single-mic fallback) */
    {
        .label = "T2 normalvoice_50cm+single_mic",
        .c      = { .p6_normal_voice_ok = true, .active_dual_mic_voice_ok = false },
        .p3_cancel = true, .p3_record = true, .A_pass = false, .B_pass = true, .danger = false
    },
    /* T3: normalvoice 100cm + dual mic  -> A and B both pass */
    {
        .label = "T3 normalvoice_100cm+dual",
        .c      = { .p6_normal_voice_ok = true, .active_dual_mic_voice_ok = true },
        .p3_cancel = true, .p3_record = true, .A_pass = true, .B_pass = true, .danger = false
    },
    /* T4: high_ref_voice 50cm + dual mic  -> A and B both pass (p6_high_ref_voice_ok
     *     participates in voice-class, but only when accompanied by mid_ref or
     *     nearend; here we include mid_ref to make P3 cancel pass). */
    {
        .label = "T4 high_ref_voice_50cm+dual+mid",
        .c      = { .p6_high_ref_voice_ok = true, .p6_mid_ref_strong_voice_ok = true,
                    .active_dual_mic_voice_ok = true },
        .p3_cancel = true, .p3_record = true, .A_pass = true, .B_pass = true, .danger = false
    },
    /* T5: DANGER: high_ref_strong_voice_ok=true alone + dual mic ->
     *     must NOT pass under A or B (firmware hard false + B must not regress). */
    {
        .label = "T5 high_ref_strong_DANGER",
        .c      = { .p6_high_ref_strong_voice_ok = true, .active_dual_mic_voice_ok = true },
        .p3_cancel = false, .p3_record = false, .A_pass = false, .B_pass = false, .danger = true
    },
    /* T6: low_ref_nearend_only (no shape, broad nearend) ->
     *     P3 cancel = NO (record-only path). A and B must both SUPPRESS live fast. */
    {
        .label = "T6 low_ref_nearend_only",
        .c      = { .p6_low_ref_nearend_ok = true, .active_dual_mic_voice_ok = true },
        .p3_cancel = false, .p3_record = true, .A_pass = false, .B_pass = false, .danger = false
    },
    /* T7: mid_ref_strong + single mic -> A suppress, B pass */
    {
        .label = "T7 mid_ref_strong+single",
        .c      = { .p6_mid_ref_strong_voice_ok = true, .active_dual_mic_voice_ok = false },
        .p3_cancel = true, .p3_record = true, .A_pass = false, .B_pass = true, .danger = false
    },
    /* T8: low_ref_normal_voice + dual mic -> A and B both pass (this is the
     *     narrow low-ref voice that 004-2A was widening to a single-mic path). */
    {
        .label = "T8 low_ref_normal_voice+dual",
        .c      = { .p6_low_ref_normal_voice_ok = true, .active_dual_mic_voice_ok = true },
        .p3_cancel = true, .p3_record = true, .A_pass = true, .B_pass = true, .danger = false
    },
    /* T9: high_ref_voice + dual mic -> A pass, B pass, p3_cancel true (Codex 004-4B:
     *     cancel candidate MUST include high_ref_voice). */
    {
        .label = "T9 high_ref_voice+dual",
        .c      = { .p6_high_ref_voice_ok = true, .active_dual_mic_voice_ok = true },
        .p3_cancel = true, .p3_record = true, .A_pass = true, .B_pass = true, .danger = false
    },
    /* T10: high_ref_voice + single mic -> A suppressed, B pass (no danger). */
    {
        .label = "T10 high_ref_voice+single",
        .c      = { .p6_high_ref_voice_ok = true, .active_dual_mic_voice_ok = false },
        .p3_cancel = true, .p3_record = true, .A_pass = false, .B_pass = true, .danger = false
    },
    /* T11: danger-first combined. high_ref_strong AND high_ref_voice must
     * cancel=0 even if other voice classes coexist (playback + near voice). */
    {
        .label = "T11 high_ref_strong_AND_high_ref_voice_DANGER",
        .c = { .p6_high_ref_strong_voice_ok = true, .p6_high_ref_voice_ok = true,
               .active_dual_mic_voice_ok = true },
        .p3_cancel = false, .p3_record = false, .A_pass = false, .B_pass = false, .danger = true
    },
    {
        /* F1 (004-3A positive fail 140947): user spoke normal voice on the device (ref_ratio=0.653,
         * mic_rms=5976, out_rms=720, out_to_ref_db=-3.72, erle=18.38, dual_mic_corr=0.995 but
         * dual_mic_voice_ok=0). All 4 voice_ok classes are 0; cancel_candidate must be 0; record
         * must be 1 (low_ref_nearend holds). Neither A nor B should currently pass — this is the
         * gap that the new "narrow_normalvoice_high_erle" class in fixture/F1 must address once
         * Phase 1 ships. */
        .label = "F1_positive_normalvoice_weather_fail_140947",
        .c = {
            .active_dual_mic_voice_ok = false,
            .candidate = true,
            .active_ref_ratio = 0.653f, .active_mic_rms = 5976.7f, .active_out_rms = 720.1f,
            .active_out_to_ref_db = -3.72f, .active_erle_db = 18.38f,
            .active_dual_mic_corr = 0.995f,
            .p6_high_ref_strong_voice_ok = false,
            .p6_high_ref_voice_ok = false,
            .p6_mid_ref_strong_voice_ok = false,
            .p6_normal_voice_ok = false,
            .p6_low_ref_normal_voice_ok = false,
            .p6_low_ref_nearend_ok = true,
        },
        .p3_cancel = true, .p3_record = true,
        .A_pass = false, .B_pass = true, .danger = false
    },
    {
        /* F2 (004-3A playbackonly pass): pure TTS playback, nearend_observed=1 but no real user voice.
         * dual_mic_voice_ok must hold (otherwise this is just single-mic and may be unsafe under B);
         * however record_candidate is 0 (low_ref_nearend=0), and ALL cancel classes are 0 because
         * there's no real voice. So live fast pass must be 0 under A and B alike. */
        .label = "F2_playbackonly_nocancel_pass_140244",
        .c = {
            .active_dual_mic_voice_ok = true,
            .candidate = true,
            .active_ref_ratio = 0.640f, .active_mic_rms = 4495.0f, .active_out_rms = 1649.9f,
            .active_out_to_ref_db = 1.74f, .active_erle_db = 8.71f,
            .active_dual_mic_corr = 0.94f,
            .p6_high_ref_strong_voice_ok = false,
            .p6_high_ref_voice_ok = false,
            .p6_mid_ref_strong_voice_ok = false,
            .p6_normal_voice_ok = false,
            .p6_low_ref_normal_voice_ok = false,
            .p6_low_ref_nearend_ok = false,
        },
        .p3_cancel = false, .p3_record = true,
        .A_pass = false, .B_pass = false, .danger = false
    }
};

static const int N_CASES = (int)(sizeof(cases) / sizeof(cases[0]));

/* ---------- Acceptance predicates (Codex task 004-3C) ---------------------- */
static int accept_predicates(int *n_pass_out) {
    int n_pass = 0;
    int n_total = 0;
    for (int i = 0; i < N_CASES; ++i) {
        const test_case_t *t = &cases[i];
        bool p3c = p3_p6_cancel_candidate(&t->c);
        bool p3r = p3_p6_record_candidate(&t->c);
        bool a   = live_fast_pass_A(&t->c);
        bool b   = live_fast_pass_B(&t->c);
        printf("%-44s | p3_cancel=%d p3_record=%d A=%d B=%d danger=%d\n",
               t->label, p3c ? 1 : 0, p3r ? 1 : 0, a ? 1 : 0, b ? 1 : 0, t->danger ? 1 : 0);
        /* Compare actual against expected */
        ++n_total;
        if (p3c == t->p3_cancel && p3r == t->p3_record &&
            a   == t->A_pass    && b   == t->B_pass) {
            ++n_pass;
        } else {
            printf("  [MISMATCH] expected p3_cancel=%d p3_record=%d A=%d B=%d\n",
                   t->p3_cancel ? 1 : 0, t->p3_record ? 1 : 0,
                   t->A_pass ? 1 : 0, t->B_pass ? 1 : 0);
        }
    }
    if (n_pass_out) *n_pass_out = n_pass;
    return (n_pass == n_total) ? 0 : 1;
}

static int assert_b_never_passes_danger(void) {
    for (int i = 0; i < N_CASES; ++i) {
        if (cases[i].danger && live_fast_pass_B(&cases[i].c)) {
            printf("[FAIL] B strategy passed a DANGER case: %s\n", cases[i].label);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    printf("P5-LIVEBARGEIN live fast gate A/B host-only check (004-3C)\n");
    printf("------------------------------------------------\n");
    int n_pass = 0;
    int rc1 = accept_predicates(&n_pass);
    int rc2 = assert_b_never_passes_danger();
    if (rc1 == 0 && rc2 == 0) {
        printf("\n[HOST CHECK] ALL ACCEPTANCE PREDICATES PASSED (%d/%d).\n", n_pass, N_CASES);
        printf("Note: this only verifies LOGIC consistency. Real hit rate\n");
        printf("must still be confirmed by 004-3A + later real logs.\n");
        return 0;
    }
    printf("FAIL: at least one acceptance predicate failed.\n");
    return 1;
}

