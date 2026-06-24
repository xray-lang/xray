/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_datetime_core.c - Unit tests for runtime-neutral DateTime core helpers
 */

#include "../test_framework.h"
#include "shared/xr_datetime_core.h"

TEST(datetime_core_days_in_month) {
    ASSERT_EQ_INT(xr_datetime_core_days_in_month(2024, 2), 29);
    ASSERT_EQ_INT(xr_datetime_core_days_in_month(2023, 2), 28);
    ASSERT_EQ_INT(xr_datetime_core_days_in_month(2000, 2), 29);
    ASSERT_EQ_INT(xr_datetime_core_days_in_month(1900, 2), 28);
    ASSERT_EQ_INT(xr_datetime_core_days_in_month(2026, 13), 30);
}

TEST(datetime_core_days_roundtrip) {
    int years[] = {1969, 1970, 1999, 2000, 2024};
    unsigned months[] = {1, 2, 3, 12};
    unsigned days[] = {1, 15, 28};

    for (size_t yi = 0; yi < sizeof(years) / sizeof(years[0]); yi++) {
        for (size_t mi = 0; mi < sizeof(months) / sizeof(months[0]); mi++) {
            for (size_t di = 0; di < sizeof(days) / sizeof(days[0]); di++) {
                int64_t z = xr_datetime_core_days_from_civil(years[yi], months[mi], days[di]);
                int y = 0;
                unsigned m = 0;
                unsigned d = 0;
                xr_datetime_core_civil_from_days(z, &y, &m, &d);
                ASSERT_EQ_INT(y, years[yi]);
                ASSERT_EQ_INT(m, months[mi]);
                ASSERT_EQ_INT(d, days[di]);
            }
        }
    }
}

TEST(datetime_core_epoch_boundaries) {
    ASSERT_EQ_INT(xr_datetime_core_days_from_civil(1970, 1, 1), 0);
    ASSERT_EQ_INT(xr_datetime_core_days_from_civil(1969, 12, 31), -1);

    struct tm tm = {0};
    xr_datetime_core_gmtime((time_t) -1, &tm);
    ASSERT_EQ_INT(tm.tm_year + 1900, 1969);
    ASSERT_EQ_INT(tm.tm_mon + 1, 12);
    ASSERT_EQ_INT(tm.tm_mday, 31);
    ASSERT_EQ_INT(tm.tm_hour, 23);
    ASSERT_EQ_INT(tm.tm_min, 59);
    ASSERT_EQ_INT(tm.tm_sec, 59);
    ASSERT_EQ_INT(tm.tm_wday, 3);
    ASSERT_EQ_INT(tm.tm_yday, 364);
}

TEST(datetime_core_utc_mktime) {
    struct tm tm = {0};
    tm.tm_year = 1969 - 1900;
    tm.tm_mon = 11;
    tm.tm_mday = 31;
    tm.tm_hour = 23;
    tm.tm_min = 59;
    tm.tm_sec = 59;
    ASSERT_EQ_INT(xr_datetime_core_mktime(&tm, 1), -1);

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 2000 - 1900;
    tm.tm_mon = 1;
    tm.tm_mday = 29;
    int64_t expected = xr_datetime_core_days_from_civil(2000, 2, 29) * 86400;
    ASSERT_EQ_INT(xr_datetime_core_mktime(&tm, 1), expected);
}

