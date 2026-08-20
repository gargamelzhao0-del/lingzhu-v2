#include <stdio.h>
#include <string.h>

#include "wifi_saved_network_list_internal.h"

#define TEST_CAPACITY 4

static int s_failures;

static void expect_true(const char *name, bool value)
{
    if (value) {
        printf("[PASS] %s\n", name);
    } else {
        printf("[FAIL] %s\n", name);
        s_failures++;
    }
}

static void expect_int(const char *name, int actual, int expected)
{
    if (actual == expected) {
        printf("[PASS] %s=%d\n", name, actual);
    } else {
        printf("[FAIL] %s actual=%d expected=%d\n", name, actual, expected);
        s_failures++;
    }
}

static void expect_string(const char *name, const char *actual,
                          const char *expected)
{
    if (strcmp(actual, expected) == 0) {
        printf("[PASS] %s=%s\n", name, actual);
    } else {
        printf("[FAIL] %s actual=%s expected=%s\n", name, actual, expected);
        s_failures++;
    }
}

static void test_sparse_slots_are_compacted(void)
{
    char ssids[TEST_CAPACITY][NUWAX_WIFI_SAVED_SSID_BYTES] = {{0}};
    char passwords[TEST_CAPACITY][NUWAX_WIFI_SAVED_PASSWORD_BYTES] = {{0}};
    int count = 0;

    expect_true("append_slot_0",
                nuwax_wifi_saved_network_append(ssids, passwords,
                                                TEST_CAPACITY, &count,
                                                "first", "first-password"));
    expect_true("reject_empty_slot_1",
                !nuwax_wifi_saved_network_append(ssids, passwords,
                                                 TEST_CAPACITY, &count,
                                                 "", "ignored"));
    expect_true("append_slot_2_after_gap",
                nuwax_wifi_saved_network_append(ssids, passwords,
                                                TEST_CAPACITY, &count,
                                                "third", "third-password"));

    expect_int("sparse_compact_count", count, 2);
    expect_string("sparse_compact_ssid_0", ssids[0], "first");
    expect_string("sparse_compact_ssid_1", ssids[1], "third");
    expect_string("sparse_compact_password_1", passwords[1],
                  "third-password");
    expect_string("sparse_compact_unused_slot", ssids[2], "");

    expect_true("append_after_compaction",
                nuwax_wifi_saved_network_append(ssids, passwords,
                                                TEST_CAPACITY, &count,
                                                "fourth", "fourth-password"));
    expect_int("post_compaction_count", count, 3);
    expect_string("post_compaction_preserves_third", ssids[1], "third");
    expect_string("post_compaction_new_ssid", ssids[2], "fourth");
}

static void test_missing_password_is_open_network(void)
{
    char ssids[TEST_CAPACITY][NUWAX_WIFI_SAVED_SSID_BYTES] = {{0}};
    char passwords[TEST_CAPACITY][NUWAX_WIFI_SAVED_PASSWORD_BYTES] = {{0}};
    int count = 0;

    expect_true("append_open_network",
                nuwax_wifi_saved_network_append(ssids, passwords,
                                                TEST_CAPACITY, &count,
                                                "open-network", NULL));
    expect_int("open_network_count", count, 1);
    expect_string("open_network_password", passwords[0], "");
}

static void test_capacity_and_bounded_copy(void)
{
    char ssids[1][NUWAX_WIFI_SAVED_SSID_BYTES] = {{0}};
    char passwords[1][NUWAX_WIFI_SAVED_PASSWORD_BYTES] = {{0}};
    int count = 0;
    const char *long_ssid = "12345678901234567890123456789012-extra";
    const char *expected_ssid = "12345678901234567890123456789012";

    expect_true("append_bounded_network",
                nuwax_wifi_saved_network_append(ssids, passwords, 1, &count,
                                                long_ssid, "password"));
    expect_string("bounded_ssid", ssids[0], expected_ssid);
    expect_true("reject_over_capacity",
                !nuwax_wifi_saved_network_append(ssids, passwords, 1, &count,
                                                 "second", "password"));
    expect_int("capacity_count_unchanged", count, 1);
}

int main(void)
{
    test_sparse_slots_are_compacted();
    test_missing_password_is_open_network();
    test_capacity_and_bounded_copy();
    return s_failures == 0 ? 0 : 1;
}
