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

#include <stdint.h>

typedef struct XrLocation {
    const char *file;     // File path
    uint32_t line;        // 1-indexed line number
    uint32_t column;      // 1-indexed column number
    uint32_t end_line;    // End line (for ranges)
    uint32_t end_column;  // End column
} XrLocation;

#endif  // XLOCATION_H
