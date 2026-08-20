#include "nuwax_test.h"

#include "menu/nuwax_focus_menu_wheel.h"

static void test_index_wrap(void);
static void test_role_style_current_vs_side(void);
static void test_style_mix_endpoints(void);

void nuwax_test_wheel(void) {
    NUWAX_TEST_RUN(test_index_wrap);
    NUWAX_TEST_RUN(test_role_style_current_vs_side);
    NUWAX_TEST_RUN(test_style_mix_endpoints);
}

static void test_index_wrap(void) {
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_index_wrap(0), 0);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_index_wrap(NUWAX_MENU_COUNT), 0);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_index_wrap(-1), NUWAX_MENU_COUNT - 1);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_index_wrap(NUWAX_MENU_COUNT + 2), 2);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_index_wrap(-NUWAX_MENU_COUNT - 1), NUWAX_MENU_COUNT - 1);
}

static void test_role_style_current_vs_side(void) {
    nuwax_focus_menu_wheel_item_style_t cur = nuwax_focus_menu_wheel_role_style(1);
    nuwax_focus_menu_wheel_item_style_t prev = nuwax_focus_menu_wheel_role_style(0);
    nuwax_focus_menu_wheel_item_style_t off = nuwax_focus_menu_wheel_role_style(-1);

    /* current slot is fully opaque, emphasized, and the widest */
    NUWAX_CHECK_EQ_INT(cur.icon_opa, LV_OPA_COVER);
    NUWAX_CHECK_EQ_INT(cur.text_opa, LV_OPA_COVER);
    NUWAX_CHECK_EQ_INT(cur.emphasis_256, 256);
    NUWAX_CHECK_EQ_INT(cur.w, WHEEL_CURR_W);
    NUWAX_CHECK_EQ_INT(cur.h, WHEEL_CURR_H);

    /* side slot is dimmer and not emphasized */
    NUWAX_CHECK(prev.icon_opa < LV_OPA_COVER);
    NUWAX_CHECK_EQ_INT(prev.emphasis_256, 0);
    NUWAX_CHECK_EQ_INT(prev.w, WHEEL_OTHER_W);

    /* off-screen slot is fully transparent */
    NUWAX_CHECK_EQ_INT(off.icon_opa, 0);
    NUWAX_CHECK_EQ_INT(off.text_opa, 0);
}

static void test_style_mix_endpoints(void) {
    nuwax_focus_menu_wheel_item_style_t a = nuwax_focus_menu_wheel_role_style(0);
    nuwax_focus_menu_wheel_item_style_t b = nuwax_focus_menu_wheel_role_style(1);

    nuwax_focus_menu_wheel_item_style_t at0 = nuwax_focus_menu_wheel_style_mix(a, b, 0);
    nuwax_focus_menu_wheel_item_style_t at256 = nuwax_focus_menu_wheel_style_mix(a, b, 256);

    NUWAX_CHECK_EQ_INT(at0.w, a.w);
    NUWAX_CHECK_EQ_INT(at256.w, b.w);
    NUWAX_CHECK_EQ_INT(at256.emphasis_256, b.emphasis_256);

    /* progress is clamped: values beyond 256 behave like 256 */
    nuwax_focus_menu_wheel_item_style_t over = nuwax_focus_menu_wheel_style_mix(a, b, 999);
    NUWAX_CHECK_EQ_INT(over.w, b.w);

    /* midpoint width is strictly between the endpoints */
    nuwax_focus_menu_wheel_item_style_t mid = nuwax_focus_menu_wheel_style_mix(a, b, 128);
    NUWAX_CHECK(mid.w > a.w && mid.w < b.w);
}
