#include "touch_gesture.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_shell.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nuwax_gesture.h"
#include "touch_input.h"
#include "nuwax_ui_v28/control_center_input_policy.h"

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
#define NUWAX_TOUCH_PAGE_DRAG_START_PX NUWAX_CC_TOUCH_SLOP_PX
#define NUWAX_TOUCH_PAGE_DRAG_AXIS_BIAS_NUM 112
#define NUWAX_TOUCH_PAGE_DRAG_AXIS_BIAS_DEN 100
#define NUWAX_TOUCH_PAGE_DRAG_COMMIT_PX 47
#define NUWAX_TOUCH_PAGE_DRAG_VELOCITY_X1000 340
#define NUWAX_TOUCH_FLICK_MIN_PX 8
#define NUWAX_TOUCH_FLICK_REVERSAL_SLOP_PX 4
#define NUWAX_TOUCH_FLICK_SAMPLE_STEP_PX 2
#define NUWAX_TOUCH_DRAWER_VOLUME_PREVIEW_MS NUWAX_CC_VOLUME_FRAME_MS
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
    bool drawer_touch_started;
    bool drawer_tap_cancelled;
    bool drawer_panel_drag_active;
    bool drawer_page_drag_active;
    bool drawer_volume_drag_active;
    int drawer_panel_drag_start_y;
    int64_t last_drag_dispatch_us;
    int64_t last_menu_drag_us;
    int64_t last_drawer_panel_drag_us;
    int64_t last_drawer_page_drag_us;
    int64_t last_volume_preview_us;
    int64_t start_us;
    int64_t last_sample_us;
    struct { uint32_t ms; uint16_t x, y; } motion[12];
    uint8_t motion_next, motion_count;
} touch_session_t;

static QueueHandle_t s_shell_actions;

void nuwax_touch_gesture_set_action_queue(QueueHandle_t queue) {
    s_shell_actions = queue;
}

static void reset_touch_session(touch_session_t *session) {
    if (!session) {
        return;
    }
    memset(session, 0, sizeof(*session));
}

static void record_touch_motion(touch_session_t *session, const nuwax_touch_point_t *point) {
    session->last_sample_us = esp_timer_get_time();
    const unsigned i = session->motion_next;
    session->motion[i].ms = point->sample_ms ? point->sample_ms : (uint32_t)(session->last_sample_us / 1000);
    session->motion[i].x = point->x;
    session->motion[i].y = point->y;
    session->motion_next = (i + 1U) % 12U;
    if (session->motion_count < 12U) session->motion_count++;
}

static int touch_velocity(const touch_session_t *session, bool vertical, uint32_t now_ms) {
    const int end = vertical ? session->last.y : session->last.x;
    int direction = 0;
    int span = 0;
    uint32_t duration = 0U;
    /* Walk the most recent directed movement. Stationary samples and
     * small sensor reversals cannot pull the anchor into an earlier stroke. */
    for (unsigned n = 1U; n <= session->motion_count; ++n) {
        const unsigned i = (session->motion_next + 12U - n) % 12U;
        const uint32_t elapsed = now_ms - session->motion[i].ms;
        if (elapsed > NUWAX_CC_VELOCITY_WINDOW_MS) break;
        const int distance = end - (vertical ? session->motion[i].y : session->motion[i].x);
        if (direction == 0) {
            if (distance > -NUWAX_TOUCH_FLICK_MIN_PX && distance < NUWAX_TOUCH_FLICK_MIN_PX) {
                continue;
            }
            direction = distance < 0 ? -1 : 1;
        }
        const int directed = direction * distance;
        if (directed < span - NUWAX_TOUCH_FLICK_REVERSAL_SLOP_PX) break;
        if (directed >= span + NUWAX_TOUCH_FLICK_SAMPLE_STEP_PX) {
            span = directed;
            duration = elapsed;
        }
    }
    return direction * span * 1000 / (int)(duration < 16U ? 16U : duration);
}

static void begin_touch_session(touch_session_t *session, const nuwax_touch_point_t *point) {
    reset_touch_session(session);
    session->active = true;
    session->start = *point;
    session->last = *point;
    session->start_us = esp_timer_get_time();
    record_touch_motion(session, point);
    nuwax_app_shell_touch_begin();
}

static int abs_int(int value) {
    return value < 0 ? -value : value;
}

static bool page_release_commit(int dx, int velocity) {
    if (abs_int(velocity) >= NUWAX_TOUCH_PAGE_DRAG_VELOCITY_X1000)
        return (dx < 0 && velocity < 0) || (dx > 0 && velocity > 0);
    return abs_int(dx) >= NUWAX_TOUCH_PAGE_DRAG_COMMIT_PX;
}

static void update_drawer_contact(touch_session_t *session, const nuwax_touch_point_t *point) {
    if (!session->drawer_touch_started) return;
    if (nuwax_cc_contact_moved((int)point->x - session->start.x,
                               (int)point->y - session->start.y))
        session->drawer_tap_cancelled = true;
    /* Coalescing cannot retarget feedback to another card. Once cancelled,
     * moving back to the original card does not re-arm this contact. */
    nuwax_app_shell_drawer_contact(!session->drawer_tap_cancelled,
        session->start.x, session->start.y);
}

