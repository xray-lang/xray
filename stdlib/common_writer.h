/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * common_writer.h - Shared serialization writer for stdlib modules
 *
 * KEY CONCEPT:
 *   Serializers in json / yaml / toml / xml / csv used to each define their
 *   own XxWriter struct with hand-rolled malloc/realloc buffer growth,
 *   newline/indent helpers and append primitives. That amounted to ~500
 *   lines of duplicated and subtly-drifting boilerplate, plus several
 *   silent OOM paths that could segfault on tight memory.
 *
 *   This header consolidates the common parts behind XrSerWriter, which
 *   is a thin wrapper over the shared XrCtxBuf byte buffer. Every
 *   allocation goes through xr_malloc / XR_REALLOC_OR_ABORT so that:
 *     - OOM is observable (abort with file/line) rather than silent UB.
 *     - Leak tracking in debug builds covers every serializer.
 *     - Domain-specific writers (e.g. indentation level, delimiter char)
 *       can still layer their own state on top by embedding an
 *       XrSerWriter field.
 *
 * USAGE:
 *     typedef struct {
 *         XrSerWriter w;
 *         XrayIsolate *isolate;
 *         int          indent;
 *         int          depth;
 *     } JsonWriter;
 *
 *     JsonWriter jw;
 *     xr_serw_init(&jw.w, 1024);
 *     xr_serw_str(&jw.w, "{\"k\":1}");
 *     XrString *s = xr_string_intern(iso, jw.w.data, jw.w.len, 0);
 *     xr_serw_free(&jw.w);
 *
 * CONVENTIONS:
 *   - xr_serw_newline(&w, indent, level) writes '\n' followed by
 *     level * indent spaces, only when indent > 0 (pretty-print mode).
 *     For compact mode (indent == 0) it is a no-op and callers should
 *     simply skip it.
 *   - xr_serw_printf allows printf-style append without bringing in a
 *     separate temporary buffer; it delegates to xr_ctxbuf_appendf.
 *   - All helpers are inline and should compile to direct xr_ctxbuf_*
 *     calls with no overhead in release builds.
 */

#ifndef XR_STDLIB_COMMON_WRITER_H
#define XR_STDLIB_COMMON_WRITER_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ctxbuf.h"

/*
 * XrSerWriter is intentionally a typedef over XrCtxBuf so that callers
 * can access .data / .len / .cap directly when handing the buffer to
 * xr_string_intern / xr_string_new. The type alias mainly exists for
 * documentation: the byte buffer is used as a serializer output, not a
 * generic formatting scratch pad.
 */
typedef XrCtxBuf XrSerWriter;

/* ========== Lifecycle ========== */

static inline void xr_serw_init(XrSerWriter *w, size_t hint) {
    xr_ctxbuf_init(w, hint);
}

static inline void xr_serw_free(XrSerWriter *w) {
    xr_ctxbuf_free(w);
}

static inline char* xr_serw_steal(XrSerWriter *w) {
    return xr_ctxbuf_steal(w);
}

/* ========== Growth ========== */

static inline void xr_serw_reserve(XrSerWriter *w, size_t extra) {
    xr_ctxbuf_reserve(w, extra);
}

/* ========== Append primitives ========== */

// Append exactly `n` bytes from `s`. NUL bytes are preserved.
static inline void xr_serw_append(XrSerWriter *w, const char *s, size_t n) {
    xr_ctxbuf_append(w, s, n);
}

// Append a NUL-terminated C string. NULL is treated as empty.
static inline void xr_serw_str(XrSerWriter *w, const char *s) {
    if (!s) return;
    xr_ctxbuf_append_cstr(w, s);
}

// Append a single byte.
static inline void xr_serw_char(XrSerWriter *w, char c) {
    xr_ctxbuf_putc(w, c);
}

// printf-style append. Uses xr_ctxbuf_appendf under the hood which runs
// a single vsnprintf probe then a formatted write, so no caller-supplied
// scratch buffer is required.
static inline void xr_serw_printf(XrSerWriter *w, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
;

static inline void xr_serw_printf(XrSerWriter *w, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (needed <= 0) { va_end(ap); return; }
    xr_ctxbuf_reserve(w, (size_t)needed);
    int written = vsnprintf(w->data + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (written > 0) w->len += (size_t)written;
    w->data[w->len] = '\0';
}

/* ========== Indentation / newline ========== */

// Write `level * indent_width` spaces. No-op if either is zero/negative.
static inline void xr_serw_indent(XrSerWriter *w, int level, int indent_width) {
    if (indent_width <= 0 || level <= 0) return;
    size_t n = (size_t)level * (size_t)indent_width;
    xr_ctxbuf_reserve(w, n);
    memset(w->data + w->len, ' ', n);
    w->len += n;
    w->data[w->len] = '\0';
}

// Write a newline followed by `level * indent_width` spaces, but only
// when indent_width > 0 (pretty-print). For indent_width == 0 this is
// a no-op so callers can use the same writer for compact output.
static inline void xr_serw_newline(XrSerWriter *w, int level, int indent_width) {
    if (indent_width <= 0) return;
    size_t pad = (size_t)level * (size_t)indent_width;
    size_t need = 1 + pad;
    xr_ctxbuf_reserve(w, need);
    w->data[w->len++] = '\n';
    if (pad) {
        memset(w->data + w->len, ' ', pad);
        w->len += pad;
    }
    w->data[w->len] = '\0';
}

#endif // XR_STDLIB_COMMON_WRITER_H
