/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_string_cursor.h - An exclusive cursor over canonical String storage
 */
#ifndef XRAY_STRING_CURSOR_H
#define XRAY_STRING_CURSOR_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#define XR_STRING_CURSOR_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define XR_STRING_CURSOR_RESTRICT __restrict__
#elif defined(__cplusplus)
#define XR_STRING_CURSOR_RESTRICT
#else
#define XR_STRING_CURSOR_RESTRICT restrict
#endif

/* The producer owns the storage throughout this borrow. Commit before any
 * observation, retirement or operation that can replace that storage. Cursor
 * storage is separate from the canonical owner's metadata and byte body. */
typedef struct XrStringCursor {
    char *bytes;
    size_t length, runes, capacity;
    size_t *owner_length, *owner_runes;
} XrStringCursor;

static inline void xr_string_cursor_commit(XrStringCursor *XR_STRING_CURSOR_RESTRICT cursor) {
    *cursor->owner_length = cursor->length;
    *cursor->owner_runes = cursor->runes;
}
/* The caller has checked overflow and capacity. Source may overlap the body. */
static inline void xr_string_cursor_append(XrStringCursor *XR_STRING_CURSOR_RESTRICT cursor, const char *bytes,
    size_t length, size_t runes) {
    if (length == 1) cursor->bytes[cursor->length] = *bytes;
    else memmove(cursor->bytes + cursor->length, bytes, length);
    cursor->length += length;
    cursor->runes += runes;
    cursor->bytes[cursor->length] = 0;
}
#endif // XRAY_STRING_CURSOR_H
