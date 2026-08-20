#include "menu/nuwax_device_qr_view.h"

#include <string.h>

#include "nuwax_theme.h"

#define DEVICE_QR_SIZE 232
#define DEVICE_QR_Y    55

static lv_obj_t *s_root;
static lv_obj_t *s_qr;
static lv_obj_t *s_device_id;
static lv_obj_t *s_error;
static bool s_open;

static lv_obj_t *make_label(lv_obj_t *parent,
                            const lv_font_t *font,
                            lv_opa_t opacity,
                            lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return NULL;
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xf4f7fb), 0);
    lv_obj_set_style_text_opa(label, opacity, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return label;
}

bool nuwax_device_qr_view_create(lv_obj_t *parent) {
    if (s_root || !parent) {
        return false;
    }

    s_root = lv_obj_create(parent);
    if (!s_root) return false;
    lv_obj_set_size(s_root, NUWAX_SCREEN_W, NUWAX_SCREEN_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x0c0e12), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = make_label(s_root,
                                 &lv_font_montserrat_14,
                                 LV_OPA_COVER,
                                 LV_TEXT_ALIGN_CENTER);
    if (!title) goto fail;
    lv_obj_set_pos(title, 70, 27);
    lv_obj_set_width(title, 220);
    lv_label_set_text(title, "Add device");

    lv_obj_t *back = make_label(s_root,
                                &lv_font_montserrat_14,
                                190,
                                LV_TEXT_ALIGN_CENTER);
    if (!back) goto fail;
    lv_obj_set_pos(back, 52, 58);
    lv_obj_set_size(back, 20, 20);
    lv_label_set_text(back, LV_SYMBOL_LEFT);

    s_qr = lv_qrcode_create(s_root);
    if (!s_qr) goto fail;
    lv_qrcode_set_size(s_qr, DEVICE_QR_SIZE);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x050505));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(0xffffff));
    lv_qrcode_set_quiet_zone(s_qr, true);
    lv_obj_set_pos(s_qr, (NUWAX_SCREEN_W - DEVICE_QR_SIZE) / 2, DEVICE_QR_Y);
    lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_device_id = make_label(s_root,
                             &lv_font_montserrat_12,
                             LV_OPA_COVER,
                             LV_TEXT_ALIGN_CENTER);
    if (!s_device_id) goto fail;
    lv_obj_set_pos(s_device_id, 54, 296);
    lv_obj_set_width(s_device_id, 252);

    lv_obj_t *hint = make_label(s_root,
                                &lv_font_montserrat_10,
                                110,
                                LV_TEXT_ALIGN_CENTER);
    if (!hint) goto fail;
    lv_obj_set_pos(hint, 78, 322);
    lv_obj_set_width(hint, 204);
    lv_label_set_text(hint, "Swipe right to return");

    s_error = make_label(s_root,
                         &lv_font_montserrat_12,
                         LV_OPA_COVER,
                         LV_TEXT_ALIGN_CENTER);
    if (!s_error) goto fail;
    lv_obj_set_pos(s_error, 80, 160);
    lv_obj_set_width(s_error, 200);
    lv_label_set_text(s_error, "QR unavailable");
    lv_obj_add_flag(s_error, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    return true;

fail:
    lv_obj_delete(s_root);
    nuwax_device_qr_view_reset();
    return false;
}

bool nuwax_device_qr_view_show(const char *payload, const char *device_id) {
    if (!s_root || !s_qr || !s_device_id || !s_error) {
        return false;
    }

    const bool have_payload = payload && payload[0];
    const bool qr_ok = have_payload &&
                       lv_qrcode_update(s_qr, payload,
                                        (uint32_t)strlen(payload)) ==
                           LV_RESULT_OK;
    if (qr_ok) {
        lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_error, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_error, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_device_id,
                      device_id && device_id[0] ? device_id : "Device ID unavailable");
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    s_open = true;
    return qr_ok;
}

void nuwax_device_qr_view_hide(void) {
    if (s_root) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    s_open = false;
}

bool nuwax_device_qr_view_is_open(void) {
    return s_open;
}

void nuwax_device_qr_view_reset(void) {
    s_root = NULL;
    s_qr = NULL;
    s_device_id = NULL;
    s_error = NULL;
    s_open = false;
}
