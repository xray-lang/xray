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
/* Two cause rows lead the shared payload; every following word row keeps
 * its natural alignment without padding or another allocation owner. */
_Static_assert((2*sizeof(EffectInvocationCause))%_Alignof(uint64_t)==0,
    "Invocation cause rows preserve word alignment");
/* One fact identifies a genuine site or one bit of its real Cell capture
 * relation at an SSA or return vertex. Descending authentic causes cannot
 * manufacture either identity or an environment in an unrooted SCC. */
typedef struct EffectInvocationProducer {
    uint32_t value,site,next_node,next_value,edge,instruction;
    uint64_t distance;
    uint32_t atom;
} EffectInvocationProducer;
enum { EFFECT_CLASS_KNOWN=1u, EFFECT_CLASS_ROOT=2u, EFFECT_CLASS_UNKNOWN=4u,
    EFFECT_CLASS_VALID=7u };
/* Masks preserve residual uncertainty. Only a complete local relation has a
 * positive descending rank; a type or a temporary lattice bottom has none. */
typedef struct EffectInvocationClassFact {
    uint32_t value,mask;
    uint64_t rank;
} EffectInvocationClassFact;

typedef struct EffectInvocationNode {
    uint32_t root,body,parameter_count,instruction_count;
    uint32_t intrinsic_mask,local_mask,cause_count;
    EffectInvocationCause *causes;
    uint64_t *input,*cells,*local_cells;
    EffectInvocationProducer *producers;
    uint32_t producer_count,producer_capacity;
    uint64_t return_coverage;
    uint64_t *parameter_coverage;
    EffectInvocationClassFact *classes;
    uint32_t class_count,class_return_mask;
    uint64_t class_return_rank;
    uint32_t edge_head;
    bool deferred,local_deferred;
} EffectInvocationNode;

/* Construction history belongs only to the solver's finite scratch owner.
 * It cannot qualify any receiving Final fact or survive publication. */
typedef struct EffectInvocationState { bool queued,expanded; } EffectInvocationState;
_Static_assert(_Alignof(EffectInvocationState)<=_Alignof(uint32_t),
    "Invocation state follows aligned reverse-index storage");

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
        !!node->cells==!!words && !!node->local_cells==!!words &&
        !!node->parameter_coverage==!!node->parameter_count;
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
    XrXirRootEffects *facts;
    XrXirRootEffectWitness *witnesses;
};

/* The complete semantic inventory survives construction without a solver,
 * queue, compile callback, mutable receiver or edge lookup workspace. */
typedef struct EffectInvocationFinal {
    EffectFinalFunctions summary;
    EffectPhysicalDomain *physical;
    EffectBodyMaterial *material;
    const XrXirCellProvenance *cells;
    EffectInvocationNode *nodes;
    EffectInvocationEdge *edges;
    EffectInvocationSite *sites;
    EffectInvocationDeferred *deferred_calls;
    uint32_t *roots,*root_spaces;
    const XrXirRootEffects *local_roots;
    const XrXirRootEffectWitness *local_witnesses;
    uint32_t site_count,binding_count;
    uint32_t deferred_count,deferred_capacity;
    uint32_t count,capacity,edge_count,edge_capacity;
} EffectInvocationFinal;

/* Replays share only released storage. Every live replay claims a distinct
 * block; no origin, flag or permission survives a new initialization. */
typedef struct EffectInvocationScratch {
    struct EffectInvocationScratch *next;
    XrCompileResources *resources;
    size_t capacity;
    bool claimed;
} EffectInvocationScratch;

typedef struct EffectInvocationWorkspace {
    EffectInvocationFinal final;
    const XrXirCompileContext *work;
    XrXirEffects *effects;
    EffectGraph *graph;
    const EffectInvocationDeclaredBounds *declared;
    EffectBodyRequest body;
    EffectFinalFunctions *prepared_result;
    uint32_t *edge_index;
    size_t edge_index_capacity;
    EffectInvocationScratch *replay_scratch;
    uint32_t *consumers;
    EffectInvocationState *states;
    uint32_t maximum;
    uint32_t front,back,pending;
    bool producer_enabled,class_enabled;
} EffectInvocationWorkspace;

