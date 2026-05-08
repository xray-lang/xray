/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray_methods.c - Array instance method bodies + dispatch table.
 *
 * Notes on the inline-push path:
 *
 *   The legacy dispatcher inlined push to skip a function call
 *   inside a hot loop. Per-type table dispatch incurs one indirect
 *   call (slot->fn), which is what the JIT and invoke-IC will
 *   eventually constant-fold. Until that lands, push uses the
 *   same fast path (capacity check + direct write + GC back
 *   barrier) inside the method body — equivalent code, just
 *   reachable via the table instead of a hand-written if-else.
 */

#include "xarray_methods.h"
#include "xarray.h"
#include "xstring.h"
#include "../closure/xclosure.h"
#include "../value/xvalue.h"
#include "../value/xvalue_format.h"
#include "../gc/xalloc_unified.h"
#include "../gc/xcoro_gc.h"
#include "../../coro/xcoroutine.h"
#include "../../base/xchecks.h"
#include <string.h>

static inline XrArray *array_self(XrValue self) {
    /* Slices share XrArray layout — both shapes are valid receivers. */
    XR_DCHECK(XR_IS_ARRAY_OR_SLICE(self), "array method: receiver is not an array or slice");
    return XR_TO_ARRAY(self);
}

/* === Mutation === */

static XrValue m_push(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    XrArray *arr = array_self(self);
    if (argc >= 1) {
        if (arr->length >= arr->capacity) {
            xr_array_grow(arr);
            /* Defensive: if grow silently failed fall back to safe push. */
            if (arr->length >= arr->capacity) {
                xr_array_push(arr, args[0]);
                return self;
            }
        }
        if (arr->elem_type == XR_ELEM_ANY) {
            ((XrValue *) arr->data)[arr->length++] = args[0];
            XR_ARRAY_MARK_GC_PTRS(arr, args[0]);
            /* Notify incremental GC the (possibly black) container was mutated. */
            XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), arr);
        } else {
            xr_array_set_element(arr, arr->length++, args[0]);
        }
    }
    return self;
}

static XrValue m_pop(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrArray *arr = array_self(self);
    return arr->length > 0 ? xr_array_pop(arr) : xr_null();
}

static XrValue m_shift(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_array_shift(array_self(self));
}

static XrValue m_unshift(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return xr_int(0);
    XrArray *arr = array_self(self);
    xr_array_unshift(arr, args[0]);
    return xr_int((xr_Integer) xr_array_size(arr));
}

static XrValue m_clear(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    xr_array_clear(array_self(self));
    return xr_null();
}

static XrValue m_reverse(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrArray *arr = array_self(self);
    xr_array_reverse(arr);
    return self;
}

static XrValue m_fill(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return self;
    XrArray *arr = array_self(self);
    XrValue fill_val = args[0];
    int start = 0, end = (int) arr->length;
    if (argc >= 2 && XR_IS_INT(args[1]))
        start = (int) XR_TO_INT(args[1]);
    if (argc >= 3 && XR_IS_INT(args[2]))
        end = (int) XR_TO_INT(args[2]);
    xr_array_fill(arr, fill_val, start, end);
    return self;
}

static XrValue m_sort(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrArray *arr = array_self(self);
    struct XrClosure *cmp = NULL;
    if (argc >= 1 && XR_IS_PTR(args[0])) {
        cmp = (struct XrClosure *) XR_TO_PTR(args[0]);
    }
    xr_array_sort(iso, arr, cmp);
    return self;
}

/* === Query === */

static XrValue m_is_empty(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(xr_array_is_empty(array_self(self)));
}

static XrValue m_has(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return xr_bool(0);
    return xr_bool(xr_array_has(array_self(self), args[0]));
}

static XrValue m_includes(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return xr_bool(false);
    return xr_bool(xr_array_has(array_self(self), args[0]));
}

static XrValue m_index_of(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return xr_int(-1);
    return xr_int((xr_Integer) xr_array_index_of(array_self(self), args[0]));
}

/* === Construction (returns new array) === */

static XrValue m_slice(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrArray *arr = array_self(self);
    int len = (int) arr->length;
    int start = 0, end = len;
    if (argc >= 1 && XR_IS_INT(args[0])) {
        start = (int) XR_TO_INT(args[0]);
        if (start < 0)
            start = len + start;
        if (start < 0)
            start = 0;
    }
    if (argc >= 2 && XR_IS_INT(args[1])) {
        end = (int) XR_TO_INT(args[1]);
        if (end < 0)
            end = len + end;
        if (end > len)
            end = len;
    }
    if (start >= end || start >= len) {
        return xr_value_from_array(xr_array_new(xr_current_coro(iso)));
    }
    int count = end - start;
    XrArray *result;
    if (arr->elem_type != XR_ELEM_ANY) {
        result = xr_array_with_capacity_typed(xr_current_coro(iso), count,
                                              (XrArrayElemType) arr->elem_type);
        if (result) {
            result->elem_tid = arr->elem_tid;
            memcpy(result->data, (uint8_t *) arr->data + (size_t) start * arr->elem_size,
                   (size_t) count * arr->elem_size);
            result->length = count;
        }
    } else {
        result = xr_array_with_capacity(xr_current_coro(iso), count);
        if (result) {
            memcpy(result->data, (XrValue *) arr->data + start, (size_t) count * sizeof(XrValue));
            result->length = count;
            result->has_gc_ptrs = arr->has_gc_ptrs;
        }
    }
    return xr_value_from_array(result ? result : xr_array_new(xr_current_coro(iso)));
}

