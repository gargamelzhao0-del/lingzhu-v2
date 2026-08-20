#include "nuwax_test.h"

#include "menu/nuwax_focus_menu_wifi.h"

static void test_summary_and_scan_busy(void);
static void test_set_networks_clamps(void);
static void test_adjust_selection_wraps(void);
static void test_row_index_rotation(void);
static void test_result_state(void);
static void test_saved_networks(void);

void nuwax_test_wifi(void) {
    NUWAX_TEST_RUN(test_summary_and_scan_busy);
    NUWAX_TEST_RUN(test_set_networks_clamps);
    NUWAX_TEST_RUN(test_adjust_selection_wraps);
    NUWAX_TEST_RUN(test_row_index_rotation);
    NUWAX_TEST_RUN(test_result_state);
    NUWAX_TEST_RUN(test_saved_networks);
}

/* Helper: build a fixed-stride SSID array like the public API expects. */
static void fill_ssids(char dst[][NUWAX_FOCUS_MENU_WIFI_SSID_LEN], const char *const *src, int n) {
    for (int i = 0; i < n; i++) {
        int j = 0;
        for (; src[i][j] && j < NUWAX_FOCUS_MENU_WIFI_SSID_LEN - 1; j++) dst[i][j] = src[i][j];
        dst[i][j] = '\0';
    }
}

static void test_summary_and_scan_busy(void) {
    nuwax_focus_menu_wifi_set_summary();
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_view(), NUWAX_FOCUS_MENU_WIFI_VIEW_SUMMARY);

    nuwax_focus_menu_wifi_set_scan_busy();
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_view(), NUWAX_FOCUS_MENU_WIFI_VIEW_SCANNING);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_count(), 0);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_selected(), 0);
}

static void test_set_networks_clamps(void) {
    /* Request more than MAX_ITEMS; count must clamp. */
    char ssids[NUWAX_FOCUS_MENU_WIFI_MAX_ITEMS + 4][NUWAX_FOCUS_MENU_WIFI_SSID_LEN];
    for (int i = 0; i < NUWAX_FOCUS_MENU_WIFI_MAX_ITEMS + 4; i++) {
        ssids[i][0] = 'A'; ssids[i][1] = (char)('0' + i); ssids[i][2] = '\0';
    }
    nuwax_focus_menu_wifi_set_networks(ssids, NULL, NULL, NUWAX_FOCUS_MENU_WIFI_MAX_ITEMS + 4);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_count(), NUWAX_FOCUS_MENU_WIFI_MAX_ITEMS);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_view(), NUWAX_FOCUS_MENU_WIFI_VIEW_LIST);

    /* negative count clamps to 0 */
    nuwax_focus_menu_wifi_set_networks(ssids, NULL, NULL, -5);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_count(), 0);

    /* item() bounds-checks */
    NUWAX_CHECK(nuwax_focus_menu_wifi_item(-1) == NULL);
    NUWAX_CHECK(nuwax_focus_menu_wifi_item(0) == NULL); /* count is 0 now */
}

static void test_adjust_selection_wraps(void) {
    const char *names[3] = {"net0", "net1", "net2"};
    char ssids[3][NUWAX_FOCUS_MENU_WIFI_SSID_LEN];
    fill_ssids(ssids, names, 3);
    nuwax_focus_menu_wifi_set_networks(ssids, NULL, NULL, 3);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_selected(), 0);

    NUWAX_CHECK(nuwax_focus_menu_wifi_adjust_selection(+1));
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_selected(), 1);
    nuwax_focus_menu_wifi_adjust_selection(+1);
    nuwax_focus_menu_wifi_adjust_selection(+1); /* 2 -> wrap to 0 */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_selected(), 0);

    nuwax_focus_menu_wifi_adjust_selection(-1); /* 0 -> wrap to last */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_selected(), 2);

    /* SUMMARY view: adjust is a no-op */
    nuwax_focus_menu_wifi_set_summary();
    NUWAX_CHECK(!nuwax_focus_menu_wifi_adjust_selection(+1));
}

static void test_row_index_rotation(void) {
    const char *names[3] = {"a", "b", "c"};
    char ssids[3][NUWAX_FOCUS_MENU_WIFI_SSID_LEN];
    fill_ssids(ssids, names, 3);
    nuwax_focus_menu_wifi_set_networks(ssids, NULL, NULL, 3);
    /* selected=0: row layout is [selected-1, selected, selected+1] wrapped */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_network_index_for_row(0), 2); /* -1 wraps */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_network_index_for_row(1), 0);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_network_index_for_row(2), 1);
    /* out-of-range row */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_network_index_for_row(9), -1);

    /* select a specific row commits its rotated index */
    nuwax_focus_menu_wifi_select_network_row(2); /* -> index 1 */
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_selected(), 1);
}

static void test_result_state(void) {
    nuwax_focus_menu_wifi_set_connecting("HomeNet");
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_view(), NUWAX_FOCUS_MENU_WIFI_VIEW_CONNECTING);

    nuwax_focus_menu_wifi_set_result("HomeNet", true, "connected");
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_view(), NUWAX_FOCUS_MENU_WIFI_VIEW_RESULT);
    NUWAX_CHECK(nuwax_focus_menu_wifi_result_ok());
    NUWAX_CHECK_STR_EQ(nuwax_focus_menu_wifi_result_ssid(), "HomeNet");
    NUWAX_CHECK_STR_EQ(nuwax_focus_menu_wifi_result_message(), "connected");

    /* NULL message is coerced to empty, never crashes */
    nuwax_focus_menu_wifi_set_result(NULL, false, NULL);
    NUWAX_CHECK(!nuwax_focus_menu_wifi_result_ok());
    NUWAX_CHECK_STR_EQ(nuwax_focus_menu_wifi_result_message(), "");
}

static void test_saved_networks(void) {
    const char *names[2] = {"saved0", "saved1"};
    char ssids[2][NUWAX_FOCUS_MENU_WIFI_SSID_LEN];
    fill_ssids(ssids, names, 2);
    nuwax_focus_menu_wifi_set_saved_networks(ssids, 2);
    NUWAX_CHECK_EQ_INT(nuwax_focus_menu_wifi_saved_count(), 2);

    const nuwax_focus_menu_wifi_item_t *it = nuwax_focus_menu_wifi_saved_item(0);
    NUWAX_CHECK(it != NULL);
    if (it) NUWAX_CHECK_STR_EQ(it->ssid, "saved0");
    NUWAX_CHECK(nuwax_focus_menu_wifi_saved_item(5) == NULL);
}
