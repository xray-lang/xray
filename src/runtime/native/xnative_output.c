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
#include <stdlib.h>
#include <string.h>

static size_t unsigned_decimal(char *bytes, uint64_t value) {
    char reversed[20];
    size_t count = 0;
    do { reversed[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (size_t i = 0; i < count; ++i) bytes[i] = reversed[count - i - 1];
    return count;
}

static void diagnostic_bytes(const char *bytes, size_t length) {
    (void)xrt_io_write(2, bytes, length);
}

static void diagnostic_text(const char *text) { diagnostic_bytes(text, strlen(text)); }

static void diagnostic_number(uint32_t number) {
    char bytes[20];
    diagnostic_bytes(bytes, unsigned_decimal(bytes, number));
}

static void diagnostic_path(const char *path, size_t length) {
    // Escape control bytes so even unusual paths remain a single diagnostic.
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (c < 32 || c == 127 || c == '\\') {
            char escaped[] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
            diagnostic_bytes(escaped, sizeof escaped);
        } else diagnostic_bytes(path + i, 1);
    }
}

static void diagnostic_location(const XrtSourceMap *map, uint32_t id) {
    if ((map->count == 0) != (map->locations == NULL) || map->count > UINT32_MAX) {
        diagnostic_text("invalid source map");
        return;
    }
    if (!id) { diagnostic_text("unknown source"); return; }
    if ((size_t)id > map->count) { diagnostic_text("invalid source id "); diagnostic_number(id); return; }
    const XrtSourceLocation *location = &map->locations[id - 1];
    bool has_end = location->end_line != 0 || location->end_column != 0;
    if (!location->path || !location->path_length || !location->line || !location->column ||
        (has_end && (!location->end_line || !location->end_column ||
         location->end_line < location->line ||
         (location->end_line == location->line && location->end_column < location->column)))) {
        diagnostic_text("invalid source location");
        return;
    }
    diagnostic_path(location->path, location->path_length);
    diagnostic_text(":"); diagnostic_number(location->line);
    diagnostic_text(":"); diagnostic_number(location->column);
    if (has_end) {
        diagnostic_text("-"); diagnostic_number(location->end_line);
        diagnostic_text(":"); diagnostic_number(location->end_column);
    }
}

XR_FUNC XR_NORET void xrt_output_exit(const XrtSourceMap *map, XrtIoResult failure,
    XrtOutputOperation operation, uint32_t id) {
    if (!failure.broken_pipe) {
        diagnostic_text("xray: output ");
        diagnostic_text(operation == XRT_OUTPUT_FLUSH ? "flush" : "write");
        diagnostic_text(" failed at ");
        diagnostic_location(map, id);
        if (failure.domain != XRT_IO_NONE) {
            diagnostic_text(failure.domain == XRT_IO_WINDOWS ? " [win32=" : " [errno=");
            diagnostic_number(failure.code); diagnostic_text("]");
        } else diagnostic_text(" [no system code]");
        diagnostic_text("\n");
    }
    _Exit(74);
}

XRAY_API void xrt_print_i64(int64_t value, uint32_t source_location_id) {
    char bytes[22];
    size_t length = 0;
    uint64_t magnitude = (uint64_t)value;
    if (value < 0) { bytes[length++] = '-'; magnitude = UINT64_C(0) - magnitude; }
    length += unsigned_decimal(bytes + length, magnitude);
    bytes[length++] = '\n';
    // Flush any preceding CRT output before bypassing its newline translation.
    XrtIoResult result = xrt_io_flush_stdout();
    if (result.failed) xrt_end_execution(result, XRT_OUTPUT_FLUSH, source_location_id);
    result = xrt_io_write(1, bytes, length);
    if (result.failed) xrt_end_execution(result, XRT_OUTPUT_WRITE, source_location_id);
}

XRAY_API void xrt_print_string(const char *bytes, size_t byte_length, uint32_t source_location_id) {
    XR_CHECK(bytes || !byte_length, "String output requires bytes for a nonempty span");
    XrtIoResult result = xrt_io_flush_stdout();
    if (result.failed) xrt_end_execution(result, XRT_OUTPUT_FLUSH, source_location_id);
    result = xrt_io_write(1, bytes, byte_length);
    if (result.failed) xrt_end_execution(result, XRT_OUTPUT_WRITE, source_location_id);
    result = xrt_io_write(1, "\n", 1);
    if (result.failed) xrt_end_execution(result, XRT_OUTPUT_WRITE, source_location_id);
}

XR_FUNC XR_NORET void xrt_clock_exit(const XrtSourceMap *map, XrtHostClockFailure failure) {
    diagnostic_text("xray: host clock ");
    diagnostic_text(failure.kind == 1 ? "realtime" : failure.kind == 2 ? "process-cpu" : "monotonic");
    diagnostic_text(failure.reason == XRT_CLOCK_OUT_OF_RANGE ? " out of range at " : " read failed at ");
    diagnostic_location(map, failure.source_id);
    diagnostic_text("\n");
    _Exit(1);
}
