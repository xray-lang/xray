/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_string.h - Synchronous mutation of an actual String binding
 */
#ifndef XRAY_NATIVE_STRING_H
#define XRAY_NATIVE_STRING_H
#include "xray_native_state.h"
#include "xray_string_cursor.h"

/* A borrow keeps the caller's local cell or actual module slot. Its lifetime
 * ends with the synchronous call and it never owns the referenced String. */
typedef struct XrtStringRef {
    XrtString **cell;
    XrtModuleAccess state;
} XrtStringRef;
/* Compiler-local borrow. Generated code closes it before every observation or
 * runtime interaction, and commits before selected-edge owner transfers. */
typedef struct XrtStringAppendCursor {
    XrtString *string;
    const XrtString *suffix;
    XrStringCursor cursor;
} XrtStringAppendCursor;
#ifdef __cplusplus
extern "C" {
#endif
XRAY_API XrtStringRef xrt_string_ref_local(XrtString **, uint32_t source);
XRAY_API XrtStringRef xrt_string_ref_state(XrtModuleAccess, uint32_t source);
XRAY_API XrtString *xrt_string_ref_read(XrtStringRef, uint32_t source);
XRAY_API void xrt_string_ref_write(XrtStringRef, const XrtString *, uint32_t source);
XRAY_API void xrt_string_ref_append(XrtStringRef, const XrtString *, uint32_t source);
/* The existing owner is consumed only after successful preparation. Shared
 * guest aliases retain their old value; failed preparation keeps the binding. */
XRAY_API void xrt_string_cell_append(XrtString **, const XrtString *, uint32_t source);
XRAY_API XrtString *xrt_string_append_owned(XrtString *, const XrtString *, uint32_t source);
XRAY_API XrtString *xrt_string_append_cursor_refill(XrtStringAppendCursor *, XrtString *,
    const XrtString *, uint32_t source);
#ifdef __cplusplus
}
#endif
static inline void xrt_string_append_cursor_flush(XrtStringAppendCursor *XR_STRING_CURSOR_RESTRICT cursor) {
    if (cursor->string) xr_string_cursor_commit(&cursor->cursor);
}
static inline void xrt_string_append_cursor_close(XrtStringAppendCursor *XR_STRING_CURSOR_RESTRICT cursor) {
    xrt_string_append_cursor_flush(cursor);
    cursor->string = NULL;
}
/* The compiler supplies the same literal table bytes and canonical UTF8 counts
 * that produced suffix. Its pinned identity remains checked on every borrow. */
static inline XrtString *xrt_string_append_cursor(XrtStringAppendCursor *cursor,
    XrtString *current, const XrtString *suffix, const char *bytes, size_t length,
    size_t runes, uint32_t source) {
    if (cursor->string && cursor->string == current && cursor->suffix == suffix &&
        length < SIZE_MAX - cursor->cursor.length &&
        cursor->cursor.length + length + 1 <= cursor->cursor.capacity) {
        xr_string_cursor_append(&cursor->cursor, bytes, length, runes);
        return current;
    }
    return xrt_string_append_cursor_refill(cursor, current, suffix, source);
}
#endif // XRAY_NATIVE_STRING_H
