#include "nuwax_face.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "face/emotion_animation_player.h"
#include "face/conversation_visual.h"
#include "nuwax_ui_config.h"

static const char *TAG = "nuwax_face";

#define FACE_WIDTH 360
#define FACE_HEIGHT 360
#define FAILSAFE_EYE_WIDTH 68
#define FAILSAFE_EYE_HEIGHT 18
#define FAILSAFE_EYE_Y 154
#define FAILSAFE_LEFT_EYE_X 76
#define FAILSAFE_RIGHT_EYE_X 216
#define FAILSAFE_MOUTH_WIDTH 56
#define FAILSAFE_MOUTH_HEIGHT 8
#define FAILSAFE_MOUTH_X 152
#define FAILSAFE_MOUTH_Y 218

typedef enum {
    FACE_BACKEND_NONE = 0,
    FACE_BACKEND_ANIMATED,
    FACE_BACKEND_FAILSAFE,
} face_backend_t;

static face_backend_t s_backend;
static lv_obj_t *s_parent;
static lv_obj_t *s_failsafe_root;
static nuwax_emotion_asset_id_t s_last_asset = NUWAX_EMOTION_ASSET_HAPPY;
static nuwax_emotion_asset_id_t s_automatic_asset =
    NUWAX_EMOTION_ASSET_HAPPY;
static uint32_t s_last_touch_sequence;
static bool s_semantic_touch_latched;
static bool s_touch_override_active;
static bool s_manual_preview_active;
static bool s_provisioning_boot_visual;
static bool s_boot_asset_pending;
static uint8_t s_frame_recovery_failures;
static int64_t s_last_frame_recovery_us;
static int64_t s_frame_recovery_blocked_until_us;
static bool s_interaction_active;
static bool s_overlay_covered;
static bool s_state_seen;
static bool s_player_paused;
/* Local fallback only: never publish a synthetic cloud/semantic emotion. */
static nuwax_device_state_t s_latest_state;
static bool s_speaking_fallback_eligible;
static bool s_speaking_fallback_active;
static bool s_speaking_cloud_seen;
static nuwax_emotion_asset_id_t s_speaking_asset;
static int64_t s_speaking_deadline_us;
static int64_t s_interaction_started_us;

static void reset_speaking_fallback(void)
{
    s_speaking_fallback_eligible = false;
    s_speaking_fallback_active = false;
    s_speaking_cloud_seen = false;
    s_speaking_deadline_us = 0;
}

static void update_speaking_fallback(const nuwax_device_state_t *state)
{
    if (state->dialog != NUWAX_DIALOG_SPEAKING) {
        s_speaking_cloud_seen = false;
    } else if (state->semantic_emotion_active) {
        /* Once cloud-controlled, leave the rest of this playback to its policy. */
        s_speaking_cloud_seen = true;
    }
    s_speaking_fallback_eligible =
        !s_provisioning_boot_visual && !s_manual_preview_active &&
        !s_speaking_cloud_seen && !state->semantic_emotion_active &&
        nuwax_conversation_visual_for_state(state) ==
            NUWAX_CONVERSATION_VISUAL_SPEAKING;
    if (!s_speaking_fallback_eligible) {
        s_speaking_fallback_active = false;
        s_speaking_deadline_us = 0;
        return;
    }
    if (s_interaction_active) return;
    const int64_t now_us = esp_timer_get_time();
    if (s_speaking_fallback_active && now_us < s_speaking_deadline_us) return;

    /* Independent draws, inclusive 3000..7000 ms. Repeating an asset keeps
     * its animation running rather than restarting the same expression. */
    s_speaking_asset = (esp_random() & 1U) ? NUWAX_EMOTION_ASSET_PLAYFUL
                                         : NUWAX_EMOTION_ASSET_HAPPY;
    const uint32_t hold_ms = 3000U + esp_random() % 4001U;
    s_speaking_deadline_us = now_us + (int64_t)hold_ms * 1000LL;
    s_speaking_fallback_active = true;
    ESP_LOGI(TAG, "speaking fallback emotion=%s hold_ms=%lu",
             s_speaking_asset == NUWAX_EMOTION_ASSET_PLAYFUL ? "playful" : "happy",
             (unsigned long)hold_ms);
}

