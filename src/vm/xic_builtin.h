/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xic_builtin.h - Inline cache for OP_INVOKE_BUILTIN dispatch.
 *
 * KEY CONCEPT:
 *   OP_INVOKE_BUILTIN walks an if/else chain over receiver types
 *   (map / array / string / set / json / int / float / bool / bigint
 *   / ...) before reaching xr_method_table_lookup. Each call site
 *   is overwhelmingly monomorphic in real workloads (a particular
 *   .push site only ever pushes onto arrays). The IC caches the
 *   pair (XrTypeId, XrMethodSlot*) and short-circuits the chain
 *   on subsequent hits.
 *
 *   The cache is keyed by PC (cache_index = pc - PROTO_CODE_BASE),
 *   so the bytecode operand `B` (method_symbol) is implicitly
 *   constant per cache slot — no need to store it.
 *
 *   Sticky first-write-wins: on a hit, slot->fn is called directly.
 *   On a type mismatch, the cache is left alone and the slow path
 *   runs; we count misses for tuning but do not thrash. JIT/AOT
 *   later consume the same feedback to specialize on the dominant
 *   type.
 */

#ifndef XRAY_XIC_BUILTIN_H
#define XRAY_XIC_BUILTIN_H

#include "../runtime/value/xmethod_table.h"
#include "../runtime/value/xtype_names.h"
#include "../base/xdefs.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Per-call-site IC entry ========== */

/*
 * Layout is 16 bytes on 64-bit:
 *   slot       - 8 bytes (NULL means empty)
 *   cached_tid - 2 bytes (XrTypeId widened to int16_t; -1 unused
 *                so XR_TID_NULL = 0 is the legitimate "uninit"
 *                sentinel paired with slot == NULL)
 *   hits       - 2 bytes (saturates; only used for stats)
 *   misses     - 2 bytes (saturates; only used for stats)
 *   _reserved  - 2 bytes (keep struct 16-byte aligned for fast load)
 */
typedef struct XrICBuiltin {
    const XrMethodSlot *slot;
    int16_t  cached_tid;
    uint16_t hits;
    uint16_t misses;
    uint16_t _reserved;
} XrICBuiltin;

/* ========== Per-proto IC table ========== */

typedef struct XrICBuiltinTable {
    XrICBuiltin *caches;
    int count;     /* total slots in use; equals PROTO_CODE_COUNT(proto) */
    int capacity;  /* underlying storage capacity */
} XrICBuiltinTable;

/* ========== Construction / teardown ========== */

XR_FUNC XrICBuiltinTable *xr_ic_builtin_table_new(int initial_capacity);
XR_FUNC void              xr_ic_builtin_table_free(XrICBuiltinTable *table);

/*
 * Returns the index of a freshly allocated, zero-initialized slot,
 * or -1 on OOM. The caller never reorders slots, so the index is
 * stable for the lifetime of the table.
 */
XR_FUNC int xr_ic_builtin_table_alloc(XrICBuiltinTable *table);

static inline XrICBuiltin *xr_ic_builtin_table_get(XrICBuiltinTable *table,
                                                   int index) {
    if (!table || index < 0 || index >= table->count) return NULL;
    return &table->caches[index];
}

#ifdef __cplusplus
}
#endif

#endif /* XRAY_XIC_BUILTIN_H */