static bool drawer_tap_allowed(const touch_session_t *session, nuwax_app_shell_event_t event) {
    return !(session->drawer_touch_started && session->drawer_tap_cancelled &&
             event == NUWAX_APP_SHELL_EVENT_TAP);
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
        return;
    }
    nuwax_app_shell_action_queued();
}

static void cancel_touch_session(touch_session_t *session) {
    if (!session || !session->active) {
        return;
    }
    nuwax_app_shell_drawer_contact(false, 0, 0);
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
        nuwax_app_shell_drawer_finish_drag(0, 0, 0, true);
    }
    if (session->drawer_page_drag_active) {
        nuwax_app_shell_drawer_page_finish_drag(false);
    }
    if (session->drawer_volume_drag_active) {
        nuwax_app_shell_drawer_finish_volume_drag(-1);
    }
    ESP_LOGW(TAG,
             "touch session cancelled active_listen=%d menu_drag=%d drawer_drag=%d page_drag=%d",
             session->hold_to_talk_started ? 1 : 0,
             session->menu_drag_active ? 1 : 0,
             session->drawer_panel_drag_active ? 1 : 0,
             session->drawer_page_drag_active ? 1 : 0);
    reset_touch_session(session);
}

static void finish_drawer_release(const touch_session_t *session, uint32_t release_ms) {
    const int total_dy = (int)session->last.y - (int)session->start.y;
    const int delta_y = (int)session->last.y - session->drawer_panel_drag_start_y;
    const int velocity = touch_velocity(session, true, release_ms);
    nuwax_app_shell_drawer_finish_drag((int16_t)delta_y, (int16_t)total_dy,
                                      (int16_t)velocity, false);
    ESP_LOGD(TAG, "touch drawer release delta_y=%d total_dy=%d velocity=%d",
             delta_y, total_dy, velocity);
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
    const int velocity_x1000 = touch_velocity(session, false, (uint32_t)(now_us / 1000));
    const bool commit = page_release_commit(dx, velocity_x1000);
    nuwax_app_shell_drawer_page_drag_to((int16_t)dx);
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
    if ((now_us - session->last_sample_us) / 1000 < NUWAX_TOUCH_RELEASE_GUARD_MS) {
        return;
    }
    nuwax_app_shell_drawer_contact(false, 0, 0);
    if (session->drawer_panel_drag_active) {
        finish_drawer_release(session, (uint32_t)(now_us / 1000));
        reset_touch_session(session);
    } else if (session->drawer_page_drag_active) {
        finish_page_release_guard(session, now_us);
    } else if (session->menu_drag_active) {
        finish_menu_release_guard(session, now_us);
    } else {
        /* No final value can be trusted after a missing release. */
        cancel_touch_session(session);
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
    session->drawer_panel_drag_start_y = 0;
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
    uint32_t retired_contact_id = 0U;

    while (true) {
        nuwax_touch_point_t point = {0};
        if (nuwax_touch_poll_event(&point, 100) != ESP_OK) {
            const uint32_t contact_id = session.active ? session.start.contact_id : 0U;
            handle_release_guard_timeout(&session);
            if (contact_id != 0U && !session.active) retired_contact_id = contact_id;
            /* Init/recovery cooldowns return immediately. Yield explicitly so
             * a missing touch controller cannot starve Wi-Fi or OTA startup. */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (retired_contact_id != 0U && point.contact_id == retired_contact_id) {
            if (!point.touched) retired_contact_id = 0U;
            continue;
        }
        if (session.active && point.contact_id && session.start.contact_id &&
            point.contact_id != session.start.contact_id) {
            if (!point.touched) continue; /* Delayed old release. */
            cancel_touch_session(&session); /* Raw queue dropped a boundary. */
        }
        if (point.cancelled) {
            cancel_touch_session(&session);
            continue;
        }
        if (point.touched && !session.active) {
            begin_touch_session(&session, &point);
            session.drawer_touch_started = nuwax_app_shell_drawer_is_open();
            if (session.drawer_touch_started) {
                nuwax_app_shell_drawer_contact(true, point.x, point.y);
                if (nuwax_app_shell_drawer_preview_volume_drag_at(point.x, point.y, false, NULL)) {
                    session.drawer_volume_drag_active = true;
                    session.last_volume_preview_us = esp_timer_get_time();
                    session.drag_dispatched = true;
                }
            }
            continue;
        }
        if (point.touched && session.active) {
            session.last = point;
            record_touch_motion(&session, &point);
            update_drawer_contact(&session, &point);
            if (session.drawer_volume_drag_active) {
                const int64_t now_us = esp_timer_get_time();
                if ((now_us - session.last_volume_preview_us) / 1000 >=
                    NUWAX_TOUCH_DRAWER_VOLUME_PREVIEW_MS) {
                    (void)nuwax_app_shell_drawer_preview_volume_drag_at(point.x, point.y, true, NULL);
                    session.last_volume_preview_us = now_us;
                }
                continue;
            }
            if (session.drawer_panel_drag_active) {
                const int64_t now_us = esp_timer_get_time();
                if ((now_us - session.last_drawer_panel_drag_us) / 1000 >=
                    NUWAX_TOUCH_DRAWER_DRAG_INTERVAL_MS) {
                    const int16_t delta_y = (int16_t)((int)point.y - session.drawer_panel_drag_start_y);
                    nuwax_app_shell_drawer_drag_by(delta_y, session.drawer_opened_by_drag);
                    session.last_drawer_panel_drag_us = now_us;
                }
                continue;
            }
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
                    session.drawer_opened_by_drag = true;
                    session.drawer_panel_drag_active = true;
                    session.drawer_panel_drag_start_y = (int)session.start.y;
                    session.drag_dispatched = true;
                    nuwax_app_shell_drawer_drag_by((int16_t)dy, true);
                    session.last_drawer_panel_drag_us = esp_timer_get_time();
                    ESP_LOGD(TAG,
                             "touch drawer pull open start x=%u y=%u last x=%u y=%u",
                             session.start.x,
                             session.start.y,
                             session.last.x,
                             session.last.y);
                    continue;
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
                        if (!session.drawer_page_drag_active &&
                            abs_dx > NUWAX_TOUCH_PAGE_DRAG_START_PX &&
                            nuwax_app_shell_drawer_can_page_drag((int16_t)dx) &&
                            abs_dx * NUWAX_TOUCH_PAGE_DRAG_AXIS_BIAS_DEN >
                                abs_dy * NUWAX_TOUCH_PAGE_DRAG_AXIS_BIAS_NUM) {
                            session.drawer_page_drag_active = true;
                            session.drag_dispatched = true;
                        }
                        if (session.drawer_page_drag_active) {
                            if (session.last_drawer_page_drag_us != 0 &&
                                (now_us - session.last_drawer_page_drag_us) /
                                        1000 <
                                    NUWAX_TOUCH_MENU_DRAG_INTERVAL_MS) {
                                continue;
                            }
                            int page_delta_x = dx;
                            if (page_delta_x > NUWAX_CC_PAGE_DRAG_RANGE_PX) page_delta_x = NUWAX_CC_PAGE_DRAG_RANGE_PX;
                            if (page_delta_x < -NUWAX_CC_PAGE_DRAG_RANGE_PX) page_delta_x = -NUWAX_CC_PAGE_DRAG_RANGE_PX;
                            nuwax_app_shell_drawer_page_drag_to(
                                (int16_t)page_delta_x);
                            session.last_drawer_page_drag_us = now_us;
                            ESP_LOGD(TAG,
                                     "touch drawer page drag dx=%d",
                                     page_delta_x);
                            continue;
                        }
                        if (session.drawer_touch_started &&
                            abs_dy > NUWAX_CC_TOUCH_SLOP_PX && abs_dy > abs_dx) {
                            session.drawer_panel_drag_active = true;
                            session.drawer_panel_drag_start_y = (int)point.y;
                            session.drag_dispatched = true;
                            nuwax_app_shell_drawer_drag_by(0, false);
                            session.last_drawer_panel_drag_us = now_us;
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
            nuwax_app_shell_drawer_contact(false, 0, 0);
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
            if (session.drag_dispatched) {
                if (session.drawer_page_drag_active) {
                    const int dx = (int)session.last.x - (int)session.start.x;
                    const int velocity_x1000 = touch_velocity(&session, false,
                        point.sample_ms ? point.sample_ms : (uint32_t)(esp_timer_get_time() / 1000));
                    const bool commit = page_release_commit(dx, velocity_x1000);
                    nuwax_app_shell_drawer_page_drag_to((int16_t)dx);
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
                } else if (session.drawer_volume_drag_active) {
                    uint8_t final_volume = 0;
                    const bool valid = nuwax_app_shell_drawer_preview_volume_drag_at(
                        session.last.x, session.last.y, true, &final_volume);
                    nuwax_app_shell_drawer_finish_volume_drag(valid ? final_volume : -1);
                } else if (session.drawer_panel_drag_active) {
                    finish_drawer_release(&session, point.sample_ms ? point.sample_ms :
                        (uint32_t)(esp_timer_get_time() / 1000));
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
            /* A stationary press on a visible control still activates it on
             * release. Home long-press policy remains separate. */
            if (session.drawer_touch_started && !session.drawer_tap_cancelled)
                event = NUWAX_APP_SHELL_EVENT_TAP;
            if (!drawer_tap_allowed(&session, event)) continue;
            const nuwax_touch_point_t *dispatch_point =
                session.drawer_touch_started && event == NUWAX_APP_SHELL_EVENT_TAP ?
                &session.start : &session.last;
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