static void sync_player_pause(void)
{
    const bool paused=s_interaction_active || s_overlay_covered;
    if (s_backend==FACE_BACKEND_ANIMATED && paused!=s_player_paused) {
        nuwax_emotion_animation_player_set_interaction_active(paused);
        s_player_paused=paused;
    }
}

#define FACE_FRAME_RECOVERY_INTERVAL_US 500000LL
#define FACE_FRAME_RECOVERY_MAX_ATTEMPTS 3U
#define FACE_FRAME_RECOVERY_BACKOFF_US 5000000LL
#if CONFIG_NUWAX_FACE_TEST_MODE_ENABLE
static bool s_test_mode_active;
static int64_t s_test_mode_next_us;
#endif

static nuwax_emotion_asset_id_t semantic_asset(nuwax_emotion_t emotion)
{
    switch (emotion) {
        case NUWAX_EMOTION_DIZZINESS:
        case NUWAX_EMOTION_SAD:
        case NUWAX_EMOTION_TIRED:
            return NUWAX_EMOTION_ASSET_DIZZINESS;
        case NUWAX_EMOTION_EXCITED:
        case NUWAX_EMOTION_SURPRISED:
        case NUWAX_EMOTION_AWESOME:
            return NUWAX_EMOTION_ASSET_EXCITED;
        case NUWAX_EMOTION_THINKING:
        case NUWAX_EMOTION_FOCUSED:
        case NUWAX_EMOTION_PENSIVE:
        case NUWAX_EMOTION_ANALYZING:
        case NUWAX_EMOTION_PROCESSING:
            return NUWAX_EMOTION_ASSET_THINKING;
        case NUWAX_EMOTION_THOUGHT_COMPLETE:
        case NUWAX_EMOTION_RECEIVED:
        case NUWAX_EMOTION_UNDERSTOOD:
            return NUWAX_EMOTION_ASSET_THOUGHT_COMPLETE;
        case NUWAX_EMOTION_SLEEPING_FACE:
            return NUWAX_EMOTION_ASSET_SLEEPING;
        case NUWAX_EMOTION_PROVISIONING:
            return NUWAX_EMOTION_ASSET_PROVISIONING;
        case NUWAX_EMOTION_DISCONNECTED_FACE:
            return NUWAX_EMOTION_ASSET_DISCONNECTED;
        case NUWAX_EMOTION_MEETING_FACE:
            return NUWAX_EMOTION_ASSET_MEETING;
        case NUWAX_EMOTION_MONITORING_FACE:
            return NUWAX_EMOTION_ASSET_MONITORING;
        case NUWAX_EMOTION_TOUCH:
            return NUWAX_EMOTION_ASSET_TOUCH;
        case NUWAX_EMOTION_PLAYFUL:
        case NUWAX_EMOTION_WINK:
        case NUWAX_EMOTION_TONGUE:
        case NUWAX_EMOTION_SHY:
            return NUWAX_EMOTION_ASSET_PLAYFUL;
        case NUWAX_EMOTION_LIKE:
            return NUWAX_EMOTION_ASSET_LIKE;
        case NUWAX_EMOTION_ANGRY:
            return NUWAX_EMOTION_ASSET_ANGRY;
        case NUWAX_EMOTION_LISTENING:
            return NUWAX_EMOTION_ASSET_LISTENING;
        case NUWAX_EMOTION_SPEAKING:
            return NUWAX_EMOTION_ASSET_HAPPY;
        case NUWAX_EMOTION_QUESTION:
        case NUWAX_EMOTION_WHAT:
        case NUWAX_EMOTION_LOST:
            return NUWAX_EMOTION_ASSET_QUESTION;
        case NUWAX_EMOTION_NEUTRAL:
        case NUWAX_EMOTION_HAPPY:
        case NUWAX_EMOTION_BOOTING:
        case NUWAX_EMOTION_LOW_BATTERY:
        case NUWAX_EMOTION_CHARGING:
        default:
            return NUWAX_EMOTION_ASSET_HAPPY;
    }
}

