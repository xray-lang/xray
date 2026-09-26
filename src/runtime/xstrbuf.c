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
#include "../shared/xr_strbuf_core.h"
#include "../shared/xr_buffer_capacity_core.h"
#include "object/xstring.h"
#include "../base/xmalloc.h"
#include "../shared/xr_float_fmt.h"
#include "../shared/xr_numeric_core.h"
#include <string.h>
#include <stdlib.h>

/* ========== Internal Helper Functions ========== */

static bool strbuf_resize(XrStrBuf *sb, size_t capacity) {
    if (capacity <= sb->capacity)
        return true;
    char *data = (char *) xr_realloc(sb->data, capacity);
    if (!data)
        return false;
    sb->data = data;
    sb->capacity = capacity;
    return true;
}

/* ========== Create and Destroy ========== */

XrStrBuf *xr_strbuf_new(XrVMRuntime *X, size_t init_cap) {
    XR_DCHECK(X != NULL, "strbuf_new: NULL isolate");
    size_t capacity = 0u;
    if (!xr_buffer_capacity_plan(0u, 0u, init_cap ? init_cap : XR_STRBUF_MIN_CAP,
                                       XR_STRBUF_MIN_CAP, XR_STRBUF_MAX_CAP, &capacity))
        return NULL;
    init_cap = capacity;

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

bool xr_strbuf_ensure(XrStrBuf *sb, size_t need) {
    XR_DCHECK(sb != NULL, "strbuf_ensure: NULL strbuf");
    size_t capacity = 0u;
    return sb && (sb->capacity == 0u || sb->data) &&
           xr_buffer_capacity_plan(sb->length, sb->capacity, need, XR_STRBUF_MIN_CAP,
                                         XR_STRBUF_MAX_CAP, &capacity) &&
           strbuf_resize(sb, capacity);
}

bool xr_strbuf_reserve(XrStrBuf *sb, size_t cap) {
    XR_DCHECK(sb != NULL, "strbuf_reserve: NULL strbuf");
    size_t capacity = 0u;
    return sb && (sb->capacity == 0u || sb->data) &&
           xr_buffer_capacity_plan(0u, sb->capacity, cap, XR_STRBUF_MIN_CAP,
                                         XR_STRBUF_MAX_CAP, &capacity) &&
           strbuf_resize(sb, capacity);
}

/* ========== Append Operations ========== */

bool xr_strbuf_append_cstr(XrStrBuf *sb, const char *s, size_t len) {
    if (!sb || (!s && len))
        return false;
    if (!len)
        return true;
    uintptr_t address = (uintptr_t) s, base = (uintptr_t) sb->data;
    bool aliases = sb->data && address >= base && address - base < sb->length;
    size_t offset = aliases ? (size_t) (address - base) : 0u;
    if (aliases && len > sb->length - offset)
        return false;
    if (!xr_strbuf_ensure(sb, len))
        return false;
    if (aliases)
        s = sb->data + offset;
    memmove(sb->data + sb->length, s, len);
    sb->length += len;
    return true;
}

bool xr_strbuf_append_str(XrStrBuf *sb, XrString *s) {
    return s && xr_strbuf_append_cstr(sb, s->data, s->length);
}

bool xr_strbuf_append_char(XrStrBuf *sb, char c) {
    if (!xr_strbuf_ensure(sb, 1u))
        return false;
    sb->data[sb->length++] = c;
    return true;
}

bool xr_strbuf_append_int(XrStrBuf *sb, int64_t val) {
    char buf[24];
    int len = xr_numeric_core_format_i64(buf, sizeof(buf), val);
    return len > 0 && xr_strbuf_append_cstr(sb, buf, (size_t) len);
}

bool xr_strbuf_append_float(XrStrBuf *sb, double val) {
    char buf[64];
    int len = xr_format_float(buf, sizeof(buf), val);
    return len > 0 && xr_strbuf_append_cstr(sb, buf, (size_t) len);
}

/* ========== Reset ========== */

void xr_strbuf_reset(XrStrBuf *sb) {
    sb->length = 0;
}
