#include "nuwax_test.h"

#include "menu/nuwax_focus_menu_data.h"

static void test_item_valid(void);
static void test_item_names(void);
static void test_item_descriptions(void);
static void test_item_symbols_nonempty(void);

void nuwax_test_data(void) {
    NUWAX_TEST_RUN(test_item_valid);
    NUWAX_TEST_RUN(test_item_names);
    NUWAX_TEST_RUN(test_item_descriptions);
    NUWAX_TEST_RUN(test_item_symbols_nonempty);
}

static void test_item_valid(void) {
    NUWAX_CHECK(nuwax_focus_menu_item_valid(NUWAX_MENU_CLAW));
    NUWAX_CHECK(nuwax_focus_menu_item_valid(NUWAX_MENU_DEVICE));
    NUWAX_CHECK(!nuwax_focus_menu_item_valid((nuwax_menu_item_t)-1));
    NUWAX_CHECK(!nuwax_focus_menu_item_valid(NUWAX_MENU_COUNT));
}

static void test_item_names(void) {
    NUWAX_CHECK_STR_EQ(nuwax_focus_menu_item_name(NUWAX_MENU_CLAW), "Claw");
    NUWAX_CHECK_STR_EQ(nuwax_focus_menu_item_name(NUWAX_MENU_WIFI), "Wi-Fi");
    NUWAX_CHECK_STR_EQ(nuwax_focus_menu_item_name(NUWAX_MENU_DEVICE), "Device");
    /* out-of-range falls back to a safe sentinel, never crashes */
    NUWAX_CHECK_STR_EQ(nuwax_focus_menu_item_name((nuwax_menu_item_t)999), "Unknown");
}

static void test_item_descriptions(void) {
    /* every valid item has a non-empty description */
    for (int i = 0; i < NUWAX_MENU_COUNT; i++) {
        const char *d = nuwax_focus_menu_item_description((nuwax_menu_item_t)i);
        NUWAX_CHECK(d != NULL && d[0] != '\0');
    }
    /* out-of-range yields empty string, not NULL */
    NUWAX_CHECK_STR_EQ(nuwax_focus_menu_item_description((nuwax_menu_item_t)999), "");
}

static void test_item_symbols_nonempty(void) {
    for (int i = 0; i < NUWAX_MENU_COUNT; i++) {
        const char *s = nuwax_focus_menu_item_symbol((nuwax_menu_item_t)i);
        NUWAX_CHECK(s != NULL && s[0] != '\0');
    }
}
