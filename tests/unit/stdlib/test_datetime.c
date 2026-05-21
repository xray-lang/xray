/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_datetime.c - Unit tests for DateTime component access and formatting
 *
 * KEY CONCEPT:
 *   Tests DateTime component extraction, comparison, formatting,
 *   and utility functions using stack-allocated mock DateTime objects.
 */

#include "../test_framework.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

/* DateTime is now stored as the native body of an XrInstance — the
 * body struct itself has no GC header. These tests work on stack-
 * allocated body structs and only call helpers that touch the body. */

typedef struct XrDateTime {
    int64_t timestamp;
    int32_t milliseconds;
    int16_t tz_offset;
    uint8_t is_utc;
    uint8_t _pad;
} XrDateTime;

// Forward declare component access functions
int xr_datetime_year(XrDateTime *dt);
int xr_datetime_month(XrDateTime *dt);
int xr_datetime_day(XrDateTime *dt);
int xr_datetime_hour(XrDateTime *dt);
int xr_datetime_minute(XrDateTime *dt);
int xr_datetime_second(XrDateTime *dt);
int xr_datetime_millisecond(XrDateTime *dt);
int xr_datetime_weekday(XrDateTime *dt);
int xr_datetime_yearday(XrDateTime *dt);

int xr_datetime_is_before(XrDateTime *dt1, XrDateTime *dt2);
int xr_datetime_is_after(XrDateTime *dt1, XrDateTime *dt2);
int xr_datetime_equals(XrDateTime *dt1, XrDateTime *dt2);
int xr_datetime_is_leap_year(XrDateTime *dt);
int xr_datetime_days_in_month(XrDateTime *dt);

int xr_datetime_format(XrDateTime *dt, const char *pattern, char *buf, size_t buf_size);
int xr_datetime_to_iso_string(XrDateTime *dt, char *buf, size_t buf_size);
int xr_datetime_local_offset(void);
void xr_datetime_to_tm(XrDateTime *dt, struct tm *tm);

// Helper: create a UTC DateTime on the stack
static XrDateTime make_utc_dt(int64_t ts, int32_t ms) {
    XrDateTime dt;
    memset(&dt, 0, sizeof(dt));
    dt.timestamp = ts;
    dt.milliseconds = ms;
    dt.is_utc = 1;
    dt.tz_offset = 0;
    return dt;
}

/* ========== Component Access ========== */

TEST(datetime_epoch_components) {
    // 1970-01-01 00:00:00 UTC (Unix epoch)
    XrDateTime dt = make_utc_dt(0, 0);
    ASSERT_EQ_INT(xr_datetime_year(&dt), 1970);
    ASSERT_EQ_INT(xr_datetime_month(&dt), 1);
    ASSERT_EQ_INT(xr_datetime_day(&dt), 1);
    ASSERT_EQ_INT(xr_datetime_hour(&dt), 0);
    ASSERT_EQ_INT(xr_datetime_minute(&dt), 0);
    ASSERT_EQ_INT(xr_datetime_second(&dt), 0);
    ASSERT_EQ_INT(xr_datetime_millisecond(&dt), 0);
}

TEST(datetime_known_date) {
    // 2026-03-21 14:30:45 UTC = 1774103445
    XrDateTime dt = make_utc_dt(1774103445, 123);
    ASSERT_EQ_INT(xr_datetime_year(&dt), 2026);
    ASSERT_EQ_INT(xr_datetime_month(&dt), 3);
    ASSERT_EQ_INT(xr_datetime_day(&dt), 21);
    ASSERT_EQ_INT(xr_datetime_hour(&dt), 14);
    ASSERT_EQ_INT(xr_datetime_minute(&dt), 30);
    ASSERT_EQ_INT(xr_datetime_second(&dt), 45);
    ASSERT_EQ_INT(xr_datetime_millisecond(&dt), 123);
}

TEST(datetime_y2k) {
    // 2000-01-01 00:00:00 UTC = 946684800
    XrDateTime dt = make_utc_dt(946684800, 0);
    ASSERT_EQ_INT(xr_datetime_year(&dt), 2000);
    ASSERT_EQ_INT(xr_datetime_month(&dt), 1);
    ASSERT_EQ_INT(xr_datetime_day(&dt), 1);
}

/* ========== Weekday ========== */

