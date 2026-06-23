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

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("DateTime Core");
RUN_TEST(datetime_core_days_in_month);
RUN_TEST(datetime_core_days_roundtrip);
RUN_TEST(datetime_core_epoch_boundaries);
RUN_TEST(datetime_core_utc_mktime);

TEST_MAIN_END()
