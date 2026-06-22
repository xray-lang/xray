/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xatomic.c - Atomic<T> runtime implementation
 *
 * KEY CONCEPT:
 *   System-heap allocated wrapper around a single primitive value.
 *   All operations use C11 atomics for lock-free concurrency.
 *   Registered as a native type so method dispatch goes through the
 *   unified IC-cached class lookup path (no special VM dispatch).
 */

#include "xatomic.h"
#include "xnative_type.h"
#include "../mem/xsystem_heap.h"
#include "../mem/xheap.h"
#include "../xshared.h"
#include "../xisolate_api.h"
#include "../xerror_codes.h"
#include "xexception.h"
#include "../value/xvalue_format.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../vm/xvm.h"
#include "xtuple.h"
#include "../class/xenum.h"
#include <string.h>

/* ========== Constructor ========== */

XrAtomic *xr_atomic_new(XrVMRuntime *X, XrAtomicKind kind, int64_t initial) {
    XR_DCHECK(X != NULL, "xr_atomic_new: NULL isolate");
    XR_DCHECK(kind <= XR_ATOMIC_BOOL, "xr_atomic_new: invalid kind");

    XrSystemHeap *heap = xr_isolate_get_sys_heap(X);
    if (!heap)
        return NULL;

    XrAtomic *a = (XrAtomic *) xr_sysheap_alloc_shared(heap, sizeof(XrAtomic), XR_TATOMIC);
    if (!a)
        return NULL;

    /* Atomic shared-RC: pure cross-coroutine shared data, no executor owner, so
     * the compiler tracks it like `shared const` (dup = atomic incref, last drop
     * frees). NOT XR_OBJ_MANAGED — that would leak the handle (drop no-op). */
    xr_shared_set_refc(&a->hdr, 1);
    a->kind = (uint8_t) kind;
    atomic_store(&a->value, initial);

    return a;
}

/* ========== Core Atomic Operations ========== */

int64_t xr_atomic_load(XrAtomic *a, XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_load: NULL atomic");
    return atomic_load_explicit(&a->value, xr_to_c11_order(ord));
}

void xr_atomic_store(XrAtomic *a, int64_t val, XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_store: NULL atomic");
    atomic_store_explicit(&a->value, val, xr_to_c11_order(ord));
}

int64_t xr_atomic_fetch_add(XrAtomic *a, int64_t delta, XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_fetch_add: NULL atomic");
    return atomic_fetch_add_explicit(&a->value, delta, xr_to_c11_order(ord));
}

int64_t xr_atomic_fetch_sub(XrAtomic *a, int64_t delta, XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_fetch_sub: NULL atomic");
    return atomic_fetch_sub_explicit(&a->value, delta, xr_to_c11_order(ord));
}

int64_t xr_atomic_swap(XrAtomic *a, int64_t desired, XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_swap: NULL atomic");
    return atomic_exchange_explicit(&a->value, desired, xr_to_c11_order(ord));
}

bool xr_atomic_compare_exchange(XrAtomic *a, int64_t *expected, int64_t desired,
                                XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_compare_exchange: NULL atomic");
    XR_DCHECK(expected != NULL, "xr_atomic_compare_exchange: NULL expected");
    /* Strong CAS: no spurious failures. On failure, *expected is
     * updated to the current value (standard C11 behaviour). */
    return atomic_compare_exchange_strong_explicit(&a->value, expected, desired,
                                                   xr_to_c11_order(ord), memory_order_relaxed);
}

/* ========== Ordering Argument Parser ========== */

/* Parse optional trailing Ordering argument from method args.
 * Accepts Ordering enum values (primary) and raw int (fallback).
 * Returns XR_ORDERING_SEQ_CST when not provided or out of range. */
static XrAtomicOrdering parse_ordering(XrValue *args, int nargs, int ord_idx) {
    if (nargs <= ord_idx)
        return XR_ORDERING_SEQ_CST;
    XrValue v = args[ord_idx];

    /* Ordering enum value: extract the backing int via raw_value. */
    if (XR_IS_ENUM_VALUE(v)) {
        XrEnumValue *ev = (XrEnumValue *) XR_TO_PTR(v);
        if (XR_IS_INT(ev->raw_value)) {
            int64_t ival = XR_TO_INT(ev->raw_value);
            if (ival >= XR_ORDERING_RELAXED && ival <= XR_ORDERING_SEQ_CST)
                return (XrAtomicOrdering) ival;
        }
        return XR_ORDERING_SEQ_CST;
    }
    /* Raw int fallback for internal/test use. */
    if (XR_IS_INT(v)) {
        int64_t ival = XR_TO_INT(v);
        if (ival >= XR_ORDERING_RELAXED && ival <= XR_ORDERING_SEQ_CST)
            return (XrAtomicOrdering) ival;
    }
    return XR_ORDERING_SEQ_CST;
}

/* ========== Native Method Implementations ========== */

/* .load(ordering?) -> T */
static XrValue m_load(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrAtomic *a = xr_value_to_atomic(self);
    XR_DCHECK(a != NULL, "Atomic.load: NULL atomic");
    XrAtomicOrdering ord = parse_ordering(args, nargs, 0);
    int64_t raw = xr_atomic_load(a, ord);
    return xr_atomic_unpack(raw, (XrAtomicKind) a->kind);
}

