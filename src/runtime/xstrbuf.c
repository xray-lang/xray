/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstrbuf.c - String buffer implementation
 *
 * KEY CONCEPT:
 *   Growable string buffer for efficient string concatenation.
 *   Per-execution-owner temp buffer avoids repeated allocations.
 */

#include "xstrbuf.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "object/xstring.h"
#include "../base/xmalloc.h"
#include "../shared/xr_float_fmt.h"
#include "../shared/xr_numeric_core.h"
#include <string.h>
#include <stdlib.h>

/* ========== Internal Helper Functions ========== */

// Calculate new capacity (doubling strategy)
static size_t calc_new_capacity(size_t old_cap, size_t need) {
    size_t new_cap = old_cap ? old_cap : XR_STRBUF_MIN_CAP;

    while (new_cap < need) {
        new_cap *= 2;
        if (new_cap > XR_STRBUF_MAX_CAP) {
            new_cap = XR_STRBUF_MAX_CAP;
            break;
        }
    }

    return new_cap;
}

// Expand buffer capacity
static void strbuf_grow(XrStrBuf *sb, size_t need) {
    size_t new_cap = calc_new_capacity(sb->capacity, need);

    if (new_cap > XR_STRBUF_MAX_CAP) {
        // Exceeds max capacity, error
        xr_log_warning("strbuf", "string too long, exceeds max capacity %zu",
                       (size_t) XR_STRBUF_MAX_CAP);
        return;
    }

    char *new_data = (char *) xr_realloc(sb->data, new_cap);
    if (!new_data) {
        xr_log_warning("strbuf", "memory allocation failed");
        return;
    }

    sb->data = new_data;
    sb->capacity = new_cap;
}

/* ========== Create and Destroy ========== */

XrStrBuf *xr_strbuf_new(XrVMRuntime *X, size_t init_cap) {
    XR_DCHECK(X != NULL, "strbuf_new: NULL isolate");
    if (init_cap < XR_STRBUF_MIN_CAP) {
        init_cap = XR_STRBUF_MIN_CAP;
    }

    XrStrBuf *sb = (XrStrBuf *) xr_malloc(sizeof(XrStrBuf));
    if (!sb)
        return NULL;

    sb->data = (char *) xr_malloc(init_cap);
    if (!sb->data) {
        xr_free(sb);
        return NULL;
    }

    sb->length = 0;
    sb->capacity = init_cap;
    sb->X = X;

    return sb;
}

void xr_strbuf_free(XrStrBuf *sb) {
    if (!sb)
        return;

    if (sb->data) {
        xr_free(sb->data);
    }
    xr_free(sb);
}

/* ========== Capacity Management ========== */

void xr_strbuf_ensure(XrStrBuf *sb, size_t need) {
    XR_DCHECK(sb != NULL, "strbuf_ensure: NULL strbuf");
    size_t required = sb->length + need;

    if (required > sb->capacity) {
        strbuf_grow(sb, required);
    }
}

void xr_strbuf_reserve(XrStrBuf *sb, size_t cap) {
    XR_DCHECK(sb != NULL, "strbuf_reserve: NULL strbuf");
    if (cap > sb->capacity) {
        strbuf_grow(sb, cap);
    }
}

/* ========== Append Operations ========== */

void xr_strbuf_append_str(XrStrBuf *sb, XrString *s) {
    if (!s || s->length == 0)
        return;

    xr_strbuf_ensure(sb, s->length);
    memcpy(sb->data + sb->length, s->data, s->length);
    sb->length += s->length;
}

void xr_strbuf_append_cstr(XrStrBuf *sb, const char *s, size_t len) {
    if (!s || len == 0)
        return;

    xr_strbuf_ensure(sb, len);
    memcpy(sb->data + sb->length, s, len);
    sb->length += len;
}

void xr_strbuf_append_char(XrStrBuf *sb, char c) {
    xr_strbuf_ensure(sb, 1);
    sb->data[sb->length++] = c;
}

void xr_strbuf_append_int(XrStrBuf *sb, int64_t val) {
    char buf[24];
    int len = xr_numeric_core_format_i64(buf, sizeof(buf), val);
    if (len > 0)
        xr_strbuf_append_cstr(sb, buf, (size_t) len);
}

void xr_strbuf_append_float(XrStrBuf *sb, double val) {
    char buf[64];
    int len = xr_format_float(buf, sizeof(buf), val);
    if (len > 0)
        xr_strbuf_append_cstr(sb, buf, (size_t) len);
}

/* ========== Reset ========== */

void xr_strbuf_reset(XrStrBuf *sb) {
    sb->length = 0;
}
