/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_output.c - Standalone integer output and nonreturning failure
 *
 * KEY CONCEPT:
 *   The success path needs no mutable execution binding. Only failure reads
 *   the strong image-owned source map, then exits without cleanup callbacks.
 */
#include "os/os_native_output.h"
#include "xray_native_runtime.h"
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

static void diagnostic_location(uint32_t id) {
    const XrtStandaloneSourceMap *map = &xrt_standalone_source_map;
    if ((map->count == 0) != (map->locations == NULL) || map->count > UINT32_MAX) {
        diagnostic_text("invalid source map");
        return;
    }
    if (!id) { diagnostic_text("unknown source"); return; }
    if ((size_t)id > map->count) { diagnostic_text("invalid source id "); diagnostic_number(id); return; }
    const XrtSourceLocation *location = &map->locations[id - 1];
    if (!location->path || !location->path_length || !location->line || !location->column ||
        !location->end_line || !location->end_column || location->end_line < location->line ||
        (location->end_line == location->line && location->end_column < location->column)) {
        diagnostic_text("invalid source location");
        return;
    }
    diagnostic_path(location->path, location->path_length);
    diagnostic_text(":"); diagnostic_number(location->line);
    diagnostic_text(":"); diagnostic_number(location->column);
    diagnostic_text("-"); diagnostic_number(location->end_line);
    diagnostic_text(":"); diagnostic_number(location->end_column);
}

static XR_NORET void xrt_end_execution(XrtIoResult failure, const char *operation, uint32_t id) {
    if (!failure.broken_pipe) {
        diagnostic_text("xray: output "); diagnostic_text(operation); diagnostic_text(" failed at ");
        diagnostic_location(id);
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
    if (result.failed) xrt_end_execution(result, "flush", source_location_id);
    result = xrt_io_write(1, bytes, length);
    if (result.failed) xrt_end_execution(result, "write", source_location_id);
}
