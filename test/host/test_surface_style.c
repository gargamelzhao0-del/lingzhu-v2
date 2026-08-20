#include "nuwax_test.h"

#include "menu/nuwax_focus_menu_surface_style.h"
#include "nuwax_focus_menu.h" /* for NUWAX_MENU_COUNT */

static void test_item_for_slot_wraps(void);
static void test_slot_style_center_vs_edge(void);
static void test_mix_style_endpoints(void);
static void test_focus_pop_extra_window(void);

void nuwax_test_surface_style(void) {
    NUWAX_TEST_RUN(test_item_for_slot_wraps);
    NUWAX_TEST_RUN(test_slot_style_center_vs_edge);
    NUWAX_TEST_RUN(test_mix_style_endpoints);
    NUWAX_TEST_RUN(test_focus_pop_extra_window);
}

static void test_item_for_slot_wraps(void) {
    /* base 0: slot 0 -> 0, slot -1 wraps to last, slot +1 -> 1 */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_surface_item_for_slot(0, 0), 0);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_surface_item_for_slot(0, -1), NUWAX_MENU_COUNT - 1);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_surface_item_for_slot(0, 1), 1);
    /* base near the end wraps forward */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_surface_item_for_slot(NUWAX_MENU_COUNT - 1, 1), 0);
}

static void test_slot_style_center_vs_edge(void) {
    nuwax_focus_menu_surface_style_t center = nuwax_focus_menu_surface_slot_style(0);
    nuwax_focus_menu_surface_style_t edge = nuwax_focus_menu_surface_slot_style(-2);

    /* center slot is opaque, largest icon, fully emphasized */
    NUWAX_CHECK_EQ_INT(center.opa, LV_OPA_COVER);
    NUWAX_CHECK_EQ_INT(center.emphasis_256, 256);
    NUWAX_CHECK(center.icon_size > edge.icon_size);

    /* off-screen edge slot is transparent */
    NUWAX_CHECK_EQ_INT(edge.opa, 0);
    NUWAX_CHECK_EQ_INT(edge.emphasis_256, 0);
}

static void test_mix_style_endpoints(void) {
    nuwax_focus_menu_surface_style_t a = nuwax_focus_menu_surface_slot_style(-1);
    nuwax_focus_menu_surface_style_t b = nuwax_focus_menu_surface_slot_style(0);

    nuwax_focus_menu_surface_style_t at0 = nuwax_focus_menu_surface_mix_style(a, b, 0);
    nuwax_focus_menu_surface_style_t at256 = nuwax_focus_menu_surface_mix_style(a, b, 256);
    NUWAX_CHECK_EQ_INT(at0.icon_size, a.icon_size);
    NUWAX_CHECK_EQ_INT(at256.icon_size, b.icon_size);
    NUWAX_CHECK_EQ_INT(at256.opa, b.opa);
}

static void test_focus_pop_extra_window(void) {
    /* below emphasis threshold: no pop regardless of progress */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_surface_focus_pop_extra(0, 128), 0);
    /* emphasized but early progress: no pop yet */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_surface_focus_pop_extra(256, 0), 0);
    /* emphasized, mid-window: a small positive bump */
    int bump = nuwax_focus_menu_surface_focus_pop_extra(256, 137);
    NUWAX_CHECK(bump >= 0 && bump <= 2);
    /* emphasized, end of window: returns toward 0 */
    int tail = nuwax_focus_menu_surface_focus_pop_extra(256, 255);
    NUWAX_CHECK(tail >= 0 && tail <= 2);
}
