/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_runtime.c - Self-contained embedding runtime for AOT-compiled code
 *
 * KEY CONCEPT:
 *   Implements xrt_* functions declared in xray_runtime.h.
 *   All functions are self-contained with no dependency on VM internals.
 *   Pure-compute AOT code doesn't link this file at all.
 */

#include "../../include/xray_runtime.h"
#include "../base/xchecks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../base/xmalloc.h"

/* ========== Value → String Conversion ========== */

static const char *xrt_to_cstr(XrValue v, char *buf, size_t bufsz) {
    switch (v.tag) {
        case XR_TAG_STR:
            return (const char *) v.ptr;
        case XR_TAG_I64:
            snprintf(buf, bufsz, "%" PRId64, v.i);
            return buf;
        case XR_TAG_F64:
            snprintf(buf, bufsz, "%g", v.f);
            return buf;
        case XR_TAG_BOOL:
            return v.i ? "true" : "false";
        case XR_TAG_NULL:
            return "null";
        default:
            snprintf(buf, bufsz, "<object@%p>", v.ptr);
            return buf;
    }
}

static XrValue xrt_str_concat(const char *sa, const char *sb) {
    size_t la = strlen(sa), lb = strlen(sb);
    char *r = (char *) xr_malloc(la + lb + 1);
    if (!r)
        return XR_NULL_VAL;
    memcpy(r, sa, la);
    memcpy(r + la, sb, lb + 1);
    return xrt_box_str(r);
}

/* ========== Mixed-type Arithmetic ========== */

XrValue xrt_add(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return xrt_box_int(a.i + b.i);
    if (a.tag == XR_TAG_STR || b.tag == XR_TAG_STR) {
        char ba[64], bb[64];
        return xrt_str_concat(xrt_to_cstr(a, ba, sizeof(ba)), xrt_to_cstr(b, bb, sizeof(bb)));
    }
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return xrt_box_float(fa + fb);
}

// Self-contained arithmetic (no delegation to xrt_ops.c)
XrValue xrt_sub(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return xrt_box_int(a.i - b.i);
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return xrt_box_float(fa - fb);
}

XrValue xrt_mul(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return xrt_box_int(a.i * b.i);
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return xrt_box_float(fa * fb);
}

XrValue xrt_div(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return b.i ? xrt_box_int(a.i / b.i) : xrt_box_int(0);
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    if (fb == 0.0)
        return xrt_box_float(0.0);
    return xrt_box_float(fa / fb);
}

XrValue xrt_mod(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return b.i ? xrt_box_int(a.i % b.i) : xrt_box_int(0);
    return xrt_box_int(0);
}

XrValue xrt_neg(XrValue a) {
    if (a.tag == XR_TAG_I64)
        return xrt_box_int(-a.i);
    if (a.tag == XR_TAG_F64)
        return xrt_box_float(-a.f);
    return xrt_box_int(0);
}

/* ========== Mixed-type Comparison ========== */

int64_t xrt_lt(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return a.i < b.i;
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return fa < fb;
}

int64_t xrt_le(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return a.i <= b.i;
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return fa <= fb;
}

int64_t xrt_eq(XrValue a, XrValue b) {
    if (a.tag == b.tag) {
        if (a.tag == XR_TAG_I64 || a.tag == XR_TAG_BOOL)
            return a.i == b.i;
        if (a.tag == XR_TAG_F64)
            return a.f == b.f;
        if (a.tag == XR_TAG_NULL)
            return 1;
        return a.ptr == b.ptr;
    }
    if ((a.tag == XR_TAG_I64 || a.tag == XR_TAG_F64) &&
        (b.tag == XR_TAG_I64 || b.tag == XR_TAG_F64)) {
        double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
        double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
        return fa == fb;
    }
    return 0;
}

/* ========== Print ========== */

void xrt_print(XrValue v) {
    switch (v.tag) {
        case XR_TAG_STR:
            printf("%s", (const char *) v.ptr);
            break;
        case XR_TAG_I64:
            printf("%lld", (long long) v.i);
            break;
        case XR_TAG_F64:
            printf("%g", v.f);
            break;
        case XR_TAG_BOOL:
            printf("%s", v.i ? "true" : "false");
            break;
        case XR_TAG_NULL:
            printf("null");
            break;
        default:
            printf("<object@%p>", v.ptr);
            break;
    }
}

