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

/* The producer owns the storage throughout this borrow. Commit before any
 * observation, retirement or operation that can replace that storage. */
typedef struct XrStringCursor {
    char *bytes;
    size_t length, runes, capacity;
    size_t *owner_length, *owner_runes;
} XrStringCursor;

static inline void xr_string_cursor_commit(XrStringCursor *cursor) {
    *cursor->owner_length = cursor->length;
    *cursor->owner_runes = cursor->runes;
}
/* The caller has checked overflow and capacity. Source may overlap the body. */
static inline void xr_string_cursor_append(XrStringCursor *cursor, const char *bytes,
    size_t length, size_t runes) {
    if (length == 1) cursor->bytes[cursor->length] = *bytes;
    else memmove(cursor->bytes + cursor->length, bytes, length);
    cursor->length += length;
    cursor->runes += runes;
    cursor->bytes[cursor->length] = 0;
}
#endif // XRAY_STRING_CURSOR_H
