/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_bundle.h - AOT sidecar bundle plan
 */

#ifndef XAOT_BUNDLE_H
#define XAOT_BUNDLE_H

#include "xaot_abi.h"
#include "xaot_container.h"
#include "../ir/xi_module.h"
#include "../base/xdefs.h"
#include <stdint.h>

typedef struct XaotFuncPlan {
    XiFunc *func;
    uint32_t module_index;
    uint16_t depth;
    XaotFuncAbi abi;
} XaotFuncPlan;

typedef struct XaotValuePlan {
    const XiFunc *func;
    const XiValue *value;
    XaotValueRep rep;
} XaotValuePlan;

typedef struct XaotContainerTypePlan {
    XaotContainerPlan plan;
} XaotContainerTypePlan;

enum {
    XAOT_ARRAY_STORAGE_READ = 1u << 0,
    XAOT_ARRAY_STORAGE_MUTABLE = 1u << 1,
};

typedef struct XaotArrayStoragePlan {
    const XiFunc *func;
    const XiValue *value;
    const XiValue *origin;
    uint32_t flags;
    XaotContainerElemPlan elem;
} XaotArrayStoragePlan;

enum {
    XAOT_ARRAY_CACHE_READ = 1u << 0,
    XAOT_ARRAY_CACHE_MUTABLE = 1u << 1,
    XAOT_ARRAY_CACHE_DECLARE_LOCAL = 1u << 2,
    XAOT_ARRAY_CACHE_FRESH_RESULT = 1u << 3,
    XAOT_ARRAY_CACHE_VIEW = 1u << 4,
    XAOT_ARRAY_CACHE_FILL_LOOP = 1u << 5,
    XAOT_ARRAY_CACHE_NATIVE_LOCAL = 1u << 6,
    XAOT_ARRAY_CACHE_CLASS_FIELD = 1u << 7,
};

typedef struct XaotArrayCachePlan {
    const XiFunc *func;
    const XiValue *value;
    const XiValue *storage_value;
    uint32_t flags;
    XaotContainerElemPlan elem;
} XaotArrayCachePlan;

typedef struct XaotArrayClassFieldAllocPlan {
    const XiFunc *func;
    const XiValue *origin;
    const XiValue *store;
    const XiClassData *class_data;
    uint16_t field_idx;
    XaotContainerElemPlan elem;
} XaotArrayClassFieldAllocPlan;

/* Function attribute plan: prepare proves a function free of observable
 * effects and Cgen emits the matching C attribute so the host compiler can
 * CSE / LICM across call sites. CONST = touches no memory at all;
 * PURE = reads memory but never writes / throws / suspends. Mutually
 * exclusive; evidence is the per-value effect flags re-checked by the
 * verifier. */
enum {
    XAOT_FN_ATTR_CONST = 1u << 0, /* __attribute__((const)) */
    XAOT_FN_ATTR_PURE = 1u << 1,  /* __attribute__((pure)) */
};

typedef struct XaotFuncAttrPlan {
    const XiFunc *func;
    uint32_t flags;
} XaotFuncAttrPlan;

/* Bounds plan: prepare proves an XI_INDEX_GET / XI_INDEX_SET in-bounds so
 * Cgen can emit raw element access without runtime checks. The proof is
 * computed centrally here (not pattern-matched in the emitter) so the
 * verifier can re-derive it and the dump can audit which accesses went
 * unchecked and why. Evidence bits record which rule fired; accesses that
 * stay checked are recorded with evidence == 0 and an unproven reason so
 * the dump exposes the audit budget for bounds work. */
enum {
    /* Every predecessor is the true edge of `if (idx < arr.length)` and the
     * length load is not clobbered between the guard and the access. */
    XAOT_BOUNDS_EV_DOM_GUARD = 1u << 0,
    /* Single-block counted loop: `while (i < arr.length) { arr[i] = ...; i += 1 }`
     * with a zero start, +1 step, and a checked entry edge. */
    XAOT_BOUNDS_EV_COUNTED_LOOP = 1u << 1,
    /* Index proven >= 0 (range analysis or structural induction). */
    XAOT_BOUNDS_EV_NONNEG_INDEX = 1u << 2,
};

/* Why an access stayed checked. Ordered by specificity: when both proof
 * paths fail, the most specific reason is recorded. */
enum {
    XAOT_BOUNDS_UNPROVEN_NONE = 0,         /* proven (evidence != 0) */
    XAOT_BOUNDS_UNPROVEN_NO_GUARD = 1,     /* no dominating idx < len test found */
    XAOT_BOUNDS_UNPROVEN_INDEX_RANGE = 2,  /* index not proven >= 0 */
    XAOT_BOUNDS_UNPROVEN_LEN_MISMATCH = 3, /* guard bound is not this array's length */
    XAOT_BOUNDS_UNPROVEN_CLOBBER = 4,      /* effectful op between guard / length and access */
};

