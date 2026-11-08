/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_global.inc.c - Finite origin and capture support across actual keys
 *
 * KEY CONCEPT:
 *   Physical values, return vertices and initial-key cells share one sparse
 *   descending relation. An initial key is separate from its computed SSA row.
 *   Closure and positive support consume the sole Input and Final without a solver.
 */
enum { EFFECT_CAPTURE_SCALAR, EFFECT_CAPTURE_CALLABLE, EFFECT_CAPTURE_CELL, EFFECT_CAPTURE_CLASS };

typedef struct EffectCaptureHead {
    const XrXirType *parameters;
    const uint32_t *operands;
    uint32_t parameter_count,instruction_count,operand_count;
    XrXirType result;
} EffectCaptureHead;
typedef struct EffectCaptureNode {
    EffectCaptureHead head;
    EffectInvocationBasis basis;
    uint32_t node,values;
} EffectCaptureNode;
typedef struct EffectCaptureSchedule EffectCaptureSchedule;
typedef struct EffectCaptureActualRead EffectCaptureActualRead;
typedef struct EffectCaptureCompleteTypes EffectCaptureCompleteTypes;
typedef struct EffectCaptureRead {
    const XrXirCompileContext *work;
    const EffectInvocationFinal *final;
    EffectBodyRequest body;
    EffectCaptureProof *builder;
    EffectCaptureSchedule *schedule;
    const EffectCaptureFact *selected;
    uint32_t selected_count;
    uint64_t selected_mask;
    const EffectCaptureNode *node_view,*node_views,*edge_views;
    EffectCaptureActualRead *actuals;
    EffectCaptureCompleteTypes *complete_types;
    const uint32_t *fact_nodes;
    uint32_t node_view_count,edge_view_count,maximum;
    bool fixed_only,fallback,class_results,active,changed,supported,complete_scope;
} EffectCaptureRead;

/* Clauses may share one uninterrupted read at the same node and value.
 * Each coordinate still needs its own descending cause; support is the
 * conjunction of all witnesses, including their individual rank bounds. */

/* Capture clauses consume no names or block vectors. The owning adapter uses
 * the same complete head-shape predicate and copies only their current fields;
 * a delegated reader still supplies its full original head response. */

/* Receiving borrows current heads within its uninterrupted read. A constructor
 * edge loan is shorter: every possible fact growth drops it before callbacks. */

typedef struct EffectCaptureTransfer {
    uint32_t from,value,to,destination,first,word,words;
    uint64_t rank;
    bool initial;
} EffectCaptureTransfer;

/* Sorted insertions can move both the allocation and the next source record.
 * Only copied scalar coordinates survive an emit or an allocator callback. */

/* An unused type-pool Cell does not create a value relation. The existing
 * complete local-input walk proves absence from actual heads and affine runs,
 * using fresh stack cursors and the original typed/opcode rules and charges. */

#include "xxir_effect_capture_edges.inc.c"
#include "xxir_effect_capture_complete.inc.c"

/* Every non-scalar result and type-independent clause needs its original
 * transfer. Other scalar rows contribute no capture fact. This selects only
 * operand decoding; complete input framing, closure and support still run. */

/* A missing result means this current scalar row has no capture transfer.
 * Its full record was framed before projection. No allocation occurs here. */

/* Construction can grow facts between operations. Only the numeric cursor
 * survives; each selection reopens the current record before its transfer.
 * Delegated readers retain their original full-operation behavior. */

/* Receiving cannot allocate or release. Varying rows first project their
 * current opcode and type; contributing rows keep their full original replay.
 * Constant rows already borrow the complete base without materialization.
 * The ordinary finish reader independently reopens the final record and EOF. */

/* Fallback adds only local sources to the preceding positive fixed point.
 * An unchanged local pass cannot change any already closed edge. Once a
 * source changes, preserve the original edge pass and ordered full sweeps. */

#include "xxir_effect_capture_schedule.inc.c"
/* Closure constrains every cause before a stored witness may claim support.
 * Equal-node/value groups project only their actual local and edge clauses;
 * one machine word holds the conjunction without allocation or callbacks. */

/* Canonical rows are consumed only after all positive and fallback domains
 * close. Each cleanup gets fresh scratch; interning may move the graph, so
 * the first growth ends this entire proof interval. */

/* Construction and its immediate receiver own the same private Input. The
 * parent descriptor is discarded before every possible cleanup callback;
 * no child stack projection or semantic result crosses this boundary. */
