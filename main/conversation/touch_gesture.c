#include "touch_gesture.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_shell.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nuwax_gesture.h"
#include "touch_input.h"

static const char *TAG = "NuwaxPartner6";

#define NUWAX_HOLD_TO_TALK_START_MS 180
#define NUWAX_HOLD_TO_TALK_MOVE_LIMIT_PX 28
#define NUWAX_TOUCH_SCROLL_STEP_PX 56
#define NUWAX_TOUCH_DETAIL_DRAG_START_PX 18
#define NUWAX_HOME_SWIPE_EARLY_PX 44
#define NUWAX_TOUCH_SCROLL_MIN_INTERVAL_MS 120
#define NUWAX_TOUCH_MENU_DRAG_START_PX 18
#define NUWAX_TOUCH_MENU_DRAG_AXIS_BIAS_NUM 5
#define NUWAX_TOUCH_MENU_DRAG_AXIS_BIAS_DEN 4
#define NUWAX_TOUCH_MENU_DRAG_INTERVAL_MS 16
#define NUWAX_TOUCH_DRAWER_DRAG_INTERVAL_MS 16
#define NUWAX_TOUCH_PAGE_DRAG_START_PX 8
#define NUWAX_TOUCH_PAGE_DRAG_AXIS_BIAS_NUM 112
#define NUWAX_TOUCH_PAGE_DRAG_AXIS_BIAS_DEN 100
#define NUWAX_TOUCH_PAGE_DRAG_COMMIT_PX 47
#define NUWAX_TOUCH_PAGE_DRAG_VELOCITY_X1000 340
#define NUWAX_TOUCH_DRAWER_COMMIT_PX 47
#define NUWAX_TOUCH_DRAWER_VELOCITY_X1000 340
#define NUWAX_TOUCH_DRAWER_VOLUME_PREVIEW_MS 90
#define NUWAX_TOUCH_RELEASE_GUARD_MS 260
#define NUWAX_VERY_LONG_PRESS_MS 5000

typedef struct {
    nuwax_touch_point_t start;
    nuwax_touch_point_t last;
    bool active;
    bool hold_to_talk_started;
    bool hold_cancel_dispatched;
    bool hold_blocked_logged;
    bool drag_dispatched;
    bool home_swipe_dispatched;
    bool menu_drag_active;
    bool drawer_opened_by_drag;
    bool drawer_panel_drag_active;
    bool drawer_page_drag_active;
    bool drawer_volume_drag_active;
    int drawer_volume_pending;
    int drawer_volume_last_preview;
    int drawer_panel_drag_start_y;
    int drawer_panel_drag_base_y;
    int64_t last_drag_dispatch_us;
    int64_t last_menu_drag_us;
    int64_t last_drawer_panel_drag_us;
    int64_t last_drawer_page_drag_us;
    int64_t last_volume_preview_us;
    int64_t start_us;
} touch_session_t;

static QueueHandle_t s_shell_actions;
static nuwax_touch_gesture_volume_preview_cb_t s_volume_preview_cb;

static void restore_confirmed_volume_preview(void) {
    if (!s_volume_preview_cb) {
        return;
    }
    nuwax_device_state_t state = {0};
    if (nuwax_app_shell_get_device_state_snapshot(&state, NULL)) {
        s_volume_preview_cb(state.volume);
    }
}

void nuwax_touch_gesture_set_action_queue(QueueHandle_t queue) {
    s_shell_actions = queue;
}

void nuwax_touch_gesture_set_volume_preview_callback(nuwax_touch_gesture_volume_preview_cb_t callback) {
    s_volume_preview_cb = callback;
}

static void reset_touch_session(touch_session_t *session) {
    if (!session) {
        return;
    }
    memset(session, 0, sizeof(*session));
    session->drawer_volume_pending = -1;
    session->drawer_volume_last_preview = -1;
}

static void begin_touch_session(touch_session_t *session, const nuwax_touch_point_t *point) {
    reset_touch_session(session);
    session->active = true;
    session->start = *point;
    session->last = *point;
    session->start_us = esp_timer_get_time();
}

static int abs_int(int value) {
    return value < 0 ? -value : value;
}

static int64_t touch_session_age_ms(const touch_session_t *session, int64_t now_us) {
    return (now_us - session->start_us) / 1000;
}

