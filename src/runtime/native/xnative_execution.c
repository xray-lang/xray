/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_execution.c - Scoped image binding and synchronous output recovery
 *
 * KEY CONCEPT:
 *   Bindings stay immutable while active. Only caller-prepared failure data
 *   changes across recovery, so no changed automatic owner is lost to a jump.
 */
#include "xnative_execution.h"
#include "xnative_module_state.h"
#include "base/xchecks.h"
#include "os/os_time.h"
#include "shared/xr_time_offset.h"
#include "shared/xr_value_format_core.h"
#include <setjmp.h>
#include <stdio.h>

typedef struct XrtExecutionBinding {
    XrtSourceMap map;
    const struct XrtExecutionBinding *previous;
    jmp_buf *target;
    XrtExecutionFailure *failure;
    XrtValueRoot *strings;
} XrtExecutionBinding;

static XR_THREAD_LOCAL const XrtExecutionBinding *current_execution;

XR_FUNC XR_NORET void xrt_value_failure(XrXirValueStatus status, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && binding->target && status != XR_XIR_VALUE_OK &&
        source_id <= binding->map.count, "Value failure retains the actual root status and source id");
    binding->failure->kind = XRT_FAILURE_VALUE;
    binding->failure->detail.value.status = status;
    binding->failure->detail.value.source_id = source_id;
    longjmp(*binding->target, 1);
}
static XR_NORET void native_failure_exit(const XrtSourceMap *map, const XrtExecutionFailure *failure);
XRAY_API_NORET void xrt_fixed_array_bounds(uint32_t source_id) {
    xrt_value_failure(XR_XIR_VALUE_BOUNDS, source_id);
}

/* Mutable managed-value state belongs to the caller of this setjmp function. */
static XrtExecutionStatus run_bound(const XrtSourceMap *map, XrtExecutionFailure *failure,
    XrtSynchronousRoot root, void *context, XrtValueRoot *strings) {
    jmp_buf target;
    const XrtExecutionBinding binding = {.map = *map, .previous = current_execution,
        .target = &target, .failure = failure, .strings = strings};
    current_execution = &binding;
    const XrtValueBinding previous_strings = xrt_value_root_bind((XrtValueBinding){strings, map->count});
    if (setjmp(target) == 0) {
        root(context);
        xrt_value_root_drain(strings);
        current_execution = binding.previous;
        (void)xrt_value_root_bind(previous_strings);
        return XRT_EXECUTION_COMPLETED;
    }
    xrt_value_root_drain(strings);
    current_execution = binding.previous;
    (void)xrt_value_root_bind(previous_strings);
    return XRT_EXECUTION_FAILURE;
}

static bool source_map_valid(const XrtSourceMap *map) {
    if (!map || (map->count == 0) != (map->locations == NULL) ||
        map->count > UINT32_MAX || map->count > SIZE_MAX / sizeof(*map->locations))
        return false;
    for (size_t i = 0; i < map->count; ++i) {
        const XrtSourceLocation *location = &map->locations[i];
        if (!location->path || !location->path_length || location->path_length == SIZE_MAX ||
            !location->line || !location->column)
            return false;
        if (!location->end_line && !location->end_column) continue;
        if (!location->end_line || !location->end_column || location->end_line < location->line ||
            (location->end_line == location->line && location->end_column < location->column))
            return false;
    }
    return true;
}

XRAY_API void xrt_run_standalone(const XrtSourceMap *map, XrtSynchronousRoot root, void *context) {
    XR_CHECK(root && source_map_valid(map), "A standalone root requires a valid prepared source map");
    XrtValueRoot strings = {0};
    XrtExecutionFailure failure = {0};
    if (run_bound(map, &failure, root, context, &strings) == XRT_EXECUTION_FAILURE)
        native_failure_exit(map, &failure);
}

typedef struct XrtStateInvocation {
    XrtSynchronousStateRoot root;
    void *context;
    XrtModuleState *state;
} XrtStateInvocation;

static void state_root_invoke(void *context) {
    const XrtStateInvocation *invocation = context;
    invocation->root(invocation->context, invocation->state);
}

XRAY_API void xrt_run_standalone_state(const XrtSourceMap *map, XrtSynchronousStateRoot root,
    void *context, const XrtModuleLayoutSpec *spec) {
    XR_CHECK(root && spec && source_map_valid(map), "A state root requires a prepared map and layout");
    XrtModuleLayout *layout = NULL;
    XrtModuleState *state = NULL;
    XrtModuleStatus status = xrt_module_layout_new(spec, &layout);
    if (!status) status = xrt_module_state_new(layout, spec->byte_limit, &state);
    xrt_module_layout_drop(&layout);
    XrtExecutionFailure failure = {0};
    if (status) {
        xrt_module_state_drop(&state);
        failure.kind = XRT_FAILURE_VALUE;
        failure.detail.value.status = (XrXirValueStatus)status;
        native_failure_exit(map, &failure);
    }
    XrtValueRoot strings = {0};
    XrtStateInvocation invocation = {root, context, state};
    XrtExecutionStatus result = run_bound(map, &failure, state_root_invoke, &invocation, &strings);
    xrt_module_state_drop(&state);
    if (result == XRT_EXECUTION_FAILURE) native_failure_exit(map, &failure);
}

XR_FUNC XrtExecutionStatus xrt_run_embedded(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context) {
    XrtValueRoot strings = {0};
    return xrt_run_embedded_owned(locations, failure, root, context, &strings);
}
XR_FUNC XrtExecutionStatus xrt_run_embedded_owned(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context, XrtValueRoot *strings) {
    if (!locations || !failure || !root || !strings || !xrt_value_root_empty(strings))
        return XRT_EXECUTION_INVALID;
    XrtSourceMap map = {xrt_locations_get(locations, 1), xrt_locations_count(locations)};
    return run_bound(&map, failure, root, context, strings);
}