TEST(datetime_core_add_fields) {
    XrDateTimeCoreFields jan31 = {
        .timestamp = xr_datetime_core_days_from_civil(2024, 1, 31) * 86400,
        .milliseconds = 123,
        .tz_offset = 0,
        .is_utc = 1,
    };
    XrDateTimeCoreFields out;
    ASSERT_EQ_INT(xr_datetime_core_add_fields(&jan31, 1, "months", &out), 1);
    struct tm tm = {0};
    xr_datetime_core_to_tm_fields(&out, &tm);
    ASSERT_EQ_INT(tm.tm_year + 1900, 2024);
    ASSERT_EQ_INT(tm.tm_mon + 1, 2);
    ASSERT_EQ_INT(tm.tm_mday, 29);
    ASSERT_EQ_INT(out.milliseconds, 123);

    XrDateTimeCoreFields leap_day = {
        .timestamp = xr_datetime_core_days_from_civil(2024, 2, 29) * 86400,
        .milliseconds = 0,
        .tz_offset = 0,
        .is_utc = 1,
    };
    ASSERT_EQ_INT(xr_datetime_core_add_fields(&leap_day, 1, "year", &out), 1);
    xr_datetime_core_to_tm_fields(&out, &tm);
    ASSERT_EQ_INT(tm.tm_year + 1900, 2025);
    ASSERT_EQ_INT(tm.tm_mon + 1, 2);
    ASSERT_EQ_INT(tm.tm_mday, 28);

    XrDateTimeCoreFields ms = {
        .timestamp = 10,
        .milliseconds = 250,
        .tz_offset = 0,
        .is_utc = 1,
    };
    ASSERT_EQ_INT(xr_datetime_core_add_fields(&ms, -500, "milliseconds", &out), 1);
    ASSERT_EQ_INT(out.timestamp, 9);
    ASSERT_EQ_INT(out.milliseconds, 750);
    ASSERT_EQ_INT(xr_datetime_core_add_fields(&ms, 2, "bogus", &out), 0);
    ASSERT_EQ_INT(out.timestamp, 12);
    ASSERT_EQ_INT(out.milliseconds, 250);
}

TEST(datetime_core_diff_fields) {
    XrDateTimeCoreFields later = {
        .timestamp = 12,
        .milliseconds = 250,
        .tz_offset = 0,
        .is_utc = 1,
    };
    XrDateTimeCoreFields earlier = {
        .timestamp = 10,
        .milliseconds = 750,
        .tz_offset = 0,
        .is_utc = 1,
    };
    ASSERT_EQ_INT(xr_datetime_core_diff_fields(&later, &earlier, "milliseconds"), 1500);
    ASSERT_EQ_INT(xr_datetime_core_diff_fields(&later, &earlier, "seconds"), 1);
    ASSERT_EQ_INT(xr_datetime_core_diff_fields(&later, &earlier, "bogus"), 1);

    later.timestamp = 10 * 86400;
    later.milliseconds = 0;
    earlier.timestamp = 3 * 86400;
    earlier.milliseconds = 0;
    ASSERT_EQ_INT(xr_datetime_core_diff_fields(&later, &earlier, "days"), 7);
    ASSERT_EQ_INT(xr_datetime_core_diff_fields(&later, &earlier, "weeks"), 1);
}

TEST(datetime_core_timestamp_and_timezone_rules) {
    XrDateTimeCoreFields ms = xr_datetime_core_from_timestamp_ms(-1001);
    ASSERT_EQ_INT(ms.timestamp, -2);
    ASSERT_EQ_INT(ms.milliseconds, 999);
    ASSERT_EQ_INT(ms.tz_offset, 0);
    ASSERT_EQ_INT(ms.is_utc, 1);

    XrDateTimeCoreFields same = xr_datetime_core_from_timestamp_ms(-1001);
    XrDateTimeCoreFields later = xr_datetime_core_from_timestamp_ms(-1000);
    ASSERT_EQ_INT(xr_datetime_core_compare_fields(&ms, &same), 0);
    ASSERT_TRUE(xr_datetime_core_compare_fields(&ms, &later) < 0);
    ASSERT_TRUE(xr_datetime_core_compare_fields(&later, &ms) > 0);

    XrDateTimeCoreFields utc;
    ASSERT_TRUE(xr_datetime_core_to_utc_fields(&ms, &utc));
    ASSERT_EQ_INT(utc.timestamp, -2);
    ASSERT_EQ_INT(utc.milliseconds, 999);
    ASSERT_EQ_INT(utc.tz_offset, 0);
    ASSERT_EQ_INT(utc.is_utc, 1);

    XrDateTimeCoreFields local;
    ASSERT_TRUE(xr_datetime_core_to_local_fields(&utc, &local));
    ASSERT_EQ_INT(local.timestamp, utc.timestamp);
    ASSERT_EQ_INT(local.milliseconds, utc.milliseconds);
    ASSERT_EQ_INT(local.is_utc, 0);
    ASSERT_EQ_INT(local.tz_offset, xr_datetime_core_local_offset_at((time_t) utc.timestamp));
}