static bool touch_visual_allowed(const nuwax_device_state_t *state)
{
    if (!state || state->provisioning_active || state->provisioning_starting ||
        state->wifi == NUWAX_WIFI_DISCONNECTED ||
        state->wifi == NUWAX_WIFI_CONNECTING ||
        state->cloud == NUWAX_CLOUD_AUTHFAIL ||
        state->monitoring != NUWAX_MONITORING_UI_DISABLED ||
        state->meeting != NUWAX_MEETING_UI_DISABLED) {
        return false;
    }
    /* A current physical touch may acknowledge the user locally in sleep.
     * A coalesced/expired touch or a shake cannot revive an old expression. */
    return (!state->app_low_power && state->dialog != NUWAX_DIALOG_SLEEP) ||
           (state->touch_emotion_active && state->emotion == NUWAX_EMOTION_TOUCH);
}

static nuwax_emotion_asset_id_t asset_for_state(
    const nuwax_device_state_t *state)
{
    if (s_provisioning_boot_visual) {
        return NUWAX_EMOTION_ASSET_PROVISIONING;
    }
    if (state->provisioning_active || state->provisioning_starting) {
        return NUWAX_EMOTION_ASSET_PROVISIONING;
    }
    if (state->monitoring != NUWAX_MONITORING_UI_DISABLED) {
        return NUWAX_EMOTION_ASSET_MONITORING;
    }
    if (state->meeting != NUWAX_MEETING_UI_DISABLED) {
        return NUWAX_EMOTION_ASSET_MEETING;
    }
    /* Connectivity is a visual owner, not a conversation lifecycle change.
     * A preserved sleep transaction must not hide an unplugged hotspot or
     * rejected cloud credentials. Offline recording keeps its mode above. */
    if (state->cloud == NUWAX_CLOUD_AUTHFAIL ||
        state->wifi == NUWAX_WIFI_CONNECTING ||
        state->wifi == NUWAX_WIFI_DISCONNECTED) {
        return NUWAX_EMOTION_ASSET_DISCONNECTED;
    }
    if (state->touch_emotion_active && state->emotion == NUWAX_EMOTION_TOUCH &&
        touch_visual_allowed(state)) {
        return NUWAX_EMOTION_ASSET_TOUCH;
    }
    if (state->dialog == NUWAX_DIALOG_SLEEP &&
        state->semantic_emotion_active) {
        return semantic_asset(state->emotion);
    }
    if (state->app_low_power || state->dialog == NUWAX_DIALOG_SLEEP) {
        return NUWAX_EMOTION_ASSET_SLEEPING;
    }
    /* A local touch acknowledgement remains useful while connectivity is
     * recovering. It is visual-only and expires through the display policy. */
    if (state->touch_emotion_active && touch_visual_allowed(state)) {
        return state->emotion == NUWAX_EMOTION_DIZZINESS
                   ? NUWAX_EMOTION_ASSET_DIZZINESS
                   : NUWAX_EMOTION_ASSET_TOUCH;
    }
    if (state->dialog == NUWAX_DIALOG_LISTENING) {
        return NUWAX_EMOTION_ASSET_LISTENING;
    }
    /* Cloud expressions are visual only. They override normal conversation
     * defaults, but never the real modes, connectivity or listening owners
     * above. */
    if (state->semantic_emotion_active) {
        return semantic_asset(state->emotion);
    }
    if (s_speaking_fallback_active) return s_speaking_asset;
    return semantic_asset(state->emotion);
}

