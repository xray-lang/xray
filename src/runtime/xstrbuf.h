/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstrbuf.h - String buffer for efficient concatenation
 *
 * KEY CONCEPT:
 *   Growable string buffer that avoids O(n²) complexity of repeated
 *   string concatenation. Used for loop string accumulation and
 *   multi-part string building.
 */

#ifndef XSTRBUF_H
#define XSTRBUF_H

#include <stddef.h>
#include <stdint.h>

#include "../base/xdefs.h"
#include "../base/xforward_decl.h"

/* ========== String Buffer ========== */

// Growable string buffer with doubling capacity strategy
typedef struct XrStrBuf {
    char *data;
    size_t length;
    size_t capacity;
    XrayIsolate *X;
} XrStrBuf;

#define XR_STRBUF_MIN_CAP    64
#define XR_STRBUF_MAX_CAP    (1 << 30)  // 1GB

/* ========== Creation and Destruction ========== */

// Get thread-local temporary buffer (auto-reset after to_string)
XR_FUNC XrStrBuf *xr_strbuf_tmp(XrayIsolate *X);

XR_FUNC XrStrBuf *xr_strbuf_new(XrayIsolate *X, size_t init_cap);
XR_FUNC void xr_strbuf_free(XrStrBuf *sb);

/* ========== Capacity Management ========== */

XR_FUNC void xr_strbuf_ensure(XrStrBuf *sb, size_t need);
XR_FUNC void xr_strbuf_reserve(XrStrBuf *sb, size_t cap);

/* ========== Append Operations ========== */

XR_FUNC void xr_strbuf_append_str(XrStrBuf *sb, XrString *s);
XR_FUNC void xr_strbuf_append_cstr(XrStrBuf *sb, const char *s, size_t len);
XR_FUNC void xr_strbuf_append_char(XrStrBuf *sb, char c);
XR_FUNC void xr_strbuf_append_int(XrStrBuf *sb, int64_t val);
XR_FUNC void xr_strbuf_append_float(XrStrBuf *sb, double val);

/* ========== Conversion and Reset ========== */

// Convert to XrString and reset buffer (reusable after this)
XR_FUNC XrString *xr_strbuf_to_string(XrStrBuf *sb);

XR_FUNC void xr_strbuf_reset(XrStrBuf *sb);

#endif // XSTRBUF_H
