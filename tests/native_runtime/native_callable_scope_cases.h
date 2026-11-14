/* Copyright (c) 2026 Xinglei Xu. MIT License. */
typedef struct NativeCallableScopeContext {
    XrtValueRoot root;
    XrtExecutionFailure failure;
    uint64_t result;
    bool panic;
} NativeCallableScopeContext;

static void native_callable_catalog_budget_root(void *opaque) {
    NativeCallableScopeContext *context = opaque;
    XrtCallableTypeSpec spec = {.kind=XRT_CALLABLE_SIGNATURE,
        .result={XRT_CALLABLE_SCALAR, XR_POD_I64}};
    XrXirTypeArena *first = NULL, *second = NULL;
    uint64_t cold = 0, warm = 0;
    CHECK(!context->root.map_types);
    CHECK(xrt_callable_types_new_accounted(&spec, 1, UINT64_MAX, &first, &cold) == XR_XIR_VALUE_OK);
    CHECK(xrt_callable_map_catalog(first) == context->root.map_types);
    CHECK(xrt_callable_types_new_accounted(&spec, 1, UINT64_MAX, &second, &warm) == XR_XIR_VALUE_OK);
    CHECK(cold == warm && cold > 1 && xrt_callable_map_catalog(second) == context->root.map_types);
    xr_xir_compile_type_arena_drop(first);
    xr_xir_compile_type_arena_drop(second);
    first = NULL;
    uint64_t preserved = 17;
    CHECK(xrt_callable_types_new_accounted(&spec, 1, cold - 1, &first, &preserved) == XR_XIR_VALUE_MEMORY_LIMIT);
    CHECK(!first && preserved == 17);
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    size_t begin = native_string_calls;
    CHECK(xrt_callable_types_new_accounted(&spec, 1, UINT64_MAX, &first, &warm) == XR_XIR_VALUE_OK);
    size_t sites = native_string_calls - begin;
    xr_xir_compile_type_arena_drop(first);
    first = NULL;
    size_t retained = native_string_live;
    size_t retained_bytes = native_string_bytes;
    for (size_t point = 0; point < sites; ++point) {
        native_string_fail_at = native_string_calls + point;
        native_string_injected = false;
        CHECK(xrt_callable_types_new_accounted(&spec, 1, UINT64_MAX, &first, &warm) == XR_XIR_VALUE_OOM);
        CHECK(!first && native_string_injected && native_string_live == retained && native_string_bytes == retained_bytes);
    }
    native_string_fail_at = SIZE_MAX;
    printf("warm callable catalog preserves its Map source across %zu receiving failures\n", sites);
#endif
}
static void native_callable_catalog_budget(void) {
    NativeCallableScopeContext context = {0};
    XrtLocationOwner *owner = locations("callable-catalog-budget.xr");
    CHECK(xrt_run_embedded_owned(owner, &context.failure, native_callable_catalog_budget_root,
        &context, &context.root) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&context.root));
    xrt_locations_release(owner);
    puts("cold and warm callable catalogs account for their actual retained Map source");
}

static void native_callable_empty_scope(void *opaque) {
    static const XrtCallableImage image = {0};
    NativeCallableScopeContext *context = opaque;
    XrtCallableScope outer, inner;
    xrt_callable_scope_enter(&outer, &image, 1);
    xrt_callable_scope_enter(&inner, &image, 1);
    CHECK(!xrt_callable_scope_bind(&outer, 0, NULL, 1));
    CHECK(!xrt_callable_scope_leave(&inner, NULL, 1));
    CHECK(!xrt_callable_scope_leave(&outer, NULL, 1));
    CHECK(!context->root.callables && !context->root.callable_scope);
}

