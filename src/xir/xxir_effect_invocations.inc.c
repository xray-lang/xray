/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocations.inc.c - Owned finite physical invocation equations
 *
 * KEY CONCEPT:
 *   Complete input environments use a fixed root basis. Repeated recursive
 *   keys connect existing equations instead of copying a binding path.
 */
typedef struct EffectInvocationDeferred {
    uint32_t node,instruction,parameter,count;
    uint64_t *arguments;
} EffectInvocationDeferred;
typedef struct EffectInvocationCause {
    XrXirRootEffectWitness witness;
    uint32_t next,edge,distance;
} EffectInvocationCause;
_Static_assert(offsetof(EffectInvocationCause,edge)==offsetof(EffectInvocationCause,next)+sizeof(uint32_t) &&
    offsetof(EffectInvocationCause,distance)==offsetof(EffectInvocationCause,edge)+sizeof(uint32_t),
    "Invocation sentinel fields remain contiguous");
/* Two cause rows lead the shared payload; every following word row keeps
 * its natural alignment without padding or another allocation owner. */
_Static_assert((2*sizeof(EffectInvocationCause))%_Alignof(uint64_t)==0,
    "Invocation cause rows preserve word alignment");
/* One fact identifies a genuine site, a capture column, or a current SSA bit
 * with site MAX. Data-only bits have no cause or rank. Descending authentic
 * causes alone can support identity or coverage in this same finite owner. */
typedef struct EffectInvocationProducer {
    uint32_t value,site,next_node,next_value,edge,instruction;
    uint64_t distance;
    uint32_t atom;
} EffectInvocationProducer;
typedef struct EffectInvocationProducerSet {
    EffectInvocationProducer *facts;
    uint32_t count,capacity;
} EffectInvocationProducerSet;
enum { EFFECT_CLASS_KNOWN=1u, EFFECT_CLASS_ROOT=2u, EFFECT_CLASS_UNKNOWN=4u,
    EFFECT_CLASS_VALID=7u };
/* Masks preserve residual uncertainty. Only a complete local relation has a
 * positive descending rank; a type or a temporary lattice bottom has none. */
typedef struct EffectInvocationClassFact {
    uint32_t value,mask;
    uint64_t rank;
} EffectInvocationClassFact;
typedef struct EffectInvocationClassDiscovery {
    EffectInvocationClassFact *facts;
    uint32_t count,return_mask;
    uint64_t return_rank;
} EffectInvocationClassDiscovery;

typedef struct EffectInvocationNode {
    uint32_t root,body,parameter_count,instruction_count;
    uint32_t intrinsic_mask,local_mask,cause_count;
    EffectInvocationCause *causes;
    uint64_t *input,*cells,*local_cells;
    uint32_t edge_head;
    bool deferred,local_deferred;
} EffectInvocationNode;

/* Construction history belongs only to the solver's finite scratch owner.
 * It cannot qualify any receiving Final fact or survive publication. */
typedef struct EffectInvocationState {
    EffectInvocationProducerSet producers;
    EffectInvocationClassDiscovery classes;
    uint32_t lookup_next;
    bool queued,expanded,precision_pending;
} EffectInvocationState;

/* The two output copies and cause copies share one payload lifetime. These
 * views store no permission facts and are checked against the real root and
 * body dimensions before the closed owner can be sealed or consumed. */
typedef enum EffectInvocationRow {
    EFFECT_INVOCATION_CELLS,
    EFFECT_INVOCATION_PARAMETERS,
    EFFECT_INVOCATION_CONTEXTS
} EffectInvocationRow;

static uint64_t *effect_invocation_node_row(const EffectInvocationNode *node,
    EffectInvocationRow row,bool local) {
    uint64_t *base=local?node->local_cells:node->cells;
    if (!base || node->cause_count<3 || !(node->cause_count&1)) return NULL;
    uint32_t parameters=(node->cause_count-3)/2;
    size_t dependencies=(size_t)(((uint64_t)parameters+63)/64);
    return base+(size_t)row*dependencies;
}

static EffectInvocationCause *effect_invocation_local_causes(const EffectInvocationNode *node) {
    return node->causes?node->causes+(size_t)node->cause_count:NULL;
}

static bool effect_invocation_node_shape(const EffectInvocationNode *node,
    uint32_t parameters,uint32_t instructions) {
    uint64_t dependencies=((uint64_t)parameters+63)/64;
    uint64_t contexts=((uint64_t)instructions+63)/64;
    uint64_t words=dependencies*2+contexts;
    return node->instruction_count==instructions &&
        node->cause_count==(uint64_t)parameters*2+3 && node->causes &&
        !!node->cells==!!words && !!node->local_cells==!!words;
}

typedef struct EffectInvocationEdge {
    uint32_t caller,target,instruction,producer,next,consumer_next;
    bool latent;
} EffectInvocationEdge;

typedef struct EffectInvocationBasis {
    uint32_t parameters,origin_words,fn_words,cell_words,row_words;
} EffectInvocationBasis;

typedef struct EffectInvocationSite {
    uint32_t function,instruction,target,captures,binding;
} EffectInvocationSite;

typedef struct EffectFinalFunction {
    XrXirRootFormula formula;
    XrXirRootTerm *storage;
    uint32_t capacity;
} EffectFinalFunction;

