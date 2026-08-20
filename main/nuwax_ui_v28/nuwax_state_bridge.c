#include "nuwax_state_bridge.h"

#include "emotion_display_policy.h"
#include "nuwax_device_state.h"
#include "ui_bridge_revision_policy.h"
#include "../app_shell.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static const char *TAG = "nuwax_state_bridge";
static nuwax_emotion_display_policy_t s_emotion_policy;
static bool s_emotion_policy_initialized;
static nuwax_app_state_t s_current_app_state = NUWAX_APP_STATE_BOOTING;
static nuwax_app_state_t s_last_app_state = NUWAX_APP_STATE_BOOTING;
static uint32_t s_bridge_revision;
static portMUX_TYPE s_bridge_state_mux = portMUX_INITIALIZER_UNLOCKED;
#define SEMANTIC_OWNER_SOURCE_CAPACITY 24U
static char s_semantic_owner_source[SEMANTIC_OWNER_SOURCE_CAPACITY];
static uint32_t s_semantic_owner_revision;
static uint32_t s_semantic_reject_through_revision;

static void copy_semantic_owner_source(char *dst, const char *source) {
    const char *value = source ? source : "";
    size_t length = strlen(value);
    if (length >= SEMANTIC_OWNER_SOURCE_CAPACITY) {
        length = SEMANTIC_OWNER_SOURCE_CAPACITY - 1U;
    }
    memcpy(dst, value, length);
    dst[length] = '\0';
}

static void ensure_policy_initialized_locked(void) {
    if (!s_emotion_policy_initialized) {
        nuwax_emotion_display_policy_init(&s_emotion_policy);
        s_emotion_policy_initialized = true;
    }
}

static uint32_t bridge_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static uint32_t next_bridge_revision_locked(void) {
    s_bridge_revision++;
    if (s_bridge_revision == 0U) {
        s_bridge_revision = 1U;
    }
    return s_bridge_revision;
}

static nuwax_device_state_t state_snapshot(void) {
    nuwax_device_state_t state = {0};
    if (!nuwax_app_shell_get_device_state_snapshot(&state, NULL)) {
        state = *nuwax_device_state_get();
    }
    return state;
}

static void publish_state_delta(const nuwax_device_state_t *state,
                                nuwax_app_shell_state_fields_t fields,
                                uint32_t bridge_revision) {
    if (!nuwax_app_shell_publish_device_state_delta_from_bridge(
            state, fields, bridge_revision)) {
        ESP_LOGD(TAG,
                 "stale canonical UI state dropped fields=0x%lx bridge_revision=%lu",
                 (unsigned long)fields,
                 (unsigned long)bridge_revision);
    }
}

static void publish_app_state_delta(
    nuwax_app_state_t app_state,
    const nuwax_device_state_t *state,
    nuwax_app_shell_state_fields_t fields,
    uint32_t bridge_revision) {
    if (!nuwax_app_shell_publish_app_state_delta_from_bridge(
            app_state, state, fields, bridge_revision)) {
        ESP_LOGD(TAG,
                 "stale canonical app/UI state dropped app_state=%d fields=0x%lx bridge_revision=%lu",
                 (int)app_state,
                 (unsigned long)fields,
                 (unsigned long)bridge_revision);
    }
}

bool nuwax_state_bridge_is_sleeping(void) {
    taskENTER_CRITICAL(&s_bridge_state_mux);
    const bool sleeping =
        s_current_app_state == NUWAX_APP_STATE_SLEEPING;
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    return sleeping;
}

static const char *emotion_name(nuwax_emotion_t emotion) {
    switch (emotion) {
        case NUWAX_EMOTION_NEUTRAL: return "neutral";
        case NUWAX_EMOTION_HAPPY: return "happy";
        case NUWAX_EMOTION_SURPRISED: return "surprised";
        case NUWAX_EMOTION_WINK: return "wink";
        case NUWAX_EMOTION_TONGUE: return "tongue";
        case NUWAX_EMOTION_BOOTING: return "booting";
        case NUWAX_EMOTION_LISTENING: return "listening";
        case NUWAX_EMOTION_THINKING: return "thinking";
        case NUWAX_EMOTION_FOCUSED: return "focused";
        case NUWAX_EMOTION_PENSIVE: return "pensive";
        case NUWAX_EMOTION_ANALYZING: return "analyzing";
        case NUWAX_EMOTION_PROCESSING: return "processing";
        case NUWAX_EMOTION_RECEIVED: return "received";
        case NUWAX_EMOTION_WHAT: return "what";
        case NUWAX_EMOTION_UNDERSTOOD: return "understood";
        case NUWAX_EMOTION_AWESOME: return "awesome";
        case NUWAX_EMOTION_LIKE: return "like";
        case NUWAX_EMOTION_SPEAKING: return "speaking";
        case NUWAX_EMOTION_SAD: return "sad";
        case NUWAX_EMOTION_LOST: return "lost";
        case NUWAX_EMOTION_ANGRY: return "angry";
        case NUWAX_EMOTION_SHY: return "shy";
        case NUWAX_EMOTION_TIRED: return "tired";
        case NUWAX_EMOTION_LOW_BATTERY: return "low_battery";
        case NUWAX_EMOTION_CHARGING: return "charging";
        case NUWAX_EMOTION_DIZZINESS: return "dizziness";
        case NUWAX_EMOTION_EXCITED: return "excited";
        case NUWAX_EMOTION_THOUGHT_COMPLETE: return "thought_complete";
        case NUWAX_EMOTION_SLEEPING_FACE: return "sleeping";
        case NUWAX_EMOTION_PROVISIONING: return "provisioning";
        case NUWAX_EMOTION_DISCONNECTED_FACE: return "disconnected";
        case NUWAX_EMOTION_MEETING_FACE: return "meeting";
        case NUWAX_EMOTION_MONITORING_FACE: return "monitoring";
        case NUWAX_EMOTION_TOUCH: return "touch";
        case NUWAX_EMOTION_PLAYFUL: return "playful";
        case NUWAX_EMOTION_QUESTION: return "question";
        default: return "other";
    }
}