/* .store(value, ordering?) -> null */
static XrValue m_store(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrAtomic *a = xr_value_to_atomic(self);
    XR_DCHECK(a != NULL, "Atomic.store: NULL atomic");
    XR_DCHECK(nargs >= 1, "Atomic.store: missing value argument");
    int64_t packed = xr_atomic_pack(args[0], (XrAtomicKind) a->kind);
    XrAtomicOrdering ord = parse_ordering(args, nargs, 1);
    xr_atomic_store(a, packed, ord);
    return xr_null();
}

/* .add(delta, ordering?) -> null */
static XrValue m_add(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrAtomic *a = xr_value_to_atomic(self);
    XR_DCHECK(a != NULL, "Atomic.add: NULL atomic");
    XR_DCHECK(nargs >= 1, "Atomic.add: missing delta argument");

    if (a->kind == XR_ATOMIC_INT) {
        XrAtomicOrdering ord = parse_ordering(args, nargs, 1);
        xr_atomic_fetch_add(a, XR_TO_INT(args[0]), ord);
    } else if (a->kind == XR_ATOMIC_FLOAT) {
        /* Float add: CAS loop (no hardware atomic float add) */
        double delta = XR_TO_FLOAT(args[0]);
        int64_t cur = atomic_load(&a->value);
        for (;;) {
            double dval;
            memcpy(&dval, &cur, sizeof(dval));
            dval += delta;
            int64_t desired;
            memcpy(&desired, &dval, sizeof(desired));
            if (atomic_compare_exchange_weak(&a->value, &cur, desired))
                break;
        }
    }
    return xr_null();
}

/* .sub(delta, ordering?) -> null */
static XrValue m_sub(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrAtomic *a = xr_value_to_atomic(self);
    XR_DCHECK(a != NULL, "Atomic.sub: NULL atomic");
    XR_DCHECK(nargs >= 1, "Atomic.sub: missing delta argument");

    if (a->kind == XR_ATOMIC_INT) {
        XrAtomicOrdering ord = parse_ordering(args, nargs, 1);
        xr_atomic_fetch_sub(a, XR_TO_INT(args[0]), ord);
    } else if (a->kind == XR_ATOMIC_FLOAT) {
        double delta = XR_TO_FLOAT(args[0]);
        int64_t cur = atomic_load(&a->value);
        for (;;) {
            double dval;
            memcpy(&dval, &cur, sizeof(dval));
            dval -= delta;
            int64_t desired;
            memcpy(&desired, &dval, sizeof(desired));
            if (atomic_compare_exchange_weak(&a->value, &cur, desired))
                break;
        }
    }
    return xr_null();
}

/* .fetchAdd(delta, ordering?) -> T (old value) */
static XrValue m_fetch_add(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrAtomic *a = xr_value_to_atomic(self);
    XR_DCHECK(a != NULL, "Atomic.fetchAdd: NULL atomic");
    XR_DCHECK(nargs >= 1, "Atomic.fetchAdd: missing delta argument");

    if (a->kind == XR_ATOMIC_INT) {
        XrAtomicOrdering ord = parse_ordering(args, nargs, 1);
        int64_t old = xr_atomic_fetch_add(a, XR_TO_INT(args[0]), ord);
        return xr_int(old);
    }
    if (a->kind == XR_ATOMIC_FLOAT) {
        double delta = XR_TO_FLOAT(args[0]);
        int64_t cur = atomic_load(&a->value);
        for (;;) {
            double dval;
            memcpy(&dval, &cur, sizeof(dval));
            double old_val = dval;
            dval += delta;
            int64_t desired;
            memcpy(&desired, &dval, sizeof(desired));
            if (atomic_compare_exchange_weak(&a->value, &cur, desired))
                return xr_float(old_val);
        }
    }
    return xr_null();
}

/* .fetchSub(delta, ordering?) -> T (old value) */
static XrValue m_fetch_sub(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrAtomic *a = xr_value_to_atomic(self);
    XR_DCHECK(a != NULL, "Atomic.fetchSub: NULL atomic");
    XR_DCHECK(nargs >= 1, "Atomic.fetchSub: missing delta argument");

    if (a->kind == XR_ATOMIC_INT) {
        XrAtomicOrdering ord = parse_ordering(args, nargs, 1);
        int64_t old = xr_atomic_fetch_sub(a, XR_TO_INT(args[0]), ord);
        return xr_int(old);
    }
    if (a->kind == XR_ATOMIC_FLOAT) {
        double delta = XR_TO_FLOAT(args[0]);
        int64_t cur = atomic_load(&a->value);
        for (;;) {
            double dval;
            memcpy(&dval, &cur, sizeof(dval));
            double old_val = dval;
            dval -= delta;
            int64_t desired;
            memcpy(&desired, &dval, sizeof(desired));
            if (atomic_compare_exchange_weak(&a->value, &cur, desired))
                return xr_float(old_val);
        }
    }
    return xr_null();
}