static void reset_frame_recovery_guard(void)
{
    s_frame_recovery_failures = 0U;
    s_last_frame_recovery_us = 0;
    s_frame_recovery_blocked_until_us = 0;
}

static void style_failsafe_part(lv_obj_t *part, int32_t width,
                                int32_t height, int32_t x, int32_t y)
{
    lv_obj_remove_style_all(part);
    lv_obj_set_size(part, width, height);
    lv_obj_set_pos(part, x, y);
    lv_obj_set_style_bg_color(part, lv_color_hex(0x7de5ef), 0);
    lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(part, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(part, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

static bool create_failsafe_face(void)
{
    if (!s_parent) {
        return false;
    }
    s_failsafe_root = lv_obj_create(s_parent);
    if (!s_failsafe_root) {
        return false;
    }
    lv_obj_remove_style_all(s_failsafe_root);
    lv_obj_set_size(s_failsafe_root, FACE_WIDTH, FACE_HEIGHT);
    lv_obj_set_pos(s_failsafe_root, 0, 0);
    lv_obj_set_style_bg_color(s_failsafe_root, lv_color_hex(0x02070b), 0);
    lv_obj_set_style_bg_opa(s_failsafe_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_failsafe_root,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(s_failsafe_root, 0);

    lv_obj_t *left_eye = lv_obj_create(s_failsafe_root);
    lv_obj_t *right_eye = lv_obj_create(s_failsafe_root);
    lv_obj_t *mouth = lv_obj_create(s_failsafe_root);
    if (!left_eye || !right_eye || !mouth) {
        lv_obj_delete(s_failsafe_root);
        s_failsafe_root = NULL;
        return false;
    }
    style_failsafe_part(left_eye, FAILSAFE_EYE_WIDTH,
                        FAILSAFE_EYE_HEIGHT, FAILSAFE_LEFT_EYE_X,
                        FAILSAFE_EYE_Y);
    style_failsafe_part(right_eye, FAILSAFE_EYE_WIDTH,
                        FAILSAFE_EYE_HEIGHT, FAILSAFE_RIGHT_EYE_X,
                        FAILSAFE_EYE_Y);
    style_failsafe_part(mouth, FAILSAFE_MOUTH_WIDTH,
                        FAILSAFE_MOUTH_HEIGHT, FAILSAFE_MOUTH_X,
                        FAILSAFE_MOUTH_Y);
    return true;
}

static void activate_failsafe(esp_err_t cause, const char *stage)
{
    nuwax_conversation_visual_delete();
    s_player_paused=false;
    if (s_backend == FACE_BACKEND_ANIMATED) {
        const esp_err_t cleanup_err =
            nuwax_emotion_animation_player_deinit();
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG, "animated cleanup failed stage=%s err=%s",
                     stage ? stage : "-", esp_err_to_name(cleanup_err));
        }
    }
    s_backend = FACE_BACKEND_FAILSAFE;
    if (!s_failsafe_root && !create_failsafe_face()) {
        ESP_LOGE(TAG,
                 "emotion UI failed stage=%s err=%s; static failsafe allocation failed",
                 stage ? stage : "-", esp_err_to_name(cause));
        return;
    }
    ESP_LOGE(TAG,
             "emotion UI failed stage=%s err=%s; static failsafe active",
             stage ? stage : "-", esp_err_to_name(cause));
}

esp_err_t nuwax_face_create(lv_obj_t *parent)
{
    return nuwax_face_create_with_initial_asset(
        parent, NUWAX_EMOTION_ASSET_HAPPY);
}

esp_err_t nuwax_face_create_with_initial_asset(
    lv_obj_t *parent, nuwax_emotion_asset_id_t initial_asset)
{
    if (!parent || s_backend != FACE_BACKEND_NONE || s_parent) {
        return parent ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    }
    s_parent = parent;
    s_state_seen = false;
    s_overlay_covered = false;
    s_last_asset = initial_asset;
    s_automatic_asset = initial_asset;
    s_last_touch_sequence = 0;
    s_semantic_touch_latched = false;
    s_touch_override_active = false;
    s_manual_preview_active = false;
    s_provisioning_boot_visual =
        initial_asset == NUWAX_EMOTION_ASSET_PROVISIONING;
    s_boot_asset_pending = true;
    reset_speaking_fallback();
    reset_frame_recovery_guard();
#if CONFIG_NUWAX_FACE_TEST_MODE_ENABLE
    s_test_mode_active = false;
    s_test_mode_next_us = 0;
#endif

    const esp_err_t err =
        nuwax_emotion_animation_player_init_with_initial_asset(
            parent, initial_asset);
    if (err == ESP_OK) {
        s_backend = FACE_BACKEND_ANIMATED;
        const esp_err_t visual_err=nuwax_conversation_visual_create(parent);
        if (visual_err!=ESP_OK) {
            ESP_LOGW(TAG,"conversation visual unavailable err=%s; retaining original face",
                     esp_err_to_name(visual_err));
        } else {
            ESP_LOGI(TAG,"conversation visual active overlay_only=1 ripple=listening bars=playback text=0 fps=20 snapshot_psram_bytes=259200");
        }
        ESP_LOGI(TAG, "verified animated emotion backend active");
    } else {
        activate_failsafe(err, "animation_startup");
    }
    /* Face failure must not suppress status, menu, touch or the audio system. */
    return ESP_OK;
}

void nuwax_face_clear_provisioning_boot_visual(void)
{
    s_provisioning_boot_visual = false;
}

esp_err_t nuwax_face_deinit(void)
{
    esp_err_t err = ESP_OK;
    if (s_backend == FACE_BACKEND_ANIMATED) {
        err = nuwax_emotion_animation_player_deinit();
    } else if (s_backend == FACE_BACKEND_FAILSAFE) {
        err = nuwax_emotion_animation_player_reap();
        if (err == ESP_OK && s_failsafe_root) {
            lv_obj_delete(s_failsafe_root);
            s_failsafe_root = NULL;
        }
    }
    if (err == ESP_OK) {
        nuwax_conversation_visual_delete();
        s_interaction_active=false;
        s_overlay_covered=false;
        s_state_seen=false;
        s_player_paused=false;
        s_backend = FACE_BACKEND_NONE;
        s_parent = NULL;
        s_provisioning_boot_visual = false;
        s_last_asset = NUWAX_EMOTION_ASSET_HAPPY;
        s_automatic_asset = NUWAX_EMOTION_ASSET_HAPPY;
        s_last_touch_sequence = 0;
        s_semantic_touch_latched = false;
        s_touch_override_active = false;
        s_manual_preview_active = false;
        s_boot_asset_pending = false;
        reset_speaking_fallback();
        reset_frame_recovery_guard();
#if CONFIG_NUWAX_FACE_TEST_MODE_ENABLE
        s_test_mode_active = false;
        s_test_mode_next_us = 0;
#endif
    }
    return err;
}

void nuwax_face_update(const nuwax_device_state_t *state)
{
    if (!state || s_backend != FACE_BACKEND_ANIMATED) {
        return;
    }
#if CONFIG_NUWAX_FACE_TEST_MODE_ENABLE
    if (s_test_mode_active) {
        return;
    }
#endif

    s_latest_state = *state;
    s_state_seen = true;
    /* Retain authoritative state while covered; apply the latest on return. */
    if (s_overlay_covered) return;
    update_speaking_fallback(state);
    const nuwax_emotion_asset_id_t asset = asset_for_state(state);
    const bool touch_changed =
        state->touch_sequence != s_last_touch_sequence;
    if (s_manual_preview_active) {
        if (!touch_changed && asset == s_automatic_asset) {
            return;
        }
        s_manual_preview_active = false;
        ESP_LOGI(TAG,
                 "manual emotion preview released reason=%s target=%u",
                 touch_changed ? "touch" : "state_change",
                 (unsigned)asset);
    }
    s_automatic_asset = asset;
    if (state->emotion != NUWAX_EMOTION_TOUCH) {
        s_semantic_touch_latched = false;
    }
    esp_err_t err = ESP_OK;
    const bool touch_allowed = touch_visual_allowed(state);
    const bool touch_visible = state->touch_emotion_active && touch_allowed &&
                               asset == NUWAX_EMOTION_ASSET_TOUCH;
    if (touch_visible) {
        if (!s_touch_override_active) {
            s_touch_override_active = true;
            s_semantic_touch_latched = true;
            err = nuwax_emotion_animation_player_trigger_touch();
        } else if (touch_changed) {
            err = nuwax_emotion_animation_player_trigger_touch();
        }
    } else if (s_touch_override_active) {
        /* The touch asset is longer than the product's two-second insertion
         * window. Explicitly cancel it at the policy deadline. */
        s_touch_override_active = false;
        s_semantic_touch_latched = false;
        err = nuwax_emotion_animation_player_set_asset(asset);
        if (err == ESP_OK) {
            s_last_asset = asset;
            reset_frame_recovery_guard();
        }
    } else if (asset == NUWAX_EMOTION_ASSET_TOUCH) {
        if (!s_semantic_touch_latched) {
            s_semantic_touch_latched = true;
            err = nuwax_emotion_animation_player_set_asset(asset);
            if (err == ESP_OK) {
                s_last_asset = asset;
                reset_frame_recovery_guard();
            }
        }
    } else if (asset != s_last_asset) {
        err = s_boot_asset_pending
                  ? nuwax_emotion_animation_player_set_asset_immediate(asset)
                  : nuwax_emotion_animation_player_set_asset(asset);
        if (err == ESP_OK) {
            s_last_asset = asset;
            reset_frame_recovery_guard();
        }
    }
    if (err == ESP_OK && touch_changed) {
        s_last_touch_sequence = state->touch_sequence;
    }
    if (err == ESP_OK) {
        s_boot_asset_pending = false;
        nuwax_conversation_visual_request(s_provisioning_boot_visual
            ? NUWAX_CONVERSATION_VISUAL_NONE
            : nuwax_conversation_visual_for_state(state));
    }
    if (err != ESP_OK) {
        activate_failsafe(err, "state_update");
    }
}

void nuwax_face_service_decode(void)
{
    if (s_backend == FACE_BACKEND_ANIMATED)
        nuwax_emotion_animation_player_service_decode();
}

void nuwax_face_tick(void)
{
    if (s_backend == FACE_BACKEND_FAILSAFE) {
        (void)nuwax_emotion_animation_player_reap();
        return;
    }
    if (s_backend != FACE_BACKEND_ANIMATED) {
        return;
    }
    if (s_overlay_covered) return;
    nuwax_conversation_visual_tick();
    sync_player_pause();
#if CONFIG_NUWAX_FACE_TEST_MODE_ENABLE
    if (s_test_mode_active) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us >= s_test_mode_next_us) {
            if (!nuwax_face_next_expression()) {
                return;
            }
            s_test_mode_next_us =
                now_us + (int64_t)CONFIG_NUWAX_FACE_TEST_MODE_INTERVAL_MS *
                             1000LL;
        }
    }
#endif
    const int64_t now_us = esp_timer_get_time();
    if (s_speaking_fallback_eligible && !s_interaction_active &&
        !s_manual_preview_active &&
        (!s_speaking_fallback_active || now_us >= s_speaking_deadline_us)) {
        nuwax_face_update(&s_latest_state);
        if (s_backend != FACE_BACKEND_ANIMATED) return;
    }
    if (s_frame_recovery_blocked_until_us != 0 &&
        now_us < s_frame_recovery_blocked_until_us) {
        return;
    }
    const esp_err_t err = nuwax_emotion_animation_player_tick();
    if (err != ESP_OK) {
        /* A single bad frame must not permanently replace the animated face
         * with the blue static failsafe.  Keep the last committed image on
         * screen and rate-limit a clean timeline restart; persistent failures
         * remain visible in diagnostics without spinning the LVGL task. */
        if (s_last_frame_recovery_us == 0 ||
            now_us - s_last_frame_recovery_us >=
                FACE_FRAME_RECOVERY_INTERVAL_US) {
            s_last_frame_recovery_us = now_us;
            if (s_frame_recovery_failures < UINT8_MAX) {
                s_frame_recovery_failures++;
            }
            const esp_err_t recovery_err =
                nuwax_emotion_animation_player_recover();
            if (recovery_err == ESP_OK) {
                ESP_LOGW(TAG,
                         "emotion frame timeline restarted attempt=%u tick_err=%s",
                         (unsigned)s_frame_recovery_failures,
                         esp_err_to_name(err));
            } else {
                ESP_LOGW(TAG,
                         "emotion frame recovery deferred failures=%u tick_err=%s recovery_err=%s",
                         (unsigned)s_frame_recovery_failures,
                         esp_err_to_name(err),
                         esp_err_to_name(recovery_err));
            }
            if (s_frame_recovery_failures >= FACE_FRAME_RECOVERY_MAX_ATTEMPTS) {
                s_frame_recovery_blocked_until_us =
                    now_us + FACE_FRAME_RECOVERY_BACKOFF_US;
                ESP_LOGE(TAG,
                         "emotion frame recovery circuit open attempts=%u backoff_ms=%u; retaining last committed frame",
                         (unsigned)s_frame_recovery_failures,
                         (unsigned)(FACE_FRAME_RECOVERY_BACKOFF_US / 1000LL));
            }
        }
        return;
    }
    reset_frame_recovery_guard();
}