static XrValue m_concat(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrArray *arr = array_self(self);
    int total = (int) arr->length;
    for (int i = 0; i < argc; i++) {
        total += XR_IS_ARRAY(args[i]) ? (int) XR_TO_ARRAY(args[i])->length : 1;
    }
    XrArray *result = xr_array_with_capacity(xr_current_coro(iso), total);
    if (!result)
        return xr_value_from_array(xr_array_new(xr_current_coro(iso)));
    if (arr->length > 0) {
        memcpy(result->data, arr->data, (size_t) arr->length * arr->elem_size);
        result->length = arr->length;
        result->has_gc_ptrs = arr->has_gc_ptrs;
    }
    for (int i = 0; i < argc; i++) {
        if (XR_IS_ARRAY(args[i])) {
            XrArray *other = XR_TO_ARRAY(args[i]);
            if (other->length > 0) {
                xr_array_ensure_capacity(result, result->length + other->length);
                memcpy((XrValue *) result->data + result->length, other->data,
                       (size_t) other->length * sizeof(XrValue));
                result->length += other->length;
                if (other->has_gc_ptrs)
                    result->has_gc_ptrs = 1;
            }
        } else {
            xr_array_push(result, args[i]);
        }
    }
    return xr_value_from_array(result);
}

static XrValue m_join(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrArray *arr = array_self(self);
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_string_value(xr_string_intern(iso, "", 0, 0));
    }
    XrString *delim = xr_value_to_string(iso, args[0]);
    XrString *result = xr_array_join(iso, arr, delim);
    return result ? xr_string_value(result) : xr_null();
}

/* === Higher-order callbacks === */

static XrValue m_foreach(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    XrArray *arr = array_self(self);
    struct XrClosure *cb = (struct XrClosure *) XR_TO_PTR(args[0]);
    xr_array_foreach(iso, arr, cb);
    return xr_null();
}

static XrValue m_filter(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1) {
        return xr_value_from_array(xr_array_new(xr_current_coro(iso)));
    }
    XrArray *arr = array_self(self);
    struct XrClosure *cb = (struct XrClosure *) XR_TO_PTR(args[0]);
    return xr_value_from_array(xr_array_filter(iso, arr, cb));
}

static XrValue m_map(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1) {
        return xr_value_from_array(xr_array_new(xr_current_coro(iso)));
    }
    XrArray *arr = array_self(self);
    struct XrClosure *cb = (struct XrClosure *) XR_TO_PTR(args[0]);
    return xr_value_from_array(xr_array_map(iso, arr, cb));
}

static XrValue m_reduce(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 2)
        return xr_null();
    XrArray *arr = array_self(self);
    struct XrClosure *cb = (struct XrClosure *) XR_TO_PTR(args[0]);
    return xr_array_reduce(iso, arr, cb, args[1]);
}

static XrValue m_find(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    XrArray *arr = array_self(self);
    struct XrClosure *cb = (struct XrClosure *) XR_TO_PTR(args[0]);
    return xr_array_find(iso, arr, cb);
}

static XrValue m_find_index(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1)
        return xr_int(-1);
    XrArray *arr = array_self(self);
    struct XrClosure *cb = (struct XrClosure *) XR_TO_PTR(args[0]);
    return xr_int(xr_array_find_index(iso, arr, cb));
}

static XrValue m_every(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1)
        return xr_bool(true);
    XrArray *arr = array_self(self);
    struct XrClosure *cb = (struct XrClosure *) XR_TO_PTR(args[0]);
    return xr_bool(xr_array_every(iso, arr, cb));
}

static XrValue m_some(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1)
        return xr_bool(false);
    XrArray *arr = array_self(self);
    struct XrClosure *cb = (struct XrClosure *) XR_TO_PTR(args[0]);
    return xr_bool(xr_array_some(iso, arr, cb));
}

/* === toString === */

static XrValue m_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xr_string_value(xr_value_to_string(iso, self));
}

/* ========== XrClass Registration ========== */

#include "xnative_type.h"
#include "builtins/xarray_builtins.h"

void xr_array_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod array_methods[] = {
        /* Mutation */
        {"push", m_push, 1},
        {"pop", m_pop, 0},
        {"shift", m_shift, 0},
        {"unshift", m_unshift, 1},
        {"clear", m_clear, 0},
        {"reverse", m_reverse, 0},
        {"fill", m_fill, 1},
        {"sort", m_sort, 0},
        /* Query */
        {"isEmpty", m_is_empty, 0},
        {"has", m_has, 1},
        {"includes", m_includes, 1},
        {"indexOf", m_index_of, 1},
        /* Construction */
        {"slice", m_slice, 0},
        {"concat", m_concat, 0},
        {"join", m_join, 0},
        /* Higher-order */
        {"forEach", m_foreach, 1},
        {"filter", m_filter, 1},
        {"map", m_map, 1},
        {"reduce", m_reduce, 2},
        {"find", m_find, 1},
        {"findIndex", m_find_index, 1},
        {"every", m_every, 1},
        {"some", m_some, 1},
        /* Conversion */
        {"toString", m_to_string, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod array_statics[] = {
        {"constructor", xr_builtin_array_construct, 0},
        {"from", xr_builtin_array_from, 1},
        {"range", xr_builtin_array_range, 2},
        {"withCapacity", xr_builtin_array_with_capacity, 1},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo array_info = {
        .name = "Array",
        .gc_type = XR_TARRAY,
        .methods = array_methods,
        .getters = NULL,
        .static_methods = array_statics,
    };
    xr_register_native_type(isolate, &array_info);
}