static nuwax_app_shell_event_t classify_touch(const nuwax_touch_point_t *start,
                                              const nuwax_touch_point_t *end,
                                              int64_t duration_ms) {
    int dx = (int)end->x - (int)start->x;
    int dy = (int)end->y - (int)start->y;
    int abs_dx = abs_int(dx);
    int abs_dy = abs_int(dy);
    if (abs_dx >= 50 && abs_dx > abs_dy) {
        return dx < 0 ? NUWAX_APP_SHELL_EVENT_SWIPE_LEFT : NUWAX_APP_SHELL_EVENT_SWIPE_RIGHT;
    }
    if (abs_dy >= 50 && abs_dy > abs_dx) {
        return dy < 0 ? NUWAX_APP_SHELL_EVENT_SWIPE_UP : NUWAX_APP_SHELL_EVENT_SWIPE_DOWN;
    }
    if (duration_ms >= NUWAX_VERY_LONG_PRESS_MS) {
        return NUWAX_APP_SHELL_EVENT_VERY_LONG_PRESS;
    }
    if (duration_ms >= 900) {
        return NUWAX_APP_SHELL_EVENT_LONG_PRESS;
    }
    return NUWAX_APP_SHELL_EVENT_TAP;
}

static void queue_shell_action(const nuwax_app_shell_result_t *result) {
    if (!result || result->action == NUWAX_APP_SHELL_ACTION_NONE) {
        return;
    }
    if (!s_shell_actions || xQueueSend(s_shell_actions, result, 0) != pdTRUE) {
        ESP_LOGW(TAG, "shell action queue unavailable action=%d",
                 (int)result->action);
        nuwax_app_shell_notify_action_rejected(result->action);
        if (result->action == NUWAX_APP_SHELL_ACTION_SET_VOLUME) {
            restore_confirmed_volume_preview();
        }
        return;
    }
    if (result->action == NUWAX_APP_SHELL_ACTION_SET_VOLUME) {
        nuwax_app_shell_set_volume_percent(result->index);
    }
}

static void preview_volume_percent(int volume_percent) {
    if (s_volume_preview_cb) {
        s_volume_preview_cb(volume_percent);
    }
}

static void cancel_touch_session(touch_session_t *session) {
    if (!session || !session->active) {
        return;
    }
    if (session->hold_to_talk_started) {
        const nuwax_app_shell_result_t result = {
            .action = NUWAX_APP_SHELL_ACTION_STOP_LISTEN,
            .index = -1,
        };
        queue_shell_action(&result);
    }
    if (session->menu_drag_active) {
        nuwax_app_shell_menu_finish_drag();
    }
    if (session->drawer_panel_drag_active) {
        nuwax_app_shell_drawer_finish_drag(session->drawer_opened_by_drag);
    }
    if (session->drawer_page_drag_active) {
        nuwax_app_shell_drawer_page_finish_drag(false);
    }
    if (session->drawer_volume_drag_active) {
        nuwax_app_shell_notify_action_rejected(
            NUWAX_APP_SHELL_ACTION_SET_VOLUME);
        restore_confirmed_volume_preview();
    }
    ESP_LOGW(TAG,
             "touch session cancelled active_listen=%d menu_drag=%d drawer_drag=%d page_drag=%d",
             session->hold_to_talk_started ? 1 : 0,
             session->menu_drag_active ? 1 : 0,
             session->drawer_panel_drag_active ? 1 : 0,
             session->drawer_page_drag_active ? 1 : 0);
    reset_touch_session(session);
}

static void finish_drawer_release_guard(touch_session_t *session, int64_t now_us) {
    const int total_dy = (int)session->last.y - (int)session->start.y;
    const int drawer_y = (int)nuwax_app_shell_drawer_current_y();
    const int64_t age_ms = touch_session_age_ms(session, now_us);
    const int velocity_y_x1000 = age_ms > 0
                                     ? total_dy * 1000 / age_ms
                                     : 0;
    const bool open_commit = total_dy >= NUWAX_TOUCH_DRAWER_COMMIT_PX ||
                             velocity_y_x1000 >=
                                 NUWAX_TOUCH_DRAWER_VELOCITY_X1000;
    const bool close_drawer = session->drawer_opened_by_drag
                                  ? !open_commit
                                  : (total_dy < -64 || drawer_y < -58);
    nuwax_app_shell_drawer_finish_drag(close_drawer);
    ESP_LOGI(TAG,
             "touch drawer release guard close=%d y=%d dy=%d velocity_x1000=%d age_ms=%lld",
             close_drawer ? 1 : 0,
             drawer_y,
             total_dy,
             velocity_y_x1000,
             (long long)age_ms);
    reset_touch_session(session);
}

