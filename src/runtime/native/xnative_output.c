/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_output.c - Shared integer output and standalone failure diagnostics
 *
 * KEY CONCEPT:
 *   Successful output needs no execution lookup. Failure is handed to the
 *   active root only after the I/O helper has restored its local OS state.
 */
#include "xnative_execution.h"
#include "base/xchecks.h"
#include "shared/xr_value_format_core.h"
#include <stdlib.h>
#include <string.h>

typedef struct XrtStdoutBinding {
    XrtStdoutGuard guard;
    void *context;
} XrtStdoutBinding;

static XR_THREAD_LOCAL XrtStdoutBinding stdout_binding;

XR_FUNC void xrt_stdout_set_guard(XrtStdoutGuard guard, void *context) {
    stdout_binding = (XrtStdoutBinding){guard, context};
}

static size_t unsigned_decimal(char *bytes, uint64_t value) {
    char reversed[20];
    size_t count = 0;
    do { reversed[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (size_t i = 0; i < count; ++i) bytes[i] = reversed[count - i - 1];
    return count;
}

static int diagnostic_stderr(void *context, const void *bytes, size_t length) {
    (void)context;
    (void)xrt_io_write(2, bytes, length);
    return 1;
}

static const XrValueFormatSink stderr_sink = {NULL, diagnostic_stderr};

static void diagnostic_text(const char *text) { (void)xr_value_format_text(stderr_sink, text); }

static bool diagnostic_number(XrValueFormatSink sink, uint32_t number) {
    char bytes[20];
    return xr_value_format_write(sink, bytes, unsigned_decimal(bytes, number));
}

static bool diagnostic_path(XrValueFormatSink sink, const char *path, size_t length) {
    // Escape control bytes so even unusual paths remain a single diagnostic.
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (c < 32 || c == 127 || c == '\\') {
            char escaped[] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
            if (!xr_value_format_write(sink, escaped, sizeof escaped)) return false;
        } else if (!xr_value_format_write(sink, path + i, 1)) return false;
    }
    return true;
}

static bool diagnostic_location(XrValueFormatSink sink, const XrtSourceMap *map, uint32_t id) {
    if ((map->count == 0) != (map->locations == NULL) || map->count > UINT32_MAX)
        return xr_value_format_text(sink, "invalid source map");
    if (!id) return xr_value_format_text(sink, "unknown source");
    if ((size_t)id > map->count)
        return xr_value_format_text(sink, "invalid source id ") && diagnostic_number(sink, id);
    const XrtSourceLocation *location = &map->locations[id - 1];
    bool has_end = location->end_line != 0 || location->end_column != 0;
    if (!location->path || !location->path_length || !location->line || !location->column ||
        (has_end && (!location->end_line || !location->end_column ||
         location->end_line < location->line ||
         (location->end_line == location->line && location->end_column < location->column))))
        return xr_value_format_text(sink, "invalid source location");
    return diagnostic_path(sink, location->path, location->path_length) &&
        xr_value_format_text(sink, ":") && diagnostic_number(sink, location->line) &&
        xr_value_format_text(sink, ":") && diagnostic_number(sink, location->column) &&
        (!has_end || (xr_value_format_text(sink, "-") && diagnostic_number(sink, location->end_line) &&
            xr_value_format_text(sink, ":") && diagnostic_number(sink, location->end_column)));
}

XR_FUNC void xrt_temp_failure_report(const XrtSourceLocation *source) {
    XrtSourceMap map = {source, source ? 1u : 0u};
    diagnostic_text("XR_RUN_6020: temporary path rollback is unresolved at ");
    (void)diagnostic_location(stderr_sink, &map, source ? 1u : 0u);
    diagnostic_text("\n");
}
XR_FUNC bool xrt_value_failure_format(XrXirValueStatus status, const XrtSourceLocation *source,
    const XrtSourceLocation *close, const XrValueFormatSink *sink) {
    if (!sink) return false;
    XrtSourceMap map = {source, source ? 1u : 0u};
    bool prefix = status == XR_XIR_VALUE_CANCELLED ? xr_value_format_text(*sink, "[Uncaught Cancellation] at ") :
        xr_value_format_text(*sink, "XR_RUN_6002: value operation rejected (status ") &&
        diagnostic_number(*sink, (uint32_t)status) && xr_value_format_text(*sink, ") at ");
    XrtSourceMap close_map = {close, close ? 1u : 0u};
    return prefix && diagnostic_location(*sink, &map, source ? 1u : 0u) && xr_value_format_text(*sink, "\n") &&
        (!close || (xr_value_format_text(*sink, "note: directory cleanup rejected its token at ") &&
            diagnostic_location(*sink, &close_map, 1) && xr_value_format_text(*sink, "\n")));
}

XR_FUNC void xrt_value_failure_report(XrXirValueStatus status, const XrtSourceLocation *source) {
    (void)xrt_value_failure_format(status, source, NULL, &stderr_sink);
}

XR_FUNC void xrt_value_failure_report_with_directory_close(XrXirValueStatus status, const XrtSourceLocation *primary,
    const XrtSourceLocation *close) {
    (void)xrt_value_failure_format(status, primary, close, &stderr_sink);
}

XR_FUNC bool xrt_null_unwrap_format(const XrtSourceLocation *source, const XrValueFormatSink *sink) {
    XrtSourceMap map = {source, source ? 1u : 0u};
    return sink && xr_value_format_text(*sink, "[Uncaught Panic] E0413: null value unwrapped at ") &&
        diagnostic_location(*sink, &map, source ? 1u : 0u) && xr_value_format_text(*sink, "\n");
}

XR_FUNC void xrt_null_unwrap_report(const XrtSourceLocation *source) {
    (void)xrt_null_unwrap_format(source, &stderr_sink);
}

XR_FUNC XR_NORET void xrt_output_exit(const XrtSourceMap *map, XrtIoResult failure,
    XrtOutputOperation operation, uint32_t id) {
    if (!failure.broken_pipe) {
        diagnostic_text("xray: output ");
        diagnostic_text(operation == XRT_OUTPUT_FLUSH ? "flush" : "write");
        diagnostic_text(" failed at ");
        (void)diagnostic_location(stderr_sink, map, id);
        if (failure.domain != XRT_IO_NONE) {
            diagnostic_text(failure.domain == XRT_IO_WINDOWS ? " [win32=" : " [errno=");
            (void)diagnostic_number(stderr_sink, failure.code); diagnostic_text("]");
        } else diagnostic_text(" [no system code]");
        diagnostic_text("\n");
    }
    _Exit(74);
}

static void print_record(const char *bytes, size_t length, bool append_newline, uint32_t source_id) {
    XrtStdoutBinding binding = stdout_binding;
    if (binding.guard) binding.guard(binding.context, true);
    /* Flush preceding CRT output before bypassing newline translation. */
    XrtIoResult result = xrt_io_flush_stdout();
    XrtOutputOperation operation = XRT_OUTPUT_FLUSH;
    if (!result.failed) {
        operation = XRT_OUTPUT_WRITE;
        result = xrt_io_write(1, bytes, length);
        if (!result.failed && append_newline) result = xrt_io_write(1, "\n", 1);
    }
    if (binding.guard) binding.guard(binding.context, false);
    if (result.failed) xrt_end_execution(result, operation, source_id);
}

XRAY_API void xrt_print_i64(int64_t value, uint32_t source_location_id) {
    char bytes[22];
    size_t length = 0;
    uint64_t magnitude = (uint64_t)value;
    if (value < 0) { bytes[length++] = '-'; magnitude = UINT64_C(0) - magnitude; }
    length += unsigned_decimal(bytes + length, magnitude);
    bytes[length++] = '\n';
    print_record(bytes, length, false, source_location_id);
}

XRAY_API void xrt_print_string(const char *bytes, size_t byte_length, uint32_t source_location_id) {
    XR_CHECK(bytes || !byte_length, "String output requires bytes for a nonempty span");
    print_record(bytes, byte_length, true, source_location_id);
}

XR_FUNC XR_NORET void xrt_clock_exit(const XrtSourceMap *map, XrtHostClockFailure failure) {
    diagnostic_text("xray: host clock ");
    diagnostic_text(failure.kind == 1 ? "realtime" : failure.kind == 2 ? "process-cpu" :
        failure.kind == 4 ? "utc-offset" : "monotonic");
    diagnostic_text(failure.reason == XRT_CLOCK_OUT_OF_RANGE ? " out of range at " : " read failed at ");
    (void)diagnostic_location(stderr_sink, map, failure.source_id);
    diagnostic_text("\n");
    _Exit(1);
}
