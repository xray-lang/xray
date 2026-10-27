/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_cursor.h - In-place, allocation-free closed-schema JSON primitives
 *
 * KEY CONCEPT:
 *   Explicit spans and grammar limits bound reads and writes.
 *   Decoded strings borrow the mutable input; no DOM or owner is created.
 */
#ifndef XJSON_CURSOR_H
#define XJSON_CURSOR_H
#include "xdefs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum XrJsonCursorStatus {
    XR_JSON_CURSOR_OK, XR_JSON_CURSOR_INVALID, XR_JSON_CURSOR_BUDGET,
    XR_JSON_CURSOR_OUT_OF_MEMORY, XR_JSON_CURSOR_UNSUPPORTED,
    XR_JSON_CURSOR_MEMORY_LIMIT, XR_JSON_CURSOR_OVERFLOW
} XrJsonCursorStatus;
typedef struct XrJsonCursor {
    char *begin, *cursor, *end;
    XrJsonCursorStatus status;
} XrJsonCursor;

/* NULL/zero input permits comparisons and identity framing.
 * Parsing requires a nonempty writable span. Failure is sticky; low-level
 * cursor/input progress is not transactional. The enclosing owner publishes
 * only after its entire schema has succeeded. Text comparison/length inputs
 * must be readable NUL-terminated strings; decoded strings remain in the span. */
XR_FUNC XrJsonCursor xr_json_cursor_make(char *bytes, size_t length);
XR_FUNC bool xr_json_cursor_fail(XrJsonCursor *, XrJsonCursorStatus);
XR_FUNC bool xr_json_cursor_length(XrJsonCursor *, const char *, size_t *);
XR_FUNC bool xr_json_cursor_peek(XrJsonCursor *, uint8_t *);
XR_FUNC bool xr_json_cursor_byte(XrJsonCursor *, uint8_t *);
XR_FUNC bool xr_json_cursor_space(XrJsonCursor *);
XR_FUNC bool xr_json_cursor_take(XrJsonCursor *, char);
XR_FUNC bool xr_json_cursor_equal(XrJsonCursor *, const char *, const char *);
XR_FUNC bool xr_json_cursor_string(XrJsonCursor *, size_t limit, const char **);
XR_FUNC bool xr_json_cursor_u64(XrJsonCursor *, uint64_t *);
XR_FUNC bool xr_json_cursor_key(XrJsonCursor *, const char *const *, uint32_t,
    uint64_t *seen, uint32_t *ordinal);
XR_FUNC bool xr_json_cursor_member_end(XrJsonCursor *, bool *more);
#endif // XJSON_CURSOR_H