static void native_callable_scope_root(void *opaque) {
    static const XrtCallableTypeSpec types[] = {{.kind=XRT_CALLABLE_SIGNATURE,
        .result={XRT_CALLABLE_SCALAR, XR_POD_I64}}};
    static const XrtCallableValueSpec captures[] = {{XRT_CALLABLE_SCALAR, XR_POD_I64}};
    static const XrtCallableEntrySpec entries[] = {{0, captures, 1}};
    static const XrtCallableImage image = {types, 1, entries, 1};
    NativeCallableScopeContext *context = opaque;
    XrtCallableScope parent_storage, child_storage;
    XrtCallableScope *parent = &parent_storage, *child = &child_storage;
    xrt_callable_scope_enter(parent, &image, 1);
    xrt_callable_scope_enter(child, &image, 1);
    uint64_t initial = 41;
    XrtCallable *function = xrt_callable_scope_new(child, 0, &initial, 1, 1);
    if (context->panic) xrt_arithmetic_panic(420, 1);
    function = xrt_callable_scope_leave(child, function, 1);
    xrt_callable_scope_enter(child, &image, 1);
    initial = 99;
    (void)xrt_callable_scope_new(child, 0, &initial, 1, 1);
    CHECK(!xrt_callable_scope_leave(child, NULL, 1));
    xrt_callable_scope_enter(child, &image, 1);
    CHECK(xrt_callable_scope_bind(parent, 7, function, 1) == function);
    CHECK(xrt_callable_scope_bind(parent, 8, function, 1) == function);
    CHECK(xir_object_count((const XirObject *)function) == 3);
    CHECK(!xrt_callable_scope_leave(child, NULL, 1));
    CHECK(xir_object_count((const XirObject *)function) == 3);
    xrt_callable_scope_enter(child, &image, 1);
    initial = 80;
    XrtCallable *replacement = xrt_callable_scope_new(child, 0, &initial, 1, 1);
    CHECK(xrt_callable_scope_bind(parent, 7, replacement, 1) == replacement);
    CHECK(xir_object_count((const XirObject *)function) == 2);
    CHECK(xir_object_count((const XirObject *)replacement) == 2);
    CHECK(!xrt_callable_scope_leave(child, NULL, 1));
    CHECK(xir_object_count((const XirObject *)replacement) == 1);
    CHECK(xrt_callable_capture(&image, replacement, 0, 0, 1) == 80);
    CHECK(!xrt_callable_scope_bind(parent, 7, NULL, 1));
    CHECK(xir_object_count((const XirObject *)function) == 2);
    CHECK(xrt_callable_scope_bind(parent, 7, function, 1) == function);
    CHECK(xrt_callable_scope_bind(parent, 8, function, 1) == function);
    CHECK(xir_object_count((const XirObject *)function) == 3);
    context->result = xrt_callable_capture(&image, function, 0, 0, 1) + 1;
    CHECK(!xrt_callable_scope_leave(parent, NULL, 1));
}

