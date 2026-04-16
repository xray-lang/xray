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
 *   Per-context temp buffer avoids repeated allocations.
 */

#include "xstrbuf.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "xray_isolate.h"
#include "xisolate_api.h"
#include "object/xstring.h"
#include "../base/xmalloc.h"
#include "../coro/xworker.h" // XrWorker, xr_current_worker
#include <string.h>
#include <stdio.h>
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
                (size_t)XR_STRBUF_MAX_CAP);
        return;
    }
    
    char *new_data = (char *)xr_realloc(sb->data, new_cap);
    if (!new_data) {
        xr_log_warning("strbuf", "memory allocation failed");
        return;
    }
    
    sb->data = new_data;
    sb->capacity = new_cap;
}

/* ========== Create and Destroy ========== */

// Get current execution context
// Multi-core: returns Worker's vm_ctx
// Single-thread: returns Isolate's vm_ctx
static inline XrVMContext *xr_get_current_vm_context(XrayIsolate *X) {
    XrWorker *worker = xr_current_worker();
    if (worker) {
        return &worker->m->vm_ctx;
    }
    return xr_isolate_get_vm_ctx(X);
}

XrStrBuf *xr_strbuf_tmp(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "strbuf_tmp: NULL isolate");
    // Get current context's temp buffer
    XrVMContext *ctx = xr_get_current_vm_context(X);
    
    // Lazy allocation
    if (!ctx->tmp_strbuf) {
        ctx->tmp_strbuf = xr_strbuf_new(X, XR_STRBUF_MIN_CAP);
        if (!ctx->tmp_strbuf) {
            fprintf(stderr, "[ERROR] tmp_strbuf allocation failed\n");
            return NULL;
        }
    }
    
    // Reset for reuse
    xr_strbuf_reset(ctx->tmp_strbuf);
    return ctx->tmp_strbuf;
}

XrStrBuf *xr_strbuf_new(XrayIsolate *X, size_t init_cap) {
    XR_DCHECK(X != NULL, "strbuf_new: NULL isolate");
    if (init_cap < XR_STRBUF_MIN_CAP) {
        init_cap = XR_STRBUF_MIN_CAP;
    }
    
    XrStrBuf *sb = (XrStrBuf *)xr_malloc(sizeof(XrStrBuf));
    if (!sb) return NULL;
    
    sb->data = (char *)xr_malloc(init_cap);
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
    if (!sb) return;
    
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
    if (!s || s->length == 0) return;
    
    xr_strbuf_ensure(sb, s->length);
    memcpy(sb->data + sb->length, s->data, s->length);
    sb->length += s->length;
}

void xr_strbuf_append_cstr(XrStrBuf *sb, const char *s, size_t len) {
    if (!s || len == 0) return;
    
    xr_strbuf_ensure(sb, len);
    memcpy(sb->data + sb->length, s, len);
    sb->length += len;
}

void xr_strbuf_append_char(XrStrBuf *sb, char c) {
    xr_strbuf_ensure(sb, 1);
    sb->data[sb->length++] = c;
}

void xr_strbuf_append_int(XrStrBuf *sb, int64_t val) {
    // Fast int-to-string (without snprintf)
    // Use uint64_t to avoid UB on -INT64_MIN
    char buf[24];
    char *p = buf;
    int neg = 0;
    uint64_t uval;
    
    if (val < 0) {
        neg = 1;
        uval = (uint64_t)(-(val + 1)) + 1;
    } else {
        uval = (uint64_t)val;
    }
    
    // Write digits in reverse
    char *start = p;
    do {
        *p++ = '0' + (char)(uval % 10);
        uval /= 10;
    } while (uval > 0);
    
    if (neg) {
        *p++ = '-';
    }
    
    int len = (int)(p - start);
    
    // Reverse the string
    char *end = p - 1;
    while (start < end) {
        char tmp = *start;
        *start = *end;
        *end = tmp;
        start++;
        end--;
    }
    
    xr_strbuf_append_cstr(sb, buf, (size_t)len);
}

void xr_strbuf_append_float(XrStrBuf *sb, double val) {
    // Format float
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%.14g", val);
    
    if (len > 0) {
        xr_strbuf_append_cstr(sb, buf, (size_t)len);
    }
}

/* ========== Convert and Reset ========== */

XrString *xr_strbuf_to_string(XrStrBuf *sb) {
    // Pre-calculate hash to avoid recalculation in xr_string_intern
    uint32_t hash = xr_string_hash(sb->data, sb->length);
    
    // Create new string (interned for later comparison)
    XrString *s = xr_string_intern(sb->X, sb->data, sb->length, hash);
    
    // Reset buffer for reuse
    sb->length = 0;
    
    return s;
}

void xr_strbuf_reset(XrStrBuf *sb) {
    sb->length = 0;
}