/* .swap(desired, ordering?) -> T (old value) */
static XrValue m_swap(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrAtomic *a = xr_value_to_atomic(self);
    XR_DCHECK(a != NULL, "Atomic.swap: NULL atomic");
    XR_DCHECK(nargs >= 1, "Atomic.swap: missing desired argument");

    int64_t packed = xr_atomic_pack(args[0], (XrAtomicKind) a->kind);
    XrAtomicOrdering ord = parse_ordering(args, nargs, 1);
    int64_t old = xr_atomic_swap(a, packed, ord);
    return xr_atomic_unpack(old, (XrAtomicKind) a->kind);
}

/* .compareExchange(expected, desired, ordering?) -> (T, bool) */
static XrValue m_compare_exchange(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    XrAtomic *a = xr_value_to_atomic(self);
    XR_DCHECK(a != NULL, "Atomic.compareExchange: NULL atomic");
    XR_DCHECK(nargs >= 2, "Atomic.compareExchange: need expected and desired");

    int64_t expected = xr_atomic_pack(args[0], (XrAtomicKind) a->kind);
    int64_t desired = xr_atomic_pack(args[1], (XrAtomicKind) a->kind);
    XrAtomicOrdering ord = parse_ordering(args, nargs, 2);

    int64_t prev = expected;
    bool ok = xr_atomic_compare_exchange(a, &prev, desired, ord);

    /* Return (prev_value, success) tuple */
    XrCoroutine *coro = xr_current_coro(isolate);
    XrTuple *tup = xr_tuple_new(coro, 2);
    if (tup) {
        xr_tuple_set(tup, 0, xr_atomic_unpack(prev, (XrAtomicKind) a->kind));
        xr_tuple_set(tup, 1, xr_bool(ok));
    }
    return xr_value_from_tuple(tup);
}

/* .toggle(ordering?) -> bool (old value) — bool only */
static XrValue m_toggle(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrAtomic *a = xr_value_to_atomic(self);
    XR_DCHECK(a != NULL, "Atomic.toggle: NULL atomic");

    XrAtomicOrdering ord = parse_ordering(args, nargs, 0);
    /* XOR 1 toggles the bool bit */
    int64_t old = atomic_fetch_xor_explicit(&a->value, 1, xr_to_c11_order(ord));
    return xr_bool(old != 0);
}

/* .toString() -> string */
static XrValue m_to_string(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    XrAtomic *a = xr_value_to_atomic(self);
    XR_DCHECK(a != NULL, "Atomic.toString: NULL atomic");

    int64_t raw = atomic_load(&a->value);
    XrValue inner = xr_atomic_unpack(raw, (XrAtomicKind) a->kind);
    XrString *s = xr_value_to_string(isolate, inner);
    return xr_string_value(s);
}

/* ========== Builtin Constructor ========== */

/* Atomic(initialValue) — called by the VM as static constructor */
static XrValue xr_builtin_atomic_construct(XrVMRuntime *isolate, XrValue receiver, XrValue *args,
                                           int nargs) {
    (void) receiver;
    XR_DCHECK(nargs >= 1, "Atomic constructor requires initial value");

    XrAtomicKind kind;
    int64_t initial;

    if (XR_IS_INT(args[0])) {
        kind = XR_ATOMIC_INT;
        initial = XR_TO_INT(args[0]);
    } else if (XR_IS_FLOAT(args[0])) {
        kind = XR_ATOMIC_FLOAT;
        double d = XR_TO_FLOAT(args[0]);
        memcpy(&initial, &d, sizeof(initial));
    } else if (XR_IS_BOOL(args[0])) {
        kind = XR_ATOMIC_BOOL;
        initial = XR_TO_BOOL(args[0]) ? 1 : 0;
    } else {
        /* Type error — Atomic only accepts int, float, bool */
        XrValue exc = xr_exception_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                        "Atomic requires int, float, or bool initial value");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }

    XrAtomic *a = xr_atomic_new(isolate, kind, initial);
    if (!a) {
        XrValue exc = xr_exception_newf(isolate, XR_ERR_OUT_OF_MEMORY, "Atomic allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_atomic(a);
}

/* ========== Native Type Registration ========== */

void xr_atomic_register_native_type(XrVMRuntime *isolate) {
    static const XrNativeMethod atomic_methods[] = {
        {"load", m_load, 0},
        {"store", m_store, 1},
        {"add", m_add, 1},
        {"sub", m_sub, 1},
        {"fetchAdd", m_fetch_add, 1},
        {"fetchSub", m_fetch_sub, 1},
        {"swap", m_swap, 1},
        {"compareExchange", m_compare_exchange, 2},
        {"toggle", m_toggle, 0},
        {"toString", m_to_string, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod atomic_statics[] = {
        {"call", xr_builtin_atomic_construct, 1},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo atomic_info = {
        .name = "Atomic",
        .gc_type = XR_TATOMIC,
        .methods = (XrNativeMethod *) atomic_methods,
        .getters = NULL,
        .static_methods = (XrNativeMethod *) atomic_statics,
    };
    xr_register_native_type(isolate, &atomic_info);
}