static void native_captured_cell_scope_root(void *opaque) {
    static const XrtCallableTypeSpec types[] = {
        {.kind=XRT_CALLABLE_CELL, .element={XRT_CALLABLE_SCALAR, XR_POD_I64}},
        {.kind=XRT_CALLABLE_SIGNATURE, .result={XRT_CALLABLE_SCALAR, XR_POD_I64}}
    };
    static const XrtCallableValueSpec captures[] = {{XRT_CALLABLE_CONSTRUCTED, 0}};
    static const XrtCallableEntrySpec entries[] = {{1, captures, 1}};
    static const XrtCallableImage image = {types, 2, entries, 1};
    NativeCallableScopeContext *context = opaque;
    XrtCallableScope parent, child;
    xrt_callable_scope_enter(&parent, &image, 1);
    xrt_callable_scope_enter(&child, &image, 1);
    XrtCapturedCell *cell = xrt_callable_cell_scope_new(&child, 0, 40, 1);
    CHECK(xrt_callable_cell_scope_bind(&parent, 0, cell, 1) == cell);
    uint64_t bits = (uint64_t)(uintptr_t)cell;
    XrtCallable *function = xrt_callable_scope_new(&child, 0, &bits, 1, 1);
    function = xrt_callable_scope_leave(&child, function, 1);
    CHECK(xir_object_count((const XirObject *)cell) == 2);
    xrt_callable_scope_enter(&child, &image, 1);
    for (uint32_t n = 0; n < 17; ++n) {
        XrtCapturedCell *replacement = xrt_callable_cell_scope_new(&child, 0, n, 1);
        CHECK(xrt_callable_cell_scope_bind(&parent, n, replacement, 1) == replacement);
    }
    CHECK(!xrt_callable_scope_leave(&child, NULL, 1));
    CHECK(xir_object_count((const XirObject *)cell) == 1);
    CHECK(xrt_callable_capture(&image, function, 0, 0, 1) == bits);
    xrt_callable_cell_i64_write(&image, cell, 42, 1);
    context->result = (uint64_t)xrt_callable_cell_i64_read(&image, cell, 1);
    if (context->panic) xrt_arithmetic_panic(421, 1);
    CHECK(!xrt_callable_scope_leave(&parent, NULL, 1));
}
static void native_callable_scopes(void) {
    XrtLocationOwner *owner = locations("callable-scope.xr");
    NativeCallableScopeContext *context = xr_calloc(1, sizeof(*context));
    CHECK(context);
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    native_string_calls = 0;
    native_string_fail_at = SIZE_MAX;
#endif
    CHECK(xrt_run_embedded_owned(owner, &context->failure, native_callable_empty_scope, context,
        &context->root) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&context->root));
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    CHECK(!native_string_calls && !native_string_live && !native_string_bytes);
#endif
    CHECK(xrt_run_embedded_owned(owner, &context->failure, native_callable_scope_root, context,
        &context->root) == XRT_EXECUTION_COMPLETED);
    CHECK(context->result == 42 && xrt_value_root_empty(&context->root));
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    size_t sites = native_string_calls;
    CHECK(sites && !native_string_live && !native_string_bytes);
    for (size_t point = 0; point < sites; ++point) {
        native_string_calls = 0;
        native_string_fail_at = point;
        native_string_injected = false;
        context->result = 0;
        XrtExecutionStatus status = xrt_run_embedded_owned(owner, &context->failure,
            native_callable_scope_root, context, &context->root);
        CHECK(native_string_injected && status == XRT_EXECUTION_FAILURE &&
            context->failure.kind == XRT_FAILURE_VALUE && context->failure.detail.value.status == XR_XIR_VALUE_OOM);
        CHECK(xrt_value_root_empty(&context->root) && !native_string_live && !native_string_bytes);
    }
    native_string_fail_at = SIZE_MAX;
    printf("native callable scopes retain results before callee retirement across %zu allocation failures\n", sites);
#endif
    context->panic = true;
    CHECK(xrt_run_embedded_owned(owner, &context->failure, native_callable_scope_root, context,
        &context->root) == XRT_EXECUTION_FAILURE);
    CHECK(context->failure.kind == XRT_FAILURE_ARITHMETIC_PANIC &&
        context->failure.detail.panic.code == 420 && xrt_value_root_empty(&context->root));
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    CHECK(!native_string_live && !native_string_bytes);
#endif
    context->panic = false;
    context->result = 0;
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    native_string_calls = 0;
#endif
    CHECK(xrt_run_embedded_owned(owner, &context->failure, native_captured_cell_scope_root, context,
        &context->root) == XRT_EXECUTION_COMPLETED);
    CHECK(context->result == 42 && xrt_value_root_empty(&context->root));
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    sites = native_string_calls;
    CHECK(sites && !native_string_live && !native_string_bytes);
    for (size_t point = 0; point < sites; ++point) {
        native_string_calls = 0;
        native_string_fail_at = point;
        native_string_injected = false;
        XrtExecutionStatus status = xrt_run_embedded_owned(owner, &context->failure,
            native_captured_cell_scope_root, context, &context->root);
        CHECK(native_string_injected && status == XRT_EXECUTION_FAILURE &&
            context->failure.kind == XRT_FAILURE_VALUE && context->failure.detail.value.status == XR_XIR_VALUE_OOM);
        CHECK(xrt_value_root_empty(&context->root) && !native_string_live && !native_string_bytes);
    }
    native_string_fail_at = SIZE_MAX;
    printf("native captured Cells preserve shared identity and retire across %zu allocation failures\n", sites);