typedef struct XaotBoundsPlan {
    const XiFunc *func;
    const XiValue *access;   /* XI_INDEX_GET / XI_INDEX_SET */
    uint32_t evidence;       /* XAOT_BOUNDS_EV_*; 0 = stays checked */
    uint8_t unproven_reason; /* XAOT_BOUNDS_UNPROVEN_*; 0 = proven */
} XaotBoundsPlan;

/* Alias plan: prepare proves a pointer unique over its storage so Cgen can
 * emit `restrict` and the C compiler gets Rust-noalias-grade information.
 * A wrong restrict is undefined behaviour, so each plan carries the full
 * evidence chain, the verifier re-derives it, and XRAY_AOT_NO_RESTRICT=1
 * suppresses plan creation for regression bisection. */
typedef enum XaotAliasKind {
    XAOT_ALIAS_UNIQUE_DATA = 1,  /* typed array data cache (_adN): the only
                                  * element-storage pointer in the function */
    XAOT_ALIAS_UNIQUE_RECV = 2,  /* native receiver unaliased in the method */
    XAOT_ALIAS_UNIQUE_PARAM = 3, /* pointer param provably unaliased */
} XaotAliasKind;

enum {
    /* Backing storage allocated by this function (array literal / Bytes /
     * with_capacity — a fresh malloc nothing else can point at yet). */
    XAOT_ALIAS_EV_FRESH_ALLOC = 1u << 0,
    /* Every XI_INDEX_GET / XI_INDEX_SET on the array has a proven bounds
     * plan, so every element access is emitted through the _adN cache and
     * never through a checked ->data slow path. */
    XAOT_ALIAS_EV_ALL_ACCESS_RAW = 1u << 1,
    /* Every other use is alias-free: unique fill push, length/size reads,
     * retain/release. No call, store, capture, or phi participation. */
    XAOT_ALIAS_EV_USE_WHITELIST = 1u << 2,
    /* No second array cache plan over the same backing in the function. */
    XAOT_ALIAS_EV_SOLE_CACHE = 1u << 3,
};

typedef struct XaotAliasPlan {
    const XiFunc *func;
    const XiValue *value; /* cache origin value (the _adN owner) */
    uint8_t kind;         /* XaotAliasKind */
    uint32_t evidence;    /* XAOT_ALIAS_EV_* */
} XaotAliasPlan;

typedef struct XaotPrepareStats {
    uint32_t functions_total;
    uint32_t functions_native_abi;
    uint32_t functions_tagged_abi;
    uint32_t functions_coro_abi;
    uint32_t values_total;
    uint32_t values_scalar;
    uint32_t values_tagged;
    uint32_t values_ptr;
    uint32_t values_aggregate;
    uint32_t values_view;
    uint32_t values_void;
    uint32_t boundary_count;
    uint32_t containers_total;
    uint32_t containers_array;
    uint32_t containers_map;
    uint32_t containers_set;
    uint32_t containers_direct;
    uint32_t array_storage_total;
    uint32_t array_storage_read;
    uint32_t array_storage_mutable;
    uint32_t array_cache_total;
    uint32_t array_cache_read;
    uint32_t array_cache_mutable;
} XaotPrepareStats;

typedef struct XaotBundle {
    XiModule **modules;
    uint32_t nmodules;
    uint32_t entry_module;
    XaotFuncPlan *func_plans;
    uint32_t nfunc_plans;
    uint32_t func_plan_cap;
    XaotValuePlan *value_plans;
    uint32_t nvalue_plans;
    uint32_t value_plan_cap;
    XaotContainerTypePlan *container_plans;
    uint32_t ncontainer_plans;
    uint32_t container_plan_cap;
    XaotArrayStoragePlan *array_storage_plans;
    uint32_t narray_storage_plans;
    uint32_t array_storage_plan_cap;
    XaotArrayCachePlan *array_cache_plans;
    uint32_t narray_cache_plans;
    uint32_t array_cache_plan_cap;
    XaotArrayClassFieldAllocPlan *array_class_field_alloc_plans;
    uint32_t narray_class_field_alloc_plans;
    uint32_t array_class_field_alloc_plan_cap;
    XaotFuncAttrPlan *func_attr_plans;
    uint32_t nfunc_attr_plans;
    uint32_t func_attr_plan_cap;
    XaotBoundsPlan *bounds_plans;
    uint32_t nbounds_plans;
    uint32_t bounds_plan_cap;
    XaotAliasPlan *alias_plans;
    uint32_t nalias_plans;
    uint32_t alias_plan_cap;
    XaotBoundaryStep *boundary_steps;
    uint32_t nboundary_steps;
    uint32_t boundary_step_cap;
    XaotPrepareStats stats;
    const char *error_msg;
} XaotBundle;

XR_FUNC bool xaot_bundle_init(XaotBundle *bundle, XiModule **modules, uint32_t nmodules,
                              uint32_t entry_module);
