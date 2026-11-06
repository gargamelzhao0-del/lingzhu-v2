#include "nuwax_test_panel.h"

#if NUWAX_TEST_PANEL_ENABLED
static lv_obj_t *s_network_state;
static lv_obj_t *s_network_track;
static lv_obj_t *s_network_knob;

static lv_obj_t *label(lv_obj_t *parent, int x, int y, const char *text) {
    lv_obj_t *obj = lv_label_create(parent);
    if (!obj) return NULL;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, &nuwax_font_test_panel_11, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xc9d1d9), 0);
    lv_label_set_text(obj, text);
    return obj;
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) return NULL;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x20262e), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static bool row(lv_obj_t *parent, int y, const char *title,
                const char *hint, bool network) {
    lv_obj_t *card = panel(parent, 60, y, 240, 56);
    if (!card || !label(card, 12, 10, title) || !label(card, 12, 31, hint)) return false;
    lv_obj_t *track = panel(card, 186, 10, 40, 20);
    if (!track) return false;
    lv_obj_t *knob = panel(track, 3, 3, 14, 14);
    if (!knob) return false;
    lv_obj_set_style_bg_color(knob, lv_color_hex(0x7f878d), 0);
    if (network) {
        s_network_track = track;
        s_network_knob = knob;
        s_network_state = label(card, 187, 34, "OFF");
        return s_network_state != NULL;
    }
    lv_obj_set_style_opa(track, LV_OPA_40, 0);
    return label(card, 183, 34, "LOCKED") != NULL;
}

lv_obj_t *nuwax_test_panel_create(lv_obj_t *parent) {
    nuwax_test_panel_reset();
    lv_obj_t *root = panel(parent, 0, 0, 360, 360);
    if (!root) return NULL;
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    if (!label(root, 157, 77, "测试页面") ||
        !row(root, 105, "云端断网", "保留 Wi-Fi / 局域网", true) ||
        !row(root, 170, "MIC3 参考链路", "待接入", false) ||
        !row(root, 235, "实时模式", "待接入", false)) {
        lv_obj_delete(root);
        nuwax_test_panel_reset();
        return NULL;
    }
    nuwax_test_panel_set_network(false, false, false);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    return root;
}

void nuwax_test_panel_reset(void) {
    s_network_state = NULL;
    s_network_track = NULL;
    s_network_knob = NULL;
}

void nuwax_test_panel_set_network(bool enabled, bool pending, bool target) {
    if (!s_network_state) return;
#if NUWAX_TEST_NETWORK_ENABLED
    const bool on = pending ? target : enabled;
    lv_label_set_text(s_network_state, pending ? "WAIT" : (enabled ? "ON" : "OFF"));
    lv_obj_set_x(s_network_knob, on ? 23 : 3);
    lv_obj_set_style_bg_color(s_network_track, lv_color_hex(on ? 0x397383 : 0x353c45), 0);
    lv_obj_set_style_bg_color(s_network_knob, lv_color_hex(0x8edce8), 0);
#else
    (void)enabled; (void)pending; (void)target;
    lv_label_set_text(s_network_state, "LOCKED");
    lv_obj_set_style_opa(s_network_track, LV_OPA_40, 0);
#endif
}

bool nuwax_test_panel_hit_network(uint16_t x, uint16_t y) {
#if NUWAX_TEST_NETWORK_ENABLED
    return x >= 60 && x < 300 && y >= 105 && y < 161;
#else
    (void)x; (void)y;
    return false;
#endif
}
#endif