#endif
    context->panic = true;
    CHECK(xrt_run_embedded_owned(owner, &context->failure, native_captured_cell_scope_root, context,
        &context->root) == XRT_EXECUTION_FAILURE);
    CHECK(context->failure.kind == XRT_FAILURE_ARITHMETIC_PANIC &&
        context->failure.detail.panic.code == 421 && xrt_value_root_empty(&context->root));
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    CHECK(!native_string_live && !native_string_bytes);
#endif
    xr_free(context);
    xrt_locations_release(owner);
}

typedef struct NativeCallableLargeScopeContext {
    NativeCallableScopeContext execution;
    XrtCallableImage image;
    const uint64_t *captures;
} NativeCallableLargeScopeContext;

static void native_callable_large_scope_root(void *opaque) {
    NativeCallableLargeScopeContext *context = opaque;
    XrtCallableScope scope;
    xrt_callable_scope_enter(&scope, &context->image, 1);
    uint32_t entry = context->image.entry_count - 1;
    uint32_t count = context->image.entries[entry].capture_count;
    XrtCallable *function = xrt_callable_scope_new(&scope, entry, context->captures, count, 1);
    if (count) {
        context->execution.result = xrt_callable_capture(&context->image, function, entry, count - 1, 1);
    } else {
        context->execution.result = xrt_callable_entry(&context->image, function, 1);
    }
    CHECK(!xrt_callable_scope_leave(&scope, NULL, 1));
}

static void native_callable_large_scopes(void) {
    static const XrtCallableTypeSpec types[] = {{.kind=XRT_CALLABLE_SIGNATURE,
        .result={XRT_CALLABLE_SCALAR, XR_POD_I64}}};
    const uint32_t count = 65537;
    XrtLocationOwner *owner = locations("large-callable-scope.xr");
    for (unsigned captured = 0; captured < 2; ++captured) {
        uint32_t entries_count = captured ? 1 : count;
        XrtCallableEntrySpec *entries = xr_calloc(entries_count, sizeof(*entries));
        XrtCallableValueSpec *specs = captured ? xr_calloc(count, sizeof(*specs)) : NULL;
        uint64_t *values = captured ? xr_calloc(count, sizeof(*values)) : NULL;
        NativeCallableLargeScopeContext *context = xr_calloc(1, sizeof(*context));
        CHECK(entries && context && (!captured || (specs && values)));
        for (uint32_t i = 0; captured && i < count; ++i) {
            specs[i] = (XrtCallableValueSpec){XRT_CALLABLE_SCALAR, XR_POD_I64};
            values[i] = (uint64_t)i + 1;
        }
        if (captured) entries[0] = (XrtCallableEntrySpec){0, specs, count};
        context->image = (XrtCallableImage){types, 1, entries, entries_count};
        context->captures = values;
        XrtExecutionStatus status = xrt_run_embedded_owned(owner, &context->execution.failure,
            native_callable_large_scope_root, context, &context->execution.root);
        uint64_t expected = captured ? count : count - 1;
        bool valid = status == XRT_EXECUTION_COMPLETED && context->execution.result == expected;
        if (!valid) {
            fprintf(stderr, "callable image entries=%u captures=%u status=%u failure=%u result=%llu\n",
                (unsigned)entries_count, captured ? (unsigned)count : 0u, (unsigned)status,
                (unsigned)context->execution.failure.kind, (unsigned long long)context->execution.result);
        }
        CHECK(xrt_value_root_empty(&context->execution.root));
        xr_free(context);
        xr_free(values);
        xr_free(specs);
        xr_free(entries);
        CHECK(valid);
    }
    xrt_locations_release(owner);
    puts("callable images preserve uint32 entry and capture ordinals above 65536");
}
