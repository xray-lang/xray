/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_domain.c - Owned values on one finite compiler ledger
 *
 * KEY CONCEPT:
 *   Actual allocator failures, cumulative limits and late owners stay together.
 */
#define main original_value_allocation_main
#include "test_xir_value_allocations.c"
#undef main
#include "xir/xxir_compile_domain.h"

#define TRY_VALUE(call) do { status = (call); if (status != XR_XIR_VALUE_OK) goto done; } while (0)
static XrCompileResourceLimits compile_domain_limits(void) {
    return (XrCompileResourceLimits){1048576, 1048576, 4194304};
}
static XrCompileResourceStats compile_domain_stats(XrCompileResources *resources) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
}
static XrXirValueStatus compile_domain_sequence(XrCompileResources *resources, XrXirValue *output) {
    CHECK(output && !output->type && !output->reserved && !output->payload);
    XrXirDomain *domain = NULL;
    XrXirTypeArena *arena = NULL;
    XrXirValue values[7] = {{0}};
    void *metadata = NULL, *activation = NULL;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrXirCallableParameter fields[] = {{XR_XIR_STRING, 0}, {(XrXirType)257, 0}};
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_TUPLE, .parameters = fields, .parameter_count = 2},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64}
    };
    XrXirTypes types = {nodes, 4, NULL, NULL};
    XrXirCompileContext context = {.resources = resources, .limits = xr_xir_compile_default_limits()};
    TRY_VALUE(xr_xir_compile_domain_new(resources, 65536, &domain));
    CHECK(domain->compile_resources == resources);
    XrXirDomainBudgetControls controls = {65536, 65536, 1048576, 65536, 65536};
    TRY_VALUE(xr_xir_domain_budget_bind(domain, &controls));
    TRY_VALUE(xr_xir_compile_type_arena_new(&context, &types, &arena));
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 100000, 65536};
    XrXirValue scalar = {XR_XIR_I64, 0, 37}, read = {0};
    TRY_VALUE(xr_xir_cell_new(domain, arena, (XrXirType)256, &scalar, &admission, &values[0]));
    TRY_VALUE(xr_xir_cell_read(&values[0], &read)); CHECK(read.type == XR_XIR_I64 && read.payload == 37);
    scalar.payload = 91;
    TRY_VALUE(xr_xir_cell_write(&values[0], &scalar, &admission));
    read = (XrXirValue){0}; TRY_VALUE(xr_xir_cell_read(&values[0], &read)); CHECK(read.payload == 91);
    TRY_VALUE(xr_xir_string_new(domain, "a", 1, &values[1]));
    TRY_VALUE(xr_xir_string_new(domain, "bcdefghijklmnopqrstuvwxyz", 25, &values[2]));
    uint64_t before_resize = xr_xir_domain_stats(domain).reallocations;
    TRY_VALUE(xr_xir_string_append(&values[1], &values[2]));
    CHECK(xr_xir_domain_stats(domain).reallocations > before_resize);
    const char *text = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&values[1], &text, &length) && length == 26 &&
        !memcmp(text, "abcdefghijklmnopqrstuvwxyz", 26));
    XrXirValue integers[] = {{XR_XIR_I64, 0, 1}, {XR_XIR_I64, 0, 2},
        {XR_XIR_I64, 0, 3}, {XR_XIR_I64, 0, 4}};
    XrXirValue integer = {XR_XIR_I64, 0, 5};
    TRY_VALUE(xr_xir_array_new((XrXirType)259, integers, 4, &admission, &values[3]));
    XrXirValuePlace place = {(XrXirType)259, &values[3].payload};
    before_resize = xr_xir_domain_stats(domain).reallocations;
    TRY_VALUE(xr_xir_array_push(&place, &integer, &admission));
    CHECK(xr_xir_domain_stats(domain).reallocations > before_resize);
    TRY_VALUE(xr_xir_array_new((XrXirType)257, &values[1], 1, &admission, &values[4]));
    XrXirValue tuple_fields[] = {values[1], values[4]};
    TRY_VALUE(xr_xir_tuple_new((XrXirType)258, tuple_fields, 2, &admission, &values[5]));
    metadata = xr_xir_domain_metadata_allocate(domain, 17, &status);
    if (!metadata) goto done;
    activation = xr_xir_domain_call_allocate(domain, 31, &status);
    if (!activation) goto done;
    CHECK(!memcmp(metadata, (unsigned char[17]){0}, 17));
    CHECK(!memcmp(activation, (unsigned char[31]){0}, 31));
    TRY_VALUE(xr_xir_value_copy(&values[5], &values[6]));
    *output = values[6]; values[6] = (XrXirValue){0};
