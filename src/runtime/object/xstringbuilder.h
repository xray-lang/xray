/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstringbuilder.h - StringBuilder object definition
 *
 * KEY CONCEPT:
 *   Efficient string concatenation using internal XrStrBuf
 */

#ifndef XSTRINGBUILDER_H
#define XSTRINGBUILDER_H

#include "../gc/xgc_header.h"
#include "../value/xvalue.h"
#include "../xstrbuf.h"

// Forward declarations via xforward_decl.h
struct XrCoroutine;

// XrStringBuilder - mutable string builder
typedef struct XrStringBuilder {
    XrGCHeader gc;          // GC header (must be first)
    XrStrBuf *buffer;       // String buffer
} XrStringBuilder;

/* ========== Creation and Destruction ========== */

// Create StringBuilder object
XR_FUNC XrStringBuilder *xr_stringbuilder_new(struct XrCoroutine *coro);

// Initialize StringBuilder on pre-allocated memory (for system heap)
XR_FUNC void xr_stringbuilder_init_inplace(XrStringBuilder *sb);

// Free StringBuilder object
XR_FUNC void xr_stringbuilder_free(XrStringBuilder *sb);

/* ========== Operations ========== */

// Append XrString
XR_FUNC void xr_stringbuilder_append_str(XrStringBuilder *sb, struct XrString *s);

// Append C string
XR_FUNC void xr_stringbuilder_append_cstr(XrStringBuilder *sb, const char *s, size_t len);

// Append integer
XR_FUNC void xr_stringbuilder_append_int(XrStringBuilder *sb, int64_t val);

// Append float
XR_FUNC void xr_stringbuilder_append_float(XrStringBuilder *sb, double val);

// Convert to XrString (does not reset buffer)
XR_FUNC struct XrString *xr_stringbuilder_to_string(XrStringBuilder *sb);

// Get current length
XR_FUNC size_t xr_stringbuilder_length(XrStringBuilder *sb);

// Clear contents
XR_FUNC void xr_stringbuilder_clear(XrStringBuilder *sb);

/* ========== XrValue Conversion ========== */

// Create StringBuilder XrValue
XR_FUNC XrValue xr_stringbuilder_value(XrStringBuilder *sb);

// Check if value is StringBuilder
XR_FUNC bool xr_is_stringbuilder(XrValue v);

// Convert to StringBuilder pointer
XR_FUNC XrStringBuilder *xr_to_stringbuilder(XrValue v);

#endif // XSTRINGBUILDER_H
