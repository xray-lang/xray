/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_panic_carrier_value_cases.inc.c - Byte-exact messages and atomic factory failures
 */
static void value_lifetime(const char *bytes, size_t length) {
    XrXirDomain *source = NULL, *destination = NULL;
    CHECK(xr_xir_domain_new(1048576, &source) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576, &destination) == XR_XIR_VALUE_OK);
    XrXirValue message = {0}, info = {0}, alias = {0}, suffix = {0};
    CHECK(xr_xir_string_new(source, bytes, length, &message) == XR_XIR_VALUE_OK);
    XrXirPanicPayload borrowed = {{445, 0, 0, 0}, message}, copy = {0}, moved = {0};
    size_t no_allocations = runtime_attempts;
    CHECK(xr_xir_panic_copy(&borrowed, &copy) == XR_XIR_VALUE_OK);
    xr_xir_panic_move(&copy, &moved); CHECK(xr_xir_panic_empty(&copy));
    CHECK(runtime_attempts == no_allocations);
    uint64_t before = xr_xir_domain_stats(destination).live_bytes;
    CHECK(xr_xir_panic_info_new(destination, &borrowed, &info) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_stats(destination).live_bytes == before + 80);
    CHECK(xr_xir_value_copy(&info, &alias) == XR_XIR_VALUE_OK && alias.payload == info.payload);
    CHECK(xr_xir_string_new(source, "!", 1, &suffix) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_append(&message, &suffix) == XR_XIR_VALUE_OK);
    expect_bytes(&moved.message, bytes, length);
    xr_xir_value_drop(&suffix); xr_xir_value_drop(&message); xr_xir_panic_drop(&moved);
    xr_xir_domain_drop(source); xr_xir_domain_drop(destination);
    XrXirValue extracted = {0};
    no_allocations = runtime_attempts;
    CHECK(xr_xir_panic_info_message(&alias, &extracted) == XR_XIR_VALUE_OK);
    CHECK(runtime_attempts == no_allocations);
    xr_xir_value_drop(&info); xr_xir_value_drop(&alias);
    expect_bytes(&extracted, bytes, length); xr_xir_value_drop(&extracted);
    physical_empty();
}
static void value_failures(void) {
    XrXirDomain *domain = NULL; XrXirValue message = {0};
    CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, expected_bytes, sizeof(expected_bytes), &message) == XR_XIR_VALUE_OK);
    XrXirPanicPayload borrowed = {{445, 0, 0, 0}, message};
    size_t baseline_count = runtime_live, baseline_bytes = runtime_bytes;
    XirObject *object = object_pointer(&message);
    uint32_t references = atomic_load(&object->references), domain_references = atomic_load(&domain->references);
    atomic_store(&object->references, UINT32_MAX);
    XrXirPanicPayload copy = {0}; XrXirValue info = {0}; XrXirCallResult out = {0};
    CHECK(xr_xir_panic_copy(&borrowed, &copy) == XR_XIR_VALUE_REFCOUNT_LIMIT && xr_xir_panic_empty(&copy));
    CHECK(xr_xir_panic_info_new(domain, &borrowed, &info) == XR_XIR_VALUE_REFCOUNT_LIMIT && unit_value(&info));
    CHECK(atomic_load(&domain->references) == domain_references);
    XrXirCallResult result = {XR_XIR_CALL_ASSERTION, {0}, 0, borrowed};
    CHECK(xr_xir_call_result_copy(&result, &out) == XR_XIR_VALUE_REFCOUNT_LIMIT && xr_xir_call_result_empty(&out));
    atomic_store(&object->references, references);
    runtime_attempts = 0;
    CHECK(xr_xir_panic_info_new(domain, &borrowed, &info) == XR_XIR_VALUE_OK);
    size_t sites = runtime_attempts; CHECK(sites == 1);
    xr_xir_value_drop(&info);
    for (size_t i = 0; i < sites; ++i) {
        runtime_attempts = 0; runtime_fail_at = i;
        CHECK(xr_xir_panic_info_new(domain, &borrowed, &info) == XR_XIR_VALUE_OOM && unit_value(&info));
        CHECK(runtime_live == baseline_count && runtime_bytes == baseline_bytes);
        CHECK(atomic_load(&object->references) == references && atomic_load(&domain->references) == domain_references);
        runtime_fail_at = SIZE_MAX;
    }
    uint64_t old_limit = domain->limit;
    domain->limit = xr_xir_domain_stats(domain).live_bytes + 79;
    CHECK(xr_xir_panic_info_new(domain, &borrowed, &info) == XR_XIR_VALUE_LIMIT && unit_value(&info));
    CHECK(runtime_live == baseline_count && runtime_bytes == baseline_bytes);
    domain->limit++;
    CHECK(xr_xir_panic_info_new(domain, &borrowed, &info) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&info); domain->limit = old_limit;
    for (unsigned i = 0; i < 6; ++i) {
        XrXirPanicPayload invalid = borrowed;
        if (i == 0) invalid.message = (XrXirValue){0};
        if (i == 1) invalid.message = (XrXirValue){XR_XIR_I64, 0, 1};
        if (i == 2) invalid.detail.reserved = 1;
        if (i == 3) invalid.detail.index = -1;
        if (i == 4) invalid.detail.length = 1;
        if (i == 5) invalid.detail.code = 442;
        CHECK(!xr_xir_panic_valid(&invalid));
        CHECK(xr_xir_panic_info_new(domain, &invalid, &info) == XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&info));
    }
    result.value = (XrXirValue){XR_XIR_I64, 0, 7};
    CHECK(!xr_xir_call_result_valid(&result));
    CHECK(xr_xir_call_result_copy(&result, &out) == XR_XIR_VALUE_BAD_ARGUMENT && xr_xir_call_result_empty(&out));
    CHECK(runtime_live == baseline_count && runtime_bytes == baseline_bytes);
    xr_xir_value_drop(&message); xr_xir_domain_drop(domain); physical_empty();
}
static void value_cases(void) {
    static char long_bytes[65537];
    for (size_t i = 0; i < sizeof(long_bytes); ++i) long_bytes[i] = (char)('a' + i % 26);
    long_bytes[1] = 0; long_bytes[65535] = 0;
    value_lifetime(NULL, 0); value_lifetime(expected_bytes, sizeof(expected_bytes));
    value_lifetime(long_bytes, sizeof(long_bytes)); value_failures();
    _Static_assert(sizeof(XrXirFaultDetail) == 24 && sizeof(XrXirPanicPayload) == 40 && sizeof(XirPanicInfo) == 80, "panic ABI");
    _Static_assert(sizeof(XrXirAction) == 88 && sizeof(XrXirCallResult) == 72 && sizeof(XrXirCallView) == 216, "call ABI");
    _Static_assert(sizeof(XrXirInstanceResult) == 80 && XR_XIR_CALL_ASSERTION == 19, "instance ABI");
}
