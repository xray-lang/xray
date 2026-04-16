/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_semver.c - Unit tests for semantic versioning module
 */

#include "../test_framework.h"
#include "module/xsemver.h"
#include "base/xmalloc.h"

/* ========== Version Parsing Tests ========== */

TEST(semver_parse_basic) {
    XrSemVer ver;
    ASSERT_TRUE(xr_semver_parse("1.2.3", &ver));
    ASSERT_EQ_INT(ver.major, 1);
    ASSERT_EQ_INT(ver.minor, 2);
    ASSERT_EQ_INT(ver.patch, 3);
    ASSERT_NULL(ver.prerelease);
    ASSERT_NULL(ver.build);
    xr_semver_free(&ver);
}

TEST(semver_parse_with_v_prefix) {
    XrSemVer ver;
    ASSERT_TRUE(xr_semver_parse("v2.0.1", &ver));
    ASSERT_EQ_INT(ver.major, 2);
    ASSERT_EQ_INT(ver.minor, 0);
    ASSERT_EQ_INT(ver.patch, 1);
    xr_semver_free(&ver);
}

TEST(semver_parse_with_prerelease) {
    XrSemVer ver;
    ASSERT_TRUE(xr_semver_parse("1.0.0-alpha.1", &ver));
    ASSERT_EQ_INT(ver.major, 1);
    ASSERT_EQ_INT(ver.minor, 0);
    ASSERT_EQ_INT(ver.patch, 0);
    ASSERT_STR_EQ(ver.prerelease, "alpha.1");
    ASSERT_NULL(ver.build);
    xr_semver_free(&ver);
}

TEST(semver_parse_with_build) {
    XrSemVer ver;
    ASSERT_TRUE(xr_semver_parse("1.0.0+build.123", &ver));
    ASSERT_EQ_INT(ver.major, 1);
    ASSERT_STR_EQ(ver.build, "build.123");
    xr_semver_free(&ver);
}

TEST(semver_parse_with_prerelease_and_build) {
    XrSemVer ver;
    ASSERT_TRUE(xr_semver_parse("1.0.0-beta.2+sha.456", &ver));
    ASSERT_STR_EQ(ver.prerelease, "beta.2");
    ASSERT_STR_EQ(ver.build, "sha.456");
    xr_semver_free(&ver);
}

TEST(semver_parse_major_only) {
    XrSemVer ver;
    ASSERT_TRUE(xr_semver_parse("5", &ver));
    ASSERT_EQ_INT(ver.major, 5);
    ASSERT_EQ_INT(ver.minor, 0);
    ASSERT_EQ_INT(ver.patch, 0);
    xr_semver_free(&ver);
}

TEST(semver_parse_major_minor_only) {
    XrSemVer ver;
    ASSERT_TRUE(xr_semver_parse("3.7", &ver));
    ASSERT_EQ_INT(ver.major, 3);
    ASSERT_EQ_INT(ver.minor, 7);
    ASSERT_EQ_INT(ver.patch, 0);
    xr_semver_free(&ver);
}

TEST(semver_parse_zero) {
    XrSemVer ver;
    ASSERT_TRUE(xr_semver_parse("0.0.0", &ver));
    ASSERT_EQ_INT(ver.major, 0);
    ASSERT_EQ_INT(ver.minor, 0);
    ASSERT_EQ_INT(ver.patch, 0);
    xr_semver_free(&ver);
}

TEST(semver_parse_invalid_leading_zero) {
    XrSemVer ver;
    ASSERT_FALSE(xr_semver_parse("01.2.3", &ver));
}

TEST(semver_parse_invalid_empty) {
    XrSemVer ver;
    ASSERT_FALSE(xr_semver_parse("", &ver));
}

TEST(semver_parse_invalid_null) {
    XrSemVer ver;
    ASSERT_FALSE(xr_semver_parse(NULL, &ver));
}

TEST(semver_parse_invalid_trailing) {
    XrSemVer ver;
    ASSERT_FALSE(xr_semver_parse("1.2.3.4", &ver));
}

TEST(semver_is_valid) {
    ASSERT_TRUE(xr_semver_is_valid("1.0.0"));
    ASSERT_TRUE(xr_semver_is_valid("0.1.0-alpha"));
    ASSERT_FALSE(xr_semver_is_valid(""));
    ASSERT_FALSE(xr_semver_is_valid("abc"));
}

