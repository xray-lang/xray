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
#include "xxir_class_observation.h"
#include "xxir.h"
#include "../base/xchecks.h"
#include <stdatomic.h>

/* Storage generations are distinct from logical descriptor kinds. */
enum { XIR_OBJECT_BUFFER_STORAGE = 261u,
       XIR_OBJECT_CLASS = 260u,
       XIR_OBJECT_CELL = 258u,
       XIR_OBJECT_FUNCTION = 259u };
_Static_assert((uint32_t)XIR_OBJECT_CLASS > (uint32_t)XR_XIR_TYPE_CHANNEL &&
    (uint32_t)XIR_OBJECT_CELL > (uint32_t)XR_XIR_TYPE_CHANNEL && (uint32_t)XIR_OBJECT_FUNCTION > (uint32_t)XR_XIR_TYPE_CHANNEL &&
    XIR_OBJECT_CLASS != 0x100u && XIR_OBJECT_CLASS != 257u && XIR_OBJECT_CELL != 0x100u &&
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
    /* Only root Strings set this: one block holds header and body, and the
     * object never enters the residual graph (see xr_xir_root_domain_new). */
    bool root_private;
} XirObject;
_Static_assert(sizeof(XirObject) == 72, "the root flag occupies existing object header padding");
/* The canonical String carrier. The body holds length bytes and one NUL;
 * runes caches the Unicode scalar count. Holders read it without a lease. */
typedef struct XirString {
    XirObject object;
    char *bytes;
    size_t length, runes, capacity;
} XirString;
/* Operations exclude retirement snapshots without holding the graph lock.
 * Publication requires a complete object and an active operation lease. */
XR_FUNC void xr_xir_value_graph_begin(void);
XR_FUNC void xr_xir_value_graph_end(void);
XR_FUNC void xr_xir_value_object_publish(XirObject *object);
/* The caller retains its ordinary domain lease for the duration of close.
 * A provider failure preserves the remaining page ledger and permits retry.
 * Only success closes execution ownership; reference drops do not close pages. */
XR_FUNC XrXirValueStatus xr_xir_domain_close(XrXirDomain *domain);
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
typedef struct XirFixedArray {
    XirObject object;
    uint32_t length;
    uint8_t *bytes;
} XirFixedArray;
typedef struct XirHash {
    XirObject object;
    XrXirType key_type, value_type;
    size_t length, capacity;
    uint32_t key_offset, value_offset, stride, release_capacity;
    unsigned char *data;
} XirHash;
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
/* Reads only the immutable eight-byte prefix, before current storage access. */
XR_FUNC bool xr_xir_domain_current(const XrXirDomain *domain);
/* Only the compiler constructor supplies this immutable memory-owner mode.
 * It cannot bind execution policy or allocate execution metadata/call state. */
XR_FUNC bool xr_xir_domain_is_compile(const XrXirDomain *domain);
XR_FUNC bool xr_xir_domain_retain(XrXirDomain *domain);
/* Borrowed inputs remain unchanged. Only a canonical Unit output receives an
 * owned result, allocated in domain even when either input belongs elsewhere.
 * Failure preserves all handles; no frame, call view or execution is required. */
XR_FUNC XrXirValueStatus xr_xir_string_concat(XrXirDomain *domain,
    const XrXirValue *left, const XrXirValue *right, XrXirValue *output);
/* A root domain backs one native execution root. It is never closed or
 * budget-bound: closing it aborts and binding it fails. Strings own no edges,
 * so no residual graph retirement can reach a String in an unclosed domain;
 * root Strings therefore skip graph publication and their leases. */
XR_FUNC XrXirValueStatus xr_xir_root_domain_new(uint64_t limit, XrXirDomain **output);
/* Publish one complete root String owning one reference, allocated as a
 * single block in a root domain. Borrowed inputs are live canonical Strings.
 * Failure owns nothing and leaves *output NULL. */
XR_FUNC XrXirValueStatus xr_xir_root_string_new(XrXirDomain *domain, const char *bytes,
    size_t length, XirString **output);
XR_FUNC XrXirValueStatus xr_xir_root_string_concat(XrXirDomain *domain,
    const XirString *left, const XirString *right, XirString **output);
// The canonical signed decimal text, without a scan of its ASCII bytes.
XR_FUNC XrXirValueStatus xr_xir_root_string_i64(XrXirDomain *domain, int64_t value, XirString **output);
/* The root ends: every unescaped String is gone. Frees cached blocks and
 * drops the root's domain reference; escaped Strings keep the domain. */
XR_FUNC void xr_xir_root_domain_release(XrXirDomain *domain);
/* A root String that leaves its root pins its domain first; any generic
 * retain does this implicitly. Fails only at the domain reference limit. */
XR_FUNC XrXirValueStatus xr_xir_root_string_escape(XirString *string);
/* Consume one reference to a live String. An unescaped root String is freed
 * at once, an escaped one after one atomic decrement, and neither takes a
 * graph operation; other Strings use the ordinary drop. */
XR_FUNC void xr_xir_string_release(XirString *string);
/* Only the actual NEW Instance calls this before any value publication. */
XR_FUNC XrXirValueStatus xr_xir_domain_class_observation_open(XrXirDomain *domain,
    uint32_t capacity, XrXirClassObservation **output);
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
#endif // XXIR_VALUE_INTERNAL_H
