/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstringbuilder.c - StringBuilder object implementation
 */

#include "xstringbuilder.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "xstring.h"
#include "../value/xvalue.h"
#include "../../base/xmalloc.h"
#include "../gc/xgc.h"
#include "../gc/xalloc_unified.h"
#include "../xisolate_api.h"
#include "../class/xclass_system.h"
#include <string.h>
#include <stdio.h>

/* ========== Creation and Destruction ========== */

XrStringBuilder *xr_stringbuilder_new(struct XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "stringbuilder_new: NULL coro");
    // Allocate StringBuilder on coroutine heap
    XrStringBuilder *sb =
        (XrStringBuilder *) xr_alloc(coro, sizeof(XrStringBuilder), XR_TSTRINGBUILDER);
    if (!sb) {
        xr_log_warning("stringbuilder", "memory allocation failed");
        return NULL;
    }

    // Create internal buffer
    XrayIsolate *X = xr_coro_get_isolate(coro);
    sb->buffer = xr_strbuf_new(X, 64);
    if (!sb->buffer) {
        xr_log_warning("stringbuilder", "buffer creation failed");
        return NULL;
    }

    return sb;
}

// Initialize StringBuilder on pre-allocated memory (for shared StringBuilder)
void xr_stringbuilder_init_inplace(XrStringBuilder *sb) {
    if (!sb)
        return;

    // Create internal buffer (malloc, not coroutine heap)
    sb->buffer = (XrStrBuf *) xr_malloc(sizeof(XrStrBuf));
    if (sb->buffer) {
        sb->buffer->data = (char *) xr_malloc(64);
        sb->buffer->capacity = 64;
        sb->buffer->length = 0;
        if (sb->buffer->data) {
            sb->buffer->data[0] = '\0';
        }
    }
}

void xr_stringbuilder_free(XrStringBuilder *sb) {
    if (!sb)
        return;

    if (sb->buffer) {
        xr_strbuf_free(sb->buffer);
        sb->buffer = NULL;
    }
}

/* ========== Operations ========== */

void xr_stringbuilder_append_str(XrStringBuilder *sb, XrString *s) {
    if (!sb || !sb->buffer || !s)
        return;
    xr_strbuf_append_str(sb->buffer, s);
}

void xr_stringbuilder_append_cstr(XrStringBuilder *sb, const char *s, size_t len) {
    if (!sb || !sb->buffer || !s)
        return;
    xr_strbuf_append_cstr(sb->buffer, s, len);
}

void xr_stringbuilder_append_int(XrStringBuilder *sb, int64_t val) {
    if (!sb || !sb->buffer)
        return;
    xr_strbuf_append_int(sb->buffer, val);
}

void xr_stringbuilder_append_float(XrStringBuilder *sb, double val) {
    if (!sb || !sb->buffer)
        return;
    xr_strbuf_append_float(sb->buffer, val);
}

XrString *xr_stringbuilder_to_string(XrStringBuilder *sb) {
    if (!sb || !sb->buffer)
        return NULL;

    // Create string copy without resetting buffer
    size_t len = sb->buffer->length;
    if (len == 0) {
        return xr_string_intern(sb->buffer->X, "", 0, 0);
    }

    // Temporarily add null terminator
    xr_strbuf_ensure(sb->buffer, 1);
    sb->buffer->data[len] = '\0';

    XrString *str = xr_string_intern(sb->buffer->X, sb->buffer->data, len, 0);
    return str;
}

size_t xr_stringbuilder_length(XrStringBuilder *sb) {
    if (!sb || !sb->buffer)
        return 0;
    return sb->buffer->length;
}

void xr_stringbuilder_clear(XrStringBuilder *sb) {
    if (!sb || !sb->buffer)
        return;
    xr_strbuf_reset(sb->buffer);
}

/* ========== XrValue Conversion ========== */

XrValue xr_stringbuilder_value(XrStringBuilder *sb) {
    return XR_FROM_PTR(sb);
}

bool xr_is_stringbuilder(XrValue v) {
    return XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TSTRINGBUILDER;
}

XrStringBuilder *xr_to_stringbuilder(XrValue v) {
    if (!xr_is_stringbuilder(v))
        return NULL;
    return (XrStringBuilder *) XR_TO_PTR(v);
}

/* ========== GC Integration ========== */

void xr_gc_destroy_stringbuilder(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    (void) owning_gc;
    XrStringBuilder *sb = (XrStringBuilder *) obj;
    if (sb->buffer) {
        xr_strbuf_free(sb->buffer);
        sb->buffer = NULL;
    }
}
