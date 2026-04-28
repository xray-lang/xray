/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ctxbuf.h - Dynamic byte-buffer shared by stdlib helpers
 *
 * KEY CONCEPT:
 *   A minimal growing-byte-buffer primitive used by the stdlib module
 *   surface (url, path, datetime, io, log, and the serializer layer in
 *   common_writer.h). Prior to this header each of those modules carried
 *   its own fixed-size char[4096] scratch buffer, which silently
 *   truncated overlong inputs (OAuth URLs, deeply-nested YAML, large log
 *   attribute sets). Centralising the buffer primitive gives every
 *   consumer consistent growth semantics *and* one shared OOM policy.
 *
 * WHY INLINE / HEADER-ONLY:
 *   The hot path (append a single byte or short cstr) must be inlined
 *   into the caller so that the serializer inner loops stay branch-free.
 *   All functions are `static inline`; the backing allocator comes from
 *   `xr_malloc` / `XR_REALLOC_OR_ABORT` (see docs/rules/c-coding-standards)
 *   which means OOM aborts with file/line instead of silently truncating.
 *
 * LIFECYCLE:
 *     XrCtxBuf b;
 *     xr_ctxbuf_init(&b, 128);   // starting capacity hint
 *     xr_ctxbuf_append_cstr(&b, "hello");
 *     xr_ctxbuf_putc(&b, '!');
 *     // hand buffer over to callers OR free it:
 *     char *owned = xr_ctxbuf_steal(&b);   // caller now owns .data
 *     xr_ctxbuf_free(&b);                  // if steal was not called
 *
 * DESIGN NOTES:
 *   - `.data` is always NUL-terminated (len byte set to '\0'), so it can
 *     be passed straight to `xr_string_intern` / `fprintf("%s", ...)`.
 *   - Growth doubles capacity; `xr_ctxbuf_reserve` ensures room for
 *     `extra` additional bytes plus the terminator.
 *   - The struct fields (`data`, `len`, `cap`) are deliberately public:
 *     `common_writer.h` directly memset()s padding into the tail to
 *     implement serializer indentation without an extra helper call.
 */

#ifndef XR_STDLIB_CTXBUF_H
#define XR_STDLIB_CTXBUF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/base/xmalloc.h"

typedef struct XrCtxBuf {
    char *data;
    size_t len;
    size_t cap;
} XrCtxBuf;

/* ========== Lifecycle ========== */

// Initialise the buffer with an initial capacity hint. A hint of 0 is
// treated as 64 so the first append does not immediately realloc.
static inline void xr_ctxbuf_init(XrCtxBuf *b, size_t hint) {
    if (hint < 64)
        hint = 64;
    b->data = (char *) xr_malloc(hint);
    if (!b->data) {
        fprintf(stderr, "[FATAL] %s:%d: xr_ctxbuf_init OOM (%zu bytes)\n", __FILE__, __LINE__,
                hint);
        abort();
    }
    b->data[0] = '\0';
    b->len = 0;
    b->cap = hint;
}

// Release the backing storage. Safe to call on a zeroed / already-freed
// buffer; fields are reset so re-use would require a fresh init.
static inline void xr_ctxbuf_free(XrCtxBuf *b) {
    if (!b)
        return;
    if (b->data) {
        xr_free(b->data);
        b->data = NULL;
    }
    b->len = 0;
    b->cap = 0;
}

// Transfer ownership of the backing buffer to the caller. After steal()
// the CtxBuf is left in the zero state, i.e. calling xr_ctxbuf_free()
// on it becomes a no-op. The returned pointer must be released with
// xr_free() by the caller.
static inline char *xr_ctxbuf_steal(XrCtxBuf *b) {
    char *p = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return p;
}

/* ========== Growth ========== */

// Ensure there is room to append `extra` more payload bytes plus the
// trailing NUL. Doubles capacity until the request fits; aborts with a
// diagnostic message on allocator failure (see XR_REALLOC_OR_ABORT
// rationale in src/base/xmalloc.h).
static inline void xr_ctxbuf_reserve(XrCtxBuf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len - 1) {
        fprintf(stderr, "[FATAL] %s:%d: xr_ctxbuf_reserve overflow (len=%zu extra=%zu)\n",
                __FILE__, __LINE__, b->len, extra);
        abort();
    }
    size_t need = b->len + extra + 1;  // +1 for the NUL terminator
    if (need <= b->cap)
        return;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) {
            ncap = need;  // clamp to exact need to avoid overflow
            break;
        }
        ncap *= 2;
    }
    XR_REALLOC_OR_ABORT(b->data, ncap, "xr_ctxbuf_reserve");
    b->cap = ncap;
}

/* ========== Append primitives ========== */

// Append `n` bytes from `s` verbatim. Preserves embedded NULs in `s`.
static inline void xr_ctxbuf_append(XrCtxBuf *b, const char *s, size_t n) {
    if (n == 0 || !s)
        return;
    xr_ctxbuf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

// Append a NUL-terminated C string. NULL is treated as empty.
static inline void xr_ctxbuf_append_cstr(XrCtxBuf *b, const char *s) {
    if (!s)
        return;
    xr_ctxbuf_append(b, s, strlen(s));
}

// Append a single byte. Keeps trailing NUL.
static inline void xr_ctxbuf_putc(XrCtxBuf *b, char c) {
    xr_ctxbuf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

// printf-style append. Runs a vsnprintf probe to size the growth before
// the formatted write so long formatted strings never truncate.
static inline void xr_ctxbuf_appendf(XrCtxBuf *b, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static inline void xr_ctxbuf_appendf(XrCtxBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list probe;
    va_copy(probe, ap);
    int needed = vsnprintf(NULL, 0, fmt, probe);
    va_end(probe);
    if (needed <= 0) {
        va_end(ap);
        return;
    }
    xr_ctxbuf_reserve(b, (size_t) needed);
    int written = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (written > 0)
        b->len += (size_t) written;
}

#endif  // XR_STDLIB_CTXBUF_H
