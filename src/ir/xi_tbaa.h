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
 *   fresh (storage the op allocates itself — reachable by nothing else yet)
 *
 *   Two values with the same group MAY alias; different groups NEVER alias
 *   (unless one is XI_MEM_TOP, which may alias everything).  XI_MEM_FRESH
 *   aliases nothing at all, by construction.
 *
 * NONE IS NOT CONSERVATIVE:
 *   XI_MEM_NONE means "this op touches no memory".  It is NOT a fallback for
 *   "memory I cannot classify" — xi_tbaa_may_alias() answers no-alias for it,
 *   so a store annotated NONE stops killing loads and LICM will hoist an
 *   aliasing load out of a loop containing that store.  ops.def rejects any op
 *   that declares a memory effect without a group, so the trap is unreachable
 *   from the single source of truth; see xisagen's memory-scope rule.
 *
 * ORDERING BARRIERS:
 *   TBAA answers "can these two touch the same bytes".  It says nothing about
 *   whether a reordering is *permitted* — that is the memory model's job
 *   (spec §16.9).  xi_op_is_ordering_barrier() is the separate query for it:
 *   ops that carry a :sync edge (atomics, channels, await, spawn, join) or may
 *   suspend must not have ordinary memory operations moved across them, even
 *   when their TBAA groups are provably disjoint.
 *
 * USAGE:
 *   - xi_lower fills XiValue.mem_group for every load/store op
 *   - xi_tbaa_may_alias() is the sole query interface
 *   - Passes must never bypass the query or hard-code alias assumptions
 *
 * INVARIANTS:
 *   - The annotation pass publishes revision-bound XI_EVD_ALIAS evidence
 *   - Every memory-effecting op has mem_group != XI_MEM_NONE
 *   - Every op with no memory effect has mem_group == XI_MEM_NONE
 *   - mem_group is always the op's declared group, or its FIELD -> FIELD_ID
 *     refinement; xi_tbaa_group_matches_op() is the checkable form
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
    XI_MEM_OBJECT,   /* fixed and dynamic object field storage */
    XI_MEM_TUPLE,    /* tuple element access */
    XI_MEM_CHAN,     /* channel send/recv buffer */
    XI_MEM_FRESH,    /* storage allocated by this op; aliases nothing */
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

/* Returns true if the op may clobber any tracked memory group. */
XR_FUNC bool xi_is_memory_clobber(uint16_t op);

/* Returns true if the op accesses memory at all (load or store). */
XR_FUNC bool xi_is_memory_op(uint16_t op);

/* Returns the memory group an op is declared to access in ops.def.
 * This is the single source for XiValue.mem_group; passes that rewrite an op
 * in place must reassign the group from here rather than clearing it. */
XR_FUNC XiMemGroup xi_tbaa_group_for_op(uint16_t op);

/* Returns true if v->mem_group is consistent with its op's declared group,
 * allowing the FIELD -> FIELD_ID refinement.  The verifier's TBAA check. */
XR_FUNC bool xi_tbaa_group_matches_op(const XiValue *v);

/* ========== Ordering Barrier Query (spec §16.9) ==========
 *
 * Returns true if ordinary memory operations must not be moved across this op.
 * Two independent reasons, both of which are recorded in ops.def:
 *
 *   1. :sync — the op establishes a language-level happens-before edge, so
 *      code motion across it would break acquire/release for the other side
 *      of that edge.  Disjoint TBAA groups do not license the motion.
 *   2. may-suspend — another task may run before this op resumes and mutate
 *      any reachable state, so loads on either side see different memory.
 *
 * A pass that reasons about load/store placement must consult this in addition
 * to xi_tbaa_may_alias(); alias-disjointness alone is not a licence to move. */
XR_FUNC bool xi_op_is_ordering_barrier(uint16_t op);

/* Returns true if the op ends the current memory version: it writes tracked
 * memory, clobbers unknown memory, or is an ordering barrier. */
XR_FUNC bool xi_op_ends_memory_version(uint16_t op);

/* Annotates one value with the memory group implied by its op and aux data. */
XR_FUNC void xi_tbaa_annotate_value(XiValue *v);

/* ========== TBAA Annotation Pass ==========
 *
 * Runs over all values in a function and sets mem_group based on the op
 * kind and type information.  Called during lowering or as an early
 * pipeline pass. After completion, publishes current alias evidence.
 *
 * Returns a no-IR-change pass record; mem_group is analysis metadata. */
XR_FUNC XiPassChange xi_tbaa_annotate(XiFunc *f);

#endif  // XI_TBAA_H
