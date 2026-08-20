#pragma once

/*
 * Minimal host-side test assertion framework (no external deps).
 *
 * Each test file defines test functions and registers them in a `main()`
 * that calls NUWAX_TEST_RUN. Failures print file:line and a message but do
 * not abort, so one run reports every failure. Exit code is non-zero if any
 * assertion failed.
 *
 * This is deliberately tiny: the goal is to make the pure-logic focus-menu
 * modules (style math, menu data, wifi state machine, status cards, wheel /
 * surface geometry) runnable on a PC with `gcc` and zero hardware.
 */

#include <stdio.h>
#include <string.h>

extern int nuwax_test_failures;
extern int nuwax_test_checks;

#define NUWAX_CHECK(cond)                                                      \
    do {                                                                       \
        nuwax_test_checks++;                                                   \
        if (!(cond)) {                                                         \
            nuwax_test_failures++;                                             \
            printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

#define NUWAX_CHECK_EQ_INT(actual, expected)                                   \
    do {                                                                       \
        nuwax_test_checks++;                                                   \
        long _a = (long)(actual);                                              \
        long _e = (long)(expected);                                            \
        if (_a != _e) {                                                        \
            nuwax_test_failures++;                                             \
            printf("  FAIL %s:%d  %s == %s  (got %ld, want %ld)\n",            \
                   __FILE__, __LINE__, #actual, #expected, _a, _e);            \
        }                                                                      \
    } while (0)

#define NUWAX_CHECK_STR_EQ(actual, expected)                                   \
    do {                                                                       \
        nuwax_test_checks++;                                                   \
        const char *_a = (actual);                                            \
        const char *_e = (expected);                                          \
        if (!_a || !_e || strcmp(_a, _e) != 0) {                              \
            nuwax_test_failures++;                                             \
            printf("  FAIL %s:%d  %s == \"%s\"  (got \"%s\")\n",              \
                   __FILE__, __LINE__, #actual, _e ? _e : "(null)",           \
                   _a ? _a : "(null)");                                       \
        }                                                                      \
    } while (0)

#define NUWAX_TEST_RUN(fn)                                                     \
    do {                                                                       \
        int _before = nuwax_test_failures;                                     \
        printf("- %s\n", #fn);                                                 \
        fn();                                                                  \
        if (nuwax_test_failures == _before) printf("  ok\n");                  \
    } while (0)
