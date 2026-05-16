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
#include "../../base/xdefs.h"

struct XrCoroutine;
struct XrCoroGC;

/* ========== Object Layout ========== */

/*
 * Single contiguous allocation. The flexible `elements` array follows
 * the fixed header without padding, so the entire tuple is one cache-
 * friendly block on the Immix heap.
 */
typedef struct XrTuple {
    XrGCHeader gc;
    uint16_t element_count;
    uint16_t _pad0;
    uint32_t _pad1;
    XrValue elements[];
} XrTuple;

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

/* ========== GC Integration ========== */

/*
 * Mark every element so the collector keeps live references alive.
 * Registered through g_type_ops[XR_TTUPLE].traverse in xgc.c.
 */
XR_FUNC void xr_gc_traverse_tuple(struct XrCoroGC *gc, XrGCHeader *obj);

#endif  // XTUPLE_H