TEST(datetime_weekday) {
    // 1970-01-01 is Thursday (4)
    XrDateTime dt = make_utc_dt(0, 0);
    ASSERT_EQ_INT(xr_datetime_weekday(&dt), 4);
}

/* ========== Comparison ========== */

TEST(datetime_compare) {
    XrDateTime dt1 = make_utc_dt(1000, 0);
    XrDateTime dt2 = make_utc_dt(2000, 0);
    XrDateTime dt3 = make_utc_dt(1000, 0);

    ASSERT_TRUE(xr_datetime_is_before(&dt1, &dt2));
    ASSERT_FALSE(xr_datetime_is_before(&dt2, &dt1));
    ASSERT_TRUE(xr_datetime_is_after(&dt2, &dt1));
    ASSERT_TRUE(xr_datetime_equals(&dt1, &dt3));
    ASSERT_FALSE(xr_datetime_equals(&dt1, &dt2));
}

/* ========== Leap Year ========== */

TEST(datetime_leap_year) {
    // 2000 is leap year (divisible by 400)
    XrDateTime dt2000 = make_utc_dt(946684800, 0);
    ASSERT_TRUE(xr_datetime_is_leap_year(&dt2000));

    // 1970 is not leap year
    XrDateTime dt1970 = make_utc_dt(0, 0);
    ASSERT_FALSE(xr_datetime_is_leap_year(&dt1970));

    // 2024 is leap year (divisible by 4, not 100)
    // 2024-01-01 UTC = 1704067200
    XrDateTime dt2024 = make_utc_dt(1704067200, 0);
    ASSERT_TRUE(xr_datetime_is_leap_year(&dt2024));
}

/* ========== Days In Month ========== */

TEST(datetime_days_in_month) {
    // January has 31 days
    XrDateTime jan = make_utc_dt(0, 0);  // 1970-01
    ASSERT_EQ_INT(xr_datetime_days_in_month(&jan), 31);

    // Feb 2000 (leap) has 29 days
    XrDateTime feb_leap = make_utc_dt(949363200, 0);  // 2000-02-01
    ASSERT_EQ_INT(xr_datetime_days_in_month(&feb_leap), 29);
}

/* ========== Format ========== */

TEST(datetime_format_iso) {
    XrDateTime dt = make_utc_dt(0, 0);
    char buf[64];
    int n = xr_datetime_to_iso_string(&dt, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    // Should contain "1970"
    ASSERT_NOT_NULL(strstr(buf, "1970"));
}

TEST(datetime_format_custom) {
    XrDateTime dt = make_utc_dt(0, 0);
    char buf[64];
    int n = xr_datetime_format(&dt, "YYYY-MM-DD", buf, sizeof(buf));
    ASSERT_GT(n, 0);
    ASSERT_STR_EQ(buf, "1970-01-01");
}

/* ========== Local Offset ========== */

TEST(datetime_local_offset) {
    int offset = xr_datetime_local_offset();
    // Offset should be in reasonable range: -720 to +840 minutes
    ASSERT_TRUE(offset >= -720 && offset <= 840);
}

/* ========== To TM ========== */

TEST(datetime_to_tm) {
    XrDateTime dt = make_utc_dt(0, 0);
    struct tm tm;
    xr_datetime_to_tm(&dt, &tm);
    ASSERT_EQ_INT(tm.tm_year + 1900, 1970);
    ASSERT_EQ_INT(tm.tm_mon + 1, 1);
    ASSERT_EQ_INT(tm.tm_mday, 1);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("DateTime - Components");
RUN_TEST(datetime_epoch_components);
RUN_TEST(datetime_known_date);
RUN_TEST(datetime_y2k);

RUN_TEST_SUITE("DateTime - Weekday");
RUN_TEST(datetime_weekday);

RUN_TEST_SUITE("DateTime - Comparison");
RUN_TEST(datetime_compare);

RUN_TEST_SUITE("DateTime - Leap Year");
RUN_TEST(datetime_leap_year);

RUN_TEST_SUITE("DateTime - Days In Month");
RUN_TEST(datetime_days_in_month);

RUN_TEST_SUITE("DateTime - Format");
RUN_TEST(datetime_format_iso);
RUN_TEST(datetime_format_custom);

RUN_TEST_SUITE("DateTime - Utility");
RUN_TEST(datetime_local_offset);
RUN_TEST(datetime_to_tm);

TEST_MAIN_END()