static XrXirStatus effect_root_seed(const XrXirModule *module,XrXirEffects *effects,
    uint32_t function,uint32_t instruction,const XrXirCompileContext *work);

static inline XrXirStatus effect_invocation_producer_column(const EffectInvocationWorkspace *owner,
    const EffectInvocationBasis *basis,uint32_t site,uint32_t atom,uint32_t *word,uint32_t *bit);

static XrXirStatus effect_invocation_coverage_verify(EffectInvocationWorkspace *owner);
static XrXirStatus effect_invocation_coverage_solve(EffectInvocationWorkspace *owner);
static XrXirStatus effect_invocation_class_verify(EffectInvocationWorkspace *owner);

typedef struct EffectInvocationColumn { uint32_t word,bit; } EffectInvocationColumn;
static XrXirStatus effect_invocation_producer_column_read(const EffectInvocationWorkspace *owner,
    const EffectBodyRequest *request,const EffectInvocationBasis *basis,
    uint32_t index,uint32_t atom,EffectInvocationColumn *output);
static XrXirStatus effect_invocation_class_verify_read(EffectInvocationWorkspace *owner,
    const EffectBodyRequest *request);
static XrXirStatus effect_invocation_edge_shape_read(const XrXirCompileContext *work,
    const EffectInvocationFinal *equations,const EffectBodyRequest *body,
    const XrXirTypes *types,uint32_t caller,uint32_t edge_index);
/* Each request charges the current real context and reads the sole immutable
 * material. There is no construction-time raw instruction fallback. */
static inline EffectBodyRequest effect_invocation_body(const EffectInvocationWorkspace *owner) {
    return effect_body_owned_request(owner->work,owner->final.material);
}
static XrXirStatus effect_invocation_head(const EffectInvocationWorkspace *owner,uint32_t function,
    EffectBodyFunctionHead *output) {
    if (!owner || !owner->work || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_invocation_body(owner);
    return effect_body_request_head(&request,function,output);
}
static XrXirStatus effect_invocation_dimensions(const EffectInvocationWorkspace *owner,uint32_t function,
    EffectBodyFunctionDimensions *output) {
    if (!owner || !owner->work || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_invocation_body(owner);
    return effect_body_request_dimensions(&request,function,output);
}
static XrXirStatus effect_invocation_operation(const EffectInvocationWorkspace *owner,uint32_t function,
    uint32_t instruction,XrXirInstruction *storage,const XrXirInstruction **output) {
    if (!owner || !owner->work || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_invocation_body(owner);
    return effect_body_request_instruction(&request,function,instruction,storage,output);
}

static XrXirStatus effect_invocation_shape(const EffectInvocationWorkspace *owner,uint32_t function,
    uint32_t instruction,EffectBodyInstructionShape *output) {
    if (!owner || !owner->work || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_invocation_body(owner);
    return effect_body_request_shape(&request,function,instruction,output);
}

static void effect_invocation_result_clear(EffectFinalFunctions *result);
static void effect_invocation_result_free(EffectFinalFunctions *result);
static bool effect_invocation_witness_equal(const XrXirRootEffectWitness *a,
    const XrXirRootEffectWitness *b);

#include "xxir_effect_invocation_scratch.inc.c"
#include "xxir_effect_invocation_class_nodes.inc.c"
#include "xxir_effect_invocation_nodes.inc.c"
#include "xxir_effect_invocation_causes.inc.c"
#include "xxir_effect_invocation_flow.inc.c"
#include "xxir_effect_invocation_class_flow.inc.c"
#include "xxir_effect_invocation_deferred.inc.c"
#include "xxir_effect_invocation_calls.inc.c"
#include "xxir_effect_invocation_producers.inc.c"
#include "xxir_effect_invocation_solve.inc.c"
#include "xxir_effect_invocation_solution.inc.c"
#include "xxir_effect_invocation_deferred_verify.inc.c"
#include "xxir_effect_invocation_seal.inc.c"
#include "xxir_effect_invocation_query.inc.c"
#include "xxir_effect_invocation_coverage.inc.c"
#include "xxir_effect_invocation_class_verify.inc.c"
#include "xxir_effect_invocation_projection.inc.c"
#include "xxir_effect_invocation_trace.inc.c"
#include "xxir_effect_invocation_driver.inc.c"
