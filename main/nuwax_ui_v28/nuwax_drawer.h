#pragma once

#include "lvgl.h"
#include "esp_err.h"
#include "nuwax_device_state.h"

typedef enum {
    NUWAX_DRAWER_ACTION_NONE = 0,
    NUWAX_DRAWER_ACTION_SET_VOLUME,
    NUWAX_DRAWER_ACTION_SET_MEETING_MODE,
    NUWAX_DRAWER_ACTION_REPROVISION_WIFI,
} nuwax_drawer_action_t;

typedef struct {
    nuwax_drawer_action_t action;
    int value;
} nuwax_drawer_action_result_t;

esp_err_t nuwax_drawer_create(lv_obj_t *parent);
void nuwax_drawer_deinit(void);
void nuwax_drawer_update(const nuwax_device_state_t *state);
void nuwax_drawer_open(void);
void nuwax_drawer_open_menu(void);
/* LVGL-task-only entry used by boot provisioning after its non-blocking delay. */
bool nuwax_drawer_show_provisioning_qr(void);
/* LVGL-task-only: closes the control center only when its current detail is
 * the provisioning QR. Other user-selected pages are left untouched. */
void nuwax_drawer_hide_provisioning_qr(void);
void nuwax_drawer_begin_pull(void);
void nuwax_drawer_close(void);
bool nuwax_drawer_is_open(void);
bool nuwax_drawer_is_detail(void);
bool nuwax_drawer_is_menu_page(void);
void nuwax_drawer_next_page(void);
void nuwax_drawer_prev_page(void);
void nuwax_drawer_page_drag_to(int16_t delta_x);
void nuwax_drawer_page_finish_drag(bool commit);
void nuwax_drawer_back(void);
nuwax_drawer_action_result_t nuwax_drawer_tap(uint16_t x, uint16_t y);
/* LVGL-task-only rollback for an optimistic menu action that the business
 * owner or its queue rejected. */
void nuwax_drawer_reject_action(nuwax_drawer_action_t action);
bool nuwax_drawer_hit_volume(uint16_t x, uint16_t y, uint8_t *volume_out);
bool nuwax_drawer_hit_volume_at_panel_y(uint16_t x, uint16_t y, int16_t panel_y, uint8_t *volume_out);
void nuwax_drawer_preview_volume(uint8_t volume);
void nuwax_drawer_drag_to(int16_t y);
void nuwax_drawer_finish_drag(bool close);
int16_t nuwax_drawer_current_y(void);
