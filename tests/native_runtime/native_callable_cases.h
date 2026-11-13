/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_callable_cases.h - Real code leases and shared captured Cell owners
 */
typedef struct NativeCallableLease {
    uint32_t references;
    uint32_t *retired;
    bool reject;
} NativeCallableLease;

static bool native_callable_retain(void *owner) {
    NativeCallableLease *lease = owner;
    if (lease->reject) return false;
    CHECK(lease->references);
    ++lease->references;
    return true;
}

static void native_callable_release(void *owner) {
    NativeCallableLease *lease = owner;
    CHECK(lease->references);
    if (!--lease->references) { ++*lease->retired; xr_free(lease); }
}

static void native_callable_owners(void) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_CELL, .element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE, .element=XR_XIR_NEVER, .result=XR_XIR_I64,
            .flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_CALLABLE, .element=XR_XIR_NEVER, .result=XR_XIR_I64,
            .flags=XR_XIR_CALLABLE_ROOT_NONE | XR_XIR_CALLABLE_NO_SUSPEND | XR_XIR_CALLABLE_C_ABI}
    };
    XrXirTypes types = {.nodes=nodes, .count=3};
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_type_arena_new(&context, &types, &arena) == XR_XIR_VALUE_OK);
    xr_compile_resources_release(context.resources);
    XrXirType capture_types[] = {(XrXirType)256, XR_XIR_I64};
    XrtCallableEntry entries[] = {
        {(XrXirType)257, capture_types, 2}, {(XrXirType)257, capture_types, 2}
    };
    uint32_t retired = 0;
    NativeCallableLease *owner = xr_malloc(sizeof(*owner));
    CHECK(owner);
    *owner = (NativeCallableLease){1, &retired, false};
    XrtCallableLease lease = {owner, native_callable_retain, native_callable_release};
    XrtCallableCode *code = NULL;
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        XrtCallableEntry wrong = entries[0];
        if (invalid == 0) wrong.type = XR_XIR_I64;
        if (invalid == 1) wrong.captures = NULL;
        if (invalid == 2) wrong.capture_count = 65537;
        if (invalid == 3) capture_types[1] = XR_XIR_NEVER;
        if (invalid == 4) wrong.type = (XrXirType)258;
        CHECK(xrt_callable_code_new(arena, &wrong, 1, lease, &code) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(!code && owner->references == 1);
        capture_types[1] = XR_XIR_I64;
    }
    owner->reject = true;
    CHECK(xrt_callable_code_new(arena, entries, 2, lease, &code) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!code && owner->references == 1);
    owner->reject = false;
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    native_string_fail_at = native_string_calls;
    CHECK(xrt_callable_code_new(arena, entries, 2, lease, &code) == XR_XIR_VALUE_OOM);
    CHECK(!code && owner->references == 1 && native_string_injected);
    native_string_fail_at = SIZE_MAX;
