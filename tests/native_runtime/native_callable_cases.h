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

typedef struct NativeCallableBindingContext {
    XrXirFunctionBinding *original;
    uint32_t visits;
} NativeCallableBindingContext;

static XrXirValueStatus native_callable_binding_admit(void *opaque,
    const XrXirFunctionBinding *binding, XrXirType type, uint64_t *work) {
    NativeCallableBindingContext *context = opaque;
    (void)type;
    (void)work;
    CHECK(binding->owner == context->original->owner && binding->capture_count == 1);
    ++context->visits;
    context->original->capture_count = 0;
    context->original->captures = NULL;
    return XR_XIR_VALUE_OK;
}

static void native_callable_binding_snapshot(void) {
    const XrtCallableTypeSpec spec = {.kind=XRT_CALLABLE_SIGNATURE,
        .result={XRT_CALLABLE_SCALAR, XR_POD_I64}};
    XrXirTypeArena *arena = NULL;
    CHECK(xrt_callable_types_new(&spec, 1, UINT64_MAX, &arena) == XR_XIR_VALUE_OK);
    XrXirType type = XR_XIR_UNIT;
    CHECK(xrt_callable_type_id((XrtCallableValueSpec){XRT_CALLABLE_CONSTRUCTED, 0}, 1, &type));
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(UINT64_MAX, &domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats before = xr_xir_domain_stats(domain);
    uint32_t retired = 0;
    NativeCallableLease *owner = xr_malloc(sizeof(*owner));
    CHECK(owner);
    *owner = (NativeCallableLease){1, &retired, false};
    XrXirValue capture = {XR_XIR_I64, 0, 42}, function = {0};
    XrXirFunctionBinding binding = {owner, native_callable_release, 7, &capture, 1};
    NativeCallableBindingContext context = {&binding, 0};
    XrXirValueAdmission admission = {arena, domain, native_callable_binding_admit,
        &context, UINT64_MAX, UINT64_MAX};
    CHECK(xr_xir_function_new(domain, arena, type, &binding, &admission, &function) == XR_XIR_VALUE_OK);
    const XrXirFunctionBinding *received = xr_xir_function_binding(&function);
    CHECK(context.visits == 1 && !binding.capture_count && !binding.captures);
    CHECK(received && received->capture_count == 1 && received->captures[0].payload == 42);
    xr_xir_value_drop(&function);
    XrXirDomainStats after = xr_xir_domain_stats(domain);
    CHECK(retired == 1 && after.live_bytes == before.live_bytes);
    xr_xir_domain_drop(domain);
    xr_xir_compile_type_arena_drop(arena);
}

static void native_callable_projection(void) {
    XrtCallableParameterSpec parameter = {{XRT_CALLABLE_CONSTRUCTED, 1}, XR_PARAM_MOVE, true};
    XrtCallableTypeSpec specs[] = {
        {.kind=XRT_CALLABLE_CELL, .element={XRT_CALLABLE_SCALAR, XR_POD_I64}},
        {.kind=XRT_CALLABLE_SIGNATURE, .result={XRT_CALLABLE_SCALAR, XR_POD_I64}, .flags=1},
        {.kind=XRT_CALLABLE_CELL, .element={XRT_CALLABLE_CONSTRUCTED, 1}},
        {.kind=XRT_CALLABLE_SIGNATURE, .parameters=&parameter, .parameter_count=1,
            .result={XRT_CALLABLE_CONSTRUCTED, 1}},
        {.kind=XRT_CALLABLE_FIXED_BYTES, .element={XRT_CALLABLE_SCALAR, XR_POD_U8}, .count=17},
        {.kind=XRT_CALLABLE_NULLABLE, .element={XRT_CALLABLE_CONSTRUCTED, 4}}
    };
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        XrtCallableTypeSpec saved = specs[3];
        XrtCallableParameterSpec prior = parameter;
        if (invalid == 0) specs[3].result.type = 3;
        if (invalid == 1) specs[3].throws = true;
        if (invalid == 2) specs[3].flags = 128;
        if (invalid == 3) specs[3].element.type = XR_POD_I64;
        if (invalid == 4) specs[3].parameters = NULL;
        if (invalid == 5) parameter.mode = 99;
        if (invalid == 6) parameter.value.type = 3;
        if (invalid == 7) specs[3].kind = (XrtCallableShapeKind)-1;
        XrXirTypeArena *rejected = NULL;
        CHECK(xrt_callable_types_new(specs, 6, UINT64_MAX, &rejected) ==
            (invalid == 1 ? XR_XIR_VALUE_UNSUPPORTED : XR_XIR_VALUE_BAD_ARGUMENT));
        CHECK(!rejected);
        specs[3] = saved;
        parameter = prior;
    }
    XrXirTypeArena *arena = NULL;
    CHECK(xrt_callable_types_new(specs, 6, 1, &arena) == XR_XIR_VALUE_MEMORY_LIMIT && !arena);
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    size_t first_call = native_string_calls;
    CHECK(xrt_callable_types_new(specs, 6, UINT64_MAX, &arena) == XR_XIR_VALUE_OK);
    size_t sites = native_string_calls - first_call;
    xr_xir_compile_type_arena_drop(arena);
    arena = NULL;
    CHECK(!native_string_live && !native_string_bytes);
    for (size_t point = 0; point < sites; ++point) {
        native_string_fail_at = native_string_calls + point;
        native_string_injected = false;
        CHECK(xrt_callable_types_new(specs, 6, UINT64_MAX, &arena) == XR_XIR_VALUE_OOM);
        CHECK(!arena && native_string_injected && !native_string_live && !native_string_bytes);
    }
    native_string_fail_at = SIZE_MAX;
    printf("canonical callable projection receiving failures cover %zu allocation sites\n", sites);
#endif
    CHECK(xrt_callable_types_new(specs, 6, UINT64_MAX, &arena) == XR_XIR_VALUE_OK);
    memset(specs, 0, sizeof(specs));
    memset(&parameter, 0, sizeof(parameter));
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *signature = xr_xir_callable_signature(types, (XrXirType)263);
    CHECK(signature && signature->parameters[0].mode == XR_PARAM_MOVE && signature->parameters[0].escaping &&
        signature->parameters[0].type == 262 && signature->result == 262);
    CHECK((signature->flags & XR_XIR_CALLABLE_ROOT_MASK) == XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    XrXirLayout layout = {0};
    CHECK(xr_xir_compile_type_arena_layout(arena, (XrXirType)265, &layout) && layout.size == 18);
    XrXirType capture_types[] = {(XrXirType)260, XR_XIR_I64, (XrXirType)262};
    XrtCallableEntry entries[] = {{(XrXirType)261, capture_types, 2}, {(XrXirType)261, capture_types + 2, 1}};
    uint32_t retired = 0;
    NativeCallableLease *owner = xr_malloc(sizeof(*owner));
    CHECK(owner);
    *owner = (NativeCallableLease){1, &retired, false};
    XrtCallableCode *code = NULL;
    CHECK(xrt_callable_code_new(arena, entries, 2,
        (XrtCallableLease){owner, native_callable_retain, native_callable_release}, &code) == XR_XIR_VALUE_OK);
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(UINT64_MAX, &domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats baseline = xr_xir_domain_stats(domain);
    XrXirValue initial = {XR_XIR_I64, 0, 40}, integer = {0}, first = {0}, slot = {0}, cycle = {0};
    CHECK(xrt_callable_cell_new(code, domain, (XrXirType)260, &initial, &integer) == XR_XIR_VALUE_OK);
    XrXirValue captures[] = {integer, {XR_XIR_I64, 0, 2}};
    CHECK(xrt_callable_new(code, domain, 0, captures, 2, &first) == XR_XIR_VALUE_OK);
    XrXirFunctionBinding *stored = (XrXirFunctionBinding *)xr_xir_function_binding(&first);
    CHECK(stored);
    const XrXirValue *inline_captures = stored->captures;
    stored->captures = (const XrXirValue *)(uintptr_t)1;
    CHECK(!xr_xir_value_valid(&first));
    stored->captures = inline_captures;
    CHECK(xr_xir_value_valid(&first));
    CHECK(xrt_callable_cell_new(code, domain, (XrXirType)262, &first, &slot) == XR_XIR_VALUE_OK);
    CHECK(xrt_callable_new(code, domain, 1, &slot, 1, &cycle) == XR_XIR_VALUE_OK);
    CHECK(xrt_callable_cell_write(code, domain, (XrXirType)262, &slot, &cycle) == XR_XIR_VALUE_OK);
    CHECK(xrt_callable_cell_write(code, domain, (XrXirType)260, &slot, &initial) == XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_compile_type_arena_drop(arena);
    xrt_callable_code_drop(&code);
    native_callable_release(owner);
    xr_xir_value_drop(&first);
    xr_xir_value_drop(&integer);
    xr_xir_value_drop(&cycle);
    xr_xir_value_drop(&slot);
    CHECK(!retired && xr_xir_domain_stats(domain).live_bytes > baseline.live_bytes);
    CHECK(xr_xir_domain_close(domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats final = xr_xir_domain_stats(domain);
    CHECK(retired == 1 && final.live_bytes == baseline.live_bytes && final.allocations == final.frees + 1);
    xr_xir_domain_drop(domain);
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    CHECK(!native_string_live && !native_string_bytes);
#endif
    puts("owned callable projection preserves contracts and retires a real Cell/Fn cycle");
}

static void native_callable_nested_owners(void) {
    XrXirCallableParameter parameter = {.type=XR_XIR_I64, .mode=XR_PARAM_READ};
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_CALLABLE, .element=XR_XIR_NEVER, .parameters=&parameter,
            .parameter_count=1, .result=XR_XIR_I64, .flags=XR_XIR_CALLABLE_ROOT_NONE | XR_XIR_CALLABLE_NO_SUSPEND},
        {.kind=XR_XIR_TYPE_CALLABLE, .element=XR_XIR_NEVER, .parameters=&parameter,
            .parameter_count=1, .result=XR_XIR_I64, .flags=XR_XIR_CALLABLE_ROOT_NONE}
    };
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirTypes types = {.nodes=nodes, .count=2};
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_type_arena_new(&context, &types, &arena) == XR_XIR_VALUE_OK);
    xr_compile_resources_release(context.resources);
    XrXirType strong = (XrXirType)256, weak = (XrXirType)257;
    XrtCallableEntry entries[] = {{strong, NULL, 0}, {weak, &weak, 1}, {weak, &strong, 1}, {weak, NULL, 0}};
    uint32_t retired = 0;
    NativeCallableLease *owner = xr_malloc(sizeof(*owner));
    CHECK(owner);
    *owner = (NativeCallableLease){1, &retired, false};
    XrtCallableCode *code = NULL;
    CHECK(xrt_callable_code_new(arena, entries, 4,
        (XrtCallableLease){owner, native_callable_retain, native_callable_release}, &code) == XR_XIR_VALUE_OK);
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_root_domain_new(UINT64_MAX, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_retain(domain));
    XrXirDomainStats baseline = xr_xir_domain_stats(domain);
    XrXirValue child = {0}, parent = {0}, ordinary = {0}, empty = {0};
    CHECK(xrt_callable_new(code, domain, 0, NULL, 0, &child) == XR_XIR_VALUE_OK);
    CHECK(xrt_callable_new(code, domain, 1, &child, 1, &parent) == XR_XIR_VALUE_OK);
    CHECK(xrt_callable_new(code, domain, 3, NULL, 0, &ordinary) == XR_XIR_VALUE_OK);
    CHECK(xrt_callable_new(code, domain, 2, &ordinary, 1, &empty) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!empty.type && !empty.payload);
    XrtCallableCode *borrowed = code;
    xr_xir_value_drop(&ordinary);
    xr_xir_value_drop(&child);
    xrt_callable_code_drop(&code);
    xr_xir_compile_type_arena_drop(arena);
    native_callable_release(owner);
    xr_xir_root_domain_release(domain);
    CHECK(!retired);
    const XrXirValue *captures = NULL;
    uint32_t count = 0;
    CHECK(xrt_callable_borrow(borrowed, &parent, 1, &captures, &count) == XR_XIR_VALUE_OK);
    CHECK(count == 1 && captures[0].type == (uint32_t)strong);
    CHECK(xr_xir_function_binding(&captures[0])->owner == xr_xir_function_binding(&parent)->owner);
    CHECK(xrt_callable_borrow(borrowed, &captures[0], 0, &captures, &count) == XR_XIR_VALUE_OK && !count);
    xr_xir_value_drop(&parent);
    CHECK(retired == 1 && xr_xir_domain_stats(domain).live_bytes == baseline.live_bytes);
    xr_xir_domain_drop(domain);
    puts("canonical Fn captures preserve actual NoSuspend code and survive producer retirement");
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
    for (unsigned invalid = 0; invalid < 4; ++invalid) {
        XrtCallableEntry wrong = entries[0];
        if (invalid == 0) wrong.type = XR_XIR_I64;
        if (invalid == 1) wrong.captures = NULL;
        if (invalid == 2) capture_types[1] = XR_XIR_NEVER;
        if (invalid == 3) wrong.type = (XrXirType)258;
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
