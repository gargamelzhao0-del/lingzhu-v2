#include "nuwax_test.h"

#include "menu/nuwax_focus_menu_style.h"

static void test_clamp_int(void);
static void test_mix_int(void);
static void test_mix_opa_saturates(void);
static void test_ease_out_cubic_endpoints(void);
static void test_volume_arc_value(void);
static void test_colors_distinct(void);

void nuwax_test_style(void) {
    NUWAX_TEST_RUN(test_clamp_int);
    NUWAX_TEST_RUN(test_mix_int);
    NUWAX_TEST_RUN(test_mix_opa_saturates);
    NUWAX_TEST_RUN(test_ease_out_cubic_endpoints);
    NUWAX_TEST_RUN(test_volume_arc_value);
    NUWAX_TEST_RUN(test_colors_distinct);
}

static void test_clamp_int(void) {
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_clamp_int(5, 0, 10), 5);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_clamp_int(-3, 0, 10), 0);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_clamp_int(99, 0, 10), 10);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_clamp_int(7, 7, 7), 7);
}

static void test_mix_int(void) {
    /* progress in 0..256 fixed point */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_mix_int(0, 100, 0), 0);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_mix_int(0, 100, 256), 100);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_mix_int(0, 100, 128), 50);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_mix_int(100, 0, 128), 50);
    /* mixing is monotonic and exact at the midpoint for even deltas */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_mix_int(40, 60, 128), 50);
}

static void test_mix_opa_saturates(void) {
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_mix_opa(0, 255, 0), 0);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_mix_opa(0, 255, 256), 255);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_mix_opa(0, 255, 128), 127);
    /* mixed value stays within the endpoints (no overflow/underflow) */
    lv_opa_t mid = nuwax_focus_menu_mix_opa(10, 200, 130);
    NUWAX_CHECK(mid >= 10 && mid <= 200);
}

static void test_ease_out_cubic_endpoints(void) {
    /* clamps and endpoints */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_ease_out_cubic(0, 100), 0);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_ease_out_cubic(100, 100), 256);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_ease_out_cubic(150, 100), 256); /* t>=d */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_ease_out_cubic(50, 0), 256);    /* d<=0 guard */
    /* ease-out: past the halfway time the eased value is beyond halfway */
    int half = nuwax_focus_menu_ease_out_cubic(50, 100);
    NUWAX_CHECK(half > 128);
}

static void test_volume_arc_value(void) {
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_volume_arc_value(0), 0);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_volume_arc_value(100), 100);
    /* clamped to [0,100] */
    NUWAX_CHECK(nuwax_focus_menu_volume_arc_value(200) <= 100);
    NUWAX_CHECK(nuwax_focus_menu_volume_arc_value(-50) >= 0);
}

static void test_colors_distinct(void) {
    /* ghost gray and blush must differ; panel is white */
    NUWAX_CHECK(nuwax_focus_menu_color_ghost_gray().full != nuwax_focus_menu_color_ghost_blush().full);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_color_panel().full, lv_color_hex(0xffffff).full);
}
