#include "conversation/head_touch_control.h"

#include <stdint.h>

#include "app_shell.h"
#include "nuwax_ui_v28/nuwax_state_bridge.h"
#include "board/board_gpio_inputs.h"
#include "board/shake_detector.h"
#include "conversation/head_touch_control_policy.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "nuwax_head_touch";

#ifndef CONFIG_NUWAX_HEAD_TOUCH_LONG_PRESS_MS
#define CONFIG_NUWAX_HEAD_TOUCH_LONG_PRESS_MS 1000U
#endif

#ifndef CONFIG_NUWAX_HEAD_TOUCH_GAP_TOLERANCE_MS
#define CONFIG_NUWAX_HEAD_TOUCH_GAP_TOLERANCE_MS 750U
#endif

#ifndef CONFIG_NUWAX_HEAD_TOUCH_MAX_VALID_DURATION_MS
#define CONFIG_NUWAX_HEAD_TOUCH_MAX_VALID_DURATION_MS 5000U
#endif

#ifndef CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE
#define CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE
#define CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE 0
#endif

#if defined(CONFIG_NUWAX_HEAD_TOUCH_ENABLE) && \
    CONFIG_NUWAX_HEAD_TOUCH_ENABLE
static nuwax_head_touch_state_cb_t s_get_state;
static nuwax_head_touch_allowed_cb_t s_interaction_allowed;
static void *s_state_user_ctx;
static bool s_press_started_listen;
static bool s_press_started_agent;
static bool s_press_claimed_gate;
static bool s_press_started_in_speaking;

static bool head_touch_interaction_allowed(void)
{
    return (!s_interaction_allowed ||
            s_interaction_allowed(s_state_user_ctx)) &&
           nuwax_app_shell_can_start_listen();
}