void nuwax_face_set_menu_open(bool open)
{
    if (open != s_interaction_active) {
        const int64_t now_us = esp_timer_get_time();
        if (open) {
            s_interaction_started_us = now_us;
        } else if (s_speaking_fallback_active) {
            s_speaking_deadline_us += now_us - s_interaction_started_us;
        }
    }
    s_interaction_active=open;
    nuwax_conversation_visual_freeze(open || s_overlay_covered);
    if (s_backend == FACE_BACKEND_ANIMATED) {
        sync_player_pause();
    }
}

void nuwax_face_set_overlay_covered(bool covered)
{
    if (covered == s_overlay_covered) return;
    s_overlay_covered = covered;
    nuwax_conversation_visual_freeze(covered || s_interaction_active);
    sync_player_pause();
    if (!covered && s_state_seen) nuwax_face_update(&s_latest_state);
}

esp_err_t nuwax_face_freeze_interaction(void)
{
    if (s_backend != FACE_BACKEND_ANIMATED) return ESP_ERR_INVALID_STATE;
    const esp_err_t err=nuwax_emotion_animation_player_freeze();
    if (err==ESP_OK) {
        if (!s_interaction_active) s_interaction_started_us=esp_timer_get_time();
        s_interaction_active=true;
        s_player_paused=true;
        nuwax_conversation_visual_freeze(true);
    }
    return err;
}