static const char *dialog_name(nuwax_dialog_state_t dialog) {
    switch (dialog) {
        case NUWAX_DIALOG_SLEEP: return "sleep";
        case NUWAX_DIALOG_LISTENING: return "listening";
        case NUWAX_DIALOG_RECOGNIZING: return "recognizing";
        case NUWAX_DIALOG_THINKING: return "thinking";
        case NUWAX_DIALOG_SPEAKING: return "speaking";
        case NUWAX_DIALOG_ERROR: return "error";
        case NUWAX_DIALOG_READY: return "ready";
        default: return "unknown";
    }
}

static nuwax_emotion_t emotion_for_power(nuwax_power_state_t power) {
    /* Battery/charger state is status metadata, not a full-screen face. */
    (void)power;
    return NUWAX_EMOTION_NEUTRAL;
}

static bool power_overrides_emotion(nuwax_power_state_t power) {
    (void)power;
    return false;
}

static bool power_has_default_emotion(nuwax_power_state_t power) {
    (void)power;
    return false;
}

static nuwax_emotion_t current_or_neutral(nuwax_emotion_t emotion) {
    if (emotion == NUWAX_EMOTION_BOOTING ||
        emotion == NUWAX_EMOTION_LISTENING ||
        emotion == NUWAX_EMOTION_SPEAKING ||
        emotion >= NUWAX_EMOTION_COUNT) {
        return NUWAX_EMOTION_NEUTRAL;
    }
    return emotion;
}

static nuwax_emotion_t default_emotion_for_app_state(nuwax_app_state_t app_state,
                                                     nuwax_emotion_t current) {
    switch (app_state) {
        case NUWAX_APP_STATE_BOOTING:
            return NUWAX_EMOTION_BOOTING;
        case NUWAX_APP_STATE_WIFI_CONNECTING:
            /* Connectivity progress is shown by the Wi-Fi/cloud indicators.
             * Keep the face transport-specific so it cannot look like an
             * active cloud reasoning turn. */
            return NUWAX_EMOTION_DISCONNECTED_FACE;
        case NUWAX_APP_STATE_WS_CONNECTING:
            /* Wi-Fi is already usable and the local wake detector is armed
             * before cloud connection starts. Keep that wake-ready state
             * visible while the cloud indicator reports its own progress. */
            return NUWAX_EMOTION_SLEEPING_FACE;
        case NUWAX_APP_STATE_LISTENING:
            return NUWAX_EMOTION_FOCUSED;
        case NUWAX_APP_STATE_THINKING:
            return NUWAX_EMOTION_THINKING;
        case NUWAX_APP_STATE_SPEAKING:
            return NUWAX_EMOTION_HAPPY;
        case NUWAX_APP_STATE_ERROR:
            return NUWAX_EMOTION_SAD;
        case NUWAX_APP_STATE_SLEEPING:
            return NUWAX_EMOTION_SLEEPING_FACE;
        case NUWAX_APP_STATE_IDLE:
        default:
            return current;
    }
}

static nuwax_emotion_t policy_emotion_locked(nuwax_app_state_t app_state,
                                             nuwax_emotion_t current) {
    if (s_emotion_policy.touch_active) {
        return s_emotion_policy.interaction_emotion;
    }
    if (s_emotion_policy.semantic_valid) {
        return s_emotion_policy.semantic_emotion;
    }
    if (s_emotion_policy.waiting_question_active) {
        return NUWAX_EMOTION_QUESTION;
    }
    if (s_emotion_policy.pending_sleep_visual_active) {
        return NUWAX_EMOTION_SLEEPING_FACE;
    }
    return default_emotion_for_app_state(app_state, current);
}