/* ========== Version Comparison Tests ========== */

TEST(semver_compare_equal) {
    XrSemVer a, b;
    xr_semver_parse("1.2.3", &a);
    xr_semver_parse("1.2.3", &b);
    ASSERT_EQ_INT(xr_semver_compare(&a, &b), 0);
    xr_semver_free(&a);
    xr_semver_free(&b);
}

TEST(semver_compare_major) {
    XrSemVer a, b;
    xr_semver_parse("2.0.0", &a);
    xr_semver_parse("1.9.9", &b);
    ASSERT_GT(xr_semver_compare(&a, &b), 0);
    ASSERT_LT(xr_semver_compare(&b, &a), 0);
    xr_semver_free(&a);
    xr_semver_free(&b);
}

TEST(semver_compare_minor) {
    XrSemVer a, b;
    xr_semver_parse("1.3.0", &a);
    xr_semver_parse("1.2.9", &b);
    ASSERT_GT(xr_semver_compare(&a, &b), 0);
    xr_semver_free(&a);
    xr_semver_free(&b);
}

TEST(semver_compare_patch) {
    XrSemVer a, b;
    xr_semver_parse("1.2.4", &a);
    xr_semver_parse("1.2.3", &b);
    ASSERT_GT(xr_semver_compare(&a, &b), 0);
    xr_semver_free(&a);
    xr_semver_free(&b);
}

TEST(semver_compare_prerelease_vs_release) {
    XrSemVer a, b;
    xr_semver_parse("1.0.0", &a);
    xr_semver_parse("1.0.0-alpha", &b);
    // Release > prerelease
    ASSERT_GT(xr_semver_compare(&a, &b), 0);
    xr_semver_free(&a);
    xr_semver_free(&b);
}

TEST(semver_compare_prerelease_numeric) {
    XrSemVer a, b;
    xr_semver_parse("1.0.0-2", &a);
    xr_semver_parse("1.0.0-10", &b);
    // Numeric: 2 < 10
    ASSERT_LT(xr_semver_compare(&a, &b), 0);
    xr_semver_free(&a);
    xr_semver_free(&b);
}

TEST(semver_compare_no_overflow) {
    // Regression test for integer overflow in compare
    XrSemVer a = { .major = 2147483647, .minor = 0, .patch = 0 };
    XrSemVer b = { .major = 0, .minor = 0, .patch = 0 };
    ASSERT_GT(xr_semver_compare(&a, &b), 0);
    ASSERT_LT(xr_semver_compare(&b, &a), 0);
}

/* ========== Version to String Tests ========== */

TEST(semver_to_string_basic) {
    XrSemVer ver;
    xr_semver_parse("1.2.3", &ver);
    char buf[64];
    xr_semver_to_string(&ver, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "1.2.3");
    xr_semver_free(&ver);
}

TEST(semver_to_string_with_prerelease) {
    XrSemVer ver;
    xr_semver_parse("1.0.0-alpha", &ver);
    char buf[64];
    xr_semver_to_string(&ver, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "1.0.0-alpha");
    xr_semver_free(&ver);
}

/* ========== Constraint Parsing Tests ========== */

TEST(constraint_parse_caret) {
    XrVersionConstraint c;
    ASSERT_TRUE(xr_constraint_parse("^1.2.3", &c));
    ASSERT_EQ_INT(c.op, SEMVER_OP_CARET);
    ASSERT_EQ_INT(c.version.major, 1);
    ASSERT_EQ_INT(c.version.minor, 2);
    ASSERT_EQ_INT(c.version.patch, 3);
    xr_constraint_free(&c);
}

TEST(constraint_parse_tilde) {
    XrVersionConstraint c;
    ASSERT_TRUE(xr_constraint_parse("~1.2.0", &c));
    ASSERT_EQ_INT(c.op, SEMVER_OP_TILDE);
    xr_constraint_free(&c);
}

TEST(constraint_parse_ge) {
    XrVersionConstraint c;
    ASSERT_TRUE(xr_constraint_parse(">=2.0.0", &c));
    ASSERT_EQ_INT(c.op, SEMVER_OP_GE);
    ASSERT_EQ_INT(c.version.major, 2);
    xr_constraint_free(&c);
}