struct EffectFinalFunctions {
    XrCompileResources *resources;
    uint32_t count,capacity,term_count;
    EffectFinalFunction *functions;
    XrXirRootEffectWitness *witnesses;
};

/* The complete semantic inventory survives construction without a solver,
 * queue, compile callback, mutable receiver or edge lookup workspace. */
#include "xxir_effect_parameter_summary.inc.c"

/* Value and initial-key coordinates share a finite positive cause order.
 * The complete relation remains in the sole owning Final. */
#define EFFECT_CAPTURE_FLAGS UINT32_MAX
enum { EFFECT_CAPTURE_FIXED, EFFECT_CAPTURE_RAW, EFFECT_CAPTURE_COMPLETE };
typedef struct EffectCaptureFact {
    uint32_t node,value,word,bit;
    uint64_t rank;
} EffectCaptureFact;
typedef struct EffectCaptureProof {
    EffectCaptureFact *facts;
    uint32_t count,capacity;
} EffectCaptureProof;

typedef struct EffectInvocationFinal {
    EffectCaptureProof capture;
    EffectParameterSummary parameter_summary;
    EffectFinalFunctions summary;
    EffectFinalOutcomes *outcomes;
    EffectPhysicalDomain *physical;
    EffectBodyMaterial *material;
    const XrXirCellProvenance *cells;
    EffectInvocationNode *nodes;
    EffectInvocationEdge *edges;
    EffectInvocationSite *sites;
    EffectInvocationDeferred *deferred_calls;
    uint32_t *roots,*root_spaces;
    uint32_t site_count,binding_count;
    uint32_t deferred_count,deferred_capacity;
    uint32_t count,capacity,edge_count,edge_capacity;
} EffectInvocationFinal;

/* A verification request borrows the sole Final and immutable Input. Its
 * incoming index is fresh request scratch; no solver state or semantic copy
 * exists in this view. The builder supplies the same view of its live Final. */
typedef struct EffectInvocationRoles {
    const XrXirEffects *construction;
    const EffectInvocationCertificate *sealed;
} EffectInvocationRoles;

typedef struct EffectInvocationRead {
    const XrXirCompileContext *work;
    const EffectInvocationFinal *final;
    EffectInvocationRoles roles;
    const EffectInvocationDeclaredBounds *declared;
    EffectBodyRequest body;
    uint32_t *consumers;
    const struct EffectInvocationWorkspace *discovery;
    bool producer_enabled,class_enabled;
} EffectInvocationRead;

typedef struct EffectInvocationPrimitive {
    XrXirRootEffects facts;
    XrXirRootEffectWitness witnesses[2];
} EffectInvocationPrimitive;

typedef struct EffectInvocationWorkspace {
    EffectInvocationFinal final;
    EffectInvocationRead read;
    XrXirEffects *effects;
    EffectGraph *graph;
    EffectFinalFunctions *prepared_result;
    uint32_t *edge_index,*node_index;
    size_t edge_index_capacity,node_index_capacity;
    EffectInvocationState *states;
    EffectInvocationPrimitive *primitives;
    uint32_t primitive_count;
    uint32_t maximum;
    uint32_t front,back,pending;
} EffectInvocationWorkspace;

/* Discovery rows have only the workspace lifetime. A sealed reader has no
 * workspace pointer and cannot recover this constructor capability. */

typedef struct EffectInvocationColumn { uint32_t word,bit; } EffectInvocationColumn;

/* The physical row is scratch for this single completed edge read. Only its
 * operation pointer is published on success; no fact or authority is retained. */
typedef struct EffectInvocationEdgeOperation {
    XrXirInstruction storage;
    const XrXirInstruction *operation;
} EffectInvocationEdgeOperation;
typedef struct EffectInvocationEdgeRead {
    uint32_t caller,index;
    XrXirOp *opcode;
} EffectInvocationEdgeRead;
static XrXirStatus effect_invocation_edge_read_selected(const XrXirCompileContext *work,
    const EffectInvocationFinal *equations,const EffectBodyRequest *body,
    const XrXirTypes *types,const EffectInvocationEdgeRead *selection,EffectInvocationEdgeOperation *decoded);

/* Each request charges the current real context and reads the sole immutable
 * material. There is no construction-time raw instruction fallback. */

static void effect_invocation_result_clear(EffectFinalFunctions *result);

static bool effect_invocation_witness_equal(const XrXirRootEffectWitness *a,
    const XrXirRootEffectWitness *b);

#include "xxir_effect_invocation_nodes.inc.c"
#include "xxir_effect_invocation_root_seeds.inc.c"
#include "xxir_effect_invocation_primitive.inc.c"
#include "xxir_effect_invocation_causes.inc.c"
#include "xxir_effect_invocation_flow.inc.c"
#include "xxir_effect_invocation_class_flow.inc.c"
#include "xxir_effect_invocation_calls.inc.c"
#include "xxir_effect_invocation_producers.inc.c"
#include "xxir_effect_invocation_solve.inc.c"
#include "xxir_effect_invocation_solution.inc.c"
#include "xxir_effect_invocation_seal.inc.c"
#include "xxir_effect_invocation_query.inc.c"
#include "xxir_effect_invocation_projection.inc.c"
#include "xxir_effect_invocation_trace.inc.c"
