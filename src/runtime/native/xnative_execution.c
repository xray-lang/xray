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
#include "base/xchecks.h"
#include "os/os_time.h"
#include "shared/xr_value_format_core.h"
#include <setjmp.h>

typedef struct XrtExecutionBinding {
    XrtSourceMap map;
    const struct XrtExecutionBinding *previous;
    jmp_buf *target;
    XrtExecutionFailure *failure;
} XrtExecutionBinding;

static XR_THREAD_LOCAL const XrtExecutionBinding *current_execution;

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
    const XrtExecutionBinding binding = {.map = *map, .previous = current_execution};
    current_execution = &binding;
    root(context);
    current_execution = binding.previous;
}

XR_FUNC XrtExecutionStatus xrt_run_embedded(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context) {
    if (!locations || !failure || !root)
        return XRT_EXECUTION_INVALID;
    jmp_buf target;
    const XrtExecutionBinding binding = {
        .map = {xrt_locations_get(locations, 1), xrt_locations_count(locations)},
        .previous = current_execution, .target = &target, .failure = failure};
    current_execution = &binding;
    if (setjmp(target) == 0) {
        root(context);
        current_execution = binding.previous;
        return XRT_EXECUTION_COMPLETED;
    }
    current_execution = binding.previous;
    return XRT_EXECUTION_FAILURE;
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

XRAY_API_NORET void xrt_arithmetic_panic(uint32_t code, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && (code == 420 || code == 421) && source_id <= binding->map.count,
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

static int64_t clock_read(uint32_t kind, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && kind >= 1 && kind <= 3 && source_id <= binding->map.count,
        "Clock reads require an active execution, registered kind and source id");
    uint64_t reading = kind == 1 ? xr_time_realtime_ns() : kind == 2 ?
        xr_time_process_cpu_ns() : xr_time_monotonic_ns();
    if (reading <= INT64_MAX && (reading || kind == 2)) return (int64_t)reading;
    XrtHostClockFailure failure = {kind, source_id,
        reading > INT64_MAX ? XRT_CLOCK_OUT_OF_RANGE : XRT_CLOCK_READ_FAILED};
    if (binding->target) {
        binding->failure->kind = XRT_FAILURE_HOST_CLOCK;
        binding->failure->detail.clock = failure;
        longjmp(*binding->target, 1);
    }
    xrt_clock_exit(&binding->map, failure);
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
