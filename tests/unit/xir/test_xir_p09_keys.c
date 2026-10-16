/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_keys.c - Key classes, finite work and borrowed owner lifetimes
 */
#define XR_COMPILE_DOMAIN_ENTRY original_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
static void key_pair(XrXirValue a, XrXirValue b, bool expected, XrXirValueAdmission *admission) {
    bool equal = !expected; uint64_t first = 0, second = 0;
    CHECK(xr_xir_key_equal(&a, &b, admission, &equal) == XR_XIR_VALUE_OK && equal == expected);
    CHECK(xr_xir_key_equal(&b, &a, admission, &equal) == XR_XIR_VALUE_OK && equal == expected);
    CHECK(xr_xir_key_hash(&a, admission, &first) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_key_hash(&b, admission, &second) == XR_XIR_VALUE_OK);
    if (expected) CHECK(first == second);
}
static void key_scalar_cases(XrXirValueAdmission *admission) {
    const XrXirType integers[] = {XR_XIR_I8, XR_XIR_I16, XR_XIR_I32, XR_XIR_I64,
        XR_XIR_U8, XR_XIR_U16, XR_XIR_U32, XR_XIR_U64};
    for (unsigned i = 0; i < sizeof(integers) / sizeof(integers[0]); ++i) {
        XrXirValue first = {integers[i], 0, 17}, second = {integers[i], 0, 18};
        key_pair(first, first, true, admission); key_pair(first, second, false, admission);
        first.payload = xr_xir_integer_signed(integers[i]) ? -1 : 0;
        key_pair(first, first, true, admission);
    }
    key_pair((XrXirValue){XR_XIR_BOOL, 0, 0}, (XrXirValue){XR_XIR_BOOL, 0, 1}, false, admission);
    key_pair((XrXirValue){XR_XIR_RUNE, 0, 0x10ffff}, (XrXirValue){XR_XIR_RUNE, 0, 0x10ffff}, true, admission);
    key_pair((XrXirValue){XR_XIR_F32, 0, 0}, (XrXirValue){XR_XIR_F32, 0, UINT32_C(0x80000000)}, true, admission);
    key_pair((XrXirValue){XR_XIR_F64, 0, 0}, (XrXirValue){XR_XIR_F64, 0, INT64_MIN}, true, admission);
    const uint64_t nans[] = {UINT64_C(0x7ff8000000000000), UINT64_C(0x7ff0000000000001),
        UINT64_C(0x7fffffffffffffff), UINT64_C(0xfff8000000000000), UINT64_C(0xfff0000000000001)};
    for (unsigned i = 0; i < sizeof(nans) / sizeof(nans[0]); ++i) {
        XrXirValue nan = {XR_XIR_F64, 0, (int64_t)nans[i]}, canonical = {XR_XIR_F64, 0, (int64_t)nans[0]};
        key_pair(nan, canonical, true, admission); key_pair(nan, nan, true, admission);
        bool ordinary = true;
        CHECK(xr_xir_value_equal(&nan, &nan, XR_XIR_F64, admission, &ordinary) == XR_XIR_VALUE_OK && !ordinary);
    }
    key_pair((XrXirValue){XR_XIR_F32, 0, UINT32_C(0x7fc00000)},
        (XrXirValue){XR_XIR_F32, 0, UINT32_C(0x7fc00000)}, true, admission);
    key_pair((XrXirValue){XR_XIR_F64, 0, INT64_C(0x7ff0000000000000)},
        (XrXirValue){XR_XIR_F64, 0, (int64_t)UINT64_C(0xfff0000000000000)}, false, admission);
}
static void key_string_cases(XrXirValueAdmission *admission) {
    XrXirDomain *producer = NULL; XrXirValue values[4] = {{0}};
    CHECK(xr_xir_domain_new(65536, &producer) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(producer, "a\0\xce\xa9", 4, &values[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(admission->domain, "a\0\xce\xa9", 4, &values[1]) == XR_XIR_VALUE_OK);
    CHECK(values[0].payload != values[1].payload);
    CHECK(xr_xir_string_new(producer, "a\0\xce\xb2", 4, &values[2]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(producer, "", 0, &values[3]) == XR_XIR_VALUE_OK);
    xr_xir_domain_close(producer); xr_xir_domain_drop(producer);
    size_t before = calls; fail_at = calls;
    key_pair(values[0], values[1], true, admission); key_pair(values[0], values[2], false, admission);
    key_pair(values[0], values[3], false, admission); key_pair(values[3], values[3], true, admission);
    CHECK(calls == before);
    for (unsigned i = 0; i < 4; ++i) xr_xir_value_drop(&values[i]);
    fail_at = SIZE_MAX; CHECK(calls == before);
}
static void key_failure_cases(XrXirValueAdmission *admission) {
    XrXirValue good = {XR_XIR_I64, 0, 7}, bad = {XR_XIR_BOOL, 0, 2}, unsupported = {XR_XIR_UNIT, 0, 0};
    uint64_t hash = 99; bool equal = true; uint64_t work = admission->work;
    CHECK(xr_xir_key_hash(&good, NULL, &hash) == XR_XIR_VALUE_BAD_ARGUMENT && hash == 99);
    CHECK(xr_xir_key_equal(&good, &good, NULL, &equal) == XR_XIR_VALUE_BAD_ARGUMENT && equal);
    CHECK(xr_xir_key_hash(&bad, admission, &hash) == XR_XIR_VALUE_BAD_ARGUMENT && hash == 99);
    CHECK(xr_xir_key_equal(&good, &bad, admission, &equal) == XR_XIR_VALUE_BAD_ARGUMENT && equal);
    CHECK(xr_xir_key_hash(&unsupported, admission, &hash) == XR_XIR_VALUE_UNSUPPORTED && hash == 99);
    CHECK(xr_xir_key_equal(&unsupported, &unsupported, admission, &equal) == XR_XIR_VALUE_UNSUPPORTED && equal);
    CHECK(admission->work == work);
    admission->work = 8;
    CHECK(xr_xir_key_hash(&good, admission, &hash) == XR_XIR_VALUE_LIMIT && hash == 99 && admission->work == 8);
    admission->work = 0;
    CHECK(xr_xir_key_equal(&good, &good, admission, &equal) == XR_XIR_VALUE_LIMIT && equal);
    admission->work = work;
}
static void key_domain_boundaries(void) {
    for (unsigned shortfall = 0; shortfall < 2; ++shortfall) {
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
        XrXirDomainBudgetControls controls = {65536, 65536, 1000, 65536, 65536};
        CHECK(xr_xir_domain_budget_bind(domain, &controls) == XR_XIR_VALUE_OK);
        uint64_t used = xr_xir_domain_budget_stats(domain).work;
        CHECK(xr_xir_domain_work(domain, 1000 - used - 9 + shortfall));
        XrXirValueAdmission admission = {NULL, domain, NULL, NULL, 9, 0};
        XrXirValue value = {XR_XIR_I64, 0, 7}; uint64_t hash = 99;
        CHECK(xr_xir_key_hash(&value, &admission, &hash) == (shortfall ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_OK));
        CHECK(!admission.work && (!shortfall || hash == 99));
        CHECK(xr_xir_domain_budget_stats(domain).work == (shortfall ? 992 : 1000));
        xr_xir_domain_drop(domain); CHECK(!live);
    }
}
int main(void) {
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {NULL, domain, NULL, NULL, 100000, 0};
    size_t before = calls; fail_at = calls;
    key_scalar_cases(&admission); key_failure_cases(&admission);
    fail_at = SIZE_MAX; CHECK(calls == before);
    key_string_cases(&admission); xr_xir_domain_drop(domain); CHECK(!live);
    key_domain_boundaries(); CHECK(!value_compile_live && !value_compile_bytes);
    puts("Built-in typed key equivalence/hash: IEEE classes, full bytes, finite work and final physical0 PASS");
    return 0;
}