done:
    if (activation) xr_xir_domain_call_deallocate(domain, activation, 31);
    if (metadata) xr_xir_domain_metadata_deallocate(domain, metadata, 17);
    for (unsigned n = 7; n > 0; --n) xr_xir_value_drop(&values[n - 1]);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    CHECK(!live);
    return status;
}
static void compile_domain_read(const XrXirValue *value) {
    XrXirValue text = {0}, array = {0}, element = {0};
    CHECK(xr_xir_tuple_get(value, 0, &text) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(value, 1, &array) == XR_XIR_VALUE_OK);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&text, &bytes, &length) && length == 26 &&
        !memcmp(bytes, "abcdefghijklmnopqrstuvwxyz", 26));
    XrXirValueAdmission admission = {xr_xir_value_arena(&array), object_pointer(&array)->domain,
        NULL, NULL, 100000, 65536};
    XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_get(&array, 0, &admission, &element, &fault) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_view(&element, &bytes, &length) && length == 26 &&
        !memcmp(bytes, "abcdefghijklmnopqrstuvwxyz", 26));
    xr_xir_value_drop(&element); xr_xir_value_drop(&array); xr_xir_value_drop(&text);
}
static XrCompileResourceStats compile_domain_baseline(size_t *sites) {
    XrCompileResources *resources = NULL; XrCompileResourceLimits limits = compile_domain_limits();
    value_compile_calls = 0;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    const uint64_t initial_live = compile_domain_stats(resources).live_bytes;
    XrXirValue value = {0}; CHECK(compile_domain_sequence(resources, &value) == XR_XIR_VALUE_OK);
    *sites = value_compile_calls;
    const XrCompileResourceStats baseline = compile_domain_stats(resources);
    compile_domain_read(&value);
    size_t before_drop = value_compile_calls; value_compile_fail_at = before_drop;
    xr_xir_value_drop(&value); value_compile_fail_at = SIZE_MAX;
    CHECK(value_compile_calls == before_drop);
    XrCompileResourceStats released = compile_domain_stats(resources);
    CHECK(released.live_bytes == initial_live && released.allocated_bytes == baseline.allocated_bytes &&
        released.work >= baseline.work);
    xr_compile_resources_release(resources);
    CHECK(!value_compile_live && !value_compile_bytes && !live);
    return baseline;
}
static void compile_domain_faults(size_t sites) {
    for (size_t failure = 0; failure < sites; ++failure) {
        XrCompileResources *resources = NULL; XrCompileResourceLimits limits = compile_domain_limits();
        value_compile_calls = 0; value_compile_injected = false; value_compile_fail_at = failure;
        XrCompileResourceStatus created = xr_compile_resources_new(&limits, &resources);
        XrXirValue value = {0}; XrXirValueStatus status = created == XR_COMPILE_RESOURCE_OK ?
            compile_domain_sequence(resources, &value) : XR_XIR_VALUE_OOM;
        value_compile_fail_at = SIZE_MAX;
        CHECK(value_compile_injected && value_compile_calls > failure && status == XR_XIR_VALUE_OOM);
        CHECK(!value.type && !value.reserved && !value.payload);
        if (resources) {
            XrCompileResourceStats failed = compile_domain_stats(resources);
            CHECK(failed.live_bytes == sizeof(*resources));
            CHECK(compile_domain_sequence(resources, &value) == XR_XIR_VALUE_OK);
            XrCompileResourceStats retried = compile_domain_stats(resources);
            CHECK(retried.allocated_bytes > failed.allocated_bytes && retried.work > failed.work);
            compile_domain_read(&value); xr_xir_value_drop(&value);
            CHECK(compile_domain_stats(resources).live_bytes == sizeof(*resources));
        }
        xr_compile_resources_release(resources);
        CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    printf("compile Domain physical OOM sites=%zu; same-owner retry, cumulative charges, physical=0/0\n", sites);
}
static void compile_domain_axes(const XrCompileResourceStats *baseline) {
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned shortfall = 0; shortfall < 2; ++shortfall) {
        XrCompileResourceLimits limits = compile_domain_limits();
        uint64_t exact = axis == 0 ? baseline->allocated_bytes : axis == 1 ? baseline->peak_bytes : baseline->work;
        CHECK(exact > 1);
        if (axis == 0) limits.allocated_bytes = exact - shortfall;
        else if (axis == 1) limits.live_bytes = exact - shortfall;
        else limits.work = exact - shortfall;
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrXirValue value = {0}; XrXirValueStatus status = compile_domain_sequence(resources, &value);
        CHECK(status == (shortfall ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_OK));
        if (!shortfall) xr_xir_value_drop(&value);
        else {
            CHECK(!value.type && !value.reserved && !value.payload);
            XrCompileResourceStats failed = compile_domain_stats(resources);
            CHECK(compile_domain_sequence(resources, &value) == XR_XIR_VALUE_LIMIT);
            XrCompileResourceStats retried = compile_domain_stats(resources);
            CHECK(retried.work >= failed.work && retried.allocated_bytes >= failed.allocated_bytes);
        }
        XrCompileResourceStats after = compile_domain_stats(resources);
        CHECK(after.allocated_bytes <= limits.allocated_bytes && after.peak_bytes <= limits.live_bytes && after.work <= limits.work);
        CHECK(after.live_bytes == sizeof(*resources)); xr_compile_resources_release(resources);
        CHECK(!value_compile_live && !value_compile_bytes);
        printf("compile Domain axis%u exact=%llu shortfall=%u status=%u physical=0/0\n", axis,
            (unsigned long long)exact, shortfall, status);
    }
}
static void compile_domain_late_owner(void) {
    XrCompileResourceLimits limits = compile_domain_limits(); XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrXirValue value = {0}; CHECK(compile_domain_sequence(resources, &value) == XR_XIR_VALUE_OK);
    xr_compile_resources_release(resources);
    compile_domain_read(&value);
    size_t before = value_compile_calls; value_compile_fail_at = before;
    xr_xir_value_drop(&value); value_compile_fail_at = SIZE_MAX;
    CHECK(value_compile_calls == before && !value_compile_live && !value_compile_bytes && !live);
    puts("compile Domain owns ledger after producer/arena/Domain release; last drop allocates nothing physical=0/0");
}
static void compile_domain_failed_growth(void) {
    XrCompileResourceLimits limits = compile_domain_limits(); XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrXirDomain *domain = NULL; XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_domain_new(resources, 65536, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64};
    XrXirTypes types = {&node, 1, NULL, NULL};
    XrXirCompileContext context = {.resources = resources, .limits = xr_xir_compile_default_limits()};
    CHECK(xr_xir_compile_type_arena_new(&context, &types, &arena) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 100000, 65536};
    XrXirValue text = {0}, suffix = {0}, array = {0};
    CHECK(xr_xir_string_new(domain, "a", 1, &text) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "bcdefghijklmnopqrstuvwxyz", 25, &suffix) == XR_XIR_VALUE_OK);
    XrXirValue integers[] = {{XR_XIR_I64, 0, 1}, {XR_XIR_I64, 0, 2},
        {XR_XIR_I64, 0, 3}, {XR_XIR_I64, 0, 4}};
    CHECK(xr_xir_array_new((XrXirType)256, integers, 4, &admission, &array) == XR_XIR_VALUE_OK);
    for (unsigned operation = 0; operation < 2; ++operation) {
        int64_t payload = operation ? array.payload : text.payload;
        XrCompileResourceStats before = compile_domain_stats(resources);
        value_compile_injected = false; value_compile_fail_at = value_compile_calls;
        XrXirValuePlace place = {(XrXirType)256, &array.payload};
        XrXirValue integer = {XR_XIR_I64, 0, 5};
        XrXirValueStatus status = operation ? xr_xir_array_push(&place, &integer, &admission) :
            xr_xir_string_append(&text, &suffix);
        value_compile_fail_at = SIZE_MAX;
        CHECK(status == XR_XIR_VALUE_OOM && value_compile_injected);
        CHECK((operation ? array.payload : text.payload) == payload);
        XrCompileResourceStats after = compile_domain_stats(resources);
        CHECK(after.allocated_bytes == before.allocated_bytes && after.live_bytes == before.live_bytes &&
            after.work > before.work);
        if (operation) {
            int64_t length = -1; XrXirFaultDetail fault = {0};
            CHECK(xr_xir_array_len(&array, &admission, &length) == XR_XIR_VALUE_OK && length == 4);
            for (int64_t index = 0; index < 4; ++index) {
                XrXirValue read = {0};
                CHECK(xr_xir_array_get(&array, index, &admission, &read, &fault) == XR_XIR_VALUE_OK &&
                    read.type == XR_XIR_I64 && read.payload == index + 1);
            }
            CHECK(xr_xir_array_push(&place, &integer, &admission) == XR_XIR_VALUE_OK);
        } else {
            const char *bytes = NULL; size_t length = 0;
            CHECK(xr_xir_string_view(&text, &bytes, &length) && length == 1 && bytes[0] == 'a');
            CHECK(xr_xir_string_append(&text, &suffix) == XR_XIR_VALUE_OK);
        }
    }
    xr_xir_value_drop(&array); xr_xir_value_drop(&suffix); xr_xir_value_drop(&text);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    CHECK(compile_domain_stats(resources).live_bytes == sizeof(*resources));
    xr_compile_resources_release(resources); CHECK(!value_compile_live && !value_compile_bytes && !live);
    puts("compile Domain failed string/Array growth preserves owned input; same-ledger retry physical=0/0");
}
#ifndef XR_COMPILE_DOMAIN_ENTRY
#define XR_COMPILE_DOMAIN_ENTRY main
#endif
int XR_COMPILE_DOMAIN_ENTRY(void) {
    size_t sites = 0; XrCompileResourceStats baseline = compile_domain_baseline(&sites);
    CHECK(sites > 2 && calls == 0);
    compile_domain_faults(sites); compile_domain_axes(&baseline); compile_domain_late_owner();
    compile_domain_failed_growth();
    CHECK(calls == 0 && !live);
    puts("compile Domain Cell/string/Array/Tuple/metadata/call/resize PASS"); return 0;
}
