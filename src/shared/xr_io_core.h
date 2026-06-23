/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_io_core.h - Runtime-neutral IO stdlib core helpers.
 */

#ifndef XRAY_SHARED_XR_IO_CORE_H
#define XRAY_SHARED_XR_IO_CORE_H

#include <stdbool.h>
#include <stddef.h>

typedef bool (*XrIoCoreLineFn)(void *ctx, const char *data, size_t len);

static inline size_t xr_io_core_trim_line_end(const char *data, size_t start, size_t end) {
    while (end > start && data[end - 1] == '\r')
        end--;
    return end;
}

static inline bool xr_io_core_read_lines_each(const char *data, size_t len, XrIoCoreLineFn on_line,
                                              void *ctx) {
    if ((!data && len != 0) || !on_line)
        return false;

    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n')
            continue;

        size_t end = xr_io_core_trim_line_end(data, start, i);
        if (!on_line(ctx, data + start, end - start))
            return false;
        start = i + 1;
    }

    if (start < len) {
        size_t end = xr_io_core_trim_line_end(data, start, len);
        if (!on_line(ctx, data + start, end - start))
            return false;
    }

    return true;
}

#endif /* XRAY_SHARED_XR_IO_CORE_H */
