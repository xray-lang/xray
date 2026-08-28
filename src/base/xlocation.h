/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlocation.h - Source location descriptor (file/line/column ranges)
 *
 * KEY CONCEPT:
 *   Universal source location used by analyzer, runtime class metadata,
 *   diagnostics and debug info. Lives at base layer so any layer may
 *   embed or reference it without creating upward dependencies.
 */

#ifndef XLOCATION_H
#define XLOCATION_H

#include <stdbool.h>
#include <stdint.h>

typedef struct XrLocation {
    const char *file;     // File path
    uint32_t line;        // 1-indexed line number
    uint32_t column;      // 1-indexed column number
    uint32_t end_line;    // End line (for ranges)
    uint32_t end_column;  // End column
} XrLocation;

/* A location is complete when it names a file and spans a real, non-inverted
 * range. Plans that must be attributable to exact source reject anything less,
 * so the predicate lives with the type rather than in each consumer. */
static inline bool xr_location_is_complete(XrLocation source) {
    return source.file && source.file[0] && source.line != 0 && source.column != 0 &&
           source.end_line != 0 && source.end_column != 0 && source.end_line >= source.line &&
           (source.end_line != source.line || source.end_column >= source.column);
}

#endif  // XLOCATION_H
