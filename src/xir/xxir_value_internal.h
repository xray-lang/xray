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
#include "xxir.h"
#include "../base/xchecks.h"
#include <stdatomic.h>

/* Storage generations are distinct from logical descriptor kinds. */
enum { XIR_OBJECT_CLASS = 257u,
       XIR_OBJECT_CELL = 258u,
       XIR_OBJECT_FUNCTION = 259u };
_Static_assert((uint32_t)XIR_OBJECT_CLASS > (uint32_t)XR_XIR_TYPE_TASK &&
    (uint32_t)XIR_OBJECT_CELL > (uint32_t)XR_XIR_TYPE_TASK && (uint32_t)XIR_OBJECT_FUNCTION > (uint32_t)XR_XIR_TYPE_TASK &&
    XIR_OBJECT_CLASS != 0x100u && XIR_OBJECT_CELL != 0x100u &&
    XIR_OBJECT_FUNCTION != 0x100u, "storage tags exclude retired generations");
_Static_assert(XIR_OBJECT_CLASS != XIR_OBJECT_CELL &&
    XIR_OBJECT_CLASS != XIR_OBJECT_FUNCTION && XIR_OBJECT_CELL != XIR_OBJECT_FUNCTION,
    "storage tags are distinct");

typedef struct XirObject {
    _Atomic(uint32_t) references;
    XrXirDomain *domain;
    XrXirTypeArena *arena;
    XrXirType type;
    uint32_t kind;
    struct XirObject *release_next;
    struct XirObject *graph_previous, *graph_next, *graph_work;
    uint32_t graph_trial;
    bool graph_registered, graph_live, graph_dead;
} XirObject;
/* Operations exclude retirement snapshots without holding the graph lock.
 * Publication requires a complete object and an active operation lease. */
XR_FUNC void xr_xir_value_graph_begin(void);
XR_FUNC void xr_xir_value_graph_end(void);
XR_FUNC void xr_xir_value_object_publish(XirObject *object);
/* The caller retains its ordinary domain lease for the duration of close. */
XR_FUNC void xr_xir_domain_close(XrXirDomain *domain);
/* Empty enum descriptors belong to the arena; their backpointers are weak.
 * Each external value holds an arena lease instead of a descriptor reference. */
typedef struct XirNominalValue {
    XirObject object;
    uint32_t count, variant;
    XrXirValue *fields;
} XirNominalValue;
typedef struct XirTuple {
    XirObject object;
    uint32_t count;
    XrXirValue *fields;
} XirTuple;
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
typedef struct XrXirDomainBudgetStats {
    uint64_t requested_bytes, requested_limit, work, work_limit;
    bool bound;
    uint64_t requested_call_bytes, requested_call_limit;
    uint64_t metadata_live, metadata_peak, metadata_limit, metadata_allocations, metadata_frees;
    uint64_t call_live, call_peak, call_limit, call_allocations, call_frees;
} XrXirDomainBudgetStats;
typedef struct XrXirDomainBudgetControls {
    uint64_t requested_value_limit, requested_call_limit, work_limit;
    uint64_t metadata_limit, call_limit;
} XrXirDomainBudgetControls;
/* Binding is a single lifetime epoch; terminal storage still spends this budget. */
XR_FUNC XrXirValueStatus xr_xir_domain_budget_bind(XrXirDomain *domain, const XrXirDomainBudgetControls *controls);
/* The body request is charged before the first allocator; metadata and calls
 * share cumulative requests but retain independent live allocation limits. */
XR_FUNC XrXirValueStatus xr_xir_domain_new_budgeted(uint64_t value_limit,
    const XrXirDomainBudgetControls *controls, XrXirDomain **output);
XR_FUNC void *xr_xir_domain_metadata_allocate(XrXirDomain *domain, uint64_t bytes, XrXirValueStatus *status);
XR_FUNC void xr_xir_domain_metadata_deallocate(XrXirDomain *domain, void *memory, uint64_t bytes);
XR_FUNC void *xr_xir_domain_call_allocate(XrXirDomain *domain, uint64_t bytes, XrXirValueStatus *status);
XR_FUNC void xr_xir_domain_call_deallocate(XrXirDomain *domain, void *memory, uint64_t bytes);
XR_FUNC bool xr_xir_domain_work(XrXirDomain *domain, uint64_t work);
XR_FUNC XrXirDomainBudgetStats xr_xir_domain_budget_stats(XrXirDomain *domain);
/* Only the real Instance producer path supplies this private source identity.
 * The binding gate pins the Program and its immutable certificate. Public
 * construction and weakening do not copy this identity into another object. */
typedef struct XirFunctionProducer {
    const void *owner;
    uint32_t function, instruction, site;
} XirFunctionProducer;
typedef struct XirFunctionConstruction {
    const XrXirFunctionBinding *binding;
    XrXirValueAdmission *admission;
    const XirFunctionProducer *producer;
} XirFunctionConstruction;
XR_FUNC XrXirValueStatus xr_xir_function_new_produced(XrXirDomain *domain,XrXirTypeArena *arena,
    XrXirType type,const XirFunctionConstruction *construction,XrXirValue *output);
XR_FUNC const XirFunctionProducer *xr_xir_function_producer(const XrXirValue *value);
#endif // XXIR_VALUE_INTERNAL_H
