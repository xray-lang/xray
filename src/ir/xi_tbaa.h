/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_tbaa.h - Type-Based Alias Analysis for Xi IR
 *
 * KEY CONCEPT:
 *   A simple lattice that classifies every memory-accessing Xi op into
 *   a "memory group".  Two ops whose groups are disjoint in the lattice
 *   are guaranteed not to alias, enabling GVN-PRE, LICM, and other
 *   memory optimisations to reorder or eliminate redundant loads.
 *
 * LATTICE:
 *
 *   top (any memory)
 *     +-- const      immutable data (string bytes, class descriptor)
 *     +-- field      object field access
 *     |     +-- field.<id>    per-field-id disjoint (when id is known)
 *     +-- array      array element access
 *     +-- struct     struct field access (typed, known layout)
 *     +-- shared     module-level shared variable array
 *     +-- global     name-keyed global dict (REPL path)
 *     +-- upval      closure upvalue slot
 *     +-- tls        thread-local / per-isolate state
 *
 *   Two values with the same group MAY alias; different groups NEVER alias
 *   (unless one is XI_MEM_TOP, which may alias everything).
 *
 * USAGE:
 *   - xi_lower fills XiValue.mem_group for every load/store op
 *   - xi_tbaa_may_alias() is the sole query interface
 *   - Passes must never bypass the query or hard-code alias assumptions
 *
 * INVARIANTS:
 *   - After the TBAA annotation pass, XI_INV_TBAA_ANNOTATED is set
 *   - Every load/store op has mem_group != XI_MEM_NONE
 *   - Non-memory ops always have mem_group == XI_MEM_NONE
 */

#ifndef XI_TBAA_H
#define XI_TBAA_H

#include "xi.h"
#include "xi_pass.h"
#include "../base/xdefs.h"
#include <stdbool.h>

/* ========== Memory Group Enum ==========
 *
 * Encodes the TBAA lattice node for each memory access.
 * Values are dense uint8_t so they can be stored directly on XiValue.
 */
typedef enum {
    XI_MEM_NONE = 0, /* non-memory op (default for arithmetic, etc.) */
    XI_MEM_TOP,      /* unknown / conservative (may alias anything) */
    XI_MEM_CONST,    /* immutable data: string bytes, frozen objects */
    XI_MEM_FIELD,    /* object field (generic — field id unknown) */
    XI_MEM_FIELD_ID, /* object field with known field id (disjoint per id) */
    XI_MEM_ARRAY,    /* array element access */
    XI_MEM_STRUCT,   /* struct field (typed, fixed layout) */
    XI_MEM_SHARED,   /* module shared variable array */
    XI_MEM_GLOBAL,   /* global variable dict (REPL name-keyed) */
    XI_MEM_UPVAL,    /* closure upvalue slot */
    XI_MEM_TLS,      /* thread-local / per-isolate state */
    XI_MEM_JSON,     /* JSON object field access */
    XI_MEM_TUPLE,    /* tuple element access */
    XI_MEM_CHAN,     /* channel send/recv buffer */
    XI_MEM_COUNT,    /* sentinel — total number of groups */
} XiMemGroup;

/* ========== Alias Query API ========== */

/* Returns true if the two values MAY access the same memory location.
 * Returns false only when the TBAA lattice GUARANTEES no alias.
 *
 * Both values must be memory-accessing ops (load/store/call).
 * For non-memory ops, returns false trivially (no memory access). */
XR_FUNC bool xi_tbaa_may_alias(const XiValue *a, const XiValue *b);

/* Returns true if the memory group represents a read-only (immutable) region.
 * Loads from const memory can never be invalidated by stores. */
static inline bool xi_mem_is_const(XiMemGroup g) {
    return g == XI_MEM_CONST;
}

/* Returns true if the op is a memory load (reads but does not write). */
XR_FUNC bool xi_is_memory_load(uint16_t op);

/* Returns true if the op is a memory store (writes). */
XR_FUNC bool xi_is_memory_store(uint16_t op);

/* Returns true if the op accesses memory at all (load or store). */
XR_FUNC bool xi_is_memory_op(uint16_t op);

/* Annotates one value with the memory group implied by its op and aux data. */
XR_FUNC void xi_tbaa_annotate_value(XiValue *v);

/* ========== TBAA Annotation Pass ==========
 *
 * Runs over all values in a function and sets mem_group based on the op
 * kind and type information.  Called during lowering or as an early
 * pipeline pass.  After completion, sets XI_INV_TBAA_ANNOTATED.
 *
 * Returns a pass change record (values_changed = true if any annotation
 * was added or updated). */
XR_FUNC XiPassChange xi_tbaa_annotate(XiFunc *f);

#endif  // XI_TBAA_H
