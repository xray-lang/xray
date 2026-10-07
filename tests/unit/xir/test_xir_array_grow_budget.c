/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_grow_budget.c - Cumulative requests for actual Array reallocations
 *
 * KEY CONCEPT:
 *   Exact and minus-one resource limits preserve the old contents on failure.
 */
#define main original_value_allocation_main
#include "test_xir_value_allocations.c"
#undef main

typedef struct GrowFixture {
    XrXirDomain *domain;
    XrXirDomain *receiving;
    XrXirTypeArena *arena;
    XrXirValue array;
    XrXirValueAdmission admission;
    XrXirDomainBudgetStats budget;
} GrowFixture;

static GrowFixture grow_fixture(uint64_t requested_limit, uint64_t work_limit) {
    GrowFixture fixture = {0};
    XrXirDomainBudgetControls controls = {requested_limit, 65536, work_limit, 65536, 65536};
    CHECK(xr_xir_domain_new_budgeted(65536, &controls, &fixture.domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &fixture.receiving) == XR_XIR_VALUE_OK);
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64};
    XrXirTypes types = {&node, 1, NULL, NULL};
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(65536, 0, 10000),
        &fixture.arena) == XR_XIR_VALUE_OK);
    fixture.admission = (XrXirValueAdmission){fixture.arena, fixture.domain, NULL, NULL, 65536, 65536};
    XrXirValue values[] = {{XR_XIR_I64, 0, 1}, {XR_XIR_I64, 0, 2},
        {XR_XIR_I64, 0, 3}, {XR_XIR_I64, 0, 4}};
    CHECK(xr_xir_array_new((XrXirType)256, values, 4, &fixture.admission, &fixture.array) == XR_XIR_VALUE_OK);
    fixture.budget = xr_xir_domain_budget_stats(fixture.domain);
    XirArray *array = (XirArray *)object_pointer(&fixture.array);
    CHECK(array->length == 4 && array->capacity == 4 && array->stride == sizeof(int64_t));
    return fixture;
}

static void grow_contents(GrowFixture *fixture, int64_t length) {
    XrXirValueAdmission reading = fixture->admission;
    reading.domain = fixture->receiving; reading.work = 65536;
    int64_t actual = -1;
    CHECK(xr_xir_array_len(&fixture->array, &reading, &actual) == XR_XIR_VALUE_OK && actual == length);
    for (int64_t i = 0; i < length; ++i) {
        XrXirValue result = {0};
        XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(&fixture->array, i, &reading, &result, &fault) == XR_XIR_VALUE_OK);
        CHECK(result.type == XR_XIR_I64 && !result.reserved && result.payload == i + 1);
        CHECK(xr_xir_fault_empty(fault));
        xr_xir_value_drop(&result);
    }
}

static void grow_release(GrowFixture *fixture) {
    xr_xir_value_drop(&fixture->array);
    CHECK(xr_xir_domain_stats(fixture->domain).live_bytes == sizeof(XrXirDomain));
    xr_xir_compile_type_arena_drop(fixture->arena);
    xr_xir_domain_drop(fixture->domain);
    CHECK(xr_xir_domain_stats(fixture->receiving).live_bytes == sizeof(XrXirDomain));
    xr_xir_domain_drop(fixture->receiving);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
}

static XrXirValueStatus grow_push(GrowFixture *fixture) {
    XrXirValue five = {XR_XIR_I64, 0, 5};
    XrXirValuePlace place = {(XrXirType)fixture->array.type, &fixture->array.payload};
    return xr_xir_array_push(&place, &five, &fixture->admission);
}

static void grow_rejection(uint64_t requested, uint64_t work, bool by_work) {
    GrowFixture fixture = grow_fixture(requested + (by_work ? 64 : 63), by_work ? work + 2 : 65536);
    CHECK(fixture.budget.requested_bytes == requested && fixture.budget.work == work);
    XrXirDomainStats before = xr_xir_domain_stats(fixture.domain);
    XirArray *array = (XirArray *)object_pointer(&fixture.array);
    unsigned char *data = array->data;
    size_t before_calls = calls;
    for (int retry = 0; retry < 2; ++retry) {
        CHECK(grow_push(&fixture) == XR_XIR_VALUE_LIMIT);
        CHECK(calls == before_calls && array->data == data && array->capacity == 4);
        XrXirDomainStats after = xr_xir_domain_stats(fixture.domain);
        CHECK(after.live_bytes == before.live_bytes && after.reallocations == before.reallocations);
        XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(fixture.domain);
        CHECK(budget.requested_bytes == requested && budget.work == work + (by_work ? 2 : 2 * (uint64_t)(retry + 1)));
        grow_contents(&fixture, 4);
    }
    grow_release(&fixture);
}

static void grow_exact(uint64_t requested, uint64_t work) {
    GrowFixture fixture = grow_fixture(requested + 64, work + 3);
    XrXirDomainStats before = xr_xir_domain_stats(fixture.domain);
    size_t before_calls = calls;
    CHECK(grow_push(&fixture) == XR_XIR_VALUE_OK && calls == before_calls + 1);
    XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(fixture.domain);
    XrXirDomainStats after = xr_xir_domain_stats(fixture.domain);
    CHECK(budget.requested_bytes == requested + 64 && budget.work == work + 3);
    CHECK(after.live_bytes == before.live_bytes + 32 && after.reallocations == before.reallocations + 1);
    grow_contents(&fixture, 5);
    grow_release(&fixture);
}

static void grow_oom_retains_charge(uint64_t requested, uint64_t work) {
    GrowFixture fixture = grow_fixture(requested + 128, work + 6);
    XrXirDomainStats before = xr_xir_domain_stats(fixture.domain);
    XirArray *array = (XirArray *)object_pointer(&fixture.array);
    unsigned char *data = array->data;
    size_t before_calls = calls;
    fail_at = calls;
    CHECK(grow_push(&fixture) == XR_XIR_VALUE_OOM && calls == before_calls + 1);
    fail_at = SIZE_MAX;
    XrXirDomainBudgetStats failed = xr_xir_domain_budget_stats(fixture.domain);
    XrXirDomainStats after = xr_xir_domain_stats(fixture.domain);
    CHECK(failed.requested_bytes == requested + 64 && failed.work == work + 3);
    CHECK(array->data == data && array->capacity == 4 && after.live_bytes == before.live_bytes);
    CHECK(after.reallocations == before.reallocations);
    grow_contents(&fixture, 4);
    CHECK(grow_push(&fixture) == XR_XIR_VALUE_OK && calls == before_calls + 2);
    XrXirDomainBudgetStats retried = xr_xir_domain_budget_stats(fixture.domain);
    CHECK(retried.requested_bytes == requested + 128 && retried.work == work + 6);
    grow_contents(&fixture, 5);
    grow_release(&fixture);
}

int main(void) {
    fail_at = SIZE_MAX;
    GrowFixture probe = grow_fixture(65536, 65536);
    uint64_t requested = probe.budget.requested_bytes, work = probe.budget.work;
    XrXirValue five = {XR_XIR_I64, 0, 5};
    CHECK(xr_xir_value_admit(&probe.array, (XrXirType)probe.array.type, &probe.admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&five, XR_XIR_I64, &probe.admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_budget_stats(probe.domain).work == work + 2);
    grow_release(&probe);
    grow_rejection(requested, work, false);
    grow_rejection(requested, work, true);
    grow_exact(requested, work);
    grow_oom_retains_charge(requested, work);
    CHECK(original_value_allocation_main() == 0);
    puts("Array realloc exact/minus-one budgets, charged OOM retry, preserved contents and physical release PASS");
    return 0;
}
