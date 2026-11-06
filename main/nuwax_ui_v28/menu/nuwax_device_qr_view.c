#include "menu/nuwax_device_qr_view.h"
#include "menu/nuwax_detail_style.h"
#include "nuwax_theme.h"
#include <string.h>
/* Native integer QR scaling and four-module quiet zone; never image scaling. */
#define DEVICE_QR_SIZE 220
#define DEVICE_QR_Y 76
static lv_obj_t *s_root;
static lv_obj_t *s_qr;
static lv_obj_t *s_device_id;
static lv_obj_t *s_error;
static lv_obj_t *s_back;
static bool s_open;
bool nuwax_device_qr_view_create(lv_obj_t *parent) {
    if (s_root || !parent) return false;
    s_root = lv_obj_create(parent);
    if (!s_root) return false;
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, NUWAX_SCREEN_W, NUWAX_SCREEN_H);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_back = nuwax_detail_panel(s_root, NUWAX_DETAIL_BACK_X,
        NUWAX_DETAIL_BACK_Y, NUWAX_DETAIL_BACK_W, NUWAX_DETAIL_BACK_H);
    if (s_back) {
        lv_obj_set_style_radius(s_back, LV_RADIUS_CIRCLE, 0);
        lv_obj_t *cancel = nuwax_detail_label(s_back, 4, 2, NUWAX_DETAIL_BACK_W - 12,
            &nuwax_font_lingzhu_17, "取消配网");
        if (!cancel) goto fail;
        lv_obj_set_style_text_align(cancel, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (!nuwax_detail_header(s_root, "连接设备") || !s_back) goto fail;
    lv_obj_t *hint = nuwax_detail_label(s_root, 70, 55, 220,
        &nuwax_font_lingzhu_14, "打开 APP 扫码配网");
    if (!hint) goto fail;
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    s_qr = lv_qrcode_create(s_root);
    if (!s_qr) goto fail;
    lv_qrcode_set_size(s_qr, DEVICE_QR_SIZE);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_qrcode_set_quiet_zone(s_qr, true);
    lv_obj_set_pos(s_qr, (NUWAX_SCREEN_W - DEVICE_QR_SIZE) / 2, DEVICE_QR_Y);
    lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_device_id = nuwax_detail_label(s_root, 54, 298, 252,
        &nuwax_font_lingzhu_14, "设备 ID 未就绪");
    s_error = nuwax_detail_label(s_root, 78, 162, 204,
        &nuwax_font_lingzhu_17, "二维码暂不可用\n请稍后重新打开");
    if (!s_device_id || !s_error) goto fail;
    lv_obj_set_style_text_align(s_device_id, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(s_error, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_error, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    return true;
fail:
    lv_obj_delete(s_root);
    nuwax_device_qr_view_reset();
    return false;
}
bool nuwax_device_qr_view_show(const char *payload, const char *device_id) {
    if (!s_root || !s_qr || !s_device_id || !s_error) return false;
    const bool qr_ok = payload && payload[0] &&
        lv_qrcode_update(s_qr, payload, (uint32_t)strlen(payload)) == LV_RESULT_OK;
    if (qr_ok) {
        lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_error, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Never show an old payload after a failed or empty update. */
        lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_error, LV_OBJ_FLAG_HIDDEN);
    }
    if (device_id && device_id[0]) lv_label_set_text_fmt(s_device_id, "设备 ID  %s", device_id);
    else lv_label_set_text(s_device_id, "设备 ID 未就绪");
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    s_open = true;
    return qr_ok;
}
void nuwax_device_qr_view_hide(void) {
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
}
bool nuwax_device_qr_view_is_open(void) { return s_open; }
lv_obj_t *nuwax_device_qr_view_root(void) { return s_root; }
lv_obj_t *nuwax_device_qr_view_back_button(void) { return s_back; }
void nuwax_device_qr_view_reset(void) {
    s_root = NULL; s_qr = NULL; s_device_id = NULL; s_error = NULL; s_back = NULL; s_open = false;
}
