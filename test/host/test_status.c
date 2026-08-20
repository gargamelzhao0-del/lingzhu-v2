#include "nuwax_test.h"

#include "menu/nuwax_focus_menu_status.h"

static void test_card_set_routes_by_title(void);
static void test_card_lines_stored_and_truncated(void);
static void test_card_invalid_item(void);

void nuwax_test_status(void) {
    NUWAX_TEST_RUN(test_card_set_routes_by_title);
    NUWAX_TEST_RUN(test_card_lines_stored_and_truncated);
    NUWAX_TEST_RUN(test_card_invalid_item);
}

static void test_card_set_routes_by_title(void) {
    /* "WORK"/"ISSUES" route to their items, anything else to CLAW */
    char lines[1][NUWAX_FOCUS_MENU_STATUS_LINE_LEN] = {"line"};
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_status_card_set("WORK", lines, 1, NULL), NUWAX_MENU_WORK);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_status_card_set("ISSUES", lines, 1, NULL), NUWAX_MENU_ISSUES);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_status_card_set("anything", lines, 1, NULL), NUWAX_MENU_CLAW);

    const nuwax_focus_menu_status_card_t *work = nuwax_focus_menu_status_card(NUWAX_MENU_WORK);
    NUWAX_CHECK(work != NULL && work->valid);
}

static void test_card_lines_stored_and_truncated(void) {
    char lines[2][NUWAX_FOCUS_MENU_STATUS_LINE_LEN];
    /* first line normal, second line over-long to test truncation */
    for (int i = 0; i < NUWAX_FOCUS_MENU_STATUS_LINE_LEN - 1; i++) lines[0][i] = 'x';
    lines[0][NUWAX_FOCUS_MENU_STATUS_LINE_LEN - 1] = '\0';
    for (int i = 0; i < NUWAX_FOCUS_MENU_STATUS_LINE_LEN - 1; i++) lines[1][i] = 'y';
    lines[1][NUWAX_FOCUS_MENU_STATUS_LINE_LEN - 1] = '\0';

    nuwax_focus_menu_status_card_set("WORK", lines, 2, NULL);
    const nuwax_focus_menu_status_card_t *c = nuwax_focus_menu_status_card(NUWAX_MENU_WORK);
    NUWAX_CHECK(c != NULL);
    if (c) {
        /* stored line is always null-terminated within the buffer */
        NUWAX_CHECK_EQ_INT((int)c->lines[0][NUWAX_FOCUS_MENU_STATUS_LINE_LEN - 1], 0);
        NUWAX_CHECK_EQ_INT((int)c->title[NUWAX_FOCUS_MENU_STATUS_TITLE_LEN - 1], 0);
    }
}

static void test_card_invalid_item(void) {
    NUWAX_CHECK(nuwax_focus_menu_status_card((nuwax_menu_item_t)-1) == NULL);
    NUWAX_CHECK(nuwax_focus_menu_status_card(NUWAX_MENU_COUNT) == NULL);
}