void xrt_println(XrValue v) {
    xrt_print(v);
    printf("\n");
}

/* ========== String Operations ========== */

XrValue xrt_string_concat(XrValue a, XrValue b) {
    char ba[64], bb[64];
    return xrt_str_concat(xrt_to_cstr(a, ba, sizeof(ba)), xrt_to_cstr(b, bb, sizeof(bb)));
}

XrValue xrt_string_len(XrValue s) {
    XR_DCHECK(s.tag == XR_TAG_STR, "xrt_string_len: expected string");
    if (s.tag != XR_TAG_STR || !s.ptr)
        return xrt_box_int(0);
    return xrt_box_int((int64_t) strlen((const char *) s.ptr));
}

XrValue xrt_string_slice(XrValue s, int64_t start, int64_t end) {
    XR_DCHECK(s.tag == XR_TAG_STR, "xrt_string_slice: expected string");
    if (s.tag != XR_TAG_STR || !s.ptr)
        return XR_NULL_VAL;
    const char *str = (const char *) s.ptr;
    int64_t len = (int64_t) strlen(str);
    if (start < 0)
        start += len;
    if (end < 0)
        end += len;
    if (start < 0)
        start = 0;
    if (end > len)
        end = len;
    if (start >= end)
        return xrt_box_str("");
    int64_t slen = end - start;
    char *r = (char *) xr_malloc((size_t) slen + 1);
    if (!r)
        return XR_NULL_VAL;
    memcpy(r, str + start, (size_t) slen);
    r[slen] = '\0';
    return xrt_box_str(r);
}

int64_t xrt_string_eq(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_STR && b.tag == XR_TAG_STR) {
        if (a.ptr == b.ptr)
            return 1;
        if (!a.ptr || !b.ptr)
            return 0;
        return strcmp((const char *) a.ptr, (const char *) b.ptr) == 0;
    }
    return 0;
}

/* Stub functions: AOT array/map/field operations not yet implemented.
 * These abort with a clear message rather than silently returning wrong results. */

XrValue xrt_array_new(int64_t cap) {
    (void) cap;
    fprintf(stderr, "xrt_array_new: not implemented\n");
    abort();
}
XrValue xrt_array_get(XrValue a, int64_t i) {
    (void) a;
    (void) i;
    fprintf(stderr, "xrt_array_get: not implemented\n");
    abort();
}
void xrt_array_set(XrValue a, int64_t i, XrValue v) {
    (void) a;
    (void) i;
    (void) v;
    fprintf(stderr, "xrt_array_set: not implemented\n");
    abort();
}
int64_t xrt_array_len(XrValue a) {
    (void) a;
    fprintf(stderr, "xrt_array_len: not implemented\n");
    abort();
}
void xrt_array_push(XrValue a, XrValue v) {
    (void) a;
    (void) v;
    fprintf(stderr, "xrt_array_push: not implemented\n");
    abort();
}

XrValue xrt_map_new(void) {
    fprintf(stderr, "xrt_map_new: not implemented\n");
    abort();
}
XrValue xrt_map_get(XrValue m, XrValue k) {
    (void) m;
    (void) k;
    fprintf(stderr, "xrt_map_get: not implemented\n");
    abort();
}
void xrt_map_set(XrValue m, XrValue k, XrValue v) {
    (void) m;
    (void) k;
    (void) v;
    fprintf(stderr, "xrt_map_set: not implemented\n");
    abort();
}
int64_t xrt_map_len(XrValue m) {
    (void) m;
    fprintf(stderr, "xrt_map_len: not implemented\n");
    abort();
}

XrValue xrt_field_get(XrValue o, const char *n) {
    (void) o;
    (void) n;
    fprintf(stderr, "xrt_field_get: not implemented\n");
    abort();
}
void xrt_field_set(XrValue o, const char *n, XrValue v) {
    (void) o;
    (void) n;
    (void) v;
    fprintf(stderr, "xrt_field_set: not implemented\n");
    abort();
}

void xrt_safepoint(void) {
}
void *xrt_alloc(size_t size) {
    (void) size;
    fprintf(stderr, "xrt_alloc: not implemented\n");
    abort();
}
