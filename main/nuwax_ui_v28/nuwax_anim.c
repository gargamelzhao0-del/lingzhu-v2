#include "nuwax_anim.h"
#include "nuwax_device_state.h"

bool nuwax_anim_should_skip(void) {
    const nuwax_device_state_t *s = nuwax_device_state_get();
    return s ? s->low_power : false;
}

static void opa_exec_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void nuwax_anim_fade(lv_obj_t *obj, lv_opa_t target, uint32_t duration_ms) {
    if (!obj) return;
    if (nuwax_anim_should_skip()) {
        lv_obj_set_style_opa(obj, target, 0);
        return;
    }
    lv_opa_t current = lv_obj_get_style_opa(obj, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, current, target);
    lv_anim_set_duration(&a, duration_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, opa_exec_cb);
    lv_anim_start(&a);
}