XR_FUNC void xaot_bundle_free(XaotBundle *bundle);
XR_FUNC XaotFuncPlan *xaot_bundle_add_func_plan(XaotBundle *bundle, XiFunc *func,
                                                uint32_t module_index, uint16_t depth);
XR_FUNC const XaotFuncPlan *xaot_bundle_find_func_plan(const XaotBundle *bundle,
                                                       const XiFunc *func);
XR_FUNC XaotValuePlan *xaot_bundle_add_value_plan(XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *value);
XR_FUNC const XaotValuePlan *xaot_bundle_find_value_plan(const XaotBundle *bundle,
                                                         const XiValue *value);
XR_FUNC XaotContainerTypePlan *xaot_bundle_add_container_plan(XaotBundle *bundle,
                                                              const XrType *type);
XR_FUNC const XaotContainerTypePlan *xaot_bundle_find_container_plan(const XaotBundle *bundle,
                                                                     const XrType *type);
XR_FUNC XaotArrayStoragePlan *
xaot_bundle_add_array_storage_plan(XaotBundle *bundle, const XiFunc *func, const XiValue *value,
                                   const XiValue *origin, uint32_t flags,
                                   const XaotContainerElemPlan *elem);
XR_FUNC const XaotArrayStoragePlan *xaot_bundle_find_array_storage_plan(const XaotBundle *bundle,
                                                                        const XiValue *value);
XR_FUNC XaotArrayCachePlan *xaot_bundle_add_array_cache_plan(XaotBundle *bundle, const XiFunc *func,
                                                             const XiValue *value,
                                                             const XiValue *storage_value,
                                                             uint32_t flags,
                                                             const XaotContainerElemPlan *elem);
XR_FUNC const XaotArrayCachePlan *xaot_bundle_find_array_cache_plan(const XaotBundle *bundle,
                                                                    const XiValue *value);
XR_FUNC XaotArrayClassFieldAllocPlan *xaot_bundle_add_array_class_field_alloc_plan(
    XaotBundle *bundle, const XiFunc *func, const XiValue *origin, const XiValue *store,
    const XiClassData *class_data, uint16_t field_idx, const XaotContainerElemPlan *elem);
XR_FUNC const XaotArrayClassFieldAllocPlan *
xaot_bundle_find_array_class_field_alloc_plan(const XaotBundle *bundle, const XiValue *origin);
XR_FUNC const XaotArrayClassFieldAllocPlan *
xaot_bundle_find_array_class_field_alloc_plan_for_store(const XaotBundle *bundle,
                                                        const XiValue *store);
XR_FUNC const XaotArrayClassFieldAllocPlan *xaot_bundle_find_array_class_field_alloc_plan_for_field(
    const XaotBundle *bundle, const XiFunc *func, const XiClassData *class_data,
    uint16_t field_idx);
XR_FUNC XaotFuncAttrPlan *xaot_bundle_add_func_attr_plan(XaotBundle *bundle, const XiFunc *func,
                                                         uint32_t flags);
XR_FUNC const XaotFuncAttrPlan *xaot_bundle_find_func_attr_plan(const XaotBundle *bundle,
                                                                const XiFunc *func);
XR_FUNC XaotBoundsPlan *xaot_bundle_add_bounds_plan(XaotBundle *bundle, const XiFunc *func,
                                                    const XiValue *access, uint32_t evidence,
                                                    uint8_t unproven_reason);
XR_FUNC const XaotBoundsPlan *xaot_bundle_find_bounds_plan(const XaotBundle *bundle,
                                                           const XiValue *access);
XR_FUNC XaotAliasPlan *xaot_bundle_add_alias_plan(XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *value, uint8_t kind,
                                                  uint32_t evidence);
XR_FUNC const XaotAliasPlan *xaot_bundle_find_alias_plan(const XaotBundle *bundle,
                                                         const XiValue *value);
XR_FUNC XaotBoundaryStep *xaot_bundle_add_boundary_step(XaotBundle *bundle,
                                                        XaotBoundaryStepKind kind,
                                                        const XiFunc *func, const XiValue *value,
                                                        const XiValue *input,
                                                        XaotBoundaryReason reason);
XR_FUNC const XaotBoundaryStep *
xaot_bundle_find_boundary_step(const XaotBundle *bundle, XaotBoundaryStepKind kind,
                               const XiFunc *func, const XiValue *value, const XiValue *input);
XR_FUNC const XaotBoundaryStep *
xaot_bundle_find_boundary_step_ex(const XaotBundle *bundle, XaotBoundaryStepKind kind,
                                  const XiFunc *func, const XiValue *value, const XiValue *input,
                                  const XiFunc *target_func, uint16_t arg_index);
XR_FUNC char *xaot_bundle_dump_plan(const XaotBundle *bundle);

#endif  // XAOT_BUNDLE_H
