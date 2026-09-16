/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_time_utc_offset.c - Host query failure and calendar boundary checks
 */

#include "shared/xr_time_offset.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

static bool select_zone(const char *zone) {
#ifdef XR_OS_WINDOWS
    int status = _putenv_s("TZ", zone);
    _tzset();
#else
    int status = setenv("TZ", zone, 1);
    tzset();
#endif
    if (status == 0)
        return true;
    fprintf(stderr, "FAIL: cannot select test timezone %s\n", zone);
    ++failures;
    return false;
}

static void expect_offset(int64_t timestamp, int64_t expected) {
    int64_t actual = INT64_C(987654321);
    if (!xr_time_utc_offset_at(timestamp, &actual) || actual != expected) {
        fprintf(stderr, "FAIL: timestamp %" PRId64 " expected %" PRId64 ", got %" PRId64 "\n",
                timestamp, expected, actual);
        ++failures;
    }
}

static void expect_refusal(int64_t timestamp) {
    int64_t actual = INT64_C(987654321);
    if (xr_time_utc_offset_at(timestamp, &actual) || actual != INT64_C(987654321)) {
        fprintf(stderr, "FAIL: refused timestamp %" PRId64 " changed the output\n", timestamp);
        ++failures;
    }
}

static void query_boundaries(void) {
    if (select_zone("UTC0")) {
        expect_offset(0, 0);
        expect_offset(INT64_C(1704067200), 0);
        expect_refusal(INT64_MIN);
        expect_refusal(INT64_MAX);
#ifdef XR_OS_WINDOWS
        expect_refusal(-1);
#else
        expect_offset(-1, 0);
#endif
    }
    if (xr_time_utc_offset_at(0, NULL)) {
        fprintf(stderr, "FAIL: null output accepted\n");
        ++failures;
    }
    if (select_zone("PST8")) {
        expect_offset(INT64_C(1704067200), -480);
#ifndef XR_OS_WINDOWS
        expect_offset(0, -480);
        expect_offset(-1, -480);
#endif
    }
    if (select_zone("NPT-5:45"))
        expect_offset(INT64_C(1735675200), 345);
    if (select_zone("PST8PDT")) {
        expect_offset(INT64_C(1710064799), -480);
        expect_offset(INT64_C(1710064800), -420);
        expect_offset(INT64_C(1730624399), -420);
        expect_offset(INT64_C(1730624400), -480);
    }
}

static void calendar_boundaries(void) {
    const struct tm dates[] = {
        {.tm_year = 0, .tm_mon = 1, .tm_mday = 28},
        {.tm_year = 0, .tm_mon = 2, .tm_mday = 1},
        {.tm_year = 100, .tm_mon = 1, .tm_mday = 28},
        {.tm_year = 100, .tm_mon = 2, .tm_mday = 1},
        {.tm_year = 123, .tm_mon = 11, .tm_mday = 31},
        {.tm_year = 124, .tm_mon = 0, .tm_mday = 1},
        {.tm_year = -1901, .tm_mon = 11, .tm_mday = 31},
        {.tm_year = -1900, .tm_mon = 0, .tm_mday = 1},
    };
    const int64_t expected[] = {1, 2, 1, 1};
    for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
        int64_t difference =
            xr_time_calendar_day(&dates[index * 2 + 1]) - xr_time_calendar_day(&dates[index * 2]);
        if (difference != expected[index]) {
            fprintf(stderr, "FAIL: calendar case %zu expected %" PRId64 ", got %" PRId64 "\n",
                    index, expected[index], difference);
            ++failures;
        }
    }
}

int main(void) {
    query_boundaries();
    calendar_boundaries();
    if (failures)
        return 1;
    puts("UTC offset queries preserve host failures, DST and calendar boundaries");
    return 0;
}