static void head_touch_event_callback(const nuwax_head_touch_event_t *event,
                                      void *user_ctx)
{
    (void)user_ctx;
    if (!event) {
        return;
    }

    switch (event->type) {
        case NUWAX_HEAD_TOUCH_EVENT_PRESS: {
            s_press_started_listen = false;
            s_press_started_agent = false;
            s_press_claimed_gate = false;
            s_press_started_in_speaking =
                s_get_state &&
                s_get_state(s_state_user_ctx) == NUWAX_APP_STATE_SPEAKING;
            ESP_LOGI(TAG,
                     "event=press action=pending qualification_ms=%u",
                     (unsigned)CONFIG_NUWAX_HEAD_TOUCH_LONG_PRESS_MS);
            break;
        }
        case NUWAX_HEAD_TOUCH_EVENT_LONG_PRESS: {
            if (!nuwax_shake_detector_claim_head_touch()) {
                s_press_started_listen = false;
                s_press_started_agent = false;
                s_press_claimed_gate = false;
                ESP_LOGI(TAG,
                         "event=qualified duration_ms=%llu action=none reason=interaction_active_or_cooldown",
                         (unsigned long long)event->duration_ms);
                break;
            }
            s_press_started_listen = false;
            s_press_started_agent = false;
            s_press_claimed_gate = true;
            const nuwax_app_state_t state = s_get_state
                                                ? s_get_state(s_state_user_ctx)
                                                : NUWAX_APP_STATE_BOOTING;
            nuwax_head_touch_action_t action =
                nuwax_head_touch_action_for_state(
                    state,
                    head_touch_interaction_allowed(),
                    CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE != 0,
                    CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE != 0);
            if (action == NUWAX_HEAD_TOUCH_ACTION_AGENT_TOUCH &&
                state == NUWAX_APP_STATE_LISTENING &&
                s_press_started_in_speaking) {
                action = NUWAX_HEAD_TOUCH_ACTION_VISUAL_ONLY;
                ESP_LOGI(TAG,
                         "qualified touch began during completed playback; cloud text suppressed");
            }
            /* Visual feedback is independent of wake/interrupt policy. */
            nuwax_app_shell_trigger_touch_animation();
            if (action == NUWAX_HEAD_TOUCH_ACTION_START_LISTEN) {
                ESP_LOGI(TAG,
                         "event=qualified duration_ms=%llu state=%s action=start_listen",
                         (unsigned long long)event->duration_ms,
                         nuwax_app_state_name(state));
                s_press_started_listen = nuwax_app_shell_request_action(
                    NUWAX_APP_SHELL_ACTION_START_LISTEN, 0);
                if (!s_press_started_listen) {
                    ESP_LOGW(TAG,
                             "event=qualified duration_ms=%llu state=%s action=start_listen rejected reason=shell_action_queue",
                             (unsigned long long)event->duration_ms,
                             nuwax_app_state_name(state));
                    nuwax_shake_detector_release_head_touch();
                    s_press_claimed_gate = false;
                }
            } else if (action == NUWAX_HEAD_TOUCH_ACTION_CANCEL_TURN) {
                s_press_started_agent = false;
                ESP_LOGI(TAG,
                         "event=qualified duration_ms=%llu state=%s action=cancel_turn",
                         (unsigned long long)event->duration_ms,
                         nuwax_app_state_name(state));
                if (!nuwax_app_shell_request_action(
                        NUWAX_APP_SHELL_ACTION_CANCEL, 0)) {
                    ESP_LOGW(TAG,
                             "event=qualified duration_ms=%llu state=%s action=cancel_turn rejected reason=shell_action_queue",
                             (unsigned long long)event->duration_ms,
                             nuwax_app_state_name(state));
                    nuwax_shake_detector_release_head_touch();
                    s_press_claimed_gate = false;
                }
            } else if (action == NUWAX_HEAD_TOUCH_ACTION_AGENT_TOUCH) {
                ESP_LOGI(TAG,
                         "event=qualified duration_ms=%llu state=%s action=head_touch_agent",
                         (unsigned long long)event->duration_ms,
                         nuwax_app_state_name(state));
                s_press_started_agent = nuwax_app_shell_request_action(
                    NUWAX_APP_SHELL_ACTION_HEAD_TOUCH_AGENT, 0);
                if (!s_press_started_agent) {
                    ESP_LOGW(TAG,
                             "event=qualified duration_ms=%llu state=%s action=head_touch_agent rejected reason=shell_action_queue",
                             (unsigned long long)event->duration_ms,
                             nuwax_app_state_name(state));
                    nuwax_shake_detector_release_head_touch();
                    s_press_claimed_gate = false;
                } else {
                    /* The conversation transaction now owns the shared gate;
                     * this physical press must not release it on finger-up. */
                    s_press_claimed_gate = false;
                }
            } else if (action == NUWAX_HEAD_TOUCH_ACTION_VISUAL_ONLY) {
                s_press_started_listen = false;
                s_press_started_agent = false;
                nuwax_shake_detector_release_head_touch();
                s_press_claimed_gate = false;
                ESP_LOGI(TAG,
                         "event=qualified duration_ms=%llu state=%s action=visual_only",
                         (unsigned long long)event->duration_ms,
                         nuwax_app_state_name(state));
            } else {
                s_press_started_listen = false;
                s_press_started_agent = false;
                nuwax_shake_detector_release_head_touch();
                s_press_claimed_gate = false;
                ESP_LOGI(TAG,
                         "event=qualified duration_ms=%llu state=%s action=none",
                         (unsigned long long)event->duration_ms,
                         nuwax_app_state_name(state));
            }
            if (action != NUWAX_HEAD_TOUCH_ACTION_AGENT_TOUCH) {
                s_press_started_agent = false;
            }
            break;
        }
        case NUWAX_HEAD_TOUCH_EVENT_RELEASE:
            if (s_press_started_listen) {
                ESP_LOGI(TAG,
                         "event=release duration_ms=%llu action=stop_listen",
                         (unsigned long long)event->duration_ms);
                if (!nuwax_app_shell_request_action(
                        NUWAX_APP_SHELL_ACTION_STOP_LISTEN, -1)) {
                    ESP_LOGW(TAG,
                             "event=release duration_ms=%llu action=stop_listen rejected reason=shell_action_queue",
                             (unsigned long long)event->duration_ms);
                }
            } else {
                ESP_LOGI(TAG,
                         "event=release duration_ms=%llu action=none",
                         (unsigned long long)event->duration_ms);
            }
            s_press_started_listen = false;
            if (s_press_claimed_gate) {
                nuwax_shake_detector_release_head_touch();
            }
            s_press_claimed_gate = false;
            s_press_started_agent = false;
            s_press_started_in_speaking = false;
            break;
        case NUWAX_HEAD_TOUCH_EVENT_EXPIRED:
            ESP_LOGW(TAG,
                     "event=expired duration_ms=%llu max_valid_ms=%u action=none",
                     (unsigned long long)event->duration_ms,
                     (unsigned)CONFIG_NUWAX_HEAD_TOUCH_MAX_VALID_DURATION_MS);
            if (s_press_started_listen) {
                if (nuwax_app_shell_request_action(
                        NUWAX_APP_SHELL_ACTION_STOP_LISTEN, -1)) {
                    ESP_LOGI(TAG,
                             "event=expired duration_ms=%llu action=stop_listen reason=touch_timeout",
                             (unsigned long long)event->duration_ms);
                } else {
                    ESP_LOGW(TAG,
                             "event=expired duration_ms=%llu action=stop_listen rejected reason=shell_action_queue",
                             (unsigned long long)event->duration_ms);
                }
            }
            s_press_started_listen = false;
            s_press_started_in_speaking = false;
            if (s_press_claimed_gate) {
                nuwax_shake_detector_release_head_touch();
                s_press_claimed_gate = false;
            } else {
                if (s_press_started_agent) {
                    ESP_LOGI(TAG,
                             "event=expired duration_ms=%llu action=retain_agent_interaction_gate",
                             (unsigned long long)event->duration_ms);
                }
            }
            break;
        default:
            ESP_LOGW(TAG, "ignored unknown event=%d", (int)event->type);
            break;
    }
}