static bool ascii_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static bool emotion_name_equals(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    while (ascii_space(*a)) {
        a++;
    }
    while (ascii_space(*b)) {
        b++;
    }
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);
    while (a_len > 0 && ascii_space(a[a_len - 1])) {
        a_len--;
    }
    while (b_len > 0 && ascii_space(b[b_len - 1])) {
        b_len--;
    }
    if (a_len != b_len) {
        return false;
    }
    for (size_t i = 0; i < a_len; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca == '-') {
            ca = '_';
        }
        if (cb == '-') {
            cb = '_';
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

static bool parse_emotion_name(const char *name, nuwax_emotion_t *out) {
    if (!name || !out) {
        return false;
    }
    if (emotion_name_equals(name, "neutral") || emotion_name_equals(name, "normal") ||
        emotion_name_equals(name, "idle")) {
        *out = NUWAX_EMOTION_NEUTRAL;
        return true;
    }
    if (emotion_name_equals(name, "happy") || emotion_name_equals(name, "smile")) {
        *out = NUWAX_EMOTION_HAPPY;
        return true;
    }
    if (emotion_name_equals(name, "surprised")) {
        *out = NUWAX_EMOTION_SURPRISED;
        return true;
    }
    if (emotion_name_equals(name, "wink")) {
        *out = NUWAX_EMOTION_WINK;
        return true;
    }
    if (emotion_name_equals(name, "tongue")) {
        *out = NUWAX_EMOTION_TONGUE;
        return true;
    }
    if (emotion_name_equals(name, "thinking") || emotion_name_equals(name, "think")) {
        *out = NUWAX_EMOTION_THINKING;
        return true;
    }
    if (emotion_name_equals(name, "focused")) {
        *out = NUWAX_EMOTION_FOCUSED;
        return true;
    }
    if (emotion_name_equals(name, "pensive")) {
        *out = NUWAX_EMOTION_PENSIVE;
        return true;
    }
    if (emotion_name_equals(name, "analyzing") || emotion_name_equals(name, "analysis")) {
        *out = NUWAX_EMOTION_ANALYZING;
        return true;
    }
    if (emotion_name_equals(name, "processing")) {
        *out = NUWAX_EMOTION_PROCESSING;
        return true;
    }
    if (emotion_name_equals(name, "received")) {
        *out = NUWAX_EMOTION_RECEIVED;
        return true;
    }
    if (emotion_name_equals(name, "what") || emotion_name_equals(name, "confused")) {
        *out = NUWAX_EMOTION_WHAT;
        return true;
    }
    if (emotion_name_equals(name, "understood") || emotion_name_equals(name, "ok")) {
        *out = NUWAX_EMOTION_UNDERSTOOD;
        return true;
    }
    if (emotion_name_equals(name, "awesome") || emotion_name_equals(name, "great")) {
        *out = NUWAX_EMOTION_AWESOME;
        return true;
    }
    if (emotion_name_equals(name, "like")) {
        *out = NUWAX_EMOTION_LIKE;
        return true;
    }
    if (emotion_name_equals(name, "sad") || emotion_name_equals(name, "sorry") ||
        emotion_name_equals(name, "error")) {
        *out = NUWAX_EMOTION_SAD;
        return true;
    }
    if (emotion_name_equals(name, "lost")) {
        *out = NUWAX_EMOTION_LOST;
        return true;
    }
    if (emotion_name_equals(name, "angry")) {
        *out = NUWAX_EMOTION_ANGRY;
        return true;
    }
    if (emotion_name_equals(name, "shy")) {
        *out = NUWAX_EMOTION_SHY;
        return true;
    }
    if (emotion_name_equals(name, "tired")) {
        *out = NUWAX_EMOTION_TIRED;
        return true;
    }
    if (emotion_name_equals(name, "dizziness") ||
        emotion_name_equals(name, "dizzy")) {
        *out = NUWAX_EMOTION_DIZZINESS;
        return true;
    }
    if (emotion_name_equals(name, "excited")) {
        *out = NUWAX_EMOTION_EXCITED;
        return true;
    }
    if (emotion_name_equals(name, "thought_complete") ||
        emotion_name_equals(name, "thinking_complete")) {
        *out = NUWAX_EMOTION_THOUGHT_COMPLETE;
        return true;
    }
    if (emotion_name_equals(name, "sleeping") ||
        emotion_name_equals(name, "sleep")) {
        *out = NUWAX_EMOTION_SLEEPING_FACE;
        return true;
    }
    if (emotion_name_equals(name, "provisioning") ||
        emotion_name_equals(name, "pairing")) {
        *out = NUWAX_EMOTION_PROVISIONING;
        return true;
    }
    if (emotion_name_equals(name, "disconnected") ||
        emotion_name_equals(name, "offline")) {
        *out = NUWAX_EMOTION_DISCONNECTED_FACE;
        return true;
    }
    if (emotion_name_equals(name, "meeting")) {
        *out = NUWAX_EMOTION_MEETING_FACE;
        return true;
    }
    if (emotion_name_equals(name, "monitoring") ||
        emotion_name_equals(name, "monitor")) {
        *out = NUWAX_EMOTION_MONITORING_FACE;
        return true;
    }
    if (emotion_name_equals(name, "touch") ||
        emotion_name_equals(name, "touched")) {
        *out = NUWAX_EMOTION_TOUCH;
        return true;
    }
    if (emotion_name_equals(name, "playful")) {
        *out = NUWAX_EMOTION_PLAYFUL;
        return true;
    }
    if (emotion_name_equals(name, "question")) {
        *out = NUWAX_EMOTION_QUESTION;
        return true;
    }
    return false;
}

bool nuwax_state_bridge_is_semantic_emotion_name_supported(
    const char *emotion) {
    nuwax_emotion_t parsed = NUWAX_EMOTION_NEUTRAL;
    return parse_emotion_name(emotion, &parsed);
}

void nuwax_state_bridge_invalidate_semantic_revisions_through(
    uint32_t owner_revision) {
    if (owner_revision == 0U) {
        return;
    }
    bool advanced = false;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    if (nuwax_ui_bridge_revision_is_newer(
            owner_revision, s_semantic_reject_through_revision)) {
        s_semantic_reject_through_revision = owner_revision;
        advanced = true;
    }
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    if (advanced) {
        ESP_LOGD(TAG,
                 "semantic revision fence advanced through=%lu",
                 (unsigned long)owner_revision);
    }
}

bool nuwax_state_bridge_set_semantic_emotion_name_owned(
    const char *emotion,
    const char *source,
    uint32_t owner_revision) {
    return nuwax_state_bridge_set_semantic_emotion_name_owned_leased(
        emotion, source, owner_revision, 0U, false);
}

bool nuwax_state_bridge_set_semantic_emotion_name_owned_leased(
    const char *emotion,
    const char *source,
    uint32_t owner_revision,
    uint32_t duration_ms,
    bool finite_lease) {
    nuwax_emotion_t parsed = NUWAX_EMOTION_NEUTRAL;
    if (!parse_emotion_name(emotion, &parsed)) {
        ESP_LOGW(TAG, "semantic emotion rejected name=%s", emotion ? emotion : "-");
        return false;
    }
    const uint32_t now_ms = bridge_now_ms();
    uint32_t generation = 0U;
    uint32_t bridge_revision = 0U;
    nuwax_emotion_display_mode_t mode = NUWAX_EMOTION_DISPLAY_NONE;
    uint32_t deadline_ms = 0U;
    bool interaction_active = false;
    nuwax_emotion_t interaction_emotion = NUWAX_EMOTION_NEUTRAL;
    bool applied = false;
    bool rejected_by_revision_fence = false;
    uint32_t reject_through_revision = 0U;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    reject_through_revision = s_semantic_reject_through_revision;
    rejected_by_revision_fence =
        nuwax_ui_bridge_owned_revision_is_rejected(
            owner_revision, reject_through_revision);
    if (!rejected_by_revision_fence) {
        ensure_policy_initialized_locked();
        applied = nuwax_emotion_display_policy_apply_cloud_leased(
            &s_emotion_policy, parsed, duration_ms, finite_lease,
            now_ms, &generation);
        mode = s_emotion_policy.semantic_mode;
        deadline_ms = s_emotion_policy.semantic_deadline_ms;
        interaction_active = s_emotion_policy.touch_active;
        interaction_emotion = s_emotion_policy.interaction_emotion;
        if (applied) {
            copy_semantic_owner_source(s_semantic_owner_source, source);
            s_semantic_owner_revision = owner_revision;
            bridge_revision = next_bridge_revision_locked();
        }
    }
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    if (rejected_by_revision_fence) {
        ESP_LOGI(TAG,
                 "stale semantic emotion rejected name=%s source=%s revision=%lu reject_through=%lu",
                 emotion_name(parsed), source ? source : "-",
                 (unsigned long)owner_revision,
                 (unsigned long)reject_through_revision);
        return false;
    }
    if (!applied) {
        ESP_LOGW(TAG,
                 "semantic emotion ignored name=%s source=%s generation=%lu active_mode=%d",
                 emotion_name(parsed), source ? source : "-",
                 (unsigned long)generation, (int)mode);
        return true;
    }
    nuwax_device_state_t s = state_snapshot();
    s.emotion = power_overrides_emotion(s.power)
                    ? emotion_for_power(s.power)
                    : interaction_active ? interaction_emotion : parsed;
    s.semantic_emotion_active = true;
    ESP_LOGI(TAG,
             "semantic emotion set name=%s source=%s generation=%lu mode=%d deadline_ms=%lu finite_lease=%d duration_ms=%lu",
             emotion_name(parsed),
             source ? source : "-",
             (unsigned long)generation,
             (int)mode,
             (unsigned long)deadline_ms,
             finite_lease ? 1 : 0,
             (unsigned long)duration_ms);
    publish_state_delta(&s,
                        NUWAX_APP_SHELL_STATE_EMOTION |
                            NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION,
                        bridge_revision);
    return true;
}

bool nuwax_state_bridge_set_semantic_emotion_name(const char *emotion,
                                                  const char *source) {
    return nuwax_state_bridge_set_semantic_emotion_name_owned(
        emotion, source, 0U);
}

static bool clear_semantic_emotion(const char *expected_source,
                                   uint32_t expected_revision,
                                   bool require_owner) {
    char owner_source[SEMANTIC_OWNER_SOURCE_CAPACITY];
    owner_source[0] = '\0';
    uint32_t owner_revision = 0U;
    nuwax_app_state_t app_state = NUWAX_APP_STATE_BOOTING;
    bool touch_active = false;
    nuwax_emotion_t visible_emotion = NUWAX_EMOTION_NEUTRAL;
    uint32_t bridge_revision = 0U;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    const bool source_matches = !require_owner ||
        (expected_source && expected_source[0] != '\0' &&
         strcmp(s_semantic_owner_source, expected_source) == 0);
    const bool revision_matches = !require_owner ||
        expected_revision == 0U ||
        s_semantic_owner_revision == expected_revision;
    if (!s_emotion_policy.semantic_valid || !source_matches ||
        !revision_matches) {
        taskEXIT_CRITICAL(&s_bridge_state_mux);
        return false;
    }
    copy_semantic_owner_source(owner_source, s_semantic_owner_source);
    owner_revision = s_semantic_owner_revision;
    s_emotion_policy.semantic_valid = false;
    s_emotion_policy.semantic_finite_lease = false;
    s_emotion_policy.semantic_mode = NUWAX_EMOTION_DISPLAY_NONE;
    s_emotion_policy.semantic_deadline_active = false;
    s_emotion_policy.semantic_deadline_ms = 0U;
    s_emotion_policy.semantic_paused_remaining_ms = 0U;
    s_emotion_policy.semantic_generation++;
    if (s_emotion_policy.semantic_generation == 0U) {
        s_emotion_policy.semantic_generation = 1U;
    }
    s_semantic_owner_source[0] = '\0';
    s_semantic_owner_revision = 0U;
    app_state = s_current_app_state;
    touch_active = s_emotion_policy.touch_active;
    visible_emotion = policy_emotion_locked(app_state,
                                             NUWAX_EMOTION_NEUTRAL);
    bridge_revision = next_bridge_revision_locked();
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    nuwax_device_state_t s = state_snapshot();
    s.semantic_emotion_active = false;
    s.touch_emotion_active = touch_active;
    s.emotion = visible_emotion;
    publish_state_delta(&s,
                        NUWAX_APP_SHELL_STATE_EMOTION |
                            NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION |
                            NUWAX_APP_SHELL_STATE_TOUCH_EMOTION,
                        bridge_revision);
    ESP_LOGI(TAG,
             "semantic emotion cleared source=%s owner_revision=%lu conditional=%d",
             owner_source[0] ? owner_source : "-",
             (unsigned long)owner_revision,
             require_owner ? 1 : 0);
    return true;
}

void nuwax_state_bridge_clear_semantic_emotion(void) {
    (void)clear_semantic_emotion(NULL, 0U, false);
}

bool nuwax_state_bridge_clear_semantic_emotion_if_owner(
    const char *source,
    uint32_t owner_revision) {
    return clear_semantic_emotion(source, owner_revision, true);
}

void nuwax_state_bridge_update(nuwax_app_state_t app_state) {
    bool semantic_emotion_valid = false;
    nuwax_emotion_t semantic_emotion = NUWAX_EMOTION_NEUTRAL;
    bool touch_active = false;
    bool waiting_question_active = false;
    bool pending_sleep_visual_active = false;
    nuwax_emotion_t interaction_emotion = NUWAX_EMOTION_NEUTRAL;
    bool playback_pcm_started = false;
    uint32_t bridge_revision = 0U;
    nuwax_dialog_state_t dialog = NUWAX_DIALOG_RECOGNIZING;
    const uint32_t now_ms = bridge_now_ms();
    /* Re-publishing the same lifecycle state is synchronization, not a new
     * lifecycle transition. Preserve a short semantic lease accepted after
     * LISTENING was entered (for example a no-TTS turn). */
    const bool lifecycle_changed = app_state != s_last_app_state;
    switch (app_state) {
        case NUWAX_APP_STATE_IDLE: dialog = NUWAX_DIALOG_READY; break;
        case NUWAX_APP_STATE_LISTENING: dialog = NUWAX_DIALOG_LISTENING; break;
        case NUWAX_APP_STATE_THINKING: dialog = NUWAX_DIALOG_THINKING; break;
        case NUWAX_APP_STATE_SPEAKING: dialog = NUWAX_DIALOG_SPEAKING; break;
        case NUWAX_APP_STATE_ERROR: dialog = NUWAX_DIALOG_ERROR; break;
        case NUWAX_APP_STATE_SLEEPING: dialog = NUWAX_DIALOG_SLEEP; break;
        default: dialog = NUWAX_DIALOG_RECOGNIZING; break;
    }
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    if (lifecycle_changed) {
        (void)nuwax_emotion_display_policy_enter_lifecycle(
            &s_emotion_policy, dialog, now_ms);
    }
    s_current_app_state = app_state;
    s_last_app_state = app_state;
    semantic_emotion_valid = s_emotion_policy.semantic_valid;
    semantic_emotion = s_emotion_policy.semantic_emotion;
    touch_active = s_emotion_policy.touch_active;
    waiting_question_active = s_emotion_policy.waiting_question_active;
    pending_sleep_visual_active =
        s_emotion_policy.pending_sleep_visual_active;
    interaction_emotion = s_emotion_policy.interaction_emotion;
    if (!semantic_emotion_valid) {
        s_semantic_owner_source[0] = '\0';
        s_semantic_owner_revision = 0U;
    }
    if (!semantic_emotion_valid && waiting_question_active) {
        semantic_emotion = NUWAX_EMOTION_QUESTION;
    }
    playback_pcm_started = s_emotion_policy.playback_pcm_started;
    bridge_revision = next_bridge_revision_locked();
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    nuwax_device_state_t s = state_snapshot();
    nuwax_app_shell_state_fields_t fields =
        NUWAX_APP_SHELL_STATE_EMOTION |
        NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION |
        NUWAX_APP_SHELL_STATE_DIALOG |
        NUWAX_APP_SHELL_STATE_APP_LOW_POWER |
        NUWAX_APP_SHELL_STATE_TOUCH_EMOTION |
        NUWAX_APP_SHELL_STATE_SPEAKING_PCM;
    s.app_low_power = false;
    s.semantic_emotion_active = semantic_emotion_valid;
    s.touch_emotion_active = touch_active;
    s.speaking_pcm_active = playback_pcm_started;
    s.emotion = current_or_neutral(s.emotion);
    s.emotion = default_emotion_for_app_state(app_state, s.emotion);
    switch (app_state) {
        case NUWAX_APP_STATE_BOOTING:
            s.dialog = dialog;
            s.wifi = NUWAX_WIFI_DISCONNECTED;
            s.cloud = NUWAX_CLOUD_DISCONNECTED;
            fields |= NUWAX_APP_SHELL_STATE_WIFI |
                      NUWAX_APP_SHELL_STATE_CLOUD;
            break;
        case NUWAX_APP_STATE_WIFI_CONNECTING:
            s.dialog = dialog;
            s.wifi = NUWAX_WIFI_CONNECTING;
            s.cloud = NUWAX_CLOUD_DISCONNECTED;
            fields |= NUWAX_APP_SHELL_STATE_WIFI |
                      NUWAX_APP_SHELL_STATE_CLOUD;
            break;
        case NUWAX_APP_STATE_WS_CONNECTING:
            s.dialog = dialog;
            s.cloud = NUWAX_CLOUD_CONNECTING;
            fields |= NUWAX_APP_SHELL_STATE_CLOUD;
            break;
        case NUWAX_APP_STATE_IDLE:
            s.dialog = dialog;
            break;
        case NUWAX_APP_STATE_LISTENING:
            s.dialog = dialog;
            break;
        case NUWAX_APP_STATE_THINKING:
            s.dialog = dialog;
            break;
        case NUWAX_APP_STATE_SPEAKING:
            s.dialog = dialog;
            break;
        case NUWAX_APP_STATE_ERROR:
            s.dialog = dialog;
            break;
        case NUWAX_APP_STATE_SLEEPING:
            s.dialog = dialog;
            s.app_low_power = true;
            break;
        default:
            s.dialog = dialog;
            break;
    }
    if (touch_active) {
        s.emotion = interaction_emotion;
    } else if (semantic_emotion_valid) {
        s.emotion = semantic_emotion;
    } else if (waiting_question_active) {
        s.emotion = NUWAX_EMOTION_QUESTION;
    } else if (pending_sleep_visual_active) {
        s.emotion = NUWAX_EMOTION_SLEEPING_FACE;
    } else if (power_has_default_emotion(s.power)) {
        s.emotion = emotion_for_power(s.power);
    }
    if (power_overrides_emotion(s.power)) {
        s.emotion = emotion_for_power(s.power);
    }
    /* A low battery is an alert only.  Application low-power means that the
     * conversation state machine explicitly entered sleep; battery state must
     * not suppress wake/audio operation or LVGL animations. */
    s.low_power = s.app_low_power;
    ESP_LOGI(TAG,
             "app_state=%s -> emotion=%s dialog=%s semantic=%d touch=%d pcm=%d wifi=%d cloud=%d low_power=%d",
             nuwax_app_state_name(app_state),
             emotion_name(s.emotion),
             dialog_name(s.dialog),
             s.semantic_emotion_active ? 1 : 0,
             s.touch_emotion_active ? 1 : 0,
             s.speaking_pcm_active ? 1 : 0,
             (int)s.wifi,
             (int)s.cloud,
             s.low_power ? 1 : 0);
    publish_app_state_delta(app_state, &s, fields, bridge_revision);
}

static void set_waiting_question(bool active) {
    bool changed = false;
    nuwax_emotion_t visible_emotion = NUWAX_EMOTION_NEUTRAL;
    bool semantic_valid = false;
    bool interaction_active = false;
    uint32_t bridge_revision = 0U;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    changed = active
                  ? nuwax_emotion_display_policy_start_waiting_question(
                        &s_emotion_policy)
                  : nuwax_emotion_display_policy_clear_waiting_question(
                        &s_emotion_policy);
    if (changed) {
        visible_emotion = policy_emotion_locked(
            s_current_app_state, NUWAX_EMOTION_NEUTRAL);
        semantic_valid = s_emotion_policy.semantic_valid;
        interaction_active = s_emotion_policy.touch_active;
        bridge_revision = next_bridge_revision_locked();
    }
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    if (!changed) return;

    nuwax_device_state_t s = state_snapshot();
    s.emotion = visible_emotion;
    s.semantic_emotion_active = semantic_valid;
    s.touch_emotion_active = interaction_active;
    publish_state_delta(&s,
                        NUWAX_APP_SHELL_STATE_EMOTION |
                            NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION |
                            NUWAX_APP_SHELL_STATE_TOUCH_EMOTION,
                        bridge_revision);
}

void nuwax_state_bridge_start_waiting_question(void) {
    set_waiting_question(true);
}

void nuwax_state_bridge_clear_waiting_question(void) {
    set_waiting_question(false);
}

static void set_pending_sleep_visual(bool active) {
    bool changed = false;
    nuwax_emotion_t visible_emotion = NUWAX_EMOTION_NEUTRAL;
    bool semantic_valid = false;
    bool interaction_active = false;
    uint32_t bridge_revision = 0U;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    changed = active
                  ? nuwax_emotion_display_policy_start_pending_sleep_visual(
                        &s_emotion_policy)
                  : nuwax_emotion_display_policy_clear_pending_sleep_visual(
                        &s_emotion_policy);
    if (changed) {
        visible_emotion = policy_emotion_locked(
            s_current_app_state, NUWAX_EMOTION_NEUTRAL);
        semantic_valid = s_emotion_policy.semantic_valid;
        interaction_active = s_emotion_policy.touch_active;
        bridge_revision = next_bridge_revision_locked();
    }
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    if (!changed) return;

    nuwax_device_state_t s = state_snapshot();
    s.emotion = visible_emotion;
    s.semantic_emotion_active = semantic_valid;
    s.touch_emotion_active = interaction_active;
    publish_state_delta(&s,
                        NUWAX_APP_SHELL_STATE_EMOTION |
                            NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION |
                            NUWAX_APP_SHELL_STATE_TOUCH_EMOTION,
                        bridge_revision);
}

void nuwax_state_bridge_start_pending_sleep_visual(void) {
    set_pending_sleep_visual(true);
}

void nuwax_state_bridge_clear_pending_sleep_visual(void) {
    set_pending_sleep_visual(false);
}

void nuwax_state_bridge_service(uint32_t now_ms) {
    bool changed = false;
    bool semantic_valid = false;
    bool touch_active = false;
    bool pcm_active = false;
    bool waiting_question_active = false;
    bool pending_sleep_visual_active = false;
    nuwax_emotion_t interaction_emotion = NUWAX_EMOTION_NEUTRAL;
    nuwax_emotion_t semantic_emotion = NUWAX_EMOTION_NEUTRAL;
    nuwax_app_state_t app_state = NUWAX_APP_STATE_BOOTING;
    uint32_t bridge_revision = 0U;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    changed = nuwax_emotion_display_policy_service(
        &s_emotion_policy, now_ms);
    semantic_valid = s_emotion_policy.semantic_valid;
    semantic_emotion = s_emotion_policy.semantic_emotion;
    touch_active = s_emotion_policy.touch_active;
    interaction_emotion = s_emotion_policy.interaction_emotion;
    waiting_question_active = s_emotion_policy.waiting_question_active;
    pending_sleep_visual_active =
        s_emotion_policy.pending_sleep_visual_active;
    pcm_active = s_emotion_policy.playback_pcm_started;
    app_state = s_current_app_state;
    if (changed && !semantic_valid) {
        s_semantic_owner_source[0] = '\0';
        s_semantic_owner_revision = 0U;
    }
    if (changed) {
        bridge_revision = next_bridge_revision_locked();
    }
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    if (!changed) return;

    nuwax_device_state_t s = state_snapshot();
    s.semantic_emotion_active = semantic_valid;
    s.emotion = touch_active
                    ? interaction_emotion
                    : semantic_valid
                          ? semantic_emotion
                          : waiting_question_active
                                ? NUWAX_EMOTION_QUESTION
                                : pending_sleep_visual_active
                                      ? NUWAX_EMOTION_SLEEPING_FACE
                                      : default_emotion_for_app_state(
                                            app_state,
                                            NUWAX_EMOTION_NEUTRAL);
    s.touch_emotion_active = touch_active;
    s.speaking_pcm_active = pcm_active;
    publish_state_delta(&s,
                        NUWAX_APP_SHELL_STATE_EMOTION |
                            NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION |
                            NUWAX_APP_SHELL_STATE_TOUCH_EMOTION |
                            NUWAX_APP_SHELL_STATE_SPEAKING_PCM,
                        bridge_revision);
    ESP_LOGI(TAG,
             "emotion deadline serviced semantic=%d emotion=%s touch=%d pcm=%d",
             semantic_valid ? 1 : 0,
             emotion_name(s.emotion),
             touch_active ? 1 : 0,
             pcm_active ? 1 : 0);
}

static void trigger_interaction(nuwax_emotion_t emotion,
                                uint32_t duration_ms,
                                const char *label) {
    bool semantic_valid = false;
    nuwax_emotion_t semantic_emotion = NUWAX_EMOTION_NEUTRAL;
    uint32_t generation = 0U;
    uint32_t bridge_revision = 0U;
    const uint32_t now_ms = bridge_now_ms();
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    generation = nuwax_emotion_display_policy_start_interaction(
        &s_emotion_policy, emotion, duration_ms, now_ms);
    semantic_valid = s_emotion_policy.semantic_valid;
    semantic_emotion = s_emotion_policy.semantic_emotion;
    if (!semantic_valid) {
        s_semantic_owner_source[0] = '\0';
        s_semantic_owner_revision = 0U;
    }
    bridge_revision = next_bridge_revision_locked();
    taskEXIT_CRITICAL(&s_bridge_state_mux);

    nuwax_device_state_t s = state_snapshot();
    s.touch_emotion_active = true;
    s.touch_sequence = generation;
    s.semantic_emotion_active = semantic_valid;
    s.emotion = emotion;
    publish_state_delta(&s,
                        NUWAX_APP_SHELL_STATE_TOUCH_EMOTION |
                            NUWAX_APP_SHELL_STATE_TOUCH_SEQUENCE |
                            NUWAX_APP_SHELL_STATE_EMOTION |
                            NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION,
                        bridge_revision);
    ESP_LOGI(TAG,
             "%s emotion inserted generation=%lu duration_ms=%u preserved=%s",
             label ? label : "interaction",
             (unsigned long)generation,
             (unsigned)duration_ms,
             semantic_valid ? emotion_name(semantic_emotion) : "none");
}

void nuwax_state_bridge_trigger_touch(void) {
    trigger_interaction(NUWAX_EMOTION_TOUCH,
                        NUWAX_EMOTION_TOUCH_OVERRIDE_DURATION_MS,
                        "touch");
}

void nuwax_state_bridge_trigger_shake(void) {
    trigger_interaction(NUWAX_EMOTION_DIZZINESS,
                        NUWAX_EMOTION_SHAKE_OVERRIDE_DURATION_MS,
                        "shake");
}

static void pin_interaction(nuwax_emotion_t emotion, const char *label) {
    uint32_t generation = 0U;
    uint32_t bridge_revision = 0U;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    generation = nuwax_emotion_display_policy_pin_interaction(
        &s_emotion_policy, emotion);
    s_semantic_owner_source[0] = '\0';
    s_semantic_owner_revision = 0U;
    bridge_revision = next_bridge_revision_locked();
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    if (generation == 0U) return;

    nuwax_device_state_t s = state_snapshot();
    s.touch_emotion_active = true;
    s.touch_sequence = generation;
    s.semantic_emotion_active = false;
    s.emotion = emotion;
    publish_state_delta(&s,
                        NUWAX_APP_SHELL_STATE_TOUCH_EMOTION |
                            NUWAX_APP_SHELL_STATE_TOUCH_SEQUENCE |
                            NUWAX_APP_SHELL_STATE_EMOTION |
                            NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION,
                        bridge_revision);
    ESP_LOGI(TAG,
             "%s emotion pinned generation=%lu until owned round completes",
             label ? label : "interaction",
             (unsigned long)generation);
}

void nuwax_state_bridge_pin_touch(void) {
    pin_interaction(NUWAX_EMOTION_TOUCH, "touch");
}

void nuwax_state_bridge_pin_shake(void) {
    pin_interaction(NUWAX_EMOTION_DIZZINESS, "shake");
}

void nuwax_state_bridge_cancel_touch(void) {
    bool changed = false;
    bool semantic_valid = false;
    nuwax_emotion_t visible_emotion = NUWAX_EMOTION_NEUTRAL;
    uint32_t bridge_revision = 0U;
    const uint32_t now_ms = bridge_now_ms();
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    changed = nuwax_emotion_display_policy_cancel_touch(&s_emotion_policy,
                                                         now_ms);
    if (changed) {
        semantic_valid = s_emotion_policy.semantic_valid;
        visible_emotion = policy_emotion_locked(
            s_current_app_state, NUWAX_EMOTION_NEUTRAL);
        bridge_revision = next_bridge_revision_locked();
    }
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    if (!changed) return;
    nuwax_device_state_t s = state_snapshot();
    s.touch_emotion_active = false;
    s.semantic_emotion_active = semantic_valid;
    s.emotion = visible_emotion;
    publish_state_delta(&s,
                        NUWAX_APP_SHELL_STATE_TOUCH_EMOTION |
                            NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION |
                            NUWAX_APP_SHELL_STATE_EMOTION,
                        bridge_revision);
}

bool nuwax_state_bridge_special_semantic_active(void) {
    bool active = false;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    active = nuwax_emotion_display_policy_special_semantic_active(
        &s_emotion_policy);
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    return active;
}

void nuwax_state_bridge_note_playback_pcm_started(uint32_t connection_epoch,
                                                   uint32_t generation) {
    bool changed = false;
    uint32_t bridge_revision = 0U;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    changed = nuwax_emotion_display_policy_note_first_pcm(
        &s_emotion_policy, connection_epoch, generation);
    if (changed) {
        bridge_revision = next_bridge_revision_locked();
    }
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    if (!changed) return;
    nuwax_device_state_t s = state_snapshot();
    s.speaking_pcm_active = true;
    publish_state_delta(&s, NUWAX_APP_SHELL_STATE_SPEAKING_PCM,
                        bridge_revision);
}

void nuwax_state_bridge_note_playback_stopped(uint32_t connection_epoch,
                                              uint32_t generation) {
    bool changed = false;
    uint32_t bridge_revision = 0U;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    changed = nuwax_emotion_display_policy_stop_playback(
        &s_emotion_policy, connection_epoch, generation);
    if (changed) {
        bridge_revision = next_bridge_revision_locked();
    }
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    if (!changed) return;
    nuwax_device_state_t s = state_snapshot();
    s.speaking_pcm_active = false;
    publish_state_delta(&s, NUWAX_APP_SHELL_STATE_SPEAKING_PCM,
                        bridge_revision);
}

void nuwax_state_bridge_clear_playback(void) {
    bool changed = false;
    uint32_t bridge_revision = 0U;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    changed = nuwax_emotion_display_policy_clear_playback(&s_emotion_policy);
    if (changed) {
        bridge_revision = next_bridge_revision_locked();
    }
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    if (!changed) return;
    nuwax_device_state_t s = state_snapshot();
    s.speaking_pcm_active = false;
    publish_state_delta(&s, NUWAX_APP_SHELL_STATE_SPEAKING_PCM,
                        bridge_revision);
}

void nuwax_state_bridge_refresh_power(void) {
    bool semantic_emotion_valid = false;
    nuwax_emotion_t semantic_emotion = NUWAX_EMOTION_NEUTRAL;
    nuwax_app_state_t last_app_state = NUWAX_APP_STATE_BOOTING;
    uint32_t bridge_revision = 0U;
    taskENTER_CRITICAL(&s_bridge_state_mux);
    ensure_policy_initialized_locked();
    semantic_emotion_valid = s_emotion_policy.semantic_valid;
    semantic_emotion = s_emotion_policy.semantic_emotion;
    last_app_state = s_last_app_state;
    bridge_revision = next_bridge_revision_locked();
    taskEXIT_CRITICAL(&s_bridge_state_mux);
    nuwax_device_state_t s = state_snapshot();
    if (power_overrides_emotion(s.power)) {
        s.emotion = emotion_for_power(s.power);
    } else if (!semantic_emotion_valid &&
               power_has_default_emotion(s.power)) {
        s.emotion = emotion_for_power(s.power);
    } else if (s.emotion == NUWAX_EMOTION_LOW_BATTERY ||
               s.emotion == NUWAX_EMOTION_CHARGING) {
        s.emotion = semantic_emotion_valid
                        ? semantic_emotion
                        : default_emotion_for_app_state(last_app_state,
                                                        NUWAX_EMOTION_NEUTRAL);
    }
    s.low_power = s.app_low_power;
    publish_state_delta(&s, NUWAX_APP_SHELL_STATE_EMOTION,
                        bridge_revision);
}
