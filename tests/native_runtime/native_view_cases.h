/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_view_cases.h - Actual published Cell identity and activation retirement
 */
#include "xray_native_view.h"
typedef struct NativeViewCase {
    XrtModuleState *state;
    uint32_t mode;
    int64_t result;
} NativeViewCase;

static XrXirValueStatus native_view_state(NativeViewCase *context) {
    bool ordinary = context->mode == 4;
    XrtModuleTypeSpec type = {ordinary ? XRT_MODULE_ARRAY_I64 : XRT_MODULE_CELL_ARRAY_I64, 0};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.flags=XRT_MODULE_SLOT_MUTABLE, .size=ordinary ? 8u : 16u, .alignment=8};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=slot.size, .payload_alignment=8, .byte_limit=UINT64_MAX};
    XrtModuleLayout *layout = NULL;
    XrXirValueStatus status = (XrXirValueStatus)xrt_module_layout_new(&spec, &layout);
    if (status == XR_XIR_VALUE_OK) status = (XrXirValueStatus)xrt_module_state_new(layout, UINT64_MAX, &context->state);
    xrt_module_layout_drop(&layout);
    return status;
}
static void native_view_root(void *opaque) {
    NativeViewCase *context = opaque;
    XrtViewFrame frame = xrt_view_frame_enter(1, 1, 0);
    xrt_slice_i64_cleanup(frame, 0, 0);
    if (context->mode == 6) { xrt_view_frame_leave(frame, 0); context->result = 77; return; }
    xrt_module_require(xrt_module_begin(context->state, 0), 0);
    const int64_t numbers[] = {11, 22, 33};
    XrtArray *array = xrt_array_i64_new(numbers, 3, 0);
    XrtViewCell cell = xrt_view_cell_new(frame, 0, array, 0);
    XrtModuleRef *reference = NULL;
    if (context->mode == 4) xrt_module_require(xrt_module_construct(context->state, 0, &array, &reference), 0);
    else reference = xrt_view_cell_construct(frame, cell, context->state, 0, 0);
    xrt_array_drop(&array);
    if (context->mode != 1) xrt_module_require(xrt_module_publish(reference), 0);
    XrtViewFrame child = xrt_view_frame_enter(1, 1, 0);
    XrtModuleAccess access = {0};
    xrt_module_require(xrt_module_cache(context->state, 0, &access), 0);
    XrtViewCell imported = xrt_view_cell_import(child, 0, access.reference, 0);
    XrtSliceI64 part = xrt_slice_i64_new(child, 0, imported, (XrtViewRange){1, 3, false}, 0);
    if (context->mode == 2) { xrt_view_cell_i64_set(child, imported, 0, 77, 0); CHECK(false); }
    if (context->mode == 3) { xrt_null_unwrap(0); CHECK(false); }
    part = xrt_slice_i64_return(child, part, 0);
    xrt_view_frame_leave(child, 0);
    if (context->mode == 5) { (void)xrt_view_cell_read(frame, imported, 0); CHECK(false); }
    part = xrt_slice_i64_bind(frame, 0, part, 0);
    CHECK(xrt_slice_i64_get(frame, part, 0, 0) == 22 && xrt_slice_i64_length(frame, part, 0) == 2);
    xrt_slice_i64_cleanup(frame, 0, 0);
    xrt_view_cell_i64_set(frame, cell, 0, 77, 0);
    context->result = xrt_view_cell_i64_get(frame, cell, 0, 0);
    xrt_view_frame_leave(frame, 0);
}
static XrXirValueStatus native_view_run(const XrtLocationOwner *locations, uint32_t mode) {
    NativeViewCase context = {.mode=mode};
    XrXirValueStatus status = native_view_state(&context);
    if (status == XR_XIR_VALUE_OK) {
        XrtExecutionFailure failure = {0};
        XrtValueRoot values = {0};
        XrtExecutionStatus actual = xrt_run_embedded_owned(locations, &failure, native_view_root, &context, &values);
        CHECK(xrt_value_root_empty(&values));
        if (actual == XRT_EXECUTION_COMPLETED) CHECK(context.result == 77);
        else {
            CHECK(actual == XRT_EXECUTION_FAILURE &&
                (failure.kind == XRT_FAILURE_VALUE || failure.kind == XRT_FAILURE_NULL_UNWRAP));
            status = failure.kind == XRT_FAILURE_VALUE ? failure.detail.value.status : XR_XIR_VALUE_BOUNDS;
        }
    }
    xrt_module_state_drop(&context.state);
    return status;
}
static void native_view_cases(const XrtLocationOwner *locations) {
    for (uint32_t mode = 0; mode <= 6; ++mode) {
        XrXirValueStatus expected = mode == 0 || mode == 6 ? XR_XIR_VALUE_OK :
            mode == 3 ? XR_XIR_VALUE_BOUNDS : XR_XIR_VALUE_BAD_ARGUMENT;
        CHECK(native_view_run(locations, mode) == expected);
    }
}
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
static void native_view_allocation_sweep(const XrtLocationOwner *locations) {
    for (uint32_t mode = 0; mode <= 6; ++mode) {
        native_array_calls = 0;
        (void)native_view_run(locations, mode);
        size_t sites = native_array_calls;
        CHECK(!native_array_live && !native_array_bytes);
        for (size_t point = 0; point < sites; ++point) {
            native_array_calls = 0; native_array_fail_at = point; native_array_injected = false;
            CHECK(native_view_run(locations, mode) == XR_XIR_VALUE_OOM);
            CHECK(native_array_injected && !native_array_live && !native_array_bytes);
        }
        native_array_fail_at = SIZE_MAX;
        printf("View module mode %u runtime failure points: %zu\n", mode, sites);
    }
}
#endif
