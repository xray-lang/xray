/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_flow.inc.c - Finite real callable and common Cell origin projection
 */
typedef struct EffectInvocationFlow {
    EffectInvocationWorkspace *owner;
    const EffectInvocationRead *read;
    const EffectBodyFunctionHead *function;
    EffectBodyFunctionHead head;
    EffectBodyRequest body;
    EffectInvocationBasis basis;
    uint32_t node,values,instruction,row_count;
    uint32_t *row_indices;
    const EffectPhysicalFunction *physical;
    uint64_t *rows;
    /* Receiving scratch has fixed bits only; certified identity seeds belong
     * exclusively to constructor discovery and cannot be requested sealed. */
    uint64_t *fixed,*certified,*cell_scratch;
    uint64_t local_flags[2];
    void *memory;
    bool changed,precision;
} EffectInvocationFlow;
typedef struct EffectInvocationClosureView {uint32_t value;const uint64_t *row;} EffectInvocationClosureView;

/* Requests borrow only for this one replay. Every failed field read is
 * returned before it can be interpreted as a type, origin or permission. */

/* The map belongs to this one immutable body replay, never to a Source
 * refiner generation. MAX denotes absent callable storage, not ROOT None. */

/* Both constructor flags represent every actual SSA, including non-callable
 * barrier/alias facts. A compressed bit is never interpreted as a ROOT,
 * producer, Cell or Class permission. The full independent gates remain. */

/* Constructor projection owns only scratch. Reopen actual geometry after its
 * allocation before consuming any current capture word or flag. */

/* Opaque masks are origin facts, not the target's physical type identity.
 * Carrying them in the fixed basis prevents a temporary None or a narrower
 * receiver descriptor from erasing a genuine producer UNKNOWN contribution. */

/* A captured descriptor carries a flat, finite relation for every original
 * producer site. It does not recursively copy another descriptor's captures.
 * Equal producer sites encountered through PHI or recursion only OR rows. */

/* Returned identities supplement the original opaque advertisement. Only
 * ordinary NONE return signatures participate; REF/UNKNOWN/fixed barriers
 * retain their original bounds and cannot acquire a permission here. */

/* This is only the original origin transfer's early-return condition. It
 * neither clears a carrier row nor authorizes execution or ROOT permission.
 * INVOKE_RESULT keeps its independent physical frontier check when enabled. */

/* Each fixed-point pass independently reaches current canonical EOF. Only
 * actual constant opcode/type fields share an early-return classification;
 * every active physical transfer enters the unchanged complete handler. */

/* Classification locates a real FnRef, whose complete row is still read.
 * Other initial callable values consume only opcode here; the shape reader
 * retains full canonical run checks without constructing an unused row. */

/* One replay consumes the sole immutable owning representation. Complete
 * rows may be shared only within the current request; no queue visit retains
 * its stack instruction or derives authority from a previous read. */

/* Published readers project the sole Final. Discovery is available only to
 * the actual construction workspace, never to a sealed receiving request. */

#include "xxir_effect_invocation_class_cells.inc.c"

/* This operation consumes the common Cell proof. It does not infer a second
 * set of Cell roles or origins: each compact provider dependency is projected
 * through the already-owned physical input row of this exact equation. */