esp_err_t nuwax_head_touch_control_init(nuwax_head_touch_state_cb_t get_state,
                                        nuwax_head_touch_allowed_cb_t interaction_allowed,
                                        void *user_ctx)
{
    if (!get_state) {
        return ESP_ERR_INVALID_ARG;
    }
    const nuwax_head_touch_config_t config = {
        .long_press_ms = CONFIG_NUWAX_HEAD_TOUCH_LONG_PRESS_MS,
        .gap_tolerance_ms = CONFIG_NUWAX_HEAD_TOUCH_GAP_TOLERANCE_MS,
        .max_valid_duration_ms = CONFIG_NUWAX_HEAD_TOUCH_MAX_VALID_DURATION_MS,
    };
    esp_err_t err = nuwax_head_touch_set_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "invalid product configuration: %s",
                 esp_err_to_name(err));
        return err;
    }

    s_get_state = get_state;
    s_interaction_allowed = interaction_allowed;
    s_state_user_ctx = user_ctx;
    s_press_started_listen = false;
    s_press_started_agent = false;
    s_press_started_in_speaking = false;
    s_press_claimed_gate = false;
    err = nuwax_head_touch_set_event_callback(head_touch_event_callback, NULL);
    if (err != ESP_OK) {
        s_get_state = NULL;
        s_interaction_allowed = NULL;
        s_state_user_ctx = NULL;
        ESP_LOGE(TAG, "callback registration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = nuwax_board_gpio_inputs_init(NULL, NULL);
    if (err != ESP_OK) {
        (void)nuwax_head_touch_set_event_callback(NULL, NULL);
        s_get_state = NULL;
        s_interaction_allowed = NULL;
        s_state_user_ctx = NULL;
        ESP_LOGE(TAG, "GPIO input initialization failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "product control ready qualification_ms=%u gap_tolerance_ms=%u max_valid_ms=%u cooldown_ms=%u wake_action=%d interrupt_action=%d",
             (unsigned)config.long_press_ms,
             (unsigned)config.gap_tolerance_ms,
             (unsigned)config.max_valid_duration_ms,
             (unsigned)NUWAX_HEAD_TOUCH_INTERACTION_COOLDOWN_MS,
             CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE ? 1 : 0,
             CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE ? 1 : 0);
    return ESP_OK;
}
#else
esp_err_t nuwax_head_touch_control_init(nuwax_head_touch_state_cb_t get_state,
                                        nuwax_head_touch_allowed_cb_t interaction_allowed,
                                        void *user_ctx)
{
    (void)get_state;
    (void)interaction_allowed;
    (void)user_ctx;
    ESP_LOGI(TAG, "product control disabled by configuration");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