static void finish_menu_release_guard(touch_session_t *session, int64_t now_us) {
    const int dy = (int)session->last.y - (int)session->start.y;
    nuwax_app_shell_menu_finish_drag();
    ESP_LOGI(TAG,
             "touch menu release guard dy=%d age_ms=%lld",
             dy,
             (long long)touch_session_age_ms(session, now_us));
    reset_touch_session(session);
}

static void finish_page_release_guard(touch_session_t *session,
                                      int64_t now_us) {
    const int dx = (int)session->last.x - (int)session->start.x;
    const int64_t age_ms = touch_session_age_ms(session, now_us);
    const int velocity_x1000 = age_ms > 0 ? abs_int(dx) * 1000 / age_ms : 0;
    const bool commit = abs_int(dx) >= NUWAX_TOUCH_PAGE_DRAG_COMMIT_PX ||
                        velocity_x1000 >=
                            NUWAX_TOUCH_PAGE_DRAG_VELOCITY_X1000;
    nuwax_app_shell_drawer_page_finish_drag(commit);
    ESP_LOGI(TAG,
             "touch page release guard commit=%d dx=%d velocity_x1000=%d age_ms=%lld",
             commit ? 1 : 0, dx, velocity_x1000, (long long)age_ms);
    reset_touch_session(session);
}

static void handle_release_guard_timeout(touch_session_t *session) {
    if (!session->active) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if (touch_session_age_ms(session, now_us) < NUWAX_TOUCH_RELEASE_GUARD_MS) {
        return;
    }
    if (session->drawer_panel_drag_active) {
        finish_drawer_release_guard(session, now_us);
    } else if (session->drawer_page_drag_active) {
        finish_page_release_guard(session, now_us);
    } else if (session->menu_drag_active) {
        finish_menu_release_guard(session, now_us);
    }
}

static void reset_after_listen_release(touch_session_t *session) {
    session->hold_to_talk_started = false;
    session->hold_blocked_logged = false;
    session->drag_dispatched = false;
    session->home_swipe_dispatched = false;
    session->menu_drag_active = false;
    session->last_drag_dispatch_us = 0;
    session->last_menu_drag_us = 0;
}

static void reset_after_drag_release(touch_session_t *session) {
    session->drag_dispatched = false;
    session->home_swipe_dispatched = false;
    session->menu_drag_active = false;
    session->drawer_panel_drag_active = false;
    session->drawer_page_drag_active = false;
    session->drawer_volume_drag_active = false;
    session->drawer_volume_pending = -1;
    session->drawer_volume_last_preview = -1;
    session->drawer_panel_drag_start_y = 0;
    session->drawer_panel_drag_base_y = 0;
    session->hold_blocked_logged = false;
    session->last_drag_dispatch_us = 0;
    session->last_menu_drag_us = 0;
    session->last_drawer_panel_drag_us = 0;
    session->last_drawer_page_drag_us = 0;
    session->last_volume_preview_us = 0;
}

