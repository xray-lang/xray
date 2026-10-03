/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_cursor.h - In-place, allocation-free closed-schema JSON primitives
 *
 * KEY CONCEPT:
 *   Every byte read and write uses the caller's explicit work policy.
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
    XR_JSON_CURSOR_OUT_OF_MEMORY, XR_JSON_CURSOR_UNSUPPORTED
} XrJsonCursorStatus;
typedef XrJsonCursorStatus (*XrJsonCursorWork)(void *context, uint64_t units);
typedef struct XrJsonCursor {
    char *begin, *cursor, *end;
    void *context;
    XrJsonCursorWork work;
    XrJsonCursorStatus status;
} XrJsonCursor;

/* NULL/zero input is useful for metered comparisons and identity framing.
 * Parsing requires a nonempty writable span. Failure is sticky; low-level
 * cursor/input progress is not transactional. The enclosing owner publishes
 * only after its entire schema has succeeded. */
XR_FUNC XrJsonCursor xr_json_cursor_make(char *bytes, size_t length,
    void *context, XrJsonCursorWork work);
XR_FUNC bool xr_json_cursor_fail(XrJsonCursor *, XrJsonCursorStatus);
XR_FUNC bool xr_json_cursor_work(XrJsonCursor *, uint64_t);
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
