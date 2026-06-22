/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xatomic.h - Atomic<T> shared primitive wrapper for lock-free concurrency
 *
 * KEY CONCEPT:
 *   Atomic<T> wraps a single primitive value (int, float, bool) with
 *   hardware atomic operations. Lives on the system heap with atomic
 *   refcount (XR_OBJ_STORAGE_SHARED), same as Channel.
 *
 * INVARIANTS:
 *   - T must be int, float, or bool (compiler-enforced)
 *   - Must be declared as `shared const` (container immutable, value atomic)
 *   - All operations are lock-free via C11 atomics
 *   - Float atomicity via int64_t bit-cast (all platforms support 64-bit CAS)
 *   - Deep copy = incref (XR_COPY_SHARED_REF), never cloned
 */

#ifndef XATOMIC_H
#define XATOMIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#include "../gc/xgc_header.h"
#include "../value/xvalue.h"
#include "../../base/xdefs.h"

/* ========== Atomic Kind ========== */

typedef enum {
    XR_ATOMIC_INT = 0,
    XR_ATOMIC_FLOAT = 1,
    XR_ATOMIC_BOOL = 2
} XrAtomicKind;

/* ========== Atomic Memory Ordering ========== */

typedef enum {
    XR_ORDERING_RELAXED = 0,
    XR_ORDERING_ACQUIRE = 1,
    XR_ORDERING_RELEASE = 2,
    XR_ORDERING_ACQUIRE_RELEASE = 3,
    XR_ORDERING_SEQ_CST = 4
} XrAtomicOrdering;

/* ========== XrAtomic Object Layout ========== */

typedef struct XrAtomic {
    XrObjHeader gc;         /* type = XR_TATOMIC, storage = SHARED */
    _Atomic(int64_t) value; /* 64-bit: int64 direct, float64 bit-cast, bool 0/1 */
    uint8_t kind;           /* XrAtomicKind */
} XrAtomic;

/* ========== Constructor ========== */

struct XrayIsolate;

/* Allocate Atomic on system heap with initial value. */
XR_FUNC XrAtomic *xr_atomic_new(struct XrayIsolate *X, XrAtomicKind kind, int64_t initial);

/* ========== Value Conversion Helpers ========== */

/* Pack XrValue into int64_t for atomic storage. */
static inline int64_t xr_atomic_pack(XrValue v, XrAtomicKind kind) {
    switch (kind) {
        case XR_ATOMIC_INT:
            return XR_TO_INT(v);
        case XR_ATOMIC_BOOL:
            return XR_TO_BOOL(v) ? 1 : 0;
        case XR_ATOMIC_FLOAT: {
            double d = XR_TO_FLOAT(v);
            int64_t bits;
            memcpy(&bits, &d, sizeof(bits));
            return bits;
        }
    }
    return 0;
}

/* Unpack int64_t from atomic storage to XrValue. */
static inline XrValue xr_atomic_unpack(int64_t raw, XrAtomicKind kind) {
    switch (kind) {
        case XR_ATOMIC_INT:
            return xr_int(raw);
        case XR_ATOMIC_BOOL:
            return xr_bool(raw != 0);
        case XR_ATOMIC_FLOAT: {
            double d;
            memcpy(&d, &raw, sizeof(d));
            return xr_float(d);
        }
    }
    return xr_null();
}

/* ========== C11 Ordering Mapping ========== */

static inline memory_order xr_to_c11_order(XrAtomicOrdering ord) {
    switch (ord) {
        case XR_ORDERING_RELAXED:
            return memory_order_relaxed;
        case XR_ORDERING_ACQUIRE:
            return memory_order_acquire;
        case XR_ORDERING_RELEASE:
            return memory_order_release;
        case XR_ORDERING_ACQUIRE_RELEASE:
            return memory_order_acq_rel;
        case XR_ORDERING_SEQ_CST:
            return memory_order_seq_cst;
    }
    return memory_order_seq_cst;
}

/* ========== Atomic Operations ========== */

XR_FUNC int64_t xr_atomic_load(XrAtomic *a, XrAtomicOrdering ord);
XR_FUNC void xr_atomic_store(XrAtomic *a, int64_t val, XrAtomicOrdering ord);
XR_FUNC int64_t xr_atomic_fetch_add(XrAtomic *a, int64_t delta, XrAtomicOrdering ord);
XR_FUNC int64_t xr_atomic_fetch_sub(XrAtomic *a, int64_t delta, XrAtomicOrdering ord);
XR_FUNC int64_t xr_atomic_swap(XrAtomic *a, int64_t desired, XrAtomicOrdering ord);
XR_FUNC bool xr_atomic_compare_exchange(XrAtomic *a, int64_t *expected, int64_t desired,
                                        XrAtomicOrdering ord);

/* ========== XrValue Helper ========== */

static inline bool xr_value_is_atomic(XrValue v) {
    return XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TATOMIC;
}

static inline XrAtomic *xr_value_to_atomic(XrValue v) {
    return (XrAtomic *) XR_TO_PTR(v);
}

static inline XrValue xr_value_from_atomic(XrAtomic *a) {
    return XR_FROM_PTR(a);
}

/* ========== Native Type Registration ========== */

XR_FUNC void xr_atomic_register_native_type(struct XrayIsolate *X);

#endif  // XATOMIC_H