void nuwax_touch_gesture_task(void *arg) {
    (void)arg;
    touch_session_t session;
    reset_touch_session(&session);

    while (true) {
        nuwax_touch_point_t point = {0};
        if (nuwax_touch_poll_event(&point, 100) != ESP_OK) {
            handle_release_guard_timeout(&session);
            /* Init/recovery cooldowns return immediately. Yield explicitly so
             * a missing touch controller cannot starve Wi-Fi or OTA startup. */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (point.cancelled) {
            cancel_touch_session(&session);
            continue;
        }
        if (point.touched && !session.active) {
            begin_touch_session(&session, &point);
            continue;
        }
        if (point.touched && session.active) {
            session.last = point;
            if (!session.hold_to_talk_started &&
                !session.hold_cancel_dispatched) {
                int dx = (int)session.last.x - (int)session.start.x;
                int dy = (int)session.last.y - (int)session.start.y;
                int abs_dx = abs_int(dx);
                int abs_dy = abs_int(dy);
                int64_t duration_ms = touch_session_age_ms(&session, esp_timer_get_time());
                bool shell_owns_touch = nuwax_app_shell_is_menu_active();
                if (!shell_owns_touch &&
                    !session.drawer_opened_by_drag &&
                    session.start.y < NUWAX_GESTURE_TOP_BAND &&
                    dy >= NUWAX_GESTURE_DRAWER_DY &&
                    abs_dy > abs_dx) {
                    nuwax_app_shell_drawer_begin_pull();
                    session.drawer_opened_by_drag = true;
                    session.drawer_panel_drag_active = true;
                    session.drawer_panel_drag_start_y = (int)session.start.y;
                    session.drawer_panel_drag_base_y = -226;
                    session.drag_dispatched = true;
                    const int target_y = -226 + dy;
                    nuwax_app_shell_drawer_drag_to(
                        (int16_t)(target_y > 48 ? 48 : target_y));
                    shell_owns_touch = true;
                    ESP_LOGD(TAG,
                             "touch drawer pull open start x=%u y=%u last x=%u y=%u",
                             session.start.x,
                             session.start.y,
                             session.last.x,
                             session.last.y);
                }
                if (session.home_swipe_dispatched) {
                    continue;
                }
                if (!shell_owns_touch &&
                    !session.drawer_opened_by_drag &&
                    !session.drag_dispatched &&
                    dx < 0 &&
                    abs_dx >= NUWAX_HOME_SWIPE_EARLY_PX &&
                    abs_dx > abs_dy) {
                    nuwax_app_shell_result_t result =
                        nuwax_app_shell_handle_touch_event(NUWAX_APP_SHELL_EVENT_SWIPE_LEFT, &session.last);
                    queue_shell_action(&result);
                    session.drag_dispatched = true;
                    session.home_swipe_dispatched = true;
                    session.hold_blocked_logged = false;
                    ESP_LOGD(TAG,
                             "touch home swipe early event=%d dx=%d dy=%d x=%u y=%u",
                             (int)NUWAX_APP_SHELL_EVENT_SWIPE_LEFT,
                             dx,
                             dy,
                             session.last.x,
                             session.last.y);
                    continue;
                }
                if (shell_owns_touch) {
                    if (nuwax_app_shell_drawer_is_open()) {
                        int64_t now_us = esp_timer_get_time();
                        if (!session.drawer_panel_drag_active &&
                            !session.drawer_page_drag_active) {
                            if (session.drawer_volume_drag_active &&
                                session.last_volume_preview_us != 0 &&
                                (now_us - session.last_volume_preview_us) /
                                        1000 <
                                    NUWAX_TOUCH_DRAWER_VOLUME_PREVIEW_MS) {
                                continue;
                            }
                            uint8_t volume = 0;
                            if (nuwax_app_shell_drawer_preview_volume_at(
                                    session.last.x, session.last.y, &volume)) {
                                if (session.drawer_volume_last_preview !=
                                    (int)volume) {
                                    preview_volume_percent((int)volume);
                                    session.drawer_volume_last_preview =
                                        (int)volume;
                                }
                                session.drawer_volume_drag_active = true;
                                session.drawer_volume_pending = (int)volume;
                                session.last_volume_preview_us = now_us;
                                session.drag_dispatched = true;
                                continue;
                            }
                        }
                        if (!session.drawer_volume_drag_active &&
                            !session.drawer_page_drag_active &&
                            !nuwax_app_shell_drawer_is_detail()) {
                            if (!session.drawer_panel_drag_active &&
                                abs_dx >= NUWAX_TOUCH_PAGE_DRAG_START_PX &&
                                abs_dx * NUWAX_TOUCH_PAGE_DRAG_AXIS_BIAS_DEN >
                                    abs_dy * NUWAX_TOUCH_PAGE_DRAG_AXIS_BIAS_NUM) {
                                session.drawer_page_drag_active = true;
                                session.drag_dispatched = true;
                            }
                        }
                        if (session.drawer_page_drag_active) {
                            if (session.last_drawer_page_drag_us != 0 &&
                                (now_us - session.last_drawer_page_drag_us) /
                                        1000 <
                                    NUWAX_TOUCH_MENU_DRAG_INTERVAL_MS) {
                                continue;
                            }
                            int page_delta_x = dx;
                            if (page_delta_x > 120) page_delta_x = 120;
                            if (page_delta_x < -120) page_delta_x = -120;
                            nuwax_app_shell_drawer_page_drag_to(
                                (int16_t)page_delta_x);
                            session.last_drawer_page_drag_us = now_us;
                            ESP_LOGD(TAG,
                                     "touch drawer page drag dx=%d",
                                     page_delta_x);
                            continue;
                        }
                        if (!session.drawer_volume_drag_active) {
                            if (!session.drawer_panel_drag_active && abs_dy > 12 && abs_dy > abs_dx) {
                                session.drawer_panel_drag_active = true;
                                session.drawer_panel_drag_start_y = (int)session.start.y;
                                session.drawer_panel_drag_base_y = nuwax_app_shell_drawer_current_y();
                                session.drag_dispatched = true;
                            }
                            if (session.drawer_panel_drag_active) {
                                int64_t now_us = esp_timer_get_time();
                                if (session.last_drawer_panel_drag_us != 0 &&
                                    (now_us - session.last_drawer_panel_drag_us) / 1000 <
                                        NUWAX_TOUCH_DRAWER_DRAG_INTERVAL_MS) {
                                    continue;
                                }
                                int target_y =
                                    session.drawer_panel_drag_base_y +
                                    ((int)session.last.y - session.drawer_panel_drag_start_y);
                                if (target_y > 48) target_y = 48;
                                if (target_y < -226) target_y = -226;
                                nuwax_app_shell_drawer_drag_to((int16_t)target_y);
                                session.last_drawer_panel_drag_us = now_us;
                                ESP_LOGD(TAG, "touch drawer panel drag y=%d", target_y);
                                continue;
                            }
                        }
                        nuwax_app_shell_result_t result = {.action = NUWAX_APP_SHELL_ACTION_NONE, .index = 0};
                        queue_shell_action(&result);
                        if (session.drawer_opened_by_drag) {
                            continue;
                        }
                    }
                    if (nuwax_app_shell_focus_menu_main_is_open()) {
                        if (abs_dy < NUWAX_TOUCH_MENU_DRAG_START_PX ||
                            abs_dy * NUWAX_TOUCH_MENU_DRAG_AXIS_BIAS_DEN <=
                                abs_dx * NUWAX_TOUCH_MENU_DRAG_AXIS_BIAS_NUM) {
                            continue;
                        }
                        int64_t now_us = esp_timer_get_time();
                        if (session.last_menu_drag_us != 0 &&
                            (now_us - session.last_menu_drag_us) / 1000 < NUWAX_TOUCH_MENU_DRAG_INTERVAL_MS) {
                            continue;
                        }
                        int menu_delta_y = dy;
                        if (menu_delta_y > NUWAX_TOUCH_SCROLL_STEP_PX * 3) {
                            menu_delta_y = NUWAX_TOUCH_SCROLL_STEP_PX * 3;
                        } else if (menu_delta_y < -NUWAX_TOUCH_SCROLL_STEP_PX * 3) {
                            menu_delta_y = -NUWAX_TOUCH_SCROLL_STEP_PX * 3;
                        }
                        nuwax_app_shell_menu_drag_to((int16_t)menu_delta_y);
                        session.last_menu_drag_us = now_us;
                        session.drag_dispatched = true;
                        session.menu_drag_active = true;
                        session.hold_blocked_logged = false;
                        ESP_LOGD(TAG,
                                 "touch menu drag dy=%d x=%u y=%u",
                                 menu_delta_y,
                                 session.last.x,
                                 session.last.y);
                        continue;
                    }
                    if (nuwax_app_shell_focus_menu_detail_is_open()) {
                        if (abs_dy <= abs_dx) {
                            continue;
                        }
                        if (abs_dy < NUWAX_TOUCH_DETAIL_DRAG_START_PX) {
                            continue;
                        }
                        session.drag_dispatched = true;
                        session.hold_blocked_logged = false;
                        if (abs_dy < NUWAX_TOUCH_SCROLL_STEP_PX) {
                            continue;
                        }
                        int64_t now_us = esp_timer_get_time();
                        if (session.last_drag_dispatch_us != 0 &&
                            (now_us - session.last_drag_dispatch_us) / 1000 < NUWAX_TOUCH_SCROLL_MIN_INTERVAL_MS) {
                            continue;
                        }
                        int direction = dy < 0 ? 1 : -1;
                        nuwax_app_shell_menu_detail_adjust(direction);
                        session.last_drag_dispatch_us = now_us;
                        session.start = session.last;
                        session.start_us = now_us;
                        ESP_LOGD(TAG,
                                 "touch detail drag adjust direction=%d x=%u y=%u",
                                 direction,
                                 session.last.x,
                                 session.last.y);
                        continue;
                    }
                    if (abs_dy < NUWAX_TOUCH_SCROLL_STEP_PX || abs_dy <= abs_dx) {
                        continue;
                    }
                    int64_t now_us = esp_timer_get_time();
                    if (session.last_drag_dispatch_us != 0 &&
                        (now_us - session.last_drag_dispatch_us) / 1000 < NUWAX_TOUCH_SCROLL_MIN_INTERVAL_MS) {
                        continue;
                    }
                    nuwax_app_shell_event_t scroll_event =
                        dy < 0 ? NUWAX_APP_SHELL_EVENT_SWIPE_UP : NUWAX_APP_SHELL_EVENT_SWIPE_DOWN;
                    nuwax_app_shell_result_t result = nuwax_app_shell_handle_touch_event(scroll_event, &session.last);
                    queue_shell_action(&result);
                    session.last_drag_dispatch_us = now_us;
                    session.start = session.last;
                    session.start_us = now_us;
                    session.drag_dispatched = true;
                    session.hold_blocked_logged = false;
                    ESP_LOGD(TAG,
                             "touch drag scroll event=%d x=%u y=%u",
                             (int)scroll_event,
                             session.last.x,
                             session.last.y);
                    continue;
                }
                bool can_start_listen = nuwax_app_shell_can_start_listen();
                if (duration_ms >= NUWAX_HOLD_TO_TALK_START_MS &&
                    abs_dx <= NUWAX_HOLD_TO_TALK_MOVE_LIMIT_PX &&
                    abs_dy <= NUWAX_HOLD_TO_TALK_MOVE_LIMIT_PX &&
                    can_start_listen) {
                    nuwax_app_shell_result_t result =
                        nuwax_app_shell_handle_touch_event(NUWAX_APP_SHELL_EVENT_PRESS, &session.start);
                    queue_shell_action(&result);
                    if (result.action == NUWAX_APP_SHELL_ACTION_CANCEL) {
                        session.hold_cancel_dispatched = true;
                        ESP_LOGI(TAG,
                                 "touch interrupt requested x=%u y=%u",
                                 session.start.x,
                                 session.start.y);
                    } else if (result.action ==
                               NUWAX_APP_SHELL_ACTION_START_LISTEN) {
                        session.hold_to_talk_started = true;
                        ESP_LOGI(TAG,
                                 "touch listen start x=%u y=%u",
                                 session.start.x,
                                 session.start.y);
                    }
                } else if (duration_ms >= NUWAX_HOLD_TO_TALK_START_MS &&
                           !can_start_listen &&
                           !session.hold_blocked_logged) {
                    ESP_LOGI(TAG, "touch listen blocked by current shell state");
                    session.hold_blocked_logged = true;
                }
            }
            continue;
        }
        if (!point.touched && session.active) {
            session.active = false;
            int64_t duration_ms = touch_session_age_ms(&session, esp_timer_get_time());
            if (session.hold_to_talk_started) {
                nuwax_app_shell_result_t result = {
                    .action = NUWAX_APP_SHELL_ACTION_STOP_LISTEN,
                    .index = -1,
                };
                queue_shell_action(&result);
                reset_after_listen_release(&session);
                ESP_LOGI(TAG,
                         "touch listen release duration_ms=%lld x=%u y=%u",
                         (long long)duration_ms,
                         session.last.x,
                         session.last.y);
                continue;
            }
            if (session.hold_cancel_dispatched) {
                ESP_LOGI(TAG,
                         "touch interrupt release duration_ms=%lld x=%u y=%u",
                         (long long)duration_ms,
                         session.last.x,
                         session.last.y);
                reset_touch_session(&session);
                continue;
            }
            if (!session.drag_dispatched && nuwax_app_shell_drawer_is_open()) {
                const int dx = (int)session.last.x - (int)session.start.x;
                const int dy = (int)session.last.y - (int)session.start.y;
                const int abs_dx = abs_int(dx);
                const int abs_dy = abs_int(dy);
                if (abs_dy >= 36 && abs_dy > abs_dx) {
                    if (dy < 0) {
                        int target_y = 48 + dy;
                        if (target_y > 48) target_y = 48;
                        if (target_y < -72) target_y = -72;
                        nuwax_app_shell_drawer_drag_to((int16_t)target_y);
                        nuwax_app_shell_drawer_finish_drag(true);
                        ESP_LOGD(TAG,
                                 "touch drawer release fallback close y=%d dy=%d",
                                 target_y,
                                 dy);
                    } else {
                        nuwax_app_shell_drawer_finish_drag(false);
                        ESP_LOGD(TAG, "touch drawer release fallback open dy=%d", dy);
                    }
                    session.hold_blocked_logged = false;
                    session.last_drag_dispatch_us = 0;
                    session.last_menu_drag_us = 0;
                    session.last_drawer_panel_drag_us = 0;
                    session.last_volume_preview_us = 0;
                    continue;
                }
            }
            if (session.drag_dispatched) {
                if (session.drawer_page_drag_active) {
                    const int dx = (int)session.last.x - (int)session.start.x;
                    const int velocity_x1000 = duration_ms > 0
                        ? abs_int(dx) * 1000 / duration_ms
                        : 0;
                    const bool commit =
                        abs_int(dx) >= NUWAX_TOUCH_PAGE_DRAG_COMMIT_PX ||
                        velocity_x1000 >=
                            NUWAX_TOUCH_PAGE_DRAG_VELOCITY_X1000;
                    nuwax_app_shell_drawer_page_finish_drag(commit);
                    ESP_LOGD(TAG,
                             "touch drawer page release commit=%d dx=%d velocity_x1000=%d",
                             commit ? 1 : 0, dx, velocity_x1000);
                } else if (session.menu_drag_active) {
                    const int dy = (int)session.last.y - (int)session.start.y;
                    nuwax_app_shell_menu_finish_drag();
                    ESP_LOGD(TAG,
                             "touch menu drag release duration_ms=%lld dy=%d x=%u y=%u",
                             (long long)duration_ms,
                             dy,
                             session.last.x,
                             session.last.y);
                } else if (session.drawer_volume_drag_active && session.drawer_volume_pending >= 0) {
                    nuwax_app_shell_result_t result = {
                        .action = NUWAX_APP_SHELL_ACTION_SET_VOLUME,
                        .index = session.drawer_volume_pending,
                    };
                    queue_shell_action(&result);
                    ESP_LOGI(TAG, "touch drawer volume commit volume=%d", session.drawer_volume_pending);
                }
                if (session.drawer_panel_drag_active) {
                    const int total_dy = (int)session.last.y - (int)session.start.y;
                    const int drawer_y = (int)nuwax_app_shell_drawer_current_y();
                    const int velocity_y_x1000 = duration_ms > 0
                        ? total_dy * 1000 / duration_ms
                        : 0;
                    const bool open_commit =
                        total_dy >= NUWAX_TOUCH_DRAWER_COMMIT_PX ||
                        velocity_y_x1000 >=
                            NUWAX_TOUCH_DRAWER_VELOCITY_X1000;
                    const bool close_drawer = session.drawer_opened_by_drag
                                                  ? !open_commit
                                                  : (total_dy < -64 || drawer_y < -58);
                    nuwax_app_shell_drawer_finish_drag(close_drawer);
                    ESP_LOGD(TAG,
                             "touch drawer panel release close=%d y=%d dy=%d velocity_x1000=%d",
                             close_drawer ? 1 : 0,
                             drawer_y,
                             total_dy,
                             velocity_y_x1000);
                }
                reset_after_drag_release(&session);
                ESP_LOGD(TAG,
                         "touch drag release duration_ms=%lld x=%u y=%u",
                         (long long)duration_ms,
                         session.last.x,
                         session.last.y);
                continue;
            }
            nuwax_app_shell_event_t event = classify_touch(&session.start, &session.last, duration_ms);
            const nuwax_touch_point_t *dispatch_point = &session.last;
            if (event == NUWAX_APP_SHELL_EVENT_SWIPE_DOWN && session.start.y < NUWAX_GESTURE_TOP_BAND) {
                dispatch_point = &session.start;
                ESP_LOGD(TAG,
                         "touch top swipe down dispatch start x=%u y=%u last x=%u y=%u",
                         session.start.x,
                         session.start.y,
                         session.last.x,
                         session.last.y);
            }
            nuwax_app_shell_result_t result = nuwax_app_shell_handle_touch_event(event, dispatch_point);
            queue_shell_action(&result);
            session.hold_blocked_logged = false;
        }
    }
}