void nuwax_face_set_interaction_active(bool active)
{
    nuwax_face_set_menu_open(active);
}

bool nuwax_face_frozen_frame_ready(void)
{
    return s_backend == FACE_BACKEND_ANIMATED &&
           nuwax_emotion_animation_player_frozen_frame_ready();
}

const uint16_t *nuwax_face_frozen_rgb565(size_t *stride_pixels)
{
    if (stride_pixels) *stride_pixels = 0U;
    if (s_backend != FACE_BACKEND_ANIMATED) return NULL;
    size_t stride=0;
    const uint16_t *base=nuwax_emotion_animation_player_frozen_rgb565(&stride);
    const uint16_t *composed=nuwax_conversation_visual_frozen_rgb565(base,stride);
    if (stride_pixels) *stride_pixels=composed ? FACE_WIDTH : stride;
    return composed ? composed : base;
}

bool nuwax_face_draw_frozen(void *layer)
{
    if (s_backend != FACE_BACKEND_ANIMATED) return false;
    size_t stride=0;
    const uint16_t *base=nuwax_emotion_animation_player_frozen_rgb565(&stride);
    if (nuwax_conversation_visual_draw_frozen(layer,base,stride)) return true;
    return s_backend == FACE_BACKEND_ANIMATED &&
           nuwax_emotion_animation_player_draw_frozen(layer);
}