TEST(constraint_parse_any) {
    XrVersionConstraint c;
    ASSERT_TRUE(xr_constraint_parse("*", &c));
    ASSERT_EQ_INT(c.op, SEMVER_OP_ANY);
    xr_constraint_free(&c);
}

TEST(constraint_parse_any_with_trailing_rejected) {
    // Regression: "*foo" should not parse as valid
    XrVersionConstraint c;
    ASSERT_FALSE(xr_constraint_parse("*foo", &c));
}

TEST(constraint_parse_exact) {
    XrVersionConstraint c;
    ASSERT_TRUE(xr_constraint_parse("1.0.0", &c));
    ASSERT_EQ_INT(c.op, SEMVER_OP_EQ);
    xr_constraint_free(&c);
}

/* ========== Constraint Matching Tests ========== */

TEST(constraint_match_caret_major) {
    // ^1.2.3 matches 1.2.3 <= v < 2.0.0
    XrVersionConstraint c;
    xr_constraint_parse("^1.2.3", &c);

    XrSemVer v1, v2, v3, v4;
    xr_semver_parse("1.2.3", &v1);
    xr_semver_parse("1.9.9", &v2);
    xr_semver_parse("2.0.0", &v3);
    xr_semver_parse("1.2.2", &v4);

    ASSERT_TRUE(xr_constraint_matches(&v1, &c));
    ASSERT_TRUE(xr_constraint_matches(&v2, &c));
    ASSERT_FALSE(xr_constraint_matches(&v3, &c));
    ASSERT_FALSE(xr_constraint_matches(&v4, &c));

    xr_semver_free(&v1);
    xr_semver_free(&v2);
    xr_semver_free(&v3);
    xr_semver_free(&v4);
    xr_constraint_free(&c);
}

TEST(constraint_match_caret_zero_minor) {
    // ^0.2.3 matches 0.2.3 <= v < 0.3.0
    XrVersionConstraint c;
    xr_constraint_parse("^0.2.3", &c);

    XrSemVer v1, v2, v3;
    xr_semver_parse("0.2.5", &v1);
    xr_semver_parse("0.3.0", &v2);
    xr_semver_parse("0.2.3", &v3);

    ASSERT_TRUE(xr_constraint_matches(&v1, &c));
    ASSERT_FALSE(xr_constraint_matches(&v2, &c));
    ASSERT_TRUE(xr_constraint_matches(&v3, &c));

    xr_semver_free(&v1);
    xr_semver_free(&v2);
    xr_semver_free(&v3);
    xr_constraint_free(&c);
}

TEST(constraint_match_caret_zero_zero) {
    // ^0.0.3 matches exactly 0.0.3
    XrVersionConstraint c;
    xr_constraint_parse("^0.0.3", &c);

    XrSemVer v1, v2;
    xr_semver_parse("0.0.3", &v1);
    xr_semver_parse("0.0.4", &v2);

    ASSERT_TRUE(xr_constraint_matches(&v1, &c));
    ASSERT_FALSE(xr_constraint_matches(&v2, &c));

    xr_semver_free(&v1);
    xr_semver_free(&v2);
    xr_constraint_free(&c);
}

TEST(constraint_match_tilde) {
    // ~1.2.3 matches 1.2.3 <= v < 1.3.0
    XrVersionConstraint c;
    xr_constraint_parse("~1.2.3", &c);

    XrSemVer v1, v2, v3;
    xr_semver_parse("1.2.5", &v1);
    xr_semver_parse("1.3.0", &v2);
    xr_semver_parse("1.2.3", &v3);

    ASSERT_TRUE(xr_constraint_matches(&v1, &c));
    ASSERT_FALSE(xr_constraint_matches(&v2, &c));
    ASSERT_TRUE(xr_constraint_matches(&v3, &c));

    xr_semver_free(&v1);
    xr_semver_free(&v2);
    xr_semver_free(&v3);
    xr_constraint_free(&c);
}

TEST(constraint_match_any) {
    XrVersionConstraint c;
    xr_constraint_parse("*", &c);

    XrSemVer v;
    xr_semver_parse("99.99.99", &v);
    ASSERT_TRUE(xr_constraint_matches(&v, &c));
    xr_semver_free(&v);
    xr_constraint_free(&c);
}