#endif
    CHECK(xrt_callable_code_new(arena, entries, 2, lease, &code) == XR_XIR_VALUE_OK);
    CHECK(owner->references == 2);
    XrtCallableCode *foreign = NULL;
    CHECK(xrt_callable_code_new(arena, entries, 2, lease, &foreign) == XR_XIR_VALUE_OK);
    capture_types[0] = capture_types[1] = XR_XIR_STRING;
    memset(entries, 0, sizeof(entries));
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_root_domain_new(UINT64_MAX, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_retain(domain));
    XrXirDomainStats baseline = xr_xir_domain_stats(domain);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, UINT64_MAX, UINT64_MAX};
    XrXirValue initial = {XR_XIR_I64, 0, 40}, cell = {0};
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType)256, &initial, &admission, &cell) == XR_XIR_VALUE_OK);
    XrXirValue captures[] = {cell, {XR_XIR_I64, 0, 2}}, first = {0}, second = {0}, copy = {0};
    uint64_t before = xr_xir_domain_stats(domain).live_bytes;
    CHECK(xrt_callable_new(code, domain, 2, captures, 2, &first) == XR_XIR_VALUE_BAD_ARGUMENT && !first.type);
    CHECK(xrt_callable_new(code, domain, 0, captures, 1, &first) == XR_XIR_VALUE_BAD_ARGUMENT && !first.type);
    captures[1].type = XR_XIR_STRING;
    captures[1].payload = 0;
    CHECK(xrt_callable_new(code, domain, 0, captures, 2, &first) == XR_XIR_VALUE_BAD_ARGUMENT && !first.type);
    captures[1] = (XrXirValue){XR_XIR_I64, 0, 2};
    CHECK(xr_xir_domain_stats(domain).live_bytes == before);
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    atomic_store_explicit(&code->references, UINT32_MAX, memory_order_relaxed);
    CHECK(xrt_callable_new(code, domain, 0, captures, 2, &first) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!first.type);
    atomic_store_explicit(&code->references, 1, memory_order_relaxed);
    native_string_fail_at = native_string_calls;
    CHECK(xrt_callable_new(code, domain, 0, captures, 2, &first) == XR_XIR_VALUE_OOM);
    CHECK(!first.type && native_string_injected && xr_xir_domain_stats(domain).live_bytes == before);
    native_string_fail_at = SIZE_MAX;
#endif
    CHECK(xrt_callable_new(code, domain, 0, captures, 2, &first) == XR_XIR_VALUE_OK);
    CHECK(xrt_callable_new(code, domain, 1, captures, 2, &second) == XR_XIR_VALUE_OK);
    const XrXirValue *rejected = NULL;
    uint32_t rejected_count = 0;
    CHECK(xrt_callable_borrow(foreign, &first, 0, &rejected, &rejected_count) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!rejected && !rejected_count);
    xrt_callable_code_drop(&foreign);
    CHECK(owner->references == 2);
    XrXirValue occupied = {XR_XIR_I64, 0, 19};
    CHECK(xrt_callable_new(code, domain, 0, captures, 2, &occupied) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(occupied.type == XR_XIR_I64 && occupied.payload == 19);
    CHECK(xr_xir_value_copy(&first, &copy) == XR_XIR_VALUE_OK);
    XrtCallableCode *borrowed_code = code;
    native_callable_release(owner);
    xrt_callable_code_drop(&code);
    xr_xir_compile_type_arena_drop(arena);
    xr_xir_value_drop(&cell);
    xr_xir_root_domain_release(domain);
    CHECK(!retired && !code);
    const XrXirValue *bound = NULL;
    uint32_t count = 0;
    CHECK(xrt_callable_borrow(borrowed_code, &first, 1, &bound, &count) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!bound && !count);
    CHECK(xrt_callable_borrow(borrowed_code, &first, 0, &bound, &count) == XR_XIR_VALUE_OK);
    CHECK(count == 2 && bound[0].payload == captures[0].payload && bound[1].payload == 2);
    XrXirValue read = {0}, replacement = {XR_XIR_I64, 0, 41};
    CHECK(xr_xir_cell_read(&bound[0], &read) == XR_XIR_VALUE_OK && read.payload == 40);
    xr_xir_value_drop(&read);
    CHECK(xr_xir_cell_write(&bound[0], &replacement, &admission) == XR_XIR_VALUE_OK);
    CHECK(xrt_callable_borrow(borrowed_code, &second, 1, &bound, &count) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_read(&bound[0], &read) == XR_XIR_VALUE_OK && read.payload == 41);
    xr_xir_value_drop(&read);
    xr_xir_value_drop(&first);
    xr_xir_value_drop(&second);
    CHECK(!retired);
    CHECK(xrt_callable_borrow(borrowed_code, &copy, 0, &bound, &count) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&copy);
    XrXirDomainStats final = xr_xir_domain_stats(domain);
    CHECK(retired == 1 && final.live_bytes == baseline.live_bytes && final.allocations == final.frees + 1);
    xr_xir_domain_drop(domain);
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    CHECK(!native_string_live && !native_string_bytes);
#endif
    puts("canonical closures retain shared Cell identity and code after producer retirement");
}
