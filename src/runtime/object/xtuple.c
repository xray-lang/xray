/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtuple.c - Heap-allocated tuple object
 */

#include "xtuple.h"

#include <string.h>

#include "../value/xvalue.h"
#include "../value/xvalue_hash.h"
#include "../gc/xgc.h"
#include "../gc/xcoro_gc.h"
#include "../../base/xdefs.h"

/* ========== Allocation ========== */

static inline size_t tuple_alloc_size(uint16_t element_count) {
    return sizeof(XrTuple) + (size_t) element_count * sizeof(XrValue);
}

XrTuple *xr_tuple_new(struct XrCoroutine *coro, uint16_t element_count) {
    XR_DCHECK(coro != NULL, "xr_tuple_new: NULL coro");

    size_t size = tuple_alloc_size(element_count);
    XrTuple *t = (XrTuple *) xr_alloc(coro, size, XR_TTUPLE);
    if (!t)
        return NULL;

    xr_gc_header_init_type(&t->gc, XR_TTUPLE);
    t->element_count = element_count;
    t->_pad0 = 0;
    t->_pad1 = 0;

    /* Pre-fill with null so traversal of a half-built tuple is safe
     * (the GC may run before the caller populates every slot). */
    XrValue null_v = {0};
    for (uint16_t i = 0; i < element_count; i++) {
        t->elements[i] = null_v;
    }
    return t;
}

XrTuple *xr_tuple_from_values(struct XrCoroutine *coro, const XrValue *values, uint16_t count) {
    XR_DCHECK(coro != NULL, "xr_tuple_from_values: NULL coro");
    XR_DCHECK(count == 0 || values != NULL,
              "xr_tuple_from_values: NULL values with non-zero count");

    XrTuple *t = xr_tuple_new(coro, count);
    if (!t)
        return NULL;
    if (count > 0)
        memcpy(t->elements, values, (size_t) count * sizeof(XrValue));
    return t;
}

/* ========== Field Access ========== */

XrValue xr_tuple_get(XrTuple *t, uint16_t index) {
    if (!t || index >= t->element_count) {
        XrValue null_v = {0};
        return null_v;
    }
    return t->elements[index];
}

void xr_tuple_set(XrTuple *t, uint16_t index, XrValue value) {
    XR_DCHECK(t != NULL, "xr_tuple_set: NULL tuple");
    XR_DCHECK(index < t->element_count, "xr_tuple_set: index out of range");
    if (!t || index >= t->element_count)
        return;
    t->elements[index] = value;
}

/* ========== Structural Equality ========== */

/* Recursive equality so nested tuples compare element-wise rather
 * than by heap pointer. Falls back to xr_value_eq for non-tuple
 * elements (primitives, strings, and reference types). */
static bool tuple_elem_eq(XrValue a, XrValue b) {
    if (XR_IS_TUPLE(a) && XR_IS_TUPLE(b))
        return xr_tuple_equals(XR_TO_TUPLE(a), XR_TO_TUPLE(b));
    return xr_value_eq(a, b);
}

bool xr_tuple_equals(XrTuple *a, XrTuple *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    if (a->element_count != b->element_count)
        return false;
    for (uint16_t i = 0; i < a->element_count; i++) {
        if (!tuple_elem_eq(a->elements[i], b->elements[i]))
            return false;
    }
    return true;
}

/* ========== Hash ========== */

/*
 * FNV-1a style mix over per-element 32-bit hashes, plus the arity in
 * the seed so `()` and `(0,)` do not collide. Returned as 64-bit for
 * future widening; current callers truncate to 32 if desired.
 */
uint64_t xr_tuple_hash(XrTuple *t) {
    if (!t)
        return 0;
    uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t) t->element_count;
    for (uint16_t i = 0; i < t->element_count; i++) {
        uint32_t eh = xr_hash_value(t->elements[i]);
        h ^= (uint64_t) eh;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ========== GC Traversal ========== */

void xr_gc_traverse_tuple(struct XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    XrTuple *t = (XrTuple *) obj;
    XR_DCHECK(XR_GC_GET_TYPE(&t->gc) == XR_TTUPLE, "gc_traverse_tuple: object is not a tuple");
    for (uint16_t i = 0; i < t->element_count; i++) {
        xr_coro_gc_markvalue(gc, t->elements[i]);
    }
}