TEST(datetime_core_int_argument_rules) {
    int64_t value = 99;
    ASSERT_EQ_INT(xr_datetime_core_int_arg_or(true, 42, 1970), 42);
    ASSERT_EQ_INT(xr_datetime_core_int_arg_or(false, 42, 1970), 1970);

    ASSERT_TRUE(xr_datetime_core_required_int_arg(true, -7, &value));
    ASSERT_EQ_INT(value, -7);

    value = 99;
    ASSERT_FALSE(xr_datetime_core_required_int_arg(false, 123, &value));
    ASSERT_EQ_INT(value, 0);
    ASSERT_FALSE(xr_datetime_core_required_int_arg(true, 123, NULL));
}

TEST(datetime_core_parse_fields) {
    XrDateTimeCoreFields fields;
    ASSERT_TRUE(xr_datetime_core_parse_fields("2024-01-15T10:30:45.123Z", NULL, &fields));
    ASSERT_EQ_INT(fields.timestamp,
                  xr_datetime_core_days_from_civil(2024, 1, 15) * 86400 + 10 * 3600 + 30 * 60 + 45);
    ASSERT_EQ_INT(fields.milliseconds, 123);
    ASSERT_EQ_INT(fields.tz_offset, 0);
    ASSERT_EQ_INT(fields.is_utc, 1);

    ASSERT_TRUE(xr_datetime_core_parse_fields("2024-01-15T10:30:45.5+02:30", "iso", &fields));
    ASSERT_EQ_INT(fields.timestamp,
                  xr_datetime_core_days_from_civil(2024, 1, 15) * 86400 + 8 * 3600 + 45);
    ASSERT_EQ_INT(fields.milliseconds, 500);
    ASSERT_EQ_INT(fields.is_utc, 1);

    ASSERT_TRUE(xr_datetime_core_parse_fields("2024/01/15", "date", &fields));
    struct tm tm = {0};
    xr_datetime_core_to_tm_fields(&fields, &tm);
    ASSERT_EQ_INT(tm.tm_year + 1900, 2024);
    ASSERT_EQ_INT(tm.tm_mon + 1, 1);
    ASSERT_EQ_INT(tm.tm_mday, 15);

    ASSERT_TRUE(xr_datetime_core_parse_fields("07:32", "time", &fields));
    xr_datetime_core_to_tm_fields(&fields, &tm);
    ASSERT_EQ_INT(tm.tm_year + 1900, 1970);
    ASSERT_EQ_INT(tm.tm_mon + 1, 1);
    ASSERT_EQ_INT(tm.tm_mday, 1);
    ASSERT_EQ_INT(tm.tm_hour, 7);
    ASSERT_EQ_INT(tm.tm_min, 32);

    ASSERT_TRUE(!xr_datetime_core_parse_fields("not-a-date", NULL, &fields));
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("DateTime Core");
RUN_TEST(datetime_core_days_in_month);
RUN_TEST(datetime_core_days_roundtrip);
RUN_TEST(datetime_core_epoch_boundaries);
RUN_TEST(datetime_core_utc_mktime);
RUN_TEST(datetime_core_add_fields);
RUN_TEST(datetime_core_diff_fields);
RUN_TEST(datetime_core_timestamp_and_timezone_rules);
RUN_TEST(datetime_core_int_argument_rules);
RUN_TEST(datetime_core_parse_fields);

TEST_MAIN_END()
