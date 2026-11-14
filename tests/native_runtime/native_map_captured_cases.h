/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_map_captured_cases.h - Captured Map references retain actual Cell identity
 *
 * KEY CONCEPT:
 *   Cell declarations and Map values use different arena ordinals. Shared
 *   Cell mutation publishes a shared replacement while saved values stay live.
 */
#include "xir/xxir_type_arena_internal.h"
static XrXirTypeArena *native_map_cell_types(void) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirTypeNode nodes[] = {{.kind=XR_XIR_TYPE_ARRAY, .element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MAP, .element=XR_XIR_STRING, .result=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL, .element=(XrXirType)257},
        {.kind=XR_XIR_TYPE_CELL, .element=XR_XIR_I64}};
    XrXirTypes types = {nodes, 4, NULL, NULL};
    XrXirTypeArena *arena = NULL;
    OK(xr_xir_compile_type_arena_new(&context, &types, &arena));
    xr_compile_resources_release(context.resources);
    return arena;
}
static void native_map_captured_root(void *opaque) {
    bool *completed = opaque;
    XrtValueRoot *root = xrt_value_root_current(1);
    XrXirTypeArena *arena = native_map_cell_types();
    uint64_t unchanged = 71;
    CHECK(xir_native_type_arena_string_i64_map_bytes(arena, &unchanged) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(unchanged == 71);
    XrXirValueAdmission admission = {arena, xrt_value_root_domain(root, 1),
        NULL, NULL, UINT64_MAX, UINT64_MAX};
    XrtMap *initial = native_map_create();
    XrXirValue value = {((XirObject *)initial)->type, 0, (int64_t)(intptr_t)initial}, cell = {0};
    CHECK(!xr_xir_value_argument(&value, arena, (XrXirType)257));
    CHECK(xir_native_type_arena_receive_string_i64_map(arena, (XrXirType)256,
        xrt_map_root_types(1)) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_compile_type_arena_retain(arena));
    CHECK(xir_native_type_arena_receive_string_i64_map(arena, (XrXirType)257,
        xrt_map_root_types(1)) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!xir_native_type_arena_string_i64_map_source(arena, (XrXirType)257));
    xr_xir_compile_type_arena_drop(arena);
    OK(xir_native_type_arena_receive_string_i64_map(arena, (XrXirType)257, xrt_map_root_types(1)));
    CHECK(xir_native_type_arena_string_i64_map_source(arena, (XrXirType)257) == xrt_map_root_types(1));
    CHECK(!xir_native_type_arena_string_i64_map_source(arena, (XrXirType)256));
    CHECK(xr_xir_value_argument(&value, arena, (XrXirType)257));
    OK(xr_xir_value_admit(&value, (XrXirType)257, &admission));
    OK(xr_xir_cell_new(admission.domain, arena, (XrXirType)258, &value, &admission, &cell));
    xrt_map_drop(&initial);
    XrtMapRef reference = xrt_map_ref_captured((struct XirCell *)(intptr_t)cell.payload, 1);
    XrtMapRef alias = reference;
    XrtMap *saved = xrt_map_ref_read(reference, 1);
    CHECK(((XirObject *)saved)->type == (XrXirType)256);
    OK(xr_xir_value_share(&cell));
    XrtString *key = xrt_string_new("same", 4, 1);
    xrt_map_ref_string_i64_set(reference, key, 99, 1);
    XrtMap *changed = xrt_map_ref_read(alias, 1);
    CHECK(changed != saved && reference.captured == alias.captured);
    CHECK(xir_object_references((XirObject *)reference.captured) < 0);
    CHECK(xir_object_references((XirObject *)changed) < 0);
    native_map_value(saved, "same", 4, 42);
    native_map_value(changed, "same", 4, 99);
    xrt_map_ref_write(alias, saved, 1);
    XrtMap *restored = xrt_map_ref_read(reference, 1);
    native_map_value(restored, "same", 4, 42);
    CHECK(xrt_map_ref_string_i64_remove(alias, key, 1));
    CHECK(!xrt_map_ref_string_i64_remove(reference, key, 1));
    xrt_map_ref_clear(reference, 1);
    XrtMap *empty = xrt_map_ref_read(alias, 1);
    CHECK(!xrt_map_length(empty, 1));
    xr_xir_value_drop(&cell);
    xr_xir_compile_type_arena_drop(arena);
    native_map_value(saved, "same", 4, 42);
    native_map_value(changed, "same", 4, 99);
    xrt_map_drop(&saved); xrt_map_drop(&changed); xrt_map_drop(&restored); xrt_map_drop(&empty);
    xrt_string_drop(&key);
    *completed = true;
}
typedef struct NativeCapturedMapRetirement {
    XrXirValue cell;
    XrXirTypeArena *arena;
    bool completed;
} NativeCapturedMapRetirement;
static void native_map_captured_initialize(void *opaque) {
    NativeCapturedMapRetirement *probe = opaque;
    XrXirTypeArena *source = NULL, *different = NULL;
    uint64_t bytes = 0;
    OK(xrt_map_types_new(UINT64_MAX, &source, &bytes));
    uint64_t observed = 0;
    OK(xir_native_type_arena_string_i64_map_bytes(source, &observed));
    CHECK(observed == bytes && observed);
    CHECK(xir_native_type_arena_string_i64_map_bytes(source, NULL) == XR_XIR_VALUE_BAD_ARGUMENT);
    OK(xrt_map_root_bind_types(source));
    OK(xrt_map_root_bind_types(source));
    const XrXirTypeArena *borrowed = NULL;
    OK(xrt_map_root_types_ensure(&borrowed));
    CHECK(borrowed == source);
    OK(xrt_map_types_new(UINT64_MAX, &different, &bytes));
    CHECK(xrt_map_root_bind_types(different) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xrt_map_root_types(1) == source);
    xr_xir_compile_type_arena_drop(different);
    xr_xir_compile_type_arena_drop(source);
    probe->arena = native_map_cell_types();
    XrtMap *initial = native_map_create();
    OK(xir_native_type_arena_receive_string_i64_map(probe->arena, (XrXirType)257, xrt_map_root_types(1)));
    XrXirValue value = {((XirObject *)initial)->type, 0, (int64_t)(intptr_t)initial};
    XrXirValueAdmission admission = {probe->arena, xrt_value_root_domain(xrt_value_root_current(1), 1),
        NULL, NULL, UINT64_MAX, UINT64_MAX};
    OK(xr_xir_cell_new(admission.domain, probe->arena, (XrXirType)258, &value, &admission, &probe->cell));
    xrt_map_drop(&initial);
}
static void native_map_captured_after_retirement(void *opaque) {
    NativeCapturedMapRetirement *probe = opaque;
    XrtMapRef reference = xrt_map_ref_captured((struct XirCell *)(intptr_t)probe->cell.payload, 1);
    XrtMap *saved = xrt_map_ref_read(reference, 1);
    native_map_value(saved, "same", 4, 42);
    XrtString *key = xrt_string_new("same", 4, 1);
    xrt_map_ref_string_i64_set(reference, key, 101, 1);
    XrtMap *changed = xrt_map_ref_read(reference, 1);
    native_map_value(changed, "same", 4, 101);
    native_map_value(saved, "same", 4, 42);
    xr_xir_value_drop(&probe->cell);
    xr_xir_compile_type_arena_drop(probe->arena);
    probe->arena = NULL;
    native_map_value(changed, "same", 4, 101);
    xrt_map_drop(&saved); xrt_map_drop(&changed); xrt_string_drop(&key);
    probe->completed = true;
}
static void native_map_captured_cases(XrtLocationOwner *locations) {
    const XrXirTypeArena *absent = NULL;
    CHECK(xrt_map_root_types_ensure(&absent) == XR_XIR_VALUE_BAD_ARGUMENT && !absent);
    XrtExecutionFailure failure = {0};
    bool completed = false;
    XrtExecutionStatus status = xrt_run_embedded(locations, &failure, native_map_captured_root, &completed);
    if (status != XRT_EXECUTION_COMPLETED)
        fprintf(stderr, "Captured Map execution status %u, failure %u, value status %u\n",
            (unsigned)status, (unsigned)failure.kind, (unsigned)failure.detail.value.status);
    CHECK(status == XRT_EXECUTION_COMPLETED && completed);
    NativeCapturedMapRetirement probe = {0};
    CHECK(xrt_run_embedded(locations, &failure, native_map_captured_initialize, &probe) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_run_embedded(locations, &failure, native_map_captured_after_retirement, &probe) ==
        XRT_EXECUTION_COMPLETED && probe.completed);
    puts("Captured Map actual Cell identity, cross-arena type and shared publication passed");
}
