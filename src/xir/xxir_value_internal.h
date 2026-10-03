/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_internal.h - Shared physical allocation and atomic ownership primitives
 *
 * KEY CONCEPT:
 *   Every allocation pins its accounting domain until its physical release.
 */
#ifndef XXIR_VALUE_INTERNAL_H
#define XXIR_VALUE_INTERNAL_H
#include "xxir_value.h"
#include "../base/xchecks.h"
#include <stdatomic.h>

typedef struct XirObject {
    _Atomic(uint32_t) references;
    XrXirDomain *domain;
    XrXirTypeArena *arena;
    XrXirType type;
    uint32_t kind;
    struct XirObject *release_next;
} XirObject;
/* Empty enum descriptors belong to the arena; their backpointers are weak.
 * Each external value holds an arena lease instead of a descriptor reference. */
typedef struct XirNominalValue {
    XirObject object;
    uint32_t count, variant;
    XrXirValue *fields;
} XirNominalValue;
XR_FUNC const XirNominalValue *xr_xir_compile_type_arena_empty_variant(
    const XrXirTypeArena *arena, XrXirType type, uint32_t variant);

static inline bool xr_xir_reference_retain(_Atomic(uint32_t) *references) {
    uint32_t count = atomic_load_explicit(references, memory_order_relaxed);
    for (;;) {
        if (!count || count == UINT32_MAX) return false;
        if (atomic_compare_exchange_weak_explicit(references, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
}
static inline bool xr_xir_reference_release(_Atomic(uint32_t) *references) {
    uint32_t count = atomic_load_explicit(references, memory_order_relaxed);
    for (;;) {
        XR_CHECK(count, "reference count underflow");
        if (atomic_compare_exchange_weak_explicit(references, &count, count - 1,
                memory_order_acq_rel, memory_order_relaxed)) return count == 1;
    }
}
XR_FUNC bool xr_xir_domain_retain(XrXirDomain *domain);
XR_FUNC void *xr_xir_domain_allocate(XrXirDomain *domain, size_t bytes, XrXirValueStatus *status);
XR_FUNC void xr_xir_domain_deallocate(XrXirDomain *domain, void *memory, size_t bytes);
#endif // XXIR_VALUE_INTERNAL_H
