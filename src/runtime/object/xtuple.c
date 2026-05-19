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

#include <stdio.h>
#include <string.h>

#include "../value/xvalue.h"
#include "../value/xvalue_hash.h"
#include "../gc/xgc.h"
#include "../gc/xcoro_gc.h"
#include "../class/xclass.h"
#include "../class/xclass_builder.h"
#include "../class/xclass_system.h"
#include "../class/xinstance.h"
#include "../xisolate_api.h"
#include "../../base/xchecks.h"
#include "../../base/xdefs.h"
#include "../../base/xhash.h"
#include "../../coro/xcoroutine.h"

/* ========== Class Construction ========== */

/* Build a tuple class for the given arity. Each class declares N
 * untyped fields named "0".."N-1" so field IO uses the same field
 * lookup as regular user classes (digit symbols are well-defined). */
static XrClass *build_tuple_class(XrayIsolate *X, uint16_t arity) {
    XR_DCHECK(X != NULL, "build_tuple_class: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XR_DCHECK(core != NULL && core->objectClass != NULL,
              "build_tuple_class: core / Object not ready");

    char name[32];
    snprintf(name, sizeof(name), "Tuple%u", (unsigned) arity);

    XrClassBuilder *builder = xr_class_builder_new(X, name, core->objectClass);
    XR_CHECK(builder != NULL, "build_tuple_class: builder alloc failed");

    char field_name[8];
    for (uint16_t i = 0; i < arity; i++) {
        snprintf(field_name, sizeof(field_name), "%u", (unsigned) i);
        xr_class_builder_add_field(builder, field_name, 0);
    }

    XrClass *cls = xr_class_builder_finalize(builder);
    XR_CHECK(cls != NULL, "build_tuple_class: finalize failed");
    cls->flags |= XR_CLASS_BUILTIN | XR_CLASS_TUPLE | XR_CLASS_FINAL;
    return cls;
}

XrClass *xr_get_or_create_tuple_class(XrayIsolate *X, uint16_t arity) {
    XR_DCHECK(X != NULL, "tuple_class: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XR_DCHECK(core != NULL, "tuple_class: core not initialised");

    if (arity < XR_TUPLE_CLASS_PREALLOC) {
        if (!core->tupleClassesSmall[arity]) {
            core->tupleClassesSmall[arity] = build_tuple_class(X, arity);
        }
        return core->tupleClassesSmall[arity];
    }
    /* Cold path: arities >= XR_TUPLE_CLASS_PREALLOC are rare in real
     * source. Build a fresh class every time — caching them would
     * require a hash table that buys nothing here. */
    return build_tuple_class(X, arity);
}

/* ========== Allocation ========== */

XrTuple *xr_tuple_new(struct XrCoroutine *coro, uint16_t element_count) {
    XR_DCHECK(coro != NULL, "xr_tuple_new: NULL coro");
    XrayIsolate *X = coro->isolate;
    XR_DCHECK(X != NULL, "xr_tuple_new: coro->isolate is NULL");

    XrClass *cls = xr_get_or_create_tuple_class(X, element_count);
    if (!cls)
        return NULL;

    /* Allocate the instance bytes on the coroutine heap. We bypass
     * xr_instance_new on purpose: it would memcpy field defaults from
     * the class, but tuple slots are caller-initialised right after
     * this returns. Pre-fill with null only so a half-built tuple
     * is still safe for the GC to traverse before xr_tuple_set runs. */
    size_t size = xr_instance_size(cls);
    XrTuple *t = (XrTuple *) xr_alloc(coro, size, XR_TINSTANCE);
    if (!t)
        return NULL;

    xr_gc_header_init_type(&t->gc, XR_TINSTANCE);
    t->klass = cls;

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
    if (!t || index >= xr_tuple_arity(t)) {
        XrValue null_v = {0};
        return null_v;
    }
    return t->elements[index];
}

void xr_tuple_set(XrTuple *t, uint16_t index, XrValue value) {
    XR_DCHECK(t != NULL, "xr_tuple_set: NULL tuple");
    uint16_t arity = xr_tuple_arity(t);
    XR_DCHECK(index < arity, "xr_tuple_set: index out of range");
    if (!t || index >= arity)
        return;
    t->elements[index] = value;
}

/* ========== Type Check ========== */

bool xr_value_is_tuple(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass != NULL && (inst->klass->flags & XR_CLASS_TUPLE) != 0;
}

/* ========== Structural Equality ========== */

/* Recursive equality so nested tuples compare element-wise rather
 * than by heap pointer. Falls back to xr_value_eq for non-tuple
 * elements (primitives, strings, and reference types). */
static bool tuple_elem_eq(XrValue a, XrValue b) {
    if (xr_value_is_tuple(a) && xr_value_is_tuple(b))
        return xr_tuple_equals((XrTuple *) XR_TO_PTR(a), (XrTuple *) XR_TO_PTR(b));
    return xr_value_eq(a, b);
}

bool xr_tuple_equals(XrTuple *a, XrTuple *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    uint16_t na = xr_tuple_arity(a);
    uint16_t nb = xr_tuple_arity(b);
    if (na != nb)
        return false;
    for (uint16_t i = 0; i < na; i++) {
        if (!tuple_elem_eq(a->elements[i], b->elements[i]))
            return false;
    }
    return true;
}

/* ========== Hash ========== */

/*
 * 64-bit FNV-1a fold over the per-element XrValue hashes, with the
 * arity mixed in first so `()` and `(0,)` cannot collide. Constants
 * come from xhash.h so the tuple hash stays bit-identical with every
 * other FNV-based hash in the codebase. Returned as 64-bit; callers
 * that only need 32 bits can truncate.
 */
uint64_t xr_tuple_hash(XrTuple *t) {
    if (!t)
        return 0;
    uint16_t arity = xr_tuple_arity(t);
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    /* Mix the arity byte by byte so the whole 16 bits actually steer
     * the hash state, instead of being reduced to a single XOR. */
    for (int i = 0; i < (int) sizeof(arity); i++) {
        h ^= (uint64_t) ((arity >> (i * 8)) & 0xFF);
        h *= XR_FNV64_PRIME;
    }
    /* xr_hash_value returns 32-bit, so likewise fold each byte. */
    for (uint16_t i = 0; i < arity; i++) {
        uint32_t eh = xr_hash_value(t->elements[i]);
        for (int b = 0; b < 4; b++) {
            h ^= (uint64_t) ((eh >> (b * 8)) & 0xFF);
            h *= XR_FNV64_PRIME;
        }
    }
    return h;
}
