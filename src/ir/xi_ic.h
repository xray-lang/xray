/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_ic.h - IC (inline cache) snapshot metadata for Xi IR
 *
 * KEY CONCEPT:
 *   Bridges VM-collected inline cache data into Xi IR so that
 *   speculative optimization passes (type narrow, spec inline)
 *   can query call-site type profiles.
 *
 *   The metadata is attached as a side table on XiFunc, keyed by
 *   value ID.  Only call-site ops (XI_CALL_METHOD,
 *   XI_CALL_METHOD_DIRECT, XI_LOAD_FIELD, XI_STORE_FIELD) carry
 *   meaningful IC entries.
 *
 * LIFECYCLE:
 *   xi_ic_attach() reads IC snapshots and populates func->ic_table.
 *   xi_ic_lookup() queries the table during optimization.
 *   xi_ic_table_free() releases the table (called by xi_func_free).
 *
 * INVARIANT:
 *   XI_INV_IC_ATTACHED is set after xi_ic_attach() completes.
 *   Speculative passes require this bit in their requires_inv_mask.
 */

#ifndef XI_IC_H
#define XI_IC_H

#include "xi.h"
#include "xi_ops_gen.h"
#include <stdint.h>
#include <stdbool.h>

/* Forward declarations for VM IC tables (avoid header coupling). */
struct XrICFieldTable;
struct XrICMethodTable;

/* IC classification for a call site. */
typedef enum XiIcKind {
    XI_IC_NONE = 0, /* no IC data available */
    XI_IC_MONO = 1, /* monomorphic: single observed type */
    XI_IC_POLY = 2, /* polymorphic: 2-4 observed types */
    XI_IC_MEGA = 3, /* megamorphic: too many types */
} XiIcKind;

/* Maximum targets tracked per call site (matches VM IC). */
#define XI_IC_MAX_TARGETS 4

/* Per-target entry in the IC snapshot. */
typedef struct XiIcTarget {
    uint32_t type_id;   /* observed type/class ID */
    uint32_t hit_count; /* number of hits for this target */
    uint32_t field_id;  /* resolved field/method symbol ID */
} XiIcTarget;

/* IC metadata attached to a single call-site value. */
typedef struct XiIcMeta {
    uint32_t value_id;    /* SSA value this entry belongs to */
    XiIcKind kind;        /* IC classification */
    uint32_t total_count; /* total invocations at this site */
    uint32_t ntargets;    /* number of valid target entries */
    XiIcTarget targets[XI_IC_MAX_TARGETS];
} XiIcMeta;

/* IC metadata side table: per-function, heap-allocated. */
typedef struct XiIcTable {
    XiIcMeta *entries;
    uint32_t nentries;
    uint32_t capacity;
} XiIcTable;

/* Attach IC snapshot data from VM profiling to Xi IR call sites.
 * Walks all call-site values in f and populates f->ic_table.
 * Sets XI_INV_IC_ATTACHED on f->invariant_mask.
 *
 * ic_fields / ic_methods may be NULL (AOT or cold path); in that
 * case the table is still created (empty) and the invariant set,
 * so downstream passes can safely query without null-checking.
 *
 * Returns true on success, false on allocation failure. */
XR_FUNC bool xi_ic_attach(XiFunc *f, struct XrICFieldTable *ic_fields,
                          struct XrICMethodTable *ic_methods);

/* Query IC metadata for a value.  Returns NULL if no entry exists. */
XR_FUNC const XiIcMeta *xi_ic_lookup(const XiFunc *f, uint32_t value_id);

/* Free the IC table.  Called by xi_func_free; safe to call on NULL. */
XR_FUNC void xi_ic_table_free(XiIcTable *table);

/* Query helpers for speculative passes. */
static inline uint8_t xi_ic_site_kind(uint16_t op) {
    return xi_generated_op_ic_site(op);
}

static inline bool xi_ic_is_mono(const XiIcMeta *meta) {
    return meta && meta->kind == XI_IC_MONO;
}
static inline bool xi_ic_is_poly(const XiIcMeta *meta) {
    return meta && meta->kind == XI_IC_POLY;
}
static inline bool xi_ic_is_mega(const XiIcMeta *meta) {
    return meta && meta->kind == XI_IC_MEGA;
}

#endif /* XI_IC_H */
