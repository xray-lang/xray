/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_format.h - Borrowed typed values for the shared display algorithm
 *
 * KEY CONCEPT:
 *   The reader never retains a value or invents a second runtime layout.
 */
#ifndef XXIR_FORMAT_H
#define XXIR_FORMAT_H
#include "xxir_value.h"
#include "xxir_declarations.h"
#include "../shared/xr_value_format_core.h"

/* The root owner remains alive and immutable throughout a synchronous walk.
 * Child nodes borrow context slots only until the next child selection at the
 * same or an outer depth. The shared walker consumes each child immediately;
 * immutable name and byte spans continue to borrow their actual value owner. */
typedef struct XrXirFormatCursor {
    const void *storage;
    const XrXirTypeArena *arena;
    XrXirType type;
    bool compact;
} XrXirFormatCursor;
typedef struct XrXirFormatAggregate {
    XrXirLiteral name, member;
    uint64_t count;
    bool array;
} XrXirFormatAggregate;
typedef struct XrXirFormatContext {
    XrXirFormatCursor cursors[XR_VALUE_FORMAT_MAX_DEPTH + 1];
} XrXirFormatContext;
/* The shared walker starts at depth zero. Context belongs to this one
 * synchronous traversal. A nested display uses a
 * distinct context; source values and their immutable storage remain borrowed.
 * Compact cursors describe actual owned storage, never executable values. */
XR_FUNC XrValueFormatReader xr_xir_value_format_reader(XrXirFormatContext *context);
XR_FUNC bool xr_xir_value_format_cursor(const XrXirValue *, XrXirFormatCursor *);
XR_FUNC bool xr_xir_value_format_leaf(const XrXirFormatCursor *, XrXirValue *borrowed);
XR_FUNC bool xr_xir_value_format_aggregate(const XrXirFormatCursor *, XrXirFormatAggregate *);
XR_FUNC bool xr_xir_value_format_child(const XrXirFormatCursor *, uint32_t index, XrXirFormatCursor *);
#endif // XXIR_FORMAT_H
