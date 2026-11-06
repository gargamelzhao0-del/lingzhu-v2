#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "nuwax_device_state.h"

/**
 * Create the entire V28 UI on the given screen.
 * Call after nuwax_lvgl_port_init() while holding the LVGL port mutex.
 */
esp_err_t nuwax_app_create(lv_obj_t *screen);
/* Creates the UI with a provisioning-first face. This is used only by the
 * low-memory boot profile so the first visible full-frame is the real
 * provisioning asset rather than the normal happy/disconnected transition. */
esp_err_t nuwax_app_create_with_provisioning_face(lv_obj_t *screen,
                                                  bool provisioning_face);
/** Destroy the V28 UI while holding the LVGL port mutex. */
esp_err_t nuwax_app_deinit(void);

/**
 * Update UI from external state change (called via state bridge).
 */
void nuwax_app_on_state_changed(const nuwax_device_state_t *state);