TEST(constraint_match_ge) {
    XrVersionConstraint c;
    xr_constraint_parse(">=1.5.0", &c);

    XrSemVer v1, v2;
    xr_semver_parse("1.5.0", &v1);
    xr_semver_parse("1.4.9", &v2);

    ASSERT_TRUE(xr_constraint_matches(&v1, &c));
    ASSERT_FALSE(xr_constraint_matches(&v2, &c));

    xr_semver_free(&v1);
    xr_semver_free(&v2);
    xr_constraint_free(&c);
}

/* ========== Select Best Version Tests ========== */

TEST(semver_select_best) {
    XrSemVer versions[4];
    xr_semver_parse("1.0.0", &versions[0]);
    xr_semver_parse("1.2.0", &versions[1]);
    xr_semver_parse("1.5.0", &versions[2]);
    xr_semver_parse("2.0.0", &versions[3]);

    XrVersionConstraint c;
    xr_constraint_parse("^1.0.0", &c);

    int best = xr_semver_select_best(versions, 4, &c);
    ASSERT_EQ_INT(best, 2);  // 1.5.0 is best match for ^1.0.0

    for (int i = 0; i < 4; i++) xr_semver_free(&versions[i]);
    xr_constraint_free(&c);
}

TEST(semver_select_best_no_match) {
    XrSemVer versions[2];
    xr_semver_parse("2.0.0", &versions[0]);
    xr_semver_parse("3.0.0", &versions[1]);

    XrVersionConstraint c;
    xr_constraint_parse("^1.0.0", &c);

    int best = xr_semver_select_best(versions, 2, &c);
    ASSERT_EQ_INT(best, -1);

    for (int i = 0; i < 2; i++) xr_semver_free(&versions[i]);
    xr_constraint_free(&c);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("SemVer Parsing");
    RUN_TEST(semver_parse_basic);
    RUN_TEST(semver_parse_with_v_prefix);
    RUN_TEST(semver_parse_with_prerelease);
    RUN_TEST(semver_parse_with_build);
    RUN_TEST(semver_parse_with_prerelease_and_build);
    RUN_TEST(semver_parse_major_only);
    RUN_TEST(semver_parse_major_minor_only);
    RUN_TEST(semver_parse_zero);
    RUN_TEST(semver_parse_invalid_leading_zero);
    RUN_TEST(semver_parse_invalid_empty);
    RUN_TEST(semver_parse_invalid_null);
    RUN_TEST(semver_parse_invalid_trailing);
    RUN_TEST(semver_is_valid);

    RUN_TEST_SUITE("SemVer Comparison");
    RUN_TEST(semver_compare_equal);
    RUN_TEST(semver_compare_major);
    RUN_TEST(semver_compare_minor);
    RUN_TEST(semver_compare_patch);
    RUN_TEST(semver_compare_prerelease_vs_release);
    RUN_TEST(semver_compare_prerelease_numeric);
    RUN_TEST(semver_compare_no_overflow);

    RUN_TEST_SUITE("SemVer to String");
    RUN_TEST(semver_to_string_basic);
    RUN_TEST(semver_to_string_with_prerelease);

    RUN_TEST_SUITE("Constraint Parsing");
    RUN_TEST(constraint_parse_caret);
    RUN_TEST(constraint_parse_tilde);
    RUN_TEST(constraint_parse_ge);
    RUN_TEST(constraint_parse_any);
    RUN_TEST(constraint_parse_any_with_trailing_rejected);
    RUN_TEST(constraint_parse_exact);

    RUN_TEST_SUITE("Constraint Matching");
    RUN_TEST(constraint_match_caret_major);
    RUN_TEST(constraint_match_caret_zero_minor);
    RUN_TEST(constraint_match_caret_zero_zero);
    RUN_TEST(constraint_match_tilde);
    RUN_TEST(constraint_match_any);
    RUN_TEST(constraint_match_ge);

    RUN_TEST_SUITE("Select Best Version");
    RUN_TEST(semver_select_best);
    RUN_TEST(semver_select_best_no_match);

TEST_MAIN_END()
