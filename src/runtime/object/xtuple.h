/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtuple.h - Heap-allocated tuple object
 *
 * KEY CONCEPT:
 *   - Fixed arity, decided at construction time (element_count is set once).
 *   - Single allocation: header followed by a flexible array of XrValue.
 *   - O(1) field access by index — `.N` is lowered to elements[N].
 *   - Structurally immutable from user code (no element_set API exposed):
 *     once built, GC traversal is the only writer-visible operation.
 *   - The 0-arity tuple is the canonical Unit value at the type level;
 *     at runtime we still allow allocating one, but most call sites
 *     materialize unit as a known singleton rather than a heap object.
 */

#ifndef XTUPLE_H
#define XTUPLE_H

#include "../value/xvalue.h"
#include "../gc/xgc_header.h"
#include "../class/xclass.h"
#include "../class/xinstance.h"
#include "../../base/xdefs.h"

struct XrCoroutine;
struct XrCoroGC;
struct XrayIsolate;

/* ========== Object Layout ==========
 *
 * Tuples are regular XrInstance values backed by a per-arity XrClass
 * (core->tupleClassesSmall[arity] for arities < XR_TUPLE_CLASS_PREALLOC,
 * lazily-built classes for larger arities). The struct mirrors the
 * XrInstance layout exactly so that casts in either direction read the
 * same bytes:
 *
 *   gc header  (16)
 *   klass *    (8)      <- always a tuple class (klass->flags & XR_CLASS_TUPLE)
 *   elements[] (16*arity)
 *
 * Arity is read from klass->field_count, never stored separately.
 */
typedef struct XrTuple {
    XrGCHeader gc;
    struct XrClass *klass;
    XrValue elements[];
} XrTuple;

/* Number of elements in `t`. Returns 0 for NULL. */
static inline uint16_t xr_tuple_arity(const XrTuple *t) {
    if (!t || !t->klass)
        return 0;
    return (uint16_t) xr_class_instance_field_count(t->klass);
}

/* ========== Creation ========== */

/*
 * Allocate a tuple with `count` slots on the coroutine GC heap.
 * Elements are zero-initialised (XR_TAG_NULL) and must be populated
 * by the caller before any GC-visible publication.
 *
 * Returns NULL on allocation failure.
 */
XR_FUNC XrTuple *xr_tuple_new(struct XrCoroutine *coro, uint16_t element_count);

/*
 * Convenience constructor that copies `count` values into a fresh
 * tuple. Equivalent to xr_tuple_new + a manual fill loop, but lets
 * the lowering layer emit a single CALL_C site.
 */
XR_FUNC XrTuple *xr_tuple_from_values(struct XrCoroutine *coro, const XrValue *values,
                                      uint16_t count);

/* ========== Field Access ========== */

/*
 * Read elements[i]. Returns XR_TAG_NULL when `t` is NULL or `i` is
 * out of bounds — the front-end has already proven that .N indices
 * are in range at compile time, so this guard is purely defensive.
 */
XR_FUNC XrValue xr_tuple_get(XrTuple *t, uint16_t index);

/*
 * Write elements[i]. Intended for the construction path only; user
 * code never mutates a tuple after it has been published.
 */
XR_FUNC void xr_tuple_set(XrTuple *t, uint16_t index, XrValue value);

/* ========== Type Check ========== */

/* True iff `v` is an XrInstance whose class has XR_CLASS_TUPLE set.
 * This replaces the legacy XR_IS_TUPLE macro that depended on a
 * dedicated XR_TTUPLE GC tag. */
XR_FUNC bool xr_value_is_tuple(XrValue v);

/* ========== Structural Equality and Hashing ========== */

/*
 * Structural equality: same arity, recursively equal elements via
 * xr_value_equals. Returns true when both arguments alias the same
 * object.
 */
XR_FUNC bool xr_tuple_equals(XrTuple *a, XrTuple *b);

/*
 * Stable structural hash. Combines element hashes with a multiplicative
 * mix; arity participates so `()` and `(0,)` do not collide.
 */
XR_FUNC uint64_t xr_tuple_hash(XrTuple *t);

#endif  // XTUPLE_H