XR_FUNC XR_NORET void xrt_end_execution(XrtIoResult failure,
    XrtOutputOperation operation, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && failure.failed, "Output failure requires an active execution");
    if (binding->target) {
        binding->failure->kind = XRT_FAILURE_OUTPUT;
        binding->failure->detail.output = (XrtOutputFailure) {failure, operation, source_id};
        longjmp(*binding->target, 1);
    }
    xrt_output_exit(&binding->map, failure, operation, source_id);
}

static int panic_write(void *context, const void *bytes, size_t length) {
    (void)context;
    return !xrt_io_write(2, bytes, length).failed;
}
static XR_NORET void native_failure_exit(const XrtSourceMap *map, const XrtExecutionFailure *failure) {
    if (failure->kind == XRT_FAILURE_OUTPUT)
        xrt_output_exit(map, failure->detail.output.io,
            failure->detail.output.operation, failure->detail.output.source_id);
    if (failure->kind == XRT_FAILURE_HOST_CLOCK) xrt_clock_exit(map, failure->detail.clock);
    if (failure->kind == XRT_FAILURE_NULL_UNWRAP) {
        uint32_t id = failure->detail.panic.source_id;
        xrt_null_unwrap_report(id && id <= map->count ? &map->locations[id - 1] : NULL);
        _Exit(1);
    }
    if (failure->kind == XRT_FAILURE_VALUE) {
        uint32_t id = failure->detail.value.source_id;
        const XrtSourceLocation *source = id && id <= map->count ? &map->locations[id - 1] : NULL;
        xrt_value_failure_report(failure->detail.value.status, source);
        _Exit(1);
    }
    XR_CHECK(failure->kind == XRT_FAILURE_ARITHMETIC_PANIC, "Native failure has a known physical carrier");
    XrValueFormatSink sink = {NULL, panic_write};
    if (!xr_value_format_panic(sink, failure->detail.panic.code, 0, 0, 0, 0, NULL, 0, 0)) {
        static const char fallback[] = "XR_RUN_6005: cannot render uncaught typed panic\n";
        (void)xrt_io_write(2, fallback, sizeof(fallback) - 1);
    }
    _Exit(1);
}

XRAY_API_NORET void xrt_null_unwrap(uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && binding->target && source_id <= binding->map.count,
        "Null unwrap uses the active execution and its actual source point");
    binding->failure->kind = XRT_FAILURE_NULL_UNWRAP;
    binding->failure->detail.panic = (XrtArithmeticPanic){413, source_id};
    longjmp(*binding->target, 1);
}

XRAY_API_NORET void xrt_arithmetic_panic(uint32_t code, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && (code == 420 || code == 421 || code == 422) && source_id <= binding->map.count,
        "Arithmetic panic requires an active execution, valid code and source id");
    if (binding->target) {
        binding->failure->kind = XRT_FAILURE_ARITHMETIC_PANIC;
        binding->failure->detail.panic = (XrtArithmeticPanic) {code, source_id};
        longjmp(*binding->target, 1);
    }
    XrValueFormatSink sink = {NULL, panic_write};
    if (!xr_value_format_panic(sink, code, 0, 0, 0, 0, NULL, 0, 0)) {
        static const char fallback[] = "XR_RUN_6005: cannot render uncaught typed panic\n";
        (void)xrt_io_write(2, fallback, sizeof(fallback) - 1);
    }
    _Exit(1);
}

static XR_NORET void clock_failure(uint32_t kind, uint32_t source_id, XrtHostClockReason reason) {
    const XrtExecutionBinding *binding = current_execution;
    XrtHostClockFailure failure = {kind, source_id, reason};
    if (binding->target) {
        binding->failure->kind = XRT_FAILURE_HOST_CLOCK;
        binding->failure->detail.clock = failure;
        longjmp(*binding->target, 1);
    }
    xrt_clock_exit(&binding->map, failure);
}
static int64_t clock_read(uint32_t kind, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && kind >= 1 && kind <= 3 && source_id <= binding->map.count,
        "Clock reads require an active execution, registered kind and source id");
    uint64_t reading = kind == 1 ? xr_time_realtime_ns() : kind == 2 ?
        xr_time_process_cpu_ns() : xr_time_monotonic_ns();
    if (reading <= INT64_MAX && (reading || kind == 2)) return (int64_t)reading;
    clock_failure(kind, source_id,
        reading > INT64_MAX ? XRT_CLOCK_OUT_OF_RANGE : XRT_CLOCK_READ_FAILED);
}
XRAY_API int64_t xrt_clock_realtime_nanos(uint32_t source_id) {
    return clock_read(1, source_id);
}
XRAY_API int64_t xrt_clock_process_cpu_nanos(uint32_t source_id) {
    return clock_read(2, source_id);
}
XRAY_API int64_t xrt_clock_monotonic_nanos(uint32_t source_id) {
    return clock_read(3, source_id);
}
XRAY_API int64_t xrt_clock_utc_offset_at(int64_t seconds, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && source_id <= binding->map.count,
        "UTC offset queries require an active execution and registered source id");
    int64_t minutes = 0;
    if (xr_time_utc_offset_at(seconds, &minutes)) return minutes;
    clock_failure(4, source_id, XRT_CLOCK_OUT_OF_RANGE);
}