void nuwax_face_set_overlay_progress(uint16_t progress_permille)
{
    if (s_backend == FACE_BACKEND_ANIMATED) {
        nuwax_emotion_animation_player_set_overlay_progress(
            progress_permille > 1000U ? 1000U : progress_permille);
    }
}

bool nuwax_face_next_expression(void)
{
    if (s_backend != FACE_BACKEND_ANIMATED) {
        return false;
    }
    static const nuwax_emotion_asset_id_t sequence[] = {
        NUWAX_EMOTION_ASSET_HAPPY,
        NUWAX_EMOTION_ASSET_EXCITED,
        NUWAX_EMOTION_ASSET_SLEEPING,
        NUWAX_EMOTION_ASSET_PROVISIONING,
        NUWAX_EMOTION_ASSET_DISCONNECTED,
        NUWAX_EMOTION_ASSET_THINKING,
        NUWAX_EMOTION_ASSET_THOUGHT_COMPLETE,
        NUWAX_EMOTION_ASSET_MEETING,
        NUWAX_EMOTION_ASSET_MONITORING,
        NUWAX_EMOTION_ASSET_TOUCH,
        NUWAX_EMOTION_ASSET_PLAYFUL,
        NUWAX_EMOTION_ASSET_LIKE,
        NUWAX_EMOTION_ASSET_ANGRY,
        NUWAX_EMOTION_ASSET_LISTENING,
        NUWAX_EMOTION_ASSET_QUESTION,
        NUWAX_EMOTION_ASSET_DIZZINESS,
    };
    size_t next = 0;
    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        if (sequence[i] == s_last_asset) {
            next = (i + 1U) % (sizeof(sequence) / sizeof(sequence[0]));
            break;
        }
    }
    const esp_err_t err =
        nuwax_emotion_animation_player_set_asset(sequence[next]);
    if (err == ESP_OK) {
        s_last_asset = sequence[next];
        s_manual_preview_active = true;
        nuwax_conversation_visual_request(NUWAX_CONVERSATION_VISUAL_NONE);
        ESP_LOGI(TAG, "manual emotion preview asset=%u",
                 (unsigned)s_last_asset);
        return true;
    }
    activate_failsafe(err, "manual_expression");
    return false;
}

void nuwax_face_start_test_mode(void)
{
#if CONFIG_NUWAX_FACE_TEST_MODE_ENABLE
    if (s_backend != FACE_BACKEND_ANIMATED) {
        ESP_LOGW(TAG, "emotion test carousel unavailable backend=%d",
                 (int)s_backend);
        return;
    }
    s_test_mode_active = true;
    s_test_mode_next_us =
        esp_timer_get_time() +
        (int64_t)CONFIG_NUWAX_FACE_TEST_MODE_INTERVAL_MS * 1000LL;
    ESP_LOGW(TAG, "emotion test carousel active interval_ms=%u",
             (unsigned)CONFIG_NUWAX_FACE_TEST_MODE_INTERVAL_MS);
#else
    ESP_LOGW(TAG, "emotion test carousel is disabled in this build");
#endif
}
